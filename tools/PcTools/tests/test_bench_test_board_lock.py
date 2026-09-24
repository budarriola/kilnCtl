#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.board_lock and its wiring into
BenchTestRunner.run() -- docs/audits/profile_executor_panic_2026-09-24.md
(HP-02/HP-05): two `bench_test_run(suite="heat")` calls from two different
sessions drove the same board at once. Everything here uses fakes; no real
board or MCP server is touched.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_board_lock.py -q
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import board_lock  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.runner import BenchTestRunner  # noqa: E402

# Reuse the fake srv/capability_preflight scaffolding from the runner's own
# test module rather than re-inventing it.
from test_bench_test_runner import _FakeSrv, _FakeCapabilityPreflightReport  # noqa: E402


class SuiteClassificationTest(unittest.TestCase):
    def test_read_only_suites_are_not_mutating(self):
        for suite in ("smoke", "static", "stack", "lcd"):
            self.assertFalse(board_lock.suite_is_mutating(suite), suite)

    def test_known_mutating_suites_are_mutating(self):
        for suite in ("heat", "autotune", "ota", "flash", "safety", "web", "nightly", "full"):
            self.assertTrue(board_lock.suite_is_mutating(suite), suite)

    def test_unknown_suite_name_is_mutating_by_default(self):
        # Fail closed: a suite this table has never seen is NOT assumed safe.
        self.assertTrue(board_lock.suite_is_mutating("some_future_suite"))


class AcquireReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="board_lock_test_")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_read_only_suite_never_touches_the_lock_file(self):
        lock = board_lock.acquire("smoke", logs_root=self.tmpdir)
        self.assertIsNone(lock)
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))

    def test_mutating_suite_acquires_and_writes_a_lock_file(self):
        lock = board_lock.acquire("heat", tag="mytag", logs_root=self.tmpdir)
        self.assertIsNotNone(lock)
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        self.assertTrue(os.path.exists(path))
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        self.assertEqual(data["pid"], os.getpid())
        self.assertEqual(data["suite"], "heat")
        self.assertEqual(data["tag"], "mytag")
        lock.release()
        self.assertFalse(os.path.exists(path))

    def test_second_acquire_by_a_live_process_is_refused(self):
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        with self.assertRaises(board_lock.BoardLockHeld) as ctx:
            board_lock.acquire("ota", logs_root=self.tmpdir)
        msg = str(ctx.exception)
        self.assertIn(f"pid={os.getpid()}", msg)
        self.assertIn("suite='heat'", msg)

    def test_release_is_idempotent(self):
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        lock.release()
        lock.release()  # must not raise
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))

    def test_release_only_removes_a_lock_still_recognized_as_its_own(self):
        # Simulate: we held a lock, someone stale-reclaimed it and wrote a
        # NEW lock file in its place (different started_at), then our old
        # handle's release() must NOT delete the new owner's file.
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        other = board_lock.LockInfo(pid=999999, hostname="other", suite="ota",
                                     started_at="2026-01-01T00:00:00Z", tag=None)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(other.to_json())
        lock.release()
        self.assertTrue(os.path.exists(path))  # the other owner's file survives
        with open(path, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["pid"], 999999)

    def test_stale_lock_from_a_dead_pid_is_reclaimed(self):
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        os.makedirs(self.tmpdir, exist_ok=True)
        dead = board_lock.LockInfo(pid=_dead_pid(), hostname="ghost", suite="heat",
                                    started_at="2020-01-01T00:00:00Z", tag=None)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(dead.to_json())
        lock = board_lock.acquire("ota", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        self.assertIsNotNone(lock.reclaimed_from)
        self.assertEqual(lock.reclaimed_from.pid, dead.pid)
        self.assertEqual(lock.info.suite, "ota")

    def test_unparseable_lock_file_refuses_rather_than_guessing(self):
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        os.makedirs(self.tmpdir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("not json")
        with self.assertRaises(board_lock.BoardLockHeld):
            board_lock.acquire("heat", logs_root=self.tmpdir)


def _dead_pid() -> int:
    """A pid essentially guaranteed not to be alive right now, for both
    Windows and POSIX -- start a trivial process and wait for it to exit."""
    import subprocess

    if os.name == "nt":
        proc = subprocess.Popen(["cmd", "/c", "exit 0"])
    else:
        proc = subprocess.Popen(["true"])
    pid = proc.pid
    proc.wait(timeout=5)
    return pid


class RunnerLockIntegrationTest(unittest.TestCase):
    """Exercises the lock as wired into BenchTestRunner.run() -- the actual
    call path bench_test_run/ota_matrix_run/run_suite all go through."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="board_lock_runner_test_")
        self.fake_srv = _FakeSrv()
        self.cp_report = _FakeCapabilityPreflightReport(ok=True)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _ctx(self):
        return {
            "srv": self.fake_srv, "host": None,
            "capability_preflight_run": lambda preset, host, **kw: self.cp_report,
            "bench_test_log_doc_path": os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"),
        }

    def test_two_concurrent_heat_runs_are_refused(self):
        """The exact HP-02/HP-05 collision, reproduced with fakes: a second
        `heat` run started while the first is still in flight must be
        refused, not silently allowed to race the board."""
        runner_a = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)  # runner A "still running"
        self.addCleanup(lock.release)

        runner_b = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        with self.assertRaises(board_lock.BoardLockHeld) as ctx:
            runner_b.run(suite="heat", cases=["HP-01"])
        self.assertIn("suite='heat'", str(ctx.exception))
        # runner_b must not have written a run directory -- it never got
        # past lock acquisition.
        self.assertEqual(os.listdir(self.tmpdir), [board_lock.LOCK_FILENAME])
        del runner_a  # unused; documents which runner "owns" the board here

    def test_lock_is_released_after_a_normal_mutating_run(self):
        runner = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", cases=["HP-01"], allow_heat=False)
        self.assertIsNotNone(outcome)
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))

    def test_lock_is_released_even_if_a_case_raises(self):
        self._saved = R.REGISTRY["HP-01"]
        self.addCleanup(lambda: R.REGISTRY.__setitem__("HP-01", self._saved))

        def _boom(ctx):
            raise RuntimeError("simulated case crash")

        import dataclasses
        R.REGISTRY["HP-01"] = dataclasses.replace(self._saved, judge=_boom)

        runner = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        outcome = runner.run(suite="heat", cases=["HP-01"])
        # runner.run() swallows the case exception into a FAIL verdict
        # (existing behavior) -- but the point of this test is the lock:
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))
        self.assertEqual(outcome.results["HP-01"].verdict, R.Verdict.FAIL)

    def test_lock_is_released_even_if_run_raises_before_completing(self):
        """Simulates the release()-on-exception path more directly: patch
        teardown() to raise, and confirm the lock is still gone afterward."""
        runner = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)

        def _boom_teardown():
            raise RuntimeError("simulated teardown crash")

        runner.teardown = _boom_teardown  # type: ignore[assignment]
        with self.assertRaises(RuntimeError):
            runner.run(suite="heat", cases=["HP-01"], allow_heat=False)
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))

    def test_read_only_suite_runs_freely_while_a_mutating_lock_is_held(self):
        held = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(held.release)
        runner = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        outcome = runner.run(suite="smoke", cases=["ST-05"])
        self.assertIsNotNone(outcome)  # never refused: smoke needs no lock

    def test_two_read_only_runs_can_both_proceed(self):
        runner_a = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        runner_b = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        outcome_a = runner_a.run(suite="smoke", cases=["ST-05"])
        outcome_b = runner_b.run(suite="stack", cases=["SK-01"])
        self.assertIsNotNone(outcome_a)
        self.assertIsNotNone(outcome_b)


if __name__ == "__main__":
    unittest.main()
