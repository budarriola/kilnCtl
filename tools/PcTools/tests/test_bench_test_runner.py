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


class _FakeCapabilityPreflightReport:
    """Stands in for capability_preflight.PreflightReport -- just the
    fields runner.preflight() reads off it."""

    def __init__(self, ok=True, crash_unacknowledged=False, crash_summary=None,
                 readiness_blocked=(), reachable=True, error=""):
        self.ok = ok
        self.board = _FakeCapabilityPreflightBoard(
            crash_unacknowledged, crash_summary, readiness_blocked, reachable, error)

    def describe(self):
        return f"ok={self.ok}"


class _FakeCapabilityPreflightBoard:
    def __init__(self, crash_unacknowledged, crash_summary, readiness_blocked, reachable, error):
        self.crash_unacknowledged = crash_unacknowledged
        self.crash_summary = crash_summary
        self.readiness_blocked = readiness_blocked
        self.reachable = reachable
        self.error = error


class _FakeSrv:
    """Stands in for kilnctrl.mcp_server: every function the runner's
    preflight/teardown calls, all returning a healthy board by default."""

    def __init__(self):
        self.stale = ""
        self.exec_status = _FakeExecStatus("idle")
        self.autotune_status = _FakeAutotuneStatus("idle")
        self.safety_status = _FakeSafetyStatus(link_up=True)
        self.safety_diag = _FakeSafetyDiag(ever_received=True, trip_reason=0)
        self.heap_status = "heap ok, no crash pending"
        self.profiles_stop_called = False
        self.autotune_abort_called = False
        self._profiles = _FakeProfilesClient(self)
        self._autotune = _FakeAutotuneClient(self)
        self._safety = _FakeSafetyClient(self)

    def _stale_banner(self):
        return self.stale

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
        self.cp_report = _FakeCapabilityPreflightReport(ok=True)
        self.ctx = {
            "srv": self.fake_srv, "host": None,
            "capability_preflight_run": lambda preset, host, **kw: self.cp_report,
            # Never let a unit test append to the real docs/BENCH_TEST_LOG.md.
            "bench_test_log_doc_path": os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"),
        }
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
        self.fake_srv.exec_status = _FakeExecStatus("running")
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)

    def test_unacknowledged_crash_fails_preflight(self):
        self.cp_report = _FakeCapabilityPreflightReport(
            ok=False, crash_unacknowledged=True, crash_summary="safety_poll panic")
        self.ctx["capability_preflight_run"] = lambda preset, host, **kw: self.cp_report
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)
        self.assertIn("crash", reason.lower())

    def test_readiness_blocked_fails_preflight(self):
        self.cp_report = _FakeCapabilityPreflightReport(
            ok=False, readiness_blocked=(("estop_verified", "E-stop", "not verified"),))
        self.ctx["capability_preflight_run"] = lambda preset, host, **kw: self.cp_report
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)
        self.assertIn("readiness", reason.lower())

    def test_failed_preflight_marks_every_case_not_run(self):
        self.fake_srv.exec_status = _FakeExecStatus("running")
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke")
        self.assertFalse(outcome.preflight_ok)
        for result in outcome.results.values():
            self.assertEqual(result.verdict, R.Verdict.NOT_RUN)
        # Wave 2's four-way contract: a refused preflight (no case even
        # attempted) is its own code, distinct from a case FAIL.
        self.assertEqual(outcome.exit_code, 2)

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

    def test_lcd_stop_heat_defaults_false_at_runner_and_mcp_layer(self):
        # 2026-09-30 review advisory: pin lcd_stop_heat's default at both
        # the layer that actually gates LCD-19's bench firing (run(), which
        # writes ctx["lcd19_allow_heat"]) and the MCP tool surface
        # (bench_test_run()) an ordinary caller goes through -- an
        # unnoticed default flip at either would silently make an ordinary
        # bench run start a real firing.
        import inspect
        from kilnctrl import mcp_server_bench_test as MT
        run_default = inspect.signature(BenchTestRunner.run).parameters["lcd_stop_heat"].default
        self.assertIs(run_default, False)
        tool_default = inspect.signature(MT.bench_test_run.__wrapped__ if hasattr(MT.bench_test_run, "__wrapped__") else MT.bench_test_run).parameters["lcd_stop_heat"].default
        self.assertIs(tool_default, False)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        runner.run(suite="smoke", dry_run=True)
        self.assertIs(runner.ctx.get("lcd19_allow_heat"), False)

    def test_lcd_edit_heat_defaults_false_at_runner_and_mcp_layer(self):
        # Same pin as lcd_stop_heat above, for LCD-22's opt-in.
        import inspect
        from kilnctrl import mcp_server_bench_test as MT
        run_default = inspect.signature(BenchTestRunner.run).parameters["lcd_edit_heat"].default
        self.assertIs(run_default, False)
        fn = MT.bench_test_run.__wrapped__ if hasattr(MT.bench_test_run, "__wrapped__") else MT.bench_test_run
        self.assertIs(inspect.signature(fn).parameters["lcd_edit_heat"].default, False)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        runner.run(suite="smoke", dry_run=True)
        self.assertIs(runner.ctx.get("lcd22_allow_heat"), False)
        runner2 = BenchTestRunner(dict(self.ctx), logs_root=self.tmpdir)
        runner2.run(suite="smoke", dry_run=True, lcd_edit_heat=True)
        self.assertIs(runner2.ctx.get("lcd22_allow_heat"), True)

    def test_heat_case_skipped_when_allow_heat_false(self):
        heat_ids = [c for c in R.SUITES["heat"] if R.get_case(c).heat]
        self.assertTrue(heat_ids)
        cid = heat_ids[0]
        self._patch_judge(cid, lambda ctx: R.CaseResult(R.Verdict.PASS))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", cases=[cid], allow_heat=False)
        self.assertEqual(outcome.results[cid].verdict, R.Verdict.SKIP)

    def test_ota_heat_cases_preskip_names_how_to_enable_and_ota_opt_in_defaults_false(self):
        import inspect
        from kilnctrl import mcp_server_bench_test as MT
        self.assertIs(inspect.signature(BenchTestRunner.run).parameters["ota_allow_heat"].default, False)
        fn = MT.bench_test_run.__wrapped__ if hasattr(MT.bench_test_run, "__wrapped__") else MT.bench_test_run
        self.assertIs(inspect.signature(fn).parameters["ota_allow_heat"].default, False)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="ota", cases=["OT-E07", "OT-E08"], allow_heat=False)
        for cid in ("OT-E07", "OT-E08"):
            self.assertEqual(outcome.results[cid].verdict, R.Verdict.SKIP)
            self.assertIn("allow_heat not set; OT-E07/E08/G04 start their own heat", outcome.results[cid].reason)
        self.assertIs(runner.ctx.get("ota_allow_heat"), False)

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

    def test_full_suite_cases_filter_crosses_suite_boundary_for_a_dependency(self):
        """LCD-19 (suite `lcd`) `depends_on` WEB-SEC-04 (suite `web`) -- the
        only catalogue that lists both is `full`. This proves
        `runner.run(suite="full", cases=["WEB-SEC-04", "LCD-19"])` is
        already a legal, exact-two-case selection today: `requested` is
        filtered to precisely those two ids, in dependency order, and never
        implicitly widens to include any other `full` member (in particular
        SP-05, the E-stop-verify case, which must never be pulled in by an
        automated cross-suite selection like this one -- see
        judge_estop_verify()'s and _case_sp05()'s own docstrings for why
        it's read-only today, and why running it unattended is still
        something a caller must ask for by name, not receive as a side
        effect)."""
        self.assertEqual(R.get_case("LCD-19").depends_on, "WEB-SEC-04")
        self.assertNotIn("WEB-SEC-04", R.SUITES["lcd"])
        self.assertNotIn("LCD-19", R.SUITES["web"])
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="full", cases=["WEB-SEC-04", "LCD-19"], dry_run=True)
        self.assertEqual(outcome.requested, ["WEB-SEC-04", "LCD-19"])
        self.assertNotIn("SP-05", outcome.requested)
        self.assertNotIn("SP-05", outcome.results)

    def test_full_suite_cases_filter_rejects_an_unrequested_id_by_name(self):
        """Sanity check on the negative side of the same mechanism: naming
        SP-05 explicitly still selects it (nothing refuses that on its
        own -- it's a legitimate `full`/`smoke`/`safety` member) but it is
        never *implicitly* added just because `full` is the suite in play."""
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="full", cases=["WEB-SEC-04", "LCD-19", "SP-05"], dry_run=True)
        self.assertIn("SP-05", outcome.requested)
        self.assertEqual(outcome.requested, ["SP-05", "WEB-SEC-04", "LCD-19"])

    def test_run_writes_a_summary_json_to_the_logs_root(self):
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.PASS))
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        run_dir = os.path.join(self.tmpdir, outcome.run_id)
        self.assertTrue(os.path.isfile(os.path.join(run_dir, "summary.json")))
        self.assertTrue(os.path.isfile(os.path.join(run_dir, "transcript.md")))

    def test_tainted_ctx_flag_reaches_outcome_summary_and_exit_code(self):
        import json

        def judge(ctx):
            ctx["_tainted"] = True
            return R.CaseResult(R.Verdict.PASS)
        self._patch_judge("ST-05", judge)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertTrue(outcome.tainted)
        self.assertEqual(outcome.exit_code, 1)
        run_dir = os.path.join(self.tmpdir, outcome.run_id)
        with open(os.path.join(run_dir, "summary.json"), encoding="utf-8") as f:
            self.assertIs(json.load(f)["tainted"], True)
        with open(os.path.join(run_dir, "transcript.md"), encoding="utf-8") as f:
            self.assertIn("TAINTED", f.read())

    def test_stale_taint_in_a_reused_ctx_is_cleared(self):
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.PASS))
        self.ctx["_tainted"] = True
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertFalse(outcome.tainted)
        self.assertEqual(outcome.exit_code, 0)

    def test_teardown_stops_a_stuck_profile_run(self):
        self.fake_srv.exec_status = _FakeExecStatus("running")
        # Bypass preflight (which would refuse) to exercise teardown alone.
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        runner.teardown()
        self.assertTrue(self.fake_srv.profiles_stop_called)

    def test_teardown_stops_a_paused_run(self):
        self.fake_srv.exec_status = _FakeExecStatus("paused")
        self.ctx["teardown_idle_poll_s"] = 0
        BenchTestRunner(self.ctx, logs_root=self.tmpdir).teardown()
        self.assertTrue(self.fake_srv.profiles_stop_called)

    def test_teardown_reports_executor_not_idle(self):
        self.fake_srv.exec_status = _FakeExecStatus("running")  # stop does not take effect
        self.ctx["teardown_idle_poll_s"] = 0
        after = BenchTestRunner(self.ctx, logs_root=self.tmpdir).teardown()
        self.assertIn("not confirmed idle", after["teardown_executor"])

    def test_teardown_hook_error_reported(self):
        def boom(ctx):
            raise RuntimeError("restore failed")
        self.ctx["teardown_hooks"] = [boom]
        after = BenchTestRunner(self.ctx, logs_root=self.tmpdir).teardown()
        self.assertIn("restore failed", after["teardown_hook_errors"][0])
        self.assertTrue(self.ctx["_tainted"])


class _FwSrv(_FakeSrv):
    """_FakeSrv plus the MCP get_fw_version tool, counting calls."""

    fw_text = ("uart_protocol_version: 13\ncompatible: yes\ncommit: abc1234def\n"
               "tree: clean\nbuilt: 2026-10-04")

    def __init__(self):
        super().__init__()
        self.fw_calls = 0

    def get_fw_version(self):
        self.fw_calls += 1
        return self.fw_text


class PreflightFirmwareVersionTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="bench_test_runner_fw_")
        self.srv = _FwSrv()
        self.ctx = {
            "srv": self.srv, "host": None,
            "capability_preflight_run": lambda preset, host, **kw: _FakeCapabilityPreflightReport(ok=True),
            "bench_test_log_doc_path": os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"),
        }

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_preflight_calls_get_fw_version_exactly_once_and_records_commit(self):
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertTrue(ok, reason)
        self.assertEqual(self.srv.fw_calls, 1)
        self.assertEqual(before["fw_commit"], "abc1234def")

    def test_summary_carries_the_firmware_commit(self):
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", dry_run=True)
        import json
        with open(os.path.join(outcome.run_dir, "summary.json"), encoding="utf-8") as f:
            summary = json.load(f)
        self.assertEqual(summary["board_before"]["fw_commit"], "abc1234def")
        self.assertEqual(self.srv.fw_calls, 1)

    def test_failed_fw_version_refuses_the_run(self):
        self.srv.fw_text = "error: no INFO reply"
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, before = runner.preflight()
        self.assertFalse(ok)
        self.assertIn("get_fw_version", reason)

    def test_incompatible_protocol_refuses_the_run(self):
        self.srv.fw_text = "uart_protocol_version: 1\ncompatible: NO - device speaks v1\ncommit: x"
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        ok, reason, _before = runner.preflight()
        self.assertFalse(ok)
        self.assertIn("protocol", reason)


class PreflightFirmwareSkipVisibleTest(unittest.TestCase):
    def test_srv_without_get_fw_version_records_visible_skip(self):
        tmp = tempfile.mkdtemp(prefix="bench_test_runner_skip_")
        try:
            ctx = {
                "srv": _FakeSrv(), "host": None,
                "capability_preflight_run": lambda preset, host, **kw: _FakeCapabilityPreflightReport(ok=True),
            }
            ok, reason, before = BenchTestRunner(ctx, logs_root=tmp).preflight()
            self.assertEqual(before["fw_version"], "skipped: srv has no get_fw_version")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_real_mcp_server_exposes_callable_get_fw_version(self):
        from kilnctrl import mcp_server
        self.assertTrue(callable(getattr(mcp_server, "get_fw_version", None)))


if __name__ == "__main__":
    unittest.main()
