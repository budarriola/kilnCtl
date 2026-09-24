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


class FindTargetCaseInsensitiveTest(unittest.TestCase):
    """ui_page_home.c's real button label is Title Case ("Start"), while
    callers here pass lowercase literals ("start") -- _find_target must
    match across that case difference, not just exact-string match."""

    def test_lowercase_query_matches_titlecase_name(self):
        targets = [{"name": "Start", "cx": 1, "cy": 2, "hidden": False}]
        t = J._find_target(targets, "start")
        self.assertIsNotNone(t)
        self.assertEqual(t["name"], "Start")

    def test_titlecase_query_matches_lowercase_name(self):
        targets = [{"name": "start", "cx": 1, "cy": 2, "hidden": False}]
        t = J._find_target(targets, "Start")
        self.assertIsNotNone(t)

    def test_no_match_returns_none(self):
        targets = [{"name": "Start", "cx": 1, "cy": 2, "hidden": False}]
        self.assertIsNone(J._find_target(targets, "pause"))

    def test_missing_name_field_does_not_raise(self):
        targets = [{"cx": 1, "cy": 2, "hidden": False}]
        self.assertIsNone(J._find_target(targets, "start"))


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

    def test_absent_pause_target_still_passes_with_a_capture(self):
        """A board that omits a hidden Pause target entirely must not pin
        LCD-01 at INCONCLUSIVE forever when the camera worked fine."""
        targets = [t for t in _HOME_TARGETS if t["name"] != "pause"]
        r = J.judge_lcd_home_idle("home", targets, True, None)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_absent_pause_still_inconclusive_without_a_capture(self):
        targets = [t for t in _HOME_TARGETS if t["name"] != "pause"]
        r = J.judge_lcd_home_idle("home", targets, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

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

    def test_color_debug_attached_to_observed_and_evidence(self):
        # 2026-09-24 fix: a FAIL (or PASS) on the color half must carry the
        # sampled/bezel RGB and the capture path it came from, not a bare
        # bool with nothing to corroborate it against.
        color_debug = {
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (60, 138, 92), "bezel_rgb": (26, 31, 43), "matches": False},
        }
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True, color_debug=color_debug)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(r.observed["color_debug"], color_debug)
        self.assertIn("/tmp/run/captures/lcd01_start_pause.jpg", r.evidence)

    def test_no_color_debug_leaves_evidence_empty(self):
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, True, True)
        self.assertEqual(r.evidence, [])
        self.assertNotIn("color_debug", r.observed)


_CONFIG_TARGETS = [
    {"name": "Profiles", "cx": 100, "cy": 100, "hidden": False},
    {"name": "Temperature", "cx": 200, "cy": 100, "hidden": False},
    {"name": "Network / Wi-Fi", "cx": 300, "cy": 100, "hidden": False},
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
        targets = [dict(t, hidden=True) if t["name"] == "Network / Wi-Fi" else t for t in _CONFIG_TARGETS]
        r = J.judge_lcd_config_hub("config", targets)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_stale_shortened_network_label_fails(self):
        # Negative test for the 2026-09-24 LCD-08 fix: the button's real
        # firmware label is "Network / Wi-Fi" (ui_page_config.c's
        # build_nav_item() call); a page offering only the old, shortened
        # "Network" literal must still be reported as missing the real tile
        # -- proving the comparison wasn't loosened to a bare substring
        # match that would let the stale name silently pass.
        targets = [dict(t, name="Network") if t["name"] == "Network / Wi-Fi" else t for t in _CONFIG_TARGETS]
        r = J.judge_lcd_config_hub("config", targets)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("Network / Wi-Fi", r.reason)


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


class LcdHomeFiringTest(unittest.TestCase):
    def test_passes_with_full_data(self):
        r = J.judge_lcd_home_firing("home", _HOME_TARGETS, True, True, [1.0, 60.0], True, True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_page_fails(self):
        r = J.judge_lcd_home_firing("profiles", _HOME_TARGETS, True, True, [1.0, 60.0], True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_start_not_reading_stop_fails(self):
        r = J.judge_lcd_home_firing("home", _HOME_TARGETS, False, True, [1.0, 60.0], True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_progress_not_advancing_fails(self):
        r = J.judge_lcd_home_firing("home", _HOME_TARGETS, True, True, [10.0, 10.0], True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_optional_data_is_inconclusive_not_pass(self):
        r = J.judge_lcd_home_firing("home", _HOME_TARGETS, True, None, [], None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdHomePausedTest(unittest.TestCase):
    def test_resume_label_and_zero_duties_pass(self):
        r = J.judge_lcd_home_paused("Resume", [0.0, 0.0, 0.0])
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_label_fails(self):
        r = J.judge_lcd_home_paused("Stop", [0.0, 0.0])
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_nonzero_duty_while_paused_fails(self):
        r = J.judge_lcd_home_paused("Resume", [0.0, 0.4])
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_label_captured_is_inconclusive(self):
        r = J.judge_lcd_home_paused(None, [])
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdHomeTrippedTest(unittest.TestCase):
    def test_visible_matching_and_cleared_passes(self):
        r = J.judge_lcd_home_tripped(True, True, True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_strip_not_visible_fails(self):
        r = J.judge_lcd_home_tripped(False, True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_strip_still_lit_after_clear_fails(self):
        r = J.judge_lcd_home_tripped(True, True, False)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_camera_data_is_inconclusive(self):
        r = J.judge_lcd_home_tripped(True, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


_PROFILE_ROWS = [
    {"name": "profile_row_0", "starred": True},
    {"name": "profile_row_1", "starred": False},
]


class LcdProfilesPickerTest(unittest.TestCase):
    def test_passes_with_valid_picker(self):
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, True, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_page_fails(self):
        r = J.judge_lcd_profiles_picker("home", _PROFILE_ROWS, True, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_too_many_rows_fails(self):
        rows = [{"name": f"profile_row_{i}", "starred": False} for i in range(5)]
        r = J.judge_lcd_profiles_picker("profiles", rows, True, True, "profile_detail", max_rows=4)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_favorite_not_sorted_first_fails(self):
        rows = [{"name": "profile_row_0", "starred": False}, {"name": "profile_row_1", "starred": True}]
        r = J.judge_lcd_profiles_picker("profiles", rows, True, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_paging_fails(self):
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, False, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_rows_is_inconclusive(self):
        r = J.judge_lcd_profiles_picker("profiles", [], True, True, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdTemperaturePageTest(unittest.TestCase):
    def test_matching_values_pass(self):
        r = J.judge_lcd_temperature_page("temperature", {0: 100.0}, {0: 100.4}, True, True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_page_fails(self):
        r = J.judge_lcd_temperature_page("home", {0: 100.0}, {0: 100.0}, True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_value_mismatch_fails(self):
        r = J.judge_lcd_temperature_page("temperature", {0: 100.0}, {0: 110.0}, True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_safety_line_mismatch_fails(self):
        r = J.judge_lcd_temperature_page("temperature", {0: 100.0}, {0: 100.0}, False, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_safety_line_is_inconclusive_not_fail(self):
        r = J.judge_lcd_temperature_page("temperature", {0: 100.0}, {0: 100.0}, None, True)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_no_zone_rows_is_inconclusive(self):
        r = J.judge_lcd_temperature_page("temperature", {}, {0: 100.0}, True, True)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdDiagnosticsPagesTest(unittest.TestCase):
    def test_all_titles_in_order_passes(self):
        r = J.judge_lcd_diagnostics_pages(list(J._DIAG_TITLES), False, 0, 2.0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_order_fails(self):
        titles = list(J._DIAG_TITLES)
        titles[0], titles[1] = titles[1], titles[0]
        r = J.judge_lcd_diagnostics_pages(titles, False, 0, 2.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_relay_life_reset_button_present_fails(self):
        r = J.judge_lcd_diagnostics_pages(list(J._DIAG_TITLES), True, 0, 2.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_crash_report_visible_entries_fails(self):
        r = J.judge_lcd_diagnostics_pages(list(J._DIAG_TITLES), False, 1, 2.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_heap_diff_too_large_fails(self):
        r = J.judge_lcd_diagnostics_pages(list(J._DIAG_TITLES), False, 0, 25.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unparseable_heap_is_inconclusive_not_pass(self):
        r = J.judge_lcd_diagnostics_pages(list(J._DIAG_TITLES), False, 0, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdPinLockTest(unittest.TestCase):
    def test_full_flow_passes(self):
        r = J.judge_lcd_pin_lock(True, True, True, True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_keypad_not_raised_fails(self):
        r = J.judge_lcd_pin_lock(False, True, True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_wrong_pin_accepted_fails(self):
        r = J.judge_lcd_pin_lock(True, False, True, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_right_pin_refused_fails(self):
        r = J.judge_lcd_pin_lock(True, True, False, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_stop_gated_behind_pin_fails(self):
        # This is the safety-critical check: Stop must NEVER be gated.
        r = J.judge_lcd_pin_lock(True, True, True, False)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_data_is_inconclusive_not_pass(self):
        r = J.judge_lcd_pin_lock(None, None, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


if __name__ == "__main__":
    unittest.main()
