#!/usr/bin/env python3
"""Unit tests for kilnctrl.mcp_server_ota_matrix -- the ota_matrix_run tool
(ROADMAP.md M8's "scripted run_pctools_tests-style regression" for suite
`ota`). Every board/HTTP call is faked via BenchTestRunner's own injectable
ctx seam (the same one test_bench_test_runner.py/test_bench_test_cases_ota.py
use); this has never been run against real hardware.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_ota_matrix.py -q
"""
from __future__ import annotations

import dataclasses
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota_matrix as M  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402


class _FakeExecStatus:
    def __init__(self, state_name="idle"):
        self.state_name = state_name


class _FakeAutotuneStatus:
    def __init__(self, state_name="idle"):
        self.state_name = state_name


class _FakeSafetyStatus:
    def __init__(self, link_up=True):
        self.link_up = link_up


class _FakeSafetyDiag:
    def __init__(self, ever_received=True, trip_reason=0):
        self.ever_received = ever_received
        self.trip_reason = trip_reason


class _FakeProfilesClient:
    def __init__(self, outer):
        self._outer = outer

    def get_exec_status(self):
        return self._outer.exec_status


class _FakeAutotuneClient:
    def __init__(self, outer):
        self._outer = outer

    def get_status(self):
        return self._outer.autotune_status


class _FakeSafetyClient:
    def __init__(self, outer):
        self._outer = outer

    def get_status(self):
        return self._outer.safety_status

    def get_diag(self):
        return self._outer.safety_diag


class _FakeCapabilityPreflightBoard:
    def __init__(self, crash_unacknowledged=False, crash_summary=None,
                 readiness_blocked=(), reachable=True, error=""):
        self.crash_unacknowledged = crash_unacknowledged
        self.crash_summary = crash_summary
        self.readiness_blocked = readiness_blocked
        self.reachable = reachable
        self.error = error


class _FakeCapabilityPreflightReport:
    """Stands in for capability_preflight.PreflightReport -- just the
    fields BenchTestRunner.preflight() reads off it."""

    def __init__(self, ok=True, **board_kwargs):
        self.ok = ok
        self.board = _FakeCapabilityPreflightBoard(**board_kwargs)

    def describe(self):
        return f"ok={self.ok}"


class _FakeSrv:
    """Stands in for kilnctrl.mcp_server: every function
    BenchTestRunner.preflight()/teardown() calls, all returning a healthy
    board by default."""

    def __init__(self):
        self.stale = ""
        self.exec_status = _FakeExecStatus("idle")
        self.autotune_status = _FakeAutotuneStatus("idle")
        self.safety_status = _FakeSafetyStatus(link_up=True)
        self.safety_diag = _FakeSafetyDiag(ever_received=True, trip_reason=0)
        self.heap_status = "heap ok, no crash pending"
        self._profiles = _FakeProfilesClient(self)
        self._autotune = _FakeAutotuneClient(self)
        self._safety = _FakeSafetyClient(self)

    def _stale_banner(self):
        return self.stale

    def get_heap_status(self, host=None):
        return self.heap_status

    def profiles_stop(self):
        return "stopped"

    def autotune_abort(self):
        return "aborted"


class OtaMatrixRunConfirmGateTest(unittest.TestCase):
    """The tool wrapper's own gates -- neither ever touches a board."""

    def test_refuses_without_confirm(self):
        result = M.ota_matrix_run(confirm=False)
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("confirm=True", result)

    def test_dry_run_wins_even_without_confirm(self):
        # dry_run=True must list cases regardless of confirm -- the case
        # list is meant to be visible with neither flag set.
        result = M.ota_matrix_run(confirm=False, dry_run=True)
        self.assertNotIn("error:", result)
        self.assertIn("no board access", result)

    def test_dry_run_lists_suite_ota_cases(self):
        result = M.ota_matrix_run(dry_run=True)
        ota_ids = R.suite_case_ids("ota")
        self.assertTrue(ota_ids)
        for cid in ota_ids:
            self.assertIn(cid, result)
        self.assertIn("Preconditions checked", result)

    def test_dry_run_narrows_to_requested_cases(self):
        result = M.ota_matrix_run(dry_run=True, cases="OT-E01,OT-E02")
        self.assertIn("OT-E01", result)
        self.assertIn("OT-E02", result)
        self.assertNotIn("OT-P01", result)

    def test_dry_run_rejects_case_outside_suite(self):
        result = M.ota_matrix_run(dry_run=True, cases="ST-05")
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("ST-05", result)


class RunOtaMatrixTest(unittest.TestCase):
    """`_run_ota_matrix` -- the board-touching path, exercised only through
    BenchTestRunner's fake-ctx seam (never a real board)."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="ota_matrix_test_")
        self.fake_srv = _FakeSrv()
        self.cp_report = _FakeCapabilityPreflightReport(ok=True)
        self.ctx = {
            "srv": self.fake_srv, "host": None, "ap_password": None,
            "capability_preflight_run": lambda preset, host, **kw: self.cp_report,
            "bench_test_log_doc_path": os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"),
        }
        self._saved_specs = {}

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)
        for cid, spec in self._saved_specs.items():
            R.REGISTRY[cid] = spec

    def _patch_judge(self, case_id, judge_fn):
        self._saved_specs.setdefault(case_id, R.REGISTRY[case_id])
        spec = R.REGISTRY[case_id]
        R.REGISTRY[case_id] = dataclasses.replace(spec, judge=judge_fn)

    def test_precondition_failure_refuses_the_whole_run(self):
        # profile running -- CLAUDE.md: never OTA during a firing.
        self.fake_srv.exec_status = _FakeExecStatus("running")
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_flash=False, logs_root=self.tmpdir)
        self.assertIn("PREFLIGHT FAILED", result)
        self.assertIn("OT-E12: NOT_RUN", result)

    def test_precondition_failure_on_unacknowledged_crash(self):
        self.cp_report = _FakeCapabilityPreflightReport(
            ok=False, crash_unacknowledged=True, crash_summary="synthetic panic")
        self.ctx["capability_preflight_run"] = lambda preset, host, **kw: self.cp_report
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_flash=False, logs_root=self.tmpdir)
        self.assertIn("PREFLIGHT FAILED", result)
        self.assertIn("crash", result.lower())

    def test_passing_case_reports_pass(self):
        self._patch_judge("OT-E12", lambda ctx: R.CaseResult(R.Verdict.PASS))
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_flash=False, logs_root=self.tmpdir)
        self.assertIn("OT-E12: PASS", result)
        self.assertIn("exit_code=0", result)

    def test_failing_case_reports_fail_with_reason(self):
        self._patch_judge(
            "OT-E12", lambda ctx: R.CaseResult(R.Verdict.FAIL, reason="synthetic failure"))
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_flash=False, logs_root=self.tmpdir)
        self.assertIn("OT-E12: FAIL -- synthetic failure", result)
        self.assertIn("exit_code=1", result)

    def test_unknown_case_id_is_an_error(self):
        result = M._run_ota_matrix(self.ctx, cases="OT-NOPE", tag=None,
                                    allow_flash=False, logs_root=self.tmpdir)
        self.assertTrue(result.startswith("error:"), result)


if __name__ == "__main__":
    unittest.main()
