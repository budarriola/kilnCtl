#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.runner -- preflight/execute/teardown
lifecycle, with a fake `srv` module standing in for kilnctrl.mcp_server so
no real board or MCP server is touched.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_runner.py -q
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.runner import BenchTestRunner  # noqa: E402


class _FakeSrv:
    """Stands in for kilnctrl.mcp_server: every function the runner's
    preflight/teardown calls, all returning healthy-board text by default."""

    def __init__(self):
        self.stale = ""
        self.exec_status = "state: 0 (idle)"
        self.autotune_status = "status: idle"
        self.safety_status = "link_up: true, trip_reason: 0"
        self.heap_status = "heap ok, no crash pending"
        self.profiles_stop_called = False
        self.autotune_abort_called = False

    def _stale_banner(self):
        return self.stale

    def profiles_get_exec_status(self):
        return self.exec_status

    def autotune_get_status(self):
        return self.autotune_status

    def safety_get_status(self):
        return self.safety_status

    def get_heap_status(self, host=None):
        return self.heap_status

    def profiles_stop(self):
        self.profiles_stop_called = True
        return "stopped"

    def autotune_abort(self):
        self.autotune_abort_called = True
        return "aborted"


class RunnerLifecycleTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="bench_test_runner_test_")
        self.fake_srv = _FakeSrv()
        self.ctx = {"srv": self.fake_srv, "host": None}
        # Save/restore anything we monkeypatch onto the real REGISTRY so
        # this test never leaks state into other tests or the module.
        self._saved_specs = {}

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)
        for cid, spec in self._saved_specs.items():
            R.REGISTRY[cid] = spec

    def _patch_judge(self, case_id, judge_fn):
        self._saved_specs.setdefault(case_id, R.REGISTRY[case_id])
        spec = R.REGISTRY[case_id]
        import dataclasses
        R.REGISTRY[case_id] = dataclasses.replace(spec, judge=judge_fn)

    def test_healthy_board_preflight_passes(self):
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertTrue(ok, reason)

    def test_stale_server_fails_preflight(self):
        self.fake_srv.stale = "STALE: server running old code"
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)
        self.assertIn("stale", reason.lower())

    def test_running_profile_fails_preflight(self):
        self.fake_srv.exec_status = "state: 2 (RUNNING)"
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)

    def test_unacknowledged_crash_fails_preflight(self):
        self.fake_srv.heap_status = "UNACKNOWLEDGED CRASH REPORT: safety_poll panic"
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)

    def test_failed_preflight_marks_every_case_not_run(self):
        self.fake_srv.exec_status = "state: 2 (RUNNING)"
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke")
        self.assertFalse(outcome.preflight_ok)
        for result in outcome.results.values():
            self.assertEqual(result.verdict, R.Verdict.NOT_RUN)
        self.assertEqual(outcome.exit_code, 1)

    def test_all_passing_cases_exit_zero(self):
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.PASS))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertEqual(outcome.results["ST-05"].verdict, R.Verdict.PASS)
        self.assertEqual(outcome.exit_code, 0)

    def test_a_failing_case_exits_one(self):
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.FAIL, reason="synthetic failure"))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertEqual(outcome.exit_code, 1)

    def test_a_skip_with_no_failures_exits_three(self):
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.SKIP, reason="synthetic skip"))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertEqual(outcome.exit_code, 3)

    def test_dry_run_skips_every_case_without_calling_judge(self):
        calls = []
        self._patch_judge("ST-05", lambda ctx: calls.append(1) or R.CaseResult(R.Verdict.PASS))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"], dry_run=True)
        self.assertEqual(outcome.results["ST-05"].verdict, R.Verdict.SKIP)
        self.assertEqual(calls, [])

    def test_heat_case_skipped_when_allow_heat_false(self):
        heat_ids = [c for c in R.SUITES["heat"] if R.get_case(c).heat]
        self.assertTrue(heat_ids)
        cid = heat_ids[0]
        self._patch_judge(cid, lambda ctx: R.CaseResult(R.Verdict.PASS))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", cases=[cid], allow_heat=False)
        self.assertEqual(outcome.results[cid].verdict, R.Verdict.SKIP)

    def test_case_raising_becomes_a_fail_not_a_crash(self):
        def _boom(ctx):
            raise RuntimeError("synthetic case blowup")
        self._patch_judge("ST-05", _boom)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertEqual(outcome.results["ST-05"].verdict, R.Verdict.FAIL)
        self.assertIn("synthetic case blowup", outcome.results["ST-05"].reason)

    def test_not_implemented_case_reports_not_run(self):
        # SP-10 (CT / S9 / S14 / S15) has judge=None in wave 0.
        self.assertIsNone(R.get_case("SP-10").judge)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="safety", cases=["SP-10"])
        self.assertEqual(outcome.results["SP-10"].verdict, R.Verdict.NOT_RUN)
        self.assertEqual(outcome.results["SP-10"].reason, "not_implemented")

    def test_run_writes_a_summary_json_to_the_logs_root(self):
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.PASS))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        run_dir = os.path.join(self.tmpdir, outcome.run_id)
        self.assertTrue(os.path.isfile(os.path.join(run_dir, "summary.json")))
        self.assertTrue(os.path.isfile(os.path.join(run_dir, "transcript.md")))

    def test_teardown_stops_a_stuck_profile_run(self):
        self.fake_srv.exec_status = "state: 2 (RUNNING)"
        # Bypass preflight (which would refuse) to exercise teardown alone.
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        runner.teardown()
        self.assertTrue(self.fake_srv.profiles_stop_called)


if __name__ == "__main__":
    unittest.main()
