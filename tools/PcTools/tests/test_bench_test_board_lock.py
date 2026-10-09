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
import socket
import subprocess
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
        for suite in ("smoke", "static", "stack"):
            self.assertFalse(board_lock.suite_is_mutating(suite), suite)

    def test_known_mutating_suites_are_mutating(self):
        for suite in ("heat", "autotune", "ota", "flash", "safety", "web", "lcd", "nightly", "full"):
            self.assertTrue(board_lock.suite_is_mutating(suite), suite)

    def test_unknown_suite_name_is_mutating_by_default(self):
        # Fail closed: a suite this table has never seen is NOT assumed safe.
        self.assertTrue(board_lock.suite_is_mutating("some_future_suite"))


class AcquireReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="board_lock_test_")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_read_only_suite_is_refused_while_a_live_mutating_lock_is_held(self):
        held = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(held.release)
        with self.assertRaises(board_lock.BoardLockHeld) as ctx:
            board_lock.acquire("stack", logs_root=self.tmpdir)
        self.assertIn("suite='heat'", str(ctx.exception))

    def test_read_only_suite_ignores_but_never_removes_a_stale_lock(self):
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        dead = board_lock.LockInfo(pid=_dead_pid(), hostname=socket.gethostname(), suite="heat",
                                    started_at="2020-01-01T00:00:00Z", tag=None)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(dead.to_json())
        reader = board_lock.acquire("smoke", logs_root=self.tmpdir)
        self.addCleanup(reader.release)
        self.assertIsNotNone(reader)
        self.assertTrue(os.path.exists(path))  # left for a mutating run to reclaim

    def test_read_only_suite_refuses_an_unparseable_lock_file(self):
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("not json")
        with self.assertRaises(board_lock.BoardLockHeld):
            board_lock.acquire("smoke", logs_root=self.tmpdir)

    def test_lock_from_another_host_is_never_reclaimed(self):
        path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        foreign = board_lock.LockInfo(pid=_dead_pid(), hostname="some-other-host", suite="heat",
                                       started_at="2020-01-01T00:00:00Z", tag=None)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(foreign.to_json())
        with self.assertRaises(board_lock.BoardLockHeld):
            board_lock.acquire("heat", logs_root=self.tmpdir)

    def test_mutating_acquire_creates_a_missing_logs_directory(self):
        root = os.path.join(self.tmpdir, "fresh_clone", "logs", "bench_test")
        lock = board_lock.acquire("heat", logs_root=root)
        self.addCleanup(lock.release)
        self.assertTrue(os.path.exists(os.path.join(root, board_lock.LOCK_FILENAME)))

    def test_read_only_suite_never_touches_the_lock_file(self):
        lock = board_lock.acquire("smoke", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))
        # It does register a reader marker, in its own directory.
        readers_dir = os.path.join(self.tmpdir, board_lock.READERS_DIRNAME)
        self.assertEqual(len(os.listdir(readers_dir)), 1)

    def test_read_only_reader_marker_is_removed_on_release(self):
        lock = board_lock.acquire("smoke", logs_root=self.tmpdir)
        readers_dir = os.path.join(self.tmpdir, board_lock.READERS_DIRNAME)
        self.assertEqual(len(os.listdir(readers_dir)), 1)
        lock.release()
        self.assertEqual(len(os.listdir(readers_dir)), 0)

    def test_mutating_acquire_is_refused_while_a_read_only_reader_is_registered(self):
        """Closes the second documented gap: a mutating run starting while a
        read-only run is already in flight is now blocked, not silently
        allowed to race the board."""
        reader = board_lock.acquire("smoke", logs_root=self.tmpdir)
        self.addCleanup(reader.release)
        with self.assertRaises(board_lock.BoardLockHeld) as ctx:
            board_lock.acquire("heat", logs_root=self.tmpdir)
        self.assertIn("read-only run is in progress", str(ctx.exception))
        # Refusing must not have created the main lock file.
        self.assertFalse(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))

    def test_dead_reader_marker_is_cleaned_up_and_does_not_block(self):
        readers_dir = os.path.join(self.tmpdir, board_lock.READERS_DIRNAME)
        os.makedirs(readers_dir, exist_ok=True)
        dead = board_lock.LockInfo(pid=_dead_pid(), hostname=socket.gethostname(), suite="smoke",
                                    started_at="2020-01-01T00:00:00Z", tag=None)
        marker = os.path.join(readers_dir, "99999_1.json")
        with open(marker, "w", encoding="utf-8") as fh:
            fh.write(dead.to_json())
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        self.assertFalse(os.path.exists(marker))  # opportunistically cleaned up

    def test_mutating_acquire_proceeds_once_the_reader_releases(self):
        reader = board_lock.acquire("smoke", logs_root=self.tmpdir)
        reader.release()
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        self.assertIsNotNone(lock)

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
        dead = board_lock.LockInfo(pid=_dead_pid(), hostname=socket.gethostname(), suite="heat",
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


class StaleReclaimRaceTest(unittest.TestCase):
    """Deterministic repro of the stale-reclaim race: two mutating
    acquire() calls that both read the same dead-pid lock file must never
    both end up believing they hold it.

    Ordering is forced with events, not sleeps: thread B reads the stale
    file and decides its holder is dead (the first `_holder_alive` call on
    B's thread), then parks; only then does thread A start, reclaim the
    stale file and create its own live lock; only after A has returned does
    B resume into its reclaim step. The original implementation's blind
    `os.remove(path)` at that point deletes A's live lock by name and B's
    create then succeeds too -- two winners. The fix re-verifies under an
    exclusive reclaim mutex that the file is still the stale one it read,
    finds A's lock instead, removes nothing, and refuses.
    """

    WAIT_S = 10

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="board_lock_race_test_")
        self.path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)
        self.dead = board_lock.LockInfo(pid=_dead_pid(), hostname=socket.gethostname(),
                                        suite="heat", started_at="2020-01-01T00:00:00Z", tag=None)
        with open(self.path, "w", encoding="utf-8") as fh:
            fh.write(self.dead.to_json())

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_two_racing_reclaimers_never_both_win(self):
        import threading
        from unittest import mock

        b_decided_stale = threading.Event()
        a_done = threading.Event()
        real_holder_alive = board_lock._holder_alive
        b_state = {"thread": None, "gated": False}
        waits = []

        def gated_holder_alive(info):
            alive = real_holder_alive(info)
            if (threading.current_thread() is b_state["thread"] and not b_state["gated"]
                    and not alive):
                b_state["gated"] = True
                b_decided_stale.set()
                waits.append(a_done.wait(timeout=self.WAIT_S))
            return alive

        results = {}

        def run_a():
            waits.append(b_decided_stale.wait(timeout=self.WAIT_S))
            try:
                results["a"] = board_lock.acquire("heat", logs_root=self.tmpdir)
            except board_lock.BoardLockHeld as exc:
                results["a"] = exc
            finally:
                a_done.set()

        def run_b():
            try:
                results["b"] = board_lock.acquire("ota", logs_root=self.tmpdir)
            except board_lock.BoardLockHeld as exc:
                results["b"] = exc

        t_a = threading.Thread(target=run_a)
        t_b = threading.Thread(target=run_b)
        b_state["thread"] = t_b
        with mock.patch.object(board_lock, "_holder_alive", side_effect=gated_holder_alive):
            t_b.start()
            t_a.start()
            t_a.join(timeout=self.WAIT_S * 2)
            t_b.join(timeout=self.WAIT_S * 2)

        self.assertEqual(waits, [True, True], "forced ordering did not happen")
        winners = [v for v in results.values() if isinstance(v, board_lock.BoardLock)]
        for w in winners:
            self.addCleanup(w.release)
        self.assertEqual(
            len(winners), 1,
            f"expected exactly one winner of the racing reclaim, got {len(winners)}: {results}",
        )
        self.assertIsInstance(results["a"], board_lock.BoardLock)
        with open(self.path, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["suite"], "heat")  # A's lock survived intact

    def test_reclaim_never_empties_the_path_while_a_live_lock_exists(self):
        """The first fix renamed whatever was at the path aside and renamed
        it back if live; a third acquirer could create in that gap and the
        put-back overwrote it. Drive the reclaim step directly against a
        LIVE lock, attempt a third acquire from inside its re-verification
        step (while the reclaim mutex is held), and require that the third acquire is refused and the live lock is
        untouched."""
        os.remove(self.path)
        a = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(a.release)
        real_same_holder = board_lock._same_holder
        third = {}

        def hooked(a_info, b_info):
            if "c" not in third:
                try:
                    third["c"] = board_lock.acquire("ota", logs_root=self.tmpdir)
                except board_lock.BoardLockHeld as exc:
                    third["c"] = exc
            return real_same_holder(a_info, b_info)

        from unittest import mock
        with mock.patch.object(board_lock, "_same_holder", side_effect=hooked):
            # The caller believed the file held `self.dead` (stale read).
            self.assertIsNone(board_lock._reclaim_stale(self.path, self.dead))
        if isinstance(third.get("c"), board_lock.BoardLock):
            self.addCleanup(third["c"].release)
        self.assertIsInstance(third.get("c"), board_lock.BoardLockHeld)
        with open(self.path, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["started_at"], a.info.started_at)

    def test_orphaned_reclaim_mutex_from_a_dead_pid_does_not_block_forever(self):
        mutex = self.path + board_lock.RECLAIM_MUTEX_SUFFIX
        with open(mutex, "w", encoding="utf-8") as fh:
            fh.write(self.dead.to_json())
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        self.assertIsNotNone(lock.reclaimed_from)
        self.assertFalse(os.path.exists(mutex))

    def test_reclaim_mutex_held_by_a_live_process_refuses_and_leaves_the_lock(self):
        mutex = self.path + board_lock.RECLAIM_MUTEX_SUFFIX
        live = board_lock.LockInfo(pid=os.getpid(), hostname=socket.gethostname(),
                                   suite="reclaim", started_at="2026-01-01T00:00:00Z", tag=None)
        with open(mutex, "w", encoding="utf-8") as fh:
            fh.write(live.to_json())
        with self.assertRaises(board_lock.BoardLockHeld):
            board_lock.acquire("heat", logs_root=self.tmpdir)
        self.assertTrue(os.path.exists(mutex))
        with open(self.path, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["pid"], self.dead.pid)  # stale lock untouched


class ReaderWriterOrderingTest(unittest.TestCase):
    """Each side publishes its own claim before checking for the other's,
    so a reader registering while a mutating run starts can never let both
    proceed. Driven deterministically by running the other side's acquire()
    from inside this side's check step."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="board_lock_rw_test_")
        self.readers_dir = os.path.join(self.tmpdir, board_lock.READERS_DIRNAME)
        self.lock_path = os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_mutating_run_starting_during_reader_registration_is_refused(self):
        from unittest import mock
        real_check = board_lock._refuse_reader_if_mutating_holder
        inner = {}

        def hooked(path, suite):
            try:
                inner["m"] = board_lock.acquire("heat", logs_root=self.tmpdir)
            except board_lock.BoardLockHeld as exc:
                inner["m"] = exc
            return real_check(path, suite)

        with mock.patch.object(board_lock, "_refuse_reader_if_mutating_holder", side_effect=hooked):
            reader = board_lock.acquire("smoke", logs_root=self.tmpdir)
        self.addCleanup(reader.release)
        self.assertIsInstance(inner["m"], board_lock.BoardLockHeld)
        self.assertFalse(os.path.exists(self.lock_path))

    def test_reader_registering_during_mutating_acquire_is_refused_and_cleans_up(self):
        from unittest import mock
        real_scan = board_lock._live_readers
        inner = {}

        def hooked(logs_root):
            try:
                inner["r"] = board_lock.acquire("smoke", logs_root=self.tmpdir)
            except board_lock.BoardLockHeld as exc:
                inner["r"] = exc
            return real_scan(logs_root)

        with mock.patch.object(board_lock, "_live_readers", side_effect=hooked):
            lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(lock.release)
        self.assertIsInstance(inner["r"], board_lock.BoardLockHeld)
        self.assertEqual(os.listdir(self.readers_dir), [])  # refused reader removed its marker

    def test_unparseable_reader_marker_refuses_a_mutating_run(self):
        os.makedirs(self.readers_dir)
        with open(os.path.join(self.readers_dir, "1_x.json"), "w", encoding="utf-8") as fh:
            fh.write("not json")
        with self.assertRaises(board_lock.BoardLockHeld):
            board_lock.acquire("heat", logs_root=self.tmpdir)
        self.assertFalse(os.path.exists(self.lock_path))

    def test_reader_marker_on_another_host_counts_as_live(self):
        os.makedirs(self.readers_dir)
        other = board_lock.LockInfo(pid=_dead_pid(), hostname="some-other-host", suite="smoke",
                                    started_at="2026-01-01T00:00:00Z", tag=None)
        with open(os.path.join(self.readers_dir, "1_y.json"), "w", encoding="utf-8") as fh:
            fh.write(other.to_json())
        with self.assertRaises(board_lock.BoardLockHeld):
            board_lock.acquire("heat", logs_root=self.tmpdir)

    def test_leftover_tmp_marker_is_ignored(self):
        os.makedirs(self.readers_dir)
        with open(os.path.join(self.readers_dir, "1_z.tmp"), "w", encoding="utf-8") as fh:
            fh.write("")
        lock = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(lock.release)

    def test_release_retries_through_a_transient_sharing_violation(self):
        from unittest import mock
        reader = board_lock.acquire("smoke", logs_root=self.tmpdir)
        real_remove = os.remove
        calls = {"n": 0}

        def flaky(path, *a, **k):
            calls["n"] += 1
            if calls["n"] == 1:
                raise PermissionError(32, "sharing violation")
            return real_remove(path, *a, **k)

        with mock.patch("os.remove", side_effect=flaky):
            reader.release()
        self.assertEqual(os.listdir(self.readers_dir), [])
        self.assertGreaterEqual(calls["n"], 2)


class PidAliveTest(unittest.TestCase):
    def test_own_pid_is_alive(self):
        self.assertTrue(board_lock._pid_alive(os.getpid()))

    def test_exited_pid_is_dead(self):
        self.assertFalse(board_lock._pid_alive(_dead_pid()))

    def test_exited_process_whose_handle_is_still_open_is_dead(self):
        # Popen keeps its process handle open until the object is collected,
        # so on Windows OpenProcess() on this pid still SUCCEEDS -- a crashed
        # server whose parent shell still holds its handle must read dead.
        proc = subprocess.Popen(_trivial_cmd())
        proc.wait(timeout=5)
        self.assertFalse(board_lock._pid_alive(proc.pid))

    @unittest.skipUnless(os.name == "nt", "Windows access-denied path")
    def test_access_denied_pid_counts_as_alive(self):
        # pid 4 is the Windows "System" process: always running, and
        # OpenProcess on it fails with ERROR_ACCESS_DENIED for a normal user.
        self.assertTrue(board_lock._pid_alive(4))


def _trivial_cmd():
    return ["cmd", "/c", "exit 0"] if os.name == "nt" else ["true"]


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

    def test_read_only_run_is_refused_while_a_mutating_lock_is_held(self):
        held = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(held.release)
        runner = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        with self.assertRaises(board_lock.BoardLockHeld):
            runner.run(suite="smoke", cases=["ST-05"])
        self.assertTrue(os.path.exists(os.path.join(self.tmpdir, board_lock.LOCK_FILENAME)))
        # The refused reader backed its own marker out again.
        self.assertEqual(os.listdir(os.path.join(self.tmpdir, board_lock.READERS_DIRNAME)), [])

    def test_lcd_run_takes_the_lock(self):
        held = board_lock.acquire("heat", logs_root=self.tmpdir)
        self.addCleanup(held.release)
        runner = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        with self.assertRaises(board_lock.BoardLockHeld):
            runner.run(suite="lcd", cases=["LCD-19"])

    def test_two_read_only_runs_can_both_proceed(self):
        runner_a = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        runner_b = BenchTestRunner(self._ctx(), logs_root=self.tmpdir)
        outcome_a = runner_a.run(suite="smoke", cases=["ST-05"])
        outcome_b = runner_b.run(suite="stack", cases=["SK-01"])
        self.assertIsNotNone(outcome_a)
        self.assertIsNotNone(outcome_b)


if __name__ == "__main__":
    unittest.main()
