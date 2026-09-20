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
