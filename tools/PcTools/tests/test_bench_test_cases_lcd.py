#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_lcd -- the fetch half of
LCD-01/08/21. srv._ui_test and lcd_sampler are both mocked; these tests
confirm each _case_XXX fetches the right thing, degrades gracefully when
the camera is unavailable, and hands the right data to judgments.py --
never that a real board or webcam answers a certain way.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd.py -q
"""
from __future__ import annotations

import itertools
import os
import sys
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_heat as CH  # noqa: E402
from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import cases_web_rw as CW  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test import lcd_sampler  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
from kilnctrl.devices_touch import (  # noqa: E402
    TOUCH_POWER_STATE_ERROR_HOLD,
    TOUCH_POWER_STATE_ON,
)
from test_bench_test_cases_web_rw import FakeSec04Client  # noqa: E402

#: This whole file drives cases_lcd.py's click/page-poll helpers against a
#: FakeUiTest double that never actually needs real wall-clock time to
#: settle -- every poll/retry loop in cases_lcd.py (``_wait_for_page``,
#: ``_click_then_page``, ``_wake_and_home``'s screen_on wait, etc.) exists
#: to ride out a REAL board's LVGL flush/screen_idle timing, which has no
#: counterpart here. Before cases_lcd.py's poll/retry helpers were fixed to
#: resolve their `timeout_s`/`interval_s` defaults at call time instead of
#: at function-definition time (2026-09-25 -- the same class of bug
#: f40e8d37's parent commit fixed for the `_lcd19_*` helpers), a bare
#: ``mock.patch.object(cases_lcd, "_PAGE_POLL_TIMEOUT_S", ...)`` could not
#: shrink any of these waits: the function's own default argument value was
#: already bound to the *original* 2.0s constant at import time, so every
#: FAIL/retry path in this file burned real seconds sleeping for no reason
#: -- this file took ~222s to run as a result (`--durations=15` showed
#: WakeAndHomeTest/Lcd08/09/14/16* dominating). Patching the module
#: constants module-wide for every test in this file (rather than adding a
#: separate patch to each slow test) is safe because production code never
#: reads these constants at import time either -- only at call time, now
#: that the sentinel fix landed -- and a handful of tests below still layer
#: their own narrower patch on top of this one, which mock supports fine.
_TIMING_PATCHERS: "list" = []


def setUpModule():
    patches = [
        mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.05),
        mock.patch.object(C, "_PAGE_POLL_INTERVAL_S", 0.01),
        mock.patch.object(C, "_WAKE_SCREEN_ON_TIMEOUT_S", 0.05),
        mock.patch.object(C, "_WAKE_SCREEN_ON_POLL_S", 0.01),
    ]
    for p in patches:
        p.start()
        _TIMING_PATCHERS.append(p)


def tearDownModule():
    for p in _TIMING_PATCHERS:
        p.stop()
    _TIMING_PATCHERS.clear()


class FakeUiTest:
    def __init__(self, page="home", targets=None, click_result="ok"):
        self._page = page
        self._targets = targets if targets is not None else []
        self._click_result = click_result

    def get_current_page(self):
        return self._page

    def list_tap_targets(self):
        return {"targets": self._targets, "truncated": False}

    def click_by_name(self, name):
        if self._click_result == "ok":
            self._page = "config"
        return {"result": self._click_result, "cx": 0, "cy": 0}


class FakeSrv:
    def __init__(self, ui_test):
        self._ui_test = ui_test


_HOME_TARGETS = [
    {"name": "start", "cx": 240, "cy": 280, "hidden": False},
    {"name": "pause", "cx": 240, "cy": 280, "hidden": True},
]


class Lcd01CastFallbackTest(unittest.TestCase):
    """cases_lcd's cast_channel derivation (7b487885 review): the degraded
    two-channel fallback must engage only when the independent background
    reference reads a channel clipped, and a fallback-only PASS must say so
    in its reason. Numbers are the 2026-09-25 20260925T055234Z_lcd capture's
    (bg (0,53,104), bezel (0,1,7), Start (0,152,96))."""

    _BEZEL = (0, 1, 7)

    def _run(self, bg_rgb, start_rgb):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        start = _HOME_TARGETS[0]
        start_xy = (start["cx"], start["cy"] + lcd_sampler.LABEL_AVOID_OFFSET_PX)

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            if (x, y) == start_xy:
                return lcd_sampler.RegionSample(region=start_rgb, bezel=self._BEZEL)
            if (x, y) == (start["cx"], start["cy"]):  # Pause: hidden, reads as bezel
                return lcd_sampler.RegionSample(region=self._BEZEL, bezel=self._BEZEL)
            return lcd_sampler.RegionSample(region=bg_rgb, bezel=self._BEZEL)

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="x.jpg"),              mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            return C._case_lcd01({"srv": srv})

    def test_clipped_reference_recovers_green_and_says_so(self):
        result = self._run((0, 53, 104), (0, 152, 96))
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed["color_debug"]["bg_reference"]["cast_channel"], 0)
        self.assertTrue(result.observed["color_debug"]["start"]["matched_via_cast_fallback"])
        self.assertIn("degraded cast fallback", result.reason)
        self.assertIn("ACCENT_1", result.reason)

    def test_clipped_reference_still_fails_the_red_stop_color(self):
        # The fire button's only other color is ACCENT_5 (Stop); with R
        # crushed its G:B (0x55:0x5F) is far from ACCENT_4's, so the color
        # fallback never engages -- but the background reference is ALSO
        # cast_suspected on this same frame (R crushed there too), so per
        # judgments.judge_lcd_home_idle's cast_suspected branch (2026-09-25
        # fix: this branch used to only annotate `reason` and fall through
        # to a hard FAIL, inconsistent with the weaker bg_out_of_tolerance
        # signal below it that DOES downgrade) this is now INCONCLUSIVE, not
        # FAIL: a mismatch on a frame whose own background reference reads
        # camera cast is not trustworthy evidence of a real firmware defect.
        result = self._run((0, 53, 104), (0, 0x55, 0x5F))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_cast_without_a_clipped_channel_does_not_engage_fallback(self):
        # Chroma offset well over CAST_CHROMA_THRESHOLD, but every channel
        # >= CAST_CHANNEL_CRUSH_MAX: a tint, not clipping -- no fallback. The
        # background reference is cast_suspected here too, so (2026-09-25
        # fix, see test_clipped_reference_still_fails_the_red_stop_color
        # above) the Start mismatch downgrades to INCONCLUSIVE rather than
        # FAIL.
        result = self._run((12, 60, 110), (0, 152, 96))
        self.assertTrue(result.observed["color_debug"]["bg_reference"]["cast_suspected"])
        self.assertIsNone(result.observed["color_debug"]["bg_reference"]["cast_channel"])
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_uncast_pass_has_no_fallback_reason(self):
        result = self._run((96, 126, 154), (68, 192, 130))
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertFalse(result.observed["color_debug"]["start"]["matched_via_cast_fallback"])
        self.assertEqual(result.reason, "")


class Lcd01BgOutOfToleranceTest(unittest.TestCase):
    """cases_lcd's bg_out_of_tolerance derivation (2026-09-25,
    20260925T170424Z_full/summary.json): a background reference that reads
    plainly wrong by absolute distance/chroma, but under CAST_CHROMA_
    THRESHOLD, must downgrade a Start-color FAIL to INCONCLUSIVE rather than
    being silently ignored because the cast fallback never engaged."""

    _BEZEL = (0, 1, 7)

    def _run(self, bg_rgb, start_rgb):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        start = _HOME_TARGETS[0]
        start_xy = (start["cx"], start["cy"] + lcd_sampler.LABEL_AVOID_OFFSET_PX)

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            if (x, y) == start_xy:
                return lcd_sampler.RegionSample(region=start_rgb, bezel=self._BEZEL)
            if (x, y) == (start["cx"], start["cy"]):  # Pause: hidden, reads as bezel
                return lcd_sampler.RegionSample(region=self._BEZEL, bezel=self._BEZEL)
            return lcd_sampler.RegionSample(region=bg_rgb, bezel=self._BEZEL)

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="x.jpg"),              mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            return C._case_lcd01({"srv": srv})

    def test_bad_background_downgrades_wrong_button_to_inconclusive(self):
        # 2026-09-25 bench evidence, near-exact: bg (52,90,111) vs target
        # (26,31,43) -- chroma offset well under CAST_CHROMA_THRESHOLD, but
        # absolute distance well over COLOR_MATCH_TOLERANCE. Start (27,153,76)
        # vs ACCENT_4 (92,192,110) is a genuine mismatch by both measures.
        result = self._run((52, 90, 111), (27, 153, 76))
        bg_ref = result.observed["color_debug"]["bg_reference"]
        self.assertFalse(bg_ref["cast_suspected"])
        self.assertTrue(bg_ref["bg_out_of_tolerance"])
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("background reference off", result.reason)

    def test_good_background_with_wrong_button_still_fails(self):
        # Negative direction: a clean background reference must never
        # downgrade a genuinely wrong button color.
        result = self._run((26, 31, 43), (214, 32, 32))
        bg_ref = result.observed["color_debug"]["bg_reference"]
        self.assertFalse(bg_ref["bg_out_of_tolerance"])
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_good_background_with_right_button_passes(self):
        result = self._run((26, 31, 43), (0x5C, 0xC0, 0x6E))
        bg_ref = result.observed["color_debug"]["bg_reference"]
        self.assertFalse(bg_ref["bg_out_of_tolerance"])
        self.assertEqual(result.verdict, Verdict.PASS)


class Lcd01Test(unittest.TestCase):
    def test_camera_unavailable_degrades_to_inconclusive(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_wrong_page_fails_without_touching_the_camera(self):
        srv = FakeSrv(FakeUiTest(page="profiles", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame") as cap:
            result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        cap.assert_not_called()

    def test_successful_capture_feeds_camera_data_through(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        fake_sample = lcd_sampler.RegionSample(region=(0x5C, 0xC0, 0x6E), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", return_value=fake_sample):
            result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_records_visited_page_in_ctx(self):
        ctx = {"srv": FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))}
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            C._case_lcd01(ctx)
        self.assertIn("home", ctx["_lcd_pages_visited"])

    def test_start_sample_uses_label_avoid_offset_not_raw_centre(self):
        # 2026-09-24 round 5 regression: the Start button's tap-target centre
        # sits on its own centred label (lv_obj_center(label)), so the case
        # must sample cy + LABEL_AVOID_OFFSET_PX, never the raw cy, or the
        # sample lands on the label glyph. Assert the actual (x, y) passed to
        # sample_widget by way of sample_widget_body.
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        fake_sample = lcd_sampler.RegionSample(region=(0x5C, 0xC0, 0x6E), bezel=(26, 31, 43))
        seen_xy = []

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            seen_xy.append((x, y))
            return fake_sample

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            C._case_lcd01({"srv": srv})
        start_target = _HOME_TARGETS[0]
        # Pause legitimately shares Start's raw (cx, cy) in this fixture and
        # is sampled unoffset by unchanged code, so a bare assertNotIn on the
        # raw coordinate would collide with that legitimate call. Assert
        # precisely what changed instead: Start's OWN sample used the offset.
        self.assertIn(
            (start_target["cx"], start_target["cy"] + lcd_sampler.LABEL_AVOID_OFFSET_PX),
            seen_xy,
        )

    def test_capture_lands_under_run_dir_captures_and_evidence_recorded(self, ):
        # 2026-09-24 fix: ctx["_lcd_capture_dir"] was never set anywhere, so
        # a captured frame always fell back to the OS tempdir and never
        # landed in the run's own captures/ directory, and CaseResult.evidence
        # stayed empty. A run_dir in ctx must now make the capture land under
        # <run_dir>/captures with its path recorded in evidence.
        import shutil
        import tempfile as _tempfile
        run_dir = _tempfile.mkdtemp(prefix="lcd01_run_")
        try:
            srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
            fake_sample = lcd_sampler.RegionSample(region=(0x5C, 0xC0, 0x6E), bezel=(26, 31, 43))
            captured_paths = []

            def fake_capture(path, repo_root=None):
                captured_paths.append(path)
                with open(path, "wb") as f:
                    f.write(b"\x00")

            with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=fake_capture), \
                 mock.patch.object(lcd_sampler, "sample_widget", return_value=fake_sample):
                result = C._case_lcd01({"srv": srv, "run_dir": run_dir})
            self.assertEqual(result.verdict, Verdict.PASS)
            self.assertTrue(captured_paths)
            self.assertTrue(captured_paths[0].startswith(os.path.join(run_dir, "captures")))
            self.assertTrue(os.path.isdir(os.path.join(run_dir, "captures")))
            self.assertEqual(result.evidence, [captured_paths[0]])
            self.assertIn("color_debug", result.observed)
            self.assertIn("start", result.observed["color_debug"])
            self.assertEqual(result.observed["color_debug"]["start"]["sampled_rgb"], fake_sample.region)
        finally:
            shutil.rmtree(run_dir, ignore_errors=True)

    def test_wrong_color_still_fails_with_numeric_evidence(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        # Bezel is a realistic dark reference (the measured bezel on both
        # 2026-09-24 captures reads ~(6,11,15)), distinctly darker than the
        # theme BG -- a fake bezel equal to _BG_RGB would make every
        # background point, including CORNER_CHECK_POINTS[0] == (5, 5),
        # read as bezel and downgrade this FAIL to INCONCLUSIVE.
        bezel = (6, 11, 15)
        red = lcd_sampler.RegionSample(region=(0xD6, 0x20, 0x20), bezel=bezel)
        bg = lcd_sampler.RegionSample(region=C._BG_RGB, bezel=bezel)

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            # No color cast in this capture: only the widget itself reads
            # wrong, so the bg-reference sanity check must not fire and
            # mask a genuine defect as INCONCLUSIVE. Background points
            # (the bg reference and every corner-check point) read as lit
            # theme BG; everything else is the wrong-colored widget.
            if (x, y) == C._BG_REFERENCE_XY or (x, y) in lcd_sampler.CORNER_CHECK_POINTS:
                return bg
            return red

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="x.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            result = C._case_lcd01({"srv": srv, "_lcd_capture_dir": "/cap"})
        # The real _downgrade_if_corners_stale() ran and found sound corners.
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertNotIn("frame_corners_stale", result.observed)
        self.assertFalse(result.observed["color_debug"]["bg_reference"]["reads_as_bezel"])
        dbg = result.observed["color_debug"]["start"]
        self.assertFalse(dbg["matches"])
        self.assertEqual(dbg["sampled_rgb"], (0xD6, 0x20, 0x20))
        self.assertGreater(dbg["distance"], dbg["distance_tolerance"])
        self.assertEqual(len(result.evidence), 1)

    def test_readonly_run_dir_never_fails_the_case(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        with mock.patch.object(C.os, "makedirs", side_effect=PermissionError("ro")), \
             mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")) as cap:
            result = C._case_lcd01({"srv": srv, "run_dir": "/ro/run"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertFalse(cap.call_args[0][0].startswith(os.path.join("/ro/run", "captures")))

    def test_explicit_lcd_capture_dir_overrides_run_dir(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")) as cap:
            C._case_lcd01({"srv": srv, "run_dir": "/should/not/be/used", "_lcd_capture_dir": "/explicit/dir"})
        called_path = cap.call_args[0][0]
        self.assertTrue(called_path.startswith("/explicit/dir") or called_path.startswith(os.path.normpath("/explicit/dir")))


_CONFIG_TARGETS = [
    {"name": "Profiles", "cx": 100, "cy": 100, "hidden": False},
    {"name": "Temperature", "cx": 200, "cy": 100, "hidden": False},
    {"name": "Network / Wi-Fi", "cx": 300, "cy": 100, "hidden": False},
    {"name": "Diagnostics", "cx": 100, "cy": 200, "hidden": False},
]


class Lcd08Test(unittest.TestCase):
    def test_menu_tap_reaches_config_hub(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_menu_tap_not_found_fails(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="not_found"))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not_found", result.observed.get("attribution", ""))

    def test_records_visited_page_in_ctx(self):
        ctx = {"srv": FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))}
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            C._case_lcd08(ctx)
        self.assertIn("config", ctx["_lcd_pages_visited"])

    def test_retries_once_on_swallowed_tap_then_passes(self):
        # 2026-09-24 fix: _case_lcd08 used to call click_by_name('settings')
        # directly with no retry -- migrated to _click_then_page(), which
        # retries once when the click said 'ok' but the page never moved
        # (the screen_idle swallow race).
        class SwallowOnceUi(FakeUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0
                self.settings_calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if name == "settings":
                    self.settings_calls += 1
                    if self.settings_calls == 1:
                        return {"result": "ok", "cx": 0, "cy": 0}  # swallowed: no page change
                    self._page = "config"
                    return {"result": "ok", "cx": 0, "cy": 0}
                # Any other click (e.g. _navigate_home's "home"/"back") is a
                # normal, un-swallowed navigation back to home.
                self._page = "home"
                return {"result": "ok", "cx": 0, "cy": 0}

        ui = SwallowOnceUi(page="home", targets=_CONFIG_TARGETS)
        srv = FakeSrv(ui)
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(ui.settings_calls, 2)

    def test_saves_a_capture_as_evidence(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))
        captured = {}

        def fake_capture(path, **kw):
            captured["path"] = path

        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=fake_capture):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.evidence, [captured["path"]])
        self.assertTrue(captured["path"].endswith("lcd08_config_hub.jpg"))


class StuckOnConfigUi(FakeUiTest):
    """A double whose 'settings' click reaches 'config' normally, but whose
    'home'/'back' clicks always report 'ok' without ever actually moving the
    page -- the finally block's _navigate_home() call is left stuck on
    'config', exactly the failed-restore case this class's tests exist to
    prove is harmless to the case's own verdict."""

    def click_by_name(self, name):
        if name == "settings":
            self._page = "config"
        return {"result": "ok", "cx": 0, "cy": 0}


class Lcd08NavigateHomeFoldTest(unittest.TestCase):
    """A failed best-effort _navigate_home() in _case_lcd08's finally block
    must be recorded (observed["navigate_home"]) but must never itself
    change the verdict or reason a PASS or a FAIL already settled on before
    the finally block ran."""

    def test_pass_stays_pass_when_navigate_home_fails(self):
        srv = FakeSrv(StuckOnConfigUi(page="home", targets=_CONFIG_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertIn("navigate_home", result.observed)
        self.assertFalse(result.observed["navigate_home"]["ok"])
        self.assertEqual(result.observed["navigate_home"]["page"], "config")

    def test_fail_stays_fail_when_navigate_home_fails(self):
        missing_diagnostics = [t for t in _CONFIG_TARGETS if t["name"] != "Diagnostics"]
        srv = FakeSrv(StuckOnConfigUi(page="home", targets=missing_diagnostics))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Diagnostics", result.reason)
        self.assertIn("navigate_home", result.observed)
        self.assertFalse(result.observed["navigate_home"]["ok"])


class FakeSafetyDiag:
    def __init__(self, trip_reason=0, trip_mask=None):
        self.trip_reason = trip_reason
        self.trip_mask = trip_mask if trip_mask is not None else (1 << (trip_reason - 1) if trip_reason else 0)


class FakeSafety:
    def __init__(self, diag=None):
        self._diag = diag or FakeSafetyDiag()

    def get_diag(self):
        return self._diag


class FakeExecStatus:
    def __init__(self, state_name="idle", zones=None):
        self.state_name = state_name
        self.zones = zones or []


class FakeProfiles:
    def __init__(self, status=None, summaries=None, list_all_raises=None):
        self._status = status or FakeExecStatus()
        self._summaries = summaries if summaries is not None else []
        self._list_all_raises = list_all_raises

    def get_exec_status(self):
        return self._status

    def list_all(self):
        if self._list_all_raises is not None:
            raise self._list_all_raises
        return self._summaries


class FakeThermoReading:
    """Stand-in for kilnctrl.devices_thermo.ThermoReading -- just enough
    surface (.channel/.temperature_c/.valid) for cases_lcd._case_lcd14's
    srv._thermo.read(THERMO_CHANNEL_ALL) call."""

    def __init__(self, channel, temperature_c, valid=True):
        self.channel = channel
        self.temperature_c = temperature_c
        self.valid = valid


class FakeThermo:
    """Real ThermoClient has only .read(channel), never a .read_all() --
    2026-09-24 bench root cause for LCD-14 (cases_lcd.py called a method
    that never existed, always raising AttributeError, silently swallowed).
    `readings` is {channel: temperature_c}, same convenient shape the old
    (nonexistent) read_all() would have returned, translated here into the
    list[ThermoReading]-shaped shape .read() actually returns."""

    def __init__(self, readings=None, raises=None):
        self._readings = readings or {}
        self._raises = raises

    def read(self, channel=0xFF, timeout=None):
        if self._raises is not None:
            raise self._raises
        return [FakeThermoReading(ch, temp) for ch, temp in self._readings.items()]


class FakeControl:
    """Stand-in for kilnctrl.control.ControlClient -- just enough surface
    (.get_zones()) for cases_lcd._configured_zone_count()'s
    srv._control.get_zones() call. Real get_zones() returns
    (thermo_count, relay_count, zones: list[ZoneConfig]); only len(zones)
    is used by _configured_zone_count(), so `zone_count` here builds a
    same-length placeholder list. `raises`, if set, is raised instead --
    the "zone count unreadable" case, which must produce None (never a
    fabricated/guessed count), per the opus review of d66ba612."""

    def __init__(self, zone_count=3, raises=None):
        self._zone_count = zone_count
        self._raises = raises

    def get_zones(self):
        if self._raises is not None:
            raise self._raises
        return (self._zone_count, self._zone_count, [object()] * self._zone_count)


class FakeSrvFull(FakeSrv):
    """Extends FakeSrv with the extra client attributes LCD-02/03/04/09/14/16/19
    read (safety/profiles/thermo, and safety_clear_trip() at module scope)."""

    def __init__(self, ui_test, safety=None, profiles=None, thermo=None, clear_trip_raises=False,
                 control=None):
        super().__init__(ui_test)
        self._safety = safety or FakeSafety()
        self._profiles = profiles or FakeProfiles()
        self._thermo = thermo or FakeThermo()
        self._control = control or FakeControl()
        self._clear_trip_raises = clear_trip_raises
        self.clear_trip_called = False

    def safety_clear_trip(self):
        self.clear_trip_called = True
        if self._clear_trip_raises:
            raise RuntimeError("clear_trip failed")

    def get_heap_status(self):
        return "heap free: 100000 bytes"


class PageNavUiTest(FakeUiTest):
    """A UI double that actually tracks which page a named click lands on,
    via an explicit name->page map, and a per-page targets map -- unlike
    the base FakeUiTest (used by LCD-01/08/21) which always lands on a
    single fixed page regardless of which name was clicked."""

    def __init__(self, page="home", page_targets=None, nav_map=None, click_result="ok"):
        super().__init__(page=page, targets=(page_targets or {}).get(page, []), click_result=click_result)
        self._page_targets = page_targets or {}
        self._nav_map = nav_map or {}

    def list_tap_targets(self):
        return {"targets": self._page_targets.get(self._page, []), "truncated": False}

    def click_by_name(self, name):
        if self._click_result != "ok":
            return {"result": self._click_result, "cx": 0, "cy": 0}
        dest = self._nav_map.get(name)
        if dest is not None:
            self._page = dest
        return {"result": "ok", "cx": 0, "cy": 0}


class RaisingUiTest(PageNavUiTest):
    """A UI double whose click_by_name raises on the Nth call (after any
    earlier calls have already navigated), to exercise the
    finally-restore-on-exception path with the board actually having left
    'home' by the time the exception hits."""

    def __init__(self, *a, raise_on_call=2, **kw):
        super().__init__(*a, **kw)
        self._call_count = 0
        self._raise_on_call = raise_on_call
        self.home_calls = 0

    def click_by_name(self, name):
        self._call_count += 1
        if name == "home":
            self.home_calls += 1
            self._page = "home"
            return {"result": "ok"}
        if name == "back":
            return {"result": "not_found"}
        if self._call_count == self._raise_on_call:
            raise RuntimeError("board went unresponsive")
        return super().click_by_name(name)


class BackThenNoHomeUiTest(PageNavUiTest):
    """Mirrors the real topbar's per-page icon set for _navigate_home's
    home-first ordering: 'back' walks a page toward home one hop at a time
    (diagnostics -> config -> home, config's own Back), while 'home' is a
    no-op (not_found) on config specifically -- config hub has no Home icon
    (show_home=false) -- and otherwise jumps straight to home."""

    def click_by_name(self, name):
        if name == "home":
            if self._page == "config":
                return {"result": "not_found", "cx": 0, "cy": 0}
            self._page = "home"
            return {"result": "ok", "cx": 0, "cy": 0}
        if name == "back":
            dest = self._nav_map.get(self._page)
            if dest is not None:
                self._page = dest
            return {"result": "ok", "cx": 0, "cy": 0}
        return super().click_by_name(name)


class NavigateHomeTest(unittest.TestCase):
    def test_back_then_home_from_diagnostics_via_config(self):
        # diagnostics has no direct Home icon target in this double's map,
        # only a Back that lands on config; config itself has no Home icon
        # (show_home=false) and its own Back reaches home. A "back"-first
        # ordering would click "back" (diagnostics -> config), then "home"
        # (not_found on config) and strand on config; "home"-first either
        # jumps straight home from a page that has it, or falls through to
        # "back" from config, which does reach home.
        ui = BackThenNoHomeUiTest(page="diagnostics", nav_map={"diagnostics": "config", "config": "home"})
        C._navigate_home(ui)
        self.assertEqual(ui.get_current_page(), "home")


class Lcd02Test(unittest.TestCase):
    def test_not_run_when_hp01_absent(self):
        srv = FakeSrvFull(FakeUiTest(page="home", targets=_HOME_TARGETS))
        result = C._case_lcd02({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_wrong_page_fails(self):
        # _case_lcd02 calls _capture() unconditionally (before the page check
        # is judged), so this test must mock lcd_sampler.capture_full_frame --
        # otherwise it shells out to capture_lcd.ps1 and depends on the real
        # bench webcam. Page mismatch fails immediately regardless of what
        # the (mocked-away) camera returns (judge_lcd_home_firing's first
        # check), so the busy/unavailable side effect used elsewhere in this
        # class is sufficient here too.
        srv = FakeSrvFull(FakeUiTest(page="profiles", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd02({"srv": srv, "_hp01": {}})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_camera_unavailable_degrades_to_inconclusive(self):
        targets = [dict(t, label="Stop") if t["name"] == "start" else t for t in _HOME_TARGETS]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=targets))
        hp01 = {"progress_samples": [], "profile_name_greyed": None, "profile_name_tap_noop": None}
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd02({"srv": srv, "_hp01": hp01})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_full_data_passes(self):
        targets = [
            {"name": "start", "cx": 240, "cy": 280, "hidden": False, "label": "Stop"},
            {"name": "pause", "cx": 200, "cy": 280, "hidden": False},
            {"name": "profile_name", "cx": 240, "cy": 30, "hidden": False},
        ]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=targets))
        hp01 = {"progress_samples": [1.0, 90.0], "profile_name_greyed": True, "profile_name_tap_noop": True}
        fake_sample = lcd_sampler.RegionSample(region=(0xE8, 0x97, 0x4E), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", return_value=fake_sample):
            result = C._case_lcd02({"srv": srv, "_hp01": hp01})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_pause_sample_uses_label_avoid_offset_not_raw_centre(self):
        # Same class of regression as Lcd01Test's Start check: Pause is also
        # a ui_home_build_button() button with a centred caption, and now
        # goes through _sample_button_bool -> sample_widget_body.
        targets = [
            {"name": "start", "cx": 240, "cy": 280, "hidden": False, "label": "Stop"},
            {"name": "pause", "cx": 200, "cy": 280, "hidden": False},
            {"name": "profile_name", "cx": 240, "cy": 30, "hidden": False},
        ]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=targets))
        hp01 = {"progress_samples": [1.0, 90.0], "profile_name_greyed": True, "profile_name_tap_noop": True}
        fake_sample = lcd_sampler.RegionSample(region=(0xE8, 0x97, 0x4E), bezel=(26, 31, 43))
        seen_xy = []

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            seen_xy.append((x, y))
            return fake_sample

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            C._case_lcd02({"srv": srv, "_hp01": hp01})
        self.assertIn((200, 280 + lcd_sampler.LABEL_AVOID_OFFSET_PX), seen_xy)
        self.assertNotIn((200, 280), seen_xy)


class Lcd03Test(unittest.TestCase):
    def test_not_run_when_hp04_absent(self):
        result = C._case_lcd03({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_resume_label_zero_duty_passes(self):
        ctx = {"_hp04": {
            "pause_ui_targets": [{"name": "pause", "label": "Resume"}],
            "duties_while_paused": [0.0, 0.0],
        }}
        result = C._case_lcd03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_nonzero_duty_fails(self):
        ctx = {"_hp04": {
            "pause_ui_targets": [{"name": "pause", "label": "Resume"}],
            "duties_while_paused": [0.0, 0.5],
        }}
        result = C._case_lcd03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Lcd04Test(unittest.TestCase):
    def test_not_run_when_no_trip_latched(self):
        srv = FakeSrvFull(FakeUiTest(page="home"), safety=FakeSafety(FakeSafetyDiag(trip_reason=0)))
        result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertFalse(srv.clear_trip_called)

    def test_camera_unavailable_degrades_to_inconclusive(self):
        strip_targets = [{"name": "trip_strip", "cx": 240, "cy": 10, "hidden": False}]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=strip_targets), safety=FakeSafety(FakeSafetyDiag(trip_reason=6)))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertTrue(srv.clear_trip_called)

    def test_unexpected_trip_mask_refuses_to_clear(self):
        # trip_reason=6 (S6a) implies mask 0x0020 by the formula in CLAUDE.md;
        # a board reporting a different mask must never be cleared blind.
        # _case_lcd04 captures its "before" frame unconditionally once a trip
        # is latched, before it ever reaches the mask check below, so this
        # test must mock the camera too or it shells out for real.
        srv = FakeSrvFull(FakeUiTest(page="home"), safety=FakeSafety(FakeSafetyDiag(trip_reason=6, trip_mask=0x0040)))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertFalse(srv.clear_trip_called)

    def test_full_data_passes_and_clears(self):
        strip_targets = [{"name": "trip_strip", "cx": 240, "cy": 10, "hidden": False}]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=strip_targets), safety=FakeSafety(FakeSafetyDiag(trip_reason=6)))
        region_sample = lcd_sampler.RegionSample(region=(0xD6, 0x55, 0x5F), bezel=(26, 31, 43))
        off_sample = lcd_sampler.RegionSample(region=(26, 31, 43), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=[region_sample, off_sample]):
            result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertTrue(srv.clear_trip_called)

    def test_before_and_after_frames_get_distinct_paths(self):
        # Both frames land in evidence; a shared filename would make the
        # after-clear frame overwrite the before-clear one.
        strip_targets = [{"name": "trip_strip", "cx": 240, "cy": 10, "hidden": False}]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=strip_targets), safety=FakeSafety(FakeSafetyDiag(trip_reason=6)))
        region_sample = lcd_sampler.RegionSample(region=(0xD6, 0x55, 0x5F), bezel=(26, 31, 43))
        off_sample = lcd_sampler.RegionSample(region=(26, 31, 43), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=[region_sample, off_sample]):
            result = C._case_lcd04({"srv": srv, "_lcd_capture_dir": "/cap"})
        self.assertEqual(len(result.evidence), 2)
        self.assertEqual(len(set(result.evidence)), 2)


_PROFILES_NAV = {"settings": "config", "Profiles": "profiles", "profile_row_0": "profile_detail"}
# Round 3 (item 4): "paging"/"new_profile" were fabricated names no
# firmware target has ever used -- ui_page_profile_picker.c's topbar
# icons are untagged glyphs, located only by position via
# _profiles_topbar_icons() (back/home anchors + a glyph in the New slot).
_PROFILES_GLYPH = "���"
_PROFILES_PAGE_TARGETS = {
    "home": [{"name": "settings", "cx": 10, "cy": 10, "hidden": False}],
    "config": [{"name": "Profiles", "cx": 100, "cy": 100, "hidden": False}],
    "profiles": [
        {"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False, "starred": False},
        {"name": "back", "cx": 140, "cy": 26, "hidden": False},
        {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        {"name": _PROFILES_GLYPH, "cx": 180 + 40, "cy": 26, "hidden": False},   # Prev
        {"name": _PROFILES_GLYPH, "cx": 180 + 80, "cy": 26, "hidden": False},   # Next
        {"name": _PROFILES_GLYPH, "cx": 180 + 120, "cy": 26, "hidden": False},  # New
    ],
    "profile_detail": [],
}


class PollTimeoutSentinelTakesEffectTest(unittest.TestCase):
    """Negative test for the 2026-09-25 fix itself: proves that patching
    ``cases_lcd._PAGE_POLL_TIMEOUT_S`` actually shortens a caller's wait when
    that caller passes no explicit `timeout_s` of its own -- i.e. that the
    default is resolved at CALL time, not bound once when the function was
    defined. Before the fix, ``_wait_for_page``'s signature was
    ``timeout_s: float = _PAGE_POLL_TIMEOUT_S``: that default value is
    captured at import time, so this same patch would have done nothing and
    this test would have measured ~2.0s (this module's setUpModule() patches
    it to 0.05s for every other test in this file, which is exactly why this
    test exists: to prove that patch is not a no-op)."""

    def test_patched_page_poll_timeout_shortens_wait_for_page(self):
        ui = FakeUiTest(page="home")  # never reaches "config"
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.01), \
             mock.patch.object(C, "_PAGE_POLL_INTERVAL_S", 0.001):
            start = time.monotonic()
            page, waited_s = C._wait_for_page(ui, "config")
            elapsed = time.monotonic() - start
        self.assertEqual(page, "home")
        # Bounded well under the production 2.0s default -- if the sentinel
        # resolution regressed back to a bare bound default, this would
        # measure ~2.0s (or this file's setUpModule()-patched 0.05s) instead.
        self.assertLess(elapsed, 0.3)
        self.assertLess(waited_s, 0.3)

    def test_unpatched_call_uses_this_files_setupmodule_patch_not_production_default(self):
        """Sanity companion: with no per-test override, the wait still stays
        short because setUpModule() already patched the module constant to
        0.05s for the whole file -- confirming the same call-time resolution
        applies when a test relies on the module-wide patch instead of its
        own local one."""
        ui = FakeUiTest(page="home")
        start = time.monotonic()
        page, _ = C._wait_for_page(ui, "config")
        elapsed = time.monotonic() - start
        self.assertEqual(page, "home")
        self.assertLess(elapsed, 0.5)


class ClickThenPageTest(unittest.TestCase):
    """Direct unit tests for _click_then_page() itself (2026-09-24 LCD-09/
    14/16 fix): a bare click_by_name() + _wait_for_page() with the wait's
    return value discarded let a case march on to click a target that could
    not exist on whatever page the board was really parked on. These tests
    exercise the helper directly, independent of any one case, including
    the timeout-not-checked regression the fix specifically closes."""

    def test_success_returns_none_fail_and_the_arrived_page(self):
        ui = PageNavUiTest(page="home", page_targets={"home": [], "config": []},
                            nav_map={"settings": "config"})
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config")
        self.assertIsNone(fail)
        self.assertEqual(page, "config")

    def test_click_itself_not_found_fails_without_polling(self):
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={}, click_result="not_found")
        with mock.patch.object(C.time, "sleep"):
            fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("not_found", fail.reason)

    def test_click_itself_not_found_records_page_before_for_diagnosis(self):
        # 2026-09-30 (LCD-16 bench investigation, 20260930T043143Z_lcd_lcd19_
        # rerun_0929_59c9306a/summary.json): a "not_found" on 'settings'
        # (home-only per firmware) is equally explained by a genuinely
        # absent target OR by the board never actually having reached
        # "home" (e.g. a prior case's best-effort navigate-home silently
        # failing) -- the two are indistinguishable without knowing which
        # page the board was actually on right before the click. The helper
        # already queries get_current_page() for its own swallow-retry loop;
        # this asserts that value is also surfaced in the FAIL's `observed`
        # dict on the immediate-failure path, not just used internally and
        # discarded.
        ui = PageNavUiTest(page="temperature", page_targets={"temperature": []},
                            nav_map={}, click_result="not_found")
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.observed.get("page_before"), "temperature")

    def test_click_inject_failed_fails_without_polling(self):
        # 2026-09-24 follow-up: lvgl_port_inject_touch() itself refused (no
        # press ever sent) -- attributed distinctly from "not_found" and,
        # like it, never waited on for a page change this click could not
        # have caused.
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={},
                            click_result="inject_failed")
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("inject_failed", fail.reason)
        self.assertEqual(fail.observed.get("attribution"), "inject_failed")
        self.assertEqual(waited_s, 0.0)

    def test_click_ok_but_page_never_arrives_fails(self):
        # This is the exact regression the 2026-09-24 fix closes: without
        # checking _wait_for_page()'s return value, a click that replies
        # 'ok' but never actually lands on the destination page would be
        # treated as a successful hop -- and a caller would go on to click
        # a target that cannot exist on the page the board is really on.
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={})  # "settings" click has no nav_map entry
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("config", fail.reason)
        self.assertEqual(page, "home")

    def test_retries_once_and_recovers_on_second_click(self):
        # A click that says 'ok' but the page swallowed the tap on the
        # first attempt must be retried once before failing outright -- the
        # retry lands on an already-awake panel (no wake edge involved), so
        # a second click that actually reaches the target must recover.
        class _RecoversOnSecondClick(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if self.calls == 1:
                    return {"result": "ok"}  # swallowed: no page change
                return super().click_by_name(name)

        ui = _RecoversOnSecondClick(page="home", page_targets={"home": [], "config": []},
                                     nav_map={"settings": "config"})
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNone(fail)
        self.assertEqual(page, "config")
        self.assertEqual(ui.calls, 2)

    def test_not_found_is_retried_and_recovers_on_second_click(self):
        # 2026-09-30 (LCD-09/LCD-16 bench investigation, run
        # 20260930T215921Z_lcd): kiln_ui_click_by_name()'s own doc comment
        # says a dispatch timeout onto lvgl_port_task (the tap-target walk's
        # 300 ms UI_WALK_WAIT_TIMEOUT_MS window, lvgl_port.c) "surfaces as
        # n == 0 ... indistinguishable from a genuinely absent name" -- a
        # transient race, not a defect. A first click reporting 'not_found'
        # must therefore be retried (bounded by
        # _CLICK_THEN_PAGE_NOT_FOUND_RETRIES) before being treated as a real
        # absence; a second click that actually finds the target must
        # recover, exactly like the swallowed-tap retry above. No press is
        # ever sent on a 'not_found' click, so this blind retry is safe.
        class _RecoversOnSecondClick(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if self.calls == 1:
                    return {"result": "not_found", "cx": 0, "cy": 0}
                return super().click_by_name(name)

        ui = _RecoversOnSecondClick(page="home", page_targets={"home": [], "config": []},
                                     nav_map={"settings": "config"})
        with mock.patch.object(C.time, "sleep"):
            fail, page, waited_s, swallow_retries, not_found_retries, _ = C._click_then_page(
                ui, "settings", "config", timeout_s=0.05)
        self.assertIsNone(fail)
        self.assertEqual(page, "config")
        self.assertEqual(ui.calls, 2)
        # 2026-09-30 (Opus review of ea345753): the not_found retry must be
        # counted separately from a swallow retry, not folded into one
        # counter -- otherwise a caller's `observed` dict would misreport a
        # not_found recovery as a swallow.
        self.assertEqual(swallow_retries, 0)
        self.assertEqual(not_found_retries, 1)

    def test_not_found_persisting_past_its_retry_budget_still_fails(self):
        # The retry above cannot make a genuinely-absent target start
        # existing -- a 'not_found' that persists through every retry must
        # still fail, same as before this fix, just after
        # _CLICK_THEN_PAGE_NOT_FOUND_RETRIES extra round trips.
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={}, click_result="not_found")
        with mock.patch.object(C.time, "sleep"):
            fail, page, waited_s, _, not_found_retries, _ = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("not_found", fail.reason)
        # 2026-09-30 (Opus review of ea345753): a failure result's own
        # counters are 0 per _click_then_page()'s contract -- the retries
        # actually spent are recorded inside fail.observed instead.
        self.assertEqual(not_found_retries, 0)
        self.assertEqual(fail.observed.get("not_found_retries"), 2)

    def test_retry_exhausted_fails_naming_the_retry(self):
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={})
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("retried 2 times", fail.reason)
        # No click in this attempt (initial or either retry) ever reported
        # "swallowed" -- _CountingNavUi always says "ok" -- so this is
        # attributed as a genuine UI defect, not the wake/dismiss race.
        self.assertEqual(fail.observed.get("attribution"), "genuine_defect")
        self.assertIn("every final click delivered un-swallowed", fail.reason)
        # Default max_retries=2: the original tap plus exactly two retries,
        # never more.
        self.assertEqual(ui.clicks, ["settings", "settings", "settings"])
        self.assertIn("retry_click", fail.observed)
        self.assertEqual(fail.observed.get("retries"), 2)

    def test_swallow_resolved_then_ok_unmoved_is_still_genuine_defect(self):
        # Review fix 2026-09-24: the first click reports "swallowed", the
        # immediate re-click reports un-swallowed "ok", and the page still
        # never moves across two more "ok" retries. The resolved swallow does
        # not explain three un-swallowed clicks doing nothing -- this must be
        # attributed as a genuine defect, never relabelled as a swallow.
        class _SwallowOnceThenDeadNav(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if self.calls == 1:
                    return {"result": "swallowed", "cx": 0, "cy": 0}
                return {"result": "ok", "cx": 0, "cy": 0}  # dead widget: never navigates

        ui = _SwallowOnceThenDeadNav(page="home", page_targets={"home": []}, nav_map={})
        fail, page, _, _sr, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertEqual(fail.observed.get("attribution"), "genuine_defect")
        self.assertEqual(fail.observed.get("swallow_retries"), 1)
        self.assertIn("1 earlier swallow(s)", fail.reason)
        self.assertEqual(ui.calls, 4)  # swallowed + ok, then two ok retries

    def test_initial_swallow_resolved_by_reclick_passes(self):
        class _SwallowOnceThenNav(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if self.calls == 1:
                    return {"result": "swallowed", "cx": 0, "cy": 0}
                return super().click_by_name(name)

        ui = _SwallowOnceThenNav(page="home", page_targets={"home": [], "config": []},
                                 nav_map={"settings": "config"})
        fail, page, _, _sr, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNone(fail)
        self.assertEqual(page, "config")
        self.assertEqual(ui.calls, 2)

    def test_always_swallowed_fails_bounded_never_ok(self):
        # A board that swallows every tap (e.g. stuck re-entering
        # ERROR_HOLD) must FAIL within the swallow budget, never pass.
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={},
                            click_result="swallowed")
        fail, page, _, _sr, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertEqual(len(ui.clicks), 1 + C._CLICK_THEN_PAGE_SWALLOW_RETRIES)

    def test_double_swallow_recovers_on_second_retry(self):
        # 2026-09-24 LCD-08 bench run 20260924T233113Z_lcd: the original
        # click AND its one (then-only) retry both stayed on 'home'. This is
        # the regression test for that exact shape -- two swallows in a row,
        # recovered by the third attempt now that max_retries defaults to 2.
        class _RecoversOnThirdClick(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if self.calls <= 2:
                    return {"result": "ok"}  # swallowed twice: no page change
                return super().click_by_name(name)

        ui = _RecoversOnThirdClick(page="home", page_targets={"home": [], "config": []},
                                    nav_map={"settings": "config"})
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNone(fail)
        self.assertEqual(page, "config")
        self.assertEqual(ui.calls, 3)

    def test_always_swallowed_first_click_attributed_swallowed(self):
        # A swallow that outlasts the immediate re-click budget on the FIRST
        # click is a swallow, not "not_found" -- the target was found.
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={},
                            click_result="swallowed")
        fail, _, _, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertEqual(fail.observed.get("attribution"), "swallowed")

    def test_verdict_unknown_press_that_landed_is_not_reclicked(self):
        # The usual cause of 'verdict_unknown' is a slow flush delaying the
        # verdict, not the press: the press navigates. It must be judged by
        # the page arriving and never blind re-clicked on the new page.
        class _UnknownButNavigates(_CountingNavUi):
            def click_by_name(self, name):
                super().click_by_name(name)
                return {"result": "verdict_unknown", "cx": 0, "cy": 0}

        ui = _UnknownButNavigates(page="home", page_targets={"home": [], "config": []},
                                  nav_map={"settings": "config"})
        fail, page, _, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNone(fail)
        self.assertEqual(page, "config")
        self.assertEqual(ui.clicks, ["settings"])

    def test_always_verdict_unknown_unmoved_fails_bounded_never_defect(self):
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={},
                            click_result="verdict_unknown")
        fail, page, _, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertEqual(fail.observed.get("attribution"), "verdict_unknown")
        self.assertIn("not a confirmed defect", fail.reason)
        # No immediate re-click for verdict_unknown: only the page-unchanged
        # retries, so 1 + max_retries clicks.
        self.assertEqual(len(ui.clicks), 1 + C._CLICK_THEN_PAGE_MAX_RETRIES)

    def test_verdict_unknown_plus_one_confirmed_ok_is_not_genuine_defect(self):
        # One confirmed-clean click is not the two genuine_defect rests on.
        class _UnknownThenDeadOk(_CountingNavUi):
            def click_by_name(self, name):
                self.clicks.append(name)
                r = "verdict_unknown" if len(self.clicks) == 1 else "ok"
                return {"result": r, "cx": 0, "cy": 0}

        ui = _UnknownThenDeadOk(page="home", page_targets={"home": []}, nav_map={})
        fail, _, _, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05, max_retries=1)
        self.assertEqual(fail.observed.get("attribution"), "verdict_unknown")
        ui = _UnknownThenDeadOk(page="home", page_targets={"home": []}, nav_map={})
        fail, _, _, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05, max_retries=2)
        self.assertEqual(fail.observed.get("attribution"), "genuine_defect")

    def test_retry_click_inject_failed_attributed_and_not_reclicked(self):
        # First click 'ok' but the page never moves; the page-unchanged
        # retry's press is never queued. Attributed inject_failed (not the
        # generic retry_click_failed), never a pass, and no further click.
        class _OkThenInjectFailed(_CountingNavUi):
            def click_by_name(self, name):
                self.clicks.append(name)
                r = "ok" if len(self.clicks) == 1 else "inject_failed"
                return {"result": r, "cx": 0, "cy": 0}

        ui = _OkThenInjectFailed(page="home", page_targets={"home": []}, nav_map={})
        fail, _, _, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertEqual(fail.observed.get("attribution"), "inject_failed")
        self.assertIn("'inject_failed'", fail.reason)
        self.assertEqual(ui.clicks, ["settings", "settings"])

    def test_max_retries_zero_restores_single_attempt_behavior(self):
        # Negative-test knob: max_retries=0 collapses back to the old
        # never-retry behavior, so the retry loop itself (not some other
        # path) is what's responsible for recovering a swallow.
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={})
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05, max_retries=0)
        self.assertIsNotNone(fail)
        self.assertIn("not retried", fail.reason)
        self.assertEqual(fail.observed.get("attribution"), "wrong_page")
        self.assertEqual(ui.clicks, ["settings"])

    def test_not_found_attribution_retries_then_gives_up(self):
        # 2026-09-30 (LCD-09/LCD-16 bench investigation): this used to
        # assert a bare 'not_found' was NEVER retried at all -- see
        # _CLICK_THEN_PAGE_NOT_FOUND_RETRIES's comment for why that turned
        # out to be wrong: a dispatch timeout onto lvgl_port_task produces
        # the identical 'not_found' as a genuine absence, so a persistent
        # 'not_found' now costs exactly
        # `1 + _CLICK_THEN_PAGE_NOT_FOUND_RETRIES` clicks (3 at the
        # default) before it is attributed as such, not just the original
        # one.
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={}, click_result="not_found")
        with mock.patch.object(C.time, "sleep"):
            fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.observed.get("attribution"), "not_found")
        self.assertEqual(ui.clicks, ["settings", "settings", "settings"])
        # 2026-09-30 (Opus review of ea345753): recorded as its own counter,
        # never folded into (or confused with) swallow_retries.
        self.assertEqual(fail.observed.get("not_found_retries"), 2)
        self.assertNotIn("swallow_retries", fail.observed)

    def test_page_moved_elsewhere_is_never_retried(self):
        # The first tap DID move the board, just not to the expected page.
        # A second tap by the same name would land on a different page's
        # widget, so it must never be sent.
        ui = _CountingNavUi(page="home", page_targets={"home": [], "diagnostics": []},
                            nav_map={"settings": "diagnostics"})
        fail, page, waited_s, _, _, _ = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(ui.clicks, ["settings"])
        self.assertEqual(fail.observed.get("attribution"), "wrong_page")
        self.assertEqual(page, "diagnostics")

    def test_every_call_site_is_a_pure_navigation_target(self):
        # A retried click is a second real tap. Pin EVERY call site of both
        # name-based retrying helpers (_click_then_page and
        # _click_then_targets_change -- the latter now unused by any case
        # since round 3's LCD-16 rewrite moved to a raw-touch retry instead,
        # but is checked here too in case a future case reintroduces it) so
        # a Start/Stop/Confirm/PIN/toggle target can't be added without
        # revisiting the retry. Every argument here must be a literal name
        # in the allowlist -- round 3 removed the one non-literal case
        # (`title` iterating J._DIAG_TITLES). LCD-09's fix (2026-09-24)
        # reintroduced one non-literal call site: `rows[0]["name"]`, the
        # profile-picker's row label read live off the board. It is
        # explicitly allowed here rather than added to `literals` (which
        # must stay a set of exact strings) because the row name is a
        # profile name chosen by whatever profiles exist on the bench, never
        # a fixed string -- but it is still reviewed and safe: it names a
        # row on the `profiles` list page, structurally never the same
        # widget as Start/Stop/Confirm/PIN/toggle, which all live on other
        # pages entirely (home / settings), so the `unsafe`-word check below
        # does not apply to it.
        import inspect
        import re
        src = inspect.getsource(C)
        calls = re.findall(r'(?<!def )\b(_click_then_page|_click_then_targets_change)\(\s*ui\s*,\s*([^,)]+)', src)
        self.assertTrue(calls, "no call sites found -- the regex no longer matches the source")
        allowed_nonliteral = {'rows[0]["name"]'}
        literals = set()
        for helper, arg in calls:
            arg = arg.strip()
            if arg in allowed_nonliteral:
                continue
            m = re.fullmatch(r'"([^"]+)"', arg)
            self.assertIsNotNone(m, f"non-literal retried click target {arg!r} in {helper}()")
            literals.add(m.group(1))
        # LCD-22 deliberately does NOT route "Edit" through _click_then_page:
        # on a PIN-locked panel Edit raises the keypad while the page still
        # reads "home", so a retry would tap the keypad. It clicks Edit once.
        self.assertEqual(literals, {"settings", "Profiles", "Temperature", "Diagnostics"})
        unsafe = ("start", "stop", "confirm", "pin", "toggle", "cancel", "ack", "reset", "clear", "ok")
        for title in literals:
            words = re.findall(r"[a-z]+", title.lower())
            for word in unsafe:
                self.assertNotIn(word, words, f"retried click target {title!r} contains {word!r}")

    def test_next_touch_retry_never_uses_click_by_name(self):
        # Round 3 (item 5): the diagnostics topbar's Next icon is paged via
        # a raw coordinate touch (_tap_next_then_targets_change()), never
        # click_by_name() -- Prev/Next share one undecodable glyph name
        # (_is_glyph_name()) and click_by_name() answers AMBIGUOUS for it on
        # a live board, so this call site must never be added to (or
        # confused with) the name-based retry allowlist above.
        import inspect
        import re
        src = inspect.getsource(C._tap_next_then_targets_change)
        # Match a call/attribute-access, not the docstring's prose mentions
        # of click_by_name() explaining why it is deliberately avoided.
        self.assertNotRegex(src, r'\.click_by_name\s*\(')
        self.assertIn("touch.inject", src)


class _QueuedClickUi(PageNavUiTest):
    """A UI double whose click_by_name() for one specific target name pops
    results off a queue (one per call), falling back to PageNavUiTest's
    normal nav-map behavior once the queue is exhausted or for any other
    name -- used to exercise _click_resolving_swallow()'s/_click_then_page()'s
    walk_busy retry loop, which needs the SAME click to answer differently
    across consecutive calls."""

    def __init__(self, *a, queued_name=None, queued_results=None, **kw):
        super().__init__(*a, **kw)
        self._queued_name = queued_name
        self._queue = list(queued_results or [])
        self.calls_for_queued_name = 0

    def click_by_name(self, name):
        if name == self._queued_name and self._queue:
            self.calls_for_queued_name += 1
            result = self._queue.pop(0)
            if result == "ok":
                dest = self._nav_map.get(name)
                if dest is not None:
                    self._page = dest
            return {"result": result, "cx": 0, "cy": 0}
        return super().click_by_name(name)


class _QueuedListTapTargetsUi(FakeUiTest):
    """A UI double whose list_tap_targets() pops one result off a queue per
    call, falling back to repeating the last queued result once exhausted --
    used to exercise _list_tap_targets_resolving_busy()'s retry loop."""

    def __init__(self, *a, queued_results=None, **kw):
        super().__init__(*a, **kw)
        self._queue = list(queued_results or [])
        self.calls = 0

    def list_tap_targets(self):
        self.calls += 1
        if self._queue:
            result = self._queue.pop(0)
        else:
            result = {"targets": [], "truncated": False, "busy": False}
        return result


class ClickResolvingSwallowWalkBusyTest(unittest.TestCase):
    """Direct unit tests for _click_resolving_swallow()'s walk_busy arm
    (cases_lcd.py, 2026-09-30): a 'walk_busy' click result (KILN_UI_CLICK_
    WALK_BUSY / UI_TEST_CLICK_WALK_BUSY) must be retried, bounded by
    _CLICK_THEN_PAGE_WALK_BUSY_RETRIES, with its own counter kept separate
    from swallow_retries/not_found_retries."""

    def test_retries_once_then_succeeds_counts_one(self):
        ui = _QueuedClickUi(page="home", nav_map={"settings": "config"},
                             queued_name="settings",
                             queued_results=["walk_busy", "ok"])
        with mock.patch.object(C.time, "sleep"):
            click, swallow_retries, not_found_retries, walk_busy_retries = C._click_resolving_swallow(ui, "settings")
        self.assertEqual(click["result"], "ok")
        self.assertEqual(walk_busy_retries, 1)
        self.assertEqual(swallow_retries, 0)
        self.assertEqual(not_found_retries, 0)
        self.assertEqual(ui.calls_for_queued_name, 2)

    def test_gives_up_after_budget_returns_last_busy_read(self):
        # Budget is _CLICK_THEN_PAGE_WALK_BUSY_RETRIES (2 by default): the
        # first call plus 2 retries == 3 calls total, still busy.
        ui = _QueuedClickUi(page="home", nav_map={"settings": "config"},
                             queued_name="settings",
                             queued_results=["walk_busy", "walk_busy", "walk_busy", "ok"])
        with mock.patch.object(C.time, "sleep"):
            click, swallow_retries, not_found_retries, walk_busy_retries = C._click_resolving_swallow(ui, "settings")
        self.assertEqual(click["result"], "walk_busy")
        self.assertEqual(walk_busy_retries, C._CLICK_THEN_PAGE_WALK_BUSY_RETRIES)
        self.assertEqual(ui.calls_for_queued_name, 3)

    def test_click_then_page_attributes_walk_busy_after_budget_exhausted(self):
        ui = _QueuedClickUi(page="home", nav_map={"settings": "config"},
                             queued_name="settings",
                             queued_results=["walk_busy", "walk_busy", "walk_busy"])
        with mock.patch.object(C.time, "sleep"):
            fail, page, waited_s, swallow_retries, not_found_retries, walk_busy_retries = C._click_then_page(
                ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertEqual(fail.observed.get("attribution"), "walk_busy")
        self.assertEqual(fail.observed.get("walk_busy_retries"), C._CLICK_THEN_PAGE_WALK_BUSY_RETRIES)
        self.assertEqual(walk_busy_retries, 0)  # failure path's own tuple slot is always 0


class ClickThenTargetsChangeWalkBusyTest(unittest.TestCase):
    """Direct unit tests for _click_then_targets_change()'s single walk_busy
    retry (cases_lcd.py:~861-878, 2026-09-30, Opus review follow-up): a
    'walk_busy' click result is retried exactly once, same shape as
    _click_resolving_swallow()'s walk_busy arm but with its own one-shot
    budget (there is no separate retry counter here -- the function's
    return shape predates that addition and was not widened for it), before
    falling through to the same not-ok handling every other non-ok result
    uses. A non-walk_busy not-ok result (e.g. 'not_found') must still never
    be retried, same as before this fix landed."""

    _PAGE_TARGETS = {"home": [{"name": "a", "cx": 0, "cy": 0, "hidden": False}],
                      "config": [{"name": "b", "cx": 0, "cy": 0, "hidden": False}]}
    _NAV_MAP = {"settings": "config"}
    _PREV_NAMES = {"a"}

    def test_busy_once_then_ok_proceeds(self):
        ui = _QueuedClickUi(page="home", page_targets=self._PAGE_TARGETS, nav_map=self._NAV_MAP,
                             queued_name="settings", queued_results=["walk_busy", "ok"])
        with mock.patch.object(C.time, "sleep"):
            fail, tap, waited_s, changed = C._click_then_targets_change(ui, "settings", self._PREV_NAMES)
        self.assertIsNone(fail)
        self.assertTrue(changed)
        self.assertEqual({t.get("name") for t in tap.get("targets", [])}, {"b"})
        self.assertEqual(ui.calls_for_queued_name, 2)

    def test_busy_twice_attributes_walk_busy(self):
        # Only one retry is budgeted: a second consecutive 'walk_busy' falls
        # through to the same non-ok handling as any other not-ok result,
        # attributed as 'walk_busy' rather than folded into 'not_found'.
        ui = _QueuedClickUi(page="home", page_targets=self._PAGE_TARGETS, nav_map=self._NAV_MAP,
                             queued_name="settings", queued_results=["walk_busy", "walk_busy"])
        with mock.patch.object(C.time, "sleep"):
            fail, tap, waited_s, changed = C._click_then_targets_change(ui, "settings", self._PREV_NAMES)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertEqual(fail.observed.get("attribution"), "walk_busy")
        self.assertEqual(ui.calls_for_queued_name, 2)

    def test_not_found_is_never_retried(self):
        # Pinning pre-existing behavior: a plain 'not_found' (not
        # 'walk_busy') gets no retry at all here, exactly as before this
        # fix added the walk_busy-only retry above it.
        ui = _QueuedClickUi(page="home", page_targets=self._PAGE_TARGETS, nav_map=self._NAV_MAP,
                             queued_name="settings", queued_results=["not_found"])
        with mock.patch.object(C.time, "sleep"):
            fail, tap, waited_s, changed = C._click_then_targets_change(ui, "settings", self._PREV_NAMES)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.observed.get("attribution"), "not_found")
        self.assertEqual(ui.calls_for_queued_name, 1)


class ListTapTargetsResolvingBusyTest(unittest.TestCase):
    """Direct unit tests for _list_tap_targets_resolving_busy() (cases_lcd.py,
    2026-09-30 LCD-09 fix): retries a busy (count==0, truncated=True) read,
    stops as soon as a non-busy read arrives, and gives up after
    _LIST_TAP_TARGETS_BUSY_RETRIES, returning the last (still-busy) read."""

    _BUSY = {"targets": [], "truncated": True, "busy": True}

    def test_retries_while_busy_then_returns_real_read(self):
        real = {"targets": [{"name": "Profiles", "cx": 1, "cy": 2, "hidden": False}],
                "truncated": False, "busy": False}
        ui = _QueuedListTapTargetsUi(queued_results=[self._BUSY, real])
        with mock.patch.object(C.time, "sleep"):
            tap, busy_retries = C._list_tap_targets_resolving_busy(ui)
        self.assertEqual(tap, real)
        self.assertEqual(busy_retries, 1)
        self.assertEqual(ui.calls, 2)

    def test_non_busy_empty_read_returns_immediately_no_retry(self):
        # A genuinely empty page (truncated=False) must never be retried --
        # only the busy (truncated=True, count==0) shape is.
        empty = {"targets": [], "truncated": False, "busy": False}
        ui = _QueuedListTapTargetsUi(queued_results=[empty])
        with mock.patch.object(C.time, "sleep") as fake_sleep:
            tap, busy_retries = C._list_tap_targets_resolving_busy(ui)
        self.assertEqual(tap, empty)
        self.assertEqual(busy_retries, 0)
        self.assertEqual(ui.calls, 1)
        fake_sleep.assert_not_called()

    def test_gives_up_after_budget_returns_last_busy_read(self):
        ui = _QueuedListTapTargetsUi(queued_results=[self._BUSY, self._BUSY, self._BUSY])
        with mock.patch.object(C.time, "sleep"):
            tap, busy_retries = C._list_tap_targets_resolving_busy(ui)
        self.assertTrue(tap.get("busy"))
        self.assertEqual(busy_retries, C._LIST_TAP_TARGETS_BUSY_RETRIES)
        self.assertEqual(ui.calls, 1 + C._LIST_TAP_TARGETS_BUSY_RETRIES)


class Lcd09WalkBusyTest(unittest.TestCase):
    """_case_lcd09() must route its profile-list read through
    _list_tap_targets_resolving_busy() (2026-09-30 LCD-09 fix,
    logs/bench_test/20260930T234916Z_lcd_harness_retry_verify) and record the
    retries actually used in observed['walk_busy_retries']."""

    def test_busy_once_then_real_read_recorded_and_inconclusive(self):
        # 2026-09-30 Opus review: this fixture's "add" target is a literal
        # name, not the glyph bytes (U+FFFD) _is_glyph_name()/
        # _profiles_topbar_icons() actually require to recognise the New
        # icon, and it carries no "home" target at all -- so
        # _profiles_topbar_icons() can't find its back/home anchor and
        # judge_lcd_profiles_picker() reports INCONCLUSIVE, never PASS, for
        # this fixture. That is fine: this test's job is the busy-retry
        # plumbing (walk_busy_retries recorded, the resolving-busy helper
        # called exactly once), not the topbar-icon judgment, so the
        # verdict is pinned to what this fixture actually produces rather
        # than asserted away or left unchecked.
        real_targets = [
            {"name": "back", "cx": 10, "cy": 20, "hidden": False},
            {"name": "add", "cx": 400, "cy": 20, "hidden": False},
            {"name": "profile1", "cx": 240, "cy": 200, "hidden": False},
        ]
        ui = PageNavUiTest(
            page="home",
            page_targets={"home": [], "config": _CONFIG_TARGETS, "profiles": real_targets},
            nav_map={"settings": "config", "Profiles": "profiles", "profile1": "profile_detail"},
        )
        busy = {"targets": [], "truncated": True, "busy": True}
        real = {"targets": real_targets, "truncated": False, "busy": False}
        srv = FakeSrvFull(ui)
        # Only the FIRST read taken while parked on 'profiles' is busy --
        # _wake_and_home()'s own stray-overlay check (_lcd19_clear_stray_
        # overlay -> _lcd19_overlay_names) also calls list_tap_targets() once
        # while still on 'home', before _case_lcd09 ever navigates anywhere;
        # a plain queue (busy, real, ...) would hand that unrelated home-page
        # read the busy response instead of the profiles-page read this test
        # means to exercise.
        profiles_calls = {"n": 0}
        orig_list_tap_targets = PageNavUiTest.list_tap_targets

        def fake_list_tap_targets(self):
            if self._page == "profiles":
                profiles_calls["n"] += 1
                if profiles_calls["n"] == 1:
                    return busy
                return real
            return orig_list_tap_targets(self)

        with mock.patch.object(C, "_list_tap_targets_resolving_busy",
                                wraps=C._list_tap_targets_resolving_busy) as wrapped, \
             mock.patch.object(ui, "list_tap_targets", new=fake_list_tap_targets.__get__(ui)), \
             mock.patch.object(C.time, "sleep"):
            result = C._case_lcd09({"srv": srv})
        wrapped.assert_called_once()
        self.assertEqual(result.observed.get("walk_busy_retries"), 1)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


class EntryAllDigitsNotFoundWalkBusyTest(unittest.TestCase):
    """_entry_all_digits_not_found() (cases_lcd.py:2804, 2026-09-30 follow-up)
    must accept 'walk_busy' as equivalent to 'not_found' for both
    digit_results and ok_result, since enter_pin()'s own internal retry can
    still leave a digit/OK result as 'walk_busy' (not folded down to
    'not_found') after its own retry budget is exhausted."""

    def test_all_walk_busy_digits_and_ok_counts_as_all_not_found(self):
        entry = {
            "digit_results": [{"result": "walk_busy"}, {"result": "not_found"}],
            "ok_result": {"result": "walk_busy"},
        }
        self.assertTrue(C._entry_all_digits_not_found(entry))

    def test_one_ok_digit_result_is_not_all_not_found(self):
        entry = {
            "digit_results": [{"result": "walk_busy"}, {"result": "ok"}],
            "ok_result": {"result": "not_found"},
        }
        self.assertFalse(C._entry_all_digits_not_found(entry))

    def test_walk_busy_ok_result_with_not_found_digits_counts(self):
        entry = {
            "digit_results": [{"result": "not_found"}, {"result": "not_found"}],
            "ok_result": {"result": "walk_busy"},
        }
        self.assertTrue(C._entry_all_digits_not_found(entry))


class ThemeMirrorDriftTest(unittest.TestCase):
    """cases_lcd.py mirrors four ui_theme.h colors as reference values.
    Fails (never skips) if the header moves or a value drifts."""

    HEADER = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW",
                          "App", "drivers", "ui", "ui_theme.h")

    def _hex(self, text, macro):
        import re
        m = re.search(r"#define\s+" + macro + r"\s+0x([0-9a-fA-F]{6})\b", text)
        self.assertIsNotNone(m, f"{macro} not found in ui_theme.h")
        v = int(m.group(1), 16)
        return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)

    def test_mirrors_match_ui_theme_h(self):
        self.assertTrue(os.path.isfile(self.HEADER), f"missing {os.path.abspath(self.HEADER)}")
        with open(self.HEADER, encoding="utf-8") as fh:
            text = fh.read()
        self.assertEqual(C._BG_RGB, self._hex(text, "UI_THEME_COLOR_BG_HEX"))
        self.assertEqual(C._ACCENT_1_RGB, self._hex(text, "UI_THEME_ACCENT_1_HEX"))
        self.assertEqual(C._ACCENT_4_RGB, self._hex(text, "UI_THEME_ACCENT_4_HEX"))
        self.assertEqual(C._ACCENT_5_RGB, self._hex(text, "UI_THEME_ACCENT_5_HEX"))
        self.assertEqual(C._CARD_RGB, self._hex(text, "UI_THEME_COLOR_CARD_HEX"))
        self.assertEqual(C._TEXT_PRIMARY_RGB, self._hex(text, "UI_THEME_COLOR_TEXT_PRIMARY_HEX"))


class _CountingNavUi(PageNavUiTest):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.clicks = []

    def click_by_name(self, name):
        self.clicks.append(name)
        return super().click_by_name(name)


class _RecordingNavUi(PageNavUiTest):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.clicked = []

    def click_by_name(self, name):
        self.clicked.append(name)
        return super().click_by_name(name)


class FirstHopTimeoutCaseTest(unittest.TestCase):
    """Case-level negative test for the 2026-09-24 LCD-09/14/16 fix: the
    "settings" click replies 'ok' but the board never leaves home. Each
    case must FAIL naming that first hop and must never go on to click its
    second target -- even though (to make the regression observable) the
    second target is also offered on home and would navigate if clicked."""

    def _run(self, case_fn, second, dest):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}, {"name": second, "hidden": False}],
            dest: [],
        }
        ui = _RecordingNavUi(page="home", page_targets=page_targets, nav_map={second: dest})
        srv = FakeSrvFull(ui)
        with mock.patch.object(C._click_then_page, "__defaults__",
                                (0.05, C._CLICK_THEN_PAGE_MAX_RETRIES)):
            result = case_fn({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("'settings'", result.reason)
        self.assertIn("'config'", result.reason)
        self.assertNotIn(second, ui.clicked)

    def test_lcd09(self):
        self._run(C._case_lcd09, "Profiles", "profiles")

    def test_lcd14(self):
        self._run(C._case_lcd14, "Temperature", "temperature")

    def test_lcd16(self):
        self._run(C._case_lcd16, "Diagnostics", "diagnostics")


class Lcd09Test(unittest.TestCase):
    def test_passes_with_valid_picker(self):
        ui = PageNavUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV)
        srv = FakeSrvFull(ui)
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_menu_tap_failure_fails(self):
        ui = PageNavUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, click_result="not_found")
        srv = FakeSrvFull(ui)
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_row_tap_retries_once_on_swallowed_tap_then_passes(self):
        # 2026-09-24 bench root cause (LCD-09, three identical runs:
        # 20260924T191429Z/203338Z/221915Z): the row tap used to go through
        # a bare click_by_name()+_wait_for_page_change() with no retry,
        # unlike every other hop in this module -- the same
        # screen_idle/touch-poll swallow race _click_then_page() already
        # guards every other case against (see Lcd08Test's identical
        # scenario above). The first tap on the picker's row says "ok" but
        # the page never actually moves off "profiles"; only the retried
        # second tap lands on "profile_detail".
        class SwallowRowOnceUi(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.row_calls = 0

            def click_by_name(self, name):
                if name == "profile_row_0":
                    self.row_calls += 1
                    if self.row_calls == 1:
                        return {"result": "ok", "cx": 0, "cy": 0}  # swallowed
                    self._page = "profile_detail"
                    return {"result": "ok", "cx": 0, "cy": 0}
                return super().click_by_name(name)

        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False},
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        ui = SwallowRowOnceUi(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ui.row_calls, 2)

    def test_armed_delete_confirm_button_is_never_a_row_or_tapped(self):
        # Review of ebec7d06: the row tap now goes through the retrying
        # _click_then_page(), so rows[0] must never be a destructive
        # target. ui_page_profile_picker.c relabels an armed Delete button
        # "Confirm?" (and never resets the label when the window lapses);
        # a stale "Confirm?" sorting above the first row's name button must
        # be excluded from rows, never clicked -- let alone twice.
        class RecordingUi(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.clicks = []

            def click_by_name(self, name):
                self.clicks.append(name)
                return super().click_by_name(name)

        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "Confirm?", "cx": 400, "cy": 148, "hidden": False},
            {"name": "Cone 6 Bisque", "cx": 50, "cy": 150, "hidden": False},
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        nav["Cone 6 Bisque"] = "profile_detail"
        ui = RecordingUi(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertNotIn("Confirm?", ui.clicks)
        self.assertIn("Cone 6 Bisque", ui.clicks)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_finally_restores_home_on_exception(self):
        ui = RaisingUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd09({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)

    # Firmware-shaped profiles page: ui_topbar.c's untagged Prev/Next/New
    # icons are listed under their glyph label text, which the PC decodes
    # as U+FFFD bytes; only "back"/"home" carry tag names. Row 0 is kept so
    # the row-tap half of the judge is exercised.
    _GLYPH = "\ufffd\ufffd\ufffd"

    def _profiles_targets(self, slots):
        back = {"name": "back", "cx": 140, "cy": 26, "hidden": False}
        home = {"name": "home", "cx": 180, "cy": 26, "hidden": False}
        glyphs = [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in slots]
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [{"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False, "starred": False},
                            back, home] + glyphs
        return page

    def _run_firmware_shaped(self, slots):
        ui = PageNavUiTest(page="home", page_targets=self._profiles_targets(slots), nav_map=_PROFILES_NAV)
        return C._case_lcd09({"srv": FakeSrvFull(ui)})

    def test_glyph_icons_by_position_pass(self):
        self.assertEqual(self._run_firmware_shaped([1, 2, 3]).verdict, Verdict.PASS)

    def test_only_next_enabled_passes(self):
        self.assertEqual(self._run_firmware_shaped([2, 3]).verdict, Verdict.PASS)

    def test_single_page_both_paging_disabled_is_inconclusive(self):
        self.assertEqual(self._run_firmware_shaped([3]).verdict, Verdict.INCONCLUSIVE)

    def test_no_topbar_icons_fails(self):
        result = self._run_firmware_shaped([])
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Prev/Next", result.reason)

    def test_new_icon_missing_fails(self):
        self.assertEqual(self._run_firmware_shaped([1, 2]).verdict, Verdict.FAIL)

    def test_row_located_by_real_firmware_name_not_fixed_prefix(self):
        # build_row() (ui_page_profile_picker.c) tags no fixed name on a
        # row's own button -- kiln_ui.c falls back to the button's label
        # text, i.e. the profile's real name. A row named "Cone 6 Bisque"
        # (never matching a "profile_row_" prefix) must still be found by
        # _profile_rows_by_position() and drive a PASS.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "Cone 6 Bisque", "cx": 50, "cy": 150, "hidden": False},
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        nav["Cone 6 Bisque"] = "profile_detail"
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_full_page_with_unnamed_rows_col_container_passes(self):
        # Review 2026-09-24: the list's own `rows_col` container
        # (ui_page_profile_picker.c, plain lv_obj_create -> CLICKABLE by
        # default, walked with name "") sits at cy ~= 178, inside the list
        # band. A full 4-row page plus that container must read as 4 rows,
        # not 5 (a false max_rows FAIL), and never tap the container.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "", "cx": 240, "cy": 178, "hidden": False},
        ] + [
            {"name": "Profile %d" % k, "cx": 50, "cy": 74 + 32 * k, "hidden": False}
            for k in range(4)
        ] + [
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        nav["Profile 0"] = "profile_detail"
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_no_rows_with_known_positive_profiles_count_fails_not_inconclusive(self):
        # 2026-09-24 coordinator follow-up: an empty list area used to read
        # as merely undecidable even when the board itself reports profiles
        # exist -- that combination is a real stuck/empty-list defect.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=_PROFILES_NAV)
        srv = FakeSrvFull(ui, profiles=FakeProfiles(summaries=["cone6", "cone10"]))
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("profiles_count", result.reason)

    def test_no_rows_with_unknown_profiles_count_stays_inconclusive(self):
        # list_all() raising (e.g. link busy) must never fabricate a FAIL --
        # profiles_count stays None and the original undecidable verdict
        # is preserved.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=_PROFILES_NAV)
        srv = FakeSrvFull(ui, profiles=FakeProfiles(list_all_raises=RuntimeError("busy")))
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


_TEMP_NAV = {"settings": "config", "Temperature": "temperature"}
_TEMP_PAGE_TARGETS = {
    "home": [{"name": "settings", "hidden": False}],
    "config": [{"name": "Temperature", "hidden": False}],
    "temperature": [],
}


class Lcd14Test(unittest.TestCase):
    # Round 3 rewrite (item 3): zone_temp_/safety_line are not real tap
    # targets -- ui_page_temperature.c's zone rows and the Safety (K4) line
    # are plain, non-clickable labels, so LIST_TAP_TARGETS never reports
    # them. The judge is now driven by capture-based region sampling
    # (_sample_widget_off) against the fixed zone-row geometry derived from
    # ui_page_temperature.c's layout constants, not by named-target lookup.
    def _run_with_rows(self, rendered_zones, thermo=None, configured_zones=3,
                        header_rendered=True, control=None):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=thermo or FakeThermo({0: 100.0, 1: 100.0, 2: 100.0}),
                           profiles=FakeProfiles(FakeExecStatus("idle")),
                           control=control or FakeControl(zone_count=configured_zones))
        # The Relays card header samples at the SAME formula as a
        # (configured_zones)'th zone row (see cases_lcd.py's header_y
        # comment) -- model it as one more "row" slot in the same dict so
        # this fake doesn't need a second code path.
        rendered = dict(rendered_zones)
        rendered[configured_zones] = header_rendered

        def fake_sample_widget(image_path, cx, cy, repo_root=None):
            # Realistic model (2026-09-24 contrast-judge follow-up): a
            # rendered row's TEXT point (near _LCD14_ZONE_TEXT_X) reads
            # UI_THEME_COLOR_TEXT_PRIMARY against its own CARD background at
            # the BG-reference point (_LCD14_ROW_X); a MISSING row makes
            # BOTH points read the page's own BG -- never the bezel, which
            # neither point is anywhere near in real geometry.
            zone = round((cy - C._LCD14_ZONE_ROW_Y0) / C._LCD14_ZONE_ROW_PITCH)
            on = rendered.get(zone, False)
            is_text_point = abs(cx - C._LCD14_ZONE_TEXT_X) < abs(cx - C._LCD14_ROW_X)
            if zone == configured_zones and on:
                # The Relays header is ONE left-aligned label spanning
                # x ~14..256 (not a name/value pair with a gap at x=240 like
                # a zone row) -- any point in that span reads text.
                is_text_point = 14.0 <= cx <= 256.0
            if not on:
                region = C._BG_RGB
            else:
                region = C._TEXT_PRIMARY_RGB if is_text_point else C._CARD_RGB
            return lcd_sampler.RegionSample(region=region, bezel=(6, 13, 22))

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            return C._case_lcd14({"srv": srv})

    def test_all_rows_rendered_pass(self):
        result = self._run_with_rows({0: True, 1: True, 2: True})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_missing_row_fails(self):
        result = self._run_with_rows({0: True, 1: False, 2: True})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("1", result.reason)

    # -- Round 4: configured-zone-count cross-check (opus review of d66ba612) --

    def test_header_shifted_up_when_row_missing_fails(self):
        # The exact bug shape the review found: the board actually rendered
        # only 2 zone rows (0, 1) while 3 are configured -- rows 0/1 read as
        # rendered, but the Relays header is one row pitch higher than
        # header_y (58 + 3*32 = 154), so sampling AT header_y reads
        # background, not card content. A per-row-only check (the old code)
        # could not tell this apart from a genuine 3rd row landing there.
        result = self._run_with_rows({0: True, 1: True, 2: True}, configured_zones=3,
                                      header_rendered=False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_zero_zones_configured_label_rendered_passes(self):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({}), profiles=FakeProfiles(FakeExecStatus("idle")),
                           control=FakeControl(zone_count=0))

        def fake_sample_widget(image_path, cx, cy, repo_root=None):
            is_text_point = abs(cx - C._LCD14_ZONE_TEXT_X) < abs(cx - C._LCD14_ROW_X)
            region = C._TEXT_PRIMARY_RGB if is_text_point else C._BG_RGB
            return lcd_sampler.RegionSample(region=region, bezel=(6, 13, 22))

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_zone_count_unreadable_is_inconclusive(self):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")),
                           control=FakeControl(raises=RuntimeError("no reply")))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", return_value=lcd_sampler.RegionSample(
                 region=C._TEXT_PRIMARY_RGB, bezel=(6, 13, 22))), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("zones_count_error", result.observed)

    def test_no_capture_is_inconclusive(self):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_finally_restores_home_on_exception(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
        }
        ui = RaisingUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd14({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)


_DIAG_GLYPH = "���"
_DIAG_BACK = {"name": "back", "cx": 140, "cy": 26, "hidden": False}
_DIAG_HOME = {"name": "home", "cx": 180, "cy": 26, "hidden": False}
_DIAG_PITCH = 40.0
_DIAG_NEXT = {"name": _DIAG_GLYPH, "cx": 180 + 2 * _DIAG_PITCH, "cy": 26, "hidden": False}


class DowngradeIfCornersStaleTest(unittest.TestCase):
    """_downgrade_if_corners_stale() may only ever lower a FAIL to
    INCONCLUSIVE, and only on an explicit True from the self-check."""

    def _fail(self):
        return C.CaseResult(Verdict.FAIL, reason="wrong color", observed={"x": 1}, evidence=["e.jpg"])

    def _run(self, result, stale=None, raises=False):
        def fake(*a, **k):
            if raises:
                raise RuntimeError("boom")
            return stale
        with mock.patch.object(lcd_sampler, "frame_corners_look_stale", fake):
            return C._downgrade_if_corners_stale({}, result, "img.jpg")

    def test_pass_is_never_touched(self):
        r = C.CaseResult(Verdict.PASS)
        self.assertIs(self._run(r, stale=True), r)

    def test_fail_with_stale_corners_becomes_inconclusive_keeping_reason(self):
        out = self._run(self._fail(), stale=True)
        self.assertEqual(out.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(out.observed["original_fail_reason"], "wrong color")
        self.assertTrue(out.observed["frame_corners_stale"])
        self.assertEqual(out.evidence, ["e.jpg"])

    def test_fail_with_sound_corners_stays_fail(self):
        self.assertEqual(self._run(self._fail(), stale=False).verdict, Verdict.FAIL)

    def test_fail_with_undeterminable_corners_stays_fail(self):
        self.assertEqual(self._run(self._fail(), stale=None).verdict, Verdict.FAIL)

    def test_self_check_exception_stays_fail(self):
        self.assertEqual(self._run(self._fail(), raises=True).verdict, Verdict.FAIL)

    def test_no_image_stays_fail(self):
        with mock.patch.object(lcd_sampler, "frame_corners_look_stale", lambda *a, **k: True):
            self.assertEqual(C._downgrade_if_corners_stale({}, self._fail(), None).verdict, Verdict.FAIL)


class DiagPagingUi(PageNavUiTest):
    """Models ui_page_diagnostics.c's 8 sub-pages, reached only by the
    topbar's untagged Next icon (position-located, never click_by_name --
    see _diagnostics_next_target()'s docstring). ``step_content`` maps a
    sub-page index to the extra targets that page shows (e.g. an
    Acknowledge button on Crash Report, a still-present Reset on Relay
    Life for the regression check)."""

    def __init__(self, *a, step_content=None, page_count=8, swallow_step=None, **kw):
        super().__init__(*a, **kw)
        self.step = 0
        self._step_content = step_content or {}
        self._page_count = page_count
        self._swallow_step = swallow_step
        self._swallowed_once = set()

    def list_tap_targets(self):
        if self._page != "diagnostics":
            return super().list_tap_targets()
        targets = [_DIAG_BACK, _DIAG_HOME]
        if self.step > 0:
            targets.append({"name": _DIAG_GLYPH, "cx": 180 + 1 * _DIAG_PITCH, "cy": 26, "hidden": False})
        if self.step + 1 < self._page_count:
            targets.append(_DIAG_NEXT)
        targets.extend(self._step_content.get(self.step, []))
        return {"targets": targets, "truncated": False}


class FakeTouchAdvancesDiag:
    """A TouchClient double: inject()ing a press+release at Next's own
    cx/cy advances DiagPagingUi.step, exactly the raw-coordinate path
    _tap_next_then_targets_change() uses (click_by_name() cannot target
    Next -- Prev/Next share one undecodable glyph name)."""

    def __init__(self, ui):
        self._ui = ui
        self.injected = []

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))
        if not pressed and abs(x - (180 + _DIAG_PITCH)) < 1 and abs(y - 26) < 1:
            if self._ui.step > 0:
                self._ui.step -= 1  # Prev
            return
        if not pressed and abs(x - _DIAG_NEXT["cx"]) < 1 and abs(y - _DIAG_NEXT["cy"]) < 1:
            if self._ui._swallow_step == self._ui.step and self._ui.step not in self._ui._swallowed_once:
                self._ui._swallowed_once.add(self._ui.step)
                return  # swallowed once: step does not advance
            if self._ui.step + 1 < self._ui._page_count:
                self._ui.step += 1


def _diag_srv(ui):
    srv = FakeSrvFull(ui)
    srv._touch = FakeTouchAdvancesDiag(ui)
    return srv


_DIAG_NAV = {"settings": "config", "Diagnostics": "diagnostics"}
_DIAG_PAGE_TARGETS = {
    "home": [{"name": "settings", "hidden": False}],
    "config": [{"name": "Diagnostics", "hidden": False}],
}


class DiagPagingUiTransientRead(DiagPagingUi):
    """Models the 2026-09-24 race: a hop's own ENTRY read -- used to locate
    Next before tapping it -- lands right after the PREVIOUS hop's tap and
    can come back with the topbar's own back/home anchors transiently
    missing (board mid-redraw). The previous hop's own post-tap read (its
    boundary wait-for-change poll, or an interior hop's single trust read)
    is always the FIRST list_tap_targets() call observed at a given
    ``self.step`` value; the following hop's entry read is the SECOND. This
    blanks exactly that second read, once, for a chosen step."""

    def __init__(self, *a, blank_once_at_step=None, **kw):
        super().__init__(*a, **kw)
        self._blank_once_at_step = blank_once_at_step
        self._blanked = set()
        self._step_call_counts: dict = {}

    def list_tap_targets(self):
        if self._page == "diagnostics":
            count = self._step_call_counts.get(self.step, 0) + 1
            self._step_call_counts[self.step] = count
            if (self.step == self._blank_once_at_step and count == 2
                    and self.step not in self._blanked):
                self._blanked.add(self.step)
                return {"targets": [], "truncated": False}
        return super().list_tap_targets()


class DiagPagingUiTruncated(DiagPagingUi):
    """Diagnostics reads whose tap list is cut right after Home (the 253 B
    wire cap): Prev/Next/page content are dropped and ``truncated`` is set.
    ``truncate_first_reads`` truncates only that many diagnostics reads
    (None = every read)."""

    def __init__(self, *a, truncate_first_reads=None, **kw):
        super().__init__(*a, **kw)
        self._truncate_left = truncate_first_reads

    def list_tap_targets(self):
        if self._page != "diagnostics":
            return super().list_tap_targets()
        if self._truncate_left is not None:
            if self._truncate_left <= 0:
                return super().list_tap_targets()
            self._truncate_left -= 1
        return {"targets": [_DIAG_BACK, _DIAG_HOME], "truncated": True}


class Lcd16Test(unittest.TestCase):
    def test_finally_restores_home_on_exception(self):
        nav_map = {"settings": "config", "Diagnostics": "diag_hub"}
        ui = RaisingUiTest(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=nav_map, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd16({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)

    def test_menu_tap_failure_fails(self):
        ui = PageNavUiTest(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV, click_result="not_found")
        srv = FakeSrvFull(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_full_paging_with_ack_at_last_step_passes(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                           step_content={7: [{"name": "Acknowledge", "cx": 240, "cy": 280, "hidden": False}]})
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)
        self.assertEqual(result.observed.get("crash_report_step"), 7)

    def test_starts_on_last_subpage_rewinds_then_passes(self):
        # Pages are never torn down, so a prior run leaves the diagnostics
        # screen on its last sub-page; the case must page back to the first
        # before counting hops (bench run 20261001T172420Z: 0/7 hops).
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        ui.step = 7
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)
        self.assertEqual(result.observed.get("rewind_prev_taps"), 7)

    def test_starts_mid_subpage_rewinds_then_passes(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        ui.step = 3
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("rewind_prev_taps"), 3)

    def test_truncated_first_read_is_repolled_then_rewinds(self):
        # Back+Home present but Prev cut by truncation must not read as
        # "first sub-page"; the re-poll sees the full list and rewinds.
        ui = DiagPagingUiTruncated(page="home", page_targets=_DIAG_PAGE_TARGETS,
                                   nav_map=_DIAG_NAV, truncate_first_reads=1)
        ui.step = 3
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("rewind_prev_taps"), 3)
        self.assertNotIn("rewind_truncated", result.observed)

    def test_persistently_truncated_rewind_records_flag_not_page_one(self):
        ui = DiagPagingUiTruncated(page="home", page_targets=_DIAG_PAGE_TARGETS,
                                   nav_map=_DIAG_NAV)
        ui.step = 3
        srv = _diag_srv(ui)
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.3):
            result = C._case_lcd16({"srv": srv})
        self.assertNotEqual(result.verdict, Verdict.PASS)
        self.assertIs(result.observed.get("rewind_truncated"), True)
        self.assertNotIn("rewind_prev_taps", result.observed)
        self.assertIn("rewind", result.reason)
        self.assertEqual(ui.step, 3)  # nothing tapped without a located glyph
        self.assertNotIn((180 + _DIAG_PITCH, 26, True), srv._touch.injected)

    def test_rewind_helper_truncated_does_not_claim_first_page(self):
        ui = DiagPagingUiTruncated(page="diagnostics")
        ui.step = 2
        srv = _diag_srv(ui)
        tap = ui.list_tap_targets()
        tap, taps, trunc = C._rewind_diagnostics_to_first({"srv": srv}, ui, tap, timeout_s=0.3)
        self.assertEqual((taps, trunc), (0, True))

    def test_clean_start_sends_no_prev_taps(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertNotIn("rewind_prev_taps", result.observed)

    def test_relay_life_reset_still_present_fails(self):
        # Relay Life's own Reset control was fully removed
        # (UI_PLAN.md section 6.4) -- a reappearing Reset target is a
        # regression, not a pass.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                           step_content={3: [{"name": "Reset", "cx": 240, "cy": 280, "hidden": False}]})
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Reset", result.reason)

    def test_swallowed_next_tap_retries_once_then_passes(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV, swallow_step=0)
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)

    def test_no_retry_recorded_on_a_clean_run(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("retried_hops"), [])

    def test_swallowed_interior_tap_is_recorded_not_silent(self):
        # Interior hops are single, unconfirmed taps, so a tap dropped at
        # step 3 is only rescued by the LAST boundary hop's retry. The
        # final state is still correct (verdict unchanged), but the retry
        # must be visible in observed rather than silently absorbed.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV, swallow_step=3)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(ui.step, 7)
        self.assertEqual(result.observed.get("retried_hops"), [6])

    def test_next_stuck_after_retry_is_inconclusive(self):
        class StuckDiagUi(DiagPagingUi):
            pass

        ui = StuckDiagUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        srv = FakeSrvFull(ui)

        class StuckTouch:
            def __init__(self):
                self.injected = []

            def inject(self, x, y, pressed):
                self.injected.append((x, y, pressed))  # never advances ui.step

        srv._touch = StuckTouch()
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(result.observed.get("pages_paged"), 0)

    def test_truncated_tap_list_is_named_in_the_reason(self):
        # Review of ebec7d06: the LIST_TAP_TARGETS reply's 253 B wire cap
        # can cut the topbar icons from a sub-page with long row names
        # (Internal RAM); that read carries truncated=True. LCD-16 must
        # say so instead of only "Next may be broken".
        class TruncatingDiagUi(DiagPagingUi):
            def list_tap_targets(self):
                tap = super().list_tap_targets()
                if self._page == "diagnostics" and self.step >= 1:
                    return {"targets": [{"name": "Internal RAM free", "cx": 240, "cy": 120, "hidden": False}],
                            "truncated": True}
                return tap

        ui = TruncatingDiagUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertNotEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("tap_list_truncated_steps"), [1])
        self.assertIn("truncated", result.reason)

    def test_untruncated_run_records_no_truncation(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("tap_list_truncated_steps"), [])
        self.assertNotIn("truncated", result.reason or "")

    def test_interior_hop_transient_blank_read_recovers_without_extra_tap(self):
        # 2026-09-24 coordinator follow-up: an interior hop's own ENTRY read
        # (locating Next before tapping it) lands right after the PREVIOUS
        # hop's tap and raced the board's redraw, coming back with no
        # anchors at all -- previously misread as "Next is gone" and
        # stopped paging after the first hop. The stabilization poll must
        # recover WITHOUT sending any touch.
        #
        # blank_once_at_step=1 blanks step-1 hop's own entry read (step 1 is
        # interior: not step 0 or step expected_hops-1=6, so it uses
        # confirm=False and this fix's poll) -- the SECOND
        # list_tap_targets() call observed at step==1 (the first is step-0
        # hop's own post-tap wait-for-change read, which already tolerates
        # a blank reading on its own and must not be the one under test).
        ui = DiagPagingUiTransientRead(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                                        blank_once_at_step=1)
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)
        # Exactly one press+release pair per real hop, plus the one
        # _wake_and_home() wake tap this fake touch double has no
        # get_state() to satisfy (falls to need_wake=True) -- never a
        # doubled tap for the interior hop whose read was transiently blank.
        presses = [t for t in srv._touch.injected if t[2] is True]
        self.assertEqual(len(presses), 8)

    def test_interior_hop_still_blank_at_timeout_reports_next_missing(self):
        # Negative-test companion: if the anchors never come back at all
        # (a genuinely broken page, not merely a transient race), the poll
        # must give up at timeout_s and report found_next=False (paging
        # stops) rather than hang or fabricate a recovery.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)

        class _AlwaysBlankAfterFirstTap(FakeTouchAdvancesDiag):
            def inject(self, x, y, pressed):
                super().inject(x, y, pressed)
                if not pressed:
                    self._ui._force_blank = True

        srv = FakeSrvFull(ui)
        srv._touch = _AlwaysBlankAfterFirstTap(ui)
        ui._force_blank = False
        orig_list = ui.list_tap_targets

        def list_tap_targets():
            if ui._page == "diagnostics" and getattr(ui, "_force_blank", False) and ui.step >= 1:
                return {"targets": [], "truncated": False}
            return orig_list()

        ui.list_tap_targets = list_tap_targets
        with mock.patch.object(C._tap_next_then_targets_change, "__defaults__", (0.05, True)):
            result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.observed.get("pages_paged"), 1)
        presses = [t for t in srv._touch.injected if t[2] is True]
        self.assertEqual(len(presses), 2)

    def test_no_touch_transport_is_inconclusive(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        srv = FakeSrvFull(ui)  # no _touch attribute at all
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


class PopupUiTest(FakeUiTest):
    """A UiTestClient double for LCD-19: both the PIN keypad and the Confirm
    Start/Confirm Stop dialogs are top-layer popups that never change
    current_page (kiln_ui.c) -- this fake models that by keeping `_page`
    fixed at "home" and instead tracking an overlay's tap-target names,
    which appear only after `trigger_name` is clicked and disappear once
    "Cancel" is clicked (never any other button -- the case must never
    press the popup's own confirm button)."""

    #: Home always shows at least its own fire button in real firmware --
    #: the fake's baseline (no-overlay) listing must be non-empty too, or
    #: the "empty listing is never a real answer" fix (cases_lcd.py's
    #: `_lcd19_overlay_raw`) would treat every no-overlay read here as a
    #: stalled query and poll it out to the full timeout.
    _BASELINE_NAMES = ["Start"]

    def __init__(self, trigger_name, overlay_names):
        super().__init__(page="home", targets=[])
        self._trigger_name = trigger_name
        self._overlay_names = overlay_names
        self._overlay_open = False

    def list_tap_targets(self):
        names = list(self._BASELINE_NAMES)
        if self._overlay_open:
            names += list(self._overlay_names)
        return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}

    def click_by_name(self, name):
        if name == self._trigger_name and not self._overlay_open:
            self._overlay_open = True
            return {"result": "ok"}
        if name == "Cancel" and self._overlay_open:
            self._overlay_open = False
            return {"result": "ok"}
        return {"result": "not_found"}

    def close_overlay_via_backdrop(self):
        """Models `backdrop_click_cb()` (ui_lcd_keypad.c) -- the touch-inject
        dismiss path a paired :class:`BackdropTouch` fake calls into."""
        self._overlay_open = False


class BackdropTouch:
    """A TouchClient double whose `inject()` at the keypad-backdrop point
    (cases_lcd.py's `_KEYPAD_BACKDROP_XY`) closes the paired popup fake's
    overlay, modeling `backdrop_click_cb()` -- any other point is recorded
    but does nothing. Both press and release report ok."""

    def __init__(self, ui, backdrop_xy=(20, 160)):
        self._ui = ui
        self._backdrop_xy = backdrop_xy
        self.injected = []

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))
        if (x, y) == self._backdrop_xy and not pressed:
            self._ui.close_overlay_via_backdrop()
        return mock.Mock(ok=True)


class PinKeypadUiTest(FakeUiTest):
    """Models the real ui_lcd_keypad.c/ui_confirm.c state machine for
    LCD-19's idle-branch happy path, closely enough for enter_pin() to be
    exercised meaningfully: Start opens the keypad (digits 0-9 + "OK" +
    "Cancel"); submitting a wrong PIN resets the entry but leaves the
    keypad open ("OK" still present, same as lcd_keypad_state_submit()'s
    LCD_KEYPAD_SUBMIT_DENIED path); submitting the right PIN closes the
    keypad and opens the Confirm Start dialog ("OK" gone, "Start"/"Cancel"
    only)."""

    def __init__(self, right_pin: str, wrong_pin: str, policy_fn=None, cancel_raises=False):
        super().__init__(page="home", targets=[])
        self._right_pin = right_pin
        self._wrong_pin = wrong_pin
        # policy_fn() -> lcd_enabled: when given, Start with the policy off
        # goes straight to Confirm Start with no keypad, as
        # ui_lcd_lock_has_role() does on the real board.
        self._policy_fn = policy_fn
        self._cancel_raises = cancel_raises
        self._state = "idle"  # idle -> keypad -> confirm
        self._entry = ""

    def list_tap_targets(self):
        if self._state == "keypad":
            # Real shape (logs/bench_test/20260930T090222Z_lcd/summary.json):
            # empty-named backdrop/msgbox, the borrowed-container dots name,
            # digit/OK/Cancel keys, and the prompt -- no firmware tag.
            names = (
                ["", ""]
                + ["*" * len(self._entry)]
                + [str(d) for d in range(10)]
                + ["Cancel", "Enter PIN to start firing", "OK"]
            )
        elif self._state == "confirm":
            names = ["Start", "Cancel"]
        else:
            # Real home page always shows >=1 tap target even with no popup
            # open -- an empty listing here would now be treated as "no
            # read" (see cases_lcd.py's `_lcd19_overlay_raw`).
            names = ["Start"]
        return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}

    def click_by_name(self, name):
        if name == "Cancel" and self._cancel_raises:
            raise RuntimeError("simulated UI_TEST reply lost")
        if self._state == "idle":
            if name == "Start":
                if self._policy_fn is not None and not self._policy_fn():
                    self._state = "confirm"
                    return {"result": "ok"}
                self._state = "keypad"
                self._entry = ""
                return {"result": "ok"}
            return {"result": "not_found"}
        if self._state == "keypad":
            if name in "0123456789":
                self._entry += name
                return {"result": "ok"}
            if name == "OK":
                if self._entry == self._right_pin:
                    self._state = "confirm"
                else:
                    self._entry = ""
                return {"result": "ok"}
            if name == "Cancel":
                self._state = "idle"
                return {"result": "ok"}
            return {"result": "not_found"}
        if self._state == "confirm":
            if name == "Cancel":
                self._state = "idle"
                return {"result": "ok"}
            return {"result": "not_found"}
        return {"result": "not_found"}

    def enter_pin(self, pin):
        digit_results = [self.click_by_name(ch) for ch in pin]
        ok_result = self.click_by_name("OK")
        return {"digit_results": digit_results, "ok_result": ok_result}

    def enter_pin_verified(self, pin):
        # Models UiTestClient.enter_pin_verified() against this fake's own
        # real self._entry state: a click that reports "ok" here always DID
        # apply (this fixture has no silent-loss bug to model), so the dot
        # count -- len(self._entry) -- always matches immediately. Subclasses
        # that override click_by_name to return "ok" without actually
        # updating self._entry (modeling a real silent loss) will correctly
        # produce entry_incomplete=True here, same as the real client would.
        digit_results = []
        dot_wait_s = []
        for i, ch in enumerate(pin):
            click = self.click_by_name(ch)
            digit_results.append(click)
            if click.get("result") != "ok":
                # A click that isn't "ok" stops entry immediately -- OK must
                # never be pressed on a PIN known to be incomplete.
                dot_wait_s.append(0.0)
                return {
                    "digit_results": digit_results,
                    "ok_result": None,
                    "entry_incomplete": True,
                    "expected_dot_count": i + 1,
                    "observed_dot_count": None,
                    "dot_wait_s": dot_wait_s,
                }
            dot_wait_s.append(0.0)
            if len(self._entry) != i + 1:
                return {
                    "digit_results": digit_results,
                    "ok_result": None,
                    "entry_incomplete": True,
                    "expected_dot_count": i + 1,
                    "observed_dot_count": len(self._entry),
                    "dot_wait_s": dot_wait_s,
                }
        ok_result = self.click_by_name("OK")
        return {
            "digit_results": digit_results,
            "ok_result": ok_result,
            "entry_incomplete": False,
            "dot_wait_s": dot_wait_s,
        }


class FakeLcd19SecClient:
    """Minimal fake for LCD-19's own lcd_enabled on/off toggle (fix 1):
    tracks policy state across get_config/set_policy so the enable-before,
    restore-after round trip is real, not trivially true. Defaults to the
    bench's normal off state (web_enabled=False, lcd_enabled=False)."""

    def __init__(self, web_enabled=False, lcd_enabled=False,
                 web_timeout_min=30, lcd_timeout_min=30,
                 enable_ok=True, restore_ok=True):
        self._cfg = {
            "web_enabled": web_enabled, "lcd_enabled": lcd_enabled,
            "web_timeout_min": web_timeout_min, "lcd_timeout_min": lcd_timeout_min,
        }
        self.enable_ok = enable_ok
        self.restore_ok = restore_ok
        self.set_policy_calls = []

    def get_config(self):
        return 200, dict(self._cfg)

    def set_policy(self, web_enabled, lcd_enabled, web_timeout_min, lcd_timeout_min):
        self.set_policy_calls.append(lcd_enabled)
        if lcd_enabled and not self.enable_ok:
            return 200, {"ok": False}
        if not lcd_enabled and not self.restore_ok:
            return 200, {"ok": False}
        self._cfg["web_enabled"] = web_enabled
        self._cfg["lcd_enabled"] = lcd_enabled
        self._cfg["web_timeout_min"] = web_timeout_min
        self._cfg["lcd_timeout_min"] = lcd_timeout_min
        return 200, {"ok": True}


class _MismatchAfterRestoreSecClient(FakeLcd19SecClient):
    """Like FakeLcd19SecClient, but the SECOND get_config() call (the
    post-restore readback at cases_lcd.py's ``readback_status, cfg_after =
    client.get_config()``) reports lcd_enabled flipped from what was just
    written -- restore's own set_policy POST reports 200/ok (restore_post_ok
    True) while the readback still disagrees (restore_matches False), the
    other half of the ``not restore_post_ok or not restore_matches`` OR."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._get_config_calls = 0

    def get_config(self):
        self._get_config_calls += 1
        status, cfg = super().get_config()
        if self._get_config_calls >= 2:
            cfg = dict(cfg)
            cfg["lcd_enabled"] = not cfg["lcd_enabled"]
        return status, cfg


class DelayedOverlayUiTest(FakeUiTest):
    """``list_tap_targets`` returns ``pre`` for the first ``stale_reads``
    reads, then ``post`` -- models the real LCD-19 click-then-read race:
    home's own Start/nav buttons are non-empty (``pre``) even before LVGL
    has processed the click, and the popup's own buttons only replace them
    a couple of polls later (``post``)."""

    def __init__(self, pre, post, stale_reads=1):
        super().__init__(page="home", targets=[])
        self._pre = pre
        self._post = post
        self._stale_reads = stale_reads
        self._reads = 0

    def list_tap_targets(self):
        self._reads += 1
        names = self._pre if self._reads <= self._stale_reads else self._post
        return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}


class WaitForOverlayNamesBaselineTest(unittest.TestCase):
    """Unit tests for the LCD-19 baseline-wait fix: _wait_for_overlay_names
    used to treat `present=True` as "wait for a non-empty set", which the
    home page's own Start/nav buttons already satisfy before the popup
    ever appears -- a real bug found from a suspiciously fast (0.92s vs a
    2.0s timeout) LCD-19 FAIL. It must instead wait for the set to differ
    from a caller-supplied pre-click baseline."""

    def test_present_true_waits_past_stale_nonempty_baseline(self):
        ui = DelayedOverlayUiTest(pre={"Start", "settings"},
                                   post={"Start", "settings", "OK", "Cancel"},
                                   stale_reads=2)
        baseline = C._lcd19_overlay_names(ui)  # consumes one read, itself "pre"
        names, _waited, _empty, _trunc = C._wait_for_overlay_names(
            ui, present=True, baseline=baseline, timeout_s=1.0, interval_s=0.01)
        self.assertIsNotNone(names)
        self.assertIn("OK", names)
        self.assertIn("Cancel", names)

    def test_present_true_never_diverging_from_baseline_times_out_honestly(self):
        # A popup that never actually appears (baseline == post) must not
        # be reported as "present" just because the timeout elapsed --
        # the function returns the last (unsatisfying) read, not a lie.
        same = {"Start", "settings"}
        ui = DelayedOverlayUiTest(pre=same, post=same, stale_reads=0)
        baseline = C._lcd19_overlay_names(ui)
        names, waited, _empty, _trunc = C._wait_for_overlay_names(
            ui, present=True, baseline=baseline, timeout_s=0.05, interval_s=0.01)
        self.assertEqual(names, same)
        self.assertGreaterEqual(waited, 0.05)

    def test_present_true_without_baseline_falls_back_to_bare_nonempty(self):
        # Documents the fallback for a caller that omits `baseline`: the
        # OLD, buggy-for-LCD-19 bare-non-empty behavior. _case_lcd19 itself
        # always passes a baseline now; this is not an endorsement.
        ui = DelayedOverlayUiTest(pre={"Start", "settings"},
                                   post={"Start", "settings", "OK", "Cancel"})
        names, _waited, _empty, _trunc = C._wait_for_overlay_names(
            ui, present=True, timeout_s=1.0, interval_s=0.01)
        self.assertEqual(names, {"Start", "settings"})  # returns immediately, stale


class Lcd19SelfSeedTest(unittest.TestCase):
    """A standalone LCD-suite run (e.g. ``bench_test_run(suite="lcd")`` with
    only LCD-01/LCD-19 selected) never executes WEB-SEC-04 -- a different
    suite -- so ctx["_lcd_pin"] is never populated the way it is when both
    run in the same session. LCD-19 must be able to self-seed it through
    the same shared `cases_web_rw.seed_lcd_pin` helper WEB-SEC-04 itself
    uses, and must still NOT_RUN, naming the env var, when
    KILNCTL_LCD_PIN is genuinely unset -- self-seeding is not a way around
    that refusal."""

    def setUp(self):
        # mock.patch.dict + addCleanup restores whatever the process already
        # had for KILNCTL_LCD_PIN afterward (e.g. the owner's real User-scope
        # value) instead of unconditionally deleting it for the rest of the
        # process, the way a bare os.environ.pop in tearDown would.
        patcher = mock.patch.dict(os.environ, {}, clear=False)
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop(CW._LCD_PIN_ENV, None)

    def test_not_run_when_no_pin_configured_and_env_unset(self):
        srv = FakeSrvFull(FakeUiTest(page="home"))
        result = C._case_lcd19({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertIn(CW._LCD_PIN_ENV, result.reason)

    def test_self_seeds_from_env_when_ctx_lcd_pin_absent(self):
        # No ctx["_lcd_pin"] at all (as if WEB-SEC-04 never ran this
        # session), but KILNCTL_LCD_PIN is set and the board has no admin
        # PIN yet -- LCD-19 must write it itself via the shared helper
        # (never any path other than seed_lcd_pin's own set_lcd_pin call)
        # and then proceed to drive the keypad with it.
        os.environ[CW._LCD_PIN_ENV] = "1234"
        sec = FakeSec04Client(admin_pin_set=False)
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin=CW._derive_wrong_lcd_pin("1234"))
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": sec}
        result = C._case_lcd19(ctx)
        self.assertEqual(sec.set_lcd_pin_calls, [("admin", "1234")])
        self.assertEqual(ctx["_lcd_pin"], {"right_pin": "1234", "wrong_pin": "1235"})
        # The idle-branch keypad/PIN checks all completed (this fixture never
        # exercises the firing/Stop branch, so `stop_gated` stays None
        # and the overall verdict is judge_lcd_pin_lock's own by-design
        # INCONCLUSIVE for an idle-only run -- not what this test is about).
        self.assertEqual(result.observed.get("keypad_raised"), True)
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), True)

    def test_self_seed_does_not_overwrite_an_existing_admin_pin(self):
        # Same as WEB-SEC-04's own rule: never overwrite an admin PIN the
        # board already has -- trust KILNCTL_LCD_PIN already matches it.
        os.environ[CW._LCD_PIN_ENV] = "1234"
        sec = FakeSec04Client(admin_pin_set=True)
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin=CW._derive_wrong_lcd_pin("1234"))
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": sec}
        result = C._case_lcd19(ctx)
        self.assertEqual(sec.set_lcd_pin_calls, [])
        self.assertEqual(ctx["_lcd_pin"]["right_pin"], "1234")
        self.assertEqual(result.observed.get("keypad_raised"), True)
        self.assertEqual(result.observed.get("right_pin_started"), True)

    def test_self_seed_write_failure_fails_and_never_drives_keypad(self):
        os.environ[CW._LCD_PIN_ENV] = "1234"
        sec = FakeSec04Client(admin_pin_set=False, set_lcd_pin_ok=False)
        srv = FakeSrvFull(FakeUiTest(page="home", targets=_HOME_TARGETS))
        ctx = {"srv": srv, "sec_client": sec}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertNotIn("_lcd_pin", ctx)

    def test_self_seed_never_leaks_the_pin(self):
        import json
        os.environ[CW._LCD_PIN_ENV] = "8642097"
        srv = FakeSrvFull(FakeUiTest(page="home"))
        result = C._case_lcd19({"srv": srv, "sec_client": FakeSec04Client(admin_pin_set=False, set_lcd_pin_ok=False)})
        blob = (result.reason or "") + json.dumps(result.observed or {}, default=str)
        self.assertNotIn("8642097", blob)

    def test_self_seed_success_path_never_leaks_the_pin_and_reports_pin_seed(self):
        # Same leak check as above, but on the success path (a real
        # set_lcd_pin write happens and the case runs through to a verdict)
        # rather than an immediate write-failure abort -- and additionally
        # confirms the write is visible in `observed["pin_seed"]` without
        # ever including the PIN value itself.
        import json

        os.environ[CW._LCD_PIN_ENV] = "9137456"
        sec = FakeSec04Client(admin_pin_set=False)
        ui = PinKeypadUiTest(right_pin="9137456", wrong_pin=CW._derive_wrong_lcd_pin("9137456"))
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": sec}
        result = C._case_lcd19(ctx)
        self.assertEqual(sec.set_lcd_pin_calls, [("admin", "9137456")])
        pin_seed = result.observed.get("pin_seed")
        self.assertIsNotNone(pin_seed)
        self.assertEqual(pin_seed.get("admin_pin_set_before"), False)
        self.assertEqual(pin_seed.get("set_lcd_pin_status"), 200)
        blob = (result.reason or "") + json.dumps(result.observed or {}, default=str)
        self.assertNotIn("9137456", blob)


class Lcd19Test(unittest.TestCase):
    def setUp(self):
        # 2026-09-30 review scope-addition: _wait_for_home_settled (the
        # Start-click settle-wait, added alongside the pre-existing Stop-
        # click one) does a real `time.sleep(2.5)` per call by default --
        # every Start-driving test in this class now goes through it, so a
        # fake, instantly-advancing monotonic clock keeps this test file
        # fast and deterministic rather than adding 2.5s+ of real wall time
        # per test. ctx["_now"]/ctx["_sleep"] are not used here (most tests
        # build ctx by hand without them); patching the module's own `time`
        # functions is the one change that reaches every call site
        # (_wait_for_home_settled's `time.monotonic` default AND
        # _wait_for_keypad_raise's direct use of `time.monotonic`/
        # `time.sleep`) without touching 28 separate ctx literals.
        fake_time = [0.0]
        self.sleep_calls = []

        def fake_monotonic():
            return fake_time[0]

        def fake_sleep(seconds):
            fake_time[0] += seconds
            self.sleep_calls.append(seconds)

        patcher = mock.patch.multiple(C.time, monotonic=fake_monotonic, sleep=fake_sleep)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_start_click_waits_for_settle_after_enabling_lcd(self):
        # 2026-09-30 review scope-addition: the `set_policy(lcd_enabled=True)`
        # enable at case entry trips the same tick_timer_cb self-close race
        # `_wait_for_home_settled` exists for -- clicking "Start" right after
        # that POST with no wait can lose the race. Proves the settle-wait is
        # actually invoked (set_policy recorded) and completes strictly
        # before the Start click.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        sec = FakeLcd19SecClient()
        events = []
        real_set_policy = sec.set_policy

        def recording_set_policy(*a, **kw):
            events.append(("set_policy", a, kw))
            return real_set_policy(*a, **kw)

        sec.set_policy = recording_set_policy
        real_click = ui.click_by_name

        def recording_click(name):
            if name == "Start":
                events.append(("click_start",))
            return real_click(name)

        ui.click_by_name = recording_click
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": sec,
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        set_policy_events = [i for i, e in enumerate(events) if e[0] == "set_policy"]
        click_events = [i for i, e in enumerate(events) if e[0] == "click_start"]
        self.assertTrue(set_policy_events, "expected the enabling set_policy call")
        self.assertTrue(click_events, "Start was never clicked")
        # set_policy_events[0] is the entry-point enable call; a later
        # set_policy (if any) is the unrelated end-of-case policy restore in
        # `finally`, not part of this ordering check.
        self.assertLess(set_policy_events[0], click_events[0],
                         "the settle-wait (after set_policy) must complete before the Start click")
        self.assertTrue(result.observed.get("start_settle_ok"))
        # Proves the wait itself actually ran (>= the 2.5s / 2-tick-period
        # minimum), not merely that set_policy happens to precede the click
        # for unrelated reasons -- removing the wait call entirely would
        # still leave that ordering intact, so this is the assertion that
        # actually catches that mutation.
        self.assertTrue(self.sleep_calls, "the settle-wait's minimum sleep was never called")
        self.assertGreaterEqual(self.sleep_calls[0], 2.5)

    def test_not_run_when_no_pin_configured(self):
        # This test must never reach the real board: on a machine where the
        # owner's KILNCTL_LCD_PIN is genuinely set (User scope), an
        # unpatched env plus no sec_client/host in ctx would make
        # _case_lcd19 self-seed for real -- a live GET /api/auth/config,
        # and possibly a real set_lcd_pin write. Force the env var unset for
        # the duration of this test regardless of the real environment, and
        # supply a fake sec client that must see zero calls, so this test
        # can never touch a board even if it did fall through.
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(CW._LCD_PIN_ENV, None)
            sec = FakeSec04Client(admin_pin_set=False)
            srv = FakeSrvFull(FakeUiTest(page="home"))
            result = C._case_lcd19({"srv": srv, "sec_client": sec})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertEqual(sec.set_lcd_pin_calls, [])

    def test_negative_unpatched_env_and_no_fake_client_would_resolve_real_host(self):
        # Proves the hazard the test above guards against is real: with
        # KILNCTL_LCD_PIN set (as it is on the owner's machine) and no
        # sec_client/host override in ctx -- i.e. the shape of the test
        # above BEFORE it patched the environment and supplied a fake
        # client -- _case_lcd19 does reach _sec_client's real-host fallback.
        # Patch _ota_resolve_host to raise instead of actually resolving
        # anything, and confirm _case_lcd19 propagates that raise rather
        # than silently succeeding some other way.
        from kilnctrl import mcp_server_ota

        with mock.patch.dict(os.environ, {CW._LCD_PIN_ENV: "1234"}):
            with mock.patch.object(
                mcp_server_ota, "_ota_resolve_host", side_effect=RuntimeError("would resolve a real host")
            ):
                srv = FakeSrvFull(FakeUiTest(page="home"))
                with self.assertRaises(RuntimeError):
                    C._case_lcd19({"srv": srv})

    def test_wakes_and_homes_before_driving_the_keypad(self):
        # 2026-09-24 bench root cause (LCD-19, three identical runs:
        # 20260924T191429Z/203338Z/221915Z): this case went straight to
        # click_by_name("Start")/("Stop") with no _wake_and_home() call --
        # the one exception among every click-driven LCD case. On a bench
        # run where the panel had blanked or drifted off `home` since the
        # previous case, that first click legitimately failed (a blanked
        # panel swallows the tap; a stale page has no "Start"/"Stop"
        # widget), leaving all four PIN-lock booleans at None -- the
        # observed INCONCLUSIVE. Pin that _wake_and_home() runs before the
        # click.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_wake_and_home") as wake:
            C._case_lcd19(ctx)
        wake.assert_called_once_with(ctx)

    def test_wake_and_home_precedes_every_click(self):
        # Review of ebec7d06: the test above only pins that the wake ran,
        # not that it ran BEFORE the first click -- which is the claimed fix.
        order = []
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        real_click = ui.click_by_name

        def recording_click(name):
            order.append(("click", name))
            return real_click(name)

        ui.click_by_name = recording_click
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_wake_and_home", side_effect=lambda c: order.append(("wake",))):
            C._case_lcd19(ctx)
        self.assertTrue(any(o[0] == "click" for o in order), order)
        self.assertEqual(order[0], ("wake",), order)

    def test_owns_its_own_lcd_enabled_toggle_and_restores_it_off(self):
        # Fix 1: LCD-19 must not depend on WEB-SEC-04 leaving lcd_enabled on
        # -- it turns the policy on itself before driving the keypad and
        # restores the original (off) value afterward, verified by readback.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(lcd_enabled=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        # True while the keypad was being driven, then restored to False.
        self.assertIn(True, sec.set_policy_calls)
        self.assertEqual(sec.set_policy_calls[-1], False)
        self.assertEqual(sec._cfg["lcd_enabled"], False)
        self.assertTrue(result.observed["restore"]["readback_matches"])

    def test_enable_not_confirmed_fails_without_touching_keypad(self):
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(enable_ok=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            result = C._case_lcd19(ctx)
        spy.assert_not_called()
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_restore_failure_fails_even_if_keypad_flow_passed(self):
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(restore_ok=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_idle_click_uses_exact_start_label_and_detects_keypad(self):
        # kiln_ui_click_by_name() (firmware/KilnFW/App/drivers/ui/kiln_ui.c)
        # matches with an exact strcmp; the home fire button's label is
        # exactly "Start" when idle (ui_page_home.c). A lowercase "start"
        # never matches and always returns NOT_FOUND. Tapping "Start" opens
        # the PIN keypad (ui_lcd_keypad.c: digit buttons + "OK" + "Cancel"),
        # a top-layer popup -- current_page stays "home", so the keypad must
        # be detected from tap-target names, never a page name (no page is
        # ever named "pin_entry").
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
            result = C._case_lcd19(ctx)
        spy.assert_any_call("Start")
        self.assertNotIn(mock.call("start"), spy.call_args_list)
        self.assertEqual(result.observed.get("keypad_raised"), True)
        # enter_pin() now drives the keypad: a wrong PIN is refused (keypad
        # stays open) and the right PIN opens Confirm Start. stop_gated
        # is never observed on the idle branch, so the case still tops out
        # at INCONCLUSIVE, not PASS -- see judge_lcd_pin_lock.
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        # The Confirm Start dialog left open by the right PIN must not be
        # left open on the board afterward.
        self.assertEqual(result.observed["overlay_dismiss"]["present"], True)
        self.assertTrue(result.observed["overlay_dismiss"]["dismissed"])
        self.assertEqual(
            [t["name"] for t in ui.list_tap_targets()["targets"]], ["Start"]
        )

    def test_start_click_that_changes_nothing_fails_with_names_recorded(self):
        # The Start click is acknowledged but no popup appears: the
        # post-click set never diverges from the pre-click baseline, so the
        # keypad is not raised, the case FAILs, and the raw names it saw
        # are still recorded in observed for the operator.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        real_click = ui.click_by_name
        ui.click_by_name = lambda name: {"result": "ok"} if name == "Start" else real_click(name)
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.05):
            result = C._case_lcd19(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIs(result.observed.get("keypad_raised"), False)
        self.assertIn("after_start_click_names", result.observed)
        # Baseline itself ("Start") is what a bare-non-empty check would
        # have wrongly matched immediately -- it never diverges here, so
        # the wait times out and reports that same unchanged set, not [].
        self.assertEqual(result.observed["after_start_click_names"], ["Start"])

    def test_restore_runs_even_if_overlay_dismiss_raises(self):
        # The dismiss is a UART round trip that can raise; the HTTP policy
        # restore must still run so lcd_enabled is never left on.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000", cancel_raises=True)
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(lcd_enabled=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(sec._cfg["lcd_enabled"], False)
        self.assertTrue(result.observed["restore"]["readback_matches"])
        self.assertEqual(result.observed["overlay_dismiss"].get("checked"), False)

    def test_pins_never_in_result(self):
        import json
        right, wrong = "8642097", "8642098"
        for ui in (PinKeypadUiTest(right_pin=right, wrong_pin=wrong),
                   PinKeypadUiTest(right_pin=wrong, wrong_pin=wrong)):
            srv = FakeSrvFull(ui)
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
                   "_lcd_pin": {"right_pin": right, "wrong_pin": wrong}}
            result = C._case_lcd19(ctx)
            blob = (result.reason or "") + json.dumps(result.observed or {}, default=str)
            self.assertNotIn(right, blob)
            self.assertNotIn(wrong, blob)

    def test_keypad_that_ignores_ok_never_passes_right_pin(self):
        # Vacuity guard: a keypad that never reacts to OK leaves the button
        # set unchanged for BOTH PINs. The wrong-PIN read alone can't tell
        # that apart from a refusal, so right_pin_started must be False
        # (keypad still open), never True -- the right-PIN close is the
        # signal that distinguishes the two verdicts.
        ui = PinKeypadUiTest(right_pin="never-matches", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_wrong_pin_actually_granted_fails(self):
        # Negative test: if a wrong PIN were incorrectly accepted (keypad
        # closes to Confirm Start instead of staying open), this must FAIL,
        # not read as refused. Model that by making the fake's "right_pin"
        # equal to the configured wrong_pin -- i.e. the board grants entry
        # on the wrong PIN.
        ui = PinKeypadUiTest(right_pin="0000", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("wrong_pin_refused"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_dropped_digit_leaves_refused_none_but_records_why(self):
        # 2026-09-24 bench root cause (20260925T055159Z_full): keypad_raised
        # was true but wrong_pin_refused/right_pin_started/stop_gated
        # all read None with nothing in `observed` to explain why --
        # _entry_all_clicked_ok() correctly refuses to draw a conclusion
        # when a digit click didn't land "ok", but the `entry` it inspected
        # was discarded, so the INCONCLUSIVE was undiagnosable from
        # summary.json alone. This models a dropped digit (one click
        # reporting "swallowed" instead of "ok") and asserts the redacted
        # entry summary now lands in `observed` -- result codes and a count
        # only, never the digit identities/order/coordinates that would
        # reconstruct the PIN. 2026-09-30 review fix: enter_pin_verified()
        # now stops entry on the FIRST non-"ok" click, so only that one
        # click is attempted/recorded (digit_count 1), not all 4.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        real_click = ui.click_by_name
        call_count = {"n": 0}

        def flaky_click(name):
            if ui._state == "keypad" and name in "0123456789":
                call_count["n"] += 1
                if call_count["n"] == 1:
                    ui._entry += name  # firmware still recorded the digit
                    return {"result": "swallowed", "cx": 1, "cy": 1}
            return real_click(name)

        ui.click_by_name = flaky_click
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        entry_summary = result.observed.get("wrong_pin_entry")
        self.assertIsNotNone(entry_summary, "the dropped-click entry must be recorded even when no verdict follows")
        self.assertEqual(entry_summary["present"], True)
        self.assertEqual(entry_summary["digit_count"], 1)
        self.assertEqual(entry_summary["digit_results"][0], "swallowed")
        self.assertNotIn("cx", entry_summary)
        self.assertNotIn("cy", entry_summary)
        self.assertNotIn("right_pin_entry", result.observed)  # never reached: wrong_pin_refused is None
        import json
        blob = json.dumps(result.observed, default=str)
        self.assertNotIn("0000", blob)
        self.assertNotIn("1234", blob)

    def test_wrong_pin_refused_stays_none_when_post_submit_reads_are_empty(self):
        # 2026-09-30 bench regression: `wrong_pin_refused = names is not
        # None and "OK" in names and "Cancel" in names` collapsed a
        # read-timeout (names is None) straight to False, which
        # judge_lcd_pin_lock then reported as a hard FAIL ("a wrong PIN was
        # not refused") even though no real read was ever obtained. A read
        # that never recovers within the poll window must leave
        # wrong_pin_refused at its initial None (INCONCLUSIVE) instead.
        class _EmptyAfterWrongPinUiTest(PinKeypadUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self._wrong_pin_submitted = False

            def click_by_name(self, name):
                result = super().click_by_name(name)
                if name == "OK" and self._state == "keypad" and self._entry == "":
                    # A submit that left us back in "keypad" state with a
                    # cleared entry is exactly the wrong-PIN-refused signal
                    # -- mark it so subsequent reads can be stalled.
                    self._wrong_pin_submitted = True
                return result

            def list_tap_targets(self):
                if self._wrong_pin_submitted:
                    # Model a stalled board that never answers again.
                    return {"targets": [], "truncated": True}
                return super().list_tap_targets()

        ui = _EmptyAfterWrongPinUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_PIN_SUBMIT_POLL_TIMEOUT_S", 0.05):
            result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("keypad_raised"), True)
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_right_pin_started_stays_none_when_post_submit_reads_are_empty(self):
        # Same collapse-to-False bug, for the correct-PIN submit: the
        # keypad-to-Confirm-Start handoff stalling must never read as
        # right_pin_started=False ("the correct PIN did not start the
        # firing") -- only as INCONCLUSIVE.
        class _EmptyAfterRightPinUiTest(PinKeypadUiTest):
            def list_tap_targets(self):
                if self._state == "confirm":
                    return {"targets": [], "truncated": True}
                return super().list_tap_targets()

        ui = _EmptyAfterRightPinUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_PIN_SUBMIT_POLL_TIMEOUT_S", 0.05):
            result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("keypad_raised"), True)
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_right_pin_started_stays_none_when_post_submit_reads_never_stabilize(self):
        # 2026-09-30 review finding: `names is not None` alone is not
        # enough -- `_wait_stable_names` can time out on a real, non-empty
        # read that never repeated (e.g. the confirm dialog's own tap-target
        # set still changing between polls), which is exactly as
        # inconclusive as an empty/timeout read. Model that by alternating
        # between two different non-empty, "Cancel"-only (no "OK") sets
        # forever after the right PIN is submitted -- never a stable
        # 2-in-a-row repeat -- and confirm right_pin_started stays None
        # (INCONCLUSIVE), not a fabricated False from trusting the last,
        # never-repeated read.
        class _AlternatingAfterRightPinUiTest(PinKeypadUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self._confirm_read_n = 0

            def list_tap_targets(self):
                if self._state == "confirm":
                    self._confirm_read_n += 1
                    # Two different non-empty sets, alternating every read --
                    # both lack "OK" and contain "Cancel" (a naive
                    # `names is not None` check would happily accept
                    # either), but neither ever repeats back-to-back.
                    if self._confirm_read_n % 2 == 1:
                        names = ["Start", "Cancel"]
                    else:
                        names = ["Start", "Cancel", "Extra"]
                    return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}
                return super().list_tap_targets()

        ui = _AlternatingAfterRightPinUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_PIN_SUBMIT_POLL_TIMEOUT_S", 0.05):
            result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("keypad_raised"), True)
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertIsNotNone(result.observed.get("after_right_pin_names"))  # last read kept for diagnostics
        self.assertEqual(result.observed.get("after_right_pin_stabilized"), False)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_entry_waits_for_a_stable_keypad_read_before_typing(self):
        # 2026-09-30 fix: `keypad_raised` only proves the post-click read
        # differed from the pre-click baseline -- it says nothing about
        # whether the overlay-raise transition has actually settled. The
        # normal PinKeypadUiTest fixture already reads stable immediately,
        # so this pins that the new before-entry wait runs and passes (and
        # is visible in `observed`) on the ordinary happy path, without
        # changing that path's outcome.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("before_entry_stabilized"), True)
        self.assertIn("0", result.observed.get("before_entry_names") or [])
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), True)

    def test_entry_skipped_and_inconclusive_when_keypad_never_stabilizes(self):
        # 2026-09-30 bench evidence (20260930T082643Z_lcd/082657Z_lcd): the
        # overlay-raise transition was still settling when digit entry
        # began, and every wrong-PIN digit click plus the trailing "OK"
        # click came back "not_found". Model a keypad read that never
        # settles (alternates forever) so the new before-entry stabilization
        # wait times out honestly -- entry must never be attempted, and the
        # case must land on INCONCLUSIVE, never FAIL.
        class NeverStableKeypadUiTest(PinKeypadUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self._keypad_read_n = 0

            def list_tap_targets(self):
                if self._state == "keypad":
                    self._keypad_read_n += 1
                    base = [str(d) for d in range(10)] + ["OK", "Cancel"]
                    if self._keypad_read_n % 2 == 0:
                        base = base + ["Extra"]
                    return {"targets": [{"name": n, "hidden": False} for n in base], "truncated": False}
                return super().list_tap_targets()

        ui = NeverStableKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_PIN_SUBMIT_POLL_TIMEOUT_S", 0.05):
            with mock.patch.object(ui, "enter_pin", wraps=ui.enter_pin) as spy:
                result = C._case_lcd19(ctx)
        spy.assert_not_called()
        self.assertEqual(result.observed.get("before_entry_stabilized"), False)
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_all_digit_and_ok_clicks_not_found_is_inconclusive_with_marker(self):
        # 2026-09-30 bench shape: the keypad's tap-target *reads* were
        # stable and digit-bearing (so the new before-entry wait passes),
        # but every digit click AND the trailing "OK" click still came back
        # "not_found" -- consistent with each click's own 300ms
        # UI_WALK_WAIT_TIMEOUT_MS busy window, distinct from a genuinely
        # absent target. enter_pin_verified() (2026-09-30 review fix)
        # now stops entry on the FIRST non-"ok" click rather than
        # attempting every digit -- so this reports `entry_incomplete`
        # after just one click, not the older `entry_all_not_found`
        # all-clicks-attempted marker. Either way this must stay
        # INCONCLUSIVE, never FAIL.
        class AllNotFoundKeypadUiTest(PinKeypadUiTest):
            def click_by_name(self, name):
                if self._state == "keypad" and (name in "0123456789" or name == "OK"):
                    return {"result": "not_found"}
                return super().click_by_name(name)

        ui = AllNotFoundKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("before_entry_stabilized"), True)
        wrong_entry = result.observed.get("wrong_pin_entry") or {}
        self.assertTrue(wrong_entry.get("entry_incomplete"))
        self.assertIsNone(wrong_entry.get("ok_result"))
        self.assertEqual(len(wrong_entry.get("digit_results") or []), 1)
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_stop_raises_keypad_is_gated(self):
        # Owner decision 2026-09-28 ("stop needs login. there is an estop
        # button"): with the session locked, a Stop tap must raise the PIN
        # keypad ("OK" present alongside "Cancel") rather than Confirm Stop.
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "OK", "Cancel"])
        srv = FakeSrvFull(ui)
        # "OK" is present alongside "Cancel" -- this is the keypad shape, so
        # dismissal in `finally` must go through the backdrop touch-inject
        # path, never click_by_name("Cancel") (see fix 2).
        srv._touch = BackdropTouch(ui)
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000", "firing_active_with_lock": True}}
            result = C._case_lcd19(ctx)
        self.assertEqual(spy.call_args_list[0], mock.call("Stop"))
        self.assertNotIn(mock.call("Start"), spy.call_args_list)
        self.assertNotIn(mock.call("Cancel"), spy.call_args_list)
        self.assertEqual(result.observed.get("stop_gated"), True)
        self.assertNotEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(result.observed["overlay_dismiss"]["dismiss_method"], "backdrop_touch_inject")
        self.assertTrue(result.observed["overlay_dismiss"]["dismissed"])
        self.assertIn((20, 160, True), srv._touch.injected)
        self.assertIn((20, 160, False), srv._touch.injected)

    def test_stop_opens_confirm_dialog_directly_fails(self):
        # A Confirm Stop dialog (ui_confirm.c) has only its confirm_label
        # ("Stop") + "Cancel" -- no "OK". Stop landing directly on Confirm
        # Stop with the session locked means Stop was NOT gated -- a
        # regression against the 2026-09-28 owner decision, and a real FAIL.
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["Stop", "Cancel"])
        srv = FakeSrvFull(ui)
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000", "firing_active_with_lock": True}}
            result = C._case_lcd19(ctx)
        self.assertEqual(spy.call_args_list[0], mock.call("Stop"))
        self.assertNotIn(mock.call("Start"), spy.call_args_list)
        # Dismissal in `finally` must use "Cancel" only, never the dialog's
        # own confirm button.
        self.assertEqual(spy.call_args_list[-1], mock.call("Cancel"))
        self.assertEqual(result.observed.get("stop_gated"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Stop", result.reason)
        self.assertTrue(result.observed["overlay_dismiss"]["dismissed"])

    def test_dot_count_advances_normally_flow_proceeds(self):
        # LCD-19 (2026-09-30): the happy path for enter_pin_verified() --
        # every digit's dot count advances exactly as expected, so both
        # entries complete and the boolean pair gets a real verdict rather
        # than an INCONCLUSIVE from an incomplete entry.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertFalse((result.observed.get("wrong_pin_entry") or {}).get("entry_incomplete", False))
        self.assertFalse((result.observed.get("right_pin_entry") or {}).get("entry_incomplete", False))
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), True)

    def test_wrong_pin_refused_stays_none_when_dots_dont_clear(self):
        # Review advisory (2026-09-30, after 72bbd810): "OK"+"Cancel" still
        # present after a wrong-PIN OK is not by itself proof the entry was
        # reset -- the masked-PIN-dots count must also have cleared to 0.
        # Models a stuck-dots defect: the wrong PIN is refused (keypad
        # stays open, doesn't advance to Confirm Start) but the dots label
        # never resets to "" the way ui_lcd_keypad.c's real reset does.
        # wrong_pin_refused must stay None (INCONCLUSIVE), never a
        # fabricated True.
        class StuckDotsKeypadUiTest(PinKeypadUiTest):
            def click_by_name(self, name):
                if self._state == "keypad" and name == "OK" and self._entry != self._right_pin:
                    # Real reset (base class) sets self._entry = "" here --
                    # this fixture deliberately does not, modeling the dots
                    # label failing to clear.
                    return {"result": "ok"}
                return super().click_by_name(name)

        ui = StuckDotsKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertFalse((result.observed.get("wrong_pin_entry") or {}).get("entry_incomplete", False))
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_lost_digit_stops_entry_ok_never_pressed_inconclusive(self):
        # LCD-19 real bench root cause (20260930T090222Z_lcd): a digit click
        # reported "ok" but bm_value_changed_cb() never applied it (the dot
        # count never advanced). enter_pin_verified() must stop right there
        # -- never press "OK", never re-tap -- and the case must land on
        # INCONCLUSIVE with `entry_incomplete` recorded, never FAIL.
        class SilentLossKeypadUiTest(PinKeypadUiTest):
            """Digit at `drop_index` (0-based, among digit clicks only)
            reports "ok" without actually appending to self._entry --
            models the real silent-loss bug without touching the harness
            itself."""

            def __init__(self, *a, drop_index=2, **kw):
                super().__init__(*a, **kw)
                self._digit_click_n = -1
                self._drop_index = drop_index

            def click_by_name(self, name):
                if self._state == "keypad" and name in "0123456789":
                    self._digit_click_n += 1
                    if self._digit_click_n == self._drop_index:
                        return {"result": "ok"}  # claimed ok, never applied
                return super().click_by_name(name)

        ui = SilentLossKeypadUiTest(right_pin="1234", wrong_pin="0125", drop_index=2)
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0125"}}
        result = C._case_lcd19(ctx)
        wrong_entry = result.observed.get("wrong_pin_entry") or {}
        self.assertTrue(wrong_entry.get("entry_incomplete"))
        self.assertIsNone(wrong_entry.get("ok_result"))
        self.assertEqual(wrong_entry.get("expected_dot_count"), 3)
        self.assertEqual(wrong_entry.get("observed_dot_count"), 2)
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_keypad_closed_before_entry_is_recorded(self):
        # LCD-19 run-1 shape (20260930T090130Z_lcd): the keypad was seen
        # right after the Start click, but had already closed on its own
        # (display timeout) by the time the debounced pre-entry read ran.
        # Must record `keypad_closed_before_entry` and stay INCONCLUSIVE --
        # entry must never even be attempted.
        class ClosesBeforeEntryKeypadUiTest(PinKeypadUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self._keypad_reads = 0

            def list_tap_targets(self):
                if self._state == "keypad":
                    self._keypad_reads += 1
                    if self._keypad_reads <= 2:
                        # The reads right after the Start click
                        # (`_wait_for_keypad_raise`'s own two-consecutive-
                        # reads debounce, satisfied here since both show the
                        # full keypad) -- `keypad_raised` is confirmed True.
                        return super().list_tap_targets()
                    # Every read after that (the debounced pre-entry wait)
                    # shows the keypad already gone -- back to home.
                    return {"targets": [{"name": "Start", "hidden": False}], "truncated": False}
                return super().list_tap_targets()

        ui = ClosesBeforeEntryKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        with mock.patch.object(ui, "enter_pin_verified", wraps=ui.enter_pin_verified) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
                   "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
            result = C._case_lcd19(ctx)
        spy.assert_not_called()
        self.assertEqual(result.observed.get("keypad_closed_before_entry"), True)
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_verified_complete_entry_keypad_stays_open_after_ok_is_fail(self):
        # Requirement 2: `right_pin_started` may only be judged False (and
        # so FAIL) once entry was verified complete (every digit's dot count
        # confirmed) AND "OK" was applied. Model a right PIN whose digits are
        # all genuinely applied (dot count tracks correctly) but the keypad
        # never actually closes after "OK" -- a real firmware regression
        # shape, distinct from every "entry never verified" INCONCLUSIVE
        # case above -- and confirm this one still reaches a hard FAIL.
        class StaysOpenAfterOkKeypadUiTest(PinKeypadUiTest):
            def click_by_name(self, name):
                if self._state == "keypad" and name == "OK":
                    # Same bookkeeping as the real state machine (reset
                    # entry on a wrong PIN) EXCEPT the transition to Confirm
                    # Start on a right PIN is suppressed -- the press lands
                    # (so entry_incomplete is never set) but the keypad never
                    # actually closes, same as a stuck
                    # lcd_keypad_state_submit() would look like.
                    if self._entry != self._right_pin:
                        self._entry = ""
                    return {"result": "ok"}
                return super().click_by_name(name)

        ui = StaysOpenAfterOkKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        right_entry = result.observed.get("right_pin_entry") or {}
        self.assertFalse(right_entry.get("entry_incomplete", False))
        self.assertEqual(result.observed.get("right_pin_started"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_dismiss_overlay_not_needed_when_nothing_open(self):
        ui = PopupUiTest(trigger_name="Start", overlay_names=[])
        # Simulate a click that reports ok but raises nothing (defensive):
        ui._trigger_name = None  # click never opens an overlay
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed["overlay_dismiss"], {"checked": True, "present": False})


class _StopGatedUiTest(PinKeypadUiTest):
    """Extends PinKeypadUiTest with a "Stop" widget for the allow_heat
    stop_gated sub-check (owner decision 2026-09-30). The allow_heat path
    starts its firing directly over the mocked UART profiles client, never
    through this UI's own Confirm-Start button, so the UI is left sitting
    in whatever state block A's PIN entry produced ("confirm", after a
    correct PIN) when Stop is tapped. `stop_mode` controls what tapping
    "Stop" reveals: "gated" (Cancel+OK, i.e. a PIN keypad), "ungated"
    (Cancel only, i.e. Confirm Stop shown directly), or "none" (neither --
    stop_gated stays None)."""

    def __init__(self, *a, stop_mode="gated", stop_result="ok", **kw):
        super().__init__(*a, **kw)
        self._stop_mode = stop_mode
        self._stop_result = stop_result
        self._stop_tapped = False
        # Flip True only once a test's mocked exec-status poll confirms the
        # allow_heat firing reached "running" -- distinct from self._state
        # == "confirm", which block A's own right-PIN Confirm-Start dialog
        # read also uses (2026-09-30 review fix, item 3): without this
        # separate flag, the settled-home "Stop"-only reading below would
        # also intercept block A's own Start/Cancel confirm-dialog read and
        # break right_pin_started detection.
        self._firing_settled = False

    def list_tap_targets(self):
        if self._stop_tapped:
            if self._stop_mode == "gated":
                names = ["Cancel", "OK"] + [str(d) for d in range(10)]
            elif self._stop_mode == "ungated":
                # The real Confirm Stop dialog is a "Stop"/"Cancel" pair
                # (advisory C, 2026-09-30 review) -- "Cancel" alone
                # undersold what an ungated tap actually shows.
                names = ["Stop", "Cancel"]
            else:
                names = ["SomethingElse"]
            return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}
        if self._firing_settled:
            # Pre-tap (settle-wait poll, 2026-09-30 review fix): models the
            # real home page settled with the fire button reading "Stop"
            # once a firing is confirmed running -- the allow_heat firing
            # here is started directly over the mocked UART profiles client,
            # never through this UI's own Confirm-Start button, so nothing
            # else in this fixture drives that transition.
            return {"targets": [{"name": "Stop", "hidden": False}], "truncated": False}
        return super().list_tap_targets()

    def click_by_name(self, name):
        if name == "Stop":
            self._stop_tapped = True
            return {"result": self._stop_result}
        return super().click_by_name(name)


class Lcd19AllowHeatStopGatedTest(unittest.TestCase):
    """LCD-19's allow_heat opt-in (owner decision 2026-09-30): makes
    stop_gated reachable via a real, short bench firing started over the
    same hidden BENCH_HP UART path every HP-* case uses
    (cases_heat._start_bench_profile/_cleanup_bench_profile/_read_energized
    -- mocked here, never reimplemented). ctx["allow_heat"] defaults to
    falsy, so an ordinary run's behavior (block A only, stop_gated always
    None) must be provably unchanged."""

    def _ctx(self, ui, allow_heat=True, lcd19_allow_heat=None, **extra):
        srv = FakeSrvFull(ui, profiles=extra.pop("profiles", None))
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
            # Advisory A (2026-09-30 review): the allow_heat start refuses
            # up front without ctx["host"] (_read_energized needs it) --
            # give these tests a fake host so that new guard doesn't mask
            # what each test is actually exercising.
            "host": "10.0.0.99",
            # Fast, deterministic clock for _wait_for_stop_home_settled's
            # min-wait/poll loop -- both flags must be true (2026-09-30
            # review fix) for the case to ever reach it, so every test that
            # wants to reach the Stop-tap branch needs this regardless of
            # whether it separately patches C.time for the cleanup-
            # verification poll (a different, non-ctx-injectable loop).
            "_now": (lambda c=itertools.count(0.0, 0.1): next(c)),
            "_sleep": lambda s: None,
        }
        if allow_heat:
            ctx["allow_heat"] = True
        # Default: whatever allow_heat is, unless caller overrides -- lets
        # existing "allow_heat=True" callers keep meaning "the firing
        # starts" without every call site naming both flags explicitly.
        if lcd19_allow_heat is None:
            lcd19_allow_heat = allow_heat
        if lcd19_allow_heat:
            ctx["lcd19_allow_heat"] = True
        ctx.update(extra)
        return ctx, srv

    def test_happy_path_passes_and_cleanup_runs_once(self):
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        running_status = FakeExecStatus(state_name="running")
        idle_status = FakeExecStatus(state_name="idle")
        profiles = FakeProfiles(status=idle_status)
        # get_exec_status is polled: idle before start, then running once
        # confirming the start, then idle again during the cleanup poll.
        calls = {"n": 0}
        real_get = profiles.get_exec_status

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)) as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        start_mock.assert_called_once()
        cleanup_mock.assert_called_once()
        self.assertEqual(result.observed.get("stop_gated"), True)
        self.assertTrue(result.observed["bench_cleanup"]["verified"])
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_allow_heat_start_dismisses_stale_confirm_start_via_cancel_only(self):
        # 2026-09-30 fix: block A's right_pin_started sub-check enters the
        # right PIN and stops at the Confirm Start dialog without ever
        # pressing it (PinKeypadUiTest's state machine lands in "confirm",
        # i.e. list_tap_targets() -> ["Start", "Cancel"]) -- it never
        # dismisses that dialog itself. Before the allow_heat block starts
        # its own (mocked) API firing, it must notice that stale dialog and
        # dismiss it via the on-glass "Cancel" -- never by pressing "Start"
        # (this dialog's own confirm button) and never a PIN digit. Track
        # every click_by_name() call so a Start click would be caught even
        # though nothing downstream currently depends on this dialog's
        # state (the allow_heat firing starts over a mocked UART path, and
        # _firing_settled overrides list_tap_targets() once exec status
        # reads "running" regardless of self._state).
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        clicks: list = []
        real_click = ui.click_by_name

        def tracked_click(name):
            clicks.append(name)
            return real_click(name)

        ui.click_by_name = tracked_click
        running_status = FakeExecStatus(state_name="running")
        idle_status = FakeExecStatus(state_name="idle")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)), \
             mock.patch.object(CH, "_cleanup_bench_profile"), \
             mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        # "Start" appears exactly once in `clicks` -- block A's own initial
        # tap that opens the PIN keypad from the idle home page (unrelated
        # to this fix). The dialog left open afterward is the Confirm Start
        # popup itself (state "confirm", ["Start", "Cancel"]); THIS fix's
        # assertion is that its own confirm button is never pressed a
        # second time to dismiss it -- only "Cancel" is used for that.
        self.assertEqual(clicks.count("Start"), 1, clicks)
        self.assertEqual(
            clicks.count("Cancel"), 1,
            "must dismiss the stale Confirm Start dialog via exactly one Cancel click, "
            f"never its own Start button: {clicks}")
        self.assertLess(
            clicks.index("Cancel"), clicks.index("Stop"),
            f"the stale-dialog Cancel must happen before Stop is ever tapped: {clicks}")
        dismiss = result.observed.get("allow_heat_pre_start_dismiss")
        self.assertIsNotNone(dismiss, "allow_heat start must record the pre-start dismiss attempt")
        self.assertTrue(dismiss.get("checked"))
        self.assertTrue(dismiss.get("present"), dismiss)
        self.assertFalse(dismiss.get("is_keypad"), dismiss)
        self.assertEqual(dismiss.get("dismiss_method"), "click_by_name_cancel")
        self.assertTrue(dismiss.get("dismissed"), dismiss)
        self.assertEqual(ui._state, "idle", "Cancel must actually close the stale Confirm Start dialog")
        self.assertEqual(result.observed.get("stop_gated"), True)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_stop_not_gated_fails_and_cleanup_still_runs(self):
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="ungated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        cleanup_mock.assert_called_once()
        self.assertEqual(result.observed.get("stop_gated"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Stop", result.reason)

    def test_start_refused_stays_inconclusive_and_never_starts_a_firing(self):
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        profiles = FakeProfiles(status=idle_status)
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(False, "refused: zone ceiling", None)) as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock:
            result = C._case_lcd19(ctx)
        start_mock.assert_called_once()
        cleanup_mock.assert_not_called()
        self.assertIsNone(result.observed.get("stop_gated"))
        self.assertFalse(ui._stop_tapped, "Stop must never be tapped when no firing was actually started")
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_cleanup_verification_failure_fails_loud_naming_estop(self):
        # _cleanup_bench_profile itself never raises (best-effort by
        # design) but the bounded post-cleanup verification poll can still
        # never observe a de-energized relay -- that must be a hard FAIL
        # naming the hardware E-stop, never merely reported alongside a
        # PASS/other verdict.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status  # cleanup poll: exec goes idle, but relay never clears (see _read_energized below)

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        # Drive the 15s bounded cleanup-verification poll without a real
        # 15-second wait: fake monotonic() advances 2.0s on every call
        # (regardless of caller) and sleep() is a no-op, so the deadline
        # trips after a handful of fast iterations instead of real time.
        counter = itertools.count(0.0, 2.0)
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=True),              mock.patch.object(C.time, "monotonic", side_effect=counter.__next__),              mock.patch.object(C.time, "sleep", return_value=None):
            result = C._case_lcd19(ctx)
        cleanup_mock.assert_called_once()
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("E-stop", result.reason)
        self.assertFalse(result.observed["bench_cleanup"]["verified"])

    def test_estop_reason_survives_a_failed_policy_restore(self):
        # 2026-09-30 review fix: when the bench-firing cleanup verification
        # above already set the E-stop-naming FAIL reason AND the policy
        # restore below also fails, the E-stop reason must survive (it is
        # the more safety-relevant of the two) with the restore failure
        # appended, never clobbered outright by the restore's own reason.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status  # cleanup poll: exec idle, but relay never clears

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        ctx["sec_client"] = FakeLcd19SecClient(restore_ok=False)
        srv._profiles = profiles
        counter = itertools.count(0.0, 2.0)
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=True),              mock.patch.object(C.time, "monotonic", side_effect=counter.__next__),              mock.patch.object(C.time, "sleep", return_value=None):
            result = C._case_lcd19(ctx)
        cleanup_mock.assert_called_once()
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertTrue(
            result.reason.startswith("allow_heat bench firing could not be confirmed stopped"),
            result.reason,
        )
        self.assertIn("E-stop", result.reason)
        self.assertIn("policy restore did not round-trip", result.reason)

    def test_estop_reason_survives_a_restore_readback_mismatch(self):
        # Same as above, but restore's set_policy POST itself reports
        # 200/ok (restore_post_ok True) while the post-restore readback
        # still disagrees (restore_matches False) -- the other half of the
        # "not restore_post_ok or not restore_matches" OR that triggers the
        # append-not-clobber path.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        ctx["sec_client"] = _MismatchAfterRestoreSecClient()
        srv._profiles = profiles
        counter = itertools.count(0.0, 2.0)
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=True),              mock.patch.object(C.time, "monotonic", side_effect=counter.__next__),              mock.patch.object(C.time, "sleep", return_value=None):
            result = C._case_lcd19(ctx)
        cleanup_mock.assert_called_once()
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertTrue(
            result.reason.startswith("allow_heat bench firing could not be confirmed stopped"),
            result.reason,
        )
        self.assertIn("E-stop", result.reason)
        self.assertIn("policy restore did not round-trip", result.reason)

    def test_exception_mid_stop_branch_still_runs_cleanup(self):
        # A raise inside the Stop-tap branch (e.g. a lost UART reply on the
        # overlay read) must not skip the bench-firing teardown in
        # `finally` -- cleanup is safety-relevant and must run regardless.
        class RaisingStopUiTest(_StopGatedUiTest):
            def click_by_name(self, name):
                if name == "Stop":
                    raise RuntimeError("simulated UI_TEST reply lost")
                return super().click_by_name(name)

        ui = RaisingStopUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)) as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=False):
            with self.assertRaises(RuntimeError):
                C._case_lcd19(ctx)
        start_mock.assert_called_once()
        cleanup_mock.assert_called_once()

    def test_allow_heat_false_never_starts_a_firing_and_is_unchanged(self):
        # Default (no ctx["allow_heat"]) behavior must be provably
        # identical to before this feature: block A only, stop_gated stays
        # None, no bench profile touched at all.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        ctx, srv = self._ctx(ui, allow_heat=False)
        with mock.patch.object(CH, "_start_bench_profile") as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock:
            result = C._case_lcd19(ctx)
        start_mock.assert_not_called()
        cleanup_mock.assert_not_called()
        self.assertFalse(ui._stop_tapped)
        self.assertIsNone(result.observed.get("stop_gated"))
        self.assertNotIn("bench_cleanup", result.observed)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_lcd19_allow_heat_false_never_starts_even_with_suite_allow_heat_true(self):
        # 2026-09-30 review fix, item 1: ctx["allow_heat"]=True alone (the
        # suite-wide default) must NOT start LCD-19's firing -- only the
        # separate ctx["lcd19_allow_heat"] opt-in does, and that flag
        # defaults False at the runner/MCP/CLI layer.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        ctx, srv = self._ctx(ui, allow_heat=True, lcd19_allow_heat=False)
        with mock.patch.object(CH, "_start_bench_profile") as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock:
            result = C._case_lcd19(ctx)
        start_mock.assert_not_called()
        cleanup_mock.assert_not_called()
        self.assertFalse(ui._stop_tapped)
        self.assertIsNone(result.observed.get("stop_gated"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_relock_happens_before_stop_click_ordering_checked(self):
        # 2026-09-30 review fix, item 5 mutation (a): proves the relock
        # (set_policy off-then-on) is actually attempted, and that it
        # completes before Stop is ever tapped -- catches `if
        # started_firing_here:` being replaced with `if False:` in the
        # relock block, which would skip the relock (and, under the
        # not-relock_ok branch added by this same fix, would then also skip
        # the Stop tap entirely).
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        sec_client = ctx["sec_client"]
        events = []
        real_set_policy = sec_client.set_policy

        def recording_set_policy(*a, **kw):
            events.append(("set_policy", a, kw))
            return real_set_policy(*a, **kw)

        sec_client.set_policy = recording_set_policy
        real_click = ui.click_by_name

        def recording_click(name):
            if name == "Stop":
                events.append(("click_stop",))
            return real_click(name)

        ui.click_by_name = recording_click
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile"),              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        # At least one relock set_policy pair (off, then on) happened, and
        # the Stop click came strictly after both.
        set_policy_events = [e for e in events if e[0] == "set_policy"]
        click_events = [i for i, e in enumerate(events) if e[0] == "click_stop"]
        self.assertGreaterEqual(len(set_policy_events), 2, "expected an off-then-on relock pair")
        self.assertTrue(click_events, "Stop was never clicked")
        # The relock pair (off, then on) is the first two set_policy calls --
        # a further set_policy after the Stop click is the unrelated
        # end-of-case policy *restore* (the finally block), not the relock,
        # so only the relock pair's own index is checked against the tap.
        relock_indices = [i for i, e in enumerate(events) if e[0] == "set_policy"][:2]
        self.assertLess(max(relock_indices), click_events[0],
                         "relock must complete before the Stop click")
        self.assertTrue(result.observed.get("allow_heat_relock_ok"))

    def test_cleanup_runs_even_when_start_confirmation_never_observes_running(self):
        # 2026-09-30 review fix, item 5 mutation (b): proves the teardown
        # gate stays on `bench_profile_started` (set as soon as
        # _start_bench_profile itself reports ok) rather than the narrower
        # `started_firing_here` (only set once the running-confirmation poll
        # succeeds) -- catches `if bench_profile_started:` at the teardown
        # being narrowed to `if started_firing_here:`, which would skip
        # cleanup/verification here even though a real firing may still be
        # running on the board.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        profiles = FakeProfiles(status=idle_status)
        # get_exec_status never reports "running" -- the start-confirmation
        # poll times out, so started_firing_here stays False even though
        # _start_bench_profile itself reported ok (bench_profile_started
        # stays True).
        profiles.get_exec_status = lambda: idle_status
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        counter = itertools.count(0.0, 20.0)  # trips the 10s confirmation poll deadline on the 2nd read
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)) as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=False),              mock.patch.object(C.time, "monotonic", side_effect=counter.__next__),              mock.patch.object(C.time, "sleep", return_value=None):
            result = C._case_lcd19(ctx)
        start_mock.assert_called_once()
        cleanup_mock.assert_called_once()
        self.assertTrue(result.observed["bench_cleanup"]["verified"])
        # The Stop-tap branch never ran (firing_active never went True since
        # started_firing_here never got set), so stop_gated stays None and
        # the case is INCONCLUSIVE -- but cleanup still ran and verified.
        self.assertIsNone(result.observed.get("stop_gated"))
        self.assertFalse(ui._stop_tapped)

    def test_relock_failure_leaves_stop_gated_none_and_never_taps_stop(self):
        # 2026-09-30 review fix, item 2: a relock that does not confirm ok
        # must not be treated as "Stop still produces real evidence either
        # way" -- Stop must never be tapped, and stop_gated must stay None
        # (INCONCLUSIVE), not False.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        sec_client = ctx["sec_client"]
        real_set_policy = sec_client.set_policy
        relock_calls = {"n": 0}

        def failing_relock(*a, **kw):
            relock_calls["n"] += 1
            if relock_calls["n"] == 1:
                # The initial enable-lcd_enabled-before-driving-the-keypad
                # call (block entry) must still succeed.
                return real_set_policy(*a, **kw)
            # Every relock attempt after that (the off/on pair) fails.
            return 500, None

        sec_client.set_policy = failing_relock
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile"),              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        self.assertFalse(ui._stop_tapped, "Stop must never be tapped after a relock that did not confirm ok")
        self.assertIsNone(result.observed.get("stop_gated"))
        self.assertFalse(result.observed.get("allow_heat_relock_ok"))
        self.assertIn("relock", result.observed.get("allow_heat_stop_skip_reason", ""))

    def test_set_policy_raises_during_relock_cleanup_still_runs(self):
        # 2026-09-30 review fix, item 5: an exception raised by set_policy
        # itself during the relock attempt must not skip the bench-firing
        # teardown -- cleanup is safety-relevant and must run regardless of
        # what raised inside the try block.
        ui = _StopGatedUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        sec_client = ctx["sec_client"]
        real_set_policy = sec_client.set_policy
        relock_calls = {"n": 0}

        def raising_relock(*a, **kw):
            relock_calls["n"] += 1
            if relock_calls["n"] == 1:
                return real_set_policy(*a, **kw)
            raise RuntimeError("simulated HTTP failure during relock")

        sec_client.set_policy = raising_relock
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)) as start_mock,              mock.patch.object(CH, "_cleanup_bench_profile") as cleanup_mock,              mock.patch.object(CH, "_read_energized", return_value=False):
            with self.assertRaises(RuntimeError):
                C._case_lcd19(ctx)
        start_mock.assert_called_once()
        cleanup_mock.assert_called_once()

    def test_stop_tap_waits_for_settled_home_before_tapping(self):
        # 2026-09-30 review fix, item 3: the Stop tap must wait for a
        # STABLE settled read ("Stop" present, "Cancel" absent) -- a UI
        # still mid-transition (e.g. still showing the Confirm Start dialog
        # from block A, or flapping) must not be tapped yet. This fixture
        # reports unsettled for the first two polls, then settles.
        class SlowSettleUiTest(_StopGatedUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self._settle_polls = 0

            def list_tap_targets(self):
                if self._stop_tapped or not self._firing_settled:
                    # Before the firing is confirmed running, defer to block
                    # A's own PIN/confirm-dialog state machine untouched.
                    return super().list_tap_targets()
                self._settle_polls += 1
                if self._settle_polls <= 2:
                    # Still showing the Confirm Start dialog -- not settled.
                    return {"targets": [{"name": "Start", "hidden": False},
                                         {"name": "Cancel", "hidden": False}],
                            "truncated": False}
                return {"targets": [{"name": "Stop", "hidden": False}], "truncated": False}

        ui = SlowSettleUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile"),              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        self.assertTrue(ui._stop_tapped, "Stop should still be tapped once the read settles")
        self.assertTrue(result.observed.get("allow_heat_settle_ok"))
        self.assertEqual(result.observed.get("stop_gated"), True)

    def test_stop_tap_skipped_when_never_settles(self):
        # 2026-09-30 review fix, item 3: if the settle-wait never stabilizes
        # within its bounded window, the result must be INCONCLUSIVE and
        # Stop must never be tapped -- not a FAIL, and not a guess.
        class NeverSettleUiTest(_StopGatedUiTest):
            def list_tap_targets(self):
                if self._stop_tapped or not self._firing_settled:
                    # Before the firing is confirmed running, defer to block
                    # A's own PIN/confirm-dialog state machine untouched.
                    return super().list_tap_targets()
                # Always mid-transition: alternates so it's never stable.
                import random
                name = "Start" if random.random() < 0.5 else "Stop"
                return {"targets": [{"name": name, "hidden": False},
                                     {"name": "Cancel", "hidden": False}],
                        "truncated": False}

        ui = NeverSettleUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile"),              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        self.assertFalse(ui._stop_tapped, "Stop must never be tapped when the home page never settles")
        self.assertFalse(result.observed.get("allow_heat_settle_ok"))
        self.assertIsNone(result.observed.get("stop_gated"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_stop_tap_settles_despite_live_temperature_readout_changing(self):
        # Bench bug, run 20260930T200715Z_lcd (LCD-19, allow_heat=True):
        # allow_heat_relock_ok was True but allow_heat_settle_ok stayed
        # False and Stop was never tapped, leaving stop_gated INCONCLUSIVE.
        # Root cause: the pre-fix `_wait_for_home_settled` required the
        # FULL tap-target name set to read byte-identical across two
        # consecutive polls. While a firing is genuinely active the home
        # page's own live temperature readout changes on its own cadence
        # (independent of the lock-race the helper watches for), so on a
        # real board "Stop" was present and "Cancel" was absent on every
        # single poll, but the reading (e.g. "17C" -> "18C") ticking over
        # kept resetting the stability counter and the wait always timed
        # out. This fixture reproduces that shape: every settle-phase poll
        # already qualifies (Stop present, Cancel absent), but the
        # temperature name changes every single read.
        class ChangingTempUiTest(_StopGatedUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self._settle_polls = 0

            def list_tap_targets(self):
                if self._stop_tapped or not self._firing_settled:
                    return super().list_tap_targets()
                self._settle_polls += 1
                temp_name = f"{16 + self._settle_polls}C"  # a different name every poll
                return {"targets": [{"name": "Stop", "hidden": False},
                                     {"name": temp_name, "hidden": False}],
                        "truncated": False}

        ui = ChangingTempUiTest(right_pin="1234", wrong_pin="0000", stop_mode="gated")
        idle_status = FakeExecStatus(state_name="idle")
        running_status = FakeExecStatus(state_name="running")
        profiles = FakeProfiles(status=idle_status)
        calls = {"n": 0}

        def sequenced():
            calls["n"] += 1
            if calls["n"] == 1:
                return idle_status
            if calls["n"] == 2:
                ui._firing_settled = True
                return running_status
            return idle_status

        profiles.get_exec_status = sequenced
        ctx, srv = self._ctx(ui, allow_heat=True)
        srv._profiles = profiles
        with mock.patch.object(CH, "_start_bench_profile", return_value=(True, "started", 20.0)),              mock.patch.object(CH, "_cleanup_bench_profile"),              mock.patch.object(CH, "_read_energized", return_value=False):
            result = C._case_lcd19(ctx)
        self.assertTrue(ui._stop_tapped,
                         "Stop must still be tapped when the qualifying condition holds even if "
                         "unrelated live content (e.g. temperature) changes every poll")
        self.assertTrue(result.observed.get("allow_heat_settle_ok"))
        self.assertEqual(result.observed.get("stop_gated"), True)
        reads = result.observed.get("allow_heat_settle_reads")
        self.assertIsNotNone(reads)
        self.assertTrue(len(reads) >= 2)
        self.assertTrue(all(r["qualifies"] for r in reads),
                         "every settle-phase poll already qualified (Stop present, Cancel absent)")
        # The reproduction of the bug: consecutive reads' full name sets
        # actually differ (the changing temperature reading), yet settling
        # still succeeds under the fixed, subset-based stability check.
        self.assertNotEqual(reads[0]["names"], reads[1]["names"])

    def test_settle_requires_consecutive_qualifying_reads(self):
        # 2026-09-30 opus review, item 1: the "consecutive" part of
        # `stable_reads` was untested -- changing the non-qualifying else
        # branch's `stable_count = 0` to a no-op `pass` left every prior
        # test green, because none of them exercised a qualifying read,
        # then a non-qualifying read, then qualifying reads again. This
        # test drives `_wait_for_home_settled` directly (rather than
        # through the full LCD-19 case) with exactly that sequence:
        #   read 1: qualifies (Stop present, Cancel absent)      -> count=1
        #   read 2: does NOT qualify (Cancel present)             -> count=0 (reset)
        #   read 3: qualifies again                               -> count=1
        #   read 4: qualifies again                               -> count=2, settled
        # A correct implementation must poll all 4 times before returning
        # True; the reset-skipping mutation returns True after only 3
        # (reads 1 then 3 then 4 would already read as 2-in-a-row without
        # the reset, since a bare `pass` leaves stable_count at 1 across
        # the bad read instead of dropping it to 0).
        responses = [
            {"targets": [{"name": "Stop", "hidden": False}], "truncated": False},
            {"targets": [{"name": "Stop", "hidden": False}, {"name": "Cancel", "hidden": False}],
             "truncated": False},
            {"targets": [{"name": "Stop", "hidden": False}], "truncated": False},
            {"targets": [{"name": "Stop", "hidden": False}], "truncated": False},
        ]
        calls = {"n": 0}

        class FakeUi:
            def list_tap_targets(self):
                idx = calls["n"]
                calls["n"] += 1
                # Any read beyond the scripted 4 means the helper settled
                # (or kept polling) later than the sequence above intends --
                # returning the last scripted response keeps the loop from
                # crashing while `calls["n"]` still records the true count.
                return responses[min(idx, len(responses) - 1)]

        ui = FakeUi()
        ctx = {"_sleep": lambda s: None, "_now": itertools.count(0.0, 0.1).__next__}
        result = C._wait_for_home_settled(
            ctx, ui, "Stop", min_wait_s=0.0, timeout_s=100.0, poll_interval_s=0.0, stable_reads=2)
        self.assertTrue(result)
        self.assertEqual(
            calls["n"], 4,
            "settling must require stable_reads consecutive qualifying reads -- a "
            "non-qualifying read in between must reset the count, not merely be ignored")

    def test_settle_accepts_truncated_read_when_target_present_cancel_absent(self):
        # 2026-09-30 opus review, item 2 / reviewer advisory A: kiln_ui.c's
        # log_all_tap_targets() walks the overlay layers (lv_layer_top(),
        # kiln_ui.c:629-633) before the home screen's own normal children,
        # and uart_bridge_ui_test.c's UI_TEST_CMD_LIST_TAP_TARGETS handler
        # truncates by simply stopping mid-walk once the 253 B wire reply
        # is full -- always dropping entries off the END of that order. So
        # a truncated read that still contains the home page's own
        # `target_name` (walked last of all) proves the walk got all the
        # way past the overlay section, meaning any modal's own "Cancel"
        # would already have been emitted had one been open. Such a read
        # must still qualify.
        responses = {"targets": [{"name": "Stop", "hidden": False}], "truncated": True}

        class FakeUi:
            def list_tap_targets(self):
                return dict(responses)

        ui = FakeUi()
        log: list = []
        ctx = {"_sleep": lambda s: None, "_now": itertools.count(0.0, 0.1).__next__}
        result = C._wait_for_home_settled(
            ctx, ui, "Stop", min_wait_s=0.0, timeout_s=100.0, poll_interval_s=0.0,
            stable_reads=2, log=log)
        self.assertTrue(result, "a truncated read with target present and Cancel absent must qualify")
        self.assertTrue(len(log) >= 2)
        self.assertTrue(all(r["truncated"] for r in log))
        self.assertTrue(all(r["qualifies"] for r in log))

    def test_settle_rejects_truncated_read_missing_target(self):
        # Companion to the above: a truncated read that does NOT contain
        # target_name is indeterminate (the cut could have landed before or
        # after where an overlay's own targets would appear) and must never
        # qualify, so the wait times out and Stop/Start is never tapped.
        responses = {"targets": [{"name": "17C", "hidden": False}], "truncated": True}

        class FakeUi:
            def list_tap_targets(self):
                return dict(responses)

        ui = FakeUi()
        log: list = []
        ctx = {"_sleep": lambda s: None, "_now": itertools.count(0.0, 0.1).__next__}
        result = C._wait_for_home_settled(
            ctx, ui, "Stop", min_wait_s=0.0, timeout_s=0.5, poll_interval_s=0.05,
            stable_reads=2, log=log)
        self.assertFalse(result, "a truncated read missing target_name must never qualify")
        self.assertTrue(log)
        self.assertFalse(any(r["qualifies"] for r in log))


class _SharedBoardSecClient:
    """One fake board's policy/PIN state, shared across both WEB-SEC-04 and
    LCD-19 in :class:`WebSec04ThenLcd19Test` -- proves the fix-1 sequencing
    actually works end to end: WEB-SEC-04 restores lcd_enabled to its
    original (off) value in its own finally, and LCD-19 must then be able to
    turn it back on for its own run and restore it off again afterward,
    against the SAME underlying state rather than two independent fakes."""

    def __init__(self):
        self._cfg = {
            "web_enabled": False, "lcd_enabled": False,
            "web_timeout_min": 30, "lcd_timeout_min": 30,
            "admin_pin_set": False,
        }
        self.set_lcd_pin_calls = []
        self.set_policy_calls = []

    def get_config(self):
        return 200, dict(self._cfg)

    def set_lcd_pin(self, role, pin):
        self.set_lcd_pin_calls.append((role, pin))
        self._cfg["admin_pin_set"] = True
        return 200, {"ok": True}

    def set_policy(self, web_enabled, lcd_enabled, web_timeout_min, lcd_timeout_min):
        self.set_policy_calls.append(lcd_enabled)
        self._cfg["web_enabled"] = web_enabled
        self._cfg["lcd_enabled"] = lcd_enabled
        self._cfg["web_timeout_min"] = web_timeout_min
        self._cfg["lcd_timeout_min"] = lcd_timeout_min
        return 200, {"ok": True}


class WebSec04ThenLcd19Test(unittest.TestCase):
    """Fix 1's required test: run WEB-SEC-04 then LCD-19 against ONE shared
    fake board state, proving lcd_enabled ends up off after WEB-SEC-04 (its
    own restore), gets turned on by LCD-19 for its own run, and ends up off
    again after LCD-19 (its own restore) -- never relying on one case's
    leftover policy state for the other's keypad to appear."""

    def test_lcd19_turns_policy_on_itself_after_web_sec04_restores_it_off(self):
        os.environ[CW._LCD_PIN_ENV] = "1234"
        try:
            sec = _SharedBoardSecClient()
            ctx = {"sec_client": sec}

            sec04_result = CW._case_web_sec04(ctx)
            self.assertEqual(sec04_result.verdict, Verdict.PASS)
            # WEB-SEC-04 must leave lcd_enabled exactly as it found it (off).
            self.assertEqual(sec._cfg["lcd_enabled"], False)
            self.assertIn("_lcd_pin", ctx)

            # The fake only raises the keypad while the SHARED board state has
            # lcd_enabled on, so this passes only if LCD-19 enables it itself.
            ui = PinKeypadUiTest(right_pin="1234", wrong_pin=ctx["_lcd_pin"]["wrong_pin"],
                                 policy_fn=lambda: sec._cfg["lcd_enabled"])
            ctx["srv"] = FakeSrvFull(ui)
            lcd19_result = C._case_lcd19(ctx)

            # LCD-19 must have turned lcd_enabled on for its own run (else
            # the keypad could never have appeared) and restored it off
            # again afterward -- against the SAME shared board state.
            self.assertIn(True, sec.set_policy_calls)
            self.assertEqual(sec._cfg["lcd_enabled"], False)
            self.assertTrue(lcd19_result.observed["restore"]["readback_matches"])
            self.assertEqual(lcd19_result.observed.get("wrong_pin_refused"), True)
            self.assertEqual(lcd19_result.observed.get("right_pin_started"), True)
        finally:
            os.environ.pop(CW._LCD_PIN_ENV, None)


class DelayedPageNavUiTest(PageNavUiTest):
    """Models the click_by_name()-then-read race (2026-09-24 bench finding):
    click_by_name() reports 'ok' immediately, but the destination page name
    only becomes visible after `flips_after` subsequent get_current_page()
    polls -- the real LVGL-task-poll delay collapsed to "N calls" so the
    test runs instantly. `flips_after=None` means the page never flips at
    all (the never-arrives case)."""

    def __init__(self, *a, flips_after=1, **kw):
        super().__init__(*a, **kw)
        self._flips_after = flips_after
        self._pending_dest = None
        self._poll_count = 0

    def click_by_name(self, name):
        if self._click_result != "ok":
            return {"result": self._click_result, "cx": 0, "cy": 0}
        dest = self._nav_map.get(name)
        if dest is not None:
            self._pending_dest = dest
            self._poll_count = 0
        return {"result": "ok", "cx": 0, "cy": 0}

    def get_current_page(self):
        if self._pending_dest is not None and self._flips_after is not None:
            self._poll_count += 1
            if self._poll_count > self._flips_after:
                self._page = self._pending_dest
                self._pending_dest = None
        return self._page

    def list_tap_targets(self):
        return {"targets": self._page_targets.get(self._page, []), "truncated": False}


class Lcd08PollTest(unittest.TestCase):
    """LCD-08 specifically reproduces the bench log: click_by_name('settings')
    succeeds but an immediate page read still says 'home'."""

    def test_page_flips_one_poll_after_click_still_passes(self):
        ui = DelayedPageNavUiTest(
            page="home",
            page_targets={"home": [], "config": _CONFIG_TARGETS},
            nav_map={"settings": "config"},
            flips_after=1,
        )
        srv = FakeSrv(ui)
        # page flip succeeds so _case_lcd08 falls through to its unconditional
        # _capture() call; mock the camera so this test never shells out.
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertIn("page_wait_s", result.observed)

    def test_page_never_flips_is_still_a_fail(self):
        ui = DelayedPageNavUiTest(
            page="home",
            page_targets={"home": [], "config": _CONFIG_TARGETS},
            nav_map={"settings": "config"},
            flips_after=None,
        )
        srv = FakeSrv(ui)
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("home", result.reason)


class Lcd09PollTest(unittest.TestCase):
    def test_page_flips_one_poll_after_each_click_still_passes(self):
        ui = DelayedPageNavUiTest(
            page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, flips_after=1,
        )
        srv = FakeSrvFull(ui)
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)


class Lcd14PollTest(unittest.TestCase):
    def test_page_flips_one_poll_after_each_click_still_passes(self):
        ui = DelayedPageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV, flips_after=1)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0, 1: 100.0, 2: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))

        def fake_sample_widget(image_path, cx, cy, repo_root=None):
            is_text_point = abs(cx - C._LCD14_ZONE_TEXT_X) < abs(cx - C._LCD14_ROW_X)
            region = C._TEXT_PRIMARY_RGB if is_text_point else C._CARD_RGB
            return lcd_sampler.RegionSample(region=region, bezel=(6, 13, 22))

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)


class Lcd16PollTest(unittest.TestCase):
    def test_relay_life_reset_seen_after_a_swallowed_next_tap_still_fails(self):
        # Same "poll one step late" race as Lcd14PollTest, expressed through
        # DiagPagingUi's swallow_step: a Next tap on step 3 (Relay Life) is
        # swallowed once, so the retry path must still be the one that
        # observes the reappeared Reset control rather than silently
        # skipping past it.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                           step_content={3: [{"name": "Reset", "cx": 240, "cy": 280, "hidden": False}]},
                           swallow_step=2)
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.observed.get("relay_life_has_reset"), True)
        self.assertEqual(result.verdict, Verdict.FAIL)


class FakeTouchThatWakes:
    """A TouchClient double: inject() records the call and wakes the
    SwallowingUiTest it is paired with, modeling `screen_idle.c` treating a
    real touch_inject press/release as a genuine touch for the idle timer
    and wake decision."""

    def __init__(self, ui, idle_ms=0, state_error=None):
        self._ui = ui
        self.injected = []
        self.idle_ms = idle_ms
        self.state_error = state_error

    def get_state(self):
        if self.state_error is not None:
            raise self.state_error
        return mock.Mock(screen_on=not self._ui._asleep, idle_ms=self.idle_ms)

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))
        self._ui.wake()


class SwallowingUiTest(PageNavUiTest):
    """Models `screen_idle.c`/`lvgl_port.c`'s touch-swallow: while asleep,
    `click_by_name()` still replies 'ok' (matching the real firmware's
    behavior -- it only reports whether a target by that name exists, not
    whether the tap reached it) but never actually navigates. The first
    real touch_inject (via `wake()`) clears the asleep flag, same as a real
    panel waking on a tap."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self._asleep = True

    def wake(self):
        self._asleep = False

    def click_by_name(self, name):
        if self._asleep:
            return {"result": "ok", "cx": 0, "cy": 0}
        return super().click_by_name(name)


class FakeSrvWithTouch(FakeSrv):
    def __init__(self, ui_test):
        super().__init__(ui_test)
        self._touch = FakeTouchThatWakes(ui_test)


class WakeAndHomeTest(unittest.TestCase):
    """Covers `_wake_and_home()` itself, plus the negative test required
    alongside it: a UI double that swallows every click until woken must
    make the OLD path (no wake step) fail, and the NEW path (with the wake
    step) pass -- proving the fix actually closes the 2026-09-24 bench
    race rather than merely adding an unused helper."""

    def test_wake_and_home_injects_touch_before_navigating(self):
        ui = SwallowingUiTest(page="home", page_targets=_CONFIG_TARGETS and {
            "home": [], "config": _CONFIG_TARGETS,
        }, nav_map={"settings": "config"})
        srv = FakeSrvWithTouch(ui)
        ctx = {"srv": srv}
        C._wake_and_home(ctx)
        self.assertEqual(srv._touch.injected, [(5, 5, True), (5, 5, False)])
        self.assertFalse(ui._asleep)
        self.assertEqual(ui.get_current_page(), "home")

    def test_wake_runs_per_case_after_a_re_blank(self):
        """The panel can blank again between cases (shortest timeout is
        1 min): a second call after a re-blank must wake it again, not be
        skipped as already-done-this-run."""
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrvWithTouch(ui)
        ctx = {"srv": srv}
        C._wake_and_home(ctx)
        self.assertEqual(len(srv._touch.injected), 2)
        ui._asleep = True  # re-blanked between cases
        C._wake_and_home(ctx)
        self.assertEqual(len(srv._touch.injected), 4)
        self.assertFalse(ui._asleep)

    def test_no_wake_tap_on_an_awake_recently_active_panel(self):
        """On an awake panel the (5,5) tap would be delivered for real
        (home's top-left auth-reset corner) -- skip it when not needed."""
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        ui._asleep = False
        srv = FakeSrvWithTouch(ui)
        srv._touch.idle_ms = 1000
        C._wake_and_home({"srv": srv})
        self.assertNotIn((180 + _DIAG_PITCH, 26, True), srv._touch.injected)

    def test_wake_tap_sent_when_awake_but_idle_long(self):
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        ui._asleep = False
        srv = FakeSrvWithTouch(ui)
        srv._touch.idle_ms = C._WAKE_IDLE_MS_THRESHOLD
        C._wake_and_home({"srv": srv})
        self.assertEqual(len(srv._touch.injected), 2)

    def test_wake_tap_sent_when_state_unreadable(self):
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrvWithTouch(ui)
        srv._touch.state_error = RuntimeError("GET_STATE timed out")
        C._wake_and_home({"srv": srv})
        self.assertEqual(len(srv._touch.injected), 2)
        self.assertFalse(ui._asleep)

    def test_blanked_screen_swallow_old_path_fails_new_path_passes(self):
        page_targets = {"home": [], "config": _CONFIG_TARGETS}
        nav_map = {"settings": "config"}

        # OLD path: no wake step at all -- click_by_name('settings') reports
        # 'ok' (swallowed-but-ok, per screen_idle.c's real behavior) while
        # the page never actually changes, so LCD-08 must FAIL.
        ui_old = SwallowingUiTest(page="home", page_targets=page_targets, nav_map=nav_map)
        srv_old = FakeSrvWithTouch(ui_old)
        # _case_lcd08 captures unconditionally on success (NEW path reaches
        # config, page-flip succeeded), so mock the camera for both calls.
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            with mock.patch.object(C, "_wake_and_home", lambda ctx: None):
                result_old = C._case_lcd08({"srv": srv_old})
            self.assertEqual(result_old.verdict, Verdict.FAIL)

            # NEW path: the real _wake_and_home runs first, sends the wake tap,
            # and the same case now passes.
            ui_new = SwallowingUiTest(page="home", page_targets=page_targets, nav_map=nav_map)
            srv_new = FakeSrvWithTouch(ui_new)
            result_new = C._case_lcd08({"srv": srv_new})
        self.assertEqual(result_new.verdict, Verdict.PASS)


class FakeTouchErrorHold:
    """A TouchClient double for _wake_and_home()'s ERROR_HOLD dismissal
    branch (cases_lcd.py lines ~169-197): get_state() reports power_state
    (not just screen_on, both read True in ERROR_HOLD per
    display_power_policy.c's rule 5) so `_power_state_is_on`/
    `_power_state_is_error_hold` can actually distinguish the two. Each
    dismiss tap (inject) decrements a counter of remaining ERROR_HOLD
    reports before the state advances to ON, modeling rule 5 dismissing
    ERROR_HOLD on its own next touch."""

    def __init__(self, error_hold_reports: int):
        self.error_hold_reports = error_hold_reports
        self.injected = []

    def get_state(self):
        if self.error_hold_reports > 0:
            return mock.Mock(screen_on=True, idle_ms=0,
                              power_state=TOUCH_POWER_STATE_ERROR_HOLD)
        return mock.Mock(screen_on=True, idle_ms=0, power_state=TOUCH_POWER_STATE_ON)

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))
        if pressed and self.error_hold_reports > 0:
            self.error_hold_reports -= 1


class WakeAndHomeErrorHoldTest(unittest.TestCase):
    """Task 4 (2026-09-24 follow-up): _wake_and_home()'s ERROR_HOLD
    dismissal loop had no direct coverage -- only the plain screen_on/off
    wake path did. These pin the dismiss-then-proceed behavior and the
    bound on retries."""

    def test_error_hold_is_dismissed_by_one_extra_tap(self):
        # error_hold_reports=2: the very first wake tap (sent because the
        # initial need_wake check itself reads ERROR_HOLD) decrements this
        # to 1 as a side effect of its own press -- FakeTouchErrorHold.inject()
        # decrements on every press, including the one that triggered the
        # wake in the first place. One further dismiss tap inside the wait
        # loop is needed to actually clear it and observe ON.
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrv(ui)
        srv._touch = FakeTouchErrorHold(error_hold_reports=2)
        C._wake_and_home({"srv": srv})
        # Initial wake press+release, plus exactly one dismiss press+release.
        self.assertEqual(srv._touch.injected, [
            (5, 5, True), (5, 5, False),
            (5, 5, True), (5, 5, False),
        ])

    def test_error_hold_dismissal_is_bounded_and_falls_through(self):
        # A board stuck in ERROR_HOLD past _ERROR_HOLD_DISMISS_MAX_ATTEMPTS
        # must not loop forever -- it falls through once the attempt bound
        # is hit and the wait's own timeout elapses, still on ERROR_HOLD.
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrv(ui)
        srv._touch = FakeTouchErrorHold(error_hold_reports=1_000_000)
        C._wake_and_home({"srv": srv})
        dismiss_taps = len(srv._touch.injected) // 2 - 1  # minus the initial wake tap
        self.assertEqual(dismiss_taps, C._ERROR_HOLD_DISMISS_MAX_ATTEMPTS)
        # Best-effort: never raises even though the panel is still stuck.
        self.assertEqual(ui.get_current_page(), "home")

    def test_older_firmware_without_power_state_never_enters_error_hold_branch(self):
        # A board on firmware predating GET_STATE's power_state field (only
        # screen_on) must fall back to the plain screen_on check and never
        # dereference power_state at all -- _power_state_is_on returns None
        # for that case, and _power_state_is_error_hold reads False.
        class _NoPowerStateTouch:
            def __init__(self):
                self.injected = []
                self._asleep = True

            def get_state(self):
                return mock.Mock(spec=["screen_on", "idle_ms"],
                                  screen_on=not self._asleep, idle_ms=0)

            def inject(self, x, y, pressed):
                self.injected.append((x, y, pressed))
                if pressed:
                    self._asleep = False

        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrv(ui)
        srv._touch = _NoPowerStateTouch()
        C._wake_and_home({"srv": srv})
        # Only the single initial wake press+release; the ERROR_HOLD branch
        # is never reached because get_state() has no power_state attr.
        self.assertEqual(srv._touch.injected, [(5, 5, True), (5, 5, False)])


class _EmptyThenRealUiTest(FakeUiTest):
    """``list_tap_targets()`` returns the walk-timeout sentinel (count 0,
    ``truncated`` per `truncated_first`) for the first `empty_reads` polls,
    then a real, non-empty listing -- models `uart_bridge_ui_test.c`'s
    300ms LVGL walk window occasionally losing the race."""

    def __init__(self, real_names, empty_reads=2, truncated_first=True):
        super().__init__(page="home", targets=[])
        self._real_names = real_names
        self._empty_reads = empty_reads
        self._truncated_first = truncated_first
        self._reads = 0

    def list_tap_targets(self):
        self._reads += 1
        if self._reads <= self._empty_reads:
            return {"targets": [], "truncated": self._truncated_first}
        return {
            "targets": [{"name": n, "hidden": False} for n in self._real_names],
            "truncated": False,
        }


class Lcd19OverlayFixesTest(unittest.TestCase):
    """Direct unit tests for the three 2026-09-25 harness fixes (Opus
    root-cause of logs/bench_test/20260925T150041Z_full and
    20260925T150107Z_lcd): an empty/truncated tap-target listing must never
    be read as a real answer; a dismiss must never be reported without
    evidence; and the PIN keypad must be dismissed via the backdrop
    touch-inject path, never the (off-glass) Cancel button."""

    # -- Fix 1: empty/truncated listing is "no read yet", never a real set --

    def test_empty_then_real_listing_is_not_mistaken_for_an_answer(self):
        ui = _EmptyThenRealUiTest(real_names=["Start"], empty_reads=3)
        names = C._lcd19_overlay_names(ui, timeout_s=1.0, interval_s=0.01)
        self.assertEqual(names, {"Start"})

    def test_wait_for_overlay_names_never_treats_empty_as_gone(self):
        # present=False (waiting for a popup to disappear) must not be
        # satisfied by the empty/timeout sentinel -- only by a real,
        # non-empty listing with no popup content.
        ui = _EmptyThenRealUiTest(real_names=["Start"], empty_reads=3)
        names, _elapsed, empty_polls, truncated_seen = C._wait_for_overlay_names(
            ui, present=False, timeout_s=1.0, interval_s=0.01)
        self.assertEqual(names, {"Start"})
        self.assertGreaterEqual(empty_polls, 3)
        self.assertTrue(truncated_seen)

    def test_wait_for_overlay_names_records_empty_polls_and_truncated_when_it_never_recovers(self):
        # Every read stays empty/truncated for the whole timeout: the
        # caller must still get an honest None, plus a count of the empty
        # polls and that truncated was seen, never a fabricated set.
        ui = _EmptyThenRealUiTest(real_names=["Start"], empty_reads=10_000)
        names, _elapsed, empty_polls, truncated_seen = C._wait_for_overlay_names(
            ui, present=True, timeout_s=0.1, interval_s=0.01)
        self.assertIsNone(names)
        self.assertGreater(empty_polls, 0)
        self.assertTrue(truncated_seen)

    def test_wait_stable_names_discards_empty_reads_from_the_streak(self):
        ui = _EmptyThenRealUiTest(real_names=["1", "2", "OK", "Cancel"], empty_reads=2)
        names, _elapsed, empty_polls, _trunc, stabilized = C._wait_stable_names(
            ui, timeout_s=1.0, interval_s=0.01, stable_reads=2)
        self.assertEqual(names, {"1", "2", "OK", "Cancel"})
        self.assertEqual(empty_polls, 2)
        self.assertTrue(stabilized)

    def test_wait_stable_names_reports_unstabilized_when_reads_never_repeat(self):
        # A real (non-empty) read every poll, but it keeps changing right up
        # to the timeout -- never repeating `stable_reads` times in a row.
        # `stabilized` must come back False so a caller doesn't mistake the
        # last-seen read for a settled answer.
        class _AlwaysDifferentUiTest(FakeUiTest):
            def __init__(self):
                super().__init__(page="home", targets=[])
                self._n = 0

            def list_tap_targets(self):
                self._n += 1
                return {"targets": [{"name": f"x{self._n}", "hidden": False}], "truncated": False}

        ui = _AlwaysDifferentUiTest()
        names, _elapsed, empty_polls, _trunc, stabilized = C._wait_stable_names(
            ui, timeout_s=0.1, interval_s=0.01, stable_reads=2)
        self.assertIsNotNone(names)  # last read is still recorded for diagnostics
        self.assertFalse(stabilized)

    # -- Fix 2: dismiss requires real evidence, never a bare "ok" click --

    def test_dismiss_never_reports_true_when_overlay_listing_never_clears(self):
        # click_by_name("Cancel") reports "ok", but the listing keeps
        # showing "Cancel" (or reads empty/timeout) afterward -- the
        # original bug reported dismissed=True purely off the click result.
        class _StuckDialogUiTest(FakeUiTest):
            def __init__(self):
                super().__init__(page="home", targets=[])

            def list_tap_targets(self):
                return {"targets": [{"name": "Start", "hidden": False},
                                     {"name": "Cancel", "hidden": False}],
                        "truncated": False}

            def click_by_name(self, name):
                return {"result": "ok"}

        ui = _StuckDialogUiTest()
        ctx = {"srv": FakeSrv(ui)}
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.05):
            result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertEqual(result["dismiss_method"], "click_by_name_cancel")
        self.assertEqual(result["cancel_click_result"], "ok")
        self.assertFalse(result["dismissed"])

    def test_dismiss_false_when_click_itself_is_not_ok(self):
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["Stop", "Cancel"])
        ui.click_by_name("Stop")  # open the confirm dialog
        real_click = ui.click_by_name
        ui.click_by_name = lambda name: {"result": "not_found"} if name == "Cancel" else real_click(name)
        ctx = {"srv": FakeSrv(ui)}
        result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertFalse(result["dismissed"])
        self.assertEqual(result["cancel_click_result"], "not_found")

    def test_dismiss_true_only_after_real_evidence_of_a_clear_listing(self):
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["Stop", "Cancel"])
        ui.click_by_name("Stop")
        ctx = {"srv": FakeSrv(ui)}
        result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertTrue(result["dismissed"])
        self.assertEqual(result["dismiss_method"], "click_by_name_cancel")

    # -- Fix 2b: backdrop touch-inject is chosen for the keypad, never a tap
    #    on OK/digits/Confirm, and click_by_name("Cancel") is never used --

    def test_keypad_overlay_dismissed_via_backdrop_not_cancel_click(self):
        ui = PopupUiTest(trigger_name="Start", overlay_names=["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "OK", "Cancel"])
        ui.click_by_name("Start")
        touch = BackdropTouch(ui)
        srv = FakeSrv(ui)
        srv._touch = touch
        ctx = {"srv": srv}
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertEqual(result["dismiss_method"], "backdrop_touch_inject")
        self.assertTrue(result["dismissed"])
        spy.assert_not_called()  # never OK, a digit, or Cancel
        self.assertIn((20, 160, True), touch.injected)
        self.assertIn((20, 160, False), touch.injected)

    def test_keypad_dismiss_without_a_touch_client_is_honestly_not_dismissed(self):
        ui = PopupUiTest(trigger_name="Start", overlay_names=["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "OK", "Cancel"])
        ui.click_by_name("Start")
        ctx = {"srv": FakeSrv(ui)}  # no _touch attribute at all
        result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertFalse(result["dismissed"])
        self.assertIn("error", result)

    def test_confirm_dialog_overlay_still_uses_cancel_click_not_backdrop(self):
        # Only "Cancel" is present (no "OK") -- a Confirm Start/Stop dialog,
        # whose own Cancel button is on-glass -- so this path is unaffected
        # by the keypad-specific backdrop workaround.
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["Stop", "Cancel"])
        ui.click_by_name("Stop")
        ctx = {"srv": FakeSrv(ui)}  # no _touch at all -- must not be needed
        result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertEqual(result["dismiss_method"], "click_by_name_cancel")
        self.assertTrue(result["dismissed"])

    # -- Fix 3: LCD-01 must FAIL against a stray overlay, never the Start
    #    button, and must proceed normally once it is dismissed --

    def test_lcd01_fails_naming_stray_overlay_when_it_cannot_dismiss(self):
        ui = PopupUiTest(trigger_name="Start", overlay_names=["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "OK", "Cancel"])
        ui.click_by_name("Start")  # simulate a leftover keypad from a prior run
        srv = FakeSrv(ui)
        # No _touch client -- the keypad backdrop dismiss cannot be attempted,
        # so this must FAIL naming the stray overlay, not blame Start.
        result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("stray overlay", result.reason)
        self.assertNotIn("Start button", result.reason)  # never blame Start
        stray = result.observed["stray_overlay"]
        self.assertIn("Cancel", stray["names"])
        self.assertFalse(stray["dismiss"]["dismissed"])

    def test_lcd01_proceeds_normally_once_stray_overlay_is_dismissed(self):
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["Stop", "Cancel"])
        ui.click_by_name("Stop")  # leftover Confirm Stop dialog from a prior run
        srv = FakeSrv(ui)
        with mock.patch.object(lcd_sampler, "capture_full_frame",
                                side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd01({"srv": srv})
        # The overlay must be gone afterward (dismiss succeeded via
        # click_by_name("Cancel") -- no keypad, no touch client needed) and
        # the case must reach its normal home-page judgment path rather than
        # failing over the overlay.
        self.assertNotIn("Cancel", ui.list_tap_targets()["targets"] and
                          {t["name"] for t in ui.list_tap_targets()["targets"]})
        self.assertNotEqual(result.reason or "", "")
        self.assertNotIn("stray overlay", (result.reason or ""))

    # -- 2026-09-25 Opus review fixes --

    def test_ok_cancel_dialog_without_digits_uses_cancel_click_not_backdrop(self):
        # "Cannot Start" (ui_page_home_actions.c) and the on-glass OK
        # confirms on the profile-detail/review pages are OK+Cancel dialogs
        # with no digit keys -- the old `"OK" in before` check alone would
        # have misidentified this as the PIN keypad and routed its dismiss
        # through the (wrong) backdrop touch-inject path instead of the
        # on-glass Cancel button this dialog actually has.
        ui = PopupUiTest(trigger_name="Start", overlay_names=["OK", "Cancel"])
        ui.click_by_name("Start")
        ctx = {"srv": FakeSrv(ui)}  # no _touch at all -- must not be needed
        result = C._dismiss_lcd19_overlay(ctx, ui)
        self.assertEqual(result["dismiss_method"], "click_by_name_cancel")
        self.assertTrue(result["dismissed"])
        self.assertFalse(result["is_keypad"])

    def test_stray_overlay_check_skips_a_non_home_page_with_no_click_or_inject(self):
        # _navigate_home() is best-effort and can leave the board parked on
        # some other page (e.g. a stuck Wi-Fi "network" page, which also
        # happens to have its own "Back"/"Cancel" targets) -- this must never
        # be mistaken for an LCD-19 popup: no click, no touch_inject.
        class _NetworkPageUiTest(FakeUiTest):
            def __init__(self):
                super().__init__(page="network", targets=[
                    {"name": "Back", "hidden": False},
                    {"name": "Cancel", "hidden": False},
                ])

            def click_by_name(self, name):
                raise AssertionError(f"must not click {name!r} off a non-home page")

        ui = _NetworkPageUiTest()
        touch = BackdropTouch(ui)
        srv = FakeSrv(ui)
        srv._touch = touch
        result = C._lcd19_clear_stray_overlay({"srv": srv}, ui)
        self.assertEqual(result, {"checked": True, "present": False, "page": "network"})
        self.assertEqual(touch.injected, [])

    def test_keypad_raised_stays_inconclusive_when_every_post_start_read_is_empty(self):
        # Every tap-target read after the Start click comes back empty (the
        # walk-timeout sentinel, never a real answer) -- keypad_raised must
        # stay None (INCONCLUSIVE), not become a fabricated False the way
        # `names is not None and ...` used to collapse both cases to.
        class _AlwaysEmptyAfterStartUiTest(PopupUiTest):
            def list_tap_targets(self):
                if self._overlay_open:
                    return {"targets": [], "truncated": True}
                return super().list_tap_targets()

        ui = _AlwaysEmptyAfterStartUiTest(trigger_name="Start", overlay_names=["OK", "Cancel"])
        srv = FakeSrvFull(ui)
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
        }
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.05):
            result = C._case_lcd19(ctx)
        self.assertIsNone(result.observed.get("keypad_raised"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_keypad_raised_then_auto_dismissed_is_inconclusive_not_fail(self):
        # Hypothesis under test, not a claim about any specific bench run:
        # ui_lcd_lock.c's tick_timer_cb inactivity expiry or its unlocked->
        # locked edge (firmware/KilnFW/App/drivers/ui/ui_lcd_lock.c L181-239)
        # can dismiss the keypad with no PC-side click in between. (Bench
        # run 20260930T103536Z_lcd, which first prompted this fix, does NOT
        # itself demonstrate this shape -- its summary.json shows a single
        # empty/truncated read after the Start click and no keypad signature
        # ever read at all, i.e. the "never raised, ambiguous evidence"
        # shape covered by the truncated-read test below, not this one.) If
        # the keypad is raised and then closes again before the poll ends,
        # that must still read INCONCLUSIVE ("raised then closed"), never
        # FAIL, since a raise-then-close race is not evidence the keypad
        # never appeared.
        class _RaiseThenCloseUiTest(PopupUiTest):
            def __init__(self, reads_before_close=1):
                super().__init__(
                    trigger_name="Start",
                    overlay_names=["OK", "Cancel"] + [str(d) for d in range(10)],
                )
                self._reads_since_open = 0
                self._reads_before_close = reads_before_close

            def list_tap_targets(self):
                if self._overlay_open:
                    self._reads_since_open += 1
                    if self._reads_since_open > self._reads_before_close:
                        self._overlay_open = False
                return super().list_tap_targets()

        ui = _RaiseThenCloseUiTest()
        srv = FakeSrvFull(ui)
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
        }
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.05):
            result = C._case_lcd19(ctx)
        self.assertIsNone(result.observed.get("keypad_raised"))
        self.assertTrue(result.observed.get("keypad_raised_then_closed"))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        # PIN entry must never be attempted against a keypad that already
        # closed -- wrong_pin_refused/right_pin_started stay unresolved.
        self.assertIsNone(result.observed.get("wrong_pin_refused"))
        self.assertIsNone(result.observed.get("right_pin_started"))

    def test_never_raised_with_truncated_read_stays_inconclusive(self):
        # The keypad never actually raises (Start is acknowledged but the
        # overlay never opens). The very FIRST read in the poll window comes
        # back truncated -- an ambiguous, possibly-partial read, never proof
        # the keypad is absent -- and the window then ends before two clean,
        # stable reads can follow it (a timeout only slightly larger than
        # one poll interval allows just one more read in). With no clean
        # stable tail behind the truncated read, this must stay
        # INCONCLUSIVE, never collapse to the FAIL a genuinely clean,
        # stable "never raised" tail gets (see the empty-then-clean-tail
        # FAIL test below, which contrasts this by giving the tail enough
        # room to stabilize).
        class _NeverRaisesButTruncatedUiTest(PopupUiTest):
            def __init__(self):
                super().__init__(trigger_name="Start", overlay_names=["OK", "Cancel"])
                self._click_happened = False
                self._post_click_reads = 0

            def click_by_name(self, name):
                # Acknowledge the Start tap but never actually open the
                # overlay -- the keypad genuinely never raises here.
                if name == self._trigger_name:
                    self._click_happened = True
                    return {"result": "ok"}
                return super().click_by_name(name)

            def list_tap_targets(self):
                if self._click_happened:
                    self._post_click_reads += 1
                    if self._post_click_reads == 1:
                        # A single ambiguous, possibly-partial read right
                        # after the click -- never proof of "no keypad
                        # here" the way a clean full read is (see the
                        # sibling FAIL test above). The pre-click baseline
                        # read is left untouched (real, non-truncated).
                        return {"targets": [{"name": "Start", "hidden": False}], "truncated": True}
                return super().list_tap_targets()

        ui = _NeverRaisesButTruncatedUiTest()
        srv = FakeSrvFull(ui)
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
        }
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.005):
            result = C._case_lcd19(ctx)
        self.assertIsNone(result.observed.get("keypad_raised"))
        self.assertFalse(result.observed.get("keypad_raised_then_closed", False))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_delayed_raise_after_baseline_repeats_is_not_a_false_fail(self):
        # 2026-09-30 review of fa028826: the first version of
        # _wait_for_keypad_raise stopped its poll on ANY two consecutive
        # equal *real* reads, including two equal pre-raise baseline reads
        # -- the same "0.92s FAIL" shape _wait_for_overlay_names's docstring
        # records, reproduced here with a keypad that genuinely raises, just
        # a couple of reads later than the first. Must still resolve
        # keypad_raised=True, not a false FAIL from stopping early on the
        # repeated baseline.
        class _DelayedRaiseUiTest(PinKeypadUiTest):
            # Subclasses the full idle->keypad->confirm state machine (so
            # enter_pin_verified() and friends still work once the keypad
            # is confirmed raised) but delays what list_tap_targets()
            # REPORTS after the Start click without delaying the
            # underlying state transition itself -- click_by_name() still
            # flips self._state to "keypad" immediately, the same race the
            # real board's async LVGL popup has.
            def __init__(self, reads_before_raise=2):
                super().__init__(right_pin="1234", wrong_pin="0000")
                self._reads_since_click = 0
                self._reads_before_raise = reads_before_raise
                self._click_happened = False

            def click_by_name(self, name):
                if name == "Start" and self._state == "idle":
                    self._click_happened = True
                return super().click_by_name(name)

            def list_tap_targets(self):
                if self._click_happened and self._state == "keypad":
                    self._reads_since_click += 1
                    if self._reads_since_click <= self._reads_before_raise:
                        return {"targets": [{"name": "Start", "hidden": False}], "truncated": False}
                return super().list_tap_targets()

        ui = _DelayedRaiseUiTest()
        srv = FakeSrvFull(ui)
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
        }
        # A larger timeout than the usual 0.05s test default: at the 0.1s
        # poll interval, the window needs enough iterations to reach the
        # delayed (3rd) read before giving up.
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.5):
            result = C._case_lcd19(ctx)
        self.assertIs(result.observed.get("keypad_raised"), True)
        self.assertNotEqual(result.verdict, Verdict.FAIL)

    def test_empty_read_then_clean_stable_tail_is_fail(self):
        # 2026-09-30 review: FAIL used to require zero empty/truncated reads
        # ANYWHERE in the poll window -- 6 of 12 historical bench runs had
        # 1-2 empty polls right after the Start click, so a genuine
        # never-raises regression would read INCONCLUSIVE about half the
        # time instead of FAIL. One empty read followed by a clean, stable,
        # non-keypad tail must still FAIL: the tail is unambiguous negative
        # evidence even though the window's first read was not.
        class _EmptyThenCleanTailUiTest(PopupUiTest):
            def __init__(self):
                super().__init__(trigger_name="Start", overlay_names=["OK", "Cancel"])
                self._click_happened = False
                self._post_click_reads = 0

            def click_by_name(self, name):
                if name == self._trigger_name:
                    self._click_happened = True
                    return {"result": "ok"}
                return super().click_by_name(name)

            def list_tap_targets(self):
                if self._click_happened:
                    self._post_click_reads += 1
                    if self._post_click_reads == 1:
                        # The pre-click baseline read is left untouched
                        # (real, non-truncated); only the first read of
                        # the raise poll itself is the empty/truncated one.
                        return {"targets": [], "truncated": True}
                # Every read after that is a clean, stable, non-keypad
                # home read -- the tap-target set never left home.
                return {"targets": [{"name": "Start", "hidden": False}], "truncated": False}

        ui = _EmptyThenCleanTailUiTest()
        srv = FakeSrvFull(ui)
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
        }
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.5):
            result = C._case_lcd19(ctx)
        self.assertIs(result.observed.get("keypad_raised"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_is_keypad_names_requires_digits_not_just_ok_and_cancel(self):
        # 2026-09-30 review (round 1) added the ten digit keys to
        # _is_keypad_names's signature specifically so it can never alias a
        # Confirm Start/Stop dialog (which also carries "Cancel", but never
        # "OK"). Pin that the digit requirement actually matters: an
        # "OK"+"Cancel" set with NO digit keys must not read as the keypad.
        self.assertFalse(C._is_keypad_names({"OK", "Cancel"}))
        self.assertTrue(
            C._is_keypad_names({"OK", "Cancel"} | {str(d) for d in range(10)}))

    def test_never_raised_clean_tail_tolerates_a_changing_earlier_tail_read(self):
        # 2026-09-30 review (round 2): the home screen carries a live
        # temperature label that can legitimately change value between
        # reads with no keypad ever appearing. never_raised_clean_tail used
        # to require the WHOLE tail (every read after the last bad one) to
        # be byte-identical, which would read this ordinary case as
        # INCONCLUSIVE instead of FAIL. Only the LAST 2 reads need to agree
        # now: an earlier tail read differing (simulating the temperature
        # label ticking over) must not prevent FAIL as long as the tail
        # settles on 2 matching, non-keypad reads by the end of the poll.
        class _ChangingTailUiTest(PopupUiTest):
            def __init__(self):
                super().__init__(trigger_name="Start", overlay_names=["OK", "Cancel"])
                self._click_happened = False
                self._post_click_reads = 0

            def click_by_name(self, name):
                if name == self._trigger_name:
                    self._click_happened = True
                    return {"result": "ok"}
                return super().click_by_name(name)

            def list_tap_targets(self):
                if self._click_happened:
                    self._post_click_reads += 1
                    if self._post_click_reads == 1:
                        # One bad (truncated) read right after the click,
                        # same shape as the sibling empty-then-clean-tail
                        # FAIL test above.
                        return {"targets": [], "truncated": True}
                    if self._post_click_reads == 2:
                        # An earlier tail read showing a different (but
                        # still non-keypad) reading -- e.g. "14C" ticking to
                        # "15C" on the home screen's live temperature label.
                        return {"targets": [{"name": "Start", "hidden": False},
                                             {"name": "14C", "hidden": False}],
                                "truncated": False}
                # The last 2+ reads settle on an identical, non-keypad
                # reading and stay there.
                return {"targets": [{"name": "Start", "hidden": False},
                                     {"name": "15C", "hidden": False}],
                        "truncated": False}

        ui = _ChangingTailUiTest()
        srv = FakeSrvFull(ui)
        ctx = {
            "srv": srv, "sec_client": FakeLcd19SecClient(),
            "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"},
        }
        with mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.5):
            result = C._case_lcd19(ctx)
        self.assertIs(result.observed.get("keypad_raised"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Lcd21Test(unittest.TestCase):
    def test_uses_pages_visited_by_earlier_cases(self):
        ctx = {"_lcd_pages_visited": {
            "home": {"targets": [{"name": "start", "cx": 1, "cy": 300}], "truncated": False},
        }}
        result = C._case_lcd21(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_no_pages_visited_is_not_run(self):
        result = C._case_lcd21({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_offscreen_target_from_an_earlier_case_fails(self):
        ctx = {"_lcd_pages_visited": {
            "config": {"targets": [{"name": "ghost", "cx": 1, "cy": 500}], "truncated": False},
        }}
        result = C._case_lcd21(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
