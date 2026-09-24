#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_lcd -- the fetch half of
LCD-01/08/21. srv._ui_test and lcd_sampler are both mocked; these tests
confirm each _case_XXX fetches the right thing, degrades gracefully when
the camera is unavailable, and hands the right data to judgments.py --
never that a real board or webcam answers a certain way.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import lcd_sampler  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


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


_CONFIG_TARGETS = [
    {"name": "Profiles", "cx": 100, "cy": 100, "hidden": False},
    {"name": "Temperature", "cx": 200, "cy": 100, "hidden": False},
    {"name": "Network", "cx": 300, "cy": 100, "hidden": False},
    {"name": "Diagnostics", "cx": 100, "cy": 200, "hidden": False},
]


class Lcd08Test(unittest.TestCase):
    def test_menu_tap_reaches_config_hub(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_menu_tap_not_found_fails(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="not_found"))
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not_found", result.reason)

    def test_records_visited_page_in_ctx(self):
        ctx = {"srv": FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))}
        C._case_lcd08(ctx)
        self.assertIn("config", ctx["_lcd_pages_visited"])


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
    def __init__(self, status=None):
        self._status = status or FakeExecStatus()

    def get_exec_status(self):
        return self._status


class FakeThermo:
    def __init__(self, readings=None):
        self._readings = readings or {}

    def read_all(self):
        return self._readings


class FakeSrvFull(FakeSrv):
    """Extends FakeSrv with the extra client attributes LCD-02/03/04/09/14/16/19
    read (safety/profiles/thermo, and safety_clear_trip() at module scope)."""

    def __init__(self, ui_test, safety=None, profiles=None, thermo=None, clear_trip_raises=False):
        super().__init__(ui_test)
        self._safety = safety or FakeSafety()
        self._profiles = profiles or FakeProfiles()
        self._thermo = thermo or FakeThermo()
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
        if name == "Home":
            self.home_calls += 1
            self._page = "home"
            return {"result": "ok"}
        if name == "Back":
            return {"result": "not_found"}
        if self._call_count == self._raise_on_call:
            raise RuntimeError("board went unresponsive")
        return super().click_by_name(name)


class Lcd02Test(unittest.TestCase):
    def test_not_run_when_hp01_absent(self):
        srv = FakeSrvFull(FakeUiTest(page="home", targets=_HOME_TARGETS))
        result = C._case_lcd02({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_wrong_page_fails(self):
        srv = FakeSrvFull(FakeUiTest(page="profiles", targets=_HOME_TARGETS))
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
        srv = FakeSrvFull(FakeUiTest(page="home"), safety=FakeSafety(FakeSafetyDiag(trip_reason=6, trip_mask=0x0040)))
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


_PROFILES_NAV = {"settings": "config", "Profiles": "profiles", "profile_row_0": "profile_detail"}
_PROFILES_PAGE_TARGETS = {
    "home": [{"name": "settings", "cx": 10, "cy": 10, "hidden": False}],
    "config": [{"name": "Profiles", "cx": 100, "cy": 100, "hidden": False}],
    "profiles": [
        {"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False, "starred": False},
        {"name": "paging", "cx": 400, "cy": 300, "hidden": False},
        {"name": "new_profile", "cx": 400, "cy": 10, "hidden": False},
    ],
    "profile_detail": [],
}


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

    def test_finally_restores_home_on_exception(self):
        ui = RaisingUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd09({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)


_TEMP_NAV = {"settings": "config", "Temperature": "temperature"}


class Lcd14Test(unittest.TestCase):
    def test_matching_values_pass(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
            "temperature": [{"name": "zone_temp_0", "value": 100.0}, {"name": "safety_line", "on": False}],
        }
        ui = PageNavUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_value_mismatch_fails(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
            "temperature": [{"name": "zone_temp_0", "value": 100.0}, {"name": "safety_line", "on": False}],
        }
        ui = PageNavUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 150.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)

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


class Lcd16Test(unittest.TestCase):
    def test_finally_restores_home_on_exception(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Diagnostics", "hidden": False}],
        }
        nav_map = {"settings": "config", "Diagnostics": "diag_hub"}
        ui = RaisingUiTest(page="home", page_targets=page_targets, nav_map=nav_map, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd16({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)

    def test_menu_tap_failure_fails(self):
        targets = [{"name": "settings", "hidden": False}]
        ui = FakeUiTest(page="home", targets=targets, click_result="not_found")
        srv = FakeSrvFull(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Lcd19Test(unittest.TestCase):
    def test_not_run_when_no_pin_configured(self):
        srv = FakeSrvFull(FakeUiTest(page="home"))
        result = C._case_lcd19({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_stop_gated_fails(self):
        class PinUi(FakeUiTest):
            def click_by_name(self, name):
                if name == "start":
                    self._page = "pin_entry"
                    return {"result": "ok"}
                return super().click_by_name(name)

        ui = PinUi(page="home")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000", "firing_active_with_lock": True}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Stop", result.reason)


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
