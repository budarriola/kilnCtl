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
from typing import Any, Dict, Optional

from . import judgments as J
from . import lcd_sampler
from .registry import CaseResult, Verdict, get_case

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
    srv = _srv(ctx)
    ui = srv._ui_test
    click = ui.click_by_name("settings")
    if click.get("result") != "ok":
        return CaseResult(
            Verdict.FAIL,
            reason=f"click_by_name('settings') returned {click.get('result')!r}, expected 'ok'",
            observed={"click": click},
        )
    page = ui.get_current_page()
    tap = ui.list_tap_targets()
    _remember_page_targets(ctx, "config", tap)
    return J.judge_lcd_config_hub(page, tap.get("targets", []))


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
            ui.click_by_name("Back")
            if ui.get_current_page() != "home":
                ui.click_by_name("Home")
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
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        click = ui.click_by_name("settings")
        if click.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('settings') returned {click.get('result')!r}", observed={"click": click})
        click2 = ui.click_by_name("Profiles")
        if click2.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('Profiles') returned {click2.get('result')!r}", observed={"click": click2})
        page = ui.get_current_page()
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
                detail_page = ui.get_current_page()
        return J.judge_lcd_profiles_picker(page, rows, paging_present, new_icon_present, detail_page)
    finally:
        _navigate_home(ui)


# ---------------------------------------------------------------------------
# LCD-14 -- temperature page: three zone rows vs. thermo_read(), and (post-
# rework) the Safety (K4) line vs. whether a firing is active.
# ---------------------------------------------------------------------------

def _case_lcd14(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    ui = srv._ui_test
    try:
        click = ui.click_by_name("settings")
        if click.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('settings') returned {click.get('result')!r}", observed={"click": click})
        click2 = ui.click_by_name("Temperature")
        if click2.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('Temperature') returned {click2.get('result')!r}", observed={"click": click2})
        page = ui.get_current_page()
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
        return J.judge_lcd_temperature_page(page, zone_rows, readings, safety_on, expect_safety_on)
    finally:
        _navigate_home(ui)


# ---------------------------------------------------------------------------
# LCD-16 -- diagnostics sub-pages, visited in order.
# ---------------------------------------------------------------------------

def _case_lcd16(ctx: dict) -> CaseResult:
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
        click2 = ui.click_by_name("Diagnostics")
        if click2.get("result") != "ok":
            return CaseResult(Verdict.FAIL, reason=f"click_by_name('Diagnostics') returned {click2.get('result')!r}", observed={"click": click2})
        for title in J._DIAG_TITLES:
            click3 = ui.click_by_name(title)
            if click3.get("result") != "ok":
                break
            tap = ui.list_tap_targets()
            targets = tap.get("targets", [])
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
    after = _lcd19_overlay_names(ui)
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
                names = _lcd19_overlay_names(ui)
                keypad_raised = names is not None and "OK" in names and "Cancel" in names
            # wrong_pin_refused / right_pin_started stay None: UiTestClient
            # has no enter_pin() to drive the keypad any further.
        else:
            stop_click = ui.click_by_name("Stop")  # widget reads "Stop" while firing
            if stop_click.get("result") == "ok":
                names = _lcd19_overlay_names(ui)
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
