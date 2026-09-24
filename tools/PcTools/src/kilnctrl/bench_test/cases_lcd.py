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

#: judgments.py's page-mismatch FAIL reasons (judge_lcd_home_idle,
#: judge_lcd_config_hub, judge_lcd_profiles_picker, judge_lcd_temperature_page)
#: append `judgments.BLANKED_SCREEN_HINT` to name this same race as a likely
#: cause when the page a case waited for never arrived -- see that module
#: for the hint text (kept there, not here, since this module would
#: otherwise need a reason string round trip through judgments.py just to
#: reuse a constant).


def _wake_and_home(ctx: dict) -> None:
    """One-shot per run (``ctx["_lcd_woken"]``): wake the panel via a real
    ``touch_inject`` press+release, then navigate back to home and confirm
    it via the existing page-poll helper. Best-effort -- a TOUCH-task query
    failure (board not wired for touch injection, transport hiccup) must
    never crash a case; if the wake itself fails, the case's own
    click/page-wait logic still runs and fails honestly on its own terms
    rather than this helper manufacturing a false precondition failure."""
    if ctx.get("_lcd_woken"):
        return
    ctx["_lcd_woken"] = True
    srv = _srv(ctx)
    touch = getattr(srv, "_touch", None)
    if touch is not None:
        try:
            x, y = _WAKE_TOUCH_XY
            touch.inject(x, y, True)
            touch.inject(x, y, False)
        except Exception:
            pass
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

#: Mirrored from firmware/KilnFW/App/drivers/ui/ui_theme.h -- reference
#: values ONLY. Per CLAUDE.md ("never by matching theme source constants"),
#: a sampled region is judged against these plus a live bezel sample
#: (lcd_sampler.matches_color()'s min_bezel_contrast floor), never against
#: this hex value alone with no reference to what the camera actually saw.
_ACCENT_4_RGB = (0x5C, 0xC0, 0x6E)
_ACCENT_1_RGB = (0xE8, 0x97, 0x4E)
_ACCENT_5_RGB = (0xD6, 0x55, 0x5F)


def _srv(ctx: dict):
    srv = ctx.get("srv")
    if srv is None:
        from .. import mcp_server as srv  # local import: keeps this module importable with no board attached
    return srv


def _remember_page_targets(ctx: dict, page: str, tap_targets: dict) -> None:
    ctx.setdefault("_lcd_pages_visited", {})[page] = tap_targets


def _try_capture_and_sample(ctx: dict, targets: "list[dict]") -> "tuple[Optional[tuple], Optional[bool]]":
    """Best-effort: capture one -Full frame and sample the 'start'/'pause'
    tap targets' regions. Returns (start_sample_or_None, pause_is_hidden)
    -- both None/absent on any capture failure (camera busy etc.)."""
    # Case-insensitive: firmware's label is Title Case ("Start"/"Pause");
    # see judgments._find_target's docstring for why this can't just be "==".
    start = J._find_target(targets, "start")
    pause = J._find_target(targets, "pause")
    if start is None and pause is None:
        return None, None
    tmpdir = ctx.get("_lcd_capture_dir") or tempfile.gettempdir()
    image_path = os.path.join(tmpdir, "bench_test_lcd_full.jpg")
    try:
        lcd_sampler.capture_full_frame(image_path, repo_root=ctx.get("repo_root"))
    except lcd_sampler.LcdCaptureError:
        return None, None
    finally:
        pass
    start_matches: Optional[bool] = None
    pause_hidden: Optional[bool] = None
    try:
        if start is not None and not start.get("hidden"):
            sample = lcd_sampler.sample_widget(image_path, start["cx"], start["cy"], repo_root=ctx.get("repo_root"))
            if sample.bezel is not None:
                start_matches = lcd_sampler.matches_color(sample.region, _ACCENT_4_RGB, sample.bezel)
        if pause is not None:
            sample2 = lcd_sampler.sample_widget(image_path, pause["cx"], pause["cy"], repo_root=ctx.get("repo_root"))
            if sample2.bezel is not None:
                pause_hidden = lcd_sampler.is_off(sample2.region, sample2.bezel)
    except lcd_sampler.LcdCaptureError:
        pass
    return start_matches, pause_hidden


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
    start_matches_accent4, pause_is_hidden = _try_capture_and_sample(ctx, targets)
    return J.judge_lcd_home_idle(page, targets, start_matches_accent4, pause_is_hidden)


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
    click = ui.click_by_name("settings")
    if click.get("result") != "ok":
        return CaseResult(
            Verdict.FAIL,
            reason=f"click_by_name('settings') returned {click.get('result')!r}, expected 'ok'",
            observed={"click": click},
        )
    page, waited_s = _wait_for_page(ui, "config")
    tap = ui.list_tap_targets()
    _remember_page_targets(ctx, "config", tap)
    result = J.judge_lcd_config_hub(page, tap.get("targets", []))
    result.observed = dict(result.observed or {})
    result.observed["page_wait_s"] = round(waited_s, 3)
    return result


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


def _capture(ctx: dict) -> Optional[str]:
    tmpdir = ctx.get("_lcd_capture_dir") or tempfile.gettempdir()
    image_path = os.path.join(tmpdir, "bench_test_lcd_full.jpg")
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
            ui.click_by_name("home")
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
    image_path = _capture(ctx)
    pause_matches_accent1 = _sample_widget_bool(ctx, image_path, pause, _ACCENT_1_RGB) if image_path else None
    progress_samples = hp01.get("progress_samples", [])
    profile_name_greyed = hp01.get("profile_name_greyed")
    profile_name_tap_noop = hp01.get("profile_name_tap_noop")
    if profile_name is None:
        profile_name_greyed = profile_name_greyed if profile_name_greyed is not None else None
    return J.judge_lcd_home_firing(
        page, targets, start_reads_stop, pause_matches_accent1, progress_samples,
        profile_name_greyed, profile_name_tap_noop,
    )


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
    image_path = _capture(ctx)
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
    image_path2 = _capture(ctx)
    strip_off_after_clear = _sample_widget_off(ctx, image_path2, strip_after) if image_path2 else None
    return J.judge_lcd_home_tripped(strip_visible_before, strip_matches_accent5_before, strip_off_after_clear)


# ---------------------------------------------------------------------------
# LCD-09 -- profiles picker (tap Profiles from the config hub).
# ---------------------------------------------------------------------------

def _case_lcd09(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        click = ui.click_by_name("settings")
        if click.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('settings') returned {click.get('result')!r}", observed={"click": click})
        _wait_for_page(ui, "config")
        click2 = ui.click_by_name("Profiles")
        if click2.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('Profiles') returned {click2.get('result')!r}", observed={"click": click2})
        page, waited_s = _wait_for_page(ui, "profiles")
        tap = ui.list_tap_targets()
        targets = tap.get("targets", [])
        _remember_page_targets(ctx, "profiles", tap)
        rows = [t for t in targets if str(t.get("name", "")).startswith("profile_row_")]
        paging_present = _find(targets, "paging") is not None
        new_icon_present = _find(targets, "new_profile") is not None
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
# LCD-14 -- temperature page: three zone rows vs. thermo_read(), and (post-
# rework) the Safety (K4) line vs. whether a firing is active.
# ---------------------------------------------------------------------------

def _case_lcd14(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        click = ui.click_by_name("settings")
        if click.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('settings') returned {click.get('result')!r}", observed={"click": click})
        _wait_for_page(ui, "config")
        click2 = ui.click_by_name("Temperature")
        if click2.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('Temperature') returned {click2.get('result')!r}", observed={"click": click2})
        page, waited_s = _wait_for_page(ui, "temperature")
        tap = ui.list_tap_targets()
        targets = tap.get("targets", [])
        _remember_page_targets(ctx, "temperature", tap)
        zone_rows: Dict[int, float] = {}
        for t in targets:
            name = str(t.get("name", ""))
            if name.startswith("zone_temp_") and t.get("value") is not None:
                try:
                    zone_rows[int(name.rsplit("_", 1)[-1])] = float(t["value"])
                except (TypeError, ValueError):
                    pass
        try:
            readings = {int(z): float(v) for z, v in srv._thermo.read_all().items()}
        except Exception:
            readings = {}
        safety_target = _find(targets, "safety_line")
        safety_on = safety_target.get("on") if safety_target is not None else None
        try:
            expect_safety_on = bool(srv._profiles.get_exec_status().state_name == "running")
        except Exception:
            expect_safety_on = False
        result = J.judge_lcd_temperature_page(page, zone_rows, readings, safety_on, expect_safety_on)
        result.observed = dict(result.observed or {})
        result.observed["page_wait_s"] = round(waited_s, 3)
        return result
    finally:
        _navigate_home(ui)


# ---------------------------------------------------------------------------
# LCD-16 -- diagnostics sub-pages, visited in order.
# ---------------------------------------------------------------------------

def _case_lcd16(ctx: dict) -> CaseResult:
    _wake_and_home(ctx)
    srv = _srv(ctx)
    ui = srv._ui_test
    titles_seen = []
    relay_life_has_reset: Optional[bool] = None
    crash_report_visible_entries: Optional[int] = None
    board_heap_value: Optional[float] = None
    try:
        click = ui.click_by_name("settings")
        if click.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('settings') returned {click.get('result')!r}", observed={"click": click})
        config_page, _ = _wait_for_page(ui, "config")
        click2 = ui.click_by_name("Diagnostics")
        if click2.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('Diagnostics') returned {click2.get('result')!r}", observed={"click": click2})
        # ui_page_diagnostics.c's sub-tabs never change kiln_ui's top-level
        # page name (they are one page's own internal s_pages[], see
        # _wait_for_targets_change's docstring), so waiting for a page name
        # here only ever catches config -> diagnostics; each per-title tap
        # below is followed by a tap-target-set wait instead.
        _wait_for_page_change(ui, config_page)
        prev_names = {t.get("name") for t in ui.list_tap_targets().get("targets", [])}
        for title in J._DIAG_TITLES:
            click3 = ui.click_by_name(title)
            if click3.get("result") != "ok":
                break
            tap, _ = _wait_for_targets_change(ui, prev_names)
            targets = tap.get("targets", [])
            prev_names = {t.get("name") for t in targets}
            page_title = ui.get_current_page()
            titles_seen.append(title)
            if title == "Relay Life":
                relay_life_has_reset = _find(targets, "reset") is not None
                heap_target = _find(targets, "board_heap_free")
                if heap_target is not None:
                    try:
                        board_heap_value = float(heap_target.get("value"))
                    except (TypeError, ValueError):
                        board_heap_value = None
            if title == "Crash Report":
                entries = [t for t in targets if str(t.get("name", "")).startswith("crash_entry_")]
                crash_report_visible_entries = len(entries)
            del page_title
        heap_diff_pct: Optional[float] = None
        if board_heap_value is not None:
            try:
                import re
                status_text = srv.get_heap_status()
                match = re.search(r"free[^0-9]*([0-9]+)", status_text, re.IGNORECASE)
                if match:
                    reported = float(match.group(1))
                    if reported:
                        heap_diff_pct = abs(board_heap_value - reported) / reported * 100.0
            except Exception:
                heap_diff_pct = None
        return J.judge_lcd_diagnostics_pages(
            titles_seen, relay_life_has_reset, crash_report_visible_entries, heap_diff_pct,
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
# UiTestClient has no enter_pin() today (that PIN-entry wiring is WEB-SEC-04's
# to add), so wrong_pin_refused/right_pin_started can never be observed here
# and stay None -- meaning judge_lcd_pin_lock can reach at most INCONCLUSIVE
# on any given run, never PASS, until that wiring lands. "right_pin_started"
# is a placeholder name kept for judgments.py's existing signature; entering
# the right PIN only opens the Confirm Start dialog, it does not itself start
# a firing.
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
                             interval_s: float = _PAGE_POLL_INTERVAL_S) -> "tuple[Optional[set], float]":
    """Same click-then-read race as :func:`_wait_for_page`, for LCD-19's
    top-layer popups: both the PIN keypad and the Confirm Start/Confirm
    Stop dialogs are lv_msgbox popups raised asynchronously off the same
    LVGL-task touch poll as any page switch (see kiln_ui_click_by_name()'s
    comment), so an immediate tap-target read after Start/Stop/Cancel can
    still see the pre-click set. `present=True` waits for a non-empty set
    (after opening a popup); `present=False` waits for empty (after
    dismissing one). Never raises; a popup that never (dis)appears is still
    reported honestly via whatever the last poll saw."""
    start = time.monotonic()
    names = _lcd19_overlay_names(ui)
    while names is not None and bool(names) != present:
        if time.monotonic() - start >= timeout_s:
            break
        time.sleep(interval_s)
        names = _lcd19_overlay_names(ui)
    return names, time.monotonic() - start


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
    keypad_raised = wrong_pin_refused = right_pin_started = stop_not_gated = None
    result: Optional[CaseResult] = None
    overlay: Optional[Dict[str, Any]] = None
    try:
        # kiln_ui_click_by_name() (kiln_ui.c) matches with an exact strcmp,
        # never case-insensitively, and the home fire button's label text is
        # exactly "Start" when idle/done/faulted or "Stop" while
        # RUNNING/PAUSED (ui_page_home.c / ui_page_home_refresh.c).
        firing_active = bool(pin_cfg.get("firing_active_with_lock"))
        if not firing_active:
            click = ui.click_by_name("Start")
            if click.get("result") == "ok":
                names, _ = _wait_for_overlay_names(ui, present=True)
                keypad_raised = names is not None and "OK" in names and "Cancel" in names
            # wrong_pin_refused / right_pin_started stay None: UiTestClient
            # has no enter_pin() to drive the keypad any further.
        else:
            stop_click = ui.click_by_name("Stop")  # widget reads "Stop" while firing
            if stop_click.get("result") == "ok":
                names, _ = _wait_for_overlay_names(ui, present=True)
                if names is not None:
                    has_cancel = "Cancel" in names
                    has_ok = "OK" in names
                    if has_cancel and not has_ok:
                        stop_not_gated = True  # Confirm Stop shown directly, no PIN keypad
                    elif has_cancel and has_ok:
                        stop_not_gated = False  # PIN keypad appeared -- Stop was gated
                    # else: neither popup present -- leave None (INCONCLUSIVE)
        result = J.judge_lcd_pin_lock(keypad_raised, wrong_pin_refused, right_pin_started, stop_not_gated)
        return result
    finally:
        overlay = _dismiss_lcd19_overlay(ui)
        if result is not None:
            result.observed = dict(result.observed or {})
            result.observed["overlay_dismiss"] = overlay
        _navigate_home(ui)


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
