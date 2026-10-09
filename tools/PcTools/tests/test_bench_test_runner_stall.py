#!/usr/bin/env python3
"""ROADMAP B3 (2026-10-02): a heat run took logs/bench_test/.board_lock and
then stalled with no run directory. These tests pin the fix in
kilnctrl.bench_test.runner: runner.log exists as soon as the lock is held,
every wait between lock and first case is bounded and fails loud, and the
lock is released on every exit path. Fake board only; no hardware.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_runner_stall.py -q
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
import json
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from test_bench_test_runner import (  # noqa: E402
    _FakeCapabilityPreflightReport, _FakeSrv,
)
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test import report as report_mod  # noqa: E402
from kilnctrl.bench_test import runner as runner_mod  # noqa: E402
from kilnctrl.bench_test.runner import BenchTestRunner  # noqa: E402


class StalledPreflightTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="bench_test_stall_")
        self.release = threading.Event()
        self.srv = _FakeSrv()
        self.cp_report = _FakeCapabilityPreflightReport(ok=True)
        self.ctx = {
            "srv": self.srv, "host": None,
            "capability_preflight_run": lambda preset, host, **kw: self.cp_report,
            "bench_test_log_doc_path": os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"),
            "preflight_timeout_s": 0.4,
            "teardown_timeout_s": 0.4,
        }

    def tearDown(self):
        self.release.set()
        # Let abandoned workers finish so they do not make the next test's
        # preflight refuse.
        deadline = time.time() + 5
        while runner_mod._live_abandoned() and time.time() < deadline:
            time.sleep(0.01)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _lock_path(self):
        return os.path.join(self.tmpdir, ".board_lock")

    def _stall_heap(self, seen=None):
        def stalled(host=None):
            if seen is not None:
                seen["lock_held"] = os.path.exists(self._lock_path())
                dirs = [d for d in os.listdir(self.tmpdir)
                        if os.path.isdir(os.path.join(self.tmpdir, d))]
                seen["run_dirs"] = dirs
                if dirs:
                    seen["log"] = open(
                        os.path.join(self.tmpdir, dirs[0], "runner.log"), encoding="utf-8").read()
            self.release.wait(30)
            return "late"
        self.srv.get_heap_status = stalled

    def test_stalled_preflight_fails_loud_names_step_and_releases_lock(self):
        self._stall_heap()
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", dry_run=True)
        self.assertFalse(outcome.preflight_ok)
        self.assertIn("preflight stalled", outcome.preflight_reason)
        self.assertIn("get_heap_status", outcome.preflight_reason)
        self.assertFalse(os.path.exists(self._lock_path()), "board lock leaked after a stall")
        run_dir = os.path.join(self.tmpdir, outcome.run_id)
        self.assertTrue(os.path.isfile(os.path.join(run_dir, "summary.json")))
        log = open(os.path.join(run_dir, "runner.log"), encoding="utf-8").read()
        self.assertIn("preflight stalled", log)

    def test_run_dir_and_runner_log_exist_while_preflight_is_stalled(self):
        seen = {}
        self._stall_heap(seen)
        BenchTestRunner(self.ctx, logs_root=self.tmpdir).run(suite="heat", dry_run=True)
        self.assertTrue(seen["lock_held"])
        self.assertEqual(len(seen["run_dirs"]), 1)
        self.assertIn("board lock: held", seen["log"])
        self.assertIn("step: preflight: get_heap_status (HTTP)", seen["log"])
        # Every runner.log line carries a UTC timestamp.
        for line in seen["log"].splitlines():
            self.assertRegex(line, r"^\d{8}|^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z ")

    def test_stalled_teardown_is_bounded(self):
        calls = {"n": 0}
        real = self.srv.get_heap_status

        def heap(host=None):
            calls["n"] += 1
            if calls["n"] >= 2:  # preflight call is #1, teardown call is #2
                self.release.wait(30)
            return real(host)
        self.srv.get_heap_status = heap
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", dry_run=True)
        self.assertTrue(outcome.preflight_ok)
        self.assertFalse(os.path.exists(self._lock_path()))
        run_dir = os.path.join(self.tmpdir, outcome.run_id)
        log = open(os.path.join(run_dir, "runner.log"), encoding="utf-8").read()
        self.assertIn("teardown stalled", log)
        summary = json.load(open(os.path.join(run_dir, "summary.json"), encoding="utf-8"))
        self.assertIn("teardown_stall", summary["board_after"])

    def test_lock_released_when_body_raises(self):
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)

        def boom(**kw):
            raise RuntimeError("boom")
        runner._run_locked = boom
        with self.assertRaises(RuntimeError):
            runner.run(suite="heat", dry_run=True)
        self.assertFalse(os.path.exists(self._lock_path()))

    def test_run_dir_creation_failure_is_swallowed_and_lock_released(self):
        # Only the run directory is uncreatable: a FILE sits where the run
        # dir must go. The lock itself is taken normally.
        run_dir = os.path.join(self.tmpdir, "fixed_run_id")
        open(run_dir, "w").close()
        self.ctx["run_dir"] = run_dir
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        orig = runner_mod.report_mod.run_dir_path
        runner_mod.report_mod.run_dir_path = lambda root, rid: run_dir
        try:
            try:
                runner.run(suite="heat", dry_run=True)
            except OSError:
                pass  # write_run may fail later; the point is the earlier branch
        finally:
            runner_mod.report_mod.run_dir_path = orig
        self.assertTrue(any("could not create run dir" in l for l in runner.transcript))
        self.assertFalse(os.path.exists(self._lock_path()))

    def test_runner_log_is_redacted(self):
        secret = "secretX-1234"
        os.environ["KILNCTL_WEB_PASSWORD"] = secret
        self.addCleanup(os.environ.pop, "KILNCTL_WEB_PASSWORD", None)
        saved = R.REGISTRY["ST-05"]
        self.addCleanup(R.REGISTRY.__setitem__, "ST-05", saved)
        import dataclasses

        def judge(ctx):
            raise RuntimeError(f"login failed with {secret}")
        R.REGISTRY["ST-05"] = dataclasses.replace(saved, judge=judge)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        log = open(os.path.join(self.tmpdir, outcome.run_id, "runner.log"), encoding="utf-8").read()
        self.assertIn("case raised", log + "".join(runner.transcript))
        self.assertIn("RuntimeError", log)
        self.assertNotIn(secret, log)

    def test_preflight_stall_skips_teardown_probes(self):
        self._stall_heap()
        probes = {"n": 0}
        orig = self.srv._safety.get_status

        def counted():
            probes["n"] += 1
            return orig()
        self.srv._safety.get_status = counted
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", dry_run=True)
        self.assertEqual(probes["n"], 1, "teardown re-probed a wedged board")
        summary = json.load(open(os.path.join(self.tmpdir, outcome.run_id, "summary.json"), encoding="utf-8"))
        self.assertEqual(summary["board_after"],
                         {"teardown_skipped": "preflight stalled; no case ran"})

    def test_next_preflight_refuses_while_abandoned_worker_alive(self):
        self._stall_heap()
        BenchTestRunner(self.ctx, logs_root=self.tmpdir).run(suite="heat", dry_run=True)
        self.assertTrue(runner_mod._live_abandoned())
        again = BenchTestRunner(self.ctx, logs_root=self.tmpdir).run(suite="heat", dry_run=True)
        self.assertFalse(again.preflight_ok)
        self.assertIn("abandoned worker", again.preflight_reason)
        self.assertIn("bench-preflight", again.preflight_reason)

    def test_abandoned_teardown_never_issues_mutating_calls(self):
        self.srv.exec_status.state_name = "running"
        gate = threading.Event()
        orig = self.srv._profiles.get_exec_status

        def slow():
            gate.wait(30)
            return orig()
        self.srv._profiles.get_exec_status = slow
        self.addCleanup(gate.set)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        out = runner.teardown()
        self.assertIn("teardown_stall", out)
        gate.set()
        deadline = time.time() + 5
        while runner_mod._live_abandoned() and time.time() < deadline:
            time.sleep(0.01)
        self.assertFalse(self.srv.profiles_stop_called)

    def test_list_recent_runs_skips_dirs_without_summary(self):
        done = os.path.join(self.tmpdir, "20260101T000000Z_smoke")
        os.makedirs(done)
        json.dump({"run_id": "done"}, open(os.path.join(done, "summary.json"), "w"))
        os.makedirs(os.path.join(self.tmpdir, "20260102T000000Z_heat"))  # in flight
        got = report_mod.list_recent_runs(self.tmpdir, n=1)
        self.assertEqual([r["run_id"] for r in got], ["done"])

    def test_run_bounded_helper_passes_result_and_exception(self):
        self.assertEqual(runner_mod._run_bounded(lambda: 7, 1.0, "x"), 7)
        with self.assertRaises(ValueError):
            runner_mod._run_bounded(lambda: (_ for _ in ()).throw(ValueError("v")), 1.0, "x")


if __name__ == "__main__":
    unittest.main()
