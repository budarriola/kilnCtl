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
    # GET /api/cfgfs never emits a pending flag; GET /api/cfgfs/format_pending
    # returns {"pending": bool, "reason": str} (cfg_fs_format_http.c).
    CFGFS = {"mounted": True, "status": "ok", "file_count": 7}

    def test_not_pending_passes(self):
        r = J.judge_cfgfs_state(self.CFGFS, {"pending": False, "reason": ""})
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_pending_is_inconclusive_not_fail(self):
        # FL-07 never FAILs (plan 7 owner decision 6) -- record-only.
        r = J.judge_cfgfs_state(self.CFGFS, {"pending": True, "reason": "wrong size"})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("True", r.reason)

    def test_cfgfs_body_alone_cannot_pass(self):
        r = J.judge_cfgfs_state(self.CFGFS)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_missing_field_is_inconclusive_not_fail(self):
        r = J.judge_cfgfs_state({}, {})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


class PicoSlotMetadataTest(unittest.TestCase):
    def test_good_commit_and_power_on_passes(self):
        r = J.judge_pico_slot_metadata("82548f2e", "power_on")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_bad_commit_fails(self):
        r = J.judge_pico_slot_metadata("not-a-hash", "power_on")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_watchdog_boot_alone_is_inconclusive(self):
        """OpenOCD's rp2040 SWD reset path reboots via the watchdog too, so a
        lone 'watchdog' reading is indistinguishable from an ordinary
        debug_program(peer="pico") reset -- observed directly in
        docs/audits/short_proof_run_completed_2026-09-09.md:33-36 (a
        deliberate SWD reset reported boot reason 'watchdog'). Not a bare
        PASS either: this is a real open question."""
        r = J.judge_pico_slot_metadata("82548f2e", "watchdog")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_watchdog_boot_corroborated_fails(self):
        r = J.judge_pico_slot_metadata("82548f2e", "watchdog", boot_loop_corroborated=True)
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

    def test_real_route_field_names_pass(self):
        """GET /api/saftyfw_stack_margin actually reports
        stack_total_words/high_water_words, not configured/free -- sample
        shape from logs/bench_test/20260921T004457Z_smoke/pico_stack_margin/
        stack_margin_pico_unknown_20260921T004615Z.json, a healthy board."""
        tasks = [
            {"name": "relay_owner", "task_id": 0, "measured": True, "stack_total_words": 256, "high_water_words": 214},
            {"name": "safety_core", "task_id": 1, "measured": True, "stack_total_words": 1536, "high_water_words": 1114},
            {"name": "link_task", "task_id": 5, "measured": True, "stack_total_words": 2560, "high_water_words": 1478},
        ]
        result = J.judge_pico_stack_margins(tasks)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_not_measured_fails(self):
        tasks = [{"name": "relay_owner", "measured": False, "stack_total_words": 256, "high_water_words": 214}]
        result = J.judge_pico_stack_margins(tasks)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not measured", result.observed["failing"][0]["reason"])


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

    def test_boundary_at_floor_passes(self):
        """8704 is firmware's own KILN_DRAM_LARGEST_ALARM_BYTES
        (dram_margin.h) -- the floor triggers strictly below it, not at it.
        The measured 9216 B baseline still sits comfortably above it."""
        r = J.judge_heap_dram_floor(8704, 8704, False)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_boundary_one_below_floor_fails(self):
        r = J.judge_heap_dram_floor(8703, 8703, False)
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

    def test_params_list_shape_passes(self):
        """GET /api/safety/commissioning carries abs_max_temp_c inside
        params:[{name,set,value}], not as a top-level field -- sample shape
        from the 20260921T004457Z_smoke run's SP-01 observed payload
        (safety_cfg_http_client.get_commissioning)."""
        commissioning = {
            "commissioned": True,
            "stale": False,
            "params": [
                {"id": 259, "name": "tc_placement_mode", "set": True, "type": "u8", "value": 0},
                {"id": 260, "name": "abs_max_temp_c", "set": True, "type": "f32", "value": 80},
            ],
        }
        r = J.judge_commissioning_readback(commissioning, 80.0)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)

    def test_params_list_unset_ceiling_fails_not_zero(self):
        """An unset param carries no `value` key at all -- this must FAIL
        as "no ceiling", never silently read as abs_max_temp_c=0."""
        commissioning = {
            "commissioned": True,
            "stale": False,
            "params": [{"id": 260, "name": "abs_max_temp_c", "set": False, "type": "f32"}],
        }
        r = J.judge_commissioning_readback(commissioning, None)
        self.assertEqual(r.verdict, Verdict.FAIL)


class StatusDiagConsistencyTest(unittest.TestCase):
    def test_healthy_passes(self):
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_healthy_passes_with_matching_mask(self):
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 0, trip_mask=0)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_link_down_fails(self):
        r = J.judge_status_diag_consistency(False, "idle", "power_on", 0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_watchdog_boot_alone_is_inconclusive(self):
        """Same rationale as PicoSlotMetadataTest's version: an OpenOCD SWD
        reset alone produces 'watchdog', so this must not be a bare FAIL."""
        r = J.judge_status_diag_consistency(True, "idle", "watchdog", 0)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_watchdog_boot_corroborated_fails(self):
        r = J.judge_status_diag_consistency(True, "idle", "watchdog", 0, boot_loop_corroborated=True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_trip_reason_set_fails(self):
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 6)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unexpected_state_fails(self):
        r = J.judge_status_diag_consistency(True, "tripped", "power_on", 0)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_trip_reason_none_is_inconclusive_not_pass(self):
        """The SP-02 defect this guards against: a diag report the case
        cannot parse must never silently read as trip_reason=0 (healthy) --
        that would make this judge always PASS regardless of the board's
        real trip state. It must not be a bare FAIL either (no evidence of
        an actual trip), so it is INCONCLUSIVE, a third distinct outcome."""
        r = J.judge_status_diag_consistency(True, "idle", "power_on", None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_mask_inconsistent_with_reason_fails(self):
        """trip_reason=6 (S6a) implies trip_mask 1<<5 = 0x0020
        (link_frame_trip_mask_for_reason()); a diag reporting a different
        mask for the same reason is the exact status/diag inconsistency
        this case exists to catch, and must FAIL even though trip_reason
        alone is nonzero (so it would already FAIL on that ground; this
        confirms the mask check is not skipped/short-circuited for a
        nonzero reason)."""
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 6, trip_mask=0x0040)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_mask_matches_expected_for_zero_reason(self):
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 0, trip_mask=0x0000)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_mask_nonzero_with_zero_reason_fails(self):
        """trip_reason=0 (NONE) implies trip_mask=0; a nonzero mask reported
        alongside reason 0 is an inconsistency even though the reason field
        alone reads 'healthy'."""
        r = J.judge_status_diag_consistency(True, "idle", "power_on", 0, trip_mask=0x0020)
        self.assertEqual(r.verdict, Verdict.FAIL)


class ParseTripFieldsTest(unittest.TestCase):
    """Pins the parsing vocabulary against SafetyDiag.describe()'s real text
    shape (devices_safety.py), not an invented fixture shape -- the bug this
    whole fix addresses was a regex written against a form
    (`trip_reason=8`) no device or describe() implementation ever emits."""

    _REAL_DIAG_TEXT = (
        "link up | boot_reason power_on | state idle | "
        "trip_reason 6 [S6a mainFault] | warn_mask 0x0000 | "
        "trip_mask 0x0020 | uptime 12345 ms | last_seq 7"
    )

    def test_parses_real_diag_text_shape(self):
        self.assertEqual(J.parse_trip_reason(self._REAL_DIAG_TEXT), 6)
        self.assertEqual(J.parse_trip_mask(self._REAL_DIAG_TEXT), 0x0020)

    def test_does_not_match_invented_equals_form_field_absent(self):
        # The status text has neither field at all -- this must return
        # None, never a stale/wrong parse and never a silent 0.
        status_text_no_trip_fields = "link up | state idle | armed False"
        self.assertIsNone(J.parse_trip_reason(status_text_no_trip_fields))
        self.assertIsNone(J.parse_trip_mask(status_text_no_trip_fields))

    def test_parses_zero_trip_reason_and_mask(self):
        healthy_diag_text = "link up | trip_reason 0 [NONE] | trip_mask 0x0000"
        self.assertEqual(J.parse_trip_reason(healthy_diag_text), 0)
        self.assertEqual(J.parse_trip_mask(healthy_diag_text), 0)


class EstopVerifyTest(unittest.TestCase):
    def test_not_asserted_passes(self):
        text = "link up | 22.50 C (CJ 23.10 C) | currents 0.00 A, 0.00 A, 0.00 A | 120 ms old | tx_dropped 0"
        self.assertEqual(J.judge_estop_verify(text).verdict, Verdict.PASS)

    def test_asserted_fails(self):
        text = "link up; E-stop asserted | 22.50 C (CJ 23.10 C) | currents 0.00 A, 0.00 A, 0.00 A | 120 ms old | tx_dropped 0"
        self.assertEqual(J.judge_estop_verify(text).verdict, Verdict.FAIL)

    def test_none_fails(self):
        self.assertEqual(J.judge_estop_verify(None).verdict, Verdict.FAIL)

    def test_link_down_is_inconclusive_not_pass(self):
        """Opus review finding (HIGH): with the link down, describe() renders
        "no flags set" (the "E-stop asserted" bit is simply absent, same as
        every other flag -- not known-clear) and "never received" for the
        context age. Judging that as a PASS would be a false PASS with zero
        real data behind it."""
        text = "no flags set | 22.50 C (CJ 23.10 C) | currents 0.00 A, 0.00 A, 0.00 A | never received | tx_dropped unknown"
        self.assertEqual(J.judge_estop_verify(text).verdict, Verdict.INCONCLUSIVE)

    def test_serial_hub_unavailable_is_inconclusive_not_fail(self):
        """The serial hub is currently held by another process on this
        bench -- a smoke run against that state must not read as a board
        defect."""
        self.assertEqual(
            J.judge_estop_verify("error: no serial port open - connect first").verdict,
            Verdict.INCONCLUSIVE,
        )
        self.assertEqual(
            J.judge_estop_verify("error: hub did not respond to 'connect' within 10.0s").verdict,
            Verdict.INCONCLUSIVE,
        )

    def test_other_query_error_fails(self):
        self.assertEqual(J.judge_estop_verify("error: something unrelated broke").verdict, Verdict.FAIL)


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
    def test_16_links_with_group_expand_passes(self):
        r = J.judge_nav_menu("text", 16, has_group_expand=True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_wrong_link_count_fails(self):
        r = J.judge_nav_menu("text", 14, has_group_expand=True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("14", r.reason)

    def test_no_group_expand_fails(self):
        r = J.judge_nav_menu("text", 16, has_group_expand=False)
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


class OtaPowerLossMidWriteTest(unittest.TestCase):
    def test_unchanged_state_passes(self):
        r = J.judge_ota_power_loss_mid_write("B1", "B1", "app", "app", True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_fw_build_changed_fails(self):
        r = J.judge_ota_power_loss_mid_write("B1", "B2", "app", "app", True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("fw_build changed", r.reason)

    def test_wrong_running_partition_fails(self):
        r = J.judge_ota_power_loss_mid_write("B1", "B1", "recovery", "app", True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unreadable_fingerprint_is_inconclusive(self):
        r = J.judge_ota_power_loss_mid_write("B1", "B1", "app", "app", None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_fingerprint_changed_fails(self):
        r = J.judge_ota_power_loss_mid_write("B1", "B1", "app", "app", False)
        self.assertEqual(r.verdict, Verdict.FAIL)


class OtaUpdateRefusedDuringStateTest(unittest.TestCase):
    def test_refused_and_state_unchanged_passes(self):
        r = J.judge_ota_update_refused_during_state(False, True, "RUNNING", "RUNNING", "RUNNING")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_precondition_not_met_is_inconclusive(self):
        r = J.judge_ota_update_refused_during_state(False, True, "idle", "idle", "RUNNING")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_interlock_reports_ok_while_busy_fails(self):
        r = J.judge_ota_update_refused_during_state(True, True, "RUNNING", "RUNNING", "RUNNING")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_push_accepted_fails(self):
        r = J.judge_ota_update_refused_during_state(False, False, "RUNNING", "RUNNING", "RUNNING")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_state_disturbed_by_refused_push_fails(self):
        r = J.judge_ota_update_refused_during_state(False, True, "RUNNING", "PAUSED", "RUNNING")
        self.assertEqual(r.verdict, Verdict.FAIL)


class OtaPicoRefusedWithTripPendingTest(unittest.TestCase):
    """OT-P05's judge: trip_pending=None (unreadable) must read distinctly
    from trip_pending=False (readable, no trip) -- both are falsy in
    Python, and the case-level bug this covers used to run one code path
    for both. See cases_ota.py's _pico_trip_pending()."""

    def test_unreadable_trip_state_is_inconclusive_and_named(self):
        r = J.judge_ota_pico_refused_with_trip_pending(None, None, "aaa1111", "aaa1111")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("unreadable", r.reason)

    def test_no_trip_pending_is_inconclusive_and_named_differently(self):
        r = J.judge_ota_pico_refused_with_trip_pending(False, None, "aaa1111", "aaa1111")
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("no trip was pending", r.reason)
        self.assertNotIn("unreadable", r.reason)

    def test_refused_and_unchanged_passes(self):
        r = J.judge_ota_pico_refused_with_trip_pending(True, True, "aaa1111", "aaa1111")
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_accepted_while_pending_fails(self):
        r = J.judge_ota_pico_refused_with_trip_pending(True, False, "aaa1111", "aaa1111")
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_commit_changed_despite_refusal_fails(self):
        r = J.judge_ota_pico_refused_with_trip_pending(True, True, "aaa1111", "bbb2222")
        self.assertEqual(r.verdict, Verdict.FAIL)


class OtaSessionAuthTiersTest(unittest.TestCase):
    def test_admin_ok_user_refused_passes(self):
        r = J.judge_ota_session_auth_tiers(True, True)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_admin_refused_fails(self):
        r = J.judge_ota_session_auth_tiers(False, True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_user_accepted_fails(self):
        r = J.judge_ota_session_auth_tiers(True, False)
        self.assertEqual(r.verdict, Verdict.FAIL)


class JudgeOtaSelfPushUptimeElapsedTest(unittest.TestCase):
    def _judge(self, **kw):
        args = dict(refusal_form="http_409", status_code=409, elapsed_s=1.0, uptime_before=100.0,
                    uptime_after=101.0, crash_before={"present": False}, crash_after={"present": False},
                    interlock_ok_after=True)
        args.update(kw)
        return J.judge_ota_self_push_refused(**args)

    def test_push_time_alone_passes(self):
        self.assertEqual(self._judge().verdict, Verdict.PASS)

    def test_settle_wait_counts_toward_continuity(self):
        r = self._judge(uptime_elapsed_s=30.0)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("rebooted", r.reason)


if __name__ == "__main__":
    unittest.main()
