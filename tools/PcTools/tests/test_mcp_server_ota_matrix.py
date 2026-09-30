#!/usr/bin/env python3
"""Unit tests for kilnctrl.mcp_server_ota_matrix -- the ota_matrix_run tool
(ROADMAP.md M8's "scripted run_pctools_tests-style regression" for suite
`ota`). Every board/HTTP call is faked via injected ctx callables
(`gpio_test_preflight_fn`, `capability_preflight_run`, `srv`) -- the same
seam test_bench_test_runner.py/test_bench_test_cases_ota.py use; this has
never been run against real hardware.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_ota_matrix.py -q
"""
from __future__ import annotations

import dataclasses
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota_matrix as M  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.coordinated_gpio_test import GpioTestPreflight  # noqa: E402


def _ok_gpio_preflight(host=None):
    """A GpioTestPreflight with every precondition satisfied -- refusal_reasons() == []."""
    return GpioTestPreflight(
        safety_armed=False, profile_running_or_paused=False, profile_state_name="idle",
        ota_interlock_ok=True, ota_interlock_reason="ok", link_up=True,
    )


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
    fields _run_level_preflight()/BenchTestRunner.preflight() read off it."""

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
    """The tool wrapper's own gates -- must never reach the board-touching
    path. `_run_ota_matrix`/`BenchTestRunner` are patched to sentinels that
    raise, so a broken gate fails the test loudly instead of silently
    falling through to a real board."""

    def setUp(self):
        self._patches = [
            mock.patch.object(
                M, "_run_ota_matrix",
                side_effect=AssertionError("_run_ota_matrix must not be called")),
            mock.patch.object(
                M, "BenchTestRunner",
                side_effect=AssertionError("BenchTestRunner must not be constructed")),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)

    def test_refuses_without_confirm(self):
        result = M.ota_matrix_run(confirm=False)
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("confirm=True", result)

    def test_refuses_truthy_non_bool_confirm(self):
        # `if not confirm` would accept "y"/2/"maybe"/[1] -- confirm must be
        # exactly True, never merely truthy (mcp_server_actions.py:97-133's
        # bug class).
        for bad in ("y", "true", 1, "maybe", [1]):
            with self.subTest(confirm=bad):
                result = M.ota_matrix_run(confirm=bad)
                self.assertTrue(result.startswith("error:"), result)
                self.assertIn("confirm=True", result)

    def test_dry_run_wins_even_without_confirm(self):
        result = M.ota_matrix_run(confirm=False, dry_run=True)
        self.assertNotIn("error:", result)
        self.assertIn("no board access", result)

    def test_dry_run_true_confirm_true_never_touches_board(self):
        # dry_run short-circuits before the confirm check even runs;
        # confirm=True + dry_run=True must still never reach _run_ota_matrix.
        result = M.ota_matrix_run(confirm=True, dry_run=True)
        self.assertNotIn("error:", result)
        self.assertIn("no board access", result)

    def test_dry_run_lists_suite_ota_cases(self):
        result = M.ota_matrix_run(dry_run=True)
        ota_ids = R.suite_case_ids("ota")
        self.assertTrue(ota_ids)
        for cid in ota_ids:
            self.assertIn(cid, result)
        self.assertIn("preconditions", result.lower())
        self.assertIn("OT-B01", result)
        self.assertIn("only OT-B01 actually executes", result)

    def test_dry_run_narrows_to_requested_cases(self):
        result = M.ota_matrix_run(dry_run=True, cases="OT-E01,OT-E02")
        self.assertIn("OT-E01", result)
        self.assertIn("OT-E02", result)
        self.assertNotIn("OT-P01", result)

    def test_dry_run_rejects_case_outside_suite(self):
        result = M.ota_matrix_run(dry_run=True, cases="FL-10")
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("FL-10", result)


class OtaMatrixRunConfirmedPassthroughTest(unittest.TestCase):
    """confirm=True must reach `_run_ota_matrix` with host/the image
    parameters threaded through, and allow_flash must no longer exist at all
    (suite ota has no case that reads it). 2026-09-29: every route this
    matrix drives is ROUTE_TIER_ADMIN only -- the AP-password HMAC these
    routes used to ALSO require was retired, so there is no `ap_password`
    parameter or ctx key any more."""

    def test_context_passthrough(self):
        captured = {}

        def fake_run(ctx, cases, tag, allow_heat, logs_root=None):
            captured["ctx"] = ctx
            captured["cases"] = cases
            captured["tag"] = tag
            captured["allow_heat"] = allow_heat
            return "ok"

        with mock.patch.object(M, "_run_ota_matrix", side_effect=fake_run):
            result = M.ota_matrix_run(
                confirm=True, host="10.0.0.5", tag="mytag",
                cases="OT-B01", allow_heat=True, ota_image_path="/tmp/x.bin",
                ota_pico_image_path="/tmp/y.bin", ota_pico_image_commit="abc123",
            )
        self.assertEqual(result, "ok")
        ctx = captured["ctx"]
        self.assertEqual(ctx["host"], "10.0.0.5")
        self.assertNotIn("ap_password", ctx)
        self.assertEqual(ctx["ota_image_path"], "/tmp/x.bin")
        self.assertEqual(ctx["ota_pico_image_path"], "/tmp/y.bin")
        self.assertEqual(ctx["ota_pico_image_commit"], "abc123")
        self.assertEqual(captured["cases"], "OT-B01")
        self.assertEqual(captured["tag"], "mytag")
        self.assertTrue(captured["allow_heat"])
        self.assertNotIn("allow_flash", ctx)

    def test_no_allow_flash_parameter(self):
        import inspect
        sig = inspect.signature(M.ota_matrix_run)
        self.assertNotIn("allow_flash", sig.parameters)

    def test_no_ap_password_parameter(self):
        import inspect
        sig = inspect.signature(M.ota_matrix_run)
        self.assertNotIn("ap_password", sig.parameters)

    def test_host_falls_back_to_resolver_when_omitted(self):
        captured = {}

        def fake_run(ctx, cases, tag, allow_heat, logs_root=None):
            captured["ctx"] = ctx
            return "ok"

        with mock.patch.object(M._ota_tool, "_ota_resolve_host_with_source",
                                return_value=("192.168.4.1", "default")):
            with mock.patch.object(M, "_run_ota_matrix", side_effect=fake_run):
                result = M.ota_matrix_run(confirm=True)
        self.assertEqual(captured["ctx"]["host"], "192.168.4.1")
        self.assertEqual(captured["ctx"]["host_source"], "default")
        self.assertNotIn("ap_password", captured["ctx"])
        self.assertEqual(result, "ok")


class RunLevelPreflightTest(unittest.TestCase):
    """`_run_level_preflight` -- the fail-closed gate checked before
    BenchTestRunner is even constructed."""

    def test_ok_gpio_and_capability_preflight_passes(self):
        ctx = {
            "gpio_test_preflight_fn": _ok_gpio_preflight,
            "resolve_host_fn": lambda host: host,
            "capability_preflight_run": lambda preset, host: _FakeCapabilityPreflightReport(ok=True),
        }
        self.assertIsNone(M._run_level_preflight(ctx, host=None))

    def test_armed_refuses(self):
        pf = dataclasses.replace(_ok_gpio_preflight(), safety_armed=True)
        ctx = {"gpio_test_preflight_fn": lambda host: pf, "resolve_host_fn": lambda host: host}
        reason = M._run_level_preflight(ctx, host=None)
        self.assertIsNotNone(reason)
        self.assertIn("ARMED", reason)
        # reworded for OTA -- must not carry GpioTestPreflight's own
        # GPIO-detach-test wording verbatim.
        self.assertNotIn("detach GPIO", reason)
        self.assertIn("OTA", reason)

    def test_host_resolved_once_and_shared_between_probes(self):
        # An explicit host, or the board's own STA IP, must reach BOTH
        # probes identically -- a LAN-only board with no explicit host must
        # not see capability_preflight silently fall back to the AP address
        # while the gpio preflight resolved the real STA IP.
        seen_gpio_host = []
        seen_cp_host = []

        def gpio_fn(host):
            seen_gpio_host.append(host)
            return _ok_gpio_preflight()

        def cp_run(preset, host):
            seen_cp_host.append(host)
            return _FakeCapabilityPreflightReport(ok=True)

        ctx = {
            "gpio_test_preflight_fn": gpio_fn,
            "resolve_host_fn": lambda host: "192.168.1.87",
            "capability_preflight_run": cp_run,
        }
        result = M._run_level_preflight(ctx, host=None)
        self.assertIsNone(result)
        self.assertEqual(seen_gpio_host, ["192.168.1.87"])
        self.assertEqual(seen_cp_host, ["192.168.1.87"])

    def test_paused_refuses(self):
        pf = dataclasses.replace(
            _ok_gpio_preflight(), profile_running_or_paused=True, profile_state_name="paused")
        ctx = {"gpio_test_preflight_fn": lambda host: pf, "resolve_host_fn": lambda host: host}
        reason = M._run_level_preflight(ctx, host=None)
        self.assertIsNotNone(reason)
        self.assertIn("paused", reason.lower())

    def test_unreadable_gpio_preflight_refuses(self):
        def raising(host):
            raise RuntimeError("link timeout")

        ctx = {"gpio_test_preflight_fn": raising, "resolve_host_fn": lambda host: host}
        reason = M._run_level_preflight(ctx, host=None)
        self.assertIsNotNone(reason)
        self.assertIn("link timeout", reason)

    def test_unreadable_capability_preflight_refuses(self):
        def raising(preset, host):
            raise RuntimeError("board unreachable")

        ctx = {
            "gpio_test_preflight_fn": _ok_gpio_preflight,
            "resolve_host_fn": lambda host: host,
            "capability_preflight_run": raising,
        }
        reason = M._run_level_preflight(ctx, host=None)
        self.assertIsNotNone(reason)
        self.assertIn("board unreachable", reason)

    def test_unacknowledged_crash_refuses(self):
        ctx = {
            "gpio_test_preflight_fn": _ok_gpio_preflight,
            "resolve_host_fn": lambda host: host,
            "capability_preflight_run": lambda preset, host: _FakeCapabilityPreflightReport(
                ok=False, crash_unacknowledged=True, crash_summary="synthetic panic"),
        }
        reason = M._run_level_preflight(ctx, host=None)
        self.assertIsNotNone(reason)
        self.assertIn("crash", reason.lower())


class RunOtaMatrixTest(unittest.TestCase):
    """`_run_ota_matrix` -- the board-touching path, exercised only through
    injected ctx fakes (never a real board)."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="ota_matrix_test_")
        self.fake_srv = _FakeSrv()
        self.cp_report = _FakeCapabilityPreflightReport(ok=True)
        self.ctx = {
            "srv": self.fake_srv, "host": None,
            "gpio_test_preflight_fn": _ok_gpio_preflight,
            "resolve_host_fn": lambda host: host,
            "capability_preflight_run": lambda preset, host: self.cp_report,
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

    def test_run_level_precondition_failure_refuses_before_runner(self):
        # ARMED -- the run-level gate must refuse before BenchTestRunner
        # ever runs, distinct from BenchTestRunner's own preflight.
        armed_pf = dataclasses.replace(_ok_gpio_preflight(), safety_armed=True)
        self.ctx["gpio_test_preflight_fn"] = lambda host: armed_pf
        result = M._run_ota_matrix(self.ctx, cases="OT-B01", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertTrue(result.startswith("error: refused"), result)
        self.assertIn("ARMED", result)

    def test_precondition_failure_on_unacknowledged_crash(self):
        self.cp_report = _FakeCapabilityPreflightReport(
            ok=False, crash_unacknowledged=True, crash_summary="synthetic panic")
        self.ctx["capability_preflight_run"] = lambda preset, host: self.cp_report
        result = M._run_ota_matrix(self.ctx, cases="OT-B01", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertTrue(result.startswith("error: refused"), result)
        self.assertIn("crash", result.lower())

    def test_passing_case_reports_pass(self):
        self._patch_judge("OT-E12", lambda ctx: R.CaseResult(R.Verdict.PASS))
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertIn("OT-E12: PASS", result)
        self.assertIn("exit_code=0", result)

    def test_failing_case_reports_fail_with_reason(self):
        self._patch_judge(
            "OT-E12", lambda ctx: R.CaseResult(R.Verdict.FAIL, reason="synthetic failure"))
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertIn("OT-E12: FAIL -- synthetic failure", result)
        self.assertIn("exit_code=1", result)

    def test_unknown_case_id_is_an_error(self):
        result = M._run_ota_matrix(self.ctx, cases="OT-NOPE", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertTrue(result.startswith("error:"), result)

    def test_out_of_suite_case_cannot_be_reached_via_cases(self):
        # FL-10 exists in a different suite -- a `cases` subset must not be
        # able to smuggle in a case outside suite `ota`.
        self.assertIn("FL-10", R.REGISTRY)
        result = M._run_ota_matrix(self.ctx, cases="FL-10", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("FL-10", result)

    def test_image_paths_reach_case_ctx(self):
        seen = {}

        def judge(ctx):
            seen["ota_image_path"] = ctx.get("ota_image_path")
            seen["ota_pico_image_commit"] = ctx.get("ota_pico_image_commit")
            return R.CaseResult(R.Verdict.PASS)

        self._patch_judge("OT-E12", judge)
        self.ctx["ota_image_path"] = "/tmp/kiln.bin"
        self.ctx["ota_pico_image_commit"] = "deadbeef"
        result = M._run_ota_matrix(self.ctx, cases="OT-E12", tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertIn("OT-E12: PASS", result)
        self.assertEqual(seen["ota_image_path"], "/tmp/kiln.bin")
        self.assertEqual(seen["ota_pico_image_commit"], "deadbeef")

    def test_allow_heat_false_skips_heat_cases(self):
        heat_ids = [cid for cid in R.suite_case_ids("ota") if R.get_case(cid).heat]
        if not heat_ids:
            self.skipTest("no heat-bearing case in suite ota to exercise")
        cid = heat_ids[0]
        self._patch_judge(cid, lambda ctx: R.CaseResult(R.Verdict.PASS))
        result = M._run_ota_matrix(self.ctx, cases=cid, tag=None,
                                    allow_heat=False, logs_root=self.tmpdir)
        self.assertIn(f"{cid}: SKIP", result)


if __name__ == "__main__":
    unittest.main()
