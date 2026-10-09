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

    def test_color_mismatch_with_suspected_cast_downgrades_to_inconclusive(self):
        # 2026-09-25 fix: this branch used to only annotate `reason` and fall
        # through to a hard FAIL, inconsistent with the weaker
        # bg_out_of_tolerance signal below it, which DOES downgrade. Evidence:
        # run 20260925T055234Z's bg_reference read chroma offset 0.1521 (just
        # over CAST_CHROMA_THRESHOLD 0.15) yet the case reported FAIL, not
        # INCONCLUSIVE -- reads_as_bezel is a separate, earlier branch (still a
        # hard FAIL, see test_bg_reference_on_bezel_still_fails_with_geometry_
        # note below), so this branch only fires on non-bezel cast, and a
        # genuine cast signal at least as strong as bg_out_of_tolerance's
        # should get the same INCONCLUSIVE treatment.
        color_debug = {
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (25, 96, 98), "matches": False},
            "bg_reference": {"sampled_rgb": (6, 40, 60), "chroma_offset": 0.30, "cast_threshold": 0.15, "cast_suspected": True},
        }
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True, color_debug=color_debug)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("color cast", r.reason)

    def test_bg_reference_on_bezel_still_fails_with_geometry_note(self):
        # The exact 2026-09-24 shape: the reference reads as the bezel, so
        # the geometry is suspect. Still FAIL, never INCONCLUSIVE.
        color_debug = {
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (25, 96, 98), "matches": False},
            "bg_reference": {"sampled_rgb": (6, 13, 22), "bezel_rgb": (6, 11, 16), "reads_as_bezel": True,
                             "chroma_offset": 0.156, "cast_threshold": 0.15, "cast_suspected": False},
        }
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True, color_debug=color_debug)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("reads as bezel", r.reason)

    def test_color_mismatch_without_suspected_cast_still_fails(self):
        # The cast check must never loosen COLOR_MATCH_TOLERANCE/
        # CHROMA_MATCH_TOLERANCE themselves: a clean background reference
        # alongside a genuine Start-color mismatch is still a hard FAIL.
        color_debug = {
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (214, 32, 32), "matches": False},
            "bg_reference": {"sampled_rgb": (24, 32, 44), "chroma_offset": 0.02, "cast_threshold": 0.15, "cast_suspected": False},
        }
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True, color_debug=color_debug)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_bad_background_downgrades_color_mismatch_to_inconclusive(self):
        # 2026-09-25 bench evidence (20260925T170424Z_full/summary.json):
        # Start sampled [27,153,76] vs target [92,192,110] (distance 83,
        # chroma 0.17 -- a real mismatch by both measures), but the
        # background reference sampled [52,90,111] vs target [26,31,43]:
        # chroma offset 0.0717 (under CAST_CHROMA_THRESHOLD, so the cast
        # fallback never engaged) yet distance ~93.7, over double
        # COLOR_MATCH_TOLERANCE (45) -- the background itself is plainly
        # wrong, so the Start mismatch on the same frame is not trustworthy.
        color_debug = {
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (27, 153, 76), "matches": False},
            "bg_reference": {
                "sampled_rgb": (52, 90, 111), "chroma_offset": 0.0717, "cast_threshold": 0.15,
                "cast_suspected": False, "reads_as_bezel": False,
                "distance": 93.7, "distance_tolerance": 45.0, "chroma_tolerance": 0.10,
                "bg_out_of_tolerance": True,
            },
        }
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True, color_debug=color_debug)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("background reference off", r.reason)

    def test_good_background_with_wrong_button_still_fails(self):
        # Negative direction: a clean background (bg_out_of_tolerance False)
        # alongside a genuine Start-color mismatch must still be a hard FAIL
        # -- the downgrade above must never apply when the background is
        # trustworthy.
        color_debug = {
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (214, 32, 32), "matches": False},
            "bg_reference": {
                "sampled_rgb": (24, 32, 44), "chroma_offset": 0.02, "cast_threshold": 0.15,
                "cast_suspected": False, "reads_as_bezel": False,
                "distance": 2.0, "distance_tolerance": 45.0, "chroma_tolerance": 0.10,
                "bg_out_of_tolerance": False,
            },
        }
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, False, True, color_debug=color_debug)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_good_background_with_right_button_passes(self):
        # Sibling positive case: a clean background reference alongside a
        # genuine Start-color match is an ordinary PASS, unaffected by any
        # of this diagnostic machinery.
        r = J.judge_lcd_home_idle("home", _HOME_TARGETS, True, True, color_debug={
            "capture_path": "/tmp/run/captures/lcd01_start_pause.jpg",
            "start": {"sampled_rgb": (92, 192, 110), "matches": True},
            "bg_reference": {
                "sampled_rgb": (24, 32, 44), "chroma_offset": 0.02, "cast_threshold": 0.15,
                "cast_suspected": False, "reads_as_bezel": False,
                "distance": 2.0, "distance_tolerance": 45.0, "chroma_tolerance": 0.10,
                "bg_out_of_tolerance": False,
            },
        })
        self.assertEqual(r.verdict, Verdict.PASS)


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
        # Review fix of ff55bda2: the Prev/Next icons DO carry a tap name
        # (build_icon() sets no tag, so kiln_ui.c falls back to the glyph
        # label text); cases_lcd.py locates them by position. A definite
        # "absent" (False) is a real FAIL.
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, False, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("paging", r.reason)

    def test_missing_new_icon_fails(self):
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, True, False, "profile_detail")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_undecidable_paging_is_inconclusive(self):
        # Both paging icons disabled (single page): cannot be confirmed.
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, None, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_no_topbar_anchor_is_inconclusive(self):
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, None, None, "profile_detail")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_undecidable_paging_does_not_mask_detail_fail(self):
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, None, True, "home")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_paging_present_and_everything_else_ok_still_passes(self):
        # Negative-test companion: proves the INCONCLUSIVE above is really
        # gated on paging_present, not something else in this fixture.
        r = J.judge_lcd_profiles_picker("profiles", _PROFILE_ROWS, True, True, "profile_detail")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_no_rows_is_inconclusive(self):
        r = J.judge_lcd_profiles_picker("profiles", [], True, True, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_no_rows_but_known_positive_profiles_count_fails(self):
        r = J.judge_lcd_profiles_picker("profiles", [], True, True, None, profiles_count=2)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("profiles_count", r.reason)

    def test_no_rows_with_zero_profiles_count_stays_inconclusive(self):
        r = J.judge_lcd_profiles_picker("profiles", [], True, True, None, profiles_count=0)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_no_rows_with_unknown_profiles_count_stays_inconclusive(self):
        r = J.judge_lcd_profiles_picker("profiles", [], True, True, None, profiles_count=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdTemperaturePageTest(unittest.TestCase):
    # Round 3 rewrite (item 3): zone rows/safety line are plain labels, not
    # tap targets, so the judge is now driven by per-zone render-or-not
    # booleans from capture sampling, never by a value comparison against
    # a thermo reading (readings are passed through only for observed{}
    # context, never gate the verdict).
    #
    # Round 4 (opus review of d66ba612): expected_zones (guessed from
    # readings, `max(len(readings), 3)`) was replaced by configured_zones
    # (the board's own real GET_ZONES count) plus header_rendered (the
    # Relays-card-header layout-shift check) and zero_zone_label_rendered
    # (the 0-zones state) -- see judgments.py's own docstring for the failure
    # shape this closes: a guessed count could sample the Relays card header
    # as if it were an (N)th zone row and PASS.
    def test_all_rows_rendered_pass(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True, 2: True}, {0: 100.0}, None, True,
            configured_zones=3, header_rendered=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_page_fails(self):
        r = J.judge_lcd_temperature_page("home", {0: True}, {0: 100.0}, None, True, configured_zones=1)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_row_fails(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: False, 2: True}, {0: 100.0}, None, True,
            configured_zones=3, header_rendered=True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_zone_rows_sampled_is_inconclusive(self):
        r = J.judge_lcd_temperature_page("temperature", {}, {0: 100.0}, None, True, configured_zones=3)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_undecided_rows_are_inconclusive_not_pass(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: None, 2: True}, {0: 100.0}, None, True,
            configured_zones=3, header_rendered=True)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_safety_line_definitely_absent_during_firing_fails(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True, 2: True}, {0: 100.0}, False, True,
            configured_zones=3, header_rendered=True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_safety_line_undetermined_never_blocks_pass(self):
        # The Safety (K4) line's y-position is dynamic (it sits after a
        # wrapped relay-button row inside a separate card), so it is
        # sampled best-effort only -- a None reading must never hold up an
        # otherwise-passing zone-row result.
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True, 2: True}, {0: 100.0}, None, True,
            configured_zones=3, header_rendered=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    # -- Round 4: configured_zones / header_rendered / zero_zone_label_rendered --

    def test_configured_zone_count_unreadable_is_inconclusive_never_pass(self):
        # The GET_ZONES query failed (or was never attempted) -- the old code
        # would have guessed 3; this must never fabricate a count.
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True, 2: True}, {0: 100.0}, None, True,
            configured_zones=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_correct_row_count_with_header_in_place_passes(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True}, {}, None, False,
            configured_zones=2, header_rendered=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_missing_row_with_header_shifted_up_fails(self):
        # The exact bug shape the opus review found: board configured for 3
        # zones but only 2 real rows rendered, so the Relays card header
        # shifted up one row pitch and landed where the sampler expects the
        # 3rd zone row -- reading as non-background content, same as a real
        # row would. zone_rows_rendered alone (all True) would have PASSed;
        # header_rendered=False at the position a 3rd row's header SHOULD
        # occupy is what catches the shift.
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True, 2: True}, {}, None, False,
            configured_zones=3, header_rendered=False)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_zero_zones_configured_with_label_rendered_passes(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {}, {}, None, False,
            configured_zones=0, zero_zone_label_rendered=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_zero_zones_configured_label_missing_fails(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {}, {}, None, False,
            configured_zones=0, zero_zone_label_rendered=False)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_zero_zones_configured_no_capture_is_inconclusive(self):
        r = J.judge_lcd_temperature_page(
            "temperature", {}, {}, None, False,
            configured_zones=0, zero_zone_label_rendered=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_header_unsampled_is_inconclusive_never_pass(self):
        # Every row reads rendered, but without the header sample the
        # "N-1 rows, header shifted up" shape cannot be ruled out.
        r = J.judge_lcd_temperature_page(
            "temperature", {0: True, 1: True, 2: True}, {}, None, False,
            configured_zones=3, header_rendered=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class LcdDiagnosticsPagesTest(unittest.TestCase):
    # Round 3 rewrite (item 2): no wire command reads a diagnostics
    # sub-page's title text at all, so the judge is now driven by
    # paging-hop count, Next's disabled state at the last page, where the
    # Crash Report Acknowledge button was seen, and whether Relay Life's
    # (removed) Reset button reappeared -- never by a title list.
    def test_full_paging_with_ack_at_last_step_passes(self):
        r = J.judge_lcd_diagnostics_pages(7, 7, True, 7, False)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_relay_life_reset_present_fails(self):
        r = J.judge_lcd_diagnostics_pages(7, 7, True, 7, True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("Reset", r.reason)

    def test_ack_seen_at_wrong_step_fails(self):
        r = J.judge_lcd_diagnostics_pages(7, 7, True, 3, False)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("Acknowledge", r.reason)

    def test_paging_stopped_early_is_inconclusive(self):
        r = J.judge_lcd_diagnostics_pages(3, 7, None, None, False)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_next_still_enabled_at_last_page_fails(self):
        r = J.judge_lcd_diagnostics_pages(7, 7, False, 7, False)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_next_disabled_state_undetermined_is_inconclusive(self):
        r = J.judge_lcd_diagnostics_pages(7, 7, None, 7, False)
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

    def test_stop_not_gated_fails(self):
        # Owner decision 2026-09-28: Stop must require the PIN; a Stop that
        # opens Confirm Stop with no keypad is a FAIL.
        r = J.judge_lcd_pin_lock(True, True, True, False)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("Stop", r.reason)

    def test_missing_data_is_inconclusive_not_pass(self):
        r = J.judge_lcd_pin_lock(None, None, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_missing_data_names_every_unresolved_stage(self):
        r = J.judge_lcd_pin_lock(None, None, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        for stage in ("keypad_raised", "wrong_pin_refused", "right_pin_started", "stop_gated"):
            self.assertIn(stage, r.reason)

    def test_missing_data_names_only_the_unresolved_stage(self):
        # 2026-09-25 fix: the reason must name which specific stage(s) came
        # back None, not the generic "missing UI_TEST API or camera" that
        # gave no way to tell a first-digit click race (LCD-19 bench
        # evidence, 20260925T170357Z_full/summary.json) apart from a
        # missing capability.
        r = J.judge_lcd_pin_lock(True, None, True, True)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("wrong_pin_refused", r.reason)
        self.assertNotIn("keypad_raised,", r.reason)
        self.assertNotIn("right_pin_started", r.reason)
        self.assertNotIn("stop_gated", r.reason)


if __name__ == "__main__":
    unittest.main()
