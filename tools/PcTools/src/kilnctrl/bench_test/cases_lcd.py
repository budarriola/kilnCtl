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
    start = next((t for t in targets if t.get("name") == "start"), None)
    pause = next((t for t in targets if t.get("name") == "pause"), None)
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
# LCD-08 -- config hub (tap Menu from home).
# ---------------------------------------------------------------------------

def _case_lcd08(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    ui = srv._ui_test
    click = ui.click_by_name("Menu")
    if click.get("result") != "ok":
        return CaseResult(
            Verdict.FAIL,
            reason=f"click_by_name('Menu') returned {click.get('result')!r}, expected 'ok'",
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


_CASE_FUNCS = {
    "LCD-01": _case_lcd01,
    "LCD-08": _case_lcd08,
    "LCD-21": _case_lcd21,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
