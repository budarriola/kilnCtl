"""LCD suite cases (plan §8 Wave 1c): LCD-01, LCD-08, LCD-21 first.

Same split as cases_smoke.py: each ``_case_XXX(ctx)`` does the fetching
(here: LVGL state via ``srv._ui_test`` -- the UiTestClient instance the
board-attached ``kilnctrl.mcp_server`` module already owns, task 14 --
plus, where a case needs it, a webcam capture through ``lcd_sampler.py``)
and hands the result to a pure judge function in ``judgments.py``.

A camera capture failure (busiest common case: ffmpeg exit -5, another
process holding the C920, CLAUDE.md's capture_lcd.ps1 section) never fails
a case outright -- the widget-state half of a case (page name, tap target
presence/visibility) is checked either way; only the color-sampling half
degrades, to INCONCLUSIVE, when no frame could be captured.
"""
from __future__ import annotations

import math
import re
import os
import tempfile
import time
from typing import Any, Dict, Optional

from . import board_lock
from . import judgments as J
from . import lcd_sampler
from .registry import CaseResult, Verdict, get_case
from ..protocol import THERMO_CHANNEL_ALL
from ..devices_touch import (
    TOUCH_POWER_STATE_ERROR_HOLD,
    TOUCH_POWER_STATE_ON,
    TOUCH_SWALLOW_REASON_WAKE,
)
from ..ui_test_client import _ENTER_PIN_RETRY_POLL_S

import logging

log = logging.getLogger(__name__)

#: click_by_name() (uart_bridge_ui_test.c -> kiln_ui_click_by_name(),
#: firmware/KilnFW/App/drivers/ui/kiln_ui.c) injects the synthetic touch
#: press+release synchronously and replies OK as soon as the release is
#: injected -- but the actual screen switch (lv_screen_load() /
#: s_current_page_name update inside kiln_ui_show(), called from the
#: button's LVGL click event) only happens later, when the LVGL task's own
#: ~30ms touch_read_cb() poll (lvgl_port.c) next samples that latched
#: release and runs its event handlers. A page (or tap-target) read
#: immediately after a successful click can race that poll and still
#: observe the OLD page -- this is exactly LCD-08/09/14/16's 2026-09-24
#: bench failure (click_by_name succeeded, immediate read still said
#: 'home'). Poll instead of reading once; bounded so a page that genuinely
#: never arrives is still a FAIL, not a hang.
_PAGE_POLL_TIMEOUT_S = 2.0
_PAGE_POLL_INTERVAL_S = 0.1

#: A SECOND, distinct race from the one above: `screen_idle.c`'s auto-blank
#: state machine can put the panel to sleep between bench-test cases (or
#: even between preflight and the first LCD case), and firmware's injected-
#: touch path (`lvgl_port.c:556-579`) calls `screen_idle_touch_swallow()`
#: *before* hit-testing a synthetic tap -- so a `click_by_name()` that
#: arrives while the panel is blanked WAKES the screen but the tap itself is
#: swallowed, while `kiln_ui_click_by_name()` (`kiln_ui.c:782-797`) still
#: replies "ok" (it only reports whether a target by that name existed, not
#: whether the resulting tap actually reached it). This is exactly the
#: 2026-09-24 bench failure shape for LCD-01/08/09/14/16: click_by_name()
#: said ok, but the page never changed because the first tap only woke the
#: panel. `_wake_and_home()` below sends one real touch_inject press/release
#: (which the idle state machine treats as a real touch and uses to reset
#: its timer and wake the panel, same as a finger) and confirms the home
#: page before any case starts tapping named targets for real.
_WAKE_TOUCH_XY = (5, 5)

#: How long, after sending the wake touch, to poll ``touch.get_state()``
#: waiting for ``screen_on`` to read True before the first navigation click
#: is issued. `TouchClient.inject()` (kilnctrl/touch.py) already blocks the
#: caller for up to ``DRIVER_ERROR_REJECT_WINDOW_S`` (0.5s) per call waiting
#: out an optional driver-error reply, so the wake's own press+release are
#: already spaced well past one ~30ms LVGL poll -- a live bench check
#: (2026-09-24) confirmed a single press+release wake reliably flips
#: `screen_on` to True by the time both `inject()` calls return. This wait
#: is a bounded belt-and-suspenders check on top of that, not a fixed sleep:
#: it returns as soon as the state reads on, and never blocks past
#: `_WAKE_SCREEN_ON_TIMEOUT_S` if the touch task itself is unavailable
#: (`touch is None`) or a query fails, so a board not wired for touch
#: injection still lets the case proceed and fail honestly on its own click.
_WAKE_SCREEN_ON_TIMEOUT_S = 1.0
_WAKE_SCREEN_ON_POLL_S = 0.05

#: 2026-09-24: `screen_on` alone cannot distinguish DISPLAY_POWER_ON from
#: DISPLAY_POWER_ERROR_HOLD (both read `screen_on=True` -- display_power_
#: policy.c's rule 5 only dismisses ERROR_HOLD on its OWN next touch, it does
#: not blank the panel). A wake landing while the board is in ERROR_HOLD
#: would previously read `screen_on=True` and proceed straight to clicking
#: nav targets on a panel that is actually still showing the error page --
#: any injected tap there is the *dismissal* tap, swallowed by rule 5, not a
#: real navigation click. When `touch.get_state()` exposes the newer
#: `power_state` field (2026-09-24 firmware or later), `_wake_and_home` waits
#: for it to read ON specifically, sending one extra dismissal tap if it
#: reads ERROR_HOLD. Bounded to a small number of dismissal attempts so a
#: board stuck in ERROR_HOLD for a real reason still falls through to the
#: case's own click and fails honestly rather than looping here forever.
_ERROR_HOLD_DISMISS_MAX_ATTEMPTS = 2


def _power_state_is_on(state) -> "bool | None":
    """None when `state` predates the 2026-09-24 power_state field (older
    firmware) -- caller falls back to `screen_on` alone in that case."""
    power_state = getattr(state, "power_state", None)
    if power_state is None:
        return None
    return power_state == TOUCH_POWER_STATE_ON


def _power_state_is_error_hold(state) -> bool:
    return getattr(state, "power_state", None) == TOUCH_POWER_STATE_ERROR_HOLD

#: The shortest persisted display timeout is 1 minute
#: (display_power_policy.c's DISPLAY_TIMEOUT_1_MIN), so a panel that reads
#: on with less idle time than this still has >= 30 s before it can blank.
#: Past this, the wake tap is sent even though the panel reads on, so the
#: idle timer is reset before the case starts clicking.
_WAKE_IDLE_MS_THRESHOLD = 30_000

#: judgments.py's page-mismatch FAIL reasons (judge_lcd_home_idle,
#: judge_lcd_config_hub, judge_lcd_profiles_picker, judge_lcd_temperature_page)
#: append `judgments.BLANKED_SCREEN_HINT` to name this same race as a likely
#: cause when the page a case waited for never arrived -- see that module
#: for the hint text (kept there, not here, since this module would
#: otherwise need a reason string round trip through judgments.py just to
#: reuse a constant).


def _wake_and_home(ctx: dict) -> Optional[Dict[str, Any]]:
    """Runs before EVERY click-driven LCD case, not once per run: the
    shortest persisted display timeout is 1 minute, and the gaps between
    LCD cases (camera captures, other suites' cases in between) routinely
    exceed that, so a panel woken once at the start of the run can blank
    again before a later case.

    Wakes the panel via a real ``touch_inject`` press+release, then
    navigates back to home via the existing page-poll helper. The wake tap
    is only sent when ``touch.get_state()`` reports the panel blanked, has
    been idle for ``_WAKE_IDLE_MS_THRESHOLD`` ms or more, or cannot be read:
    on a blanked panel the tap is swallowed (lvgl_port.c/screen_idle.c) so
    (5,5) hits nothing, while on an awake panel it WOULD be delivered --
    on home that is the top-left auth-reset gesture corner
    (ui_page_home.c), inert without E-stop asserted and the full
    four-corner sequence, but not something to tap for no reason.

    Best-effort -- a TOUCH-task query failure (board not wired for touch
    injection, transport hiccup) must never crash a case; if the wake
    itself fails, the case's own click/page-wait logic still runs and fails
    honestly on its own terms rather than this helper manufacturing a false
    precondition failure.

    Also checks for and best-effort dismisses a stray LCD-19 popup (PIN
    keypad / Confirm Start / Confirm Stop) left open on `home` -- see
    :func:`_lcd19_clear_stray_overlay`. Returns its result (``None`` when no
    stray overlay was found) so a caller like LCD-01 can FAIL against the
    overlay by name instead of misattributing a swallowed tap to its own
    button. Most callers ignore the return value, same as before this
    existed."""
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    if touch is not None:
        need_wake = True
        try:
            state = touch.get_state()
            need_wake = (
                (not state.screen_on)
                or state.idle_ms >= _WAKE_IDLE_MS_THRESHOLD
                or _power_state_is_error_hold(state)
            )
        except Exception:
            need_wake = True
        if need_wake:
            try:
                x, y = _WAKE_TOUCH_XY
                touch.inject(x, y, True)
                touch.inject(x, y, False)
            except Exception:
                pass
            else:
                # Belt-and-suspenders: confirm the wake actually took before
                # the first real navigation click -- see
                # _WAKE_SCREEN_ON_TIMEOUT_S's comment. Never raises; a touch
                # task that can't answer GET_STATE just falls through to the
                # case's own click, same as before this wait existed.
                # Waits for power_state==ON specifically when the firmware
                # exposes it (see _power_state_is_on's comment) rather than
                # merely screen_on, which is also true in ERROR_HOLD.
                deadline = time.monotonic() + _WAKE_SCREEN_ON_TIMEOUT_S
                dismiss_attempts = 0
                while time.monotonic() < deadline:
                    try:
                        state = touch.get_state()
                    except Exception:
                        break
                    is_on = _power_state_is_on(state)
                    if is_on is None:
                        # Older firmware: fall back to the original screen_on-only check.
                        if state.screen_on:
                            break
                    elif is_on:
                        break
                    elif (
                        _power_state_is_error_hold(state)
                        and dismiss_attempts < _ERROR_HOLD_DISMISS_MAX_ATTEMPTS
                    ):
                        dismiss_attempts += 1
                        try:
                            x, y = _WAKE_TOUCH_XY
                            touch.inject(x, y, True)
                            touch.inject(x, y, False)
                        except Exception:
                            break
                    time.sleep(_WAKE_SCREEN_ON_POLL_S)
    ui = srv._ui_test
    # 2026-09-30: no longer stashed on ctx -- nothing read it, and ctx
    # persists across cases so a stashed value could go stale by the time a
    # later case's failure looked at it. _navigate_home() itself already
    # logs a warning on failure, so a failure here stays visible without
    # needing to be threaded through ctx.
    _navigate_home(ui)
    return _lcd19_clear_stray_overlay(ctx, ui)


def _lcd19_clear_stray_overlay(ctx: dict, ui) -> Optional[Dict[str, Any]]:
    """LCD-19's PIN keypad and Confirm Start/Confirm Stop dialogs are
    top-layer lv_msgbox popups that never move `get_current_page()` off
    'home' (module comment above `_lcd19_overlay_names`) -- a prior run
    that left one open (a torn-down bench session, an earlier case's own
    dismiss that silently didn't take) would otherwise be invisible to
    `_navigate_home()`, which only ever checks the page name. A case that
    then taps "Start" would see the tap swallowed by the stray popup and
    read as a genuinely unresponsive button, when the real cause is the
    leftover overlay.

    Only ever acts while the page is 'home': `_navigate_home()` (this
    function's own caller, :func:`_wake_and_home`) is best-effort and can
    leave the board parked on some other page (e.g. a stuck "network" page
    from a failed hop) -- this function's own click/touch_inject fallback
    assumes home's own overlays (ui_lcd_keypad.c / ui_confirm.c) and must
    never fire against an unrelated same-named widget on a different page
    (Wi-Fi provisioning's own "Cancel", a "Cannot Start" dialog's "OK", an
    on-glass profile-detail/review "OK" confirm -- none of these are the
    LCD-19 popups this helper exists to clear). Returns a dict recording the
    page actually seen, rather than a bare ``None``, so a caller (LCD-01) can
    tell "nothing to do, page wasn't home" apart from "checked home, nothing
    there" instead of both collapsing to the same silent ``None``.

    Returns a dict with ``present: False`` when the tap-target read failed,
    the page wasn't 'home', or 'home' was checked and found clean; otherwise
    (``present: True``) a dict with the overlay's ``names`` and the
    :func:`_dismiss_lcd19_overlay` result, for a caller to fold into
    `observed` and to FAIL against by name rather than blaming whatever
    button it was about to tap."""
    try:
        page = ui.get_current_page()
    except Exception:
        page = None
    if page != "home":
        return {"checked": True, "present": False, "page": page}
    names = _lcd19_overlay_names(ui)
    if names is None:
        return {"checked": False, "present": False, "page": page}
    if "Cancel" not in names and "OK" not in names:
        return {"checked": True, "present": False, "page": page}
    try:
        dismiss = _dismiss_lcd19_overlay(ctx, ui)
    except Exception as exc:  # noqa: BLE001
        dismiss = {"checked": False, "error": type(exc).__name__}
    return {"checked": True, "present": True, "page": page, "names": sorted(names), "dismiss": dismiss}


def _wait_for_page(ui, expected: str, timeout_s: "Optional[float]" = None,
                    interval_s: "Optional[float]" = None) -> "tuple[str, float]":
    """Poll ``ui.get_current_page()`` until it equals `expected` or
    `timeout_s` elapses. Returns ``(last_page_seen, waited_s)`` -- never
    raises, and never fabricates a match: a page that never arrives comes
    back as whatever the last poll actually saw, so a caller's existing
    ``page != expected`` check still fails the case honestly.

    `timeout_s`/`interval_s` default to (and are resolved against, at call
    time, not at import time) the module-level `_PAGE_POLL_TIMEOUT_S`/
    `_PAGE_POLL_INTERVAL_S` -- a bare ``= _PAGE_POLL_TIMEOUT_S`` default
    binds the value once when this function is defined, which would defeat
    a test's ``mock.patch.object(cases_lcd, "_PAGE_POLL_TIMEOUT_S", ...)``.
    See :func:`_lcd19_poll_overlay`'s docstring for the same pattern."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if interval_s is None:
        interval_s = _PAGE_POLL_INTERVAL_S
    start = time.monotonic()
    page = ui.get_current_page()
    while page != expected:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        page = ui.get_current_page()
    return page, time.monotonic() - start


def _wait_for_page_change(ui, before: str, timeout_s: "Optional[float]" = None,
                           interval_s: "Optional[float]" = None) -> "tuple[str, float]":
    """Same race as :func:`_wait_for_page`, for a caller that does not know
    the destination page's name in advance (a page transition mediated by
    the destination page's own definition) -- poll until the page differs
    from `before` instead of matching a fixed target.

    `timeout_s`/`interval_s` are resolved at call time -- see
    :func:`_wait_for_page`'s docstring."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if interval_s is None:
        interval_s = _PAGE_POLL_INTERVAL_S
    start = time.monotonic()
    page = ui.get_current_page()
    while page == before:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        page = ui.get_current_page()
    return page, time.monotonic() - start


def _wait_for_targets_change(ui, before_names: "set",
                              timeout_s: "Optional[float]" = None,
                              interval_s: "Optional[float]" = None) -> "tuple[dict, float]":
    """Same race as :func:`_wait_for_page`, for pages whose name never
    changes even though their content does -- ui_page_diagnostics.c's
    sub-tabs are internal to that one page (its own ``s_pages[]`` array,
    switched by ``show_page()``, never registered with kiln_ui.c's
    top-level page registry), so ``get_current_page()`` stays 'diagnostics'
    across every tab and cannot detect this transition. Poll
    ``list_tap_targets()`` instead, until its name set differs from
    `before_names` or `timeout_s` elapses.

    `timeout_s`/`interval_s` are resolved at call time -- see
    :func:`_wait_for_page`'s docstring."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if interval_s is None:
        interval_s = _PAGE_POLL_INTERVAL_S
    start = time.monotonic()
    tap = ui.list_tap_targets()
    names = {t.get("name") for t in tap.get("targets", [])}
    while names == before_names:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        tap = ui.list_tap_targets()
        names = {t.get("name") for t in tap.get("targets", [])}
    return tap, time.monotonic() - start


#: 2026-09-24 LCD-08 bench run 20260924T233113Z_lcd: the original click AND
#: its one retry both landed on a still-'home' page (~4.06s of combined
#: polling; see docs/BENCH_TEST_LOG.md for that run). A single retry
#: recovers a single swallow (confirmed live the same day: the screen_idle
#: wake-swallow race eats exactly one tap), but nothing bounds it to
#: exactly one -- a second, independent swallow (e.g. the panel re-blanking
#: between the first retry's click and its own page-poll, or a second
#: display_power ERROR_HOLD dismiss-then-pass-through edge,
#: firmware/KilnFW/App/drivers/persist/display_power_policy.c) is not a
#: distinguishable failure shape from a genuinely stuck hop -- it just
#: takes one more bounded attempt to tell them apart. Two retries (three
#: attempts total) trade at most one more `timeout_s` of wall time for
#: covering that double-swallow shape; a hop that is actually stuck still
#: fails, just after three bounded waits instead of two.
_CLICK_THEN_PAGE_MAX_RETRIES = 2

#: 2026-09-24: a click that comes back "swallowed" (kiln_ui_click_result_t's
#: KILN_UI_CLICK_SWALLOWED -- the press was actually delivered to LVGL but
#: screen_idle_touch_swallow() ate it as a wake/ERROR_HOLD-dismiss tap, see
#: kiln_ui.h) is now DIRECTLY OBSERVABLE, not merely inferred from a page
#: that failed to change. Retried immediately, on its own small bounded
#: budget separate from _CLICK_THEN_PAGE_MAX_RETRIES's page-didn't-change
#: retries below: a swallow is a known, expected race (the panel woke or
#: dismissed an error on this exact tap), not the "maybe stuck, maybe raced"
#: ambiguity the page-poll retries exist for.
_CLICK_THEN_PAGE_SWALLOW_RETRIES = 2

#: 2026-09-30 (LCD-09/LCD-16 bench investigation, run 20260930T215921Z_lcd,
#: both FAILing with `click_by_name('settings') returned 'not_found'` on the
#: very FIRST click of the case, `page_before == "home"` -- not the
#: page-didn't-change ambiguity `_CLICK_THEN_PAGE_MAX_RETRIES` exists for,
#: and not a "swallowed" race either, since a swallow is a distinct,
#: directly-reported result). Root cause is on the firmware side, not in
#: kiln_ui.c's tap-walk ordering (e388752c's actionable-pass split, which
#: this run was meant to be validating): `kiln_ui_click_by_name()`
#: (firmware/KilnFW/App/drivers/ui/kiln_ui.c) dispatches its tap-target walk
#: onto lvgl_port_task, and that function's OWN doc comment says so
#: explicitly -- "a dispatch timeout here ... surfaces as n == 0, i.e. no
#: targets found ... which the match loop below already treats as
#: KILN_UI_CLICK_NOT_FOUND -- indistinguishable from a genuinely absent
#: name". The dispatch window is `UI_WALK_WAIT_TIMEOUT_MS = 300` ms
#: (lvgl_port.c); a busy lvgl_port_task -- e.g. still flushing the page that
#: was just loaded, or servicing the home page's 1 Hz refresh tick, which
#: CLAUDE.md's stack/DRAM notes already document as doing three MAX31856
#: SPI reads, a 200 ms-capable queue wait and four interrupts-disabled heap
#: walks -- can blow past that window and empty the walk for a target that
#: is genuinely on screen. `docs/BENCH_TEST_LOG.md`'s
#: `20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a` entry recorded this
#: exact `click_by_name('settings')` -> `not_found` shape on a board
#: running a commit from BEFORE e388752c existed, confirming this race
#: predates and is independent of that commit's actionable-tap-target-
#: ordering fix.
#:
#: A bounded retry here cannot make a genuinely-absent target start
#: existing, so it costs nothing on a real absence (still fails, just one
#: extra round trip later) while resolving the transient-dispatch-timeout
#: shape above. Retrying blind is safe, unlike a swallow:
#: kiln_ui_click_by_name() only injects a press AFTER finding a match in
#: the walk, so a `not_found` result never sent a press this retry could
#: double up on.
_CLICK_THEN_PAGE_NOT_FOUND_RETRIES = 2

#: 2026-09-30: firmware now reports the same lvgl_port_task dispatch-timeout
#: race _CLICK_THEN_PAGE_NOT_FOUND_RETRIES exists for as its OWN result,
#: 'walk_busy' (KILN_UI_CLICK_WALK_BUSY / UI_TEST_CLICK_WALK_BUSY), instead of
#: folding it into 'not_found' -- see kiln_ui.h's doc comment on that enum
#: value. Retried the same way (blind, since no press was ever sent for a
#: walk that never completed), on its own separate counter so a bench log
#: records which of the two shapes actually happened, rather than merging a
#: brand-new, unambiguous "the walk timed out" signal back into the older,
#: ambiguous "not_found" bucket it was invented to disambiguate from. Older
#: firmware without this result value never reports 'walk_busy' at all (it
#: still reports 'not_found' for this case), so this retry is pure upside --
#: it never fires against old firmware, and _CLICK_THEN_PAGE_NOT_FOUND_RETRIES
#: still covers that firmware's own not_found race unchanged.
_CLICK_THEN_PAGE_WALK_BUSY_RETRIES = 2

#: Same dispatch-timeout race, for LIST_TAP_TARGETS instead of CLICK_BY_NAME
#: -- see _list_tap_targets_resolving_busy()'s docstring for the LCD-09
#: bench evidence this fixes.
_LIST_TAP_TARGETS_BUSY_RETRIES = 2


def _click_resolving_swallow(ui, name: str, max_swallow_retries: "Optional[int]" = None,
                              max_not_found_retries: "Optional[int]" = None,
                              max_walk_busy_retries: "Optional[int]" = None) -> "tuple[dict, int, int, int]":
    """click_by_name(), re-clicking immediately while the result reads
    'swallowed' (bounded by `max_swallow_retries`) -- a swallow is a
    directly observable, expected race (screen_idle ate the wake/dismiss
    tap), not the "maybe stuck" ambiguity the caller's own page-poll retry
    budget exists for, so it must not consume that separate budget.

    Also retries a 'not_found' result, on its own separate bounded budget
    (`max_not_found_retries` / `_CLICK_THEN_PAGE_NOT_FOUND_RETRIES`) -- see
    that constant's comment for why this is a transient lvgl_port_task
    dispatch-timeout race indistinguishable, at the firmware level, from a
    genuine absence, and why retrying it blind is safe (no press was ever
    sent). Unlike the swallow retry (a directly observed race with nothing
    left to wait out), a not_found retry pauses `_ENTER_PIN_RETRY_POLL_S`
    (0.15 s, the same poll period `ui_test_client.py`'s own bounded retries
    already use for an analogous "give the board a moment" wait) before
    re-clicking, since the dispatch-timeout theory only makes sense if
    lvgl_port_task gets a moment to drain before the retry lands in the same
    busy window.

    'verdict_unknown' (kiln_ui_click_by_name()'s own bounded wait for the
    swallow verdict timed out) is deliberately NOT re-clicked here: unlike a
    confirmed swallow, the press may well have reached the widget (the usual
    cause is a slow LVGL flush, which delays the verdict, not the press), so
    an immediate blind re-click could land on the page that press already
    opened. It is returned as-is for the caller to resolve against the
    observable page/target change, whose own retry is guarded by an
    unchanged page. Returns ``(final_click, swallow_retries_used,
    not_found_retries_used, walk_busy_retries_used)`` as three SEPARATE
    counters (2026-09-30 -- swallow/not_found used to be folded into one
    combined count, which misattributed a not_found recovery as a swallow
    recovery in a caller's `observed` dict; walk_busy is newer still, its own
    firmware result distinct from not_found -- see
    _CLICK_THEN_PAGE_WALK_BUSY_RETRIES's comment) so a caller's `observed`
    data records which race actually happened.

    `max_swallow_retries`/`max_not_found_retries`/`max_walk_busy_retries` are
    resolved at call time against `_CLICK_THEN_PAGE_SWALLOW_RETRIES`/
    `_CLICK_THEN_PAGE_NOT_FOUND_RETRIES`/`_CLICK_THEN_PAGE_WALK_BUSY_RETRIES`
    -- see :func:`_wait_for_page`'s docstring for why a bare default would
    defeat a test's patch."""
    if max_swallow_retries is None:
        max_swallow_retries = _CLICK_THEN_PAGE_SWALLOW_RETRIES
    if max_not_found_retries is None:
        max_not_found_retries = _CLICK_THEN_PAGE_NOT_FOUND_RETRIES
    if max_walk_busy_retries is None:
        max_walk_busy_retries = _CLICK_THEN_PAGE_WALK_BUSY_RETRIES
    click = ui.click_by_name(name)
    swallow_retries = 0
    not_found_retries = 0
    walk_busy_retries = 0
    while True:
        result = click.get("result")
        if result == "swallowed" and swallow_retries < max_swallow_retries:
            swallow_retries += 1
            click = ui.click_by_name(name)
            continue
        if result == "not_found" and not_found_retries < max_not_found_retries:
            not_found_retries += 1
            time.sleep(_ENTER_PIN_RETRY_POLL_S)
            click = ui.click_by_name(name)
            continue
        if result == "walk_busy" and walk_busy_retries < max_walk_busy_retries:
            walk_busy_retries += 1
            time.sleep(_ENTER_PIN_RETRY_POLL_S)
            click = ui.click_by_name(name)
            continue
        break
    return click, swallow_retries, not_found_retries, walk_busy_retries


def _list_tap_targets_resolving_busy(ui, max_busy_retries: "Optional[int]" = None) -> "tuple[dict, int]":
    """``ui.list_tap_targets()``, retried while the result reads busy --
    ``count == 0 and truncated`` (see ``ui_test_client.py``'s
    ``list_tap_targets()`` docstring for why that shape, not an empty
    ``targets`` list alone, is the busy signal: a real, completed walk of a
    genuinely empty page reports ``truncated=False``).

    2026-09-30 (LCD-09 bench investigation,
    ``logs/bench_test/20260930T234916Z_lcd_harness_retry_verify``): that run
    read back ``profiles_count: 29`` (the board genuinely had profiles) but
    ``row_count: 0`` and "could not locate the topbar icons (no 'back'/'home'
    anchor target)" -- ``_case_lcd09()``'s single, unretried
    ``ui.list_tap_targets()`` call landed on a busy walk (same
    ``UI_WALK_WAIT_TIMEOUT_MS`` dispatch-timeout race
    ``_CLICK_THEN_PAGE_WALK_BUSY_RETRIES`` retries for ``click_by_name()``)
    and read back nothing, which this case's own judge then reported as a
    missing-anchor defect rather than a transient timing race.

    Retried blind (nothing was ever "pressed" by a list call) with the same
    ``_ENTER_PIN_RETRY_POLL_S`` pause used by every other busy/not_found
    retry in this module, bounded by `max_busy_retries`
    (`_LIST_TAP_TARGETS_BUSY_RETRIES` by default). Returns
    ``(tap_result, busy_retries_used)`` -- `tap_result` is whatever the last
    call returned (still possibly busy if the budget was exhausted; the
    caller judges it same as before, just with a fresh chance for the walk to
    have actually completed)."""
    if max_busy_retries is None:
        max_busy_retries = _LIST_TAP_TARGETS_BUSY_RETRIES
    tap = ui.list_tap_targets()
    busy_retries = 0
    while tap.get("busy") and busy_retries < max_busy_retries:
        busy_retries += 1
        time.sleep(_ENTER_PIN_RETRY_POLL_S)
        tap = ui.list_tap_targets()
    return tap, busy_retries


def _click_then_page(ui, name: str, expected_page: str,
                      timeout_s: "Optional[float]" = None,
                      max_retries: "Optional[int]" = None) -> "tuple[Optional[CaseResult], str, float, int, int, int]":
    """Click a tap target by name, then wait for the page to become
    `expected_page`, and -- unlike a bare ``ui.click_by_name()`` +
    ``_wait_for_page()`` with the wait's return value left unchecked --
    actually stop and FAIL here if either step didn't land, instead of
    letting a caller march on to click a target that cannot exist on
    whatever page the board is really parked on.

    This is the 2026-09-24 bench root cause for LCD-09/14/16: each of
    those cases clicked "settings", called ``_wait_for_page(ui, "config")``
    but discarded its return value, and then unconditionally clicked the
    next target ("Profiles"/"Temperature"/"Diagnostics") even when the wait
    had actually timed out with the board still on "home" -- so the SECOND
    click's "not_found" was a symptom of the FIRST hop never being
    confirmed, not a defect in the second click's own target name.

    Returns ``(None, page, waited_s, swallow_retries, not_found_retries)`` on
    success (click replied 'ok' AND the page arrived within `timeout_s`) --
    `swallow_retries`/`not_found_retries` are the number of 'swallowed'/
    'not_found' re-clicks it took to reach that passing click (0 when the
    very first click needed neither), kept as two SEPARATE counters
    (2026-09-30 -- previously folded into one, which misattributed a
    not_found recovery as a swallow in a caller's `observed` dict) so a
    swallow or a not_found resolved on the way to a PASS is recorded, and
    correctly labelled, rather than discarded once the hop succeeds. A
    'verdict_unknown' click is never itself a pass: it is judged only by
    whether the expected page then arrives (the same evidence an 'ok' click
    is judged by), and is never blind re-clicked unless the page provably
    stayed where it was. On any failure, the first element is a
    ready-to-return FAIL :class:`CaseResult` -- naming the click's own
    result when the click itself failed, or the page the board is actually
    on (plus the blanked-screen hint, since a swallowed wake tap reads
    identically to this) when the click said 'ok' but the page never
    changed; both retry counters are 0 in every failure case (the FAIL's
    own ``observed`` dict already carries whatever retry counts apply).

    Worst case: ``(1 + max_retries) * (1 + _CLICK_THEN_PAGE_SWALLOW_RETRIES
    + _CLICK_THEN_PAGE_NOT_FOUND_RETRIES)`` clicks (15 at the defaults, only
    when every click is either 'swallowed' or 'not_found') plus
    ``1 + max_retries`` full `timeout_s` page waits (~6 s at the 2 s
    default); a 'not_found' that persists through
    `_CLICK_THEN_PAGE_NOT_FOUND_RETRIES` retries (see that constant's
    comment) fails immediately with no page wait at all -- there is no page
    to wait for when the click never even injected a press.

    `timeout_s`/`max_retries` are resolved at call time against
    `_PAGE_POLL_TIMEOUT_S`/`_CLICK_THEN_PAGE_MAX_RETRIES` -- see
    :func:`_wait_for_page`'s docstring for why a bare default would defeat
    a test's patch."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if max_retries is None:
        max_retries = _CLICK_THEN_PAGE_MAX_RETRIES
    try:
        page_before = ui.get_current_page()
    except Exception:
        page_before = None
    click, swallow_retries, not_found_retries, walk_busy_retries = _click_resolving_swallow(ui, name)
    if click.get("result") not in ("ok", "verdict_unknown"):
        # A swallow that outlasted _click_resolving_swallow()'s own budget is
        # attributed as such, never as "not_found" (the target WAS found). A
        # 'not_found' reaching here has likewise already outlasted that
        # helper's own _CLICK_THEN_PAGE_NOT_FOUND_RETRIES budget (2026-09-30
        # -- see that constant's comment), so it is no longer attributable to
        # the transient lvgl_port_task dispatch-timeout race it exists to
        # absorb; a genuine, reproducible absence is the remaining
        # explanation. 'inject_failed' (2026-09-24) is attributed distinctly
        # too: no press was ever sent (lvgl_port_inject_touch() itself
        # refused), so this is neither a missing/ambiguous/hidden target nor
        # a swallow -- never a pass, and never grounds to wait for a page
        # change this click could not have caused.
        if click.get("result") == "swallowed":
            immediate_attribution = "swallowed"
        elif click.get("result") == "inject_failed":
            immediate_attribution = "inject_failed"
        elif click.get("result") == "offscreen":
            # 2026-09-25: the target WAS found, but its reported centre lies
            # off the panel (KILN_UI_CLICK_OFFSCREEN) -- distinct from a
            # genuinely absent target, which "not_found" means.
            immediate_attribution = "offscreen"
        elif click.get("result") == "walk_busy":
            # 2026-09-30: the walk itself never completed (KILN_UI_CLICK_
            # WALK_BUSY) -- distinct from a completed walk that found no
            # match ("not_found"). Reaching here means it outlasted
            # _click_resolving_swallow()'s own _CLICK_THEN_PAGE_WALK_BUSY_
            # RETRIES budget.
            immediate_attribution = "walk_busy"
        else:
            immediate_attribution = "not_found"
        return (
            CaseResult(
                Verdict.FAIL,
                reason=f"click_by_name({name!r}) returned {click.get('result')!r}",
                observed={
                    "click": click,
                    "attribution": immediate_attribution,
                    # 2026-09-30 (LCD-16 bench root cause investigation,
                    # 20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a): the page
                    # the board was actually on right before this click is
                    # captured above (`page_before`) for the swallow-retry
                    # loop's own use below, but was previously discarded on
                    # this immediate-failure path -- the one place it matters
                    # most, since a "not_found" here is equally explained by
                    # a genuinely absent target OR by the board never having
                    # actually reached the page this click assumed it was on
                    # (e.g. a prior case's best-effort navigate-home silently
                    # failing). Recording it turns that ambiguity into
                    # evidence instead of requiring a fresh investigation
                    # each time it recurs.
                    "page_before": page_before,
                    **({"swallow_retries": swallow_retries} if swallow_retries else {}),
                    **({"not_found_retries": not_found_retries} if not_found_retries else {}),
                    **({"walk_busy_retries": walk_busy_retries} if walk_busy_retries else {}),
                },
            ),
            "",
            0.0,
            0,
            0,
            0,
        )
    page, waited_s = _wait_for_page(ui, expected_page, timeout_s=timeout_s)
    if page != expected_page:
        # Retry up to max_retries times: click_by_name() said 'ok' (the
        # target existed and an injection was issued) but the page never
        # arrived within timeout_s -- the documented screen_idle/touch-poll
        # swallow race this module's module docstring names, where the tap
        # that landed was the one that woke an already-reblanked panel
        # rather than actually reaching the widget underneath. A retry on
        # an awake panel (screen_on is already true post-wake, so a retry's
        # own tap cannot itself be a NEW wake edge) either recovers cleanly
        # or proves the hop is genuinely stuck. The 2026-09-24 LCD-08 bench
        # run (20260924T233113Z_lcd) showed a DOUBLE swallow -- the first
        # click AND one retry both stayed on 'home' -- so a single retry is
        # not enough to distinguish "rare double swallow" from "genuinely
        # stuck"; retries are still bounded (max_retries, default 2), so a
        # real defect still fails within a bounded time.
        #
        # Retried ONLY as long as the board is still on the exact page it
        # was on before the first click (the swallow shape: nothing
        # happened). If the page moved somewhere else (a late or wrong
        # transition), a further tap by the same name would land on a
        # DIFFERENT page's widget of that name -- never do that; fail
        # without a further tap. Every current caller clicks a pure
        # navigation target (settings, Profiles, Temperature, Diagnostics),
        # so a repeated tap on the unchanged source page is harmless; never
        # route a Start/Stop/Confirm/PIN-digit/toggle click through this
        # helper.
        retries_done = 0
        last_click = click
        total_swallow_retries = swallow_retries
        total_not_found_retries = not_found_retries
        total_walk_busy_retries = walk_busy_retries
        # Clicks confirmed delivered un-swallowed ('ok'); a 'verdict_unknown'
        # click is never counted here, so it can never supply the evidence
        # "genuine_defect" rests on.
        confirmed_ok_clicks = 1 if click.get("result") == "ok" else 0
        while (
            retries_done < max_retries
            and page_before is not None
            and page == page_before
        ):
            retries_done += 1
            last_click, retry_swallow_retries, retry_not_found_retries, retry_walk_busy_retries = _click_resolving_swallow(ui, name)
            total_swallow_retries += retry_swallow_retries
            total_not_found_retries += retry_not_found_retries
            total_walk_busy_retries += retry_walk_busy_retries
            if last_click.get("result") not in ("ok", "verdict_unknown"):
                break
            if last_click.get("result") == "ok":
                confirmed_ok_clicks += 1
            retry_page, retry_waited_s = _wait_for_page(ui, expected_page, timeout_s=timeout_s)
            page, waited_s = retry_page, waited_s + retry_waited_s
            if page == expected_page:
                return None, page, waited_s, total_swallow_retries, total_not_found_retries, total_walk_busy_retries
        # Attribution now that a swallow is directly observable
        # (click_by_name() returns "swallowed", resolved by
        # _click_resolving_swallow()'s immediate re-click) rather than only
        # ever inferred from a page that failed to change. Judged on the LAST
        # click in the chain, never on "was any swallow seen at all": a
        # swallow that an immediate re-click resolved to an un-swallowed "ok"
        # does NOT explain a page that still failed to move after that "ok"
        # -- labelling it a swallow would relabel a real defect as the known
        # wake/dismiss race. Caveat: firmware older than
        # KILN_UI_CLICK_VERDICT_UNKNOWN reports "ok" (not "swallowed") when
        # its bounded verdict wait times out, so on such firmware
        # "genuine_defect" means "no swallow was REPORTED", not a proof.
        last_result = last_click.get("result")
        if page_before is None:
            attribution = "page_before_unreadable"
        elif page != page_before:
            attribution = "wrong_page"
        elif last_result == "swallowed":
            # A retry's click stayed swallowed through its whole
            # _CLICK_THEN_PAGE_SWALLOW_RETRIES budget.
            attribution = "swallowed"
        elif last_result == "inject_failed":
            # A retry's press was never queued (lvgl_port_inject_touch()
            # refused) -- attributed the same as a first-click inject_failed.
            attribution = "inject_failed"
        elif last_result not in ("ok", "verdict_unknown"):
            attribution = "retry_click_failed"
        elif confirmed_ok_clicks >= 2:
            # At least two clicks were confirmed delivered un-swallowed and
            # the page never moved.
            attribution = "genuine_defect"
        elif last_result == "verdict_unknown" or click.get("result") == "verdict_unknown":
            # Fewer than two confirmed-clean clicks and at least one whose
            # swallow verdict the firmware never resolved -- neither a pass
            # nor enough evidence for "genuine_defect".
            attribution = "verdict_unknown"
        else:
            # max_retries=0: one un-swallowed "ok" and no move -- the old
            # single-attempt shape, not enough evidence either way.
            attribution = "wrong_page"
        observed = {
            "click": click,
            "page_before": page_before,
            "page": page,
            "page_wait_s": round(waited_s, 3),
            "attribution": attribution,
        }
        if retries_done > 0:
            observed["retry_click"] = last_click
            observed["retries"] = retries_done
        if total_swallow_retries:
            observed["swallow_retries"] = total_swallow_retries
        if total_not_found_retries:
            observed["not_found_retries"] = total_not_found_retries
        if total_walk_busy_retries:
            observed["walk_busy_retries"] = total_walk_busy_retries
        retried = f"retried {retries_done} time{'s' if retries_done != 1 else ''}"
        if attribution == "swallowed":
            reason = (
                f"click_by_name({name!r}) kept returning 'swallowed' and page stayed "
                f"{page!r}, expected {expected_page!r} ({retried})"
            )
        elif attribution in ("retry_click_failed", "inject_failed"):
            reason = (
                f"click_by_name({name!r}) returned 'ok' but page stayed {page!r}, "
                f"expected {expected_page!r}; retry click returned {last_result!r} ({retried})"
            )
        elif attribution == "verdict_unknown":
            reason = (
                f"click_by_name({name!r}) returned 'verdict_unknown' (last click: "
                f"{last_result!r}) and page stayed {page!r}, expected {expected_page!r} "
                f"({retried}); the firmware's own swallow-verdict wait did not resolve, "
                "so this is not a confirmed defect"
            )
        else:
            reason = (
                f"click_by_name({name!r}) returned 'ok' but page stayed {page!r}, "
                f"expected {expected_page!r}"
            )
            if attribution == "genuine_defect":
                reason += (
                    f" ({retried}, every final click delivered un-swallowed -- likely a "
                    "genuine UI defect, not a wake/dismiss race"
                    + (f"; {total_swallow_retries} earlier swallow(s) were resolved by an "
                       "immediate re-click" if total_swallow_retries else "")
                    + ")"
                )
            elif retries_done > 0:
                reason += f" ({retried})" + J.BLANKED_SCREEN_HINT
            else:
                reason += f" (page moved from {page_before!r}; not retried)" + J.BLANKED_SCREEN_HINT
        return (
            CaseResult(Verdict.FAIL, reason=reason, observed=observed),
            page,
            waited_s,
            0,
            0,
            0,
        )
    return None, page, waited_s, swallow_retries, not_found_retries, walk_busy_retries

def _click_then_targets_change(ui, name: str, prev_names: "set",
                                timeout_s: "Optional[float]" = None) -> "tuple[Optional[CaseResult], Optional[dict], float, bool]":
    """Diagnostics-sub-tab analogue of :func:`_click_then_page`: a sub-tab
    switch never changes kiln_ui's top-level page name
    (``_wait_for_targets_change``'s docstring), so the same
    screen_idle-swallow race that helper guards against has to be detected
    by tap-target-set membership here instead of page name. 2026-09-24 bench
    root cause for LCD-16: the old loop broke on any non-'ok' click result
    with no retry, and separately never even checked whether the target set
    had actually changed after a click that DID say 'ok' -- so a swallowed
    tap on this loop silently reused the previous tab's stale targets.

    Retried once, only when the set is unchanged (the swallow shape); never
    retried when the set changed to something else, since a second tap on a
    page that already moved could land on a different tab's widget of the
    same name. Every title here is a pure navigation tap, same class as
    _click_then_page's callers, so a harmless double-tap is fine.

    Unlike :func:`_click_then_page`, an unchanged set after the retry is
    NOT reported as a hard FAIL: some diagnostics sub-tabs (e.g. an empty
    Crash Report) legitimately show no distinguishing tap targets at all,
    so "the set didn't change" is not, by itself, proof of a swallowed tap
    the way "the page name didn't change" is for a real navigation hop.
    This only ever fails on an outright not_found click; the caller gets
    the best tap-target read available after one retry either way, plus a
    ``changed`` flag (the fourth tuple element) saying whether that read
    actually differs from ``prev_names``. A caller must not treat an
    unchanged read as the new tab's content: it is the previous tab's
    targets (or a dead tab bar that answers 'ok' and does nothing), and
    reading per-tab values off it would confirm the wrong tab.

    `timeout_s` is resolved at call time against `_PAGE_POLL_TIMEOUT_S` --
    see :func:`_wait_for_page`'s docstring for why a bare default would
    defeat a test's patch."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    click = ui.click_by_name(name)
    # 'verdict_unknown' (the press was injected; only its swallow verdict
    # timed out) is judged by the target-set change below exactly like 'ok'
    # -- never a pass on its own, never a hard not_found FAIL.
    if click.get("result") == "walk_busy":
        # 2026-09-30: the walk dispatch itself timed out (KILN_UI_CLICK_
        # WALK_BUSY), distinct from a completed walk that found no match --
        # same lvgl_port_task race _click_resolving_swallow() retries for
        # _click_then_page(). One bounded retry here too, before falling
        # through to the same not-ok handling below (which now labels a
        # still-busy result as "walk_busy", not "not_found").
        time.sleep(_ENTER_PIN_RETRY_POLL_S)
        click = ui.click_by_name(name)
    if click.get("result") not in ("ok", "verdict_unknown"):
        if click.get("result") == "inject_failed":
            attribution = "inject_failed"
        elif click.get("result") == "offscreen":
            # 2026-09-25: the target WAS found, but its reported centre lies
            # off the panel -- distinct from a genuinely absent target.
            attribution = "offscreen"
        elif click.get("result") == "walk_busy":
            attribution = "walk_busy"
        else:
            attribution = "not_found"
        return (
            CaseResult(
                Verdict.FAIL,
                reason=f"click_by_name({name!r}) returned {click.get('result')!r}",
                observed={"click": click, "attribution": attribution},
            ),
            None,
            0.0,
            False,
        )
    tap, waited_s = _wait_for_targets_change(ui, prev_names, timeout_s=timeout_s)
    names = {t.get("name") for t in tap.get("targets", [])}
    if names == prev_names:
        retry_click = ui.click_by_name(name)
        if retry_click.get("result") in ("ok", "verdict_unknown"):
            tap2, waited_s2 = _wait_for_targets_change(ui, prev_names, timeout_s=timeout_s)
            waited_s += waited_s2
            tap = tap2
    changed = {t.get("name") for t in tap.get("targets", [])} != prev_names
    return None, tap, waited_s, changed


#: Mirrored from firmware/KilnFW/App/drivers/ui/ui_theme.h -- reference
#: values ONLY. Per CLAUDE.md ("never by matching theme source constants"),
#: a sampled region is judged against these plus a live bezel sample
#: (lcd_sampler.matches_color()'s min_bezel_contrast floor), never against
#: this hex value alone with no reference to what the camera actually saw.
_ACCENT_4_RGB = (0x5C, 0xC0, 0x6E)
_ACCENT_1_RGB = (0xE8, 0x97, 0x4E)
_ACCENT_5_RGB = (0xD6, 0x55, 0x5F)

#: Mirrored from UI_THEME_COLOR_BG_HEX (ui_theme.h); the drift guard is
#: test_bench_test_cases_lcd.py's ThemeMirrorDriftTest. It is diagnostic
#: only (judge_lcd_home_idle) and never softens a verdict on its own --
#: it only feeds ``cast_channel`` below, which is itself gated (see
#: matches_color()'s docstring).
#:
#: 2026-09-25: moved off widget (5, 5). That point was originally chosen
#: because it renders as plain screen background (ui_page_home.c's
#: transparent top-left corner zone above the transparent topbar) -- but
#: ui_page_home.c has since grown a temperature-graph widget spanning most
#: of the left half of the home page, and (5, 5) now reliably samples that
#: graph's gradient fill instead (bright, e.g. RGB(113,217,253) -- nowhere
#: near dark background), which made this diagnostic's chroma_offset noisy
#: and occasionally borderline against CAST_CHROMA_THRESHOLD even on
#: ordinary, non-cast captures. (470, 5) -- just left of the topbar's
#: right-side icons, right of the graph -- was checked against seven bench
#: captures across three pages (home/config_hub/temperature) and reliably
#: reads dark background with no widget on any of them.
_BG_REFERENCE_XY = (470, 5)
_BG_RGB = (0x1A, 0x1F, 0x2B)

#: Mirrored from UI_THEME_COLOR_CARD_HEX/UI_THEME_COLOR_TEXT_PRIMARY_HEX
#: (ui_theme.h) -- used only by LCD-14's contrast judge
#: (`_sample_row_contrast`) and its unit tests, never compared to a sampled
#: pixel directly (same CLAUDE.md rule the pair above follows).
_CARD_RGB = (0x24, 0x2A, 0x3A)
_TEXT_PRIMARY_RGB = (0xF0, 0xF0, 0xF0)


def _srv(ctx: dict):
    srv = ctx.get("srv")
    if srv is None:
        from .. import mcp_server as srv  # local import: keeps this module importable with no board attached
    return srv


def _remember_page_targets(ctx: dict, page: str, tap_targets: dict) -> None:
    ctx.setdefault("_lcd_pages_visited", {})[page] = tap_targets


def _capture_dir(ctx: dict) -> str:
    """Where to save a captured frame. Defaults to ``<run_dir>/captures``
    (created by report.py's write_run() -- see runner.py, ``ctx["run_dir"]``
    is set before any case runs) so a captured frame actually lands where
    the run's own report expects evidence to live, instead of always
    falling back to a scratch tempdir that nothing downstream ever reads
    (the pre-existing bug behind LCD-01's empty ``captures/``/``evidence=[]``
    symptom -- nothing ever set ``ctx["_lcd_capture_dir"]``). An explicit
    ``ctx["_lcd_capture_dir"]`` (e.g. a unit test's tmp_path) still wins."""
    override = ctx.get("_lcd_capture_dir")
    if override:
        return override
    run_dir = ctx.get("run_dir")
    if run_dir:
        captures_dir = os.path.join(run_dir, "captures")
        try:
            os.makedirs(captures_dir, exist_ok=True)
        except OSError:
            return tempfile.gettempdir()
        return captures_dir
    return tempfile.gettempdir()


def _try_capture_and_sample(ctx: dict, targets: "list[dict]") -> "tuple[Optional[tuple], Optional[bool], dict]":
    """Best-effort: capture one -Full frame and sample the 'start'/'pause'
    tap targets' regions. Returns (start_sample_or_None, pause_is_hidden,
    color_debug) -- the samples are None/absent on any capture failure
    (camera busy etc.); ``color_debug`` carries the raw evidence (sampled
    RGB, bezel RGB, region coords, distances, thresholds, capture path) for
    CaseResult.observed/evidence so a FAIL or PASS verdict on Start's color
    can be corroborated later instead of being a bare bool (CLAUDE.md:
    judge colors by numeric pixel sampling, and that sampling has to be
    recorded to be checked)."""
    # Case-insensitive: firmware's label is Title Case ("Start"/"Pause");
    # see judgments._find_target's docstring for why this can't just be "==".
    start = J._find_target(targets, "start")
    pause = J._find_target(targets, "pause")
    color_debug: Dict[str, Any] = {}
    if start is None and pause is None:
        return None, None, color_debug
    image_path = os.path.join(_capture_dir(ctx), "lcd01_start_pause.jpg")
    try:
        lcd_sampler.capture_full_frame(image_path, repo_root=ctx.get("repo_root"))
    except lcd_sampler.LcdCaptureError:
        return None, None, color_debug
    color_debug["capture_path"] = image_path
    start_matches: Optional[bool] = None
    pause_hidden: Optional[bool] = None
    try:
        # Background reference sample (2026-09-24), computed FIRST so its
        # cast diagnosis can feed the Start-button match below. See
        # matches_color()'s "cast_channel" branch: a severe camera white-
        # balance cast can clip one whole channel to 0 across the entire
        # frame (bezel and background included), which is unrecoverable
        # information loss, not merely a dim reading -- when this reference
        # (expected to read the theme's dark, but non-zero, background)
        # shows exactly that signature, the crushed channel is dropped from
        # the Start-button color comparison rather than trusted as signal.
        # This never widens the check for an ordinary (uncast) frame: the
        # gate below (CAST_CHANNEL_CRUSH_MAX) requires the reference itself
        # to read essentially clipped, not just dark.
        bg_sample = lcd_sampler.sample_widget(image_path, *_BG_REFERENCE_XY, repo_root=ctx.get("repo_root"))
        bg_offset = lcd_sampler.chroma_offset(bg_sample.region, _BG_RGB)
        bg_on_bezel = (
            bg_sample.bezel is not None
            and lcd_sampler.color_distance(bg_sample.region, bg_sample.bezel) < lcd_sampler.MIN_BEZEL_CONTRAST
        )
        cast_suspected = (not bg_on_bezel) and bg_offset > lcd_sampler.CAST_CHROMA_THRESHOLD
        cast_channel: Optional[int] = None
        if cast_suspected:
            crushed = min(range(3), key=lambda i: bg_sample.region[i])
            if bg_sample.region[crushed] < lcd_sampler.CAST_CHANNEL_CRUSH_MAX:
                cast_channel = crushed
        bg_distance = lcd_sampler.color_distance(bg_sample.region, _BG_RGB)
        # Distinct from cast_suspected above (which gates on chroma alone,
        # for dropping a crushed channel from the Start-button comparison):
        # this instead catches a background reference that is plainly wrong
        # by absolute distance and/or chroma but stays under the (looser,
        # deliberately conservative -- see CAST_CHROMA_THRESHOLD's docstring)
        # cast bar, e.g. 2026-09-25's bg sample RGB(52,90,111) against target
        # RGB(26,31,43): chroma offset 0.0717 (under both CHROMA_MATCH_
        # TOLERANCE and CAST_CHROMA_THRESHOLD -- same hue, just brighter) but
        # distance ~93.7, over double COLOR_MATCH_TOLERANCE. A background
        # reference this far off by either measure means the exposure/cast
        # is suspect and a Start-button mismatch is no longer trustworthy
        # evidence of a real firmware defect -- see judge_lcd_home_idle(),
        # which downgrades a color FAIL to INCONCLUSIVE on this flag alone
        # (never the reverse: a good background never excuses a genuinely
        # wrong button color).
        bg_out_of_tolerance = (
            (not bg_on_bezel)
            and (not cast_suspected)
            and (bg_distance > lcd_sampler.COLOR_MATCH_TOLERANCE or bg_offset > lcd_sampler.CHROMA_MATCH_TOLERANCE)
        )
        color_debug["bg_reference"] = {
            "region_xy": _BG_REFERENCE_XY,
            "sampled_rgb": bg_sample.region,
            "bezel_rgb": bg_sample.bezel,
            "target_rgb": _BG_RGB,
            "distance": round(bg_distance, 2),
            "distance_tolerance": lcd_sampler.COLOR_MATCH_TOLERANCE,
            "chroma_offset": round(bg_offset, 4),
            "chroma_tolerance": lcd_sampler.CHROMA_MATCH_TOLERANCE,
            "cast_threshold": lcd_sampler.CAST_CHROMA_THRESHOLD,
            "reads_as_bezel": bg_on_bezel,
            "cast_suspected": cast_suspected,
            "cast_channel": cast_channel,
            "cast_channel_crush_max": lcd_sampler.CAST_CHANNEL_CRUSH_MAX,
            "bg_out_of_tolerance": bg_out_of_tolerance,
        }
        if start is not None and not start.get("hidden"):
            sample = lcd_sampler.sample_widget_body(image_path, start["cx"], start["cy"], repo_root=ctx.get("repo_root"))
            if sample.bezel is not None:
                start_matches = lcd_sampler.matches_color(
                    sample.region, _ACCENT_4_RGB, sample.bezel, cast_channel=cast_channel
                )
                # Record whether ONLY the degraded two-channel fallback made
                # this match, so judge_lcd_home_idle() can say so in the
                # verdict: with one channel dropped the check can no longer
                # tell ACCENT_4 from a hue that differs mainly in that
                # channel (ACCENT_1 orange shares ACCENT_4's G:B ~2:1), nor
                # a camera cast from a panel that genuinely lost it.
                matched_via_cast_fallback = bool(
                    start_matches
                    and cast_channel is not None
                    and not lcd_sampler.matches_color(sample.region, _ACCENT_4_RGB, sample.bezel)
                )
                color_debug["start"] = {
                    "region_xy": (start["cx"], start["cy"]),
                    "sample_offset_px": lcd_sampler.LABEL_AVOID_OFFSET_PX,
                    "sampled_rgb": sample.region,
                    "bezel_rgb": sample.bezel,
                    "target_rgb": _ACCENT_4_RGB,
                    # matches_color()'s first gate: a region too close to
                    # the bezel never matches, whatever its hue.
                    "bezel_distance": round(lcd_sampler.color_distance(sample.region, sample.bezel), 2),
                    "min_bezel_contrast": lcd_sampler.MIN_BEZEL_CONTRAST,
                    "distance": round(lcd_sampler.color_distance(sample.region, _ACCENT_4_RGB), 2),
                    "distance_tolerance": lcd_sampler.COLOR_MATCH_TOLERANCE,
                    "chroma_distance": round(
                        lcd_sampler.color_distance(
                            lcd_sampler._chromaticity(sample.region), lcd_sampler._chromaticity(_ACCENT_4_RGB)
                        ),
                        4,
                    ),
                    "chroma_tolerance": lcd_sampler.CHROMA_MATCH_TOLERANCE,
                    "cast_channel": cast_channel,
                    "degraded_chroma_distance": (
                        round(
                            lcd_sampler.color_distance(
                                lcd_sampler._chromaticity_excluding(sample.region, cast_channel),
                                lcd_sampler._chromaticity_excluding(_ACCENT_4_RGB, cast_channel),
                            ),
                            4,
                        )
                        if cast_channel is not None
                        else None
                    ),
                    "matches": start_matches,
                    "matched_via_cast_fallback": matched_via_cast_fallback,
                }
        if pause is not None:
            sample2 = lcd_sampler.sample_widget(image_path, pause["cx"], pause["cy"], repo_root=ctx.get("repo_root"))
            if sample2.bezel is not None:
                pause_hidden = lcd_sampler.is_off(sample2.region, sample2.bezel)
                color_debug["pause"] = {
                    "region_xy": (pause["cx"], pause["cy"]),
                    "sampled_rgb": sample2.region,
                    "bezel_rgb": sample2.bezel,
                    "distance_from_bezel": round(lcd_sampler.color_distance(sample2.region, sample2.bezel), 2),
                    "off_tolerance": lcd_sampler.MIN_BEZEL_CONTRAST,
                    "is_hidden": pause_hidden,
                }
    except lcd_sampler.LcdCaptureError:
        pass
    return start_matches, pause_hidden, color_debug


# ---------------------------------------------------------------------------
# LCD-01 -- home, idle.
# ---------------------------------------------------------------------------

def _case_lcd01(ctx: dict) -> CaseResult:
    stray = _wake_and_home(ctx)
    if stray is not None and stray.get("present") and not (stray.get("dismiss") or {}).get("dismissed"):
        # A stray PIN keypad/Confirm popup left open from an earlier run
        # swallows every tap aimed at the home page underneath it -- fail
        # naming that overlay as the cause, not whatever this case's own
        # Start-button check would otherwise (wrongly) blame.
        return CaseResult(
            Verdict.FAIL,
            reason=(
                f"stray overlay left open before LCD-01 (tap targets={stray.get('names')}) "
                "and it could not be dismissed -- the home page underneath is not reachable, "
                "this is not a Start-button defect"
            ),
            observed={"stray_overlay": stray},
        )
    # `stray.get("checked") is False` means the stray-overlay read itself
    # timed out/failed (see _lcd19_clear_stray_overlay) -- not evidence of a
    # popup either way, but worth keeping in `observed` rather than
    # discarding, since it is otherwise indistinguishable from "checked and
    # clean" in a summary.json read after the fact.
    stray_read_failed = bool(stray) and stray.get("checked") is False
    srv = _srv(ctx)
    ui = srv._ui_test
    page = ui.get_current_page()
    tap = ui.list_tap_targets()
    targets = tap.get("targets", [])
    _remember_page_targets(ctx, "home", tap)

    if page != "home":
        # Wrong page entirely -- no point spending a webcam capture on it.
        result = J.judge_lcd_home_idle(page, targets, None, None)
        if stray_read_failed:
            result.observed = dict(result.observed or {})
            result.observed["stray_overlay_read_failed"] = True
        return result
    start_matches_accent4, pause_is_hidden, color_debug = _try_capture_and_sample(ctx, targets)
    result = J.judge_lcd_home_idle(page, targets, start_matches_accent4, pause_is_hidden, color_debug=color_debug)
    result = _downgrade_if_corners_stale(ctx, result, color_debug.get("capture_path"))
    if stray_read_failed:
        result.observed = dict(result.observed or {})
        result.observed["stray_overlay_read_failed"] = True
    return result


# ---------------------------------------------------------------------------
# LCD-08 -- config hub (tap the topbar gear, tap name "settings", from home).
# The old "Menu" button was removed 2026-08-20; the gear is icon-only and has
# no visible label, so ui_topbar.c's build_icon_named() stashes "settings" as
# a tap-name override for it (kiln_ui.c's log_tap_targets()).
# ---------------------------------------------------------------------------

def _case_lcd08(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    outcome: Optional[CaseResult] = None
    try:
        # 2026-09-24 bench root cause: this case used to call
        # click_by_name("settings") directly with no retry, so a swallowed
        # wake tap (screen_idle's touch-swallow race, same shape as every
        # other _click_then_page() caller) failed here with no second
        # chance. Migrated to the shared retry helper.
        fail, page, waited_s, swallow_retries, not_found_retries, walk_busy_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            outcome = fail
            return outcome
        tap = ui.list_tap_targets()
        _remember_page_targets(ctx, "config", tap)
        result = J.judge_lcd_config_hub(page, tap.get("targets", []))
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
        if swallow_retries:
            result.observed["swallow_retries"] = swallow_retries
        if not_found_retries:
            result.observed["not_found_retries"] = not_found_retries
        if walk_busy_retries:
            result.observed["walk_busy_retries"] = walk_busy_retries
        image_path = _capture(ctx, "lcd08_config_hub.jpg")
        if image_path:
            result.evidence = list(result.evidence or []) + [image_path]
        outcome = result
        return outcome
    finally:
        # 2026-09-30: fold _navigate_home()'s own success/failure into this
        # case's `observed` -- see _navigate_home()'s docstring. Only added
        # when it actually failed, so a normal PASS/FAIL's observed dict
        # isn't padded with a redundant "ok": True on every run.
        nav = _navigate_home(ui)
        if outcome is not None and not nav["ok"]:
            outcome.observed = dict(outcome.observed or {})
            outcome.observed["navigate_home"] = nav


# ---------------------------------------------------------------------------
# LCD-21 -- no-scroll budget, every page visited by an earlier LCD case in
# this same run (ctx["_lcd_pages_visited"], populated above).
# ---------------------------------------------------------------------------

def _case_lcd21(ctx: dict) -> CaseResult:
    pages_visited: Dict[str, Any] = ctx.get("_lcd_pages_visited", {})
    return J.judge_lcd_no_scroll_budget(pages_visited)


def _find(targets: "list[dict]", name: str) -> Optional[dict]:
    return next((t for t in targets if t.get("name") == name), None)


def _sample_widget_bool(ctx: dict, image_path: str, target: Optional[dict],
                         color: "tuple[int, int, int]") -> Optional[bool]:
    """Best-effort matches_color() for one already-captured frame. None on
    any failure or a missing/hidden target -- never a fabricated bool."""
    if target is None or target.get("hidden"):
        return None
    try:
        sample = lcd_sampler.sample_widget(image_path, target["cx"], target["cy"], repo_root=ctx.get("repo_root"))
        if sample.bezel is None:
            return None
        return lcd_sampler.matches_color(sample.region, color, sample.bezel)
    except lcd_sampler.LcdCaptureError:
        return None


def _sample_button_bool(ctx: dict, image_path: str, target: Optional[dict],
                         color: "tuple[int, int, int]") -> Optional[bool]:
    """Like _sample_widget_bool(), but for a button whose caption is
    centred on it (Pause/Resume, Start/Stop) -- uses sample_widget_body()
    to sample off the label rather than on it. See
    lcd_sampler.LABEL_AVOID_OFFSET_PX's comment. Do not use this for a
    non-button labelled widget (e.g. the home page's trip strip, whose own
    box is only as tall as its text) -- _sample_widget_bool is still
    correct for those."""
    if target is None or target.get("hidden"):
        return None
    try:
        sample = lcd_sampler.sample_widget_body(image_path, target["cx"], target["cy"], repo_root=ctx.get("repo_root"))
        if sample.bezel is None:
            return None
        return lcd_sampler.matches_color(sample.region, color, sample.bezel)
    except lcd_sampler.LcdCaptureError:
        return None


def _sample_widget_off(ctx: dict, image_path: str, target: Optional[dict]) -> Optional[bool]:
    if target is None:
        return None
    try:
        sample = lcd_sampler.sample_widget(image_path, target["cx"], target["cy"], repo_root=ctx.get("repo_root"))
        if sample.bezel is None:
            return None
        return lcd_sampler.is_off(sample.region, sample.bezel)
    except lcd_sampler.LcdCaptureError:
        return None


def _sample_row_contrast(ctx: dict, image_path: str, text_x: float, bg_x: float,
                          y: float) -> Optional[bool]:
    """Best-effort per-row content-presence check for LCD-14: sample the
    row's own text region (text_x) and a local no-text reference in the
    SAME row band (bg_x), and require they read distinctly different --
    rather than comparing either alone against the (dark) bezel
    (_sample_widget_off/is_off), which cannot tell a rendered row's own
    CARD background (UI_THEME_COLOR_CARD, 0x242a3a) apart from the page's
    darker BG a MISSING row would sample instead (UI_THEME_COLOR_BG,
    0x1a1f2b): both read similarly far from bezel, so a bezel-only check
    reads a missing row as rendered (the round-3-era bug this replaces). A
    local contrast check is immune to which of those two backgrounds the
    reference point lands on, since a missing row makes BOTH points read
    the SAME background (contrast ~0) while a genuinely rendered row's
    text (UI_THEME_COLOR_TEXT_PRIMARY, 0xf0f0f0) against its own CARD
    background measures far above lcd_sampler.ROW_CONTENT_MIN_CONTRAST.
    None on any capture failure -- never a fabricated bool."""
    try:
        text = lcd_sampler.sample_widget(image_path, text_x, y, repo_root=ctx.get("repo_root"))
        bg = lcd_sampler.sample_widget(image_path, bg_x, y, repo_root=ctx.get("repo_root"))
    except lcd_sampler.LcdCaptureError:
        return None
    if text.bezel is None or bg.bezel is None:
        return None
    return lcd_sampler.color_distance(text.region, bg.region) >= lcd_sampler.ROW_CONTENT_MIN_CONTRAST


def _downgrade_if_corners_stale(ctx: dict, result: CaseResult, image_path: Optional[str]) -> CaseResult:
    """Downgrade a FAIL to INCONCLUSIVE when lcd_sampler.frame_corners_look_stale()
    finds FRAME_CORNERS/DEFAULT_TRANSFORM landing on bezel instead of screen
    background at any of the four corner-check points -- the same symptom the
    2026-09-24 round-3 fix found and re-derived the corners for (see
    lcd_sampler.py's FRAME_CORNERS comment). Never touches a PASS, or an
    already-INCONCLUSIVE/NOT_RUN/SKIP verdict, and never masks a genuine
    color mismatch: the four check points are panel background, never a
    widget, so any of them reading as bezel means the geometry (or a dark
    panel) is at fault rather than the color judgment; the original FAIL
    reason is kept in observed["original_fail_reason"] and the verdict is
    only ever lowered to INCONCLUSIVE, never raised to PASS. A blank/dark
    panel also trips this check, hence the reason string names both."""
    if result.verdict != Verdict.FAIL or not image_path:
        return result
    try:
        stale = lcd_sampler.frame_corners_look_stale(image_path, repo_root=ctx.get("repo_root"))
    except Exception:
        stale = None
    if stale is not True:
        return result
    observed = dict(result.observed or {})
    observed["frame_corners_stale"] = True
    observed["original_fail_reason"] = result.reason
    return CaseResult(Verdict.INCONCLUSIVE, reason="frame corners stale or panel dark", observed=observed, evidence=result.evidence)


def _capture(ctx: dict, name: str) -> Optional[str]:
    """`name` must be unique per capture within a run: every frame's path
    is recorded in CaseResult.evidence, so a shared filename (LCD-04's
    before/after pair, LCD-02 vs LCD-04) would silently overwrite an
    earlier case's evidence with a later frame."""
    image_path = os.path.join(_capture_dir(ctx), name)
    try:
        lcd_sampler.capture_full_frame(image_path, repo_root=ctx.get("repo_root"))
    except lcd_sampler.LcdCaptureError:
        return None
    return image_path


def _navigate_home(ui) -> "Dict[str, Any]":
    """Best-effort restore to the home page -- used in `finally` blocks so a
    case that navigates away never leaves the board parked on a config/
    profiles/diagnostics sub-page for whatever runs next.

    2026-09-30 (LCD-16 bench investigation, 20260930T043143Z_lcd_lcd19_
    rerun_0929_59c9306a/summary.json): this used to be a bare
    ``try/except Exception: pass`` with no return value -- a failure to
    actually land back on "home" (e.g. a page read timing out, or the
    fallback "back" click itself not_found) was completely invisible to
    whatever case ran next, which could then fail a totally unrelated click
    against a target that only exists on "home" (see _click_then_page's own
    'settings' click and LCD-16). Returns a small status dict instead of
    None so a call site can fold it into its own `observed` for exactly
    this kind of diagnosis: ``{"page": <final page or None if unreadable>,
    "ok": <bool, True iff page == "home">, "exception": <exception class
    name, or None>}``. A failure is also logged as a warning here so it is
    visible even from a call site that doesn't inspect the returned dict.
    """
    exception_name = None
    try:
        if ui.get_current_page() != "home":
            # These strings must match kUiTopbarBackTapName/kUiTopbarHomeTapName
            # (ui_topbar.c) exactly -- nothing here checks that mechanically.
            #
            # "home" first, not "back": the config hub (show_home=false) has
            # no Home icon, so from diagnostics/temperature (back_page=
            # "config") a "back" click first only reaches config, where the
            # follow-up "home" click then returns not_found and strands the
            # board there. Home is present on every topbar page except home
            # and config itself, so it either works directly or (from
            # config) is a no-op we fall through from. Each click is
            # followed by _wait_for_page(), not an immediate read, for the
            # same click-then-read race this file documents above
            # (_PAGE_POLL_TIMEOUT_S).
            # Only wait out the poll timeout when the "home" click actually
            # landed on something (result 'ok') -- a page with no Home icon
            # at all (the config hub, show_home=false) makes click_by_name()
            # return 'not_found' immediately, and blindly polling the full
            # _PAGE_POLL_TIMEOUT_S here anyway (as this used to) burns up to
            # 2s of the case's own budget for nothing before ever trying
            # "back", which is what actually gets a from-config board home.
            # That wasted time was eating into the shortest display-timeout
            # margin (_WAKE_IDLE_MS_THRESHOLD) and left less slack before a
            # later click in the same case risked landing on a re-blanked
            # panel -- part of the 2026-09-24 bench failure class this file
            # documents at _click_then_page().
            click = ui.click_by_name("home")
            page = ui.get_current_page()
            if click.get("result") == "ok":
                page, _ = _wait_for_page(ui, "home")
            if page != "home":
                ui.click_by_name("back")
                page, _ = _wait_for_page(ui, "home")
    except Exception as exc:
        exception_name = type(exc).__name__

    try:
        final_page = ui.get_current_page()
    except Exception:
        final_page = None
    ok = final_page == "home"
    if not ok:
        log.warning(
            "_navigate_home: failed to reach 'home' (ended on %r, exception=%s)",
            final_page, exception_name,
        )
    return {"page": final_page, "ok": ok, "exception": exception_name}


# ---------------------------------------------------------------------------
# LCD-02 -- home page while HP-01 is firing (observer of HP-01's window, same
# pattern as SP-03/SP-06 in cases_safety.py). HP-01 does not itself stash any
# LCD/UI data today (only energized_samples/link_stats/zone temps), so most
# of the optional sub-checks below come back None/INCONCLUSIVE on a real run
# until HP-01 is extended to capture them -- honest given what is actually
# wired, never fabricated.
# ---------------------------------------------------------------------------

def _case_lcd02(ctx: dict) -> CaseResult:
    hp01 = ctx.get("_hp01")
    if hp01 is None:
        return CaseResult(Verdict.NOT_RUN, reason="HP-01 did not run in this session (ctx['_hp01'] absent)")
    srv = _srv(ctx)
    ui = srv._ui_test
    page = ui.get_current_page()
    tap = ui.list_tap_targets()
    targets = tap.get("targets", [])
    _remember_page_targets(ctx, "home", tap)
    start = _find(targets, "start")
    pause = _find(targets, "pause")
    profile_name = _find(targets, "profile_name")
    start_reads_stop = bool(start and str(start.get("label", "")).lower() == "stop")
    image_path = _capture(ctx, "lcd02_home_firing.jpg")
    pause_matches_accent1 = _sample_button_bool(ctx, image_path, pause, _ACCENT_1_RGB) if image_path else None
    progress_samples = hp01.get("progress_samples", [])
    profile_name_greyed = hp01.get("profile_name_greyed")
    profile_name_tap_noop = hp01.get("profile_name_tap_noop")
    if profile_name is None:
        profile_name_greyed = profile_name_greyed if profile_name_greyed is not None else None
    result = J.judge_lcd_home_firing(
        page, targets, start_reads_stop, pause_matches_accent1, progress_samples,
        profile_name_greyed, profile_name_tap_noop,
    )
    if image_path:
        result.evidence = list(result.evidence or []) + [image_path]
    return _downgrade_if_corners_stale(ctx, result, image_path)


# ---------------------------------------------------------------------------
# LCD-03 -- home page while HP-04 is paused (observer of HP-04's stashed
# pause window, ctx["_hp04"] -- see cases_heat.py's _case_hp04).
# ---------------------------------------------------------------------------

def _case_lcd03(ctx: dict) -> CaseResult:
    hp04 = ctx.get("_hp04")
    if hp04 is None:
        return CaseResult(Verdict.NOT_RUN, reason="HP-04 did not run in this session (ctx['_hp04'] absent)")
    targets = hp04.get("pause_ui_targets", [])
    pause = _find(targets, "pause")
    pause_label = pause.get("label") if pause else None
    duties = hp04.get("duties_while_paused", [])
    return J.judge_lcd_home_paused(pause_label, duties)


# ---------------------------------------------------------------------------
# LCD-04 -- home page's trip strip while a safety trip is latched (observer
# of OT-B01's expected S6a trip window, owned by another agent/wave). Reads
# live safety-diag state directly rather than assuming a specific ctx-stash
# key/shape from code not yet landed -- NOT_RUN if nothing is latched.
# ---------------------------------------------------------------------------

def _case_lcd04(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    diag = srv._safety.get_diag()
    trip_reason = getattr(diag, "trip_reason", 0) or 0
    if not trip_reason:
        return CaseResult(Verdict.NOT_RUN, reason="no safety trip is currently latched")
    ui = srv._ui_test
    targets_before = ui.list_tap_targets().get("targets", [])
    strip_before = _find(targets_before, "trip_strip")
    strip_visible_before = bool(strip_before and not strip_before.get("hidden"))
    image_path = _capture(ctx, "lcd04_trip_before_clear.jpg")
    strip_matches_accent5_before = (
        _sample_widget_bool(ctx, image_path, strip_before, _ACCENT_5_RGB) if image_path else None
    )
    try:
        expected_mask = 1 << (trip_reason - 1)
        got_mask = getattr(diag, "trip_mask", expected_mask)
        if got_mask != expected_mask:
            return CaseResult(
                Verdict.NOT_RUN,
                reason=f"trip_mask {got_mask:#06x} != formula {expected_mask:#06x} for trip_reason={trip_reason}; refusing to clear an unexpected trip",
            )
        srv.safety_clear_trip()
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"safety_clear_trip() raised: {exc}")
    targets_after = ui.list_tap_targets().get("targets", [])
    strip_after = _find(targets_after, "trip_strip")
    image_path2 = _capture(ctx, "lcd04_trip_after_clear.jpg")
    strip_off_after_clear = _sample_widget_off(ctx, image_path2, strip_after) if image_path2 else None
    result = J.judge_lcd_home_tripped(strip_visible_before, strip_matches_accent5_before, strip_off_after_clear)
    result.evidence = list(result.evidence or []) + [p for p in (image_path, image_path2) if p]
    return _downgrade_if_corners_stale(ctx, result, image_path2 or image_path)


# ---------------------------------------------------------------------------
# LCD-09 -- profiles picker (tap Profiles from the config hub).
# ---------------------------------------------------------------------------

def _is_glyph_name(name: object) -> bool:
    """True for a tap target whose only name is an LVGL symbol glyph.
    ui_topbar.c's build_icon() sets no tap-name tag, so kiln_ui.c's tap
    walk falls back to the button's label text, which is the glyph's UTF-8
    bytes (LV_SYMBOL_PREV/NEXT/FILE are three bytes each).
    ui_test_client._unpack_str8() decodes names as ASCII with
    errors="replace", so each of those names arrives as U+FFFD repeated.
    Prev, Next and New therefore all read the same and can only be told
    apart by position."""
    return isinstance(name, str) and name != "" and set(name) == {"�"}


def _profiles_topbar_icons(targets: "list[dict]") -> Dict[str, Optional[bool]]:
    """Locate the profiles (manage) topbar's untagged Prev/Next/New icons by
    position. ui_page_profile_picker.c builds Back, Home, Prev, Next, New in
    that order in one flex row (ui_topbar.c), Back and Home carry the
    "back"/"home" tap names, and a disabled Prev/Next is dimmed but keeps
    its layout slot (ui_topbar_set_prev_enabled()), so the slots sit at
    home.cx + k * (home.cx - back.cx) for k = 1 (Prev), 2 (Next), 3 (New).

    A disabled icon is not clickable and so is not listed at all. ``add``
    is True/False for a glyph in slot 3. ``paging`` is True when New is in
    slot 3 and a glyph sits in the Prev or Next slot; None when New is in
    slot 3 but both paging slots are empty (both disabled: a single page of
    profiles -- the slots must exist or New would have been pulled left) or
    when New is not in slot 3 but slot 1/2 holds a glyph (undecidable, and
    the missing New FAILs on its own); False when no glyph occupies slots
    1-3 at all. Both are None when Back or Home is missing (no anchor)."""
    back = _find(targets, "back")
    home = _find(targets, "home")
    if back is None or home is None:
        return {"paging": None, "add": None}
    try:
        pitch = float(home["cx"]) - float(back["cx"])
        home_cx = float(home["cx"])
        home_cy = float(home["cy"])
    except (KeyError, TypeError, ValueError):
        return {"paging": None, "add": None}
    if pitch <= 0:
        return {"paging": None, "add": None}
    glyphs = [
        t for t in targets
        if _is_glyph_name(t.get("name")) and not t.get("hidden")
        and abs(float(t.get("cy", -1000)) - home_cy) <= pitch / 4
    ]

    def slot(k: int) -> bool:
        return any(abs(float(g.get("cx", -1000)) - (home_cx + k * pitch)) <= pitch / 4 for g in glyphs)

    prev_on, next_on, add_on = slot(1), slot(2), slot(3)
    paging: Optional[bool]
    if add_on:
        # New in slot 3 proves both paging slots exist in the row.
        paging = True if (prev_on or next_on) else None
    elif prev_on or next_on:
        # New is not in slot 3: a glyph in slot 1/2 is either New pulled
        # left (paging missing) or paging with New missing. Either way the
        # missing New already FAILs; paging itself is undecidable.
        paging = None
    else:
        paging = False
    return {"paging": paging, "add": add_on}


#: `ui_page_profile_picker.c`'s `build()`: scr's own `pad_all`
#: (`UI_THEME_PADDING_PX`=8) + the topbar (`UI_THEME_STATUS_BAR_HEIGHT_PX`=32)
#: + scr's own `pad_gap` (`UI_THEME_PADDING_PX`/2=4) puts `rows_col` (the
#: row list) at widget-space y=44 -- the identical derivation
#: `_LCD14_ZONE_ROW_Y0` below uses for `ui_page_temperature.c`'s content
#: area, since both pages build their `scr` from the same theme constants
#: and a `ui_topbar_create()` call. Anything at or above this y is the
#: topbar itself (back/home/Prev/Next/New, all at cy ~= 26).
_PROFILES_LIST_AREA_Y0 = 44.0


def _profile_rows_by_position(targets: "list[dict]") -> "list[dict]":
    """Locate the profiles list's row targets by POSITION, not by a fixed
    name. `build_row()` (`ui_page_profile_picker.c`) tags no tap name on
    the per-row name button -- `kiln_ui.c`'s tap walk falls back to the
    button's own label text, i.e. the profile's real name (same fallback
    rule `_is_glyph_name()`'s comment documents for the topbar icons) -- so
    a fixed `profile_row_N` prefix (what this replaced) never matched any
    real firmware target and made LCD-09 permanently INCONCLUSIVE
    ("no profile rows").

    A row is any non-hidden, non-glyph target below the list's own
    content-area boundary (`_PROFILES_LIST_AREA_Y0`), excluding the
    Delete button's own literal "Delete" label (manage mode only,
    `build_row()`) -- back/home/Prev/Next/New all sit above that boundary
    at cy ~= 26 and are excluded by it already. An unnamed target (the
    list's own clickable `rows_col` container) is never a row; the
    name/glyph checks are
    only needed for a target that, for whatever reason, reports a cy in
    the list band. Returned ordered top-to-bottom by cy, matching on-screen
    row order (`rows[0]` is the first row `_case_lcd09` taps)."""
    rows = []
    for t in targets:
        if t.get("hidden"):
            continue
        name = t.get("name")
        # An unnamed target is never a row: the list's own `rows_col`
        # container (`ui_page_profile_picker.c`, a plain lv_obj_create, so
        # CLICKABLE by default and walked with name "") sits at cy ~= 178,
        # inside the list band -- counting it made a full 4-row page read
        # as 5 rows (a false max_rows FAIL) and an empty list read as one
        # row. A real row's name button always carries its label text.
        if not isinstance(name, str) or name == "":
            continue
        # "Confirm?" is the same Delete button in its armed state
        # (ui_page_profile_picker.c's delete_btn_clicked_cb() relabels it,
        # and nothing resets the label when the 5 s window lapses) -- never
        # a row, and never something _case_lcd09's retried row tap may aim at.
        if _is_glyph_name(name) or name in ("back", "home", "Delete", "Confirm?"):
            continue
        try:
            cy = float(t.get("cy"))
        except (TypeError, ValueError):
            continue
        if cy >= _PROFILES_LIST_AREA_Y0:
            rows.append(t)
    rows.sort(key=lambda t: float(t["cy"]))
    return rows


def _case_lcd09(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    outcome: Optional[CaseResult] = None
    try:
        fail, _config_page, _, config_swallow_retries, config_not_found_retries, config_walk_busy_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            outcome = fail
            return outcome
        fail, page, waited_s, swallow_retries, not_found_retries, walk_busy_retries = _click_then_page(ui, "Profiles", "profiles")
        if fail is not None:
            outcome = fail
            return outcome
        swallow_retries += config_swallow_retries
        not_found_retries += config_not_found_retries
        walk_busy_retries += config_walk_busy_retries
        tap, list_busy_retries = _list_tap_targets_resolving_busy(ui)
        walk_busy_retries += list_busy_retries
        targets = tap.get("targets", [])
        _remember_page_targets(ctx, "profiles", tap)
        rows = _profile_rows_by_position(targets)
        # Round 3 (item 4): "paging"/"new_profile" were fabricated names no
        # firmware target has ever used -- ui_page_profile_picker.c's topbar
        # icons are untagged glyphs (_is_glyph_name()), only locatable by
        # position via _profiles_topbar_icons(). The literal-name lookups
        # this replaced could only ever return not-found.
        icons = _profiles_topbar_icons(targets)
        paging_present = icons["paging"]
        new_icon_present = icons["add"]
        detail_page = None
        if rows:
            # 2026-09-24 (round 4): this row tap used to be a bare
            # click_by_name() + _wait_for_page_change() with no retry --
            # the exact screen_idle/touch-poll swallow race this module's
            # docstring names for LCD-01/08/09/14/16, but LCD-08/14 were
            # migrated to _click_then_page()'s retry-once helper while this
            # one was missed. A swallowed row tap read back as page
            # unchanged ('profiles'), which judge_lcd_profiles_picker
            # reports as "tapping a row opened 'profiles', expected
            # 'profile_detail'" -- identical across three consecutive bench
            # runs (20260924T191429Z/203338Z/221915Z) even though the
            # firmware side (row_name_clicked_cb() in manage mode) does
            # call kiln_ui_show("profile_detail") correctly. The
            # destination is always "profile_detail" for a manage-mode row
            # (ui_page_profile_picker.c), so it can be named directly, same
            # as every other _click_then_page() caller in this module. A
            # double tap here is harmless: the row is a pure navigation
            # target, not a Start/Stop/Confirm/PIN-digit/toggle.
            row_fail, detail_page, row_waited_s, row_swallow_retries, row_not_found_retries, row_walk_busy_retries = _click_then_page(
                ui, rows[0]["name"], "profile_detail")
            swallow_retries += row_swallow_retries
            not_found_retries += row_not_found_retries
            walk_busy_retries += row_walk_busy_retries
            if row_fail is not None:
                row_fail.observed = dict(row_fail.observed or {})
                row_fail.observed["page_wait_s"] = round(waited_s + row_waited_s, 3)
                outcome = row_fail
                return outcome
        # profiles_count is a best-effort cross-check only: when the board
        # reports at least one profile over the wire but the list area
        # shows no rows at all, that is a real defect (a stuck/empty list),
        # not "undecidable" -- see judge_lcd_profiles_picker()'s handling
        # of `profiles_count`. None (read failure) never fabricates a FAIL.
        try:
            profiles_count = len(srv._profiles.list_all())
        except Exception:
            profiles_count = None
        result = J.judge_lcd_profiles_picker(
            page, rows, paging_present, new_icon_present, detail_page,
            profiles_count=profiles_count,
        )
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
        if swallow_retries:
            result.observed["swallow_retries"] = swallow_retries
        if not_found_retries:
            result.observed["not_found_retries"] = not_found_retries
        if walk_busy_retries:
            result.observed["walk_busy_retries"] = walk_busy_retries
        outcome = result
        return outcome
    finally:
        nav = _navigate_home(ui)
        if outcome is not None and not nav["ok"]:
            outcome.observed = dict(outcome.observed or {})
            outcome.observed["navigate_home"] = nav


# ---------------------------------------------------------------------------
# LCD-14 -- temperature page: zone rows and the Safety (K4) line are plain,
# non-clickable lv_labels (ui_page_temperature.c never tap-tags them -- the
# old zone_temp_*/safety_line name lookups this replaces could only ever
# return not-found). Round 3 (item 3) judges by capture instead: sample
# each zone row's own position for presence of rendered (non-background)
# content, and cross-check thermo_read() only for sanity (finite, in a
# plausible range), never pixel-value-vs-reading equality, since neither
# is derivable from a plain label over the wire.
# ---------------------------------------------------------------------------

#: Zone-row geometry, derived (not guessed) from ui_page_temperature.c's own
#: layout constants: scr's own pad_all (UI_THEME_PADDING_PX=8) + the topbar
#: (UI_THEME_STATUS_BAR_HEIGHT_PX=32) + the scr->content pad_gap
#: (UI_THEME_PADDING_PX/2=4) puts `content` at widget-space y=44. Each zone
#: row is UI_PAGE_TEMPERATURE_ZONE_ROW_HEIGHT_PX tall
#: ((UI_THEME_PADDING_PX/2*2) + UI_THEME_FONT_LINE_HEIGHT_PX(20) = 28px),
#: stacked with content's own pad_gap (4px) between them, so row i's
#: vertical centre is 44 + i*(28+4) + 14 = 58 + i*32. Horizontal centre is
#: the row's own midpoint (LCD_WIDTH/2=240) -- a space-between flex row so
#: the very centre pixel can fall between the two labels, but the row's own
#: CARD-colored background (UI_THEME_COLOR_CARD, distinctly non-bezel) still
#: fills it, so this only confirms the row rendered AT ALL, never that its
#: text is legible or its value correct. Documented alongside the camera-
#: aim geometry in docs/COMMISSIONING_LCD_RUNBOOK.md.
#:
#: The Safety (K4) line is NOT geometrically derived this way: it sits
#: inside the Relays card, after a relay-button row whose height depends on
#: how many buttons wrapped (ROW_WRAP, dynamic) -- there is no fixed offset
#: to compute without also modelling that wrap, so this case does not
#: attempt to sample it at all; judge_lcd_temperature_page() treats a None
#: reading here as informational, never a gate.
_LCD14_ZONE_ROW_Y0 = 58
_LCD14_ZONE_ROW_PITCH = 32
_LCD14_ROW_X = float(lcd_sampler.LCD_WIDTH) / 2.0

#: Row-content contrast sample points (_sample_row_contrast(), round-3
#: follow-up): the row is a SPACE_BETWEEN flex row with a left "Zone N" name
#: label and a right "-- C"/value label, so the left label's own text sits
#: close to the row's left pad edge (pad_all(UI_THEME_PADDING_PX/2)=4) --
#: _LCD14_ZONE_TEXT_X samples under that name label's text, never under the
#: value label (whose width/position varies with the reading's digit
#: count). _LCD14_ROW_X (the row's horizontal centre) doubles as the local
#: no-text background reference: a space-between row this short leaves that
#: midpoint empty in the gap between the two labels for any real zone name/
#: reading, so it reads the row's own CARD background when the row is
#: rendered, and the page's BG when the row (and its CARD background) is
#: simply absent.
_LCD14_ZONE_TEXT_X = 40.0

#: The zero-zone "No zones configured" label (`ui_page_temperature.c`'s
#: `s_zone_count == 0` branch) is a PLAIN label dropped directly into
#: `content` with no card wrapper and no pad_all of its own -- unlike a real
#: zone row, so it does NOT sit at `_LCD14_ZONE_ROW_Y0` (that offset already
#: bakes in a card's own border + top pad + half the font line). Its vertical
#: centre is simply content's own y (44, see the module comment above) plus
#: half of LV_FONT_DEFAULT's REAL line height -- montserrat_14's
#: `.line_height = 16` (components/lvgl/src/font/lv_font_montserrat_14.c),
#: NOT UI_THEME_FONT_LINE_HEIGHT_PX (20, a conservative budget figure for
#: the _Static_asserts, not a rendered height): 44+8=52. Sampled at the same
#: left-ish x as every other row's text column -- the label starts at x=8
#: (scr pad_all) and "No zones configured" is ~147 px wide (summed
#: montserrat_14 advances), so x=40 lands under its text and x=240 on plain
#: page BG.
#:
#: (The zone-row pitch above happens to come out the same either way: the
#: real row height is border 2 (lv_theme_default's card BORDER_WIDTH at
#: CONFIG_LV_DPI_DEF=130, applied to every plain lv_obj child that does not
#: zero it, which build_zone_temp_row()/build_relays_section() do not) +
#: pad 4 + line 16 + pad 4 + border 2 = 28, identical to the budget
#: macro's pad 4 + 20 + pad 4. The Relays header centre is likewise card
#: top + 2 + 4 + 8 = card top + 14, same as a row's centre offset.)
_LCD14_ZERO_ZONE_LABEL_Y = 52.0

#: Local no-text reference x for the Relays card HEADER sample only. The
#: zone rows' _LCD14_ROW_X (240) is a gap between a row's left name label
#: and its right-aligned temperature, but the header line "Relays -- zone
#: relays are view-only" is one left-aligned label ~242 px wide (summed
#: montserrat_14 advances) starting at x=14 (card x 8 + border 2 + pad 4),
#: so it spans x ~14..256 -- x=240 lands ON the header's own text, and a
#: correctly rendered header would compare text against text (low contrast,
#: a spurious FAIL). x=400 is inside the card (content ends at x=466) and
#: past the header's end, so it reads the card's own background there.
_LCD14_HEADER_REF_X = 400.0


def _configured_zone_count(srv) -> "tuple[Optional[int], Optional[str]]":
    """The board's own authoritative zone count, over the UART CONTROL link
    (`GET_ZONES`, `uart_bridge_ext_control.c`) -- the exact same
    `zones_config_get_thermo_count()` value `ui_page_temperature.c`'s
    `ui_page_temperature_build()` uses to decide how many zone rows to
    create (`s_zone_count`) and where the Relays card lands. This replaces
    the old `expected_zones = max(len(readings), 3)` guess (opus review of
    d66ba612): that guess could read 3 while the board is actually
    configured for 2 zones, in which case the "3rd" sample lands on the
    Relays card header -- itself rendered one row higher than a real 3rd
    zone row would be -- which reads as legitimate row content and PASSes.
    Reading the real count directly removes the guess entirely. Returns
    ``(count, error)`` -- `count` is None (never fabricated) if the query
    raised for any reason, with `error` naming what happened for the
    caller's `observed` dict."""
    try:
        _thermo_count, _relay_count, zones = srv._control.get_zones()
    except Exception as exc:  # noqa: BLE001
        return None, f"{type(exc).__name__}: {exc}"
    return len(zones), None


def _case_lcd14(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    outcome: Optional[CaseResult] = None
    try:
        fail, _config_page, _, config_swallow_retries, config_not_found_retries, config_walk_busy_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            outcome = fail
            return outcome
        fail, page, waited_s, swallow_retries, not_found_retries, walk_busy_retries = _click_then_page(ui, "Temperature", "temperature")
        if fail is not None:
            outcome = fail
            return outcome
        swallow_retries += config_swallow_retries
        not_found_retries += config_not_found_retries
        walk_busy_retries += config_walk_busy_retries
        tap, list_busy_retries = _list_tap_targets_resolving_busy(ui)
        walk_busy_retries += list_busy_retries
        _remember_page_targets(ctx, "temperature", tap)
        thermo_error: Optional[str] = None
        try:
            # ThermoClient has no read_all(); read(THERMO_CHANNEL_ALL) is the
            # actual API (2026-09-24 bench root cause: the old
            # srv._thermo.read_all() call always raised AttributeError,
            # silently swallowed by a bare except, which mislabeled this as
            # "no zone rows reported" instead of a coding bug). Readings are
            # kept only as a sanity cross-check (finite values) in
            # `observed`, never used to derive expected_zones any more (see
            # _configured_zone_count()) and never compared pixel-value-to-
            # number -- see this case's module comment for why.
            readings = {
                int(r.channel): float(r.temperature_c)
                for r in srv._thermo.read(THERMO_CHANNEL_ALL)
                if r.valid
            }
        except Exception as exc:
            readings = {}
            thermo_error = f"{type(exc).__name__}: {exc}"
        configured_zones, zones_count_error = _configured_zone_count(srv)
        zone_rows_rendered: Dict[int, Optional[bool]] = {}
        header_rendered: Optional[bool] = None
        zero_zone_label_rendered: Optional[bool] = None
        image_path = _capture(ctx, "lcd14_temperature.jpg")
        if image_path is not None and configured_zones is not None:
            if configured_zones == 0:
                zero_zone_label_rendered = _sample_row_contrast(
                    ctx, image_path, _LCD14_ZONE_TEXT_X, _LCD14_ROW_X, _LCD14_ZERO_ZONE_LABEL_Y)
            else:
                for zone in range(configured_zones):
                    y = float(_LCD14_ZONE_ROW_Y0 + zone * _LCD14_ZONE_ROW_PITCH)
                    zone_rows_rendered[zone] = _sample_row_contrast(
                        ctx, image_path, _LCD14_ZONE_TEXT_X, _LCD14_ROW_X, y)
                # The Relays card's own header line lands exactly where a
                # (configured_zones)'th zone row WOULD start -- both a card's
                # top pad and a zone row's top pad are UI_THEME_PADDING_PX/2,
                # and the header line and a zone row's own label are both
                # the first thing inside their respective pad, so the two
                # formulas coincide (see the module comment above). If the
                # board actually rendered ONE FEWER row than
                # `configured_zones` (the round-3 bug this fixes: a genuine
                # missing-row defect, not just a wrong guess), the header
                # shifts up by one full row pitch and this position lands
                # 22 px into the relay-button row instead (both sample
                # points on button background, no header text) -- catching
                # the "N-1 rows, header shifted up" failure shape a
                # per-row-only check cannot distinguish from a real Nth row
                # (both read as non-background content at the SAME sampled
                # position). Caveat: a relay button lit ON (ACCENT_5) under
                # exactly one of the two points could still read as
                # contrast there. The reference point is
                # _LCD14_HEADER_REF_X, not _LCD14_ROW_X -- see its comment.
                # Skipped (None, judged INCONCLUSIVE) if the header would
                # fall off the 320 px panel: unreachable today, since both
                # GET_ZONES and s_zone_count clamp to MAX31856_CHANNEL_COUNT
                # (3, header_y <= 154), and N <= 8 would still fit.
                header_y = float(_LCD14_ZONE_ROW_Y0 + configured_zones * _LCD14_ZONE_ROW_PITCH)
                if header_y + 4.0 <= float(lcd_sampler.LCD_HEIGHT):
                    header_rendered = _sample_row_contrast(
                        ctx, image_path, _LCD14_ZONE_TEXT_X, _LCD14_HEADER_REF_X, header_y)
        try:
            expect_safety_on = bool(srv._profiles.get_exec_status().state_name == "running")
        except Exception:
            expect_safety_on = False
        # Safety (K4) line position is not geometrically derivable (see the
        # module comment above) -- always reported None (unsampled), never
        # guessed.
        safety_line_rendered: Optional[bool] = None
        result = J.judge_lcd_temperature_page(
            page, zone_rows_rendered, readings, safety_line_rendered, expect_safety_on,
            configured_zones=configured_zones,
            header_rendered=header_rendered,
            zero_zone_label_rendered=zero_zone_label_rendered,
        )
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
        if swallow_retries:
            result.observed["swallow_retries"] = swallow_retries
        if not_found_retries:
            result.observed["not_found_retries"] = not_found_retries
        if walk_busy_retries:
            result.observed["walk_busy_retries"] = walk_busy_retries
        if thermo_error is not None:
            result.observed["thermo_error"] = thermo_error
        if zones_count_error is not None:
            result.observed["zones_count_error"] = zones_count_error
        if image_path:
            result.evidence = list(result.evidence or []) + [image_path]
        outcome = _downgrade_if_corners_stale(ctx, result, image_path)
        return outcome
    finally:
        nav = _navigate_home(ui)
        if outcome is not None and not nav["ok"]:
            outcome.observed = dict(outcome.observed or {})
            outcome.observed["navigate_home"] = nav


# ---------------------------------------------------------------------------
# LCD-16 -- diagnostics sub-pages, visited in order. Round 3 rewrite (item
# 2): the 8 sub-pages (ui_page_diagnostics.c's UI_PAGE_DIAGNOSTICS_PAGE_*,
# UI_PAGE_DIAGNOSTICS_PAGE_COUNT=8) have no buttons named after their
# titles -- they are reached ONLY by the generic topbar's Prev/Next glyph
# icons, and the sub-page title text itself (update_title()'s
# "Diagnostics: %s  %u of %u") is not readable over the wire at all
# (ui_test_client.py has no label-read command) -- only a webcam capture
# could confirm it, which this case does not take. The old per-title
# click_by_name(title) loop this replaces was therefore unconditionally
# broken: none of J._DIAG_TITLES was ever a real tap target, so the very
# first click always answered not_found.
# ---------------------------------------------------------------------------

def _diagnostics_next_target(targets: "list[dict]") -> Optional[dict]:
    """Locate the diagnostics topbar's untagged Next icon by position --
    same slot arithmetic as _profiles_topbar_icons() (Back, Home, Prev,
    Next in that order, ui_topbar.c's build_icon() left-to-right order
    comment), since Prev and Next share one undecodable glyph name
    (_is_glyph_name()) and click_by_name() answers AMBIGUOUS for it on a
    live board -- only a raw coordinate touch can target Next specifically.
    Returns Next's own tap-target dict (for its cx/cy) or None when it is
    disabled (last page, or a genuinely single-page diagnostics build) or
    the Back/Home anchors are missing."""
    back = _find(targets, "back")
    home = _find(targets, "home")
    if back is None or home is None:
        return None
    try:
        pitch = float(home["cx"]) - float(back["cx"])
        home_cx = float(home["cx"])
        home_cy = float(home["cy"])
    except (KeyError, TypeError, ValueError):
        return None
    if pitch <= 0:
        return None
    return _diagnostics_topbar_glyph(targets, home_cx + 2 * pitch, home_cy, pitch)


def _diagnostics_topbar_glyph(targets: "list[dict]", want_cx: float,
                              want_cy: float, pitch: float) -> Optional[dict]:
    for t in targets:
        if not _is_glyph_name(t.get("name")) or t.get("hidden"):
            continue
        try:
            if (abs(float(t.get("cx", -1000)) - want_cx) <= pitch / 4
                    and abs(float(t.get("cy", -1000)) - want_cy) <= pitch / 4):
                return t
        except (TypeError, ValueError):
            continue
    return None


def _diagnostics_prev_target(targets: "list[dict]") -> Optional[dict]:
    """Prev icon, one pitch right of Home (see _diagnostics_next_target()).
    None when Prev is disabled (first sub-page) or the anchors are missing."""
    back = _find(targets, "back")
    home = _find(targets, "home")
    if back is None or home is None:
        return None
    try:
        pitch = float(home["cx"]) - float(back["cx"])
        home_cx = float(home["cx"])
        home_cy = float(home["cy"])
    except (KeyError, TypeError, ValueError):
        return None
    if pitch <= 0:
        return None
    return _diagnostics_topbar_glyph(targets, home_cx + pitch, home_cy, pitch)


def _rewind_diagnostics_to_first(ctx: dict, ui, tap: dict,
                                 timeout_s: "Optional[float]" = None
                                 ) -> "tuple[dict, int, bool]":
    """Page the diagnostics screen back to sub-page 1 before LCD-16 counts
    forward hops. kiln_ui pages are never torn down (kiln_ui.h), so
    ui_page_diagnostics.c's s_page_index survives leaving the page: a
    previous LCD-16 run (or a human) that left it on the last sub-page makes
    the next visit open there, Next is disabled, and the case read 0/7 hops
    (run 20261001T172420Z_lcd_lcd22run; the 155811Z run passed only because
    the board had just rebooted). Taps Prev (raw touch, same as Next) until
    Prev is gone, bounded by the page count; a missing-anchor read is
    re-polled, never taken as "at page 1". A TRUNCATED read (253 B wire cap,
    see LCD-16) is not taken as "at page 1" either, since Prev may simply
    have been cut from the list: it is re-polled within the same bounded
    poll, and if every read stays truncated the rewind stops and reports it.
    Returns (latest read, taps sent, rewind_truncated). Best effort: any
    failure just returns what it has and LCD-16 judges."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    touch = getattr(_srv(ctx), "_touch", None)
    taps = 0
    if touch is None:
        return tap, taps, False

    def _anchors(tg: "list[dict]") -> bool:
        return _find(tg, "back") is not None and _find(tg, "home") is not None

    for _ in range(J.DIAGNOSTICS_PAGE_COUNT):
        targets = tap.get("targets", [])
        prev = _diagnostics_prev_target(targets)
        if prev is None:
            if _anchors(targets) and not tap.get("truncated"):
                break  # anchors present, complete list, no Prev: first sub-page
            start = time.monotonic()
            while time.monotonic() - start < timeout_s:
                time.sleep(_PAGE_POLL_INTERVAL_S)
                tap = ui.list_tap_targets()
                targets = tap.get("targets", [])
                prev = _diagnostics_prev_target(targets)
                if prev is not None:
                    break
                if _anchors(targets) and not tap.get("truncated"):
                    break
            if prev is None:
                if _anchors(targets) and not tap.get("truncated"):
                    break  # settled on a complete read: first sub-page
                # Never got a complete read: do not claim page 1.
                return tap, taps, bool(tap.get("truncated"))
        try:
            x, y = float(prev["cx"]), float(prev["cy"])
            touch.inject(x, y, True)
            touch.inject(x, y, False)
        except Exception:
            break
        taps += 1
        time.sleep(_PAGE_POLL_INTERVAL_S)
        tap = ui.list_tap_targets()
    return tap, taps, False


def _targets_signature(targets: "list[dict]") -> tuple:
    """A richer, order-independent signature than a bare set of names --
    ui_topbar.c's Prev/Next/New icons all decode to the SAME name
    (_is_glyph_name(), three replacement chars regardless of which glyph),
    so a diagnostics sub-page whose only change is Prev appearing/Next
    disappearing has an UNCHANGED name *set* even though the actual
    target list changed. Comparing (name, rounded cx, rounded cy, hidden)
    tuples, counted, catches that; a bare name set does not."""
    def _key(t: dict) -> tuple:
        try:
            cx = round(float(t.get("cx", -1)))
        except (TypeError, ValueError):
            cx = None
        try:
            cy = round(float(t.get("cy", -1)))
        except (TypeError, ValueError):
            cy = None
        return (t.get("name"), cx, cy, bool(t.get("hidden")))
    return tuple(sorted((_key(t) for t in targets), key=repr))


def _wait_for_targets_signature_change(ui, before_sig: tuple,
                                        timeout_s: "Optional[float]" = None,
                                        interval_s: "Optional[float]" = None
                                        ) -> "tuple[dict, float]":
    """Same polling shape as :func:`_wait_for_targets_change`, but keyed on
    :func:`_targets_signature` rather than a bare name set -- see that
    function's docstring for why a name set is blind to a Prev/Next-only
    change.

    `timeout_s`/`interval_s` are resolved at call time -- see
    :func:`_wait_for_page`'s docstring."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if interval_s is None:
        interval_s = _PAGE_POLL_INTERVAL_S
    start = time.monotonic()
    tap = ui.list_tap_targets()
    sig = _targets_signature(tap.get("targets", []))
    while sig == before_sig:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        tap = ui.list_tap_targets()
        sig = _targets_signature(tap.get("targets", []))
    return tap, time.monotonic() - start


def _tap_next_then_targets_change(ctx: dict, ui, prev_sig: tuple,
                                   timeout_s: "Optional[float]" = None,
                                   confirm: bool = True,
                                   ) -> "tuple[Optional[CaseResult], Optional[dict], float, bool, bool, bool]":
    """Raw-touch analogue of :func:`_click_then_targets_change`, for the
    diagnostics topbar's Next icon specifically -- click_by_name() cannot
    target it (see _diagnostics_next_target()'s docstring), so this locates
    Next by position and injects a raw press+release touch at its own
    cx/cy via ``srv._touch.inject()``, the same mechanism _wake_and_home()
    already uses for its wake tap. This is deliberately NOT added to any
    name-based safe-target allowlist (item 5): it never calls
    click_by_name() at all, so it cannot be confused with a retry-tap on
    Start/Stop/Confirm/PIN/toggle.

    ``prev_sig`` is a :func:`_targets_signature`, not a bare name set (see
    that function's docstring for why).

    ``confirm=False`` (an interior diagnostics hop, per _case_lcd16's
    boundary-hop distinction) skips the wait-for-change/retry-tap dance
    entirely: ui_topbar.c's Prev/Next icon positions never move between
    interior sub-pages, so a "no change" read there is not evidence of a
    swallowed tap the way it is at a boundary hop, and blindly retrying a
    second tap on that false suspicion would double-advance a page that
    the first tap already turned. Only a single touch is sent, and
    whatever the very next read shows is returned as-is, ``changed=True``
    unconditionally (the caller trusts the tap; see _case_lcd16).

    Returns ``(fail, tap, waited_s, changed, found_next, retried)``.
    ``found_next`` is False when Next could not be located at all
    (disabled/last page, or missing anchors) -- distinct from a
    located-but-swallowed tap, so a caller can tell "done paging" from
    "navigation broke". ``retried`` is True when a boundary hop needed its
    second tap; the caller records it, because a dropped INTERIOR tap is
    invisible at the time and only shows up later as the final boundary
    hop needing (and being rescued by) that retry.

    `timeout_s` is resolved at call time against `_PAGE_POLL_TIMEOUT_S` --
    see :func:`_wait_for_page`'s docstring."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    tap = ui.list_tap_targets()
    targets = tap.get("targets", [])
    next_target = _diagnostics_next_target(targets)
    if next_target is None and not confirm:
        # 2026-09-24 coordinator follow-up: this ENTRY read -- used to
        # locate Next before tapping it -- is taken right after the
        # PREVIOUS hop's own tap, and can race that hop's UI update: a
        # transiently incomplete target list (missing the back/home
        # anchors _diagnostics_next_target() itself needs) reads as "Next
        # is gone" here, which stopped paging after the first interior hop
        # even though the previous tap had landed cleanly and Next was
        # still present a moment later. This is a bounded STABILIZATION
        # poll only -- it never sends a touch and never compares against
        # prev_sig, so it cannot double-advance the page the way a
        # signature-based retry (the boundary-hop path below) would if
        # wrongly applied here (see this function's own docstring for why
        # that path is deliberately not reused for interior hops). It only
        # applies when confirm=False: a boundary hop's own
        # wait-for-signature-change loop already re-polls on this same
        # condition, so re-polling here too would just duplicate it.
        start = time.monotonic()
        while next_target is None:
            if time.monotonic() - start >= timeout_s:
                break
            time.sleep(_PAGE_POLL_INTERVAL_S)
            tap = ui.list_tap_targets()
            targets = tap.get("targets", [])
            next_target = _diagnostics_next_target(targets)
    if next_target is None:
        return None, tap, 0.0, False, False, False
    if touch is None:
        return (
            CaseResult(Verdict.INCONCLUSIVE, reason="no touch-inject transport available for Next", observed={}),
            None, 0.0, False, True, False,
        )
    try:
        x, y = float(next_target["cx"]), float(next_target["cy"])
    except (KeyError, TypeError, ValueError):
        return (
            CaseResult(Verdict.FAIL, reason="located Next icon has no numeric cx/cy", observed={"next_target": next_target}),
            None, 0.0, False, True, False,
        )

    def _tap() -> bool:
        try:
            touch.inject(x, y, True)
            touch.inject(x, y, False)
            return True
        except Exception:
            return False

    if not _tap():
        return (
            CaseResult(Verdict.FAIL, reason="touch inject failed for the Next icon", observed={}),
            None, 0.0, False, True, False,
        )
    if not confirm:
        # Only a single touch is sent, and whatever the very next read
        # shows is returned as-is, ``changed=True`` unconditionally -- the
        # caller trusts the tap (see this function's docstring). The
        # transient-blank race is handled above, at entry, for the
        # FOLLOWING hop's own Next-locating read -- not here.
        tap2 = ui.list_tap_targets()
        return None, tap2, 0.0, True, True, False
    tap2, waited_s = _wait_for_targets_signature_change(ui, prev_sig, timeout_s=timeout_s)
    sig2 = _targets_signature(tap2.get("targets", []))
    retried = False
    if sig2 == prev_sig:
        retried = True
        if _tap():
            tap3, waited_s2 = _wait_for_targets_signature_change(ui, prev_sig, timeout_s=timeout_s)
            waited_s += waited_s2
            tap2 = tap3
    changed = _targets_signature(tap2.get("targets", [])) != prev_sig
    return None, tap2, waited_s, changed, True, retried


def _case_lcd16(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    pages_paged = 0
    crash_report_step: Optional[int] = None
    relay_life_has_reset: Optional[bool] = None
    next_disabled_at_end: Optional[bool] = None
    expected_hops = J.DIAGNOSTICS_PAGE_COUNT - 1
    retried_hops: list = []
    outcome: Optional[CaseResult] = None
    try:
        fail, _config_page, _, config_swallow_retries, config_not_found_retries, config_walk_busy_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            outcome = fail
            return outcome
        # The config -> diagnostics hop IS a real top-level page switch
        # (kiln_ui's own page registry), so it gets the same checked
        # click-then-page-wait as every other hop; sub-page transitions
        # below never change the page name at all (ui_page_diagnostics.c's
        # sub-tabs are its own internal s_pages[], see
        # _targets_signature's docstring), so those are tracked by a
        # richer tap-target signature instead.
        fail, _diag_page, _, diag_swallow_retries, diag_not_found_retries, diag_walk_busy_retries = _click_then_page(ui, "Diagnostics", "diagnostics")
        if fail is not None:
            outcome = fail
            return outcome
        swallow_retries = config_swallow_retries + diag_swallow_retries
        not_found_retries = config_not_found_retries + diag_not_found_retries
        walk_busy_retries = config_walk_busy_retries + diag_walk_busy_retries
        first_tap, first_tap_busy_retries = _list_tap_targets_resolving_busy(ui)
        walk_busy_retries += first_tap_busy_retries
        first_tap, rewind_taps, rewind_truncated = _rewind_diagnostics_to_first(ctx, ui, first_tap)
        targets = first_tap.get("targets", [])
        # The LIST_TAP_TARGETS reply carries a `truncated` flag (253 B wire
        # cap, see kiln_ui.c's log_all_tap_targets()); a truncated read can
        # drop the topbar icons _diagnostics_next_target() needs, which
        # otherwise reads as "Next may be broken". Recorded, and named in
        # the reason whenever paging stops short.
        truncated_steps: list = [0] if first_tap.get("truncated") else []
        prev_sig = _targets_signature(targets)
        for step in range(J.DIAGNOSTICS_PAGE_COUNT):
            if _find(targets, "Reset") is not None:
                relay_life_has_reset = True
            if _find(targets, "Acknowledge") is not None:
                crash_report_step = step
            if step + 1 >= J.DIAGNOSTICS_PAGE_COUNT:
                next_disabled_at_end = _diagnostics_next_target(targets) is None
                break
            # A "boundary" hop (leaving step 0, or leaving the
            # second-to-last step into the last one) has a structurally
            # decidable signature change: Prev appears for the first time,
            # or Next disappears for good (ui_topbar.c's icon slots).
            # Every interior hop does not: the topbar's icon positions
            # never move between interior sub-pages, and most diagnostics
            # sub-pages carry no tap targets of their own at all, so an
            # interior hop legitimately produces the exact same signature
            # on success as it would if nothing happened -- see
            # _targets_signature's docstring. Only a boundary hop's failure
            # to change is real evidence Next is unresponsive; an interior
            # hop is not waited/retried at all (see
            # _tap_next_then_targets_change's confirm=False docstring).
            is_boundary_hop = step == 0 or step == expected_hops - 1
            fail, tap, _, changed, found_next, retried = _tap_next_then_targets_change(
                ctx, ui, prev_sig, confirm=is_boundary_hop)
            if fail is not None:
                outcome = fail
                return outcome
            if tap and tap.get("truncated"):
                # With found_next False the returned read is the CURRENT
                # sub-page's (Next could not be located on it); otherwise it
                # is the read taken after the hop, i.e. the next sub-page's.
                read_step = step + 1 if found_next else step
                if read_step not in truncated_steps:
                    truncated_steps.append(read_step)
            if retried:
                # Recorded, never judged: a retry at the LAST boundary hop
                # can also be absorbing an interior tap that was dropped
                # earlier (the interior hops are not confirmed), so the
                # final state can read correct while one tap was lost.
                retried_hops.append(step)
            if not found_next:
                # Next disappeared before reaching the last page -- paging
                # broke; stop here rather than looping with nothing to tap.
                break
            if not changed and is_boundary_hop:
                break
            pages_paged += 1
            targets = tap.get("targets", []) if tap else targets
            prev_sig = _targets_signature(targets)
        result = J.judge_lcd_diagnostics_pages(
            pages_paged, expected_hops, next_disabled_at_end,
            crash_report_step, relay_life_has_reset,
        )
        observed = dict(result.observed or {})
        observed["retried_hops"] = list(retried_hops)
        observed["tap_list_truncated_steps"] = list(truncated_steps)
        if rewind_taps:
            observed["rewind_prev_taps"] = rewind_taps
        if rewind_truncated:
            observed["rewind_truncated"] = True
        if swallow_retries:
            observed["swallow_retries"] = swallow_retries
        if not_found_retries:
            observed["not_found_retries"] = not_found_retries
        if walk_busy_retries:
            observed["walk_busy_retries"] = walk_busy_retries
        result.observed = observed
        if truncated_steps and pages_paged < expected_hops and result.verdict != Verdict.PASS:
            result.reason = (
                f"{result.reason} (LIST_TAP_TARGETS reply was truncated at sub-page step(s) "
                f"{truncated_steps} -- the topbar icons may have been cut from the list, "
                f"not missing from the screen)"
            )
        if rewind_truncated and result.verdict != Verdict.PASS:
            result.reason = (
                f"{result.reason} (the rewind to sub-page 1 stopped because "
                f"LIST_TAP_TARGETS stayed truncated, so the starting sub-page "
                f"is unknown)"
            )
        outcome = result
        return outcome
    finally:
        nav = _navigate_home(ui)
        if outcome is not None and not nav["ok"]:
            outcome.observed = dict(outcome.observed or {})
            outcome.observed["navigate_home"] = nav


# ---------------------------------------------------------------------------
# LCD-19 -- PIN lock after lcd_timeout_min. Reads ctx["_lcd_pin"] if
# WEB-SEC-04 already ran this session and populated it; otherwise self-seeds
# it via cases_web_rw.seed_lcd_pin(ctx) -- the exact same env-var lookup /
# GET config / set_lcd_pin write path WEB-SEC-04 itself uses, so a
# standalone bench_test_run(suite="lcd") run can exercise this case too.
# NOT_RUN happens only when KILNCTL_LCD_PIN (or ctx["lcd_admin_pin"]) is
# unset -- a FAIL happens instead if the env var is set but seeding itself
# fails (malformed PIN, GET/set_lcd_pin failure). Stop-requires-the-PIN
# (owner decision 2026-09-28, reversing the old "Stop is never gated" rule)
# is checked without assuming any particular shape for the rest.
#
# Both the PIN keypad (ui_lcd_keypad.c) and the Confirm Start/Confirm Stop
# dialogs (ui_confirm.c) are top-layer lv_msgbox popups, not pages -- current
# page/get_current_page() stays "home" the whole time, so this case can never
# use a page name to detect either one, and no page named "pin_entry" exists
# anywhere in the firmware. Both popups are identified from the tap-target
# list instead: the keypad has digit buttons + an "OK" footer button + a
# "Cancel" footer button (ui_lcd_keypad.c); a Confirm Start/Confirm Stop
# dialog has only its confirm_label ("Start" or "Stop") + "Cancel"
# (ui_confirm.c) -- no "OK". So "Cancel" present + "OK" present == keypad;
# "Cancel" present + no "OK" == a confirm dialog.
#
# UiTestClient.enter_pin() (2026-09-24) types a PIN's digits then "OK" via
# click_by_name(); wrong_pin_refused/right_pin_started are observed from the
# keypad's own reaction: a wrong PIN resets the entry but leaves the keypad
# open (the digit/"OK"/"Cancel" button set is unchanged -- there is no
# "OK"-disappears signal for a refusal the way there is for an accept), a
# right PIN closes the keypad in favour of the Confirm Start dialog ("OK"
# gone, "Start"/"Cancel" only). Because a wrong-PIN button set never visibly
# changes, `_wait_stable_names` debounces two matching reads instead of
# waiting for a specific target state, and `_entry_all_clicked_ok` requires
# every digit click AND the trailing "OK" click to have actually landed
# (result "ok") before either boolean is set from the read that follows --
# a not_found/ambiguous/hidden click anywhere in the PIN leaves the boolean
# at None (INCONCLUSIVE) rather than a false conclusion about a PIN that was
# never actually typed as intended. The board exposes no separate signal:
# ui_lcd_keypad.c's "Wrong PIN" status text is a plain (non-clickable)
# lv_label, never walked into kiln_ui.c's tap-target list (only
# LV_OBJ_FLAG_CLICKABLE objects and buttonmatrix keys are, log_tap_targets()
# around kiln_ui.c:482) -- so it cannot be read by name at all.
# "right_pin_started" is a placeholder name kept for judgments.py's existing
# signature; entering the right PIN only opens the Confirm Start dialog, it
# does not itself start a firing -- this case never presses Confirm Start.
#
# This case owns its own `lcd_enabled` on/off around its run (rather than
# relying on WEB-SEC-04 to leave it on): WEB-SEC-04 fully restores
# `lcd_enabled` to whatever it found in `finally`, which is normally false
# on this bench, and `ui_lcd_lock_has_role()` (ui_lcd_lock.c) returns true
# whenever the policy is off -- `ui_lcd_lock_run_gated()` then goes straight
# to Confirm Start with no keypad at all (ui_page_home_actions.c), so a case
# that assumed WEB-SEC-04 left the policy on could never see a keypad.
# LCD-19 turns `lcd_enabled` on immediately before driving the keypad and
# restores the original four policy fields (web_enabled, lcd_enabled,
# web_timeout_min, lcd_timeout_min) in `finally`, over the same
# `_SecHttpClient`/`ctx["sec_client"]` seam WEB-SEC-03/04 use -- verified by
# a full config read-back, hard-FAILing regardless of the keypad verdict if
# that restore doesn't round-trip on all four fields.
#
# This case must never actually start or stop a firing itself: on the idle
# branch it only ever taps "Start" (which opens the PIN keypad, not the
# firing); on the firing-active branch it only ever taps "Stop" (which opens
# Confirm Stop, never the confirm button itself). Whichever popup this
# leaves open is dismissed via "Cancel" in `finally`, never via the popup's
# own confirm button.
# ---------------------------------------------------------------------------

#: 2026-09-30 bench evidence (two runs): the post-submit poll after a PIN's
#: trailing "OK" click (both the wrong-PIN reset and the right-PIN
#: keypad-to-Confirm-Start handoff) saw roughly 14 non-repeating reads
#: followed by an empty one at the default `_PAGE_POLL_TIMEOUT_S` (2.0s) --
#: the screen is still mid-transition after submit at that point, not
#: actually stalled. A longer, named timeout for just these two polls avoids
#: racing that transition without changing any other case's timing (this
#: constant is LCD-19-specific and nothing else in this module reads it).
_PIN_SUBMIT_POLL_TIMEOUT_S = 6.0

#: uart_bridge_ui_test.c's walk-timeout sentinel (count 0, truncated True --
#: lvgl_port.c's 300ms window) is not the only shape a stalled read comes
#: back as on this bench: a bare empty (untruncated) listing has been seen
#: too. Neither is a real answer for any screen this module ever reads --
#: home, the config hub, and every LCD-19 popup always have at least one
#: tap target -- so both are treated as "no read yet, poll again", never as
#: "the overlay is gone" or "the keypad has no buttons".
def _lcd19_overlay_raw(ui) -> "tuple[Optional[set], bool]":
    """One raw tap-target read. Returns ``(names, truncated)``: `names` is
    ``None`` on a query failure OR an empty listing (see module note above),
    never an empty ``set()`` standing in for "nothing is here". `truncated`
    mirrors the firmware's own flag even when `names` is not None, so a
    genuinely partial (but non-empty) list is still recorded."""
    try:
        tap = ui.list_tap_targets()
    except Exception:
        return None, False
    targets = tap.get("targets", [])
    truncated = bool(tap.get("truncated"))
    if not targets:
        return None, truncated
    return {t.get("name") for t in targets if not t.get("hidden")}, truncated


def _lcd19_poll_overlay(ui, satisfied, timeout_s: Optional[float] = None,
                         interval_s: Optional[float] = None
                         ) -> "tuple[Optional[set], float, int, bool]":
    """Shared poll loop over :func:`_lcd19_overlay_raw`. ``satisfied(names,
    history)`` decides when to stop, where `names` is the most recent read
    (``None`` until a usable, non-empty listing arrives -- never a stand-in
    for "the overlay is gone") and `history` is the list of every read so
    far including `names` itself, oldest first.

    `timeout_s`/`interval_s` default to (and are resolved against, at call
    time, not at import time) the module-level `_PAGE_POLL_TIMEOUT_S`/
    `_PAGE_POLL_INTERVAL_S` -- a bare ``= _PAGE_POLL_TIMEOUT_S`` default
    binds the value once when this function is defined, so a test's
    ``mock.patch.object(cases_lcd, "_PAGE_POLL_TIMEOUT_S", ...)`` would
    silently do nothing for any of this module's several LCD-19 tests that
    call through a caller (`_dismiss_lcd19_overlay`, `_case_lcd19`) with no
    override of their own -- exactly what made this module's test file take
    ~3.5 minutes instead of a fraction of that. Resolving the sentinel here
    (and in :func:`_lcd19_overlay_names`, :func:`_wait_for_overlay_names`,
    :func:`_wait_stable_names`, the same three layers a no-override caller
    passes through) makes that patch effective without changing either
    production default.

    Returns ``(names, elapsed_s, empty_polls, truncated_seen)``:
    `empty_polls` counts every raw read that came back as the empty/timeout
    sentinel (recorded, never judged -- a caller reports it in `observed`
    rather than treating it as evidence of anything), and `truncated_seen`
    is True if any read set the firmware's `truncated` flag, even one later
    superseded by a full read. Never raises; a predicate that is never
    satisfied still returns honestly once `timeout_s` elapses."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if interval_s is None:
        interval_s = _PAGE_POLL_INTERVAL_S
    start = time.monotonic()
    empty_polls = 0
    truncated_seen = False
    history: "list[Optional[set]]" = []

    def _read() -> Optional[set]:
        nonlocal empty_polls, truncated_seen
        names, truncated = _lcd19_overlay_raw(ui)
        if truncated:
            truncated_seen = True
        if names is None:
            empty_polls += 1
        history.append(names)
        return names

    names = _read()
    while not satisfied(names, history):
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        names = _read()
    return names, time.monotonic() - start, empty_polls, truncated_seen


def _lcd19_overlay_names(ui, timeout_s: Optional[float] = None,
                          interval_s: Optional[float] = None) -> Optional[set]:
    """A single "real" overlay read for a caller that just needs a
    signature to diff against (a pre-click baseline) rather than
    diagnostics -- retries past the empty/timeout sentinel (see
    :func:`_lcd19_overlay_raw`) instead of treating it as an actual empty
    tap-target list. Still returns ``None`` if every read within
    `timeout_s` came back empty/failed, so a caller checking for `None`
    keeps working exactly as before."""
    names, _, _, _ = _lcd19_poll_overlay(ui, lambda n, h: n is not None, timeout_s, interval_s)
    return names


def _wait_for_overlay_names(ui, present: bool, timeout_s: Optional[float] = None,
                             interval_s: Optional[float] = None,
                             baseline: Optional[set] = None
                             ) -> "tuple[Optional[set], float, int, bool]":
    """Same click-then-read race as :func:`_wait_for_page`, for LCD-19's
    top-layer popups: both the PIN keypad and the Confirm Start/Confirm
    Stop dialogs are lv_msgbox popups raised asynchronously off the same
    LVGL-task touch poll as any page switch (see kiln_ui_click_by_name()'s
    comment), so an immediate tap-target read after Start/Stop/Cancel can
    still see the pre-click set. An empty/timeout read (see
    :func:`_lcd19_overlay_raw`) is never treated as a real "gone" or
    "present" answer either -- it just keeps polling.

    `present=True` (after opening a popup) does NOT wait for a bare
    non-empty set: the home page's own Start/nav buttons are *already*
    non-empty before the popup ever appears, so `bool(names)` is
    trivially true on the very first read, before LVGL has processed the
    click at all -- this was a real bug (root-caused after an LCD-19 FAIL
    that finished in 0.92s against a 2.0s timeout, far too fast to have
    actually waited for anything). Instead, when `present=True`, pass the
    pre-click ``baseline`` set: this waits until the read set differs from
    `baseline` (the popup's own digit/OK/Cancel or confirm/Cancel names
    joining it), not merely until it is non-empty. `baseline=None` with
    `present=True` falls back to the old bare-non-empty wait -- callers
    opening a popup must pass their own pre-click baseline.

    `present=False` waits for a real (non-empty) listing that simply has no
    popup content -- see :func:`_dismiss_lcd19_overlay`, which polls for
    "Cancel" specifically rather than using this branch, since every real
    screen here always has *some* tap target.

    Returns ``(names, elapsed_s, empty_polls, truncated_seen)`` -- see
    :func:`_lcd19_poll_overlay`. Never raises; a popup that never
    (dis)appears, or a set that never diverges from `baseline`, is still
    reported honestly via whatever the last poll saw."""
    def _satisfied(n: Optional[set], _history) -> bool:
        if n is None:
            return False  # no real read yet -- keep polling until timeout
        if present and baseline is not None:
            return n != baseline
        return bool(n) == present

    return _lcd19_poll_overlay(ui, _satisfied, timeout_s, interval_s)


def _is_keypad_names(names: Optional[set]) -> bool:
    """True when a tap-target read shows the PIN keypad's own signature:
    ``"OK"`` + ``"Cancel"`` plus all ten digit keys. The digits were added
    2026-09-30 (review of fa028826) so this can never alias a Confirm
    Start/Stop dialog, which also carries ``"Cancel"`` but never ``"OK"``
    -- belt-and-suspenders alongside that existing distinction. Never
    raises on ``None``."""
    return (names is not None and "OK" in names and "Cancel" in names
            and _KEYPAD_DIGIT_NAMES <= names)


_TAIL_STABLE_WINDOW = 2


def _wait_for_keypad_raise(ui, timeout_s: Optional[float] = None,
                            interval_s: Optional[float] = None) -> Dict[str, Any]:
    """Poll for the PIN keypad appearing after a Start/Stop tap, but --
    unlike a plain :func:`_wait_for_overlay_names` call -- watch the WHOLE
    read history for the keypad's own signature, not merely whatever the
    last read happened to be.

    ui_lcd_lock.c's `tick_timer_cb` inactivity expiry (L206-213) and its
    unlocked->locked edge (`ui_lcd_lock_force_lock`, L228-239) can both
    legitimately dismiss the keypad with no PC-side action in between --
    a keypad seen mid-poll and gone again by the last read is evidence of a
    raise/close race, not evidence the keypad never raised, and must read
    INCONCLUSIVE, never FAIL. (This is a documented firmware capability,
    not something confirmed by any one bench run's data -- see the
    `_case_lcd19` comment at the `keypad_raised_then_closed` site.)

    This function does its own read loop directly over
    :func:`_lcd19_overlay_raw` (rather than delegating to
    :func:`_lcd19_poll_overlay`) for two reasons, both from 2026-09-30
    review of the first version of this function:

    1. Stopping early on "two consecutive equal *real* reads" -- with no
       regard for whether that pair is `baseline` repeating itself before
       the keypad has actually appeared -- reproduces the exact "0.92s
       FAIL" bug :func:`_wait_for_overlay_names`'s docstring records: two
       identical pre-raise home reads (LVGL hasn't processed the tap yet)
       stabilize the debounce and end the poll with `raised_ever=False`,
       before a real, merely-delayed raise ever gets a chance to happen.
       Early stopping is therefore now gated on `raised_ever`: a stable
       repeat of the keypad's own signature still stops early (there is
       nothing further to learn), and a stable repeat *after* a confirmed
       raise stops early too (the close has settled) -- but a stable
       repeat of anything else, before any raise has been seen, is NOT a
       stop signal and the poll keeps running to `timeout_s`.
    2. Judging "never raised" as FAIL requires distinguishing a poll window
       with clean evidence throughout from one with an empty/timeout or
       truncated read (see :func:`_lcd19_overlay_raw`) anywhere in it --
       :func:`_lcd19_poll_overlay` only reports whether such a read
       happened ANYWHERE in the window (`empty_polls`/`truncated_seen`),
       not where, which forced the original whole-window "zero empty or
       truncated reads at all" gate. That gate is too strict: ordinary
       bench runs commonly see one empty/truncated read right after a
       click before the display catches up, so a real never-raises
       regression would read INCONCLUSIVE about as often as FAIL. Keeping
       each read's own `(names, truncated)` here instead supports a
       tail-based judgment (`never_raised_clean_tail`, see below).

    Returns a dict:
      ``names``            -- the last read seen (may be None if the
                               window ended on an empty/timeout read)
      ``last_real_names``  -- the last non-None read seen anywhere in the
                               poll, or None if every read in the window was
                               empty/timeout
      ``raised_ever``      -- True if any read in the window showed the
                               keypad's signature
      ``raised_now``       -- True if the LAST real read still shows it
      ``closed_after_raise`` -- raised_ever and not raised_now: the keypad
                               was seen, then was gone again before this
                               poll gave up
      ``never_raised_clean_tail`` -- True only when `raised_ever` is False
                               AND the LAST `_TAIL_STABLE_WINDOW` (2) reads
                               after the last empty/timeout/truncated read
                               in the window are real, identical to each
                               other, and none show the keypad -- i.e.
                               unambiguous negative evidence that survived
                               to the end of the poll. 2026-09-30 review
                               (round 2): this used to require the WHOLE
                               tail (every read after the last bad one) to
                               be identical, which is wrong for a home
                               screen carrying a live temperature label --
                               an earlier tail read can legitimately differ
                               from the last one with no keypad ever having
                               appeared. Only the last 2 reads need to agree
                               now. False whenever fewer than 2 clean reads
                               remain after the last bad one, the last 2
                               disagree, or the window ended on an empty/
                               truncated read.
      ``empty_polls`` / ``truncated_seen`` -- as :func:`_lcd19_poll_overlay`
      ``reads_log`` -- one entry per read made during the WHOLE poll
                               (oldest first), each
                               ``{"truncated": bool, "n_names": int|None}``
                               (``n_names`` is None for an empty/timeout
                               read) -- so a bench run's `summary.json` can
                               show whether home-only reads are routinely
                               truncated. If they are, `never_raised_clean_tail`
                               (and therefore FAIL) would be unreachable in
                               practice, and that has to be visible rather
                               than silently inferred from `truncated_seen`
                               alone.

    Never raises; a keypad that never appears at all is still reported
    honestly via `raised_ever=False`."""
    if timeout_s is None:
        timeout_s = _PAGE_POLL_TIMEOUT_S
    if interval_s is None:
        interval_s = _PAGE_POLL_INTERVAL_S
    start = time.monotonic()
    reads: "list[tuple[Optional[set], bool]]" = []  # (names, truncated), oldest first
    empty_polls = 0
    truncated_seen = False
    raised_ever = False

    def _read() -> Optional[set]:
        nonlocal empty_polls, truncated_seen
        n, truncated = _lcd19_overlay_raw(ui)
        if truncated:
            truncated_seen = True
        if n is None:
            empty_polls += 1
        reads.append((n, truncated))
        return n

    def _satisfied() -> bool:
        nonlocal raised_ever
        real = [nm for nm, _ in reads if nm is not None]
        if real and _is_keypad_names(real[-1]):
            raised_ever = True
        if len(real) < 2 or real[-1] != real[-2]:
            return False
        # A stable repeat only ends the poll early when it is either the
        # keypad itself, or a post-raise close (raised_ever already True)
        # -- a stable repeat of `baseline` (or anything else) before any
        # raise has been observed must not stop the poll (see docstring
        # point 1 above).
        return _is_keypad_names(real[-1]) or raised_ever

    names = _read()
    while not _satisfied():
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        names = _read()

    real_all = [nm for nm, _ in reads if nm is not None]
    last_real = real_all[-1] if real_all else None
    if not raised_ever:
        raised_ever = any(_is_keypad_names(nm) for nm in real_all)
    raised_now = _is_keypad_names(last_real)
    closed_after_raise = raised_ever and not raised_now

    last_bad_idx = -1
    for i, (nm, trunc) in enumerate(reads):
        if nm is None or trunc:
            last_bad_idx = i
    tail_names = [nm for nm, _ in reads[last_bad_idx + 1:]]
    tail_window = tail_names[-_TAIL_STABLE_WINDOW:]
    never_raised_clean_tail = (
        not raised_ever
        and len(tail_window) >= _TAIL_STABLE_WINDOW
        and all(nm == tail_window[0] for nm in tail_window)
        and not _is_keypad_names(tail_window[0])
    )

    reads_log = [
        {"truncated": trunc, "n_names": (len(nm) if nm is not None else None)}
        for nm, trunc in reads
    ]

    return {
        "names": names,
        "last_real_names": last_real,
        "raised_ever": raised_ever,
        "raised_now": raised_now,
        "closed_after_raise": closed_after_raise,
        "never_raised_clean_tail": never_raised_clean_tail,
        "empty_polls": empty_polls,
        "truncated_seen": truncated_seen,
        "reads_log": reads_log,
    }


def _wait_stable_names(ui, timeout_s: Optional[float] = None,
                        interval_s: Optional[float] = None,
                        stable_reads: int = 2
                        ) -> "tuple[Optional[set], float, int, bool, bool]":
    """Poll :func:`_lcd19_overlay_raw` until the same real (non-empty)
    listing is read ``stable_reads`` times in a row, or `timeout_s`
    elapses. Unlike :func:`_wait_for_overlay_names`, this does not wait for
    a *specific* target state -- it debounces the click-then-read race for
    a submit whose expected good outcome is often "no visible change" (a
    wrong PIN resets digit entry but the keypad's own button set -- digits,
    "OK", "Cancel" -- never changes while it stays open, so there is no
    "OK"-appears/disappears signal to wait for the way there is for a page
    switch). An empty/timeout read (see :func:`_lcd19_overlay_raw`) never
    counts toward the streak -- it is neither a stable state nor a
    divergence, just a read to discard and retry. Reading the name set
    exactly once right after the click would trivially "confirm" the
    pre-submission state, which was the bug this replaces: two matching
    real reads spaced by `interval_s` at least rule out catching a
    genuinely in-flight LVGL transition. Never raises; a set that never
    stabilizes within `timeout_s` is still returned honestly (whatever the
    last read was, even a non-None one that never repeated) -- see the
    `stabilized` return value below for telling that case apart from a real
    stable read.

    Returns ``(names, elapsed_s, empty_polls, truncated_seen, stabilized)``.
    The first four are as :func:`_lcd19_poll_overlay`; `names` is still the
    LAST read even when it never stabilized (kept for diagnostics -- see
    `_entry_result_summary`'s docstring on root-causing an INCONCLUSIVE from
    `observed` alone). `stabilized` is True only when that last read was
    actually the `stable_reads`-th repeat in a row; a caller must gate any
    refused/started verdict on `stabilized`, not merely on `names is not
    None` -- a read that changed right up to the timeout (e.g. two
    different non-empty, still-transitioning sets) is exactly as
    inconclusive as an empty/timeout read, and treating it as a real
    answer produced a false FAIL (2026-09-30 bench review)."""
    stabilized_holder = [False]

    def _satisfied(n: Optional[set], history: "list[Optional[set]]") -> bool:
        if n is None:
            stabilized_holder[0] = False
            return False
        real = [x for x in history if x is not None]
        if len(real) < stable_reads:
            stabilized_holder[0] = False
            return False
        stable = all(x == real[-1] for x in real[-stable_reads:])
        stabilized_holder[0] = stable
        return stable

    names, elapsed_s, empty_polls, truncated_seen = _lcd19_poll_overlay(
        ui, _satisfied, timeout_s, interval_s)
    return names, elapsed_s, empty_polls, truncated_seen, stabilized_holder[0]


def _entry_result_summary(entry: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Redacted summary of an :meth:`UiTestClient.enter_pin` return value for
    inclusion in a case's `observed`: click *result codes* only (e.g.
    "ok"/"not_found"/"swallowed"), never the digit characters clicked, their
    order, or their `cx`/`cy` coordinates -- on this keypad's static grid
    layout, a coordinate directly identifies which digit button was pressed,
    so echoing `cx`/`cy` per click would reconstruct the PIN itself from
    "observed" even though the PIN string is never printed anywhere. This
    exists so a future INCONCLUSIVE (like the 2026-09-24 bench run that left
    `wrong_pin_refused`/`right_pin_started` at ``None`` with nothing in
    `observed` explaining why) can be root-caused from summary.json alone,
    without re-running against the board."""
    if not entry:
        return {"present": False}
    raw_digits = entry.get("digit_results") or []
    digit_results = [d.get("result") for d in raw_digits]
    # `elapsed_s` (2026-09-30, LCD-19 busy-timeout investigation) never
    # identifies which digit was pressed -- unlike cx/cy it carries no
    # positional information -- so it is safe to echo per click, and
    # distinguishes a genuine 300ms UI_WALK_WAIT_TIMEOUT_MS busy timeout
    # from a name that was truly never found (near-instant "not_found").
    digit_elapsed_s = [d.get("elapsed_s") for d in raw_digits]
    ok_result_entry = entry.get("ok_result") or {}
    ok_result = ok_result_entry.get("result")
    summary = {
        "present": True, "digit_count": len(digit_results),
        "digit_results": digit_results, "digit_elapsed_s": digit_elapsed_s,
        "ok_result": ok_result, "ok_elapsed_s": ok_result_entry.get("elapsed_s"),
    }
    # entry_incomplete()/dot_wait_s (2026-09-30, LCD-19, enter_pin_verified())
    # -- counts and timings only, never which digit stalled or the PIN
    # itself, same redaction discipline as the rest of this summary.
    if "entry_incomplete" in entry:
        summary["entry_incomplete"] = entry.get("entry_incomplete")
    if "expected_dot_count" in entry:
        summary["expected_dot_count"] = entry.get("expected_dot_count")
    if "observed_dot_count" in entry:
        summary["observed_dot_count"] = entry.get("observed_dot_count")
    if "dot_wait_s" in entry:
        summary["dot_wait_s"] = entry.get("dot_wait_s")
    return summary


#: The PIN keypad's digit buttonmatrix keys, by their literal text
#: (ui_lcd_keypad.c's s_bm_map) -- used to confirm a stable pre-entry read
#: actually shows the keypad's digits (not merely "OK"+"Cancel", which a
#: Confirm dialog also has) before typing anything into it.
_KEYPAD_DIGIT_NAMES = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"}


def _entry_all_digits_not_found(entry: Optional[Dict[str, Any]]) -> bool:
    """True only when every digit click AND the trailing "OK" click from
    :meth:`UiTestClient.enter_pin` reported "not_found" OR "walk_busy" -- the
    shape seen on 2026-09-30 (20260930T082643Z_lcd/082657Z_lcd) when every
    click landed inside the 300ms UI_WALK_WAIT_TIMEOUT_MS busy window.
    "walk_busy" is accepted here too (2026-09-30 follow-up): enter_pin()
    already retries a "not_found"/"walk_busy" click once, but a click that
    is STILL busy after that one retry is recorded in `digit_results`/
    `ok_result` as "walk_busy", not "not_found" -- the exact same
    exhausted-busy-window shape this marker exists to name, just reported
    under its own, more precise result string rather than folded into
    "not_found". Recorded as its own `observed` marker so this specific
    "every click missed" shape is distinguishable from a partial/mixed
    not_found without re-running against the board -- see
    :func:`_entry_result_summary`."""
    if not entry:
        return False
    digit_results = entry.get("digit_results") or []
    if not digit_results:
        return False
    if not all(d.get("result") in ("not_found", "walk_busy") for d in digit_results):
        return False
    ok_result = entry.get("ok_result") or {}
    return ok_result.get("result") in ("not_found", "walk_busy")


def _dots_have_cleared(names: Optional[set]) -> bool:
    """True only when `names` (a stabilized tap-target name set, as
    :func:`_wait_stable_names` returns) shows NO non-empty all-'*' name --
    i.e. the masked-PIN-dots label (borrowed into the walk by kiln_ui.c's
    generic grandchild-label path, see UiTestClient._dots_text's docstring)
    reads as 0 digits. Mirrors :meth:`UiTestClient._dots_text`'s own
    filtering (non-empty, every character '*') so an empty backdrop/msgbox
    name is never mistaken for a cleared dots count. `names is None` is
    never "cleared" -- an unreadable set proves nothing."""
    if names is None:
        return False
    return not any(n for n in names if n and set(n) == {"*"})


def _entry_all_clicked_ok(entry: Optional[Dict[str, Any]]) -> bool:
    """True only if every digit click_by_name AND the trailing "OK" click
    from :meth:`UiTestClient.enter_pin` reported ``result: "ok"``. A
    not_found/ambiguous/hidden click on any digit means the PIN typed on
    the board was NOT what the caller intended (or the keypad wasn't even
    open), so the caller must not draw any refused/accepted conclusion from
    the overlay state that follows -- this is what makes that case leave
    its boolean at ``None`` (INCONCLUSIVE) instead of trusting the read."""
    if not entry:
        return False
    digit_results = entry.get("digit_results") or []
    if not digit_results:
        return False
    if not all(d.get("result") == "ok" for d in digit_results):
        return False
    ok_result = entry.get("ok_result") or {}
    return ok_result.get("result") == "ok"


#: The PIN keypad's own "Cancel" footer button (ui_lcd_keypad.c) sits at
#: y=325 on this bench unit's panel -- off the visible glass -- so
#: click_by_name("Cancel") locates the target and reports "ok" (it only
#: checks whether a target by that name exists, not whether the resulting
#: tap landed anywhere real) without the keypad actually closing. Dismiss
#: the keypad instead via a raw touch_inject press+release on its backdrop:
#: the msgbox itself spans x 40..440 (ui_lcd_keypad.c) in 480x320 space, and
#: a point OUTSIDE that span hits the full-screen backdrop object underneath
#: it, which is wired to the same cancel handler as the (off-glass) Cancel
#: button itself (backdrop_click_cb(), ui_lcd_keypad.c:71-79). (20, 160) is
#: LEFT of the 400 px msgbox (x < 40) and lands on that full-screen backdrop
#: -- a tap anywhere INSIDE the x 40..440 span instead lands on the msgbox
#: itself (its digit grid or footer row, depending on y), not the backdrop.
#: A Confirm Start/Confirm Stop dialog's own
#: "Cancel" (ui_confirm.c) is on-glass, so that path still uses
#: click_by_name -- this workaround is keypad-specific.
_KEYPAD_BACKDROP_XY = (20, 160)


def _dismiss_lcd19_overlay(ctx: dict, ui) -> Dict[str, Any]:
    """Best-effort: dismiss a PIN keypad or Confirm Start/Confirm Stop dialog
    left open, never via the dialog's own confirm/OK/digit buttons. Returns
    a dict recording what was found and whether the dismiss actually took,
    for inclusion in a case's `observed`.

    `dismissed` is only ever True when BOTH the dismiss action itself
    reported success (the "Cancel" click result for a confirm dialog, or
    both touch_inject calls' own `ok` for the keypad backdrop tap) AND a
    subsequent listing comes back as a real (non-empty) set with no
    "Cancel" in it -- an empty/timeout read (see :func:`_lcd19_overlay_raw`)
    is never treated as "the popup is gone" the way `present=False`'s bare
    ``bool(n) == False`` used to read (every real screen here always has at
    least one tap target, so that never actually meant "gone" either)."""
    before = _lcd19_overlay_names(ui)
    if before is None:
        return {"checked": False}
    if "Cancel" not in before:
        return {"checked": True, "present": False}
    # "OK" in before is not by itself proof of the PIN keypad: the "Cannot
    # Start" dialog (ui_page_home_actions.c:115-123) and the on-glass OK
    # confirms on the profile-detail/review pages (ui_page_profile_detail.c:486,
    # ui_page_profile_builder_review.c:97,113) are also plain OK+Cancel
    # dialogs, and treating one of those as the keypad would route its
    # dismiss through the (wrong, off-glass) backdrop_touch_inject path
    # instead of the on-glass click_by_name("Cancel") those dialogs actually
    # need. The keypad's own buttonmatrix additionally always lists its
    # digit keys by their literal text ("0".."9", ui_lcd_keypad.c's
    # s_bm_map, collected verbatim by kiln_ui.c's buttonmatrix tap-target
    # walk) -- no other OK+Cancel dialog in this codebase has digit-named
    # targets, so requiring them narrows this to the keypad specifically.
    is_keypad = "OK" in before and {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"} <= before
    result: Dict[str, Any] = {"checked": True, "present": True, "is_keypad": is_keypad}
    action_ok = False
    if is_keypad:
        srv = _srv(ctx)
        touch = getattr(srv, "_touch", None)
        result["dismiss_method"] = "backdrop_touch_inject"
        if touch is None:
            result["error"] = "no touch client available"
        else:
            try:
                x, y = _KEYPAD_BACKDROP_XY
                press = touch.inject(x, y, True)
                release = touch.inject(x, y, False)
                result["press_ok"] = getattr(press, "ok", None)
                result["release_ok"] = getattr(release, "ok", None)
                action_ok = bool(getattr(press, "ok", False)) and bool(getattr(release, "ok", False))
            except Exception as exc:  # noqa: BLE001
                result["error"] = type(exc).__name__
    else:
        click = ui.click_by_name("Cancel")
        result["dismiss_method"] = "click_by_name_cancel"
        result["cancel_click_result"] = click.get("result")
        action_ok = click.get("result") == "ok"
    if not action_ok:
        result["dismissed"] = False
        return result
    after, _, empty_polls, truncated_seen = _lcd19_poll_overlay(
        ui, lambda n, h: n is not None and "Cancel" not in n)
    result["dismissed"] = after is not None and "Cancel" not in after
    if empty_polls:
        result["empty_polls"] = empty_polls
    if truncated_seen:
        result["truncated"] = truncated_seen
    return result


def _wait_for_home_settled(ctx: dict, ui, target_name: str, min_wait_s: float = 2.5,
                            timeout_s: float = 10.0, poll_interval_s: float = 0.5,
                            stable_reads: int = 2,
                            log: Optional[list] = None) -> bool:
    """Shared settle-wait for the keypad/confirm-popup self-close race
    (2026-09-30 review, scope-addition item): `set_policy(lcd_enabled=True)`
    (`cases_lcd.py`'s entry, before the Start click) and the allow_heat
    relock (before the Stop click) both make firmware set a pending
    force-lock flag that the 1Hz `tick_timer_cb` (`ui_lcd_lock.c:141-162,
    229-240`) consumes on its own cadence -- the tick's unlocked-to-locked
    edge closes any keypad/popup opened in the meantime, win-or-lose
    depending on exactly where in the current tick the `set_policy` POST
    landed. Wait at least 2.5s (more than two 1s ticks, `min_wait_s`) before
    even looking, then require `stable_reads` (default 2) consecutive polls
    that all show `target_name` present and `"Cancel"` absent (see the
    truncation rule below for how a truncated read is judged) -- before
    calling it genuinely settled. Never raises;
    returns False (caller stays INCONCLUSIVE, never taps `target_name`) if
    it never stabilizes within `timeout_s`.

    2026-09-30 bench fix (run 20260930T200715Z_lcd, LCD-19 allow_heat path):
    the Stop-side call of this helper runs while a firing is genuinely
    active, and the home page's own live content changes on its own cadence
    independent of the lock-race this helper watches for -- most likely the
    "Elapsed %s" progress label (`ui_page_home_refresh.c:163-172`), which is
    rebuilt every second the page refreshes while a profile runs, with the
    temperature readout (e.g. `"17C"` -> `"18C"`) as a secondary contributor.
    This is not confirmed from the one bench run that hit it -- that run
    predates the per-read `log` this fix adds (see below) and only recorded
    the final True/False, not the actual differing name sets -- but a future
    run's `allow_heat_settle_reads`/`start_settle_reads` log will show
    exactly which name(s) changed between reads. The original version
    required the FULL tap-target name set to read byte-identical across
    `stable_reads` consecutive polls, so on that path the qualifying
    condition (target present, Cancel absent) was true on every single poll
    but the changing label(s) reset `stable_count` back to 1 each time -- an
    unbroken string of qualifying reads was never allowed to reach 2 in a
    row, and the wait always ran out. Stability is now judged on the
    *relevant subset* -- target present, `"Cancel"` absent, plus the
    truncation rule below -- holding for `stable_reads` consecutive polls,
    not on whole-name-list identity; a live temperature or elapsed-time
    label ticking over no longer resets the counter. The Start-side call
    (page idle, nothing live changing names) behaves identically to before.

    Truncated reads (2026-09-30 review, advisory A; walk order updated
    2026-09-30 by the LCD-19 fix below): a truncated LIST_TAP_TARGETS reply
    is not rejected outright. `kiln_ui.c`'s `log_all_tap_targets()` (the walk
    both `kiln_ui_collect_tap_targets()` and this command's handler go
    through) documents its own emission order at kiln_ui.c:686-711 --
    "the overlay layers (a modal covers the page beneath it, so its targets
    matter most), then the screen's FLOATING direct children ..., then
    everything else in the screen's normal child order", with each of those
    three GROUPS now itself split into two passes: real, actionable buttons
    (lv_button_class, plus a msgbox's own lv_msgbox_footer_button_class/
    lv_msgbox_header_button_class buttons -- Cancel/Confirm/Yes/No are these,
    not lv_button_class, see kiln_ui.c's own comment at the CLICKABLE check --
    and every buttonmatrix key) first, then every other merely-CLICKABLE
    object (a decorative container borrowing a child label's text -- the
    WiFi status readout, a temperature/chart-legend label, a zone row --
    CLICKABLE only because that's an `lv_obj_create()` default in this
    codebase, never a real action) second. This is the LCD-19 bench fix
    (`logs/bench_test/20260930T203243Z_lcd/summary.json`): the home page's
    decorative labels sat ahead of action_row's real Start/Stop button in
    plain tree order and filled a truncated reply before the walk ever
    reached it, 20 of 20 `allow_heat_settle_reads` in that run. `Stop`/
    `Start` are real buttons, so the actionable-first split now protects
    them the same way -- and `uart_bridge_ui_test.c`'s
    `UI_TEST_CMD_LIST_TAP_TARGETS` handler (around line 111-135) still just
    emits entries in whatever order the firmware walk produces, stopping
    (and setting the truncated flag) once the 253 B wire reply is full, so
    truncation still always drops entries off the END of that order, never
    the start. GROUP order itself is unchanged: any modal's own "Cancel" is
    still always walked, and would be emitted, before the home page's own
    `target_name` button (a real button, so also actionable, but still in
    the normal/non-floating group, walked after the overlay and floating
    groups). If a truncated read still contains `target_name`, the walk
    necessarily got all the way past the overlay-layer group (both its
    actionable and non-actionable passes) without running out of room, so a
    "Cancel" from an open modal -- had one been open -- would already have
    been emitted too; its absence from a truncated-but-target-containing
    read is therefore real evidence, not an artifact of truncation. A
    truncated read that does NOT contain `target_name` stays non-qualifying
    (indeterminate: the cut could have landed before or after where an
    overlay's own targets would appear).

    `log`, when given a list, gets one dict appended per poll
    (`{"names": sorted(...) or None, "target_present", "cancel_present",
    "truncated", "qualifies"}`) so a caller can record the full settle
    read sequence into `state`/observed for diagnosis, rather than only the
    final True/False.

    `ctx["_now"]`/`ctx["_sleep"]` are injectable for tests, same pattern as
    `cases_heat._rest_gate`; default to real wall-clock time."""
    now = ctx.get("_now", time.monotonic)
    sleep = ctx.get("_sleep", time.sleep)
    sleep(min_wait_s)
    deadline = now() + timeout_s
    stable_count = 0
    while True:
        names: Optional[frozenset] = None
        truncated = False
        try:
            resp = ui.list_tap_targets()
            truncated = bool(resp.get("truncated"))
            targets = resp.get("targets") or []
            names = frozenset(t.get("name") for t in targets if not t.get("hidden"))
        except Exception:  # noqa: BLE001
            names = None
        target_present = names is not None and target_name in names
        cancel_present = names is not None and "Cancel" in names
        # A truncated read still qualifies as long as target_name made it
        # in -- see the truncation-rule docstring above: the walk order
        # guarantees an overlay's "Cancel" would have been emitted first,
        # so target_name present + Cancel absent is real evidence even
        # when truncated. A truncated read missing target_name never
        # qualifies (indeterminate).
        qualifies = target_present and not cancel_present
        if log is not None:
            log.append({
                "names": sorted(names) if names is not None else None,
                "target_present": target_present,
                "cancel_present": cancel_present,
                "truncated": truncated,
                "qualifies": qualifies,
            })
        if qualifies:
            stable_count += 1
            if stable_count >= stable_reads:
                return True
        else:
            stable_count = 0
        if now() >= deadline:
            return False
        sleep(poll_interval_s)


def _case_lcd19(ctx: dict) -> CaseResult:
    from . import cases_web_rw as _web  # local import: avoids a module-load cycle with cases_web_rw
    from . import cases_heat as _heat  # local import: same cycle-avoidance; reused only for the
    # allow_heat stop_gated sub-check below (_capability_preflight_ok,
    # _start_bench_profile, _cleanup_bench_profile, _read_energized) --
    # never reimplemented here.

    pin_seed_state: Optional[Dict[str, Any]] = None
    pin_cfg = ctx.get("_lcd_pin")
    if pin_cfg is None:
        # A standalone LCD-suite run never executes WEB-SEC-04 (a different
        # suite), so ctx["_lcd_pin"] is never populated that way here.
        # Seed it ourselves through the exact same shared path WEB-SEC-04
        # uses (cases_web_rw.seed_lcd_pin) -- this never calls set_lcd_pin
        # through any other path, and still refuses (NOT_RUN, naming the
        # env var) when KILNCTL_LCD_PIN is unset, same as before this case
        # could self-seed.
        try:
            seeded = _web.seed_lcd_pin(ctx)
        except _web.LcdPinSeedError as exc:
            verdict = Verdict.NOT_RUN if exc.kind == "missing" else Verdict.FAIL
            return CaseResult(verdict, reason=exc.reason, observed=exc.observed)
        pin_cfg = {"right_pin": seeded["right_pin"], "wrong_pin": seeded["wrong_pin"]}
        ctx["_lcd_pin"] = pin_cfg
        # `seeded["state"]` is already PIN-free (admin_pin_set_before /
        # set_lcd_pin_status or set_lcd_pin_skipped) -- surface it on
        # `observed` under its own key so a self-seeded run's PASS/FAIL/
        # INCONCLUSIVE result (and any early-abort path below) shows whether
        # a real write happened, without ever repeating the PIN itself.
        pin_seed_state = dict(seeded["state"])
    # 2026-09-24 (LCD-19 bench root cause): every other click-driven LCD
    # case wakes the panel and returns to `home` first (_wake_and_home's own
    # docstring above: the shortest display timeout is 1 minute, and the
    # gaps between LCD cases routinely exceed that). This case was the one
    # exception -- it went straight to `click_by_name("Start")`/("Stop")
    # with no wake/home step, so on a bench run where the panel had blanked
    # or drifted off `home` since the previous case, that first click
    # legitimately returned something other than "ok" (a blanked panel
    # swallows the tap; a stale page has no "Start"/"Stop" widget). The
    # click's own success is never in `observed`, but its absence is
    # visible indirectly: `after_start_click_names`/`after_stop_click_names`
    # never got set for three identical bench runs, meaning
    # `click.get("result") == "ok"` was false on the very first click, which
    # then left all four downstream booleans at None -- the "one or more
    # PIN-lock checks could not be exercised" INCONCLUSIVE observed on
    # 20260924T191429Z/203338Z/221915Z. Waking and homing first closes this
    # gap the same way it already does for every other case in this module.
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test

    client = _web._sec_client(ctx)
    status0, cfg0 = client.get_config()
    if status0 != 200 or cfg0 is None:
        aborted: Dict[str, Any] = {"status": status0}
        if pin_seed_state is not None:
            aborted["pin_seed"] = pin_seed_state
        return CaseResult(Verdict.FAIL, reason=f"GET /api/auth/config failed (status={status0})", observed=aborted)
    orig = _web._policy_from_config(cfg0)

    keypad_raised = wrong_pin_refused = right_pin_started = stop_gated = None
    result: Optional[CaseResult] = None
    # Owner decision 2026-09-30: stop_gated is only reachable at all when a
    # real firing is running (see the allow_heat block below) -- unlike the
    # config-only `firing_active_with_lock` flag this case has always
    # accepted (never set by anything in this tree), `started_firing_here`
    # tracks whether THIS run started that firing itself, so `finally` knows
    # whether it owns tearing it back down. Set True only once a real
    # `running` exec-status read confirms the start actually took.
    started_firing_here = False
    # Set True as soon as `_start_bench_profile` itself reports ok -- BEFORE
    # `started_firing_here`'s own bounded confirmation poll -- so `finally`
    # still tears down (and verifies) the hidden bench profile even in the
    # narrow window where the save/start succeeded but the poll below never
    # observed a `running` read in time (a real firing may still be running
    # on the board in that case; `started_firing_here` alone would miss it).
    bench_profile_started = False
    state: Dict[str, Any] = {"orig": orig}
    if pin_seed_state is not None:
        state["pin_seed"] = pin_seed_state
    try:
        # WEB-SEC-04 always restores `lcd_enabled` to whatever it found
        # (normally false on this bench) -- this case must turn it on
        # itself before the keypad can ever appear (module comment above).
        en_status, en_resp = client.set_policy(orig["web_enabled"], True, orig["web_timeout_min"], orig["lcd_timeout_min"])
        enable_ok = en_status == 200 and bool(en_resp) and en_resp.get("ok") is True
        state["enable_status"] = en_status
        if not enable_ok:
            result = CaseResult(
                Verdict.FAIL,
                reason="could not enable lcd_enabled before driving the PIN keypad -- set_policy did not confirm ok:true",
                observed=dict(state),
            )
        else:
            # kiln_ui_click_by_name() (kiln_ui.c) matches with an exact
            # strcmp, never case-insensitively, and the home fire button's
            # label text is exactly "Start" when idle/done/faulted or "Stop"
            # while RUNNING/PAUSED (ui_page_home.c / ui_page_home_refresh.c).
            firing_active = bool(pin_cfg.get("firing_active_with_lock"))
            # Both flags must be true (2026-09-30 review fix): ctx["allow_heat"]
            # defaults True (an ordinary run may exercise other, spec.heat
            # cases) and is NOT by itself a safe gate for starting an
            # unsolicited firing here; ctx["lcd19_allow_heat"] is the
            # separate, default-False opt-in specific to this sub-check.
            allow_heat = bool(ctx.get("allow_heat")) and bool(ctx.get("lcd19_allow_heat"))
            if not firing_active:
                # Scope-addition, 2026-09-30 review: the `set_policy`
                # enable above (line ~2866) trips the same tick_timer_cb
                # self-close race `_wait_for_home_settled` was built for
                # (see its docstring) -- clicking "Start" right after that
                # POST with no wait can race the tick that force-closes
                # whatever popup was open. Settle first.
                start_settle_log: list = []
                start_settled = _wait_for_home_settled(ctx, ui, "Start", log=start_settle_log)
                state["start_settle_ok"] = start_settled
                state["start_settle_reads"] = start_settle_log
                if not start_settled:
                    state["start_click_skip_reason"] = (
                        "home page never settled (stable 'Start' present / 'Cancel' absent read) "
                        "after enabling lcd_enabled -- Start was not tapped"
                    )
                    click = {"result": "skipped_unsettled"}
                else:
                    click = ui.click_by_name("Start")
                state["start_click_result"] = click.get("result")
                if click.get("result") == "ok":
                    raise_poll = _wait_for_keypad_raise(ui)
                    names = raise_poll["names"]
                    state["after_start_click_names"] = sorted(names) if names is not None else None
                    if raise_poll["empty_polls"]:
                        state["after_start_click_empty_polls"] = raise_poll["empty_polls"]
                    if raise_poll["truncated_seen"]:
                        state["after_start_click_truncated"] = raise_poll["truncated_seen"]
                    # Per-read truncated flag + name count for the WHOLE
                    # raise window, not merely whether one bad read happened
                    # ANYWHERE (`truncated_seen`) -- so a bench summary.json
                    # can show whether home-only reads are routinely
                    # truncated, which would otherwise make
                    # `never_raised_clean_tail` (and therefore FAIL)
                    # unreachable in practice without this being visible.
                    state["after_start_click_reads_log"] = raise_poll["reads_log"]
                    if raise_poll["closed_after_raise"]:
                        # The keypad WAS seen (raised_ever) but is gone again
                        # by the last read -- ui_lcd_lock.c can auto-dismiss
                        # it (inactivity timeout / unlocked->locked edge, see
                        # _wait_for_keypad_raise's docstring) with no PC-side
                        # action in between. This is evidence of a raise/close
                        # race, not evidence the keypad never raised --
                        # `keypad_raised` stays at its initial None
                        # (INCONCLUSIVE), never a fabricated False.
                        state["keypad_raised_then_closed"] = True
                        state["after_start_click_last_real_names"] = (
                            sorted(raise_poll["last_real_names"])
                            if raise_poll["last_real_names"] is not None else None
                        )
                    elif raise_poll["raised_ever"]:
                        keypad_raised = True
                    elif raise_poll["never_raised_clean_tail"]:
                        # Unambiguous negative evidence: the LAST 2 reads
                        # after the last empty/timeout/truncated read in the
                        # window (or the whole window if there was none) are
                        # identical, real, non-keypad reads -- e.g. the
                        # Start click was acknowledged but the tap-target
                        # set never left home. 2026-09-30 review (round 1):
                        # judging on the whole window (requiring zero empty/
                        # truncated reads anywhere) made a genuine
                        # never-raises regression read INCONCLUSIVE about as
                        # often as FAIL, since ordinary bench runs commonly
                        # see one empty/truncated read right after a click
                        # before the display catches up -- an empty/
                        # truncated read earlier in the window no longer
                        # prevents FAIL as long as the reads after it are
                        # clean. 2026-09-30 review (round 2): requiring the
                        # WHOLE tail to be identical was also wrong -- the
                        # home screen carries a live temperature label that
                        # can legitimately change mid-poll with no keypad
                        # ever appearing, so only the LAST 2 reads need to
                        # agree now, not every read since the last bad one.
                        keypad_raised = False
                    # Any other shape (raised_ever False with at least one
                    # empty/timeout or truncated read, or no real read at
                    # all) is genuinely ambiguous -- `keypad_raised` is left
                    # at its initial None (INCONCLUSIVE), not a fabricated
                    # False the same read failure would otherwise produce.
                if keypad_raised:
                    wrong_pin = pin_cfg.get("wrong_pin")
                    right_pin = pin_cfg.get("right_pin")
                    if wrong_pin:
                        # 2026-09-30 (20260930T082643Z_lcd/082657Z_lcd):
                        # `keypad_raised` only proves the keypad's own
                        # signature was read somewhere in the raise poll --
                        # it says nothing about whether lvgl_port_task has finished
                        # the raise transition. Both runs then saw EVERY
                        # wrong-PIN digit click and the trailing "OK" click
                        # come back "not_found" -- consistent with every
                        # click's 300ms UI_WALK_WAIT_TIMEOUT_MS window (see
                        # UiTestClient.click_by_name's docstring) expiring
                        # while the overlay-raise transition was still
                        # settling, not with the digits genuinely being
                        # absent. Debounce a stable, digit-bearing read
                        # before typing anything, the same way the
                        # post-submit polls already debounce below.
                        pre_names, _, pre_empty, pre_trunc, pre_stabilized = _wait_stable_names(
                            ui, timeout_s=_PIN_SUBMIT_POLL_TIMEOUT_S)
                        state["before_entry_stabilized"] = pre_stabilized
                        state["before_entry_names"] = sorted(pre_names) if pre_names is not None else None
                        if pre_empty:
                            state["before_entry_empty_polls"] = pre_empty
                        if pre_trunc:
                            state["before_entry_truncated"] = pre_trunc
                        digits_present = (
                            pre_stabilized and pre_names is not None and
                            _KEYPAD_DIGIT_NAMES <= pre_names and "OK" in pre_names
                        )
                        if not digits_present:
                            # 2026-09-30 (LCD-19 run-1 shape, 20260930T090130Z_lcd):
                            # the keypad was seen right after the Start click
                            # (`after_start_click_names`) but was already gone
                            # by this debounced pre-entry read -- it closed on
                            # its own before entry could start (a display
                            # timeout, most likely). Recorded explicitly so
                            # this shape is distinguishable from "the keypad
                            # never stabilized"/"a read failure" without
                            # re-running against the board; either way this
                            # case stays INCONCLUSIVE (wrong_pin left None).
                            if pre_names is not None and "Cancel" not in pre_names:
                                state["keypad_closed_before_entry"] = True
                            wrong_pin = None  # skip entry -- leave wrong_pin_refused at None (INCONCLUSIVE)
                    if wrong_pin:
                        # enter_pin_verified() (2026-09-30, LCD-19), not the
                        # older enter_pin(): confirms each digit's tap was
                        # actually APPLIED by bm_value_changed_cb() via the
                        # masked-PIN-dots read-only label before typing the
                        # next digit or pressing "OK" -- click_by_name()'s own
                        # "ok" only proves a press was injected, not that it
                        # landed (20260930T090222Z_lcd bench evidence: all 6
                        # digit clicks and the trailing OK click reported
                        # "ok", yet the keypad was still open with only 5
                        # dots afterward -- a click was silently lost).
                        entry = ui.enter_pin_verified(wrong_pin)
                        state["wrong_pin_entry"] = _entry_result_summary(entry)
                        if entry is not None and entry.get("entry_incomplete"):
                            # A digit's dot count never advanced -- entry was
                            # abandoned before "OK" was ever pressed (never
                            # re-tapped: a duplicate digit is worse than a
                            # missing one). Nothing downstream of a PIN that
                            # was never actually typed can be judged, so
                            # wrong_pin_refused stays at its initial None
                            # (INCONCLUSIVE) and the right-PIN entry below is
                            # skipped by wrong_pin_refused's own falsy value.
                            pass
                        elif entry is not None and _entry_all_digits_not_found(entry):
                            # Distinguish "every click missed" from a
                            # partial/mixed result so a future INCONCLUSIVE
                            # can be told apart without re-running against
                            # the board -- see module note above.
                            state["entry_all_not_found"] = True
                        if entry is not None and not entry.get("entry_incomplete") and _entry_all_clicked_ok(entry):
                            # A wrong PIN resets digit entry but the keypad's
                            # own button set never changes -- debounce two
                            # stable reads rather than trusting one
                            # immediate (tautologically "OK present") read.
                            names, _, empty_polls, truncated_seen, stabilized = _wait_stable_names(
                                ui, timeout_s=_PIN_SUBMIT_POLL_TIMEOUT_S)
                            state["after_wrong_pin_names"] = sorted(names) if names is not None else None
                            if empty_polls:
                                state["after_wrong_pin_empty_polls"] = empty_polls
                            if truncated_seen:
                                state["after_wrong_pin_truncated"] = truncated_seen
                            if not stabilized:
                                state["after_wrong_pin_stabilized"] = False
                            # A non-stabilized last read -- whether `None`
                            # (every read within the poll window came back
                            # empty/timeout) OR a real, non-empty read that
                            # was still changing at the timeout (e.g. two
                            # different non-empty sets, never repeating) --
                            # must leave `wrong_pin_refused` at its initial
                            # `None`, never a fabricated "not refused"
                            # False. Gating on `names is not None` alone
                            # (2026-09-30 fix) still missed the
                            # still-changing case; gating on `stabilized`
                            # (2026-09-30 review fix) covers both.
                            if stabilized:
                                if "OK" in names and "Cancel" in names:
                                    # "OK"+"Cancel" present alone doesn't
                                    # prove the wrong PIN was refused and
                                    # its entry reset -- the dots (masked
                                    # count) must have also cleared to 0.
                                    # If they haven't, this is genuinely
                                    # ambiguous (a stale/incompletely-reset
                                    # entry, or a dots-read glitch) and
                                    # wrong_pin_refused stays at its initial
                                    # None (INCONCLUSIVE) rather than a
                                    # fabricated True.
                                    if _dots_have_cleared(names):
                                        wrong_pin_refused = True
                                else:
                                    wrong_pin_refused = False
                        # else: leave wrong_pin_refused at None -- the PIN
                        # typed on the board wasn't actually the intended
                        # one, so no conclusion can be drawn from what
                        # follows.
                    if wrong_pin_refused and right_pin:
                        # No separate before-entry stabilization wait here:
                        # `wrong_pin_refused` being True already required a
                        # STABILIZED read (`after_wrong_pin_names`, via
                        # `_wait_stable_names` above) showing "OK"+"Cancel"
                        # -- the same keypad, still open, not re-raised --
                        # immediately before this entry starts.
                        entry = ui.enter_pin_verified(right_pin)
                        state["right_pin_entry"] = _entry_result_summary(entry)
                        if entry is not None and entry.get("entry_incomplete"):
                            # Same "never re-tap, never guess" rule as the
                            # wrong-PIN entry above: right_pin_started stays
                            # at its initial None (INCONCLUSIVE).
                            pass
                        elif entry is not None and _entry_all_digits_not_found(entry):
                            state["entry_all_not_found"] = True
                        if entry is not None and not entry.get("entry_incomplete") and _entry_all_clicked_ok(entry):
                            # A correct PIN closes the keypad in favour of
                            # the Confirm Start dialog -- "OK" disappears.
                            # 2026-09-30 bench evidence (two runs): the
                            # screen is still mid-transition after submit --
                            # ~14 non-repeating reads then an empty one at
                            # the default 2.0s timeout -- so this poll gets
                            # its own longer, named timeout rather than
                            # racing the transition.
                            names, _, empty_polls, truncated_seen, stabilized = _wait_stable_names(
                                ui, timeout_s=_PIN_SUBMIT_POLL_TIMEOUT_S)
                            state["after_right_pin_names"] = sorted(names) if names is not None else None
                            if empty_polls:
                                state["after_right_pin_empty_polls"] = empty_polls
                            if truncated_seen:
                                state["after_right_pin_truncated"] = truncated_seen
                            if not stabilized:
                                state["after_right_pin_stabilized"] = False
                            # A non-stabilized last read -- `None` (every
                            # read empty/timeout) OR a real, still-changing
                            # read that never repeated -- must leave
                            # `right_pin_started` at its initial `None`,
                            # never a fabricated "did not start" False. See
                            # the `wrong_pin_refused` comment above: gating
                            # on `names is not None` alone (2026-09-30 fix)
                            # still let a still-transitioning, never-
                            # repeating non-empty read at timeout produce a
                            # false FAIL ("the correct PIN did not start the
                            # firing"); gating on `stabilized` (2026-09-30
                            # review fix) covers that case too.
                            if stabilized:
                                right_pin_started = "OK" not in names and "Cancel" in names

                # Owner decision 2026-09-30: stop_gated has never been
                # reachable in practice -- `firing_active_with_lock` is not
                # set anywhere in this tree, so the Stop-tap branch below
                # never ran. `allow_heat` here requires BOTH ctx["allow_heat"]
                # (the suite-wide "heat-marked cases may run at all" flag,
                # defaults True) AND ctx["lcd19_allow_heat"] (a second,
                # independently-defaulted-False opt-in, `lcd_stop_heat` at
                # the runner/MCP/CLI layer -- 2026-09-30 review fix) to be
                # true, since LCD-19 is not itself `spec.heat`-marked and
                # `allow_heat` alone defaulting True would otherwise start an
                # unsolicited firing on an ordinary `bench_test_run(suite=
                # "lcd")` call. When true, this opts into making stop_gated
                # reachable via a real, short bench firing, the same hidden
                # BENCH_HP slot every HP-* case uses
                # (`cases_heat._start_bench_profile`, never reimplemented
                # here). This never touches `firing_active_with_lock` itself
                # -- that config key stays dead/unused -- it only starts a
                # real firing and flips the LOCAL `firing_active` so the
                # Stop-tap block below runs.
                if allow_heat:
                    # Advisory (2026-09-30 review): `_read_energized` (used
                    # by the teardown verification below) needs `ctx["host"]`
                    # to resolve anything -- without it, it can only ever
                    # return None, so the bench-firing cleanup could never be
                    # verified stopped. Refuse the start up front rather than
                    # starting a firing this case could never confirm it
                    # tore down.
                    if not ctx.get("host"):
                        state["allow_heat_start_reason"] = (
                            "ctx['host'] is not set -- refusing to start the allow_heat bench "
                            "firing, since _read_energized could never verify it stopped"
                        )
                        allow_heat = False
                if allow_heat:
                    # No _rest_gate() call here (contrast every HP-* case in
                    # cases_heat.py, which all call it before starting):
                    # this firing exists only to probe the LCD's PIN-gating
                    # UI behavior while *something* is running, never to
                    # measure a thermal response -- the ambient-rest
                    # precondition `_rest_gate` enforces (plan rule 7, zones
                    # within REST_BAND_C of ambient before a heat case may
                    # start) has no bearing on whether Stop raises a keypad,
                    # so it is deliberately skipped rather than an oversight.
                    #
                    # Fail-closed: any exception reading exec status counts
                    # as "not confirmed idle", same shape every other
                    # preflight probe in this codebase uses (see runner.py's
                    # `_safe_call`, not reused here to avoid a second import
                    # just for one call).
                    exec_idle = False
                    try:
                        exec_status = srv._profiles.get_exec_status()
                        exec_idle = exec_status.state_name in ("idle", "done", "faulted")
                        state["allow_heat_pre_exec_state"] = exec_status.state_name
                    except Exception as exc:  # noqa: BLE001
                        state["allow_heat_pre_exec_error"] = type(exc).__name__
                    if not exec_idle:
                        state["allow_heat_start_reason"] = (
                            "executor not idle before the allow_heat start attempt "
                            f"(state={state.get('allow_heat_pre_exec_state', 'unknown')})"
                        )
                    else:
                        # 2026-09-30 fix: the earlier right_pin_started
                        # sub-check (block A above) enters the right PIN and
                        # stops at the Confirm Start dialog -- it never
                        # presses Confirm Start (see the comment at
                        # "right_pin_started" is a placeholder name" above),
                        # but it also never dismisses that dialog itself, so
                        # a stale Confirm Start can still be sitting on the
                        # glass here. Starting the API firing underneath it
                        # would leave that dialog open on top of a genuinely
                        # running profile, and the settle-wait/Stop-tap logic
                        # below reads the home page's OWN "Stop"/"Cancel"
                        # tap targets, not this dialog's -- an unrelated
                        # leftover Confirm Start would be indistinguishable
                        # from a fresh one the firing start itself raised.
                        # Dismiss it via the on-glass "Cancel" (never
                        # Confirm Start/Confirm Stop, never a PIN digit --
                        # see _dismiss_lcd19_overlay()'s own header comment)
                        # before starting anything. Best-effort: a dismiss
                        # that fails or finds nothing to dismiss does not
                        # block the start attempt, it is only recorded for
                        # diagnosis.
                        state["allow_heat_pre_start_dismiss"] = _dismiss_lcd19_overlay(ctx, ui)
                        ok, reason, _ambient = _heat._start_bench_profile(ctx, zone_mask=0b001)
                        state["allow_heat_start_attempted"] = True
                        if not ok:
                            state["allow_heat_start_reason"] = reason
                        else:
                            bench_profile_started = True
                            # Bounded poll (~10s) for the executor to actually
                            # report `running` -- `profiles.start()` reporting
                            # `ok` is not itself proof the executor state
                            # machine has advanced yet.
                            deadline = time.monotonic() + 10.0
                            confirmed = False
                            while True:
                                try:
                                    st = srv._profiles.get_exec_status()
                                    if st.state_name == "running":
                                        confirmed = True
                                        break
                                except Exception:  # noqa: BLE001
                                    pass
                                if time.monotonic() >= deadline:
                                    break
                                time.sleep(0.5)
                            if confirmed:
                                started_firing_here = True
                                firing_active = True
                                state["allow_heat_started"] = True
                            else:
                                # _start_bench_profile reported ok but the
                                # executor never reached `running` within the
                                # poll window -- the profile it saved/started
                                # is still torn down by `finally` below via
                                # `_cleanup_bench_profile`, gated on
                                # `bench_profile_started` (not
                                # `started_firing_here`, which stays False
                                # here), so nothing is left dangling.
                                state["allow_heat_start_reason"] = (
                                    "profiles.start() reported ok but exec_status never read "
                                    "'running' within the poll window"
                                )
            if firing_active:
                relock_ok = True  # nothing to relock unless this run itself started the firing (below)
                if started_firing_here:
                    # The PIN-accepted Start above (block A) may have left
                    # the LCD session unlocked/granted (ui_lcd_lock.c's
                    # `gate_keypad_done_cb` calls `lcd_lock_grant`, and
                    # nothing re-locks it until inactivity timeout or another
                    # policy/credential change) -- but this run never
                    # actually reaches block A when `allow_heat` first tries
                    # a firing while idle (firing_active starts False, so
                    # block A always ran first). Force a relock the same way
                    # the case's own entry already does once (`set_policy`
                    # off->on flips `lcd_enabled`, which
                    # `web_auth_policy_check_transition`'s edge-triggered
                    # `clear_lcd` calls `ui_lcd_lock_force_lock()` for --
                    # security_backend_web_auth.c) so the Stop tap below is
                    # guaranteed to run against a locked session regardless
                    # of what block A left behind.
                    off_status, _off_resp = client.set_policy(
                        orig["web_enabled"], False, orig["web_timeout_min"], orig["lcd_timeout_min"])
                    on_status, on_resp = client.set_policy(
                        orig["web_enabled"], True, orig["web_timeout_min"], orig["lcd_timeout_min"])
                    relock_ok = (
                        off_status == 200 and on_status == 200
                        and bool(on_resp) and on_resp.get("ok") is True
                    )
                    state["allow_heat_relock_ok"] = relock_ok
                    if not relock_ok:
                        state["allow_heat_relock_status"] = (off_status, on_status)
                if not relock_ok:
                    # 2026-09-30 review fix: a relock that failed to confirm
                    # must NOT be treated as "the Stop tap still produces
                    # real evidence either way" -- the whole point of the
                    # relock is to guarantee the precondition (a genuinely
                    # locked session) a meaningful stop_gated read depends
                    # on. Tapping Stop here would read as stop_gated=False
                    # on nothing more than an unconfirmed lock state, which
                    # is not evidence Stop itself is ungated. Stay
                    # INCONCLUSIVE instead: `stop_gated` is left at its
                    # initial None, and Stop is never tapped.
                    state["allow_heat_stop_skip_reason"] = (
                        "lcd_enabled relock after the allow_heat firing did not confirm ok -- "
                        "Stop was not tapped, since a meaningful stop_gated read depends on a "
                        "confirmed-locked session"
                    )
                else:
                    settled = True
                    if started_firing_here:
                        # 2026-09-30 review fix: firmware's `tick_timer_cb`
                        # (1s period) detects the unlocked->locked edge the
                        # relock above just caused and force-closes any open
                        # keypad/confirm popup (`ui_lcd_keypad_force_close`/
                        # `ui_confirm_close_open`) -- tapping Stop
                        # immediately after the relock POST races that same
                        # tick and can observe a popup the tick is about to
                        # close out from under it. Wait at least two tick
                        # periods and poll until a STABLE read shows the
                        # home page genuinely settled (`"Stop"` present,
                        # `"Cancel"` absent) before tapping -- not merely
                        # non-empty, which the pre-relock home screen
                        # already was. This race only exists on the path
                        # that just forced a relock itself (the allow_heat
                        # firing this run started); the legacy
                        # `firing_active_with_lock` config-flag path never
                        # relocks here and never races this tick.
                        allow_heat_settle_log: list = []
                        settled = _wait_for_home_settled(ctx, ui, "Stop", log=allow_heat_settle_log)
                        state["allow_heat_settle_ok"] = settled
                        state["allow_heat_settle_reads"] = allow_heat_settle_log
                        if not settled:
                            state["allow_heat_stop_skip_reason"] = (
                                "home page never settled (stable 'Stop' present / 'Cancel' absent read) "
                                "after the allow_heat relock -- Stop was not tapped"
                            )
                    if settled:
                        baseline = _lcd19_overlay_names(ui)
                        stop_click = ui.click_by_name("Stop")  # widget reads "Stop" while firing
                        state["stop_click_result"] = stop_click.get("result")
                        if stop_click.get("result") == "ok":
                            names, _, empty_polls, truncated_seen = _wait_for_overlay_names(
                                ui, present=True, baseline=baseline)
                            state["after_stop_click_names"] = sorted(names) if names is not None else None
                            if empty_polls:
                                state["after_stop_click_empty_polls"] = empty_polls
                            if truncated_seen:
                                state["after_stop_click_truncated"] = truncated_seen
                            if names is not None:
                                has_cancel = "Cancel" in names
                                has_ok = "OK" in names
                                # Owner decision 2026-09-28 ("stop needs
                                # login. there is an estop button"): with the
                                # session locked, Stop must raise the PIN
                                # keypad first.
                                if has_cancel and has_ok:
                                    stop_gated = True  # PIN keypad appeared -- Stop is gated
                                elif has_cancel and not has_ok:
                                    stop_gated = False  # Confirm Stop shown directly, no PIN keypad
                                # else: neither popup present -- leave None (INCONCLUSIVE)
            result = J.judge_lcd_pin_lock(keypad_raised, wrong_pin_refused, right_pin_started, stop_gated)
            result.observed = dict(result.observed or {})
            result.observed.update(state)
    finally:
        if result is None:
            result = CaseResult(Verdict.FAIL, reason="LCD-19 aborted before a verdict was reached", observed=dict(state))
        result.observed = dict(result.observed or {})

        # Bench-firing teardown (allow_heat path only) -- its own try, run
        # BEFORE the overlay dismiss below: this stops a REAL firing over
        # the UART profiles path regardless of what happened on the LCD
        # (whether Stop was tapped, whether a popup ever appeared), and must
        # not be skipped just because the overlay dismiss below raises, nor
        # skip the overlay dismiss or policy restore itself if IT raises.
        if bench_profile_started:
            bench_cleanup: Dict[str, Any] = {}
            try:
                _heat._cleanup_bench_profile(ctx)
            except Exception as exc:  # noqa: BLE001
                bench_cleanup["cleanup_error"] = type(exc).__name__
            # Bounded verification: the executor must read a non-running
            # state AND the safety relay must read de-energized before this
            # case can call the fixture actually stopped -- never trust
            # `_cleanup_bench_profile`'s own best-effort, never-raises stop
            # call alone (see its docstring: it swallows every exception by
            # design).
            verified = False
            last_state = None
            last_energized: Optional[bool] = None
            deadline = time.monotonic() + 15.0
            while True:
                try:
                    st = srv._profiles.get_exec_status()
                    last_state = st.state_name
                except Exception:  # noqa: BLE001
                    last_state = None
                if last_state in ("idle", "done", "faulted"):
                    last_energized = _heat._read_energized(ctx)
                    if last_energized is False:
                        verified = True
                        break
                if time.monotonic() >= deadline:
                    break
                time.sleep(0.5)
            bench_cleanup["final_exec_state"] = last_state
            bench_cleanup["final_energized"] = last_energized
            bench_cleanup["verified"] = verified
            result.observed["bench_cleanup"] = bench_cleanup
            if not verified:
                # Hard FAIL, distinct from (and reported alongside) whatever
                # stop_gated verdict was reached above -- an unconfirmed
                # bench-firing stop is a safety-relevant harness failure,
                # not merely an inconclusive LCD reading. Named per the
                # task: the hardware E-stop is the operator's actual
                # backstop if the fixture is still heating.
                result.verdict = Verdict.FAIL
                result.reason = (
                    "allow_heat bench firing could not be confirmed stopped after LCD-19 "
                    f"(exec_state={last_state}, safety_relay_energized={last_energized}) -- "
                    "use the hardware E-stop if the fixture is still heating"
                )

        # The overlay dismiss goes over the UART UI_TEST link and can raise
        # (UiTestQueryError on a lost reply); the policy restore below is
        # HTTP and must run regardless, so never let the dismiss abort it.
        try:
            overlay = _dismiss_lcd19_overlay(ctx, ui)
        except Exception as exc:  # noqa: BLE001
            overlay = {"checked": False, "error": type(exc).__name__}
        restore_status, restore_resp = client.set_policy(
            orig["web_enabled"], orig["lcd_enabled"], orig["web_timeout_min"], orig["lcd_timeout_min"])
        restore_post_ok = restore_status == 200 and bool(restore_resp) and restore_resp.get("ok") is True
        readback_status, cfg_after = client.get_config()
        restore_matches = (
            readback_status == 200 and cfg_after is not None and
            _web._policy_from_config(cfg_after) == orig
        )
        restore_state = {
            "post_status": restore_status, "post_ok": restore_post_ok,
            "readback_status": readback_status, "readback_matches": restore_matches,
        }
        result.observed["overlay_dismiss"] = overlay
        result.observed["restore"] = restore_state
        if not restore_post_ok or not restore_matches:
            # Unconditional override, same as WEB-SEC-03/04's own finally:
            # a failed policy restore is a hard FAIL regardless of what the
            # keypad verdict above found -- the board may be left with
            # lcd_enabled changed from what this run found.
            #
            # 2026-09-30 review fix: append rather than clobber -- when the
            # bench-firing cleanup verification above already set an
            # E-stop-naming FAIL reason (result.verdict is already FAIL at
            # this point in that case), that message must survive: it is the
            # more safety-relevant of the two ("the fixture may still be
            # heating" beats "a config value didn't round-trip"), so it goes
            # first, with the restore failure appended rather than replacing
            # it outright.
            restore_reason = "lcd_enabled policy restore did not round-trip after LCD-19 -- board may be left with lcd_enabled changed"
            if result.verdict == Verdict.FAIL and result.reason:
                result.reason = f"{result.reason}; also: {restore_reason}"
            else:
                result.reason = restore_reason
            result.verdict = Verdict.FAIL
        nav = _navigate_home(ui)
        if not nav["ok"]:
            result.observed["navigate_home"] = nav
    return result


# ---------------------------------------------------------------------------
# LCD-22 -- Edit firing page (ui_page_edit_firing.c / ui_edit_firing_apply.c):
# live-edit a future segment of a RUNNING low-temperature firing from the
# panel, then prove the live working copy and executor took it. Heat case:
# runs only with allow_heat; the profile is started and stopped over the
# UART/HTTP APIs, never via the LCD Stop/Confirm path, and no PIN is ever
# typed (a PIN-locked panel is INCONCLUSIVE).
# ---------------------------------------------------------------------------

#: The Edit firing page's stepper and topbar glyph buttons carry NO tap name
#: (their labels are LV_SYMBOL glyphs, and click_by_name() answers ambiguous
#: or not_found for them), so they are tapped by panel coordinate. Measured
#: on the bench 2026-10-01 against build_step_row() (ui_page_edit_firing.c
#: lines 367-410, ROW_HEIGHT_PX 46; rows built at lines 445/447/449 in
#: Target, Ramp, Dwell order) and the topbar's Back/Home/Prev/Next slots
#: (ui_topbar.c). If the page layout moves these must be re-measured.
_LCD22_NEXT_XY = (453, 20)
_LCD22_TARGET_PLUS_XY = (443, 86)
_LCD22_DWELL_PLUS_XY = (443, 186)
#: Fallback only when click_by_name("Apply") finds no named target.
_LCD22_APPLY_XY = (239, 255)
#: Step sizes from ui_page_edit_firing.c: target +5 C, dwell +5 min.
_LCD22_TARGET_STEP_C = 5.0
_LCD22_DWELL_STEP_MIN = 5.0
#: Segment 0 is the running one (long dwell, so the firing stays on it for
#: the whole case); segment 1 is the future segment the case edits.
_LCD22_SEG0_OFFSET_C = 10.0
_LCD22_SEG1_OFFSET_C = 20.0
_LCD22_SEG0_DWELL_MIN = 10
_LCD22_SEG1_DWELL_MIN = 5
_LCD22_RAMP_C_PER_HR = 600.0
#: Highest target the case may ever command, after the +5 C edit.
_LCD22_MAX_TARGET_C = 60.0
_LCD22_ZONE_MASK = 1


def _lcd22_tap(ctx: dict, xy: "tuple[int, int]") -> bool:
    touch = getattr(_srv(ctx), "_touch", None)
    if touch is None:
        return False
    try:
        press = touch.inject(xy[0], xy[1], True)
        release = touch.inject(xy[0], xy[1], False)
    except Exception:  # noqa: BLE001
        return False
    ok = bool(getattr(press, "ok", False)) and bool(getattr(release, "ok", False))
    ctx.get("_sleep", time.sleep)(0.3)
    return ok


def _lcd22_exec_dict(srv) -> "Optional[dict]":
    try:
        st = srv._profiles.get_exec_status()
    except Exception:  # noqa: BLE001
        return None
    return {
        "state_name": getattr(st, "state_name", None),
        "profile_id": getattr(st, "profile_id", None),
        "segment_count": getattr(st, "segment_count", None),
        "segment_index": getattr(st, "segment_index", None),
    }


def _lcd22_read_live(client, host: str) -> "tuple[Optional[dict], Optional[dict]]":
    """(status, content) -- either may be None when the read raised (a 409 on
    content just means no working copy exists)."""
    try:
        status = client.get_live_status(host)
    except Exception:  # noqa: BLE001
        status = None
    try:
        content = client.get_live_content(host)
    except Exception:  # noqa: BLE001
        content = None
    return status, content


def _lcd22_zone_relays(ctx: dict, heat) -> "Optional[dict]":
    """{zone: relay_on} from /api/profile_exec; {} when the read succeeded and
    lists no zones; None when it cannot be read or a listed zone lacks a
    boolean relay_on -- never a guess.

    /api/profile_exec only lists ACTIVE zones (dashboard_json.c skips
    inactive ones) and a stopped executor clears every zone's active flag, so
    after a stop an empty list is the normal, healthy shape. It means "all
    zone relays off" only together with an idle/done/faulted executor (Halt
    forces every relay off before IDLE); the caller's verify loop already
    requires that state before it consults this."""
    host = ctx.get("host")
    if not host:
        return None
    get_json = ctx.get("_http_get_json", heat._http_get_json)
    try:
        status, body = get_json(host, "/api/profile_exec")
    except Exception:  # noqa: BLE001
        return None
    if status != 200 or not isinstance(body, dict) or not isinstance(body.get("zones"), list):
        return None
    relays = {}
    for i, z in enumerate(body["zones"]):
        if not isinstance(z, dict) or not isinstance(z.get("relay_on"), bool):
            return None
        relays[z.get("zone", z.get("index", i))] = z["relay_on"]
    return relays


def _lcd22_cleanup(ctx: dict, client, host: str, heat, own_working_id: "Optional[int]" = None
                   ) -> "tuple[bool, dict]":
    """Stop the profile via the API, discard ONLY the live working copy this run
    created (`own_working_id`, read right after Apply), then verify (bounded)
    that the executor is idle/done/faulted, no working copy remains and every
    relay (safety relay and each zone relay) reads de-energized. A working copy
    that is not provably ours, or a status that cannot be read, is never
    discarded: it is reported and the run cannot verify. Returns
    (verified, details)."""
    srv = _srv(ctx)
    now = ctx.get("_now", time.monotonic)
    sleep = ctx.get("_sleep", time.sleep)
    details: Dict[str, Any] = {}
    try:
        heat._cleanup_bench_profile(ctx)
    except Exception as exc:  # noqa: BLE001
        details["cleanup_error"] = type(exc).__name__
    details["own_working_id"] = own_working_id
    try:
        pre_status = client.get_live_status(host)
        pre_wid = int(pre_status.get("working_id", -1))
    except Exception:  # noqa: BLE001
        pre_wid = None
    if pre_wid is None:
        details["discard"] = "skipped:status_unreadable"
    elif pre_wid < 0:
        details["discard"] = "none_pending"
    elif own_working_id is None or pre_wid != own_working_id:
        details["discard"] = f"skipped:working_copy_not_ours(working_id={pre_wid})"
        details["foreign_working_copy"] = pre_wid
    else:
        try:
            client.decide_live_discard(host)
            details["discard"] = "ok"
        except Exception as exc:  # noqa: BLE001
            details["discard"] = f"{type(exc).__name__}:{getattr(exc, 'status', None)}"
    verified = False
    last_zone_relays: "Optional[dict]" = None
    last_state = None
    last_energized: Optional[bool] = None
    working_id: Optional[int] = None
    deadline = now() + 15.0
    while True:
        ex = _lcd22_exec_dict(srv)
        last_state = ex["state_name"] if ex else None
        try:
            working_id = int(client.get_live_status(host).get("working_id", -1))
        except Exception:  # noqa: BLE001
            working_id = None
        if last_state in ("idle", "done", "faulted") and working_id is not None and working_id < 0:
            last_energized = heat._read_energized(ctx)
            last_zone_relays = _lcd22_zone_relays(ctx, heat)
            if last_energized is False and last_zone_relays is not None \
                    and not any(last_zone_relays.values()):
                verified = True
                break
        if now() >= deadline:
            break
        sleep(0.5)
    details.update({"final_exec_state": last_state, "final_working_id": working_id,
                    "final_energized": last_energized, "final_zone_relays": last_zone_relays,
                    "verified": verified})
    return verified, details


#: Bounded wait for the home page's Edit button to appear after the firing
#: reaches running (see the settle call in _case_lcd22).
_LCD22_EDIT_WAIT_S = 10.0


def _case_lcd22(ctx: dict) -> CaseResult:
    if ctx.get("allow_heat") is not True:
        return CaseResult(Verdict.NOT_RUN, reason="allow_heat=False: LCD-22 starts a real low-temperature firing")
    # Second, default-False opt-in (mirrors LCD-19's lcd_stop_heat):
    # ctx["allow_heat"] defaults True, so it alone must not start a firing.
    if ctx.get("lcd22_allow_heat") is not True:
        return CaseResult(
            Verdict.NOT_RUN,
            reason="lcd_edit_heat=False: LCD-22 starts a real low-temperature firing "
                   "and needs the separate lcd_edit_heat=True opt-in")
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx (needed for profile_live and relay reads)")
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    client = ctx.get("_profile_live_client")
    if client is None:
        from .. import profile_live_http_client as client
    srv = _srv(ctx)

    # Pre-checks: nothing is started or tapped unless all of these hold.
    # An operator's own pending live edit must never be clobbered: the case
    # saves slot 7 and later discards a working copy, so refuse up front when
    # one already exists (or the status cannot be read).
    try:
        live_before = client.get_live_status(host)
        live_wid = int(live_before.get("working_id", -1))
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"could not read /api/profile/live before LCD-22 ({type(exc).__name__}); no action taken"))
    if live_wid >= 0 or live_before.get("active") or live_before.get("pending_decision"):
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            "a live profile edit or decision is already pending "
            f"(working_id={live_wid}); LCD-22 will not touch it; no action taken"),
            observed={"live_status": live_before})
    refusal_before = live_before.get("last_refusal")
    before = _lcd22_exec_dict(srv)
    if before is None or before["state_name"] not in ("idle", "done", "faulted"):
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"executor is not idle before LCD-22 (state={before['state_name'] if before else 'unreadable'}); "
            "no action taken"), observed={"exec": before})
    ok, why = _heat._capability_preflight_ok(ctx)
    if not ok:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{why}; no action taken")
    temps = _heat._zone_temps(ctx)
    if not temps:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no valid thermo reading to use as an ambient reference; no action taken")
    ambient = min(temps.values())
    seg1_target = ambient + _LCD22_SEG1_OFFSET_C
    if seg1_target + _LCD22_TARGET_STEP_C > _LCD22_MAX_TARGET_C:
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"ambient {ambient:.1f} C leaves no room under the {_LCD22_MAX_TARGET_C:.0f} C case ceiling; no action taken"))
    seg0_target = ambient + _LCD22_SEG0_OFFSET_C
    ceiling_ok, ceiling_reason = _heat._check_zone_ceilings(ctx, _LCD22_ZONE_MASK, seg1_target + _LCD22_TARGET_STEP_C)
    if not ceiling_ok:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{ceiling_reason}; no action taken")

    from .. import devices
    segments = [
        devices.ProfileSegment(target_c=seg0_target, ramp_c_per_hr=_LCD22_RAMP_C_PER_HR, dwell_min=_LCD22_SEG0_DWELL_MIN),
        devices.ProfileSegment(target_c=seg1_target, ramp_c_per_hr=_LCD22_RAMP_C_PER_HR, dwell_min=_LCD22_SEG1_DWELL_MIN),
    ]
    orig = {"seg0_target_c": seg0_target, "seg0_dwell_min": float(_LCD22_SEG0_DWELL_MIN),
            "seg1_target_c": seg1_target, "seg1_dwell_min": float(_LCD22_SEG1_DWELL_MIN)}
    expected = dict(orig)
    expected["seg1_target_c"] = seg1_target + _LCD22_TARGET_STEP_C
    expected["seg1_dwell_min"] = float(_LCD22_SEG1_DWELL_MIN) + _LCD22_DWELL_STEP_MIN

    result: Optional[CaseResult] = None
    started = False
    own_working_id: Optional[int] = None
    observed: Dict[str, Any] = {"ambient_c": ambient, "orig": orig, "expected": expected}
    ui = srv._ui_test
    now = ctx.get("_now", time.monotonic)
    sleep = ctx.get("_sleep", time.sleep)
    try:
        try:
            save = srv._profiles.save(_heat.BENCH_PROFILE_SLOT_ID, _heat.BENCH_PROFILE_NAME, _LCD22_ZONE_MASK, segments)
        except Exception as exc:  # noqa: BLE001
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.save raised {type(exc).__name__}: {exc}")
            return result
        started = True  # the slot may exist from here on: the finally tears it down
        if not save.ok:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.save refused: {save.error}")
            return result
        try:
            start = srv._profiles.start(_heat.BENCH_PROFILE_SLOT_ID)
        except Exception as exc:  # noqa: BLE001
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.start raised {type(exc).__name__}: {exc}")
            return result
        if not start.ok:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.start refused: {start.error}")
            return result

        deadline = now() + 10.0
        exec_before = _lcd22_exec_dict(srv)
        while not exec_before or exec_before["state_name"] != "running":
            if now() >= deadline:
                break
            sleep(0.5)
            exec_before = _lcd22_exec_dict(srv)
        observed["exec_before"] = exec_before
        if not exec_before or exec_before["state_name"] != "running":
            result = CaseResult(Verdict.INCONCLUSIVE, reason="firing never reached running; nothing was edited", observed=observed)
            return result

        _wake_and_home(ctx)
        # 2026-10-01 (first board run, 20261001T172420Z_lcd_lcd22run): the
        # Edit click returned 'not_found' 2.69 s in. The Edit button is built
        # hidden and only un-hidden by the home page's periodic refresh
        # (ui_page_home_refresh.c) once it sees RUNNING/PAUSED, and
        # list_tap_targets skips hidden widgets (click_by_name reports them
        # as "hidden" since 65108980's follow-up, not "not_found") -- so a click
        # right after the start can precede that refresh. Wait (bounded) for
        # a visible "Edit" with no keypad/popup open, the same settle LCD-19
        # uses before its Start/Stop click, BEFORE the single click below.
        # This only polls the read-only target listing; it never taps.
        edit_settle_log: list = []
        edit_ready = _wait_for_home_settled(
            ctx, ui, "Edit", min_wait_s=0.0, timeout_s=_LCD22_EDIT_WAIT_S, log=edit_settle_log)
        observed["edit_settle_reads"] = edit_settle_log[-5:]
        if not edit_ready:
            last = edit_settle_log[-1] if edit_settle_log else {}
            try:
                page_now = ui.get_current_page()
            except Exception:  # noqa: BLE001
                page_now = None
            result = CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
                f"'Edit' never became a visible, unobstructed tap target within {_LCD22_EDIT_WAIT_S:.0f} s "
                f"of the firing reaching running (page={page_now!r}, last names={last.get('names')!r}); "
                "no tap was sent"))
            return result
        # Edit is clicked exactly ONCE (swallow retries only: a swallowed tap
        # sent no press). It must NOT go through _click_then_page: on a
        # PIN-locked panel Edit raises the keypad while the page still reads
        # "home", and a page-poll retry would re-tap Edit's coordinates onto
        # the keypad (digit keys / Cancel). The keypad is checked before the
        # page on every poll.
        edit_click, _sw, _nf, _wb = _click_resolving_swallow(
            ui, "Edit", max_not_found_retries=0, max_walk_busy_retries=0)
        observed["edit_click"] = edit_click.get("result")
        if edit_click.get("result") not in ("ok", "verdict_unknown"):
            result = CaseResult(Verdict.FAIL, observed=observed, reason=(
                f"click_by_name('Edit') returned {edit_click.get('result')!r} while the firing was running"))
            return result
        page_deadline = now() + _PAGE_POLL_TIMEOUT_S
        while True:
            names = _lcd19_overlay_names(ui)
            if _is_keypad_names(names):
                observed["overlay_dismiss"] = _dismiss_lcd19_overlay(ctx, ui)
                result = CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
                    "Edit raised the PIN keypad (panel is locked); LCD-22 never types a PIN"))
                return result
            if ui.get_current_page() == "edit_firing":
                break
            if now() >= page_deadline:
                result = CaseResult(Verdict.FAIL, observed=observed, reason=(
                    f"Edit was clicked but the page is {ui.get_current_page()!r}, not 'edit_firing'"))
                return result
            sleep(_PAGE_POLL_INTERVAL_S)
        tap = _list_tap_targets_resolving_busy(ui)[0]
        _remember_page_targets(ctx, "edit_firing", tap)

        # The page opens on the running segment; Next reaches the future one.
        taps = {
            "next": _lcd22_tap(ctx, _LCD22_NEXT_XY),
            "target_plus": _lcd22_tap(ctx, _LCD22_TARGET_PLUS_XY),
            "dwell_plus": _lcd22_tap(ctx, _LCD22_DWELL_PLUS_XY),
        }
        observed["taps"] = taps
        if not all(taps.values()):
            result = CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                                reason="touch_inject failed for a stepper tap; Apply was not pressed")
            return result
        apply = ui.click_by_name("Apply")
        observed["apply_click"] = apply.get("result")
        if apply.get("result") == "not_found":
            observed["apply_by_coordinate"] = _lcd22_tap(ctx, _LCD22_APPLY_XY)
        elif apply.get("result") not in ("ok", "verdict_unknown"):
            result = CaseResult(Verdict.FAIL, observed=observed,
                                reason=f"click_by_name('Apply') returned {apply.get('result')!r}")
            return result

        status = content = None
        deadline = now() + 10.0
        while True:
            status, content = _lcd22_read_live(client, host)
            if own_working_id is None and status is not None:
                try:
                    wid = int(status.get("working_id", -1))
                except (TypeError, ValueError):
                    wid = -1
                if wid >= 0:
                    own_working_id = wid
            segs = (content or {}).get("segments") or []
            try:
                landed = len(segs) >= 2 and abs(float(segs[1].get("target_c", -1)) - expected["seg1_target_c"]) <= J.LCD_EDIT_FIRING_TOL
            except (TypeError, ValueError):
                landed = False
            if landed or now() >= deadline:
                break
            sleep(0.5)
        exec_after = _lcd22_exec_dict(srv)
        result = J.judge_lcd_edit_firing(orig, expected, status, content, exec_before, exec_after, tap,
                                      refusal_before=refusal_before)
        result.observed = dict(result.observed or {})
        result.observed.update(observed)
        return result
    except Exception as exc:  # noqa: BLE001
        result = CaseResult(Verdict.FAIL, reason=f"LCD-22 aborted by {type(exc).__name__}: {exc}", observed=dict(observed))
        return result
    finally:
        if started:
            try:
                verified, details = _lcd22_cleanup(ctx, client, host, _heat, own_working_id)
            except Exception as exc:  # noqa: BLE001
                verified, details = False, {"cleanup_error": type(exc).__name__}
            try:
                _navigate_home(ui)
            except Exception:  # noqa: BLE001
                pass
            if result is not None:
                result.observed = dict(result.observed or {})
                result.observed["cleanup"] = details
                if not verified:
                    # Unconditional: a cleanup that cannot be confirmed is a
                    # FAIL whatever the LCD checks found, and never a PASS.
                    prior = result.reason if result.verdict != Verdict.PASS else ""
                    msg = (
                        "LCD-22 cleanup could not be verified "
                        f"(exec_state={details.get('final_exec_state')}, working_id={details.get('final_working_id')}, "
                        f"safety_relay_energized={details.get('final_energized')}, "
                        f"zone_relays={details.get('final_zone_relays')}, discard={details.get('discard')}) -- "
                        "use the hardware E-stop if the fixture is still heating"
                    )
                    result.reason = f"{prior}; also: {msg}" if prior else msg
                    result.verdict = Verdict.FAIL


# ---------------------------------------------------------------------------
# LCD-23 / LCD-24 -- more of the Edit firing page (see LCD-22 above for the
# shared geometry, cleanup and gating rationale). Both are heat cases with the
# same safety structure as LCD-22 (allow_heat AND lcd_edit_heat, a pending-
# live-edit pre-check, one Edit click, no retried taps, a keypad check after
# every tap batch, and a finally that stops the profile, verifies every relay
# off and discards only this run's own working copy).
#
#   LCD-23: ramp-rate/target/dwell minus+plus steppers on a FUTURE segment,
#           then the refusal paths: a stale Apply of a segment that has since
#           finished (the LCD's own window check must refuse it and adopt
#           nothing) and the HTTP 409 (window) / 400 (bound) answers.
#   LCD-24: a firing with an adopted edit ends on its own; the open Edit page
#           must show the ended state and the live status must report a
#           pending decision. The LCD now has a Keep Edit? decide page (Discard /
#           Save as / Overwrite), but this case still makes the decision over
#           HTTP (an HTTP Discard); the LCD page is exercised separately.
#
# The Edit page's status line ("Refused: ...", "Applied.", "Firing ended.") is
# a plain lv_label with no tap name, so UI_TEST cannot read it; every check
# below is on an observable effect (HTTP live status/content, executor state,
# which tap targets exist) instead of the text.
# ---------------------------------------------------------------------------

#: Row centre y of the three stepper rows (LCD-22 measured Target ~86 and
#: Dwell ~186; Ramp sits between them) and the band that still counts as the
#: same row. Each row is a CLICKABLE flex container (ui_page_edit_firing.c's
#: build_step_row(): caption, minus, value, plus) that kiln_ui.c's tap walk
#: also lists, in its non-actionable pass, named after the caption label
#: ("Target"/"Ramp"/"Dwell") and centred mid-row. The steppers are the
#: glyph-named targets in the band (_is_glyph_name), minus before plus by x;
#: a disabled stepper is not clickable and so is simply absent. There is NO x
#: cut: minus sits right after the caption (cx ~70-110) on the real panel.
_LCD_EDIT_ROW_Y = {"target": 86, "ramp": 136, "dwell": 186}
_LCD_EDIT_ROW_BAND_PX = 14
_LCD_EDIT_ZONE_MASK = 1
#: Segment durations. Segment 0 dwells one whole minute (dwell_min is an
#: integer) so the firing sits in it long enough to edit a later segment.
#:
#: WARM START: the executor skips, at firing start, every segment whose target
#: is <= the zone's current temperature (profile_executor_run.c ~970 applies
#: profile_executor_plan_warm_start(), profile_executor_start.c:371-479). A
#: segment-0 target at floor(zone temp) is therefore almost always skipped
#: (first hardware run: zone 30.6-30.8 C, base 30, entered index 1). The
#: profiles below start strictly ABOVE the reading (_lcd_edit_base) so the
#: firing enters segment 0 deterministically.
_LCD_EDIT_SEG0_DWELL_MIN = 1
#: Margin above the zone reading for segment 0's target (a reading can drift
#: up a fraction of a degree between the plan and the executor's own read).
_LCD_EDIT_BASE_MARGIN_C = 2.0
#: Slack added to a computed segment duration when waiting for it, and the cap
#: on any single computed wait.
_LCD_EDIT_WAIT_MARGIN_S = 30.0
_LCD_EDIT_WAIT_CAP_S = 420.0
_LCD_EDIT_RAMP_C_PER_HR = 600.0
_LCD_EDIT_MAX_TARGET_C = 60.0
_LCD_EDIT_MIN_RAMP_C_PER_HR = 10.0
#: LCD-23 profile: [0] base/1 min, [1] base+10/1 min, [2] base+20/30 min (the
#: segment the steppers edit), [3] base+25/5 min (the last, untouched except by
#: the refused HTTP probes). The Edit page opens on the RUNNING segment
#: (ui_page_edit_firing.c:214, cur_seg = running_seg), so the case reads the
#: running segment and navigates by topbar state; it never assumes a segment.
#: The first hardware run opened on segment_index 1 because the warm start
#: skipped segment 0 (its target was at floor(zone temp), below the reading);
#: every target now sits above the reading (see the warm-start note above), so
#: the firing enters segment 0 and the running segment at Edit time is 0
#: unless a slow bench step let segment 0's dwell run out (then 1). Running
#: segment 0 or 1 leaves segment 2 as a future edit segment; running segment
#: >= 2 leaves none and the case is INCONCLUSIVE with nothing edited.
_LCD23_SEG1_OFFSET_C = 10.0
_LCD23_SEG2_OFFSET_C = 20.0
_LCD23_SEG3_OFFSET_C = 25.0
_LCD23_SEG1_DWELL_MIN = 1
_LCD23_SEG2_DWELL_MIN = 30
_LCD23_SEG3_DWELL_MIN = 5
_LCD23_EDIT_SEG = 2
#: LCD-24 profile: [0] base/2 min, [1] base/2 min, [2] base/0. All at one
#: target above the zone reading (warm start), dwells long enough that the
#: segment after the one running at Edit time is still future when Apply is
#: tapped, and the whole firing ends by itself within a few minutes.
_LCD24_SEG_DWELL_MIN = 2
#: Bounded waits. The segment-advance (LCD-23) and firing-end (LCD-24) waits
#: are computed from the plan's ramp/dwell (_lcd_edit_seg_duration_s) plus
#: _LCD_EDIT_WAIT_MARGIN_S, capped at _LCD_EDIT_WAIT_CAP_S.
_LCD_EDIT_ENDED_WAIT_S = 4.0
#: ui_page_home_internal.h:79 UI_PAGE_HOME_REFRESH_MS.
UI_HOME_REFRESH_MS = 1000
_LCD_EDIT_BOUND_TARGET_C = 5000.0
_LCD_EDIT_CEILING_PROBE_C = 10.0
_LCD_EDIT_MAX_TARGET_FIELD_C = 2015.0


def _lcd_edit_base(zone_temp: float) -> float:
    """Segment-0 target: strictly above the zone reading so the executor's
    warm start does not skip it."""
    return float(math.ceil(zone_temp)) + _LCD_EDIT_BASE_MARGIN_C


def _lcd_edit_seg_duration_s(prev_target: float, seg: dict) -> float:
    """Nominal seconds a segment takes: the ramp from the previous target at
    the segment's own rate plus its dwell. The executor's dwell credit can only
    shorten it, so it is an upper bound on the nominal time."""
    ramp = float(seg["ramp_c_per_hr"])
    ramp_s = abs(float(seg["target_c"]) - float(prev_target)) / ramp * 3600.0 if ramp > 0 else 0.0
    return ramp_s + float(seg["dwell_min"]) * 60.0


def _lcd_edit_wait_s(seconds: float) -> float:
    return min(float(seconds) + _LCD_EDIT_WAIT_MARGIN_S, _LCD_EDIT_WAIT_CAP_S)


def _lcd_edit_names(tap: dict) -> set:
    return {t.get("name") for t in (tap or {}).get("targets", []) if t.get("name")}


def _lcd_edit_page_state(ui) -> dict:
    """Read the Edit page's tap targets once and locate, by position only:
    each field's (minus_xy, plus_xy) (None when either stepper is absent, i.e.
    disabled), the Next/Prev topbar xy (None when disabled), and whether
    Apply and a keypad are present. {"readable": False} when the walk is
    busy/empty."""
    tap = _list_tap_targets_resolving_busy(ui)[0]
    targets = [t for t in (tap or {}).get("targets", []) if not t.get("hidden")]
    names = {t.get("name") for t in targets if t.get("name")}
    if (tap or {}).get("busy") or not targets:
        return {"readable": False, "keypad": False}
    if (tap or {}).get("truncated"):
        # A truncated walk can omit the Prev/Next glyphs, which would read as
        # "icon disabled"; never treat a partial list as a verified page.
        return {"readable": False, "keypad": False, "truncated": True}
    state: Dict[str, Any] = {"readable": True, "keypad": _is_keypad_names(names), "apply": "Apply" in names,
                             "steppers": {}, "names": sorted(names)}
    for field, row_y in _LCD_EDIT_ROW_Y.items():
        row = sorted((t for t in targets if abs(t.get("cy", -999) - row_y) <= _LCD_EDIT_ROW_BAND_PX
                      and _is_glyph_name(t.get("name"))), key=lambda t: t["cx"])
        state["steppers"][field] = ((row[0]["cx"], row[0]["cy"]), (row[-1]["cx"], row[-1]["cy"])) \
            if len(row) == 2 else None
    # Topbar: Back, Home, Prev, Next in one flex row; a disabled Prev/Next is
    # dimmed, not clickable, so absent from the listing, but keeps its layout
    # slot (same geometry as _profiles_topbar_icons). Slots are therefore
    # home.cx + k * (home.cx - back.cx): k=1 Prev, k=2 Next; a missing glyph
    # at a computed slot means that icon is disabled.
    state["topbar"] = {"prev": None, "next": None}
    back = next((t for t in targets if t.get("name") == "back"), None)
    home = next((t for t in targets if t.get("name") == "home"), None)
    if back is not None and home is not None:
        try:
            pitch = float(home["cx"]) - float(back["cx"])
            home_cx, home_cy = float(home["cx"]), float(home["cy"])
        except (KeyError, TypeError, ValueError):
            pitch = 0.0
        if pitch > 0:
            glyphs = [t for t in targets if _is_glyph_name(t.get("name"))
                      and abs(float(t.get("cy", -1000)) - home_cy) <= pitch / 4]
            for k, key in ((1, "prev"), (2, "next")):
                for g in glyphs:
                    if abs(float(g["cx"]) - (home_cx + k * pitch)) <= pitch / 4:
                        state["topbar"][key] = (g["cx"], g["cy"])
                        break
    return state


def _lcd_edit_nav(state: dict, which: str) -> "Optional[tuple[int, int]]":
    """Next/Prev xy from a page state, or None when that icon is disabled (no
    glyph at its computed slot) or the Back/Home anchors are missing. Never
    guesses: with Next disabled and Prev enabled the rightmost glyph is Prev,
    which must not be tapped as Next."""
    return (state.get("topbar") or {}).get("next" if which == "next" else "prev")


def _lcd_edit_step(env: dict, field: str, sign: str) -> "Optional[str]":
    """One stepper tap, located fresh. Returns None when tapped, else why not
    ('unreadable', 'keypad', 'absent'); never retries a tap."""
    st = _lcd_edit_page_state(env["ui"])
    if not st["readable"]:
        return "unreadable"
    if st["keypad"]:
        return "keypad"
    pair = st["steppers"].get(field)
    if pair is None:
        return "absent"
    xy = pair[0] if sign == "-" else pair[1]
    ok = _lcd22_tap(env["ctx"], xy)
    env["observed"].setdefault("taps", []).append({"field": field, "sign": sign, "xy": list(xy), "ok": ok})
    return None if ok else "inject_failed"


def _lcd_edit_nav_tap(env: dict, which: str) -> "Optional[str]":
    st = _lcd_edit_page_state(env["ui"])
    if not st["readable"]:
        return "unreadable"
    if st["keypad"]:
        return "keypad"
    xy = _lcd_edit_nav(st, which)
    if xy is None:
        return "absent"
    ok = _lcd22_tap(env["ctx"], xy)
    env["observed"].setdefault("taps", []).append({"nav": which, "xy": list(xy), "ok": ok})
    return None if ok else "inject_failed"


def _lcd_edit_nav_signature_ok(st: dict, k: int, count: int) -> bool:
    """True when the topbar shows segment `k` of `count`: Prev present iff
    k > 0 and Next present iff k < count - 1 (a disabled icon is absent)."""
    tb = st.get("topbar") or {}
    return (tb.get("prev") is not None) == (k > 0) and (tb.get("next") is not None) == (k < count - 1)


def _lcd_edit_note_read(env: dict, st: dict) -> None:
    """Count unreadable / truncated Edit-page tap-list reads into observed."""
    obs = env["observed"]
    if not st.get("readable") and not st.get("keypad"):
        obs["edit_page_unreadable_reads"] = obs.get("edit_page_unreadable_reads", 0) + 1
    if st.get("truncated"):
        obs["edit_page_truncated_reads"] = obs.get("edit_page_truncated_reads", 0) + 1


def _lcd_edit_read_note(env: dict) -> str:
    obs = env["observed"]
    t, u = obs.get("edit_page_truncated_reads", 0), obs.get("edit_page_unreadable_reads", 0)
    return f" ({t} truncated and {u} unreadable tap-list reads seen)" if (t or u) else ""


def _lcd_edit_read_settled(env: dict) -> dict:
    """Page state, re-polled (bounded 2 s) while unreadable (busy, empty or a
    truncated tap list) so navigation never decides from a partial read."""
    ui, sleep, now = env["ui"], env["sleep"], env["now"]
    deadline = now() + 2.0
    while True:
        st = _lcd_edit_page_state(ui)
        _lcd_edit_note_read(env, st)
        if st["readable"] or st.get("keypad") or now() >= deadline:
            return st
        sleep(0.3)


def _lcd_edit_goto(env: dict, target: int, count: int) -> "Optional[str]":
    """Put the Edit page on segment `target` of `count` WITHOUT assuming which
    segment it opened on (firmware opens it on the running segment). Drives to
    the nearer end first -- tapping Prev (or Next) until that icon is absent,
    bounded by `count` taps, which verifies the anchor segment -- then steps
    back toward `target`, checking the topbar signature after every tap
    (bounded poll). Intermediate segments of 3+ steps share a signature and
    cannot be told apart, so with <= 4 segments every target is anchor or
    anchor +/- 1 and fully verified. Returns None when the page is verified
    on `target`, else a reason (nothing is retried)."""
    if not 0 <= target < count:
        return f"segment {target} is outside 0..{count - 1}"
    ui, sleep, now = env["ui"], env["sleep"], env["now"]
    to_end = (count - 1 - target) < target
    anchor = count - 1 if to_end else 0
    drive, back = ("next", "prev") if to_end else ("prev", "next")
    for _ in range(count):
        st = _lcd_edit_read_settled(env)
        if not st["readable"]:
            return "unreadable"
        if st["keypad"]:
            return "keypad"
        if _lcd_edit_nav(st, drive) is None:
            break
        why = _lcd_edit_nav_tap(env, drive)
        if why:
            return why
        sleep(0.3)
    st = _lcd_edit_read_settled(env)
    if not st["readable"]:
        return "unreadable"
    if st["keypad"]:
        return "keypad"
    if _lcd_edit_nav(st, drive) is not None or not _lcd_edit_nav_signature_ok(st, anchor, count):
        return f"page never reached segment {anchor} (the {drive} icon is still present or the topbar is inconsistent)"
    k = anchor
    while k != target:
        k += -1 if to_end else 1
        why = _lcd_edit_nav_tap(env, back)
        if why:
            return why
        deadline = now() + 2.0
        while True:
            sleep(0.3)
            st = _lcd_edit_page_state(ui)
            _lcd_edit_note_read(env, st)
            if st.get("keypad"):
                return "keypad"
            if st["readable"] and _lcd_edit_nav_signature_ok(st, k, count):
                break
            if now() >= deadline:
                return f"page did not show segment {k} after the {back} tap"
    env["observed"].setdefault("nav_path", []).append({"anchor": anchor, "target": target})
    return None


def _lcd_edit_tap_failure(env: dict, what: str, why: str, cid: str) -> CaseResult:
    """A tap that could not be sent. A keypad means a PIN-locked panel (dismissed,
    never typed into); anything else is INCONCLUSIVE with Apply NOT pressed."""
    if why == "keypad":
        env["observed"]["overlay_dismiss"] = _dismiss_lcd19_overlay(env["ctx"], env["ui"])
        return CaseResult(Verdict.INCONCLUSIVE, observed=env["observed"], reason=(
            f"a PIN keypad appeared during {what}; {cid} never types a PIN"))
    return CaseResult(Verdict.INCONCLUSIVE, observed=env["observed"], reason=(
        f"could not tap {what} ({why}){_lcd_edit_read_note(env)}; Apply was not pressed"))


def _lcd_edit_apply(env: dict, cid: str) -> "Optional[CaseResult]":
    """Single Apply click; None when sent, else a FAIL."""
    apply = env["ui"].click_by_name("Apply")
    env["observed"].setdefault("apply_clicks", []).append(apply.get("result"))
    if apply.get("result") == "not_found":
        env["observed"]["apply_by_coordinate"] = _lcd22_tap(env["ctx"], _LCD22_APPLY_XY)
    elif apply.get("result") not in ("ok", "verdict_unknown"):
        return CaseResult(Verdict.FAIL, observed=env["observed"],
                          reason=f"click_by_name('Apply') returned {apply.get('result')!r}")
    return None


def _lcd_edit_wait_adopted(env: dict, want_seg: int, want_target: float) -> "tuple[Optional[dict], Optional[dict]]":
    """Poll live status/content (bounded 10 s) until segment `want_seg` shows
    `want_target`; records the run's own working_id the first time one exists."""
    now, sleep = env["now"], env["sleep"]
    status = content = None
    deadline = now() + 10.0
    while True:
        status, content = _lcd22_read_live(env["client"], env["host"])
        if env["state"].get("own_working_id") is None and status is not None:
            wid = J._lcd_edit_wid(status)
            if wid >= 0:
                env["state"]["own_working_id"] = wid
        segs = (content or {}).get("segments") or []
        try:
            landed = len(segs) > want_seg and abs(float(segs[want_seg].get("target_c", -1)) - want_target) \
                <= J.LCD_EDIT_FIRING_TOL
        except (TypeError, ValueError):
            landed = False
        if landed or now() >= deadline:
            return status, content
        sleep(0.5)


def _lcd_edit_run(ctx: dict, cid: str, plan, body) -> CaseResult:
    """Shared driver for LCD-23/24, mirroring _case_lcd22's structure:
    gates -> pre-checks -> save+start slot 7 -> wait running -> wait for Edit
    -> ONE Edit click (keypad checked first) -> page open -> body(env) ->
    finally: stop, verify relays off, discard only our own working copy.

    plan(ctx, zone_temp_c) -> (segments, orig_list, max_target_c, limits) or
    a CaseResult (INCONCLUSIVE, nothing started); limits is the zone's
    {"max_temp_c", "max_ramp_c_per_hr"} as read before the run."""
    if ctx.get("allow_heat") is not True:
        return CaseResult(Verdict.NOT_RUN, reason=f"allow_heat=False: {cid} starts a real low-temperature firing")
    if ctx.get("lcd22_allow_heat") is not True:
        return CaseResult(
            Verdict.NOT_RUN,
            reason=f"lcd_edit_heat=False: {cid} starts a real low-temperature firing "
                   "and needs the separate lcd_edit_heat=True opt-in")
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx (needed for profile_live and relay reads)")
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    client = ctx.get("_profile_live_client")
    if client is None:
        from .. import profile_live_http_client as client
    srv = _srv(ctx)

    try:
        live_before = client.get_live_status(host)
        live_wid = int(live_before.get("working_id", -1))
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"could not read /api/profile/live before {cid} ({type(exc).__name__}); no action taken"))
    if live_wid >= 0 or live_before.get("active") or live_before.get("pending_decision"):
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            "a live profile edit or decision is already pending "
            f"(working_id={live_wid}); {cid} will not touch it; no action taken"),
            observed={"live_status": live_before})
    refusal_before = live_before.get("last_refusal")
    before = _lcd22_exec_dict(srv)
    if before is None or before["state_name"] not in ("idle", "done", "faulted"):
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"executor is not idle before {cid} (state={before['state_name'] if before else 'unreadable'}); "
            "no action taken"), observed={"exec": before})
    ok, why = _heat._capability_preflight_ok(ctx)
    if not ok:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{why}; no action taken")
    temps = _heat._zone_temps(ctx)
    if not temps:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no valid thermo reading to use as an ambient reference; no action taken")
    zone_temp = temps.get(0, min(temps.values()))
    planned = plan(ctx, zone_temp)
    if isinstance(planned, CaseResult):
        return planned
    segments, orig, max_target, limits = planned
    if max_target > _LCD_EDIT_MAX_TARGET_C:
        return CaseResult(Verdict.INCONCLUSIVE, reason=(
            f"zone temperature {zone_temp:.1f} C leaves no room under the {_LCD_EDIT_MAX_TARGET_C:.0f} C "
            f"case ceiling; no action taken"))
    ceiling_ok, ceiling_reason = _heat._check_zone_ceilings(ctx, _LCD_EDIT_ZONE_MASK, max_target)
    if not ceiling_ok:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{ceiling_reason}; no action taken")

    result: Optional[CaseResult] = None
    started = False
    # Both Edit-page read counters exist on EVERY outcome (0 when nothing was seen):
    # _lcd_edit_note_read only increments, so a clean run used to omit them.
    observed: Dict[str, Any] = {"zone_temp_c": zone_temp, "orig": orig,
                                "edit_page_truncated_reads": 0, "edit_page_unreadable_reads": 0,
                                "edit_settle_truncated_reads": 0}
    ui = srv._ui_test
    now = ctx.get("_now", time.monotonic)
    sleep = ctx.get("_sleep", time.sleep)
    env = {"ctx": ctx, "srv": srv, "ui": ui, "client": client, "host": host, "now": now, "sleep": sleep,
           "observed": observed, "state": {"own_working_id": None}, "refusal_before": refusal_before,
           "orig": orig, "exec_before": None, "limits": limits}
    try:
        try:
            save = srv._profiles.save(_heat.BENCH_PROFILE_SLOT_ID, _heat.BENCH_PROFILE_NAME,
                                      _LCD_EDIT_ZONE_MASK, segments)
        except Exception as exc:  # noqa: BLE001
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.save raised {type(exc).__name__}: {exc}")
            return result
        started = True  # the slot may exist from here on: the finally tears it down
        if not save.ok:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.save refused: {save.error}")
            return result
        try:
            start = srv._profiles.start(_heat.BENCH_PROFILE_SLOT_ID)
        except Exception as exc:  # noqa: BLE001
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.start raised {type(exc).__name__}: {exc}")
            return result
        if not start.ok:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.start refused: {start.error}")
            return result

        deadline = now() + 10.0
        exec_before = _lcd22_exec_dict(srv)
        while not exec_before or exec_before["state_name"] != "running":
            if now() >= deadline:
                break
            sleep(0.5)
            exec_before = _lcd22_exec_dict(srv)
        observed["exec_before"] = exec_before
        env["exec_before"] = exec_before
        if not exec_before or exec_before["state_name"] != "running":
            result = CaseResult(Verdict.INCONCLUSIVE, reason="firing never reached running; nothing was edited", observed=observed)
            return result

        _wake_and_home(ctx)
        edit_settle_log: list = []
        # The home page rebuilds its Start/Pause/Edit buttons from the executor
        # state on a UI_PAGE_HOME_REFRESH_MS = 1000 ms timer
        # (ui_page_home_internal.h:79), so Edit appears within about one
        # refresh of the firing running; _LCD22_EDIT_WAIT_S (10 s) is ten
        # periods plus the settle reads. A longer wait cannot help a firing
        # that is no longer running, so the timeout path reads the executor
        # and says which it was.
        edit_wait_t0 = now()
        edit_ready = _wait_for_home_settled(
            ctx, ui, "Edit", min_wait_s=0.0, timeout_s=_LCD22_EDIT_WAIT_S, log=edit_settle_log)
        observed["edit_wait_s"] = round(now() - edit_wait_t0, 2)
        observed["edit_settle_reads"] = edit_settle_log[-5:]
        # Home-page (Edit button) settle reads that were truncated. A truncated read may qualify for
        # PRESENCE of Edit (see _wait_for_home_settled); counted so the record shows how many did.
        observed["edit_settle_truncated_reads"] = sum(1 for r in edit_settle_log if r.get("truncated"))
        if not edit_ready:
            last = edit_settle_log[-1] if edit_settle_log else {}
            try:
                page_now = ui.get_current_page()
            except Exception:  # noqa: BLE001
                page_now = None
            exec_now = _lcd22_exec_dict(srv)
            observed["exec_at_edit_timeout"] = exec_now
            if not exec_now or exec_now.get("state_name") != "running":
                why_not = (
                    f"the firing is no longer running (exec={exec_now!r}): it ended before Edit could be "
                    "tapped. Either the profile ran out (the executor's warm start skips every segment "
                    "whose target is <= the zone temperature, profile_executor_start.c:371-479, so a "
                    "segment-0 target at or below the reading is skipped and a short profile can end at "
                    "once) or the firing never started; it is not home-page refresh latency")
            else:
                why_not = (f"the firing is still running (exec={exec_now!r}) so the home page was expected to "
                           f"show Edit within a {UI_HOME_REFRESH_MS} ms refresh")
            result = CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
                f"'Edit' never became a visible, unobstructed tap target within {_LCD22_EDIT_WAIT_S:.0f} s "
                f"(waited {observed['edit_wait_s']} s) of the firing reaching running "
                f"(page={page_now!r}, last names={last.get('names')!r}); {why_not}; no tap was sent"))
            return result
        # ONE Edit click (swallow retries only); NOT via _click_then_page: on a
        # PIN-locked panel Edit raises the keypad while the page still reads
        # "home" and a page-poll retry would tap digit keys/Cancel.
        edit_click, _sw, _nf, _wb = _click_resolving_swallow(
            ui, "Edit", max_not_found_retries=0, max_walk_busy_retries=0)
        observed["edit_click"] = edit_click.get("result")
        if edit_click.get("result") not in ("ok", "verdict_unknown"):
            result = CaseResult(Verdict.FAIL, observed=observed, reason=(
                f"click_by_name('Edit') returned {edit_click.get('result')!r} while the firing was running"))
            return result
        page_deadline = now() + _PAGE_POLL_TIMEOUT_S
        while True:
            names = _lcd19_overlay_names(ui)
            if _is_keypad_names(names):
                observed["overlay_dismiss"] = _dismiss_lcd19_overlay(ctx, ui)
                result = CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
                    f"Edit raised the PIN keypad (panel is locked); {cid} never types a PIN"))
                return result
            if ui.get_current_page() == "edit_firing":
                break
            if now() >= page_deadline:
                result = CaseResult(Verdict.FAIL, observed=observed, reason=(
                    f"Edit was clicked but the page is {ui.get_current_page()!r}, not 'edit_firing'"))
                return result
            sleep(_PAGE_POLL_INTERVAL_S)
        tap = _list_tap_targets_resolving_busy(ui)[0]
        _remember_page_targets(ctx, "edit_firing", tap)

        result = body(env)
        return result
    except Exception as exc:  # noqa: BLE001
        result = CaseResult(Verdict.FAIL, reason=f"{cid} aborted by {type(exc).__name__}: {exc}", observed=dict(observed))
        return result
    finally:
        if started:
            try:
                verified, details = _lcd22_cleanup(ctx, client, host, _heat, env["state"].get("own_working_id"))
            except Exception as exc:  # noqa: BLE001
                verified, details = False, {"cleanup_error": type(exc).__name__}
            try:
                _navigate_home(ui)
            except Exception:  # noqa: BLE001
                pass
            if result is not None:
                result.observed = dict(result.observed or {})
                result.observed["cleanup"] = details
                if not verified:
                    prior = result.reason if result.verdict != Verdict.PASS else ""
                    msg = (
                        f"{cid} cleanup could not be verified "
                        f"(exec_state={details.get('final_exec_state')}, working_id={details.get('final_working_id')}, "
                        f"safety_relay_energized={details.get('final_energized')}, "
                        f"zone_relays={details.get('final_zone_relays')}, discard={details.get('discard')}) -- "
                        "use the hardware E-stop if the fixture is still heating"
                    )
                    result.reason = f"{prior}; also: {msg}" if prior else msg
                    result.verdict = Verdict.FAIL


def _lcd_edit_zone_limits(ctx: dict) -> dict:
    """Zone 0's max_temp_c / max_ramp_c_per_hr from GET /api/zones (None for a
    value that cannot be read). Read-only."""
    out: Dict[str, Any] = {"max_temp_c": None, "max_ramp_c_per_hr": None}
    try:
        from .. import zones_http_client
        snap = ctx.get("_get_zones_config", zones_http_client.get_zones)(ctx.get("host"))
        for zone in (snap or {}).get("zones", []) or []:
            if zone.get("index") == 0:
                for key in out:
                    val = zone.get(key)
                    out[key] = float(val) if isinstance(val, (int, float)) and not isinstance(val, bool) else None
    except Exception:  # noqa: BLE001
        pass
    return out


def _lcd_edit_base_ramp(limits: dict) -> "tuple[Optional[float], str]":
    """Segment ramp rate for the case's profile: _LCD_EDIT_RAMP_C_PER_HR, or,
    when the zone's own ramp ceiling is lower, 10 C/hr below that ceiling, so
    the ramp+5 edit stays within the HARD validator's ceiling (a rate above
    it is a 400 and would false-FAIL the stepper check). (None, reason) when
    the ceiling leaves no usable rate."""
    ceiling = limits.get("max_ramp_c_per_hr")
    if ceiling is None or ceiling <= 0.0:
        return _LCD_EDIT_RAMP_C_PER_HR, ""  # unknown/unset: profiles.save states its own refusal
    ramp = min(_LCD_EDIT_RAMP_C_PER_HR, math.floor(ceiling) - 10.0)
    if ramp < _LCD_EDIT_MIN_RAMP_C_PER_HR:
        return None, (f"zone 0's ramp ceiling {ceiling:.1f} C/hr leaves no room for a "
                      f"+5 C/hr edit above {_LCD_EDIT_MIN_RAMP_C_PER_HR:.0f} C/hr")
    return float(ramp), ""


def _lcd_edit_seg(target: float, ramp: float, dwell: float) -> dict:
    return {"target_c": float(target), "ramp_c_per_hr": float(ramp), "dwell_min": float(dwell)}


def _lcd_edit_pstep(ProfileSegment, seg: dict):
    return ProfileSegment(target_c=seg["target_c"], ramp_c_per_hr=seg["ramp_c_per_hr"],
                          dwell_min=int(seg["dwell_min"]))


def _lcd23_plan(ctx: dict, zone_temp: float):
    from .. import devices
    base = _lcd_edit_base(zone_temp)
    limits = _lcd_edit_zone_limits(ctx)
    ramp, why = _lcd_edit_base_ramp(limits)
    if ramp is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{why}; no action taken")
    orig = [
        _lcd_edit_seg(base, ramp, _LCD_EDIT_SEG0_DWELL_MIN),
        _lcd_edit_seg(base + _LCD23_SEG1_OFFSET_C, ramp, _LCD23_SEG1_DWELL_MIN),
        _lcd_edit_seg(base + _LCD23_SEG2_OFFSET_C, ramp, _LCD23_SEG2_DWELL_MIN),
        _lcd_edit_seg(base + _LCD23_SEG3_OFFSET_C, ramp, _LCD23_SEG3_DWELL_MIN),
    ]
    return ([_lcd_edit_pstep(devices.ProfileSegment, s) for s in orig], orig,
            base + _LCD23_SEG3_OFFSET_C, limits)


def _lcd23_body(env: dict) -> CaseResult:
    cid = "LCD-23"
    observed, orig, srv = env["observed"], env["orig"], env["srv"]
    client, host, ui = env["client"], env["host"], env["ui"]
    now, sleep = env["now"], env["sleep"]

    # Part 1: the page opens on the RUNNING segment (ui_page_edit_firing.c:214),
    # which is not necessarily 0. Navigate by topbar state to segment T (a
    # FUTURE one), verified, then target -, ramp +, ramp +, ramp -, dwell - =>
    # net target -5 C, ramp +5 C/hr, dwell -5 min.
    edit_seg = _LCD23_EDIT_SEG
    count = len(orig)
    ex_open = _lcd22_exec_dict(srv)
    observed["exec_at_page_open"] = ex_open
    run_seg = (ex_open or {}).get("segment_index")
    if not ex_open or ex_open.get("state_name") != "running" or not isinstance(run_seg, int) or run_seg >= edit_seg:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
            f"the firing is already at segment {run_seg!r} (exec={ex_open!r}) so segment {edit_seg} is no longer "
            "in the future (or the firing is not running); nothing was edited"))
    expected = [dict(s) for s in orig]
    expected[edit_seg] = _lcd_edit_seg(orig[edit_seg]["target_c"] - _LCD22_TARGET_STEP_C,
                                       orig[edit_seg]["ramp_c_per_hr"] + 5.0,
                                       orig[edit_seg]["dwell_min"] - _LCD22_DWELL_STEP_MIN)
    observed["expected"] = expected
    why = _lcd_edit_goto(env, edit_seg, count)
    if why:
        return _lcd_edit_tap_failure(env, f"navigating to segment {edit_seg}", why, cid)
    for item in (("step", "target", "-"), ("step", "ramp", "+"), ("step", "ramp", "+"),
                 ("step", "ramp", "-"), ("step", "dwell", "-")):
        why = _lcd_edit_step(env, item[1], item[2])
        if why:
            return _lcd_edit_tap_failure(env, f"{item[0]} {item[1:]}", why, cid)
    fail = _lcd_edit_apply(env, cid)
    if fail:
        return fail
    status, content = _lcd_edit_wait_adopted(env, edit_seg, expected[edit_seg]["target_c"])
    exec_after = _lcd22_exec_dict(srv)
    part1 = J.judge_lcd_edit_ramp_steppers(expected, status, content, exec_after,
                                           refusal_before=env["refusal_before"])
    part1.observed = dict(part1.observed or {})
    part1.observed.update(observed)
    if part1.verdict != Verdict.PASS:
        part1.reason = f"stepper edit: {part1.reason}"
        return part1
    wid_before = J._lcd_edit_wid(status)

    # Part 2: back to the RUNNING segment (read now, not assumed to be 0), make
    # a stale edit there, wait for the firing to move on (that segment becomes
    # 'already run'), then Apply. The candidate = the page's working copy =
    # adopted + the running-segment edit, which live_edit_check_window() must
    # refuse on the LCD, before any save. The page's own segment is verified
    # by topbar state (_lcd_edit_goto), and the running segment is re-read
    # after navigating so the edit is on the segment that is running.
    ex_now = _lcd22_exec_dict(srv)
    run_now = (ex_now or {}).get("segment_index")
    if not ex_now or ex_now.get("state_name") != "running" or not isinstance(run_now, int) or run_now >= edit_seg:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
            f"the firing is at segment {run_now!r} (exec={ex_now!r}) with no later short segment left to "
            "advance into before the edited one; refusal path not exercised"))
    why = _lcd_edit_goto(env, run_now, count)
    if why:
        return _lcd_edit_tap_failure(env, f"navigating to the running segment {run_now}", why, cid)
    ex_now = _lcd22_exec_dict(srv)
    if not ex_now or ex_now.get("segment_index") != run_now or ex_now.get("state_name") != "running":
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
            f"segment {run_now} was no longer the running segment when the stale edit was due ({ex_now!r}); "
            "refusal path not exercised"))
    observed["stale_edit_segment"] = run_now
    why = _lcd_edit_step(env, "target", "+")
    if why:
        return _lcd_edit_tap_failure(env, f"segment-{run_now} target +", why, cid)
    prev_t = observed.get("zone_temp_c") if run_now == 0 else orig[run_now - 1]["target_c"]
    advance_wait_s = _lcd_edit_wait_s(_lcd_edit_seg_duration_s(prev_t, orig[run_now]))
    observed["advance_wait_s"] = advance_wait_s
    deadline = now() + advance_wait_s
    advanced = None
    while True:
        ex_now = _lcd22_exec_dict(srv)
        if ex_now and ex_now.get("state_name") == "running" and (ex_now.get("segment_index") or 0) > run_now:
            advanced = ex_now
            break
        if not ex_now or ex_now.get("state_name") != "running" or now() >= deadline:
            break
        sleep(1.0)
    observed["advanced"] = advanced
    if advanced is None:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
            f"the firing did not leave segment {run_now} within {advance_wait_s:.0f} s "
            f"(its nominal ramp+dwell plus {_LCD_EDIT_WAIT_MARGIN_S:.0f} s) "
            f"(exec={ex_now!r}); the finished-segment refusal was not exercised"))
    # Wait (bounded) for the page's 1 s poll to lock the finished segment's steppers.
    locked_seen = False
    lock_deadline = now() + _LCD_EDIT_ENDED_WAIT_S
    lock_reads0 = (observed.get("edit_page_truncated_reads", 0), observed.get("edit_page_unreadable_reads", 0))
    while True:
        st = _lcd_edit_page_state(ui)
        _lcd_edit_note_read(env, st)
        if st.get("keypad"):
            return _lcd_edit_tap_failure(env, "the locked-segment check", "keypad", cid)
        if st.get("readable") and all(st["steppers"][f] is None for f in ("target", "ramp", "dwell")):
            locked_seen = True
            break
        if now() >= lock_deadline:
            break
        sleep(0.5)
    observed["locked_seen"] = locked_seen
    if not locked_seen and lock_reads0 != (observed.get("edit_page_truncated_reads", 0),
                                           observed.get("edit_page_unreadable_reads", 0)):
        # Never judge "steppers not locked" from a partial or empty tap list.
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
            "could not confirm the finished segment's steppers locked: the page's tap list was unreadable "
            f"during the wait{_lcd_edit_read_note(env)}; Apply was not pressed"))
    fail = _lcd_edit_apply(env, cid)
    if fail:
        return fail
    sleep(2.0)
    status_after, content_after = _lcd22_read_live(client, host)
    exec_after2 = _lcd22_exec_dict(srv)

    # HTTP refusals against the same running firing.
    def _post(segs):
        try:
            client.edit_live(host, (content_after or {}).get("name") or "Edit refusal", _LCD_EDIT_ZONE_MASK,
                             [dict(kind=0, target_c=s["target_c"], ramp_c_per_hr=s["ramp_c_per_hr"],
                                   dwell_min=s["dwell_min"]) for s in segs])
            return (None, "accepted")
        except Exception as exc:  # noqa: BLE001
            return (getattr(exc, "status", None), str(getattr(exc, "detail", "") or exc)[:120])
    last_seg = count - 1
    window_segs = [dict(s) for s in expected]
    window_segs[run_now] = dict(window_segs[run_now],
                                target_c=window_segs[run_now]["target_c"] + _LCD22_TARGET_STEP_C)
    bound_segs = [dict(s) for s in expected]
    bound_segs[last_seg] = dict(bound_segs[last_seg], target_c=_LCD_EDIT_BOUND_TARGET_C)
    http_window = _post(window_segs)
    http_bound = _post(bound_segs)
    if http_bound[0] is None:
        # Accepted: stop the firing at once so it can never reach that segment.
        try:
            srv._profiles.stop()
        except Exception:  # noqa: BLE001
            pass
    # HARD validator probe: a FUTURE segment (the last) target 10 C above the zone's
    # own max_temp_c (still a legal 0-2015 value, so only profiles_validate_
    # candidate() in HARD mode can refuse it). Skipped when the ceiling is
    # unreadable/unset or +10 would leave the 0-2015 range. If the board
    # wrongly ACCEPTS it, the firing is stopped at once so the executor can
    # never reach that segment (cleanup then discards the working copy).
    max_temp = (env.get("limits") or {}).get("max_temp_c")
    http_ceiling = None
    if not (max_temp is not None and max_temp > 0.0
            and max_temp + _LCD_EDIT_CEILING_PROBE_C <= _LCD_EDIT_MAX_TARGET_FIELD_C):
        observed["ceiling_probe_skipped"] = (
            f"zone max_temp_c unreadable or unset ({max_temp!r})" if not (max_temp and max_temp > 0.0)
            else f"max_temp_c {max_temp!r} + {_LCD_EDIT_CEILING_PROBE_C} C exceeds {_LCD_EDIT_MAX_TARGET_FIELD_C} C")
    else:
        ceiling_segs = [dict(s) for s in expected]
        ceiling_segs[last_seg] = dict(ceiling_segs[last_seg], target_c=max_temp + _LCD_EDIT_CEILING_PROBE_C)
        http_ceiling = _post(ceiling_segs)
        observed["ceiling_probe_target_c"] = max_temp + _LCD_EDIT_CEILING_PROBE_C
        if http_ceiling[0] is None:
            try:
                srv._profiles.stop()
            except Exception:  # noqa: BLE001
                pass
    _s, content_http = _lcd22_read_live(client, host)
    result = J.judge_lcd_edit_refusal(expected, wid_before, status_after, content_after, exec_after2,
                                      env["refusal_before"], locked_seen, http_window, http_bound, content_http,
                                      http_ceiling=http_ceiling)
    result.observed = dict(result.observed or {})
    result.observed.update(observed)
    if result.verdict == Verdict.PASS and observed.get("ceiling_probe_skipped"):
        result.reason = f"{result.reason} (HARD ceiling probe skipped: {observed['ceiling_probe_skipped']})"
    return result


def _case_lcd23(ctx: dict) -> CaseResult:
    return _lcd_edit_run(ctx, "LCD-23", _lcd23_plan, _lcd23_body)


def _lcd24_plan(ctx: dict, zone_temp: float):
    from .. import devices
    base = _lcd_edit_base(zone_temp)
    limits = _lcd_edit_zone_limits(ctx)
    ramp, why = _lcd_edit_base_ramp(limits)
    if ramp is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{why}; no action taken")
    orig = [
        _lcd_edit_seg(base, ramp, _LCD24_SEG_DWELL_MIN),
        _lcd_edit_seg(base, ramp, _LCD24_SEG_DWELL_MIN),
        _lcd_edit_seg(base, ramp, 0),
    ]
    return [_lcd_edit_pstep(devices.ProfileSegment, s) for s in orig], orig, base + 5.0, limits


def _lcd24_edit_and_end(env: dict, cid: str):
    """Shared by LCD-24/LCD-25: edit a FUTURE segment through the LCD, Apply, check the
    edit was adopted, then let the firing end on its own (bounded). Returns a final
    CaseResult (INCONCLUSIVE/FAIL, nothing further to do) or, on success,
    {"own_wid", "expected", "end_state"}."""
    observed, orig, srv = env["observed"], env["orig"], env["srv"]
    client, host, ui = env["client"], env["host"], env["ui"]
    now, sleep = env["now"], env["sleep"]

    # The page opens on the RUNNING segment (ui_page_edit_firing.c:214), which
    # may already be past segment 0: edit the segment right after it (any
    # future one will do), navigating by topbar state, never by assumption.
    count = len(orig)
    ex_open = _lcd22_exec_dict(srv)
    observed["exec_at_page_open"] = ex_open
    run_seg = (ex_open or {}).get("segment_index")
    if not ex_open or ex_open.get("state_name") != "running" or not isinstance(run_seg, int) \
            or run_seg + 1 >= count:
        return CaseResult(Verdict.INCONCLUSIVE, observed=observed, reason=(
            f"the firing is at segment {run_seg!r} of {count} (exec={ex_open!r}) so no future segment is left "
            "to edit (or the firing is not running); nothing was edited"))
    edit_seg = run_seg + 1
    observed["edit_segment"] = edit_seg
    expected = [dict(s) for s in orig]
    expected[edit_seg] = dict(orig[edit_seg], ramp_c_per_hr=orig[edit_seg]["ramp_c_per_hr"] + 5.0)
    observed["expected"] = expected
    why = _lcd_edit_goto(env, edit_seg, count)
    if why:
        return _lcd_edit_tap_failure(env, f"navigating to segment {edit_seg}", why, cid)
    why = _lcd_edit_step(env, "ramp", "+")
    if why:
        return _lcd_edit_tap_failure(env, "step ('ramp', '+')", why, cid)
    fail = _lcd_edit_apply(env, cid)
    if fail:
        return fail
    status, content = _lcd_edit_wait_adopted(env, edit_seg, expected[edit_seg]["target_c"])
    # Segment 1's target is unchanged, so "landed" above is trivially true;
    # wait for the ramp itself before judging.
    deadline = now() + 10.0
    while True:
        segs = (content or {}).get("segments") or []
        try:
            if len(segs) > edit_seg and abs(float(segs[edit_seg].get("ramp_c_per_hr"))
                                            - expected[edit_seg]["ramp_c_per_hr"]) \
                    <= J.LCD_EDIT_FIRING_TOL:
                break
        except (TypeError, ValueError):
            pass
        if now() >= deadline:
            break
        sleep(0.5)
        status, content = _lcd22_read_live(client, host)
    part1 = J.judge_lcd_edit_ramp_steppers(expected, status, content, _lcd22_exec_dict(srv),
                                           refusal_before=env["refusal_before"])
    part1.observed = dict(part1.observed or {})
    part1.observed.update(observed)
    if part1.verdict != Verdict.PASS:
        part1.reason = f"adopting the edit: {part1.reason}"
        return part1
    own_wid = J._lcd_edit_wid(status)

    # Let the firing end on its own: the remaining ramps/dwells of the plan
    # (every target is above the reading, so none is warm-start skipped),
    # computed from the plan and bounded.
    total_s, prev_t = 0.0, observed.get("zone_temp_c")
    for seg in orig:
        total_s += _lcd_edit_seg_duration_s(prev_t, seg)
        prev_t = seg["target_c"]
    end_wait_s = _lcd_edit_wait_s(total_s)
    observed["end_wait_s"] = end_wait_s
    deadline = now() + end_wait_s
    end_state = None
    while True:
        ex = _lcd22_exec_dict(srv)
        st_name = ex["state_name"] if ex else None
        if st_name in ("done", "idle", "faulted"):
            end_state = st_name
            break
        if now() >= deadline:
            break
        sleep(1.0)
    observed["end_state"] = end_state
    if end_state is None:
        return J.judge_lcd_edit_firing_end(expected, own_wid, None, None, None, None, None, None)
    return {"own_wid": own_wid, "expected": expected, "end_state": end_state}


def _lcd24_body(env: dict) -> CaseResult:
    cid = "LCD-24"
    observed, orig, srv = env["observed"], env["orig"], env["srv"]
    client, host, ui = env["client"], env["host"], env["ui"]
    now, sleep = env["now"], env["sleep"]

    ctx_state = _lcd24_edit_and_end(env, cid)
    if isinstance(ctx_state, CaseResult):
        return ctx_state
    own_wid, expected, end_state = ctx_state["own_wid"], ctx_state["expected"], ctx_state["end_state"]
    # Give the page's 1 s poll time to notice, then read the panel.
    lcd_ended: "Optional[bool]" = None
    ended_deadline = now() + _LCD_EDIT_ENDED_WAIT_S
    while True:
        st = _lcd_edit_page_state(ui)
        if st.get("keypad"):
            return _lcd_edit_tap_failure(env, "the ended-state check", "keypad", cid)
        if st.get("readable"):
            lcd_ended = (not st["apply"]) and all(st["steppers"][f] is None for f in ("target", "ramp", "dwell"))
            if lcd_ended:
                break
        if now() >= ended_deadline:
            break
        sleep(0.5)
    observed["lcd_ended"] = lcd_ended
    status_ended, content_ended = _lcd22_read_live(client, host)
    discard_error = None
    if J._lcd_edit_wid(status_ended) != own_wid:
        # Defence in depth: never discard a working copy that is not the run's own.
        discard_error = (f"not attempted: the live working_id is {J._lcd_edit_wid(status_ended)}, "
                         f"not this run's {own_wid}")
    else:
        try:
            client.decide_live_discard(host)
        except Exception as exc:  # noqa: BLE001
            discard_error = f"{type(exc).__name__}:{getattr(exc, 'status', None)}"
    try:
        status_discarded = client.get_live_status(host)
    except Exception:  # noqa: BLE001
        status_discarded = None
    result = J.judge_lcd_edit_firing_end(expected, own_wid, end_state, lcd_ended, status_ended, content_ended,
                                         discard_error, status_discarded)
    result.observed = dict(result.observed or {})
    result.observed.update(observed)
    return result


def _case_lcd24(ctx: dict) -> CaseResult:
    return _lcd_edit_run(ctx, "LCD-24", _lcd24_plan, _lcd24_body)


# ---------------------------------------------------------------------------
# LCD-25 -- the end-of-run "Keep?" decision on the LCD (live_decide page, PIN-gated).
# Same firing as LCD-24 (one LCD edit, Apply, firing ends); then the home page's
# "Keep?" button -> PIN -> decide page -> "Discard edit" -> confirm "Discard".
# The "Save as new" leg is deliberately NOT run: the first decision consumes the
# pending working copy, so it would need a second firing, and the profile it
# creates would need srv._profiles.delete to clean up. Its button's presence is
# still checked.
# ---------------------------------------------------------------------------
_LCD25_KEEP_WAIT_S = 10.0
_LCD25_PAGE_WAIT_S = 12.0
_LCD25_NAMES_WAIT_S = 5.0
_LCD25_CLEAR_WAIT_S = 10.0


def _lcd25_names(ui) -> "tuple[Optional[set], bool]":
    """(visible tap-target names or None when unreadable, truncated)."""
    try:
        tap, _ = _list_tap_targets_resolving_busy(ui)
    except Exception:  # noqa: BLE001
        return None, False
    if not tap or tap.get("busy"):
        return None, False
    names = {t.get("name") for t in tap.get("targets", []) if t.get("name") and not t.get("hidden")}
    if not names:
        return None, bool(tap.get("truncated"))
    return names, bool(tap.get("truncated"))


def _lcd25_wait_names(env: dict, pred, timeout_s: float) -> "tuple[Optional[set], bool]":
    """Poll until pred(names) holds on a NON-truncated read; returns the last read
    either way (names None = unreadable, trunc True = a partial list that must never
    be judged for absence)."""
    now, sleep = env["now"], env["sleep"]
    deadline = now() + timeout_s
    while True:
        names, trunc = _lcd25_names(env["ui"])
        if names is not None and not trunc and pred(names):
            return names, trunc
        if now() >= deadline:
            return names, trunc
        sleep(0.5)


def _lcd25_pin(ctx: dict) -> "Optional[str]":
    """The admin PIN from ctx / the environment, never written anywhere. LCD-25 does not
    seed or change a PIN (unlike LCD-19)."""
    from . import cases_web_rw as _web  # local import: avoids a module-load cycle
    pin = (ctx.get("_lcd_pin") or {}).get("right_pin") or ctx.get("lcd_admin_pin") \
        or os.environ.get(_web._LCD_PIN_ENV)
    return pin if pin and _web._lcd_pin_well_formed(pin) else None


def _lcd25_segs(detail) -> "Optional[list]":
    try:
        return [(float(sg.target_c), float(sg.ramp_c_per_hr), int(sg.dwell_min)) for sg in detail.segments]
    except Exception:  # noqa: BLE001
        return None


def _lcd25_heap_min_free(ctx: dict, host: str) -> "Optional[int]":
    """heap_internal min_free (bytes) from the existing read-only GET /api/status heap read;
    None when it cannot be read. ctx["_get_heap_status"] is injectable for tests."""
    getter = ctx.get("_get_heap_status")
    try:
        if getter is None:
            from .. import dashboard_http_client
            getter = dashboard_http_client.get_heap_status
        value = (getter(host) or {}).get("heap_internal", {}).get("min_free")
    except Exception:  # noqa: BLE001
        return None
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def _lcd25_inconclusive(env: dict, reason: str, dismiss: bool = False) -> CaseResult:
    if dismiss:
        env["observed"]["overlay_dismiss"] = _dismiss_lcd19_overlay(env["ctx"], env["ui"])
    return CaseResult(Verdict.INCONCLUSIVE, observed=env["observed"], reason=reason)


def _lcd25_open_decide_page(env: dict) -> "Optional[CaseResult]":
    """Tap Keep? once, enter the PIN if a keypad appears (once, never retried), wait for
    the live_decide page. None when on the page, else the INCONCLUSIVE to return."""
    ui, observed, now, sleep = env["ui"], env["observed"], env["now"], env["sleep"]
    log: list = []
    settled = _wait_for_home_settled(env["ctx"], ui, "Keep?", min_wait_s=0.0,
                                     timeout_s=_LCD25_KEEP_WAIT_S, log=log)
    observed["keep_settled"] = settled
    if not settled:
        observed["keep_settle_reads"] = log[-3:]
        return _lcd25_inconclusive(env, "the home page never showed a 'Keep?' button although live status "
                                        "reports a pending decision; nothing was tapped")
    click = ui.click_by_name("Keep?")
    observed["keep_click"] = click.get("result")
    if click.get("result") not in ("ok", "verdict_unknown"):
        return _lcd25_inconclusive(env, f"click_by_name('Keep?') returned {click.get('result')!r}")
    deadline = now() + _LCD25_PAGE_WAIT_S
    pin_deadline = None
    while True:
        if ui.get_current_page() == "live_decide":
            return None
        names, _ = _lcd25_names(ui)
        if names is not None and _is_keypad_names(names):
            if pin_deadline is None:
                pin = _lcd25_pin(env["ctx"])
                observed["pin_available"] = bool(pin)
                if not pin:
                    return _lcd25_inconclusive(env, "the Keep? tap raised a PIN keypad and no valid PIN is "
                                                    "available (KILNCTL_LCD_PIN / ctx); keypad dismissed",
                                               dismiss=True)
                entry = ui.enter_pin_verified(pin)
                observed["pin_entry"] = _entry_result_summary(entry)
                if entry is None or entry.get("entry_incomplete") or not _entry_all_clicked_ok(entry):
                    return _lcd25_inconclusive(env, "PIN entry did not complete cleanly; keypad dismissed",
                                               dismiss=True)
                pin_deadline = now() + _PIN_SUBMIT_POLL_TIMEOUT_S
            elif now() >= pin_deadline:
                return _lcd25_inconclusive(env, "the keypad stayed open after the PIN was entered "
                                                "(PIN rejected?); keypad dismissed", dismiss=True)
        if now() >= deadline:
            observed["page_after_keep"] = ui.get_current_page()
            return _lcd25_inconclusive(env, "the live_decide page never opened after tapping Keep?")
        sleep(0.5)


def _lcd25_body(env: dict) -> CaseResult:
    cid = "LCD-25"
    observed, orig, srv = env["observed"], env["orig"], env["srv"]
    client, host, ui = env["client"], env["host"], env["ui"]
    now, sleep = env["now"], env["sleep"]
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    slot = _heat.BENCH_PROFILE_SLOT_ID

    st = _lcd24_edit_and_end(env, cid)
    if isinstance(st, CaseResult):
        return st
    own_wid = st["own_wid"]
    if st["end_state"] != "done":
        return _lcd25_inconclusive(env, f"the firing ended as {st['end_state']!r}, not 'done'; no decision tapped")
    status, _content = _lcd22_read_live(client, host)
    if status is None:
        return _lcd25_inconclusive(env, "could not read live status after the firing ended")
    if not status.get("pending_decision") or J._lcd_edit_wid(status) != own_wid:
        observed["status_ended"] = status
        return CaseResult(Verdict.FAIL, observed=observed, reason=(
            "after an edited firing ended, live status shows no pending decision for this run's working copy "
            f"(pending_decision={status.get('pending_decision')!r}, working_id={J._lcd_edit_wid(status)}, "
            f"expected {own_wid}); no LCD tap was made"))
    ob = status.get("origin_is_builtin")
    origin_is_builtin = ob if isinstance(ob, bool) else None
    observed["origin_id"] = status.get("origin_id")
    observed["origin_is_builtin"] = origin_is_builtin
    try:
        ids_before = {p.id for p in srv._profiles.list_all()}
    except Exception as exc:  # noqa: BLE001
        return _lcd25_inconclusive(env, f"could not list profiles before the decision ({type(exc).__name__})")

    observed["navigate_home"] = _navigate_home(ui)
    fail = _lcd25_open_decide_page(env)
    if fail is not None:
        return fail

    names, trunc = _lcd25_wait_names(
        env, lambda n: "Discard edit" in n and any(str(x).startswith("Save as new") for x in n),
        _LCD25_NAMES_WAIT_S)
    observed["page_truncated"] = trunc
    observed["page_unreadable"] = names is None
    if names is None or trunc:
        # Unreadable or partial: a missing name could just be a dropped one, so judge
        # nothing and tap nothing (the wrapper's cleanup discards the working copy).
        return _lcd25_inconclusive(env, (
            "the decide page's tap targets could not be read" if names is None else
            "the decide page's tap-target list was truncated before 'Discard edit' and 'Save as new' "
            "were both visible") + "; nothing was tapped")
    confirm_seen: "Optional[bool]" = None
    confirm_unknown = False
    if "Discard edit" in names:
        click = ui.click_by_name("Discard edit")
        observed["discard_click"] = click.get("result")
        if click.get("result") not in ("ok", "verdict_unknown"):
            return _lcd25_inconclusive(env, f"click_by_name('Discard edit') returned {click.get('result')!r}")
        cnames, ctrunc = _lcd25_wait_names(env, lambda n: "Discard" in n and "Cancel" in n, _LCD25_NAMES_WAIT_S)
        observed["confirm_truncated"] = ctrunc
        observed["confirm_unreadable"] = cnames is None
        if cnames is None or ctrunc:
            # Dialog state unknown: never tap Discard on it. Cancel it if (and only
            # if) Cancel is readable, so no dialog is left open.
            can_cancel = cnames is not None and "Cancel" in cnames
            if can_cancel:
                cancel = ui.click_by_name("Cancel")
                observed["confirm_cancel_click"] = cancel.get("result")
            return _lcd25_inconclusive(env, (
                "the confirm dialog's tap targets could not be read" if cnames is None else
                "the confirm dialog's tap-target list was truncated") + "; 'Discard' was not tapped"
                + ("" if can_cancel else " and no 'Cancel' was readable to dismiss it"))
        confirm_seen = "Discard" in cnames and "Cancel" in cnames
        if confirm_seen:
            confirm = ui.click_by_name("Discard")
            observed["confirm_click"] = confirm.get("result")
            if confirm.get("result") not in ("ok", "verdict_unknown"):
                return _lcd25_inconclusive(env, f"click_by_name('Discard') returned {confirm.get('result')!r}",
                                           dismiss=True)
            confirm_unknown = confirm.get("result") == "verdict_unknown"
    status_after = None
    deadline = now() + _LCD25_CLEAR_WAIT_S
    while True:
        try:
            status_after = client.get_live_status(host)
        except Exception:  # noqa: BLE001
            status_after = None
        if status_after is not None and J._lcd_edit_wid(status_after) < 0 and not status_after.get("pending_decision"):
            break
        if confirm_seen is not True or now() >= deadline:
            break
        sleep(0.5)
    if confirm_unknown and status_after is not None and (
            J._lcd_edit_wid(status_after) >= 0 or status_after.get("pending_decision")):
        return _lcd25_inconclusive(env, "confirm tap outcome unknown (click_by_name('Discard') returned "
                                        "'verdict_unknown') and the working copy remains")
    try:
        detail = srv._profiles.get(slot)
        segs = None if detail is None else _lcd25_segs(detail)
        orig_unchanged = None if segs is None else segs == [
            (float(x["target_c"]), float(x["ramp_c_per_hr"]), int(x["dwell_min"])) for x in orig]
    except Exception:  # noqa: BLE001
        orig_unchanged = None
    try:
        stray = sorted({p.id for p in srv._profiles.list_all()} - ids_before)
    except Exception:  # noqa: BLE001
        stray = None
    try:
        _navigate_home(ui)
    except Exception:  # noqa: BLE001
        pass
    heap_min_free = _lcd25_heap_min_free(env["ctx"], host)
    result = J.judge_lcd_keep_discard(origin_is_builtin, names, trunc, confirm_seen, status_after,
                                      orig_unchanged, stray, heap_min_free)
    result.observed = dict(result.observed or {})
    result.observed.update(observed)
    return result


def _case_lcd25(ctx: dict) -> CaseResult:
    return _lcd_edit_run(ctx, "LCD-25", _lcd24_plan, _lcd25_body)


# ---------------------------------------------------------------------------
# LCD-26 -- Back-button walk. Every LCD page reachable from home is entered by
# a real touch_inject tap and left again by a real touch_inject tap on its
# topbar Back icon; the landed page (ui.get_current_page()) must be the
# page's documented parent. Coordinates are the source-derived geometry in
# docs/COMMISSIONING_LCD_RUNBOOK.md "Tap geometry derived from source" (Table
# 1 topbar icons at y=21, Table 2 config hub grid), bench-proven 2026-10-07;
# the three data-dependent entries (Manage networks, the first profile row,
# Segments) are resolved from list_tap_targets() by name/position instead.
#
# Never taps anything that starts a profile/firing/autotune or writes config:
# no Start, no Pause, no Edit/Delete, no builder Next, no connect/forget. The
# profiles "Add" icon only opens the builder's first page (zones), which is
# left again with Back without saving anything.
#
# Wake handling: before EVERY tap `touch.get_state()` is read; a blanked
# panel (or a power_state other than ON) gets a wake tap first and the case
# demands last_swallow_reason == wake (and a grown swallow_count when the
# firmware reports one) before it believes the panel is awake. A tap that
# then fails to move the page while swallow_count grew is a swallowed tap,
# retried once -- never judged as a broken Back button. A Back tap that
# lands on the wrong page FAILs only when no swallow evidence exists.
# ---------------------------------------------------------------------------

_LCD26_ICON_Y = 21
#: Topbar Back centre x by page (runbook Table 1; Back is the leftmost icon).
_LCD26_BACK_X = {
    "config": 454,
    "temperature": 414,
    "network": 414,
    "network_manage": 414,
    "diagnostics": 334,
    "profiles": 294,
    "profile_detail": 414,
    "profile_segments": 334,
    "profile_builder_zones": 414,
    "safety": None,  # resolved from the page's own "back" tap target
}
_LCD26_GEAR = (454, _LCD26_ICON_Y)
_LCD26_PROFILES_ADD = (454, _LCD26_ICON_Y)
#: Config hub cell centres (runbook Table 2).
_LCD26_HUB = {
    "profiles": (119, 80),
    "temperature": (345, 80),
    "network": (119, 156),
    "diagnostics": (345, 156),
}
_LCD26_TAP_SETTLE_S = 0.3
_LCD26_MAX_TAP_ATTEMPTS = 2


def _lcd26_touch_state(touch):
    try:
        return touch.get_state()
    except Exception:  # noqa: BLE001
        return None


def _lcd26_awake(state) -> "Optional[bool]":
    if state is None:
        return None
    on = _power_state_is_on(state)
    if on is None:
        return bool(getattr(state, "screen_on", False))
    return bool(on) and bool(getattr(state, "screen_on", True))


def _lcd26_ensure_awake(env: dict) -> "tuple[bool, dict]":
    """Wake the panel when touch_get_state says it is not on. Returns
    (awake, info); awake False means a wake could not be confirmed."""
    touch, sleep = env["touch"], env["sleep"]
    info: Dict[str, Any] = {}
    state = _lcd26_touch_state(touch)
    awake = _lcd26_awake(state)
    if awake is None or awake:
        # unreadable state: proceed; the swallow check after the tap still guards
        return True, info
    before = getattr(state, "swallow_count", None)
    for attempt in range(1, _ERROR_HOLD_DISMISS_MAX_ATTEMPTS + 2):
        x, y = _WAKE_TOUCH_XY
        try:
            touch.inject(x, y, True)
            touch.inject(x, y, False)
        except Exception as exc:  # noqa: BLE001
            info["wake_error"] = type(exc).__name__
            return False, info
        info["wake_taps"] = attempt
        deadline = time.monotonic() + _WAKE_SCREEN_ON_TIMEOUT_S
        while True:
            sleep(_WAKE_SCREEN_ON_POLL_S)
            state = _lcd26_touch_state(touch)
            if _lcd26_awake(state):
                reason = getattr(state, "last_swallow_reason", None)
                count = getattr(state, "swallow_count", None)
                info["wake_swallow_reason"] = reason
                if reason is not None and reason != TOUCH_SWALLOW_REASON_WAKE:
                    # woke, but the last swallow was not the wake tap: not the
                    # expected signature, so do not trust it
                    info["wake_reason_unexpected"] = True
                    return False, info
                if before is not None and count is not None and count <= before:
                    info["wake_count_not_grown"] = True
                    return False, info
                return True, info
            if time.monotonic() >= deadline:
                break
    return False, info


def _lcd26_step(env: dict, label: str, xy: "tuple[int, int]", expect: str, kind: str) -> str:
    """One tap. kind is 'enter' or 'back'. Returns 'ok' | 'miss' | 'swallowed'
    | 'wake_unconfirmed' | 'error' and appends a record to env['steps']."""
    touch, ui, sleep = env["touch"], env["ui"], env["sleep"]
    rec: Dict[str, Any] = {"label": label, "kind": kind, "xy": list(xy), "expect": expect}
    env["steps"].append(rec)
    status = "miss"
    for attempt in range(1, _LCD26_MAX_TAP_ATTEMPTS + 1):
        awake, info = _lcd26_ensure_awake(env)
        if info:
            rec.setdefault("wake", []).append(info)
        if not awake:
            rec["status"] = status = "wake_unconfirmed"
            return status
        state = _lcd26_touch_state(touch)
        before = getattr(state, "swallow_count", None)
        try:
            press = touch.inject(xy[0], xy[1], True)
            release = touch.inject(xy[0], xy[1], False)
        except Exception as exc:  # noqa: BLE001
            rec["status"] = status = "error"
            rec["error"] = type(exc).__name__
            return status
        if not (getattr(press, "ok", True) and getattr(release, "ok", True)):
            rec["status"] = status = "error"
            rec["error"] = "touch_inject refused"
            return status
        sleep(_LCD26_TAP_SETTLE_S)
        try:
            page, _ = _wait_for_page(ui, expect)
        except Exception as exc:  # noqa: BLE001
            rec["status"] = status = "error"
            rec["error"] = type(exc).__name__
            return status
        rec["landed"] = page
        rec["attempts"] = attempt
        if page == expect:
            rec["status"] = status = "ok"
            return status
        after = getattr(_lcd26_touch_state(touch), "swallow_count", None)
        if before is not None and after is not None and after > before:
            status = "swallowed"
            rec.setdefault("swallowed_attempts", []).append(attempt)
            continue
        status = "miss"
        break
    rec["status"] = status
    return status


def _lcd26_target(env: dict, pred) -> "Optional[dict]":
    try:
        tap, _ = _list_tap_targets_resolving_busy(env["ui"])
    except Exception:  # noqa: BLE001
        return None
    for t in tap.get("targets", []):
        if not t.get("hidden") and pred(t):
            return t
    return None


def _lcd26_record(env: dict, status: str, kind: str, label: str, expect: str) -> bool:
    """Fold a step status into the verdict lists. True when the walk may continue."""
    if status == "ok":
        return True
    if kind == "back" and status == "miss":
        env["fails"].append(f"{label}: landed on {env['steps'][-1].get('landed')!r}, expected {expect!r}")
    else:
        env["inconclusive"].append(f"{label}: {status}")
    return False


def _lcd26_to_config(env: dict) -> bool:
    ui = env["ui"]
    try:
        if ui.get_current_page() == "config":
            return True
    except Exception:  # noqa: BLE001
        pass
    _navigate_home(ui)
    return _lcd26_record(
        env, _lcd26_step(env, "recover home->config", _LCD26_GEAR, "config", "enter"),
        "enter", "recover home->config", "config",
    )


def _lcd26_back(env: dict, page: str, parent: str) -> bool:
    x = _LCD26_BACK_X.get(page)
    if x is None:
        t = _lcd26_target(env, lambda t: t.get("name") == "back")
        if t is None:
            env["inconclusive"].append(f"{page}: no back tap target to locate Back")
            return False
        xy = (int(t["cx"]), int(t["cy"]))
    else:
        xy = (x, _LCD26_ICON_Y)
    lab = f"{page} back"
    return _lcd26_record(env, _lcd26_step(env, lab, xy, parent, "back"), "back", lab, parent)


def _lcd26_enter(env: dict, xy: "tuple[int, int]", page: str) -> bool:
    lab = f"enter {page}"
    return _lcd26_record(env, _lcd26_step(env, lab, xy, page, "enter"), "enter", lab, page)


def _lcd26_branch_simple(env: dict, page: str) -> None:
    if _lcd26_to_config(env) and _lcd26_enter(env, _LCD26_HUB[page], page):
        _lcd26_back(env, page, "config")


def _lcd26_branch_network(env: dict) -> None:
    if not (_lcd26_to_config(env) and _lcd26_enter(env, _LCD26_HUB["network"], "network")):
        return
    t = _lcd26_target(env, lambda t: t.get("name") == "Manage networks")
    if t is None:
        env["inconclusive"].append("network_manage: no 'Manage networks' tap target")
    elif _lcd26_enter(env, (int(t["cx"]), int(t["cy"])), "network_manage"):
        if not _lcd26_back(env, "network_manage", "network"):
            return
    _lcd26_back(env, "network", "config")


def _lcd26_branch_profiles(env: dict) -> None:
    if not (_lcd26_to_config(env) and _lcd26_enter(env, _LCD26_HUB["profiles"], "profiles")):
        return
    try:
        tap, _ = _list_tap_targets_resolving_busy(env["ui"])
        rows = _profile_rows_by_position(tap.get("targets", []))
    except Exception:  # noqa: BLE001
        rows = []
    if not rows:
        env["inconclusive"].append("profile_detail: no profile row on the profiles page")
    elif _lcd26_enter(env, (int(rows[0]["cx"]), int(rows[0]["cy"])), "profile_detail"):
        seg = _lcd26_target(env, lambda t: t.get("name") == "Segments")
        if seg is None:
            env["inconclusive"].append("profile_segments: no 'Segments' tap target")
        elif _lcd26_enter(env, (int(seg["cx"]), int(seg["cy"])), "profile_segments"):
            if not _lcd26_back(env, "profile_segments", "profile_detail"):
                return
        if not _lcd26_back(env, "profile_detail", "profiles"):
            return
    else:
        return
    if _lcd26_enter(env, _LCD26_PROFILES_ADD, "profile_builder_zones"):
        if not _lcd26_back(env, "profile_builder_zones", "profiles"):
            return
    else:
        return
    _lcd26_back(env, "profiles", "config")


def _lcd26_branch_safety(env: dict) -> None:
    """Only when the config hub offers a cell named exactly 'safety' (the LCD
    Safety page may not be in the running build)."""
    if not _lcd26_to_config(env):
        return
    t = _lcd26_target(env, lambda t: str(t.get("name", "")).strip().lower() == "safety")
    if t is None:
        env["skipped"].append("safety: page not offered on the config hub of this build")
        return
    if _lcd26_enter(env, (int(t["cx"]), int(t["cy"])), "safety"):
        _lcd26_back(env, "safety", "config")


def _case_lcd26(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    if touch is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no touch client: cannot inject Back taps")
    ui = srv._ui_test
    env: Dict[str, Any] = {
        "touch": touch, "ui": ui, "sleep": ctx.get("_sleep", time.sleep),
        "steps": [], "fails": [], "inconclusive": [], "skipped": [],
    }
    try:
        _lcd26_branch_simple(env, "temperature")
        _lcd26_branch_simple(env, "diagnostics")
        _lcd26_branch_network(env)
        _lcd26_branch_profiles(env)
        _lcd26_branch_safety(env)
        # config's own Back (config -> home) last
        if _lcd26_to_config(env):
            _lcd26_back(env, "config", "home")
    finally:
        nav = _navigate_home(ui)
    observed: Dict[str, Any] = {
        "steps": env["steps"], "skipped": env["skipped"],
        "back_verified": [s["label"] for s in env["steps"] if s["kind"] == "back" and s.get("status") == "ok"],
    }
    if not nav["ok"]:
        observed["navigate_home"] = nav
    if env["fails"]:
        return CaseResult(Verdict.FAIL, reason="; ".join(env["fails"]), observed=observed)
    if env["inconclusive"]:
        return CaseResult(Verdict.INCONCLUSIVE, reason="; ".join(env["inconclusive"]), observed=observed)
    return CaseResult(Verdict.PASS, observed=observed)


# ---------------------------------------------------------------------------
# LCD-05 / LCD-06 / LCD-13 -- brightness, blanking, segments paging.
# Display settings go through GET/POST /api/settings/display_power (the same
# route WEB-DISP-02 uses; fields brightness_percent, timeout_setting ordinal
# 0=1min..5=Never, keep_on_while_firing, display_on_error, brightness_inert).
# Every setting changed is restored in `finally`.
# ---------------------------------------------------------------------------

_DISPLAY_POWER_PATH = "/api/settings/display_power"
_LCD05_MIN_DROP_FRACTION = 0.25
_LCD05_TEST_BRIGHTNESS = 50
_LCD06_TIMEOUT_1MIN = 0
_LCD06_WAIT_S = 70.0
#: Panel-interior box in the 1280x720 camera frame (inside lcd_sampler's
#: FRAME_CORNERS), sampled as the "whole panel" mean.
_LCD06_PANEL_BOX = (400, 250, 400, 250)
_LCD13_ROWS_PER_PAGE = 4


def _luminance(rgb) -> float:
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def _display_power_read(ctx: dict) -> "Optional[dict]":
    from . import cases_web_rw as _web
    status, body = _web._get_json(ctx, _DISPLAY_POWER_PATH)
    if status != 200 or not isinstance(body, dict):
        return None
    return body


def _display_power_write(ctx: dict, cfg: dict, brightness: int, timeout: int) -> bool:
    from . import cases_web_rw as _web
    status, _ = _web._post_json(ctx, _DISPLAY_POWER_PATH, {
        "brightness": str(int(brightness)),
        "timeout": str(int(timeout)),
        "keep_on_while_firing": "1" if cfg.get("keep_on_while_firing") else "0",
        "display_on_error": "1" if cfg.get("display_on_error") else "0",
    })
    return status == 200


def _display_power_restore(ctx: dict, orig: dict, observed: dict) -> None:
    try:
        ok = _display_power_write(ctx, orig, orig["brightness_percent"], orig["timeout_setting"])
    except Exception as exc:  # noqa: BLE001
        ok = False
        observed["restore_error"] = type(exc).__name__
    observed["restored"] = ok
    if not ok:
        logging.getLogger(__name__).error("display_power restore failed: %s", observed)


def _judge_lcd05(lum_full: float, lum_dim: float, inert: bool) -> CaseResult:
    obs = {"luminance_100": round(lum_full, 2), "luminance_50": round(lum_dim, 2),
           "min_drop_fraction": _LCD05_MIN_DROP_FRACTION}
    if inert:
        return CaseResult(Verdict.INCONCLUSIVE, reason="brightness_inert: this build has no backlight control line", observed=obs)
    if lum_full <= 0:
        return CaseResult(Verdict.INCONCLUSIVE, reason="100% reference region reads black", observed=obs)
    drop = (lum_full - lum_dim) / lum_full
    obs["drop_fraction"] = round(drop, 3)
    if drop >= _LCD05_MIN_DROP_FRACTION:
        return CaseResult(Verdict.PASS, observed=obs)
    return CaseResult(Verdict.FAIL, reason=f"luminance dropped only {drop:.1%} at 50% brightness (need >= {_LCD05_MIN_DROP_FRACTION:.0%})", observed=obs)


def _lcd05_sample_lum(ctx: dict, name: str, target: dict) -> "Optional[float]":
    path = _capture(ctx, name)
    if not path:
        return None
    try:
        sample = lcd_sampler.sample_widget_body(path, target["cx"], target["cy"], repo_root=ctx.get("repo_root"))
    except lcd_sampler.LcdCaptureError:
        return None
    return _luminance(sample.region)


def _case_lcd05(ctx: dict) -> CaseResult:
    orig = _display_power_read(ctx)
    if orig is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"could not read {_DISPLAY_POWER_PATH}")
    if orig.get("brightness_inert"):
        return _judge_lcd05(0.0, 0.0, True)
    _wake_and_home(ctx)
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    observed: Dict[str, Any] = {}
    try:
        targets = srv._ui_test.list_tap_targets().get("targets", [])
        # Text-bearing home widget (label drawn in TEXT_PRIMARY); any visible
        # named target works since only the relative drop is judged.
        target = next((t for t in targets if t.get("name") == "start" and not t.get("hidden")), None) \
            or next((t for t in targets if t.get("name") and not t.get("hidden")), None)
        if target is None:
            return CaseResult(Verdict.INCONCLUSIVE, reason="no visible text target to sample")
        if not _display_power_write(ctx, orig, 100, orig["timeout_setting"]):
            return CaseResult(Verdict.INCONCLUSIVE, reason="could not set brightness 100")
        sleep(1.0)
        lum_full = _lcd05_sample_lum(ctx, "lcd05_100.jpg", target)
        if not _display_power_write(ctx, orig, _LCD05_TEST_BRIGHTNESS, orig["timeout_setting"]):
            return CaseResult(Verdict.INCONCLUSIVE, reason="could not set brightness 50")
        sleep(1.0)
        lum_dim = _lcd05_sample_lum(ctx, "lcd05_50.jpg", target)
        if lum_full is None or lum_dim is None:
            return CaseResult(Verdict.INCONCLUSIVE, reason="camera capture/sample failed")
        after = _display_power_read(ctx) or {}
        return _judge_lcd05(lum_full, lum_dim, bool(after.get("brightness_inert", orig.get("brightness_inert"))))
    finally:
        _display_power_restore(ctx, orig, observed)


def _judge_lcd06(panel_rgb, bezel_rgb, woke: "Optional[bool]") -> CaseResult:
    obs = {"panel_rgb": list(panel_rgb) if panel_rgb else None,
           "bezel_rgb": list(bezel_rgb) if bezel_rgb else None, "woke": woke}
    if panel_rgb is None or bezel_rgb is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no panel/bezel sample", observed=obs)
    if not lcd_sampler.is_off(panel_rgb, bezel_rgb):
        return CaseResult(Verdict.FAIL, reason="panel still lit after the 1 min blank timeout + 70 s", observed=obs)
    if woke is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="touch state unreadable after wake tap", observed=obs)
    if not woke:
        return CaseResult(Verdict.FAIL, reason="touch_inject did not wake the blanked panel (touch_get_state not on)", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_lcd06(ctx: dict) -> CaseResult:
    orig = _display_power_read(ctx)
    if orig is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"could not read {_DISPLAY_POWER_PATH}")
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    if touch is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no touch client: cannot wake or read screen state")
    _wake_and_home(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    observed: Dict[str, Any] = {}
    try:
        if not _display_power_write(ctx, orig, orig["brightness_percent"], _LCD06_TIMEOUT_1MIN):
            return CaseResult(Verdict.INCONCLUSIVE, reason="could not set 1 min timeout")
        sleep(_LCD06_WAIT_S)
        path = _capture(ctx, "lcd06_blank.jpg")
        if not path:
            return CaseResult(Verdict.INCONCLUSIVE, reason="camera capture failed")
        try:
            x, y, w, h = _LCD06_PANEL_BOX
            sample = lcd_sampler.sample_region(path, x, y, w, h, repo_root=ctx.get("repo_root"))
        except lcd_sampler.LcdCaptureError:
            return CaseResult(Verdict.INCONCLUSIVE, reason="panel sample failed")
        woke: "Optional[bool]" = None
        try:
            wx, wy = _WAKE_TOUCH_XY
            touch.inject(wx, wy, True)
            touch.inject(wx, wy, False)
            deadline = time.monotonic() + _WAKE_SCREEN_ON_TIMEOUT_S + 2.0
            while True:
                woke = bool(touch.get_state().screen_on)
                if woke or time.monotonic() >= deadline:
                    break
                time.sleep(_WAKE_SCREEN_ON_POLL_S)
        except Exception:  # noqa: BLE001
            woke = None
        result = _judge_lcd06(sample.region, sample.bezel, woke)
        result.evidence = [path]
        return result
    finally:
        _display_power_restore(ctx, orig, observed)


def _judge_lcd13(expected_pages: int, pages_seen: int, first_page_next: bool) -> CaseResult:
    obs = {"expected_pages": expected_pages, "pages_seen": pages_seen, "next_on_first_page": first_page_next}
    if expected_pages > 1 and not first_page_next:
        return CaseResult(Verdict.FAIL, reason="no Next control on segments page 1 of a >4-segment profile", observed=obs)
    if pages_seen != expected_pages:
        return CaseResult(Verdict.FAIL, reason=f"paged through {pages_seen} pages, expected {expected_pages} at {_LCD13_ROWS_PER_PAGE} rows/page", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _lcd13_tap(touch, t: dict) -> None:
    touch.inject(int(t["cx"]), int(t["cy"]), True)
    touch.inject(int(t["cx"]), int(t["cy"]), False)
    time.sleep(_LCD26_TAP_SETTLE_S)


def _case_lcd13(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    touch = getattr(srv, "_touch", None)
    if touch is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no touch client: cannot press Next")
    try:
        cands = [p for p in srv._profiles.list_all() if p.builtin and p.segment_count > _LCD13_ROWS_PER_PAGE]
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles list failed: {type(exc).__name__}")
    if not cands:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no builtin profile with more than 4 segments")
    prof = cands[0]
    expected_pages = (prof.segment_count + _LCD13_ROWS_PER_PAGE - 1) // _LCD13_ROWS_PER_PAGE
    outcome: Optional[CaseResult] = None
    try:
        fail = _click_then_page(ui, "settings", "config")[0] or _click_then_page(ui, "Profiles", "profiles")[0]
        if fail is not None:
            outcome = fail
            return outcome
        # Page the picker until the chosen profile's row shows.
        found = None
        for _ in range(12):
            tap, _b = _list_tap_targets_resolving_busy(ui)
            targets = tap.get("targets", [])
            found = next((r for r in _profile_rows_by_position(targets) if r.get("name") == prof.name), None)
            nxt = _diagnostics_next_target(targets)
            if found is not None or nxt is None:
                break
            _lcd13_tap(touch, nxt)
        if found is None:
            outcome = CaseResult(Verdict.INCONCLUSIVE, reason=f"profile {prof.name!r} row not found on the picker")
            return outcome
        rows = [found]  # profile-picker row label, same allowed non-literal form as LCD-09
        fail = _click_then_page(ui, rows[0]["name"], "profile_detail")[0] or _click_then_page(ui, "Segments", "profile_segments")[0]
        if fail is not None:
            outcome = fail
            return outcome
        pages = 1
        first_next = False
        for _ in range(expected_pages + 3):
            tap, _b = _list_tap_targets_resolving_busy(ui)
            nxt = _diagnostics_next_target(tap.get("targets", []))
            if pages == 1:
                first_next = nxt is not None
            if nxt is None:
                break
            _lcd13_tap(touch, nxt)
            pages += 1
        outcome = _judge_lcd13(expected_pages, pages, first_next)
        outcome.observed = dict(outcome.observed or {})
        outcome.observed["profile"] = prof.name
        outcome.observed["segment_count"] = prof.segment_count
        return outcome
    finally:
        nav = _navigate_home(ui)
        if outcome is not None and not nav["ok"]:
            outcome.observed = dict(outcome.observed or {})
            outcome.observed["navigate_home"] = nav



_LCD20_WAIT_S = 70.0


def observe_recovery_idle(ctx: dict) -> CaseResult:
    """LCD-20: runs only inside OT-E11, while the recovery image is up. The
    recovery image's screen_idle recovery flag means the panel must not blank:
    wait past the default blank timeout, sample the panel against the bezel.
    Lit -> INCONCLUSIVE by design (the recovery image never blanks); dark -> FAIL;
    no capture/sample -> INCONCLUSIVE."""
    sleep = ctx.get("_sleep", time.sleep)
    sleep(ctx.get("lcd20_wait_s", _LCD20_WAIT_S))
    path = _capture(ctx, "lcd20_recovery_idle.jpg")
    if not path:
        return CaseResult(Verdict.INCONCLUSIVE, reason="camera capture failed")
    try:
        x, y, w, h = _LCD06_PANEL_BOX
        sample = lcd_sampler.sample_region(path, x, y, w, h, repo_root=ctx.get("repo_root"))
    except lcd_sampler.LcdCaptureError:
        return CaseResult(Verdict.INCONCLUSIVE, reason="panel sample failed", evidence=[path])
    obs = {"panel_rgb": list(sample.region) if sample.region else None,
           "bezel_rgb": list(sample.bezel) if sample.bezel else None}
    if sample.region is None or sample.bezel is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no panel/bezel sample", observed=obs, evidence=[path])
    if lcd_sampler.is_off(sample.region, sample.bezel):
        return CaseResult(Verdict.FAIL, reason="panel blanked in the recovery image", observed=obs, evidence=[path])
    # INCONCLUSIVE by design: the standalone recovery image never blanks
    # (recovery_lcd.c), so a lit panel proves nothing about screen_idle's
    # recovery flag, which belongs to the APP's boot_guard RECOVERY MODE
    # (screen_idle.c). Dark would still be a real defect (FAIL above).
    return CaseResult(Verdict.INCONCLUSIVE, observed=obs, evidence=[path],
                      reason="panel stayed lit in the standalone recovery image, which never blanks by design; "
                             "this does not exercise screen_idle's recovery flag (app boot_guard RECOVERY MODE)")


def _case_lcd20(ctx: dict) -> CaseResult:
    """Reports what OT-E11 observed this run; NOT_RUN if OT-E11 never reached recovery."""
    res = ctx.get("_lcd20_result")
    if res is None:
        return CaseResult(Verdict.NOT_RUN, reason="LCD-20 only runs inside OT-E11 (recovery image up)")
    return res
# ---------------------------------------------------------------------------
# LCD-10 / LCD-11 / LCD-12 -- the writing profile pages (start/stop confirm,
# two-tap delete, builder save). Every one goes through the suite's
# fail-closed write gate (board_lock.write_refusal), works ONLY on a
# transient free USER slot it picks and records itself (never slot 7, the
# hidden bench slot), and deletes what it created in `finally`. LCD-10 starts
# a real firing for a few seconds and therefore also needs allow_heat.
# ---------------------------------------------------------------------------

_LCDWR_SLOT_RANGE = range(0, 16)
_LCDWR_NAMES_WAIT_S = 5.0
_LCDWR_DELETE_DEBOUNCE_S = 0.4
_LCDWR_STALE_WAIT_S = 6.0
_LCDWR_RUN_WAIT_S = 12.0
_LCDWR_FREE_CELL_RE = re.compile(r"^(\d+) - free$")


def _lcdwr_gate(ctx: dict) -> Optional[CaseResult]:
    refusal = board_lock.write_refusal(ctx)
    if refusal:
        return CaseResult(Verdict.SKIP, reason=f"gate: {refusal}; no write attempted")
    return None


def _lcdwr_env(ctx: dict) -> dict:
    srv = _srv(ctx)
    return {"ctx": ctx, "srv": srv, "ui": srv._ui_test, "now": ctx.get("_now", time.monotonic),
            "sleep": ctx.get("_sleep", time.sleep), "observed": {}}


def _lcdwr_free_slot(srv) -> "tuple[Optional[int], Optional[set]]":
    """(slot, used_ids). used_ids is None when the listing could not be read:
    "unknown" is never the same as "empty" (review CRITICAL-1)."""
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    try:
        used = {p.id for p in srv._profiles.list_all()}
    except Exception:  # noqa: BLE001
        return None, None
    for sid in _LCDWR_SLOT_RANGE:
        if sid not in used and sid != _heat.BENCH_PROFILE_SLOT_ID:
            return sid, used
    return None, used


def _lcdwr_save_tmp(env: dict, name: str, target_c: float) -> "tuple[Optional[int], Optional[CaseResult], set]":
    """Save a one-segment transient profile into the first free user slot.
    Returns (slot, None, ids_before) or (None, INCONCLUSIVE, ids_before)."""
    from .. import devices
    srv = env["srv"]
    slot, used = _lcdwr_free_slot(srv)
    if used is None:
        return None, CaseResult(Verdict.INCONCLUSIVE,
                                reason="could not read the profile list; cannot attribute what this run creates; nothing written"), None
    if slot is None:
        return None, CaseResult(Verdict.INCONCLUSIVE, reason="no free user profile slot; nothing written"), used
    seg = [devices.ProfileSegment(target_c=target_c, ramp_c_per_hr=100.0, dwell_min=1)]
    env["created"] = slot  # may exist from here on: the finally deletes it
    env["created_name"] = name  # teardown deletes the slot only if it still holds this name
    try:
        res = srv._profiles.save(slot, name, 1, seg)
    except Exception as exc:  # noqa: BLE001
        return None, CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.save raised {type(exc).__name__}"), used
    if not res.ok:
        return None, CaseResult(Verdict.INCONCLUSIVE, reason=f"profiles.save refused: {res.error}"), used
    env["observed"]["slot"] = slot
    return slot, None, used


def _lcdwr_delete_new(env: dict, ids_before: "Optional[set]") -> dict:
    """Delete ONLY the slot this run created (env["created"]), and only after a
    fresh listing shows that slot holds the name the case saved (or, for a
    UI-created slot with no known name, that it was absent from ids_before).
    Never deletes a diff of two listings, never slot BENCH_PROFILE_SLOT_ID, and
    deletes nothing when ids_before is unknown. Reports whether the list is
    back to what it was (None when that cannot be shown)."""
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    srv = env["srv"]
    out: Dict[str, Any] = {}
    created = env.get("created")
    want_name = env.get("created_name")
    if ids_before is None:
        out["skipped"] = "ids_before unknown: nothing deleted"
        out["restored"] = None
        return out
    if created is not None and created != _heat.BENCH_PROFILE_SLOT_ID and created not in ids_before:
        try:
            now_list = srv._profiles.list_all()
        except Exception:  # noqa: BLE001
            now_list = None
            out["list_error"] = True
        if now_list is not None:
            entry = next((p for p in now_list if p.id == created), None)
            if entry is None:
                out["created_absent"] = True
            elif want_name is not None and getattr(entry, "name", None) != want_name:
                out["delete_skipped_name_mismatch"] = getattr(entry, "name", None)
            else:
                try:
                    srv._profiles.delete(created)
                except Exception as exc:  # noqa: BLE001
                    out[f"delete_{created}"] = type(exc).__name__
    try:
        after = {p.id for p in srv._profiles.list_all()}
        out["restored"] = after == ids_before
    except Exception:  # noqa: BLE001
        out["restored"] = None
    return out


def _lcdwr_open_picker(env: dict) -> "Optional[CaseResult]":
    ui = env["ui"]
    fail = _click_then_page(ui, "settings", "config")[0]
    if fail is not None:
        return fail
    return _click_then_page(ui, "Profiles", "profiles")[0]


def _lcdwr_find_row(env: dict, name: str) -> "Optional[dict]":
    ui, touch = env["ui"], getattr(env["srv"], "_touch", None)
    for _ in range(12):
        tap, _b = _list_tap_targets_resolving_busy(ui)
        targets = tap.get("targets", [])
        row = next((r for r in _profile_rows_by_position(targets) if r.get("name") == name), None)
        if row is not None:
            return row
        nxt = _diagnostics_next_target(targets)
        if nxt is None or touch is None:
            return None
        _lcd13_tap(touch, nxt)
    return None


def _lcdwr_exec_state(srv) -> Optional[str]:
    ex = _lcd22_exec_dict(srv)
    return ex["state_name"] if ex else None


def _lcdwr_finish(env: dict, result: Optional[CaseResult], ids_before: "Optional[set]") -> None:
    """Common teardown tail: dismiss any leftover popup, go home, delete only
    this run's own slots; a restore that did not verify turns a PASS into a
    FAIL (never leaves a PASS standing over a dirty board)."""
    ui = env["ui"]
    cleanup: Dict[str, Any] = {}
    try:
        cleanup["dismiss"] = _dismiss_lcd19_overlay(env["ctx"], ui)
    except Exception as exc:  # noqa: BLE001
        cleanup["dismiss_error"] = type(exc).__name__
    try:
        cleanup["navigate_home"] = _navigate_home(ui)
    except Exception as exc:  # noqa: BLE001
        cleanup["navigate_error"] = type(exc).__name__
    cleanup.update(_lcdwr_delete_new(env, ids_before))
    env["observed"]["cleanup"] = cleanup
    if result is not None:
        result.observed = dict(result.observed or {})
        result.observed.update(env["observed"])
        if cleanup.get("restored") is not True and result.verdict == Verdict.PASS:
            result.verdict = Verdict.FAIL
            result.reason = "teardown could not verify the profile list was restored"
    if cleanup.get("restored") is not True and ids_before is not None:
        # An unrestored (or unverifiable) profile list taints the run even when
        # the case itself is INCONCLUSIVE or raised.
        env["ctx"]["_tainted"] = True


# -- LCD-10 ----------------------------------------------------------------

def _judge_lcd10(opened: bool, cancel_kept_idle: bool, start_ran: bool, stop_dialog: bool,
                 stop_cancel_kept_running: bool, stopped: bool) -> CaseResult:
    obs = {"start_opens_confirm": opened, "cancel_kept_idle": cancel_kept_idle,
           "confirm_started": start_ran, "stop_opens_confirm": stop_dialog,
           "stop_cancel_kept_running": stop_cancel_kept_running, "stopped": stopped}
    steps = (("Start did not open the confirm dialog", opened),
             ("Cancel on the Start confirm still started or left the dialog", cancel_kept_idle),
             ("confirming Start did not start the firing", start_ran),
             ("Stop from home did not open a confirm dialog", stop_dialog),
             ("Cancel on the Stop confirm stopped the firing", stop_cancel_kept_running),
             ("confirming Stop did not stop the firing", stopped))
    for why, ok in steps:
        if not ok:
            return CaseResult(Verdict.FAIL, reason=why, observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_lcd10(ctx: dict) -> CaseResult:
    gate = _lcdwr_gate(ctx)
    if gate is not None:
        return gate
    if ctx.get("allow_heat") is not True:
        return CaseResult(Verdict.NOT_RUN, reason="allow_heat=False: LCD-10 starts a real (seconds-long, low) firing")
    if ctx.get("lcd22_allow_heat") is not True:
        return CaseResult(
            Verdict.NOT_RUN,
            reason="lcd_edit_heat=False: LCD-10 starts a real low-temperature firing "
                   "and needs the separate lcd_edit_heat=True opt-in")
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx; no action taken")
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    env = _lcdwr_env(ctx)
    srv, ui, now, sleep, observed = env["srv"], env["ui"], env["now"], env["sleep"], env["observed"]
    state = _lcdwr_exec_state(srv)
    if state not in ("idle", "done", "faulted"):
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"executor not idle (state={state}); no action taken")
    ok, why = _heat._capability_preflight_ok(ctx)
    if not ok:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{why}; no action taken")
    temps = _heat._zone_temps(ctx)
    if not temps:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no valid thermo reading; no action taken")
    target = min(temps.values()) + 15.0
    if target > _LCD22_MAX_TARGET_C:
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"ambient+15 = {target:.1f} C exceeds the {_LCD22_MAX_TARGET_C:.0f} C case ceiling; no action taken")
    ceil_ok, ceil_why = _heat._check_zone_ceilings(ctx, 1, target)
    if not ceil_ok:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"{ceil_why}; no action taken")
    _wake_and_home(ctx)

    name = "LCD10_TMP"
    result: Optional[CaseResult] = None
    ids_before: Optional[set] = None
    slot: Optional[int] = None
    started = False
    try:
        slot, fail, ids_before = _lcdwr_save_tmp(env, name, target)
        if fail is not None:
            result = fail
            return result
        fail = _lcdwr_open_picker(env)
        if fail is not None:
            result = fail
            return result
        row = _lcdwr_find_row(env, name)
        if row is None:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"row {name!r} not found on the picker")
            return result
        ui.click_by_name(row["name"])
        if not _wait_for_page(ui, "profile_detail"):
            result = CaseResult(Verdict.INCONCLUSIVE, reason="profile row did not open the detail page")
            return result

        def names_with(*want):
            return _lcd25_wait_names(env, lambda n: all(w in n for w in want), _LCDWR_NAMES_WAIT_S)

        click = ui.click_by_name("Start")
        observed["start_click"] = click.get("result")
        n1, t1 = names_with("Start", "Cancel")
        opened = n1 is not None and not t1 and click.get("result") in ("ok", "verdict_unknown")
        if not opened:
            result = _judge_lcd10(False, True, False, False, True, False)
            return result
        ui.click_by_name("Cancel")
        sleep(1.0)
        n2, _t2 = _lcd25_names(ui)
        cancel_ok = _lcdwr_exec_state(srv) in ("idle", "done", "faulted") and n2 is not None and "Cancel" not in n2
        if not cancel_ok:
            result = _judge_lcd10(True, False, False, False, True, False)
            return result
        # Confirm path: from here a firing may exist; the finally stops it.
        started = True
        ui.click_by_name("Start")
        names_with("Start", "Cancel")
        ui.click_by_name("Start")
        deadline = now() + _LCDWR_RUN_WAIT_S
        while _lcdwr_exec_state(srv) != "running" and now() < deadline:
            sleep(0.5)
        start_ran = _lcdwr_exec_state(srv) == "running"
        observed["running_seen"] = start_ran
        if start_ran:
            ex = _lcd22_exec_dict(srv)
            observed["running_profile_id"] = ex.get("profile_id") if ex else None
            if ex is None or ex.get("profile_id") != slot:
                # A different profile is firing: stop at once (the finally also
                # stops it) and never report PASS.
                try:
                    srv._profiles.stop()
                except Exception:  # noqa: BLE001
                    pass
                result = CaseResult(
                    Verdict.FAIL, observed=dict(observed),
                    reason=f"executor started profile {observed['running_profile_id']!r}, not this case's slot {slot}")
                return result
        stop_dialog = stop_cancel_kept = stopped = False
        if start_ran:
            observed["navigate_home"] = _navigate_home(ui)
            observed["home_settled"] = _wait_for_home_settled(
                ctx, ui, "start", min_wait_s=0.0, timeout_s=_LCDWR_NAMES_WAIT_S, log=[])
            ui.click_by_name("start")
            ns, ts = names_with("Stop", "Cancel")
            stop_dialog = ns is not None and not ts
            if stop_dialog:
                ui.click_by_name("Cancel")
                sleep(1.0)
                stop_cancel_kept = _lcdwr_exec_state(srv) == "running"
                if stop_cancel_kept:
                    ui.click_by_name("start")
                    names_with("Stop", "Cancel")
                    ui.click_by_name("Stop")
                    deadline = now() + _LCDWR_RUN_WAIT_S
                    while _lcdwr_exec_state(srv) not in ("idle", "done") and now() < deadline:
                        sleep(0.5)
                    stopped = _lcdwr_exec_state(srv) in ("idle", "done")
        result = _judge_lcd10(True, True, start_ran, stop_dialog, stop_cancel_kept, stopped)
        return result
    finally:
        # Heat is stopped FIRST, and again on every poll while the executor is in
        # ANY non-terminal state (running, paused, unreadable). Then K4 and every
        # zone relay are bounded-polled to off.
        stop_err = None
        verified = False
        final_state = None
        energized: Optional[bool] = None
        zone_relays: "Optional[dict]" = None
        deadline = now() + 15.0
        while True:
            final_state = _lcdwr_exec_state(srv)
            if final_state not in ("idle", "done", "faulted"):
                try:
                    srv._profiles.stop()
                except Exception as exc:  # noqa: BLE001
                    stop_err = type(exc).__name__
            else:
                energized = _heat._read_energized(ctx)
                zone_relays = _lcd22_zone_relays(ctx, _heat)
                if energized is False and zone_relays is not None and not any(zone_relays.values()):
                    verified = True
                    break
            if now() >= deadline:
                break
            sleep(0.5)
        observed["final_exec_state"] = final_state
        observed["final_energized"] = energized
        observed["final_zone_relays"] = zone_relays
        if stop_err:
            observed["stop_error"] = stop_err
        _lcdwr_finish(env, result, ids_before)
        if not verified:
            ctx["_tainted"] = True
            if result is None:
                result = CaseResult(Verdict.FAIL, observed=dict(observed), reason="")
            result.verdict = Verdict.FAIL
            result.reason = (f"cleanup not verified: executor={final_state!r}, energized={energized!r}, "
                             f"zone_relays={zone_relays!r}; the firing may still be running")


# -- LCD-11 ----------------------------------------------------------------

def _judge_lcd11(armed_label: bool, first_tap_kept: bool, stale_kept: bool, second_tap_deleted: bool) -> CaseResult:
    obs = {"armed_label_seen": armed_label, "first_tap_kept_profile": first_tap_kept,
           "stale_tap_kept_profile": stale_kept, "second_tap_deleted": second_tap_deleted}
    if not first_tap_kept:
        return CaseResult(Verdict.FAIL, reason="a single Delete tap deleted the profile", observed=obs)
    if not armed_label:
        return CaseResult(Verdict.FAIL, reason="first Delete tap did not relabel the button 'Confirm?'", observed=obs)
    if not stale_kept:
        return CaseResult(Verdict.FAIL, reason="a tap after the 5 s window lapsed deleted the profile or left the button on 'Confirm?'", observed=obs)
    if not second_tap_deleted:
        return CaseResult(Verdict.FAIL, reason="the second tap within 5 s did not delete the profile", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _lcdwr_delete_btn(targets: "list[dict]", row_cy: float, labels=("Delete",)) -> Optional[dict]:
    cands = [t for t in targets if t.get("name") in labels and not t.get("hidden")
             and abs(float(t.get("cy", -1000)) - row_cy) <= 20]
    return cands[0] if cands else None


def _case_lcd11(ctx: dict) -> CaseResult:
    gate = _lcdwr_gate(ctx)
    if gate is not None:
        return gate
    env = _lcdwr_env(ctx)
    srv, ui, sleep, observed = env["srv"], env["ui"], env["sleep"], env["observed"]
    touch = getattr(srv, "_touch", None)
    if touch is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no touch client; no action taken")
    _wake_and_home(ctx)
    name = "LCD11_TMP"
    result: Optional[CaseResult] = None
    ids_before: Optional[set] = None
    try:
        slot, fail, ids_before = _lcdwr_save_tmp(env, name, 50.0)
        if fail is not None:
            result = fail
            return result
        fail = _lcdwr_open_picker(env)
        if fail is not None:
            result = fail
            return result
        row = _lcdwr_find_row(env, name)
        if row is None:
            result = CaseResult(Verdict.INCONCLUSIVE, reason=f"row {name!r} not found on the picker")
            return result
        def exists() -> Optional[bool]:
            try:
                return slot in {p.id for p in srv._profiles.list_all()}
            except Exception:  # noqa: BLE001
                return None

        def resolve(labels=("Delete", "Confirm?")) -> "Optional[dict]":
            """Re-read the list and return the button on THIS case's own row
            (found by name, never by a remembered coordinate), else None."""
            t2, _ = _list_tap_targets_resolving_busy(ui)
            tg = t2.get("targets", [])
            r2 = next((r for r in _profile_rows_by_position(tg) if r.get("name") == name), None)
            if r2 is None:
                return None
            return _lcdwr_delete_btn(tg, float(r2["cy"]), labels=labels)

        abort = {"why": None}

        def tap_own() -> bool:
            """One tap on the case's own Delete/Confirm? button, only while the
            profile is still observed present and its row/button resolves."""
            if exists() is not True:
                abort["why"] = abort["why"] or "profile no longer observed present"
                return False
            b = resolve()
            if b is None:
                abort["why"] = "own row/Delete button not resolvable"
                return False
            _lcd13_tap(touch, b)
            return True

        if resolve(labels=("Delete",)) is None:
            result = CaseResult(Verdict.INCONCLUSIVE, reason="no Delete button on the transient row")
            return result
        armed_label = first_kept = stale_kept = deleted = False
        if tap_own():  # arm
            armed_label = resolve(labels=("Confirm?",)) is not None
            first_kept = exists() is True
            if first_kept:
                sleep(_LCDWR_STALE_WAIT_S)  # window lapses; the stale tap must only disarm
                if tap_own():
                    # The stale tap must only disarm: profile still there AND the
                    # button reverted to "Delete" (not left on "Confirm?").
                    stale_kept = exists() is True and resolve(labels=("Delete",)) is not None
                    if stale_kept:
                        sleep(_LCDWR_DELETE_DEBOUNCE_S)
                        if tap_own():  # arm again
                            sleep(_LCDWR_DELETE_DEBOUNCE_S)
                            if tap_own():  # confirm within the window
                                sleep(1.0)
                                deleted = exists() is False
        observed["tap_abort"] = abort["why"]
        if abort["why"] is not None and exists() is True:
            # Could not resolve our own row while it still exists: no verdict.
            result = CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                                reason=f"stopped tapping: {abort['why']}")
            return result
        result = _judge_lcd11(armed_label, first_kept, stale_kept, deleted)
        return result
    finally:
        _lcdwr_finish(env, result, ids_before)


# -- LCD-12 ----------------------------------------------------------------

_LCD12_TARGET_DIGITS = "80"


def _judge_lcd12(page_zones: bool, page_segment: bool, page_review: bool, free_cell: Optional[str],
                 saved: bool, target_ok: bool) -> CaseResult:
    obs = {"zones_page": page_zones, "segment_page": page_segment, "review_page": page_review,
           "free_cell": free_cell, "saved": saved, "saved_target_matches": target_ok}
    for why, ok in (("New did not open the zones step", page_zones),
                    ("Next did not open the segment step", page_segment),
                    ("Next did not open the review step", page_review),
                    ("review offered no free slot to save into", free_cell is not None),
                    ("the profile did not appear in the profile list after Save", saved),
                    ("the saved profile's target does not match the digits entered on the num pad", target_ok)):
        if not ok:
            return CaseResult(Verdict.FAIL, reason=why, observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _lcd12_step(env: dict, name: str, page: str) -> bool:
    ui = env["ui"]
    click = ui.click_by_name(name)
    env["observed"].setdefault("clicks", {})[name] = click.get("result")
    if click.get("result") not in ("ok", "verdict_unknown"):
        return False
    return bool(_wait_for_page(ui, page))


def _case_lcd12(ctx: dict) -> CaseResult:
    gate = _lcdwr_gate(ctx)
    if gate is not None:
        return gate
    env = _lcdwr_env(ctx)
    srv, ui, observed = env["srv"], env["ui"], env["observed"]
    touch = getattr(srv, "_touch", None)
    if touch is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no touch client; no action taken")
    _wake_and_home(ctx)
    result: Optional[CaseResult] = None
    try:
        ids_before = {p.id for p in srv._profiles.list_all()}
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"could not list profiles ({type(exc).__name__}); no action taken")
    try:
        fail = _lcdwr_open_picker(env)
        if fail is not None:
            result = fail
            return result
        tap, _b = _list_tap_targets_resolving_busy(ui)
        targets = tap.get("targets", [])
        back, home = _find(targets, "back"), _find(targets, "home")
        if not _profiles_topbar_icons(targets)["add"] or back is None or home is None:
            result = CaseResult(Verdict.INCONCLUSIVE, reason="New icon not located on the profiles topbar")
            return result
        pitch = float(home["cx"]) - float(back["cx"])
        _lcd13_tap(touch, {"cx": float(home["cx"]) + 3 * pitch, "cy": home["cy"]})
        page_zones = bool(_wait_for_page(ui, "profile_builder_zones"))
        if not page_zones:
            result = _judge_lcd12(False, False, False, None, False, False)
            return result
        zt, _b = _list_tap_targets_resolving_busy(ui)
        chip = next((t for t in zt.get("targets", []) if isinstance(t.get("name"), str)
                     and t["name"].lower().startswith("zone") and not t.get("hidden")), None)
        if chip is None:
            result = CaseResult(Verdict.INCONCLUSIVE, reason="no zone chip target on the zones step",
                                observed={"names": sorted(str(t.get("name")) for t in zt.get("targets", []))})
            return result
        ui.click_by_name(chip["name"])
        if not _lcd12_step(env, "Next", "profile_builder_segment"):
            result = _judge_lcd12(True, False, False, None, False, False)
            return result
        ui.click_by_name("Target C")
        for ch in _LCD12_TARGET_DIGITS:
            click = ui.click_by_name(ch)
            if click.get("result") not in ("ok", "verdict_unknown"):
                ui.click_by_name("Cancel")
                result = CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                                    reason=f"num pad key {ch!r} not tappable ({click.get('result')!r})")
                return result
        ui.click_by_name("Done")
        if not _lcd12_step(env, "Next", "profile_builder_review"):
            result = _judge_lcd12(True, True, False, None, False, False)
            return result
        ui.click_by_name("Save")
        names, trunc = _lcd25_wait_names(
            env, lambda n: any(_LCDWR_FREE_CELL_RE.match(str(x)) for x in n), _LCDWR_NAMES_WAIT_S)
        free = None
        from . import cases_heat as _heat_mod  # local import: avoids a module-load cycle
        if names is not None and not trunc:
            for x in sorted(str(x) for x in names):
                m = _LCDWR_FREE_CELL_RE.match(x)
                if m and int(m.group(1)) != _heat_mod.BENCH_PROFILE_SLOT_ID:
                    free = x
                    break
        observed["slot_names"] = sorted(str(x) for x in (names or []))
        if names is None or trunc:
            result = CaseResult(Verdict.INCONCLUSIVE, observed=observed,
                                reason="slot name list unreadable or truncated; no slot chosen, nothing saved")
            return result
        if free is None:
            result = _judge_lcd12(True, True, True, None, False, False)
            return result
        env["created"] = int(_LCDWR_FREE_CELL_RE.match(free).group(1))
        ui.click_by_name(free)
        env["sleep"](1.0)
        saved = env["created"] in ({p.id for p in srv._profiles.list_all()} - ids_before)
        target_ok = False
        if saved:
            detail = srv._profiles.get(env["created"])
            segs = _lcd25_segs(detail) if detail is not None else None
            target_ok = bool(segs) and len(segs) == 1 and segs[0][0] == float(_LCD12_TARGET_DIGITS)
        result = _judge_lcd12(True, True, True, free, saved, target_ok)
        return result
    finally:
        _lcdwr_finish(env, result, ids_before)


_CASE_FUNCS = {
    "LCD-20": _case_lcd20,
    "LCD-05": _case_lcd05,
    "LCD-06": _case_lcd06,
    "LCD-13": _case_lcd13,
    "LCD-01": _case_lcd01,
    "LCD-02": _case_lcd02,
    "LCD-03": _case_lcd03,
    "LCD-04": _case_lcd04,
    "LCD-08": _case_lcd08,
    "LCD-09": _case_lcd09,
    "LCD-14": _case_lcd14,
    "LCD-16": _case_lcd16,
    "LCD-19": _case_lcd19,
    "LCD-21": _case_lcd21,
    "LCD-22": _case_lcd22,
    "LCD-23": _case_lcd23,
    "LCD-24": _case_lcd24,
    "LCD-25": _case_lcd25,
    "LCD-26": _case_lcd26,
    "LCD-10": _case_lcd10,
    "LCD-11": _case_lcd11,
    "LCD-12": _case_lcd12,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn


# ---------------------------------------------------------------------------
# LCD-07 / LCD-15 / LCD-17 / LCD-18 -- read-only set: home rail pills,
# network page, touch-cal enter/back-out, topbar warning tier. None of these
# writes a setting or taps connect/forget/Start/complete-cal.
# Kept in its own block (own dict + loop) to stay rebase-friendly.
# ---------------------------------------------------------------------------

#: ui_page_home.c rail: right quarter (x 360..480), 4px pad, "Relays" caption,
#: then 4 pills 24x18 with 4px gaps. Widget-space centres, derived from source
#: (an estimate: a pill sample that reads neither ON nor NEUTRAL is
#: INCONCLUSIVE, never a colour FAIL, so a small geometry error cannot FAIL).
_LCD07_PILL_CX = (376, 404, 432, 460)
_LCD07_PILL_CY = 75
_NEUTRAL_RGB = (0x9A, 0xA0, 0xAE)  # UI_THEME_COLOR_NEUTRAL == TEXT_SECONDARY
_LCD18_WARN_DX = -40  # warning slot sits one icon (36 px) + 4 px gap left of the gear


def _judge_lcd07(page, states, running: bool) -> CaseResult:
    """states: per-pill 'on' | 'neutral' | 'other' | None (unsampled)."""
    obs = {"page": page, "pill_states": list(states), "running": running}
    if page != "home":
        return CaseResult(Verdict.FAIL, reason=f"expected page 'home', got {page!r}", observed=obs)
    if any(s is None for s in states):
        return CaseResult(Verdict.INCONCLUSIVE, reason="a rail pill could not be sampled", observed=obs)
    if any(s == "other" for s in states):
        return CaseResult(Verdict.INCONCLUSIVE, reason="a rail pill reads neither ON nor NEUTRAL (rail geometry or camera cast); not judged as FAIL", observed=obs)
    if running:
        if states[0] == "on":
            return CaseResult(Verdict.PASS, observed=obs)
        if states[0] == "neutral":
            return CaseResult(Verdict.INCONCLUSIVE, reason="zone 0 relay pill OFF at sample time (PWM off-window); not a defect by itself", observed=obs)
    if not running and all(s == "neutral" for s in states):
        return CaseResult(Verdict.PASS, observed=obs)
    return CaseResult(Verdict.FAIL, reason="idle rail pills are not all NEUTRAL" if not running else "unexpected pill state", observed=obs)


def _case_lcd07(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    page = ui.get_current_page()
    if page != "home":
        return _judge_lcd07(page, [None] * 4, False)
    try:
        running = srv._profiles.get_exec_status().state_name == "running"
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason=f"executor state unreadable ({type(exc).__name__}); cannot tell idle from running")
    image_path = _capture(ctx, "lcd07_rail.jpg")
    if not image_path:
        return CaseResult(Verdict.INCONCLUSIVE, reason="camera capture failed")
    states = []
    for cx in _LCD07_PILL_CX:
        try:
            s = lcd_sampler.sample_widget(image_path, cx, _LCD07_PILL_CY, repo_root=ctx.get("repo_root"))
        except lcd_sampler.LcdCaptureError:
            states.append(None)
            continue
        if s.bezel is None:
            states.append(None)
        elif lcd_sampler.matches_color(s.region, _ACCENT_4_RGB, s.bezel):
            states.append("on")
        elif lcd_sampler.matches_color(s.region, _NEUTRAL_RGB, s.bezel):
            states.append("neutral")
        else:
            states.append("other")
    # Pills for aux (spare-relay) outputs may be on while the executor is idle;
    # ctx["lcd07_aux_pills"] lists their 0-based indices and they are excluded
    # from the all-off check.
    aux = set(ctx.get("lcd07_aux_pills") or ())
    judged_states = [("neutral" if (i in aux and st == "on") else st) for i, st in enumerate(states)]
    result = _judge_lcd07(page, judged_states, running)
    if aux:
        result.observed = dict(result.observed or {}, aux_pills_excluded=sorted(aux), raw_pill_states=list(states))
    result.evidence = [image_path]
    return _downgrade_if_corners_stale(ctx, result, image_path)


def _judge_lcd15(page, has_manage: bool, manage_page, saved_ssids, targets_names, wifi) -> CaseResult:
    obs = {"page": page, "manage_target": has_manage, "manage_page": manage_page,
           "saved_ssids": sorted(saved_ssids), "wifi": wifi,
           "status_text_check": "not verifiable (status label is not a tap target; no OCR)"}
    if page != "network":
        return CaseResult(Verdict.FAIL, reason=f"expected page 'network', got {page!r}", observed=obs)
    if not has_manage:
        return CaseResult(Verdict.FAIL, reason="'Manage networks' target missing on network page", observed=obs)
    if manage_page != "network_manage":
        return CaseResult(Verdict.FAIL, reason=f"Manage networks led to {manage_page!r}, not 'network_manage'", observed=obs)
    if not saved_ssids:
        return CaseResult(Verdict.INCONCLUSIVE, reason="board reports no saved networks to look for", observed=obs)
    # The firmware labels rows "<ssid>  (connected)" for the active network
    # (ui_page_network_manage.c): match on the SSID prefix, not exact equality.
    def _norm(n):
        n = str(n).rstrip()
        return n[:-len("(connected)")].rstrip() if n.endswith("(connected)") else n
    if not (set(saved_ssids) & {_norm(n) for n in targets_names}):
        return CaseResult(Verdict.FAIL, reason="no saved SSID appears on the network_manage page", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_lcd15(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        nets, _ = srv._wifi.get_networks()
        saved = {n.ssid for n in nets if getattr(n, "saved", True) and n.ssid}
        st = srv._wifi.get_status()
        wifi = {"mode": st.mode_name, "state": st.state_name}
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"wifi status unreadable: {type(exc).__name__}")
    try:
        fail = _click_then_page(ui, "settings", "config")[0] or _click_then_page(ui, "Network / Wi-Fi", "network")[0]
        if fail is not None:
            return fail
        page = ui.get_current_page()
        tap, _b = _list_tap_targets_resolving_busy(ui)
        has_manage = _find(tap.get("targets", []), "Manage networks") is not None
        manage_page, names = None, []
        if has_manage:
            # Navigation only: no connect/forget target is ever clicked.
            _click_then_page(ui, "Manage networks", "network_manage")
            manage_page = ui.get_current_page()
            tap2, _b = _list_tap_targets_resolving_busy(ui)
            names = [t.get("name") for t in tap2.get("targets", []) if t.get("name")]
        return _judge_lcd15(page, has_manage, manage_page, saved, names, wifi)
    finally:
        _navigate_home(ui)


def _judge_lcd17(has_tile: bool, entered_page, after_page) -> CaseResult:
    obs = {"cal_tile": has_tile, "entered_page": entered_page, "page_after_backout": after_page,
           "touch_test": "not reachable without completing a calibration (kiln_ui.c); not exercised"}
    if not has_tile:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no Touch Calibration tile (self-calibrating controller); nothing to enter", observed=obs)
    if entered_page != "touch_cal":
        return CaseResult(Verdict.FAIL, reason=f"Touch Calibration tile led to {entered_page!r}", observed=obs)
    if after_page in ("touch_cal", "touch_test") or after_page is None:
        return CaseResult(Verdict.FAIL, reason=f"backing out of touch_cal left the board on {after_page!r}", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_lcd17(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        fail = _click_then_page(ui, "settings", "config")[0]
        if fail is not None:
            return fail
        tap, _b = _list_tap_targets_resolving_busy(ui)
        if _find(tap.get("targets", []), "Touch Calibration") is None:
            return _judge_lcd17(False, None, None)
        ui.click_by_name("Touch Calibration")
        _wait_for_page(ui, "touch_cal")
        entered = ui.get_current_page()
        after = entered
        if entered == "touch_cal":
            # Back out WITHOUT completing: never tap the calibration dots.
            tap2, _b = _list_tap_targets_resolving_busy(ui)
            names = {t.get("name") for t in tap2.get("targets", [])}
            exit_name = "Cancel" if "Cancel" in names else ("Back" if "Back" in names else None)
            if exit_name is not None:
                ui.click_by_name(exit_name)
                _wait_for_page_change(ui, "touch_cal")
            after = ui.get_current_page()
        return _judge_lcd17(True, entered, after)
    finally:
        _navigate_home(ui)


def _judge_lcd18(tier, warn_is_error, warn_is_warn) -> CaseResult:
    """tier: the board's relay_life_tier ("none" | "warn" | "error" | None).
    The topbar warning is driven ONLY by relay_cycles_max_budget_tier()
    (ui_page_home_refresh.c), never by a safety trip."""
    obs = {"relay_life_tier": tier, "warning_matches_accent5": warn_is_error, "warning_matches_accent1": warn_is_warn}
    if tier not in ("warn", "error"):
        return CaseResult(Verdict.NOT_RUN, reason=f"relay_life_tier is {tier!r}: no relay at >=80% of rated life, so no "
                                                  "topbar warning is expected to be shown", observed=obs)
    if warn_is_error is None or warn_is_warn is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="topbar warning region could not be sampled", observed=obs)
    want_err = tier == "error"
    if (warn_is_error if want_err else warn_is_warn):
        return CaseResult(Verdict.PASS, observed=obs)
    if warn_is_error or warn_is_warn:
        return CaseResult(Verdict.FAIL, reason=f"topbar warning shows the wrong tier for relay_life_tier={tier!r}", observed=obs)
    return CaseResult(Verdict.INCONCLUSIVE, reason="no warning indicator visible at the sampled slot (geometry or camera); not judged as FAIL", observed=obs)


def _lcd18_relay_life_tier(ctx: dict) -> Optional[str]:
    from . import cases_heat as _heat  # local import: avoids a module-load cycle
    host = ctx.get("host")
    if not host:
        return None
    get_json = ctx.get("_http_get_json", _heat._http_get_json)
    try:
        status, body = get_json(host, "/api/status")
    except Exception:  # noqa: BLE001
        return None
    if status != 200 or not isinstance(body, dict):
        return None
    t = body.get("relay_life_tier")
    return t if isinstance(t, str) else None


def _case_lcd18(ctx: dict) -> CaseResult:
    tier = _lcd18_relay_life_tier(ctx)
    if tier is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="relay_life_tier unreadable from /api/status; nothing judged")
    if tier not in ("warn", "error"):
        return _judge_lcd18(tier, None, None)
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    if ui.get_current_page() != "home":
        return CaseResult(Verdict.INCONCLUSIVE, reason="not on home; topbar region not located")
    gear = _find(ui.list_tap_targets().get("targets", []), "settings")
    if gear is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="topbar gear not found to anchor the warning slot")
    image_path = _capture(ctx, "lcd18_topbar.jpg")
    if not image_path:
        return CaseResult(Verdict.INCONCLUSIVE, reason="camera capture failed")
    anchor = dict(gear, cx=gear["cx"] + _LCD18_WARN_DX)
    is_err = _sample_button_bool(ctx, image_path, anchor, _ACCENT_5_RGB)
    is_warn = _sample_button_bool(ctx, image_path, anchor, _ACCENT_1_RGB)
    result = _judge_lcd18(tier, is_err, is_warn)
    result.evidence = [image_path]
    return _downgrade_if_corners_stale(ctx, result, image_path)


_CASE_FUNCS_RO = {
    "LCD-07": _case_lcd07,
    "LCD-15": _case_lcd15,
    "LCD-17": _case_lcd17,
    "LCD-18": _case_lcd18,
}

for _cid, _fn in _CASE_FUNCS_RO.items():
    get_case(_cid).judge = _fn
