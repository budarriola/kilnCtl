#!/usr/bin/env python3
"""Unit tests for the LCD-01/08/21 judge functions in
kilnctrl.bench_test.judgments (plan §8 Wave 1c). Kept in a separate file
from test_bench_test_judgments.py (which predates this wave) rather than
appended there, so this wave's diff is self-contained.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_judgments_lcd.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402

_HOME_TARGETS = [
    {"name": "start", "cx": 240, "cy": 280, "hidden": False},
    {"name": "pause", "cx": 240, "cy": 280, "hidden": True},
    {"name": "profile_name", "cx": 240, "cy": 30, "hidden": False},
]


class LcdHomeIdleTest(unittest.TestCase):
    def test_passes_with_camera_data(self):
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, True, True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_page_fails(self):
        r = J.judge_lcd_home_idle("profiles", _HOME_TARGETS, True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("page", r.reason)

    def test_missing_start_target_fails(self):
        targets = [t for t in _HOME_TARGETS if t["name"] != "start"]
        r = J.judge_lcd_home_idle("home", targets, None, None)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_hidden_start_target_fails(self):
        targets = [dict(t, hidden=True) if t["name"] == "start" else t for t in _HOME_TARGETS]
        r = J.judge_lcd_home_idle("home", targets, None, None)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_pause_not_hidden_fails(self):
        targets = [dict(t, hidden=False) if t["name"] == "pause" else t for t in _HOME_TARGETS]
        r = J.judge_lcd_home_idle("home", targets, None, None)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_camera_data_is_inconclusive_not_pass(self):
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_start_color_mismatch_fails(self):
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True)
        self.assertEqual(r.verdict, Verdict.FAIL)


_CONFIG_TARGETS = [
    {"name": "Profiles", "cx": 100, "cy": 100, "hidden": False},
    {"name": "Temperature", "cx": 200, "cy": 100, "hidden": False},
    {"name": "Network", "cx": 300, "cy": 100, "hidden": False},
    {"name": "Diagnostics", "cx": 100, "cy": 200, "hidden": False},
]


class LcdConfigHubTest(unittest.TestCase):
    def test_passes_with_all_expected_tiles(self):
        r = J.judge_lcd_config_hub("config", _CONFIG_TARGETS)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_page_fails(self):
        r = J.judge_lcd_config_hub("home", _CONFIG_TARGETS)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_tile_fails(self):
        targets = [t for t in _CONFIG_TARGETS if t["name"] != "Diagnostics"]
        r = J.judge_lcd_config_hub("config", targets)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("Diagnostics", r.reason)

    def test_hidden_tile_counts_as_missing(self):
        targets = [dict(t, hidden=True) if t["name"] == "Network" else t for t in _CONFIG_TARGETS]
        r = J.judge_lcd_config_hub("config", targets)
        self.assertEqual(r.verdict, Verdict.FAIL)


class LcdNoScrollBudgetTest(unittest.TestCase):
    def test_passes_when_every_target_within_budget(self):
        pages = {
            "home": {"targets": [{"name": "start", "cx": 1, "cy": 300}], "truncated": False},
            "config": {"targets": [{"name": "Profiles", "cx": 1, "cy": 310}], "truncated": False},
        }
        r = J.judge_lcd_no_scroll_budget(pages)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_no_pages_visited_is_not_run(self):
        r = J.judge_lcd_no_scroll_budget({})
        self.assertEqual(r.verdict, Verdict.NOT_RUN)

    def test_offscreen_target_fails(self):
        pages = {"home": {"targets": [{"name": "ghost", "cx": 1, "cy": 400}], "truncated": False}}
        r = J.judge_lcd_no_scroll_budget(pages)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("home", r.reason)

    def test_truncated_page_excludes_its_offscreen_targets(self):
        # A truncated report is a wire-format limit, not evidence of an
        # actual off-screen target -- must not count as an offender.
        pages = {"home": {"targets": [{"name": "ghost", "cx": 1, "cy": 400}], "truncated": True}}
        r = J.judge_lcd_no_scroll_budget(pages)
        self.assertEqual(r.verdict, Verdict.PASS)


if __name__ == "__main__":
    unittest.main()
