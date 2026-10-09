#!/usr/bin/env python3
"""Unit tests for LCD-07/15/17/18 (read-only set) in cases_lcd.py. Fakes only."""
from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace as NS
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import lcd_sampler  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402

V = Verdict


class Lcd07(unittest.TestCase):
    def test_idle_all_neutral_pass(self):
        self.assertEqual(C._judge_lcd07("home", ["neutral"] * 4, False).verdict, V.PASS)

    def test_idle_with_on_pill_fails(self):
        self.assertEqual(C._judge_lcd07("home", ["on", "neutral", "neutral", "neutral"], False).verdict, V.FAIL)

    def test_running_zone0_on_pass(self):
        self.assertEqual(C._judge_lcd07("home", ["on", "neutral", "neutral", "neutral"], True).verdict, V.PASS)

    def test_running_zone0_off_inconclusive(self):
        self.assertEqual(C._judge_lcd07("home", ["neutral"] * 4, True).verdict, V.INCONCLUSIVE)

    def test_other_color_inconclusive_not_fail(self):
        self.assertEqual(C._judge_lcd07("home", ["other"] + ["neutral"] * 3, False).verdict, V.INCONCLUSIVE)

    def test_wrong_page_fails(self):
        self.assertEqual(C._judge_lcd07("config", [None] * 4, False).verdict, V.FAIL)

    def test_case_samples_via_fake(self):
        ui = mock.Mock()
        ui.get_current_page.return_value = "home"
        srv = NS(_ui_test=ui, _profiles=NS(get_exec_status=lambda: NS(state_name="idle")))
        sample = lcd_sampler.RegionSample(region=C._NEUTRAL_RGB, bezel=(0, 0, 0))
        with mock.patch.object(C, "_wake_and_home"), mock.patch.object(C, "_capture", return_value="x.jpg"), \
                mock.patch.object(lcd_sampler, "sample_widget", return_value=sample), \
                mock.patch.object(C, "_downgrade_if_corners_stale", side_effect=lambda c, r, p: r):
            r = C._case_lcd07({"srv": srv})
        self.assertEqual(r.verdict, V.PASS)


class Lcd15(unittest.TestCase):
    def test_pass(self):
        r = C._judge_lcd15("network", True, "network_manage", {"A"}, ["A", "back"], {})
        self.assertEqual(r.verdict, V.PASS)

    def test_missing_ssid_fails(self):
        self.assertEqual(C._judge_lcd15("network", True, "network_manage", {"A"}, ["B"], {}).verdict, V.FAIL)

    def test_no_manage_fails(self):
        self.assertEqual(C._judge_lcd15("network", False, None, {"A"}, [], {}).verdict, V.FAIL)

    def test_wrong_manage_page_fails(self):
        self.assertEqual(C._judge_lcd15("network", True, "network", {"A"}, ["A"], {}).verdict, V.FAIL)

    def test_no_saved_inconclusive(self):
        self.assertEqual(C._judge_lcd15("network", True, "network_manage", set(), [], {}).verdict, V.INCONCLUSIVE)


class Lcd17(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(C._judge_lcd17(True, "touch_cal", "config").verdict, V.PASS)

    def test_no_tile_inconclusive(self):
        self.assertEqual(C._judge_lcd17(False, None, None).verdict, V.INCONCLUSIVE)

    def test_stuck_in_cal_fails(self):
        self.assertEqual(C._judge_lcd17(True, "touch_cal", "touch_cal").verdict, V.FAIL)

    def test_case_never_taps_calibration_dots(self):
        ui = mock.Mock()
        pages = iter(["touch_cal", "config"])
        ui.get_current_page.side_effect = lambda: next(pages, "config")
        tap = {"targets": [{"name": "Touch Calibration"}, {"name": "Cancel"}]}
        srv = NS(_ui_test=ui)
        with mock.patch.object(C, "_wake_and_home"), \
                mock.patch.object(C, "_click_then_page", return_value=(None, "config", 0, 0, 0, 0)), \
                mock.patch.object(C, "_list_tap_targets_resolving_busy", return_value=(tap, 0)), \
                mock.patch.object(C, "_wait_for_page"), mock.patch.object(C, "_wait_for_page_change"), \
                mock.patch.object(C, "_navigate_home"):
            r = C._case_lcd17({"srv": srv})
        self.assertEqual(r.verdict, V.PASS)
        clicked = [c.args[0] for c in ui.click_by_name.call_args_list]
        self.assertEqual(clicked, ["Touch Calibration", "Cancel"])


class Lcd18(unittest.TestCase):
    def test_no_trip_not_run(self):
        self.assertEqual(C._judge_lcd18(False, None, None).verdict, V.NOT_RUN)

    def test_error_tier_pass(self):
        self.assertEqual(C._judge_lcd18(True, True, False).verdict, V.PASS)

    def test_warn_tier_fails(self):
        self.assertEqual(C._judge_lcd18(True, False, True).verdict, V.FAIL)

    def test_absent_inconclusive(self):
        self.assertEqual(C._judge_lcd18(True, False, False).verdict, V.INCONCLUSIVE)

    def test_unsampled_inconclusive(self):
        self.assertEqual(C._judge_lcd18(True, None, None).verdict, V.INCONCLUSIVE)

    def test_case_no_trip(self):
        srv = NS(_safety=NS(get_diag=lambda: NS(trip_reason=0)))
        self.assertEqual(C._case_lcd18({"srv": srv}).verdict, V.NOT_RUN)


class Registered(unittest.TestCase):
    def test_judges_wired(self):
        for cid in ("LCD-07", "LCD-15", "LCD-17", "LCD-18"):
            self.assertIsNotNone(get_case(cid).judge, cid)


if __name__ == "__main__":
    unittest.main()
