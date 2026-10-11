#!/usr/bin/env python3
"""bench_test_start / ota_matrix_start / bench_test_job_status: background
runs on the mcpkit.build_jobs registry. Fakes only -- no board, no UART, no
HTTP. The point of these tests is that the new start path passes every gate
argument (confirm, allow_heat, ota_allow_heat, ...) through to the real tool
unchanged and never bypasses the board lock or the run-level preflight.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_start_jobs.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_bench_test as BT  # noqa: E402
from kilnctrl import mcp_server_ota as _ota_tool  # noqa: E402
from kilnctrl import mcp_server_ota_matrix as M  # noqa: E402
from kilnctrl.bench_test import board_lock as bt_board_lock  # noqa: E402
from mcpkit import build_jobs  # noqa: E402


class _Outcome:
    def __init__(self, exit_code=0):
        self.run_id = "fake-run"
        self.exit_code = exit_code
        self.preflight_ok = True
        self.preflight_reason = None
        self.requested = []
        self.results = {}
        self.run_dir = "logs/bench_test/fake-run"
        self.tainted = False


class _FakeRunner:
    """Records constructor ctx and run() kwargs; optionally blocks / raises."""
    instances: list = []
    release: "threading.Event | None" = None
    exit_code = 0
    raise_exc: "Exception | None" = None

    def __init__(self, ctx, logs_root=None):
        self.ctx = ctx
        self.run_kwargs = None
        _FakeRunner.instances.append(self)

    def run(self, suite, cases=None, dry_run=False, allow_heat=True, tag=None,
            lcd_stop_heat=False, lcd_edit_heat=False, ota_allow_heat=False):
        self.run_kwargs = dict(suite=suite, cases=cases, dry_run=dry_run, allow_heat=allow_heat,
                               tag=tag, lcd_stop_heat=lcd_stop_heat, lcd_edit_heat=lcd_edit_heat,
                               ota_allow_heat=ota_allow_heat)
        if _FakeRunner.release is not None:
            _FakeRunner.release.wait(5)
        if _FakeRunner.raise_exc is not None:
            raise _FakeRunner.raise_exc
        return _Outcome(_FakeRunner.exit_code)

    @classmethod
    def reset(cls):
        cls.instances = []
        cls.release = None
        cls.exit_code = 0
        cls.raise_exc = None


class _Base(unittest.TestCase):
    def setUp(self):
        _FakeRunner.reset()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        build_jobs._reset_for_tests()
        for p in (
            mock.patch.object(build_jobs, "_job_dir", lambda: self.tmp.name),
            mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                              side_effect=lambda h: (h or "10.0.0.5", "explicit" if h else "STA IP")),
            mock.patch.object(BT, "BenchTestRunner", _FakeRunner),
            mock.patch.object(M, "BenchTestRunner", _FakeRunner),
            mock.patch.object(M, "_run_level_preflight", return_value=None),
        ):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(build_jobs._reset_for_tests)

    @staticmethod
    def job_id(started: str) -> str:
        assert started.startswith("bench-job "), started
        return started.split()[1].rstrip(":")

    def finish(self, started: str, wait_s: float = 5) -> str:
        return BT.bench_test_job_status(self.job_id(started), wait_s=wait_s)


class ClassifyTest(unittest.TestCase):
    def test_exit_codes(self):
        c = BT.classify_bench_report
        self.assertEqual(c("bench_test_run: suite=smoke run_id=r exit_code=0"), "ok")
        self.assertEqual(c("ota_matrix_run: suite=ota run_id=r exit_code=3\n  X: SKIP"), "incomplete")
        self.assertEqual(c("bench_test_run: suite=smoke run_id=r exit_code=1"), "failed")
        self.assertEqual(c("bench_test_run: suite=smoke run_id=r exit_code=2"), "failed")
        self.assertEqual(c("error: refused -- board lock held"), "failed")
        self.assertEqual(c(""), "failed")


class BenchTestStartTest(_Base):
    def test_returns_at_once_then_running_then_ok(self):
        _FakeRunner.release = threading.Event()
        t0 = time.time()
        started = BT.bench_test_start(suite="smoke")
        self.assertLess(time.time() - t0, 2.0)
        self.assertIn("STARTED", started)
        running = BT.bench_test_job_status(self.job_id(started))
        self.assertIn("RUNNING", running)
        self.assertIn("bench_test_job_status", running)
        _FakeRunner.release.set()
        done = self.finish(started)
        self.assertIn("OK after", done)
        self.assertIn("exit_code=0", done)
        self.assertIn("host: 10.0.0.5 (STA IP)", done)

    def test_failed_and_incomplete_states(self):
        _FakeRunner.exit_code = 1
        self.assertIn("FAILED after", self.finish(BT.bench_test_start(suite="smoke")))
        _FakeRunner.exit_code = 3
        self.assertIn("INCOMPLETE after", self.finish(BT.bench_test_start(suite="smoke")))

    def test_every_gate_argument_reaches_the_runner_unchanged(self):
        started = BT.bench_test_start(
            suite="lcd", cases="LCD-19,LCD-22", dry_run=False, allow_heat=False,
            lcd_stop_heat=True, lcd_edit_heat=True, ota_allow_heat=True, tag="t1",
            host="10.9.9.9", attended=True, allow_flash=True)
        self.finish(started)
        runner = _FakeRunner.instances[-1]
        self.assertEqual(runner.run_kwargs, dict(
            suite="lcd", cases=["LCD-19", "LCD-22"], dry_run=False, allow_heat=False, tag="t1",
            lcd_stop_heat=True, lcd_edit_heat=True, ota_allow_heat=True))
        self.assertEqual(runner.ctx["host"], "10.9.9.9")
        self.assertIs(runner.ctx["attended"], True)
        self.assertIs(runner.ctx["allow_flash"], True)

    def test_ota_image_arguments_reach_the_runner_ctx(self):
        # bench1 2026-10-10: OT-G06 could not run via bench_test_start because
        # ota_image_path was not accepted.
        kw = dict(ota_image_path="C:/img/KilnCtrl.bin", ota_corrupt_image_path="C:/img/bad.bin",
                  ota_truncated_image_path="C:/img/trunc.bin", ota_wrong_build_image_path="C:/img/wb.bin",
                  ota_image_build="2026-10-10", ota_pico_image_path="C:/img/p.bin",
                  ota_pico_image_commit="abc1234", ota_pico_corrupt_image_path="C:/img/pb.bin",
                  update_downgrade_repo="o/old", update_wrong_repo="o/wrong")
        self.finish(BT.bench_test_start(suite="ota", cases="OT-G06", confirm=True, **kw))
        ctx = _FakeRunner.instances[-1].ctx
        for k, v in kw.items():
            self.assertEqual(ctx[k], v, k)
        # and the synchronous twin
        BT.bench_test_run(suite="ota", cases="OT-G06", confirm=True, **kw)
        ctx2 = _FakeRunner.instances[-1].ctx
        for k, v in kw.items():
            self.assertEqual(ctx2[k], v, k)

    def test_ota_images_without_confirm_refuse_before_runner(self):
        out = BT.bench_test_run(suite="ota", ota_image_path="C:/img/a.bin")
        self.assertIn("refused", out)
        out = BT.bench_test_run(suite="ota", ota_image_path="C:/img/a.bin", confirm="yes")
        self.assertIn("refused", out)
        out = BT.bench_test_start(suite="ota", update_wrong_repo="o/w")
        self.assertIn("refused", out)
        self.assertEqual(_FakeRunner.instances, [])

    def test_start_unconfirmed_ota_refused_synchronously_and_creates_no_job(self):
        # toolfx8 T4: the refusal comes back at once, not as a FAILED job report.
        for kw in ({"ota_image_path": "C:/img/a.bin"}, {"update_wrong_repo": "o/w"},
                   {"ota_pico_image_commit": "abc"}):
            for bad in (False, "yes", 1, None):
                with mock.patch.object(build_jobs, "start_job") as sj:
                    out = BT.bench_test_start(suite="ota", confirm=bad, **kw)  # type: ignore[arg-type]
                self.assertTrue(out.startswith("error: refused"), out)
                self.assertNotIn("STARTED", out)
                sj.assert_not_called()
        self.assertEqual(_FakeRunner.instances, [])

    def test_ota_images_preflight_refusal_blocks_runner(self):
        with mock.patch.object(M, "_run_level_preflight", return_value="safety is ARMED"):
            out = BT.bench_test_run(suite="ota", ota_image_path="C:/img/a.bin", confirm=True)
        self.assertIn("run-level precondition failed: safety is ARMED", out)
        self.assertEqual(_FakeRunner.instances, [])

    def test_omitted_ota_arguments_stay_out_of_ctx(self):
        self.finish(BT.bench_test_start(suite="smoke"))
        ctx = _FakeRunner.instances[-1].ctx
        self.assertNotIn("ota_image_path", ctx)

    def test_defaults_keep_opt_ins_off(self):
        self.finish(BT.bench_test_start(suite="heat"))
        kw = _FakeRunner.instances[-1].run_kwargs
        self.assertIs(kw["allow_heat"], True)  # bench_test_run's own default
        self.assertIs(kw["lcd_stop_heat"], False)
        self.assertIs(kw["lcd_edit_heat"], False)
        self.assertIs(kw["ota_allow_heat"], False)

    def test_board_lock_refusal_comes_back_as_failed_report(self):
        _FakeRunner.raise_exc = bt_board_lock.BoardLockHeld("held by pid 123")
        done = self.finish(BT.bench_test_start(suite="heat"))
        self.assertIn("FAILED after", done)
        self.assertIn("error: refused -- held by pid 123", done)

    def test_unresolvable_host_is_a_failed_job_and_runner_never_built(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source", return_value=("", "default")):
            done = self.finish(BT.bench_test_start(suite="smoke"))
        self.assertIn("FAILED after", done)
        self.assertIn("could not resolve a board host", done)
        self.assertEqual(_FakeRunner.instances, [])

    def test_unknown_suite_error_is_reported(self):
        _FakeRunner.raise_exc = KeyError("nope")
        done = self.finish(BT.bench_test_start(suite="nope"))
        self.assertIn("FAILED after", done)
        self.assertIn("error:", done)

    def test_running_status_tails_runner_log(self):
        root = os.path.join(self.tmp.name, "logs")
        run_dir = os.path.join(root, "20261005T000000Z_smoke")
        os.makedirs(run_dir)
        with open(os.path.join(run_dir, "runner.log"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(f"line {i}" for i in range(30)) + "\npassword=hunter2\n")
        _FakeRunner.release = threading.Event()
        with mock.patch.object(BT.bt_report, "default_logs_root", return_value=root):
            started = BT.bench_test_start(suite="smoke")
            running = BT.bench_test_job_status(self.job_id(started))
            _FakeRunner.release.set()
            self.finish(started)
        self.assertIn("progress:", running)
        self.assertIn("line 29", running)
        self.assertNotIn("line 0\n", running)
        self.assertNotIn("hunter2", running)

    def test_progress_placeholder_when_no_run_dir_yet(self):
        text = BT._runner_log_progress(time.time(), "smoke", logs_root=os.path.join(self.tmp.name, "none"))
        self.assertIn("not readable", text)
        text = BT._runner_log_progress(time.time(), "smoke", logs_root=self.tmp.name)
        self.assertIn("no run directory yet", text)

    def test_job_status_rejects_bad_ids(self):
        self.assertIn("invalid job id", BT.bench_test_job_status("../../etc"))
        self.assertIn("unknown", BT.bench_test_job_status("deadbeef"))

    def test_result_survives_registry_eviction_via_file(self):
        started = BT.bench_test_start(suite="smoke")
        self.finish(started)
        build_jobs._reset_for_tests()
        again = BT.bench_test_job_status(self.job_id(started))
        self.assertIn("OK after", again)
        self.assertIn("bench-job", again)

    def test_wait_is_clamped(self):
        _FakeRunner.release = threading.Event()
        started = BT.bench_test_start(suite="smoke")
        t0 = time.time()
        BT.bench_test_job_status(self.job_id(started), wait_s=0.3)
        self.assertLess(time.time() - t0, 5.0)
        _FakeRunner.release.set()
        self.finish(started)


class OtaMatrixStartTest(_Base):
    def test_unconfirmed_is_refused_synchronously_and_starts_no_job(self):
        for bad in (False, "yes", 1, None):
            out = M.ota_matrix_start(confirm=bad)  # type: ignore[arg-type]
            self.assertTrue(out.startswith("error: refused"), out)
            self.assertNotIn("STARTED", out)
        self.assertEqual(_FakeRunner.instances, [])

    def test_dry_run_lists_cases_synchronously_with_no_board_access(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source") as resolver:
            out = M.ota_matrix_start(dry_run=True)
        self.assertIn("dry_run", out)
        self.assertNotIn("STARTED", out)
        resolver.assert_not_called()
        self.assertEqual(_FakeRunner.instances, [])

    def test_confirmed_run_passes_confirm_and_allow_heat_through(self):
        started = M.ota_matrix_start(confirm=True, allow_heat=True, cases="OT-B01", tag="x",
                                     ota_image_path="C:/img.bin", host="10.1.1.1")
        self.assertIn("STARTED (ota_matrix)", started)
        done = self.finish(started)
        self.assertIn("OK after", done)
        runner = _FakeRunner.instances[-1]
        self.assertEqual(runner.run_kwargs["suite"], "ota")
        self.assertEqual(runner.run_kwargs["cases"], ["OT-B01"])
        self.assertIs(runner.run_kwargs["allow_heat"], True)
        self.assertIs(runner.run_kwargs["ota_allow_heat"], True)
        self.assertEqual(runner.ctx["ota_image_path"], "C:/img.bin")
        self.assertEqual(runner.ctx["host"], "10.1.1.1")

    def test_allow_heat_default_stays_off(self):
        self.finish(M.ota_matrix_start(confirm=True))
        kw = _FakeRunner.instances[-1].run_kwargs
        self.assertIs(kw["allow_heat"], False)
        self.assertIs(kw["ota_allow_heat"], False)

    def test_run_level_preflight_refusal_still_applies(self):
        with mock.patch.object(M, "_run_level_preflight", return_value="safety link down"):
            done = self.finish(M.ota_matrix_start(confirm=True))
        self.assertIn("FAILED after", done)
        self.assertIn("run-level precondition failed: safety link down", done)
        self.assertEqual(_FakeRunner.instances, [])

    def test_board_lock_refusal_still_applies(self):
        _FakeRunner.raise_exc = bt_board_lock.BoardLockHeld("held by pid 7")
        done = self.finish(M.ota_matrix_start(confirm=True))
        self.assertIn("FAILED after", done)
        self.assertIn("refused -- held by pid 7", done)


class RegistrationTest(unittest.TestCase):
    def test_tools_registered_with_facade_keywords(self):
        from kilnctrl import mcp_facade
        for name in ("bench_test_start", "bench_test_job_status", "ota_matrix_start"):
            self.assertIn(name, mcp_facade.KEYWORDS if hasattr(mcp_facade, "KEYWORDS") else
                          {n: 1 for n in _facade_names(mcp_facade)})


def _facade_names(mod):
    for v in vars(mod).values():
        if isinstance(v, dict) and "bench_test_run" in v:
            return list(v)
    return []


if __name__ == "__main__":
    unittest.main()
