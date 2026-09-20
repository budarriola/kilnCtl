#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.judgments -- pure judgment functions,
no I/O. Per feedback_negative_test_every_check, every judgment function
gets at least one test that feeds it a bad input and confirms it reports
something other than PASS (FAIL, INCONCLUSIVE, or NOT_RUN as appropriate).

Run with: python -m pytest tools/PcTools/tests/test_bench_test_judgments.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class TreeProvenanceTest(unittest.TestCase):
    def test_clean_tree_passes(self):
        r = J.judge_tree_provenance([], [])
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_ordinary_dirty_tree_passes(self):
        r = J.judge_tree_provenance(["docs/foo.md"], [])
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_sensitive_dirty_file_fails(self):
        r = J.judge_tree_provenance(["a.c"], ["zones_config_json.c"])
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("zones_config_json.c", r.reason)


class PartitionTableMatchTest(unittest.TestCase):
    def test_match_passes(self):
        r = J.judge_partition_table_match("partitions.csv vs chip: MATCH")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_mismatch_fails(self):
        r = J.judge_partition_table_match("partitions.csv vs chip: MISMATCH at row 2")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_error_text_fails(self):
        r = J.judge_partition_table_match("error: board unreachable")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unrecognized_text_fails(self):
        r = J.judge_partition_table_match("some garbage with neither word")
        self.assertEqual(r.verdict, Verdict.FAIL)


class RunningPartitionTest(unittest.TestCase):
    def test_app_passes(self):
        self.assertEqual(J.judge_running_partition("app").verdict, Verdict.PASS)

    def test_recovery_fails(self):
        r = J.judge_running_partition("recovery")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_none_fails(self):
        self.assertEqual(J.judge_running_partition(None).verdict, Verdict.FAIL)


class ArchivedElfMatchesTest(unittest.TestCase):
    def test_report_passes(self):
        self.assertEqual(J.judge_archived_elf_matches("archived ELF: KilnCtrl-abc123.elf").verdict, Verdict.PASS)

    def test_error_fails(self):
        self.assertEqual(J.judge_archived_elf_matches("error: no matching archived elf").verdict, Verdict.FAIL)


class BootGuardTest(unittest.TestCase):
    def test_healthy_passes(self):
        r = J.judge_boot_guard({"recovery_mode": False, "boot_count": 0})
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_recovery_mode_true_fails(self):
        r = J.judge_boot_guard({"recovery_mode": True, "boot_count": 0})
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_high_boot_count_fails(self):
        r = J.judge_boot_guard({"recovery_mode": False, "boot_count": 3})
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_boot_count_fails(self):
        r = J.judge_boot_guard({"recovery_mode": False})
        self.assertEqual(r.verdict, Verdict.FAIL)


class RecoveryImageSizedTest(unittest.TestCase):
    def test_no_row_fails(self):
        r = J.judge_recovery_image_sized(None, 1000)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_local_bin_is_not_run(self):
        r = J.judge_recovery_image_sized({"size": 1000}, None)
        self.assertEqual(r.verdict, Verdict.NOT_RUN)

    def test_oversized_fails(self):
        r = J.judge_recovery_image_sized({"size": 1000}, 2000)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_fits_passes(self):
        r = J.judge_recovery_image_sized({"size": 2000}, 1000)
        self.assertEqual(r.verdict, Verdict.PASS)


class CoredumpReadableTest(unittest.TestCase):
    def test_200_with_dict_passes(self):
        r = J.judge_coredump_readable(200, {"present": False})
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_non_200_fails(self):
        r = J.judge_coredump_readable(500, {"present": False})
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_non_dict_body_fails(self):
        r = J.judge_coredump_readable(200, "not json")
        self.assertEqual(r.verdict, Verdict.FAIL)


class CfgFsStateTest(unittest.TestCase):
    def test_not_pending_passes(self):
        r = J.judge_cfgfs_state({"format_pending": False, "mounted": False})
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_pending_is_inconclusive_not_fail(self):
        # FL-07 never FAILs (plan §7 owner decision 6) -- record-only.
        r = J.judge_cfgfs_state({"format_pending": True})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_missing_field_is_inconclusive_not_fail(self):
        r = J.judge_cfgfs_state({})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class PicoSlotMetadataTest(unittest.TestCase):
    def test_good_commit_and_power_on_passes(self):
        r = J.judge_pico_slot_metadata("82548f2e", "power_on")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_bad_commit_fails(self):
        r = J.judge_pico_slot_metadata("not-a-hash", "power_on")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_watchdog_boot_fails(self):
        r = J.judge_pico_slot_metadata("82548f2e", "watchdog")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unrecognized_boot_reason_is_inconclusive(self):
        r = J.judge_pico_slot_metadata("82548f2e", "something_else")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class StackMarginTest(unittest.TestCase):
    def test_report_passes(self):
        self.assertEqual(J.judge_stack_margin("task foo: 4096 free of 8192").verdict, Verdict.PASS)

    def test_error_fails(self):
        self.assertEqual(J.judge_stack_margin("error: board unreachable").verdict, Verdict.FAIL)


class PicoStackMarginsTest(unittest.TestCase):
    def test_all_above_threshold_passes(self):
        tasks = [{"name": "main", "configured": 4096, "free": 2048}]
        self.assertEqual(J.judge_pico_stack_margins(tasks).verdict, Verdict.PASS)

    def test_empty_list_fails(self):
        self.assertEqual(J.judge_pico_stack_margins([]).verdict, Verdict.FAIL)

    def test_below_quarter_fails(self):
        tasks = [{"name": "main", "configured": 4096, "free": 100}]
        self.assertEqual(J.judge_pico_stack_margins(tasks).verdict, Verdict.FAIL)

    def test_missing_fields_fails(self):
        tasks = [{"name": "main"}]
        self.assertEqual(J.judge_pico_stack_margins(tasks).verdict, Verdict.FAIL)


class HeapDramFloorTest(unittest.TestCase):
    def test_healthy_passes(self):
        r = J.judge_heap_dram_floor(20000, 19000, False)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_unacknowledged_crash_fails(self):
        r = J.judge_heap_dram_floor(20000, 19000, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_below_floor_fails(self):
        r = J.judge_heap_dram_floor(20000, 5000, False)
        self.assertEqual(r.verdict, Verdict.FAIL)


class CommissioningReadbackTest(unittest.TestCase):
    def test_matching_passes(self):
        r = J.judge_commissioning_readback({"commissioned": True, "stale": False, "abs_max_temp_c": 1300.0}, 1300.0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_not_commissioned_fails(self):
        r = J.judge_commissioning_readback({"commissioned": False}, 1300.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_stale_fails(self):
        r = J.judge_commissioning_readback({"commissioned": True, "stale": True, "abs_max_temp_c": 1300.0}, 1300.0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_mismatched_max_temp_fails(self):
        r = J.judge_commissioning_readback({"commissioned": True, "stale": False, "abs_max_temp_c": 1300.0}, 1250.0)
        self.assertEqual(r.verdict, Verdict.FAIL)


class StatusDiagConsistencyTest(unittest.TestCase):
    def test_healthy_passes(self):
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_link_down_fails(self):
        r = J.judge_status_diag_consistency(False, "idle", "power_on", 0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_watchdog_boot_fails(self):
        r = J.judge_status_diag_consistency(True, "idle", "watchdog", 0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_trip_reason_set_fails(self):
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 6)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unexpected_state_fails(self):
        r = J.judge_status_diag_consistency(True, "tripped", "power_on", 0)
        self.assertEqual(r.verdict, Verdict.FAIL)


class EstopVerifyTest(unittest.TestCase):
    def test_not_asserted_passes(self):
        self.assertEqual(J.judge_estop_verify(0x00).verdict, Verdict.PASS)

    def test_asserted_fails(self):
        self.assertEqual(J.judge_estop_verify(0x04).verdict, Verdict.FAIL)

    def test_none_fails(self):
        self.assertEqual(J.judge_estop_verify(None).verdict, Verdict.FAIL)


class WebRenderTest(unittest.TestCase):
    def test_landmark_and_nav_present_passes(self):
        html = '<html><body><button id="runBtn"></button><script defer src="/nav.js"></script></body></html>'
        r = J.judge_web_render(html, "runBtn", True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_missing_landmark_fails(self):
        html = '<html><body><script defer src="/nav.js"></script></body></html>'
        r = J.judge_web_render(html, "runBtn", True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("runBtn", r.reason)

    def test_missing_nav_fails_when_expected(self):
        html = '<html><body><button id="runBtn"></button></body></html>'
        r = J.judge_web_render(html, "runBtn", True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_login_page_does_not_require_nav(self):
        html = '<html><body><input id="username"></body></html>'
        r = J.judge_web_render(html, "username", False)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_fetch_error_fails(self):
        r = J.judge_web_render(None, "runBtn", True, error="GET / failed: connection refused")
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("connection refused", r.reason)

    def test_empty_body_fails(self):
        r = J.judge_web_render("", "runBtn", True)
        self.assertEqual(r.verdict, Verdict.FAIL)


class NavMenuTest(unittest.TestCase):
    def test_15_links_with_group_expand_passes(self):
        r = J.judge_nav_menu("text", 15, has_group_expand=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_link_count_fails(self):
        r = J.judge_nav_menu("text", 14, has_group_expand=True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("14", r.reason)

    def test_no_group_expand_fails(self):
        r = J.judge_nav_menu("text", 15, has_group_expand=False)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_fetch_failure_fails(self):
        r = J.judge_nav_menu(None, None)
        self.assertEqual(r.verdict, Verdict.FAIL)


class RouteTierSweepTest(unittest.TestCase):
    def test_all_matching_passes(self):
        results = [
            {"uri": "/api/status", "exercised": True, "ok": True},
            {"uri": "/diagnostics", "exercised": True, "ok": True},
            {"uri": "/api/zones", "exercised": False, "ok": True},
        ]
        r = J.judge_route_tier_sweep(results)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_a_violation_fails(self):
        results = [
            {"uri": "/api/status", "exercised": True, "ok": True},
            {"uri": "/diagnostics", "exercised": True, "ok": False},
        ]
        r = J.judge_route_tier_sweep(results)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("/diagnostics", str(r.observed))

    def test_nothing_exercised_is_inconclusive(self):
        results = [{"uri": "/api/foo", "exercised": False, "ok": True}]
        r = J.judge_route_tier_sweep(results)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class RateGuardConsistencyTest(unittest.TestCase):
    def test_matching_passes(self):
        r = J.judge_rate_guard_consistency({"max_rate_c_per_min": 10}, {"max_rate_c_per_min": 10})
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_missing_fields_is_inconclusive(self):
        r = J.judge_rate_guard_consistency({}, {"max_rate_c_per_min": 10})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_mismatched_values_fail(self):
        r = J.judge_rate_guard_consistency({"max_rate_c_per_min": 10}, {"max_rate_c_per_min": 12})
        self.assertEqual(r.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
