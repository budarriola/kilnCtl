#!/usr/bin/env python3
"""Unit tests for the HP-*/SP-* pure judgment functions added in
kilnctrl.bench_test.judgments for Wave 1b (docs/BENCH_TEST_SYSTEM_PLAN.md
section 8). Per feedback_negative_test_every_check, every function gets at
least one test that feeds it a bad input and confirms it reports something
other than PASS.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_judgments_heat.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class RestedTest(unittest.TestCase):
    def test_all_within_band_of_ambient_is_rested(self):
        self.assertTrue(J.judge_rested({0: 24.5, 1: 25.0, 2: 23.8}, ambient_ref=24.0, band=2.0))

    def test_one_zone_outside_band_is_not_rested(self):
        self.assertFalse(J.judge_rested({0: 24.0, 1: 29.0, 2: 24.0}, ambient_ref=24.0, band=2.0))

    def test_no_temps_is_not_rested(self):
        self.assertFalse(J.judge_rested({}, ambient_ref=24.0))

    def test_zones_agree_with_each_other_but_not_ambient_is_not_rested(self):
        # All three zones agree with each other but are 5C off the ambient
        # reference -- the gate checks BOTH conditions, not just mutual
        # agreement (a stale/wrong ambient reference must still fail this).
        self.assertFalse(J.judge_rested({0: 29.0, 1: 29.5, 2: 29.2}, ambient_ref=24.0, band=2.0))


class ZoneRiseOrderingTest(unittest.TestCase):
    def test_primary_rises_and_others_rise_less_passes(self):
        r = J.judge_zone_rise_ordering({0: 8.0, 1: 3.0, 2: 2.0}, primary_zone=0, min_rise=5.0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_primary_below_min_rise_fails(self):
        r = J.judge_zone_rise_ordering({0: 3.0, 1: 1.0, 2: 1.0}, primary_zone=0, min_rise=5.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_coupled_zone_rising_as_much_as_primary_fails(self):
        # This is the ordering rule the task called out by name: coupling is
        # real and other zones WILL rise, but none may rise as much as the
        # primary zone.
        r = J.judge_zone_rise_ordering({0: 8.0, 1: 8.0, 2: 1.0}, primary_zone=0, min_rise=5.0)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("1", r.reason)

    def test_missing_primary_zone_fails(self):
        r = J.judge_zone_rise_ordering({1: 3.0}, primary_zone=0, min_rise=5.0)
        self.assertEqual(r.verdict, Verdict.FAIL)


class AllZonesRiseTest(unittest.TestCase):
    def test_all_three_rise_enough_passes(self):
        r = J.judge_all_zones_rise({0: 6.0, 1: 5.5, 2: 7.0}, zone_mask=0b111, min_rise=5.0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_one_zone_short_fails(self):
        r = J.judge_all_zones_rise({0: 6.0, 1: 2.0, 2: 7.0}, zone_mask=0b111, min_rise=5.0)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("1", r.reason)


class RelayEnergizedTest(unittest.TestCase):
    def test_energized_only_while_running_passes(self):
        samples = [("running", True), ("running", True), ("done", False)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_energized_after_stop_fails(self):
        # Interior mismatch (index 1 of 3, neither first nor last sample) --
        # must still FAIL. Edge tolerance only covers the very first/last
        # poll, which can straddle the two separate HTTP calls (running-state
        # vs relay-energized) at a start/stop transition; a mismatch that
        # persists into the middle of the run is a real ordering defect.
        samples = [("running", True), ("done", True), ("done", False)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_samples_is_inconclusive(self):
        r = J.judge_relay_energized([])
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_all_none_samples_is_inconclusive_not_pass(self):
        r = J.judge_relay_energized([("running", None), ("done", None)])
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_two_sample_series_gets_no_edge_tolerance(self):
        # Pre-23fae0a5 test, restored: with only 2 reported samples both are
        # "edges", so tolerating them would make this a vacuous PASS.
        samples = [("running", True), ("done", True)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_two_sample_never_energized_while_running_fails(self):
        samples = [("running", False), ("done", True)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_single_sample_mismatch_fails(self):
        r = J.judge_relay_energized([("running", False)])
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_first_sample_mismatch_is_tolerated(self):
        # HP-01 root cause: running-state and relay-energized come from two
        # separate HTTP round trips, not one atomic read. A mismatch confined
        # to the very first sample (poll landed between the two calls at
        # start-of-run) is tolerated, not a real ordering defect.
        samples = [("done", True), ("running", True), ("running", True)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_last_sample_mismatch_is_tolerated(self):
        samples = [("running", True), ("running", True), ("done", True)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_both_edges_mismatched_but_interior_clean_is_tolerated(self):
        samples = [("done", True), ("running", True), ("done", True)]
        r = J.judge_relay_energized(samples)
        self.assertEqual(r.verdict, Verdict.PASS)


class PauseResumeTest(unittest.TestCase):
    def test_clean_pause_and_completion_passes(self):
        r = J.judge_pause_resume("paused", [0.0, 0.0, 0.0], "done")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_nonzero_duty_while_paused_fails(self):
        r = J.judge_pause_resume("paused", [0.0, 0.3, 0.0], "done")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_did_not_actually_pause_fails(self):
        r = J.judge_pause_resume("running", [0.0, 0.0, 0.0], "done")
        self.assertEqual(r.verdict, Verdict.FAIL)


class StopTest(unittest.TestCase):
    def test_clean_stop_passes(self):
        r = J.judge_stop("idle", [0.0, 0.0, 0.0], [False, False, False], acked=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_still_running_fails(self):
        r = J.judge_stop("running", [0.0, 0.0, 0.0], [False, False, False], acked=True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_relay_still_on_fails(self):
        r = J.judge_stop("idle", [0.0, 0.0, 0.0], [False, True, False], acked=True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_not_acked_fails(self):
        r = J.judge_stop("idle", [0.0, 0.0, 0.0], [False, False, False], acked=False)
        self.assertEqual(r.verdict, Verdict.FAIL)


class UnauthenticatedStopTest(unittest.TestCase):
    def test_200_and_stopped_passes(self):
        r = J.judge_unauthenticated_stop(200, "idle")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_non_200_fails(self):
        r = J.judge_unauthenticated_stop(401, "running")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_still_running_after_200_fails(self):
        r = J.judge_unauthenticated_stop(200, "running")
        self.assertEqual(r.verdict, Verdict.FAIL)


class FiringHistoryTest(unittest.TestCase):
    def test_matching_entry_with_fields_passes(self):
        entries = [{"profile_name": "BENCH_HP", "start_time": "t", "outcome": "done"}]
        r = J.judge_firing_history(entries)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_real_firmware_field_names_pass(self):
        # HP-08: firmware's actual dashboard_format_firing_history_json()
        # schema -- run_started_unix_s/duration_s, no separate outcome/state
        # field at all. A record existing with duration_s IS the outcome.
        entries = [{
            "profile_name": "BENCH_HP", "run_started_unix_s": 1234567890,
            "duration_s": 42, "zone_mask": 0b111, "zones": [],
        }]
        r = J.judge_firing_history(entries)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_empty_history_fails(self):
        r = J.judge_firing_history([])
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_matching_name_fails(self):
        entries = [{"profile_name": "USER_PROFILE", "start_time": "t", "outcome": "done"}]
        r = J.judge_firing_history(entries)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_outcome_field_fails(self):
        entries = [{"profile_name": "BENCH_HP", "start_time": "t"}]
        r = J.judge_firing_history(entries)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_real_field_names_missing_duration_fails(self):
        entries = [{"profile_name": "BENCH_HP", "run_started_unix_s": 1234567890}]
        r = J.judge_firing_history(entries)
        self.assertEqual(r.verdict, Verdict.FAIL)


class LinkStatsDeltaTest(unittest.TestCase):
    def test_zero_deltas_pass(self):
        before = {"crc_errors": 3, "timeouts": 5, "broadcast_dropped": 0}
        after = {"crc_errors": 3, "timeouts": 5, "broadcast_dropped": 0}
        r = J.judge_link_stats_delta(before, after)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_crc_error_increase_fails(self):
        before = {"crc_errors": 3, "timeouts": 5, "broadcast_dropped": 0}
        after = {"crc_errors": 4, "timeouts": 5, "broadcast_dropped": 0}
        r = J.judge_link_stats_delta(before, after)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("crc_errors", r.reason)

    def test_all_fields_missing_is_inconclusive(self):
        r = J.judge_link_stats_delta({}, {})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


if __name__ == "__main__":
    unittest.main()
