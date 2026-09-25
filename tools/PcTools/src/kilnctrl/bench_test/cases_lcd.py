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

import os
import tempfile
import time
from typing import Any, Dict, Optional

from . import judgments as J
from . import lcd_sampler
from .registry import CaseResult, Verdict, get_case
from ..protocol import THERMO_CHANNEL_ALL
from ..devices_touch import TOUCH_POWER_STATE_ERROR_HOLD, TOUCH_POWER_STATE_ON

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


def _click_resolving_swallow(ui, name: str, max_swallow_retries: "Optional[int]" = None) -> "tuple[dict, int]":
    """click_by_name(), re-clicking immediately while the result reads
    'swallowed' (bounded by `max_swallow_retries`) -- a swallow is a
    directly observable, expected race (screen_idle ate the wake/dismiss
    tap), not the "maybe stuck" ambiguity the caller's own page-poll retry
    budget exists for, so it must not consume that separate budget.

    'verdict_unknown' (kiln_ui_click_by_name()'s own bounded wait for the
    swallow verdict timed out) is deliberately NOT re-clicked here: unlike a
    confirmed swallow, the press may well have reached the widget (the usual
    cause is a slow LVGL flush, which delays the verdict, not the press), so
    an immediate blind re-click could land on the page that press already
    opened. It is returned as-is for the caller to resolve against the
    observable page/target change, whose own retry is guarded by an
    unchanged page. Returns ``(final_click, swallow_retries_used)``.

    `max_swallow_retries` is resolved at call time against
    `_CLICK_THEN_PAGE_SWALLOW_RETRIES` -- see :func:`_wait_for_page`'s
    docstring for why a bare default would defeat a test's patch."""
    if max_swallow_retries is None:
        max_swallow_retries = _CLICK_THEN_PAGE_SWALLOW_RETRIES
    click = ui.click_by_name(name)
    retries = 0
    while click.get("result") == "swallowed" and retries < max_swallow_retries:
        retries += 1
        click = ui.click_by_name(name)
    return click, retries


def _click_then_page(ui, name: str, expected_page: str,
                      timeout_s: "Optional[float]" = None,
                      max_retries: "Optional[int]" = None) -> "tuple[Optional[CaseResult], str, float, int]":
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

    Returns ``(None, page, waited_s, swallow_retries)`` on success (click
    replied 'ok' AND the page arrived within `timeout_s`) -- `swallow_retries`
    is the number of 'swallowed' re-clicks it took to reach that passing
    click (0 when the very first click was not swallowed), so a swallow
    resolved on the way to a PASS is recorded rather than discarded once the
    hop succeeds. A 'verdict_unknown' click is never itself a pass: it is
    judged only by whether the expected page then arrives (the same
    evidence an 'ok' click is judged by), and is never blind re-clicked
    unless the page provably stayed where it was. On any failure, the first
    element is a ready-to-return FAIL :class:`CaseResult` -- naming the
    click's own result when the click itself failed, or the page the board
    is actually on (plus the blanked-screen hint, since a swallowed wake tap
    reads identically to this) when the click said 'ok' but the page never
    changed; `swallow_retries` is 0 in every failure case (the FAIL's own
    ``observed`` dict already carries whatever retry count applies).

    Worst case: ``(1 + max_retries) * (1 + _CLICK_THEN_PAGE_SWALLOW_RETRIES)``
    clicks (9 at the defaults, only when every click is 'swallowed') plus
    ``1 + max_retries`` full `timeout_s` page waits (~6 s at the 2 s
    default); a not_found click fails immediately with no wait at all.

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
    click, swallow_retries = _click_resolving_swallow(ui, name)
    if click.get("result") not in ("ok", "verdict_unknown"):
        # A swallow that outlasted _click_resolving_swallow()'s own budget is
        # attributed as such, never as "not_found" (the target WAS found).
        # 'inject_failed' (2026-09-24) is attributed distinctly too: no press
        # was ever sent (lvgl_port_inject_touch() itself refused), so this is
        # neither a missing/ambiguous/hidden target nor a swallow -- never a
        # pass, and never grounds to wait for a page change this click could
        # not have caused.
        if click.get("result") == "swallowed":
            immediate_attribution = "swallowed"
        elif click.get("result") == "inject_failed":
            immediate_attribution = "inject_failed"
        elif click.get("result") == "offscreen":
            # 2026-09-25: the target WAS found, but its reported centre lies
            # off the panel (KILN_UI_CLICK_OFFSCREEN) -- distinct from a
            # genuinely absent target, which "not_found" means.
            immediate_attribution = "offscreen"
        else:
            immediate_attribution = "not_found"
        return (
            CaseResult(
                Verdict.FAIL,
                reason=f"click_by_name({name!r}) returned {click.get('result')!r}",
                observed={
                    "click": click,
                    "attribution": immediate_attribution,
                    **({"swallow_retries": swallow_retries} if swallow_retries else {}),
                },
            ),
            "",
            0.0,
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
            last_click, retry_swallow_retries = _click_resolving_swallow(ui, name)
            total_swallow_retries += retry_swallow_retries
            if last_click.get("result") not in ("ok", "verdict_unknown"):
                break
            if last_click.get("result") == "ok":
                confirmed_ok_clicks += 1
            retry_page, retry_waited_s = _wait_for_page(ui, expected_page, timeout_s=timeout_s)
            page, waited_s = retry_page, waited_s + retry_waited_s
            if page == expected_page:
                return None, page, waited_s, total_swallow_retries
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
        )
    return None, page, waited_s, swallow_retries

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
    if click.get("result") not in ("ok", "verdict_unknown"):
        if click.get("result") == "inject_failed":
            attribution = "inject_failed"
        elif click.get("result") == "offscreen":
            # 2026-09-25: the target WAS found, but its reported centre lies
            # off the panel -- distinct from a genuinely absent target.
            attribution = "offscreen"
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
        color_debug["bg_reference"] = {
            "region_xy": _BG_REFERENCE_XY,
            "sampled_rgb": bg_sample.region,
            "bezel_rgb": bg_sample.bezel,
            "target_rgb": _BG_RGB,
            "chroma_offset": round(bg_offset, 4),
            "cast_threshold": lcd_sampler.CAST_CHROMA_THRESHOLD,
            "reads_as_bezel": bg_on_bezel,
            "cast_suspected": cast_suspected,
            "cast_channel": cast_channel,
            "cast_channel_crush_max": lcd_sampler.CAST_CHANNEL_CRUSH_MAX,
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
    try:
        # 2026-09-24 bench root cause: this case used to call
        # click_by_name("settings") directly with no retry, so a swallowed
        # wake tap (screen_idle's touch-swallow race, same shape as every
        # other _click_then_page() caller) failed here with no second
        # chance. Migrated to the shared retry helper.
        fail, page, waited_s, swallow_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        tap = ui.list_tap_targets()
        _remember_page_targets(ctx, "config", tap)
        result = J.judge_lcd_config_hub(page, tap.get("targets", []))
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
        if swallow_retries:
            result.observed["swallow_retries"] = swallow_retries
        image_path = _capture(ctx, "lcd08_config_hub.jpg")
        if image_path:
            result.evidence = list(result.evidence or []) + [image_path]
        return result
    finally:
        _navigate_home(ui)


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


def _navigate_home(ui) -> None:
    """Best-effort restore to the home page -- used in `finally` blocks so a
    case that navigates away never leaves the board parked on a config/
    profiles/diagnostics sub-page for whatever runs next."""
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
                _wait_for_page(ui, "home")
    except Exception:
        pass


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
    try:
        fail, _config_page, _, config_swallow_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        fail, page, waited_s, swallow_retries = _click_then_page(ui, "Profiles", "profiles")
        if fail is not None:
            return fail
        swallow_retries += config_swallow_retries
        tap = ui.list_tap_targets()
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
            row_fail, detail_page, row_waited_s, row_swallow_retries = _click_then_page(
                ui, rows[0]["name"], "profile_detail")
            swallow_retries += row_swallow_retries
            if row_fail is not None:
                row_fail.observed = dict(row_fail.observed or {})
                row_fail.observed["page_wait_s"] = round(waited_s + row_waited_s, 3)
                return row_fail
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
        return result
    finally:
        _navigate_home(ui)


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
    try:
        fail, _config_page, _, config_swallow_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        fail, page, waited_s, swallow_retries = _click_then_page(ui, "Temperature", "temperature")
        if fail is not None:
            return fail
        swallow_retries += config_swallow_retries
        tap = ui.list_tap_targets()
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
        if thermo_error is not None:
            result.observed["thermo_error"] = thermo_error
        if zones_count_error is not None:
            result.observed["zones_count_error"] = zones_count_error
        if image_path:
            result.evidence = list(result.evidence or []) + [image_path]
        return _downgrade_if_corners_stale(ctx, result, image_path)
    finally:
        _navigate_home(ui)


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
    next_cx = home_cx + 2 * pitch
    for t in targets:
        if not _is_glyph_name(t.get("name")) or t.get("hidden"):
            continue
        try:
            if (abs(float(t.get("cx", -1000)) - next_cx) <= pitch / 4
                    and abs(float(t.get("cy", -1000)) - home_cy) <= pitch / 4):
                return t
        except (TypeError, ValueError):
            continue
    return None


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
    try:
        fail, _config_page, _, config_swallow_retries = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        # The config -> diagnostics hop IS a real top-level page switch
        # (kiln_ui's own page registry), so it gets the same checked
        # click-then-page-wait as every other hop; sub-page transitions
        # below never change the page name at all (ui_page_diagnostics.c's
        # sub-tabs are its own internal s_pages[], see
        # _targets_signature's docstring), so those are tracked by a
        # richer tap-target signature instead.
        fail, _diag_page, _, diag_swallow_retries = _click_then_page(ui, "Diagnostics", "diagnostics")
        if fail is not None:
            return fail
        swallow_retries = config_swallow_retries + diag_swallow_retries
        first_tap = ui.list_tap_targets()
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
                return fail
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
        if swallow_retries:
            observed["swallow_retries"] = swallow_retries
        result.observed = observed
        if truncated_steps and pages_paged < expected_hops and result.verdict != Verdict.PASS:
            result.reason = (
                f"{result.reason} (LIST_TAP_TARGETS reply was truncated at sub-page step(s) "
                f"{truncated_steps} -- the topbar icons may have been cut from the list, "
                f"not missing from the screen)"
            )
        return result
    finally:
        _navigate_home(ui)


# ---------------------------------------------------------------------------
# LCD-19 -- PIN lock after lcd_timeout_min (observer of WEB-SEC-04's PIN
# configuration, owned by another agent/wave). Reads a defensively-named
# ctx["_lcd_pin"] dict; NOT_RUN if absent, since this wave does not own
# wiring that key -- and Stop-is-never-gated is checked without assuming any
# particular shape for the rest.
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


def _wait_stable_names(ui, timeout_s: Optional[float] = None,
                        interval_s: Optional[float] = None,
                        stable_reads: int = 2) -> "tuple[Optional[set], float, int, bool]":
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
    last read was, even a non-None one that never repeated).

    Returns ``(names, elapsed_s, empty_polls, truncated_seen)`` -- see
    :func:`_lcd19_poll_overlay`."""
    def _satisfied(n: Optional[set], history: "list[Optional[set]]") -> bool:
        if n is None:
            return False
        real = [x for x in history if x is not None]
        if len(real) < stable_reads:
            return False
        return all(x == real[-1] for x in real[-stable_reads:])

    return _lcd19_poll_overlay(ui, _satisfied, timeout_s, interval_s)


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
    digit_results = [d.get("result") for d in (entry.get("digit_results") or [])]
    ok_result = (entry.get("ok_result") or {}).get("result")
    return {"present": True, "digit_count": len(digit_results), "digit_results": digit_results, "ok_result": ok_result}


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


def _case_lcd19(ctx: dict) -> CaseResult:
    pin_cfg = ctx.get("_lcd_pin")
    if pin_cfg is None:
        return CaseResult(Verdict.NOT_RUN, reason="no PIN was configured in this session (ctx['_lcd_pin'] absent, owned by WEB-SEC-04)")
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
    from . import cases_web_rw as _web  # local import: avoids a module-load cycle with cases_web_rw

    client = _web._sec_client(ctx)
    status0, cfg0 = client.get_config()
    if status0 != 200 or cfg0 is None:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/auth/config failed (status={status0})", observed={"status": status0})
    orig = _web._policy_from_config(cfg0)

    keypad_raised = wrong_pin_refused = right_pin_started = stop_not_gated = None
    result: Optional[CaseResult] = None
    state: Dict[str, Any] = {"orig": orig}
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
            if not firing_active:
                baseline = _lcd19_overlay_names(ui)
                click = ui.click_by_name("Start")
                state["start_click_result"] = click.get("result")
                if click.get("result") == "ok":
                    names, _, empty_polls, truncated_seen = _wait_for_overlay_names(
                        ui, present=True, baseline=baseline)
                    state["after_start_click_names"] = sorted(names) if names is not None else None
                    if empty_polls:
                        state["after_start_click_empty_polls"] = empty_polls
                    if truncated_seen:
                        state["after_start_click_truncated"] = truncated_seen
                    # `names is None` (every read within the poll window came
                    # back empty/timeout) is left at `keypad_raised`'s
                    # initial `None` -- INCONCLUSIVE, not a fabricated
                    # "keypad didn't raise" False the same read failure would
                    # otherwise have produced.
                    if names is not None:
                        keypad_raised = "OK" in names and "Cancel" in names
                if keypad_raised:
                    wrong_pin = pin_cfg.get("wrong_pin")
                    right_pin = pin_cfg.get("right_pin")
                    if wrong_pin:
                        entry = ui.enter_pin(wrong_pin)
                        state["wrong_pin_entry"] = _entry_result_summary(entry)
                        if _entry_all_clicked_ok(entry):
                            # A wrong PIN resets digit entry but the keypad's
                            # own button set never changes -- debounce two
                            # stable reads rather than trusting one
                            # immediate (tautologically "OK present") read.
                            names, _, empty_polls, truncated_seen = _wait_stable_names(ui)
                            state["after_wrong_pin_names"] = sorted(names) if names is not None else None
                            if empty_polls:
                                state["after_wrong_pin_empty_polls"] = empty_polls
                            if truncated_seen:
                                state["after_wrong_pin_truncated"] = truncated_seen
                            wrong_pin_refused = names is not None and "OK" in names and "Cancel" in names
                        # else: leave wrong_pin_refused at None -- the PIN
                        # typed on the board wasn't actually the intended
                        # one, so no conclusion can be drawn from what
                        # follows.
                    if wrong_pin_refused and right_pin:
                        entry = ui.enter_pin(right_pin)
                        state["right_pin_entry"] = _entry_result_summary(entry)
                        if _entry_all_clicked_ok(entry):
                            # A correct PIN closes the keypad in favour of
                            # the Confirm Start dialog -- "OK" disappears.
                            names, _, empty_polls, truncated_seen = _wait_stable_names(ui)
                            state["after_right_pin_names"] = sorted(names) if names is not None else None
                            if empty_polls:
                                state["after_right_pin_empty_polls"] = empty_polls
                            if truncated_seen:
                                state["after_right_pin_truncated"] = truncated_seen
                            right_pin_started = (
                                names is not None and "OK" not in names and "Cancel" in names
                            )
            else:
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
                        if has_cancel and not has_ok:
                            stop_not_gated = True  # Confirm Stop shown directly, no PIN keypad
                        elif has_cancel and has_ok:
                            stop_not_gated = False  # PIN keypad appeared -- Stop was gated
                        # else: neither popup present -- leave None (INCONCLUSIVE)
            result = J.judge_lcd_pin_lock(keypad_raised, wrong_pin_refused, right_pin_started, stop_not_gated)
            result.observed = dict(result.observed or {})
            result.observed.update(state)
    finally:
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
        if result is None:
            result = CaseResult(Verdict.FAIL, reason="LCD-19 aborted before a verdict was reached", observed=dict(state))
        result.observed = dict(result.observed or {})
        result.observed["overlay_dismiss"] = overlay
        result.observed["restore"] = restore_state
        if not restore_post_ok or not restore_matches:
            # Unconditional override, same as WEB-SEC-03/04's own finally:
            # a failed policy restore is a hard FAIL regardless of what the
            # keypad verdict above found -- the board may be left with
            # lcd_enabled changed from what this run found.
            result.verdict = Verdict.FAIL
            result.reason = "lcd_enabled policy restore did not round-trip after LCD-19 -- board may be left with lcd_enabled changed"
        _navigate_home(ui)
    return result


_CASE_FUNCS = {
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
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
