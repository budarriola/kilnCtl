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


def _wake_and_home(ctx: dict) -> None:
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
    precondition failure."""
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    if touch is not None:
        need_wake = True
        try:
            state = touch.get_state()
            need_wake = (not state.screen_on) or state.idle_ms >= _WAKE_IDLE_MS_THRESHOLD
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
                deadline = time.monotonic() + _WAKE_SCREEN_ON_TIMEOUT_S
                while time.monotonic() < deadline:
                    try:
                        if touch.get_state().screen_on:
                            break
                    except Exception:
                        break
                    time.sleep(_WAKE_SCREEN_ON_POLL_S)
    ui = srv._ui_test
    _navigate_home(ui)


def _wait_for_page(ui, expected: str, timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                    interval_s: float = _PAGE_POLL_INTERVAL_S) -> "tuple[str, float]":
    """Poll ``ui.get_current_page()`` until it equals `expected` or
    `timeout_s` elapses. Returns ``(last_page_seen, waited_s)`` -- never
    raises, and never fabricates a match: a page that never arrives comes
    back as whatever the last poll actually saw, so a caller's existing
    ``page != expected`` check still fails the case honestly."""
    start = time.monotonic()
    page = ui.get_current_page()
    while page != expected:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        page = ui.get_current_page()
    return page, time.monotonic() - start


def _wait_for_page_change(ui, before: str, timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                           interval_s: float = _PAGE_POLL_INTERVAL_S) -> "tuple[str, float]":
    """Same race as :func:`_wait_for_page`, for a caller that does not know
    the destination page's name in advance (a page transition mediated by
    the destination page's own definition) -- poll until the page differs
    from `before` instead of matching a fixed target."""
    start = time.monotonic()
    page = ui.get_current_page()
    while page == before:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        page = ui.get_current_page()
    return page, time.monotonic() - start


def _wait_for_targets_change(ui, before_names: "set",
                              timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                              interval_s: float = _PAGE_POLL_INTERVAL_S) -> "tuple[dict, float]":
    """Same race as :func:`_wait_for_page`, for pages whose name never
    changes even though their content does -- ui_page_diagnostics.c's
    sub-tabs are internal to that one page (its own ``s_pages[]`` array,
    switched by ``show_page()``, never registered with kiln_ui.c's
    top-level page registry), so ``get_current_page()`` stays 'diagnostics'
    across every tab and cannot detect this transition. Poll
    ``list_tap_targets()`` instead, until its name set differs from
    `before_names` or `timeout_s` elapses."""
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


def _click_then_page(ui, name: str, expected_page: str,
                      timeout_s: float = _PAGE_POLL_TIMEOUT_S) -> "tuple[Optional[CaseResult], str, float]":
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

    Returns ``(None, page, waited_s)`` on success (click replied 'ok' AND
    the page arrived within `timeout_s`). On any failure, the first element
    is a ready-to-return FAIL :class:`CaseResult` -- naming the click's own
    result when the click itself failed, or the page the board is actually
    on (plus the blanked-screen hint, since a swallowed wake tap reads
    identically to this) when the click said 'ok' but the page never
    changed.

    Worst case: two clicks plus two full `timeout_s` page waits (~4 s at
    the 2 s default) when both the first click and its one retry are
    swallowed; a not_found click fails immediately with no wait at all."""
    try:
        page_before = ui.get_current_page()
    except Exception:
        page_before = None
    click = ui.click_by_name(name)
    if click.get("result") != "ok":
        return (
            CaseResult(
                Verdict.FAIL,
                reason=f"click_by_name({name!r}) returned {click.get('result')!r}",
                observed={"click": click, "attribution": "not_found"},
            ),
            "",
            0.0,
        )
    page, waited_s = _wait_for_page(ui, expected_page, timeout_s=timeout_s)
    if page != expected_page:
        # One retry: click_by_name() said 'ok' (the target existed and an
        # injection was issued) but the page never arrived within
        # timeout_s -- the documented screen_idle/touch-poll swallow race
        # this module's module docstring names, where the tap that landed
        # was the one that woke an already-reblanked panel rather than
        # actually reaching the widget underneath. A second click on an
        # awake panel (screen_on is already true post-wake, so this retry's
        # own tap cannot itself be a NEW wake edge) either recovers cleanly
        # or proves the hop is genuinely stuck -- retried once, not looped,
        # so a real defect still fails within a bounded time.
        #
        # Retried ONLY when the board is still on the exact page it was on
        # before the first click (the swallow shape: nothing happened). If
        # the page moved somewhere else (a late or wrong transition), a
        # second tap by the same name would land on a DIFFERENT page's
        # widget of that name -- never do that; fail without a second tap.
        # Every current caller clicks a pure navigation target (settings,
        # Profiles, Temperature, Diagnostics), so a double tap on the
        # unchanged source page is harmless; never route a Start/Stop/
        # Confirm/PIN-digit/toggle click through this helper.
        retry_click = None
        if page_before is not None and page == page_before:
            retry_click = ui.click_by_name(name)
            if retry_click.get("result") == "ok":
                retry_page, retry_waited_s = _wait_for_page(ui, expected_page, timeout_s=timeout_s)
                if retry_page == expected_page:
                    return None, retry_page, waited_s + retry_waited_s
                page, waited_s = retry_page, waited_s + retry_waited_s
        observed = {
            "click": click,
            "page_before": page_before,
            "page": page,
            "page_wait_s": round(waited_s, 3),
            "attribution": (
                "swallowed_or_wrong_page" if retry_click is not None
                else "wrong_page" if page_before is not None
                else "page_before_unreadable"
            ),
        }
        if retry_click is not None:
            observed["retry_click"] = retry_click
        return (
            CaseResult(
                Verdict.FAIL,
                reason=(
                    f"click_by_name({name!r}) returned 'ok' but page stayed {page!r}, "
                    f"expected {expected_page!r}"
                    + (" (retried once)" if retry_click is not None
                       else f" (page moved from {page_before!r}; not retried)")
                    + J.BLANKED_SCREEN_HINT
                ),
                observed=observed,
            ),
            page,
            waited_s,
        )
    return None, page, waited_s

def _click_then_targets_change(ui, name: str, prev_names: "set",
                                timeout_s: float = _PAGE_POLL_TIMEOUT_S) -> "tuple[Optional[CaseResult], Optional[dict], float, bool]":
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
    reading per-tab values off it would confirm the wrong tab."""
    click = ui.click_by_name(name)
    if click.get("result") != "ok":
        return (
            CaseResult(
                Verdict.FAIL,
                reason=f"click_by_name({name!r}) returned {click.get('result')!r}",
                observed={"click": click, "attribution": "not_found"},
            ),
            None,
            0.0,
            False,
        )
    tap, waited_s = _wait_for_targets_change(ui, prev_names, timeout_s=timeout_s)
    names = {t.get("name") for t in tap.get("targets", [])}
    if names == prev_names:
        retry_click = ui.click_by_name(name)
        if retry_click.get("result") == "ok":
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
#: test_bench_test_cases_lcd.py's ThemeMirrorDriftTest. Sampled at widget
#: (5, 5), inside scr's 8 px outer padding: on home that is under the
#: transparent top-left auth-reset corner zone (ui_page_home.c, bg_opa
#: TRANSP) and above the transparent topbar, so it renders as screen
#: background. It is only a few frame pixels from the panel edge, though,
#: so geometry drift can put it on the bezel -- which is why it is
#: diagnostic only (judge_lcd_home_idle) and never softens a verdict.
_BG_REFERENCE_XY = (5, 5)
_BG_RGB = (0x1A, 0x1F, 0x2B)


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
        if start is not None and not start.get("hidden"):
            sample = lcd_sampler.sample_widget(image_path, start["cx"], start["cy"], repo_root=ctx.get("repo_root"))
            if sample.bezel is not None:
                start_matches = lcd_sampler.matches_color(sample.region, _ACCENT_4_RGB, sample.bezel)
                color_debug["start"] = {
                    "region_xy": (start["cx"], start["cy"]),
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
                    "matches": start_matches,
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
        # Background reference sample (2026-09-24), DIAGNOSTIC ONLY: compare
        # a UI_THEME_COLOR_BG region's chromaticity to the theme value so a
        # Start-button FAIL can say whether the capture itself looks off.
        # judge_lcd_home_idle only uses this to annotate a FAIL reason; it
        # never changes the verdict (see the comment there for the capture
        # where this point landed on the bezel). A reference that reads as
        # bezel is reported as such, never as a color cast.
        bg_sample = lcd_sampler.sample_widget(image_path, *_BG_REFERENCE_XY, repo_root=ctx.get("repo_root"))
        bg_offset = lcd_sampler.chroma_offset(bg_sample.region, _BG_RGB)
        bg_on_bezel = (
            bg_sample.bezel is not None
            and lcd_sampler.color_distance(bg_sample.region, bg_sample.bezel) < lcd_sampler.MIN_BEZEL_CONTRAST
        )
        color_debug["bg_reference"] = {
            "region_xy": _BG_REFERENCE_XY,
            "sampled_rgb": bg_sample.region,
            "bezel_rgb": bg_sample.bezel,
            "target_rgb": _BG_RGB,
            "chroma_offset": round(bg_offset, 4),
            "cast_threshold": lcd_sampler.CAST_CHROMA_THRESHOLD,
            "reads_as_bezel": bg_on_bezel,
            "cast_suspected": (not bg_on_bezel) and bg_offset > lcd_sampler.CAST_CHROMA_THRESHOLD,
        }
    except lcd_sampler.LcdCaptureError:
        pass
    return start_matches, pause_hidden, color_debug


# ---------------------------------------------------------------------------
# LCD-01 -- home, idle.
# ---------------------------------------------------------------------------

def _case_lcd01(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    page = ui.get_current_page()
    tap = ui.list_tap_targets()
    targets = tap.get("targets", [])
    _remember_page_targets(ctx, "home", tap)

    if page != "home":
        # Wrong page entirely -- no point spending a webcam capture on it.
        return J.judge_lcd_home_idle(page, targets, None, None)
    start_matches_accent4, pause_is_hidden, color_debug = _try_capture_and_sample(ctx, targets)
    result = J.judge_lcd_home_idle(page, targets, start_matches_accent4, pause_is_hidden, color_debug=color_debug)
    return _downgrade_if_corners_stale(ctx, result, color_debug.get("capture_path"))


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
        fail, page, waited_s = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        tap = ui.list_tap_targets()
        _remember_page_targets(ctx, "config", tap)
        result = J.judge_lcd_config_hub(page, tap.get("targets", []))
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
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


def _downgrade_if_corners_stale(ctx: dict, result: CaseResult, image_path: Optional[str]) -> CaseResult:
    """Downgrade a FAIL to INCONCLUSIVE when lcd_sampler.frame_corners_look_stale()
    finds FRAME_CORNERS/DEFAULT_TRANSFORM landing on bezel instead of screen
    background at all four corner-check points -- the same symptom the
    2026-09-24 round-3 fix found and re-derived the corners for (see
    lcd_sampler.py's FRAME_CORNERS comment). Never touches a PASS, or an
    already-INCONCLUSIVE/NOT_RUN/SKIP verdict, and never masks a genuine
    color mismatch: only ALL FOUR corner points reading as bezel proves the
    *geometry*, not the color judgment, is at fault -- a single point
    reading as background leaves the original FAIL untouched."""
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
    return CaseResult(Verdict.INCONCLUSIVE, reason="frame corners stale", observed=observed, evidence=result.evidence)


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
    pause_matches_accent1 = _sample_widget_bool(ctx, image_path, pause, _ACCENT_1_RGB) if image_path else None
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


def _case_lcd09(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        fail, _config_page, _ = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        fail, page, waited_s = _click_then_page(ui, "Profiles", "profiles")
        if fail is not None:
            return fail
        tap = ui.list_tap_targets()
        targets = tap.get("targets", [])
        _remember_page_targets(ctx, "profiles", tap)
        rows = [t for t in targets if str(t.get("name", "")).startswith("profile_row_")]
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
            click3 = ui.click_by_name(rows[0]["name"])
            if click3.get("result") == "ok":
                detail_page, _ = _wait_for_page_change(ui, page)
        result = J.judge_lcd_profiles_picker(page, rows, paging_present, new_icon_present, detail_page)
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
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


def _case_lcd14(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        fail, _config_page, _ = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        fail, page, waited_s = _click_then_page(ui, "Temperature", "temperature")
        if fail is not None:
            return fail
        tap = ui.list_tap_targets()
        _remember_page_targets(ctx, "temperature", tap)
        thermo_error: Optional[str] = None
        try:
            # ThermoClient has no read_all(); read(THERMO_CHANNEL_ALL) is the
            # actual API (2026-09-24 bench root cause: the old
            # srv._thermo.read_all() call always raised AttributeError,
            # silently swallowed by a bare except, which mislabeled this as
            # "no zone rows reported" instead of a coding bug). Readings are
            # kept only as a sanity cross-check (finite values), never
            # compared pixel-value-to-number -- see this case's module
            # comment for why.
            readings = {
                int(r.channel): float(r.temperature_c)
                for r in srv._thermo.read(THERMO_CHANNEL_ALL)
                if r.valid
            }
        except Exception as exc:
            readings = {}
            thermo_error = f"{type(exc).__name__}: {exc}"
        expected_zones = max(len(readings), 3) if readings else 3
        zone_rows_rendered: Dict[int, Optional[bool]] = {}
        image_path = _capture(ctx, "lcd14_temperature.jpg")
        if image_path is not None:
            for zone in range(expected_zones):
                y = _LCD14_ZONE_ROW_Y0 + zone * _LCD14_ZONE_ROW_PITCH
                off = _sample_widget_off(ctx, image_path, {"cx": _LCD14_ROW_X, "cy": float(y)})
                zone_rows_rendered[zone] = None if off is None else (not off)
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
            expected_zones=expected_zones,
        )
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
        if thermo_error is not None:
            result.observed["thermo_error"] = thermo_error
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
                                        timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                                        interval_s: float = _PAGE_POLL_INTERVAL_S
                                        ) -> "tuple[dict, float]":
    """Same polling shape as :func:`_wait_for_targets_change`, but keyed on
    :func:`_targets_signature` rather than a bare name set -- see that
    function's docstring for why a name set is blind to a Prev/Next-only
    change."""
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
                                   timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                                   confirm: bool = True,
                                   ) -> "tuple[Optional[CaseResult], Optional[dict], float, bool, bool]":
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

    Returns ``(fail, tap, waited_s, changed, found_next)``. ``found_next``
    is False when Next could not be located at all (disabled/last page, or
    missing anchors) -- distinct from a located-but-swallowed tap, so a
    caller can tell "done paging" from "navigation broke"."""
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    tap = ui.list_tap_targets()
    targets = tap.get("targets", [])
    next_target = _diagnostics_next_target(targets)
    if next_target is None:
        return None, tap, 0.0, False, False
    if touch is None:
        return (
            CaseResult(Verdict.INCONCLUSIVE, reason="no touch-inject transport available for Next", observed={}),
            None, 0.0, False, True,
        )
    try:
        x, y = float(next_target["cx"]), float(next_target["cy"])
    except (KeyError, TypeError, ValueError):
        return (
            CaseResult(Verdict.FAIL, reason="located Next icon has no numeric cx/cy", observed={"next_target": next_target}),
            None, 0.0, False, True,
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
            None, 0.0, False, True,
        )
    if not confirm:
        tap2 = ui.list_tap_targets()
        return None, tap2, 0.0, True, True
    tap2, waited_s = _wait_for_targets_signature_change(ui, prev_sig, timeout_s=timeout_s)
    sig2 = _targets_signature(tap2.get("targets", []))
    if sig2 == prev_sig:
        if _tap():
            tap3, waited_s2 = _wait_for_targets_signature_change(ui, prev_sig, timeout_s=timeout_s)
            waited_s += waited_s2
            tap2 = tap3
    changed = _targets_signature(tap2.get("targets", [])) != prev_sig
    return None, tap2, waited_s, changed, True


def _case_lcd16(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    pages_paged = 0
    crash_report_step: Optional[int] = None
    relay_life_has_reset: Optional[bool] = None
    next_disabled_at_end: Optional[bool] = None
    expected_hops = J.DIAGNOSTICS_PAGE_COUNT - 1
    try:
        fail, _config_page, _ = _click_then_page(ui, "settings", "config")
        if fail is not None:
            return fail
        # The config -> diagnostics hop IS a real top-level page switch
        # (kiln_ui's own page registry), so it gets the same checked
        # click-then-page-wait as every other hop; sub-page transitions
        # below never change the page name at all (ui_page_diagnostics.c's
        # sub-tabs are its own internal s_pages[], see
        # _targets_signature's docstring), so those are tracked by a
        # richer tap-target signature instead.
        fail, _diag_page, _ = _click_then_page(ui, "Diagnostics", "diagnostics")
        if fail is not None:
            return fail
        targets = ui.list_tap_targets().get("targets", [])
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
            fail, tap, _, changed, found_next = _tap_next_then_targets_change(
                ctx, ui, prev_sig, confirm=is_boundary_hop)
            if fail is not None:
                return fail
            if not found_next:
                # Next disappeared before reaching the last page -- paging
                # broke; stop here rather than looping with nothing to tap.
                break
            if not changed and is_boundary_hop:
                break
            pages_paged += 1
            targets = tap.get("targets", []) if tap else targets
            prev_sig = _targets_signature(targets)
        return J.judge_lcd_diagnostics_pages(
            pages_paged, expected_hops, next_disabled_at_end,
            crash_report_step, relay_life_has_reset,
        )
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

def _lcd19_overlay_names(ui) -> Optional[set]:
    try:
        targets = ui.list_tap_targets().get("targets", [])
    except Exception:
        return None
    return {t.get("name") for t in targets if not t.get("hidden")}


def _wait_for_overlay_names(ui, present: bool, timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                             interval_s: float = _PAGE_POLL_INTERVAL_S,
                             baseline: Optional[set] = None) -> "tuple[Optional[set], float]":
    """Same click-then-read race as :func:`_wait_for_page`, for LCD-19's
    top-layer popups: both the PIN keypad and the Confirm Start/Confirm
    Stop dialogs are lv_msgbox popups raised asynchronously off the same
    LVGL-task touch poll as any page switch (see kiln_ui_click_by_name()'s
    comment), so an immediate tap-target read after Start/Stop/Cancel can
    still see the pre-click set.

    `present=False` (after dismissing a popup via Cancel) waits for an
    empty set, as before.

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

    Never raises; a popup that never (dis)appears, or a set that never
    diverges from `baseline`, is still reported honestly via whatever the
    last poll saw."""
    start = time.monotonic()
    names = _lcd19_overlay_names(ui)

    def _satisfied(n: Optional[set]) -> bool:
        if n is None:
            return True  # can't poll further -- stop and report honestly
        if present and baseline is not None:
            return n != baseline
        return bool(n) == present

    while not _satisfied(names):
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        names = _lcd19_overlay_names(ui)
    return names, time.monotonic() - start


def _wait_stable_names(ui, timeout_s: float = _PAGE_POLL_TIMEOUT_S,
                        interval_s: float = _PAGE_POLL_INTERVAL_S,
                        stable_reads: int = 2) -> "tuple[Optional[set], float]":
    """Poll :func:`_lcd19_overlay_names` until the same set is read
    ``stable_reads`` times in a row, or `timeout_s` elapses. Unlike
    :func:`_wait_for_overlay_names`, this does not wait for a *specific*
    target state -- it debounces the click-then-read race for a submit
    whose expected good outcome is often "no visible change" (a wrong PIN
    resets digit entry but the keypad's own button set -- digits, "OK",
    "Cancel" -- never changes while it stays open, so there is no
    "OK"-appears/disappears signal to wait for the way there is for a page
    switch). Reading the name set exactly once right after the click would
    trivially "confirm" the pre-submission state, which was the bug this
    replaces: two matching reads spaced by `interval_s` at least rule out
    catching a genuinely in-flight LVGL transition. Never raises; a set
    that never stabilizes within `timeout_s` is still returned honestly."""
    start = time.monotonic()
    last = _lcd19_overlay_names(ui)
    count = 1
    while count < stable_reads:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        cur = _lcd19_overlay_names(ui)
        if cur == last:
            count += 1
        else:
            last = cur
            count = 1
    return last, time.monotonic() - start


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


def _dismiss_lcd19_overlay(ui) -> Dict[str, Any]:
    """Best-effort: dismiss a PIN keypad or Confirm Start/Confirm Stop dialog
    left open by this case, via "Cancel" only -- never the dialog's own
    confirm button. Returns a dict recording what was found and whether the
    dismiss actually took (verified by re-listing tap targets), for
    inclusion in the case's `observed`."""
    before = _lcd19_overlay_names(ui)
    if before is None:
        return {"checked": False}
    if "Cancel" not in before:
        return {"checked": True, "present": False}
    click = ui.click_by_name("Cancel")
    after, _ = _wait_for_overlay_names(ui, present=False)
    dismissed = after is not None and "Cancel" not in after
    return {
        "checked": True,
        "present": True,
        "cancel_click_result": click.get("result"),
        "dismissed": dismissed,
    }


def _case_lcd19(ctx: dict) -> CaseResult:
    pin_cfg = ctx.get("_lcd_pin")
    if pin_cfg is None:
        return CaseResult(Verdict.NOT_RUN, reason="no PIN was configured in this session (ctx['_lcd_pin'] absent, owned by WEB-SEC-04)")
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
                if click.get("result") == "ok":
                    names, _ = _wait_for_overlay_names(ui, present=True, baseline=baseline)
                    state["after_start_click_names"] = sorted(names) if names is not None else None
                    keypad_raised = names is not None and "OK" in names and "Cancel" in names
                if keypad_raised:
                    wrong_pin = pin_cfg.get("wrong_pin")
                    right_pin = pin_cfg.get("right_pin")
                    if wrong_pin:
                        entry = ui.enter_pin(wrong_pin)
                        if _entry_all_clicked_ok(entry):
                            # A wrong PIN resets digit entry but the keypad's
                            # own button set never changes -- debounce two
                            # stable reads rather than trusting one
                            # immediate (tautologically "OK present") read.
                            names, _ = _wait_stable_names(ui)
                            wrong_pin_refused = names is not None and "OK" in names and "Cancel" in names
                        # else: leave wrong_pin_refused at None -- the PIN
                        # typed on the board wasn't actually the intended
                        # one, so no conclusion can be drawn from what
                        # follows.
                    if wrong_pin_refused and right_pin:
                        entry = ui.enter_pin(right_pin)
                        if _entry_all_clicked_ok(entry):
                            # A correct PIN closes the keypad in favour of
                            # the Confirm Start dialog -- "OK" disappears.
                            names, _ = _wait_stable_names(ui)
                            right_pin_started = (
                                names is not None and "OK" not in names and "Cancel" in names
                            )
            else:
                baseline = _lcd19_overlay_names(ui)
                stop_click = ui.click_by_name("Stop")  # widget reads "Stop" while firing
                if stop_click.get("result") == "ok":
                    names, _ = _wait_for_overlay_names(ui, present=True, baseline=baseline)
                    state["after_stop_click_names"] = sorted(names) if names is not None else None
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
            overlay = _dismiss_lcd19_overlay(ui)
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
