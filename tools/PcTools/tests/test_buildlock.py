#!/usr/bin/env python3
"""Tests for mcpkit.buildlock -- the cross-process lock that serializes
build/test entry points sharing a build directory.

Concurrent build_kilnfw invocations were observed today corrupting a shared
build directory: a ninja manifest-lock race ("failed recompaction:
Permission denied") and an interleaved write to a generated header
(build_info.h reading back as "unknown type name 'by'"). The fix is a
cross-process lock keyed on the actual contended resource. These tests prove
that lock actually serializes concurrent processes (not just concurrent
threads sharing one interpreter -- the real bug was cross-*process*), that a
losing waiter's result still comes out correct rather than merely
"didn't crash", and that a stale lock left by a dead holder gets reclaimed
rather than wedging every future caller forever.

Run with: python -m pytest tools/PcTools/tests/test_buildlock.py -q
"""
from __future__ import annotations

import multiprocessing
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcpkit.buildlock import BuildLockTimeout, _lock_path, build_lock  # noqa: E402


def _worker(resource_key: str, shared_counter, results_queue, hold_s: float) -> None:
    """Run in a separate process. Acquires the lock, reads-increments-writes
    ``shared_counter`` with a deliberate delay in between (so an unserialized
    pair of workers would race and lose an update), then records what value
    it wrote. Multiprocessing's ``Value`` lives in real shared memory but its
    read-modify-write here is intentionally NOT protected by anything except
    ``build_lock`` -- that is the point of the test.
    """
    with build_lock(resource_key, wait_timeout=30.0, poll_interval=0.05):
        current = shared_counter.value
        time.sleep(hold_s)
        shared_counter.value = current + 1
        results_queue.put((os.getpid(), current + 1))


class BuildLockSerializesConcurrentProcesses(unittest.TestCase):
    def test_two_processes_serialize_and_both_results_are_correct(self):
        """Two real OS processes contend for the same resource key. If the
        lock works, they never overlap, so the shared counter ends at 2 with
        no lost update -- if it did NOT work, both workers would read 0
        during the other's sleep and both write 1, which this asserts
        against directly rather than just checking neither process crashed.
        """
        resource_key = f"test-serialize-{os.getpid()}-{int(time.time())}"
        ctx = multiprocessing.get_context("spawn")
        counter = ctx.Value("i", 0)
        results = ctx.Queue()

        p1 = ctx.Process(target=_worker, args=(resource_key, counter, results, 0.5))
        p2 = ctx.Process(target=_worker, args=(resource_key, counter, results, 0.5))
        started = time.monotonic()
        p1.start()
        time.sleep(0.1)  # make p1 the reliable first acquirer
        p2.start()
        p1.join(timeout=15)
        p2.join(timeout=15)
        elapsed = time.monotonic() - started

        self.assertFalse(p1.is_alive())
        self.assertFalse(p2.is_alive())
        self.assertEqual(counter.value, 2, "lost update -- the two processes overlapped")
        # Each holds the lock ~0.5s; if serialized, total wall time is >=~1s.
        # An unserialized pair would finish in ~0.6s (both racing together).
        self.assertGreaterEqual(elapsed, 0.9, "finished too fast to have been serialized")

        seen = sorted((v for _, v in (results.get() for _ in range(2))))
        self.assertEqual(seen, [1, 2], f"unexpected written values: {seen}")

    def test_timeout_reports_clearly_and_does_not_hang_forever(self):
        """A waiter that cannot get the lock within its budget raises
        BuildLockTimeout naming the resource and how long it waited --
        it does not fail immediately (contention is expected/normal) and it
        does not block forever (a dead holder must not wedge the tool)."""
        resource_key = f"test-timeout-{os.getpid()}-{int(time.time())}"
        with build_lock(resource_key, wait_timeout=60.0):
            # Held by the `with` above (this process, this thread) --
            # nested acquisition of the same key must therefore time out.
            started = time.monotonic()
            with self.assertRaises(BuildLockTimeout) as ctx:
                with build_lock(resource_key, wait_timeout=0.5, poll_interval=0.1):
                    pass  # pragma: no cover - must never be reached
            waited = time.monotonic() - started
            self.assertLess(waited, 5.0, "did not respect its own wait_timeout")
            self.assertIn(resource_key, str(ctx.exception))
            self.assertIn("waited", str(ctx.exception))

    def test_stale_lock_is_reclaimed_not_wedged_forever(self):
        """A lock file left behind by a holder that never released it (crash,
        kill -9, a rebooted box) must eventually be reclaimed -- otherwise
        every future build against that resource hangs until someone manually
        deletes a temp file, which is worse than the corruption this exists
        to prevent."""
        resource_key = f"test-stale-{os.getpid()}-{int(time.time())}"
        lock_path = _lock_path(resource_key)
        os.makedirs(os.path.dirname(lock_path), exist_ok=True)
        with open(lock_path, "w", encoding="ascii") as handle:
            handle.write("99999 0\n")  # a pid that is not us, "acquired" at epoch 0
        # Back-date the file so it looks old regardless of when epoch 0 was written.
        old = time.time() - 10.0
        os.utime(lock_path, (old, old))

        acquired = []
        try:
            with build_lock(resource_key, wait_timeout=5.0, stale_after=1.0, poll_interval=0.05):
                acquired.append(True)
        finally:
            if not acquired:
                try:
                    os.remove(lock_path)
                except OSError:
                    pass
        self.assertEqual(acquired, [True], "stale lock (age > stale_after) was never reclaimed")

    def test_unrelated_resource_keys_do_not_block_each_other(self):
        """The lock must be scoped per-resource, not a single global lock --
        a KilnFW build and a SaftyFW build must run concurrently."""
        key_a = f"test-scope-a-{os.getpid()}-{int(time.time())}"
        key_b = f"test-scope-b-{os.getpid()}-{int(time.time())}"
        with build_lock(key_a, wait_timeout=5.0):
            started = time.monotonic()
            with build_lock(key_b, wait_timeout=5.0, poll_interval=0.05):
                pass
            elapsed = time.monotonic() - started
        self.assertLess(elapsed, 1.0, "a different resource key waited on an unrelated lock")


class BuildLockDeadHolder(unittest.TestCase):
    """Reclaim by recorded PID, not only by age."""

    def _plant(self, text):
        key = f"test-pid-{os.getpid()}-{time.time_ns()}"
        path = _lock_path(key)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="ascii") as handle:
            handle.write(text)
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        return key

    def _dead_pid(self):
        import subprocess
        proc = subprocess.Popen([sys.executable, "-c", "pass"])
        proc.wait()
        return proc.pid

    def test_dead_pid_lock_is_reclaimed_at_once(self):
        key = self._plant(f"{self._dead_pid()} {time.time():.0f}\n")
        t0 = time.monotonic()
        with build_lock(key, wait_timeout=5.0, stale_after=3600.0, poll_interval=0.05):
            pass
        self.assertLess(time.monotonic() - t0, 3.0)

    def test_live_pid_lock_is_respected(self):
        key = self._plant(f"{os.getpid()} {time.time():.0f}\n")
        with self.assertRaises(BuildLockTimeout):
            with build_lock(key, wait_timeout=0.5, stale_after=3600.0, poll_interval=0.05):
                self.fail("took a lock held by a live process")

    def test_reused_pid_started_after_lock_is_reclaimed(self):
        from unittest import mock
        import mcpkit.buildlock as bl
        # Holder recorded its own start time; the live PID now started 1000 s later.
        key = self._plant(f"{os.getpid()} {time.time() - 1000:.0f} tok\n")
        with mock.patch.object(bl, "_pid_info", return_value=(True, time.time())):
            with build_lock(key, wait_timeout=5.0, stale_after=3600.0, poll_interval=0.05):
                pass

    def test_clock_step_does_not_reclaim_live_holder(self):
        """LOW-1: the guard compares recorded vs actual PROCESS start, so a
        wall-clock step cannot make a live holder look reused."""
        import mcpkit.buildlock as bl
        _alive, start = bl._pid_info(os.getpid())
        key = self._plant(f"{os.getpid()} {int(start)} tok\n")
        with self.assertRaises(BuildLockTimeout):
            with build_lock(key, wait_timeout=0.4, stale_after=3600.0, poll_interval=0.05):
                self.fail("reclaimed a live holder")

    def test_lock_records_holders_own_process_start(self):
        import mcpkit.buildlock as bl
        _alive, start = bl._pid_info(os.getpid())
        key = f"test-recstart-{os.getpid()}-{time.time_ns()}"
        with build_lock(key, wait_timeout=2.0):
            parsed = bl._parse_holder(bl._read_holder(_lock_path(key)))
        self.assertEqual(parsed[0], os.getpid())
        self.assertLessEqual(abs(parsed[1] - start), 2.0)
        self.assertIsNotNone(parsed[2])

    def test_start_within_slack_is_same_process(self):
        import mcpkit.buildlock as bl
        _alive, start = bl._pid_info(os.getpid())
        self.assertFalse(bl._text_is_dead_holder(f"{os.getpid()} {int(start) + 1} tok"))
        self.assertTrue(bl._text_is_dead_holder(f"{os.getpid()} {int(start) + 10} tok"))

    def test_unparseable_holder_is_not_treated_as_dead(self):
        key = self._plant("")
        with self.assertRaises(BuildLockTimeout):
            with build_lock(key, wait_timeout=0.4, stale_after=3600.0, poll_interval=0.05):
                self.fail("reclaimed an empty (being-written) lock")


def _stress_worker(barrier, key_fmt, rounds, overlaps, entered):
    for r in range(rounds):
        barrier.wait(60)  # main has planted the dead lock for this round
        key = key_fmt.format(r)
        with build_lock(key, wait_timeout=60.0, stale_after=3600.0, poll_interval=0.002):
            marker = _lock_path(key) + ".inside"
            try:
                fd = os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            except FileExistsError:
                with overlaps.get_lock():
                    overlaps.value += 1
            else:
                os.close(fd)
                with entered.get_lock():
                    entered.value += 1
                time.sleep(0.01)
                os.remove(marker)
        barrier.wait(60)


class BuildLockReclaimStress(unittest.TestCase):
    """REVIEW_BUILDFX3 MED-1: many waiters on a dead-PID lock, never two inside."""

    def test_eight_processes_dead_pid_lock_never_overlap(self):
        import subprocess
        ctx = multiprocessing.get_context("spawn")
        workers, rounds = 8, 20
        barrier = ctx.Barrier(workers + 1)
        overlaps = ctx.Value("i", 0)
        entered = ctx.Value("i", 0)
        key_fmt = f"test-stress-{os.getpid()}-{time.time_ns()}-{{}}"
        procs = [ctx.Process(target=_stress_worker,
                             args=(barrier, key_fmt, rounds, overlaps, entered))
                 for _ in range(workers)]
        for p in procs:
            p.start()
        try:
            for r in range(rounds):
                dead = subprocess.Popen([sys.executable, "-c", "pass"])
                dead.wait()
                path = _lock_path(key_fmt.format(r))
                with open(path, "w", encoding="ascii") as handle:
                    handle.write(f"{dead.pid} 0 deadtoken\n")
                barrier.wait(60)  # release the waiters
                barrier.wait(60)  # all finished the round
        finally:
            for p in procs:
                p.join(timeout=30)
                if p.is_alive():
                    p.terminate()
        self.assertEqual(overlaps.value, 0, "two waiters held the lock at once")
        self.assertEqual(entered.value, workers * rounds)


try:
    import pytest as _pytest
    _timeout30 = _pytest.mark.timeout(30)
except ImportError:  # pragma: no cover
    def _timeout30(cls):
        return cls


@_timeout30
class BuildLockReviewFixes(unittest.TestCase):
    def setUp(self):
        # A busy-spin or an ignored wait_timeout must FAIL as an assertion in
        # the test that hits it. pytest-timeout's Windows (thread) method
        # kills the whole pytest process, so it never prints a FAILED line
        # (REVIEW_BUILDFX5 LOW-3). Every test here keeps reclaiming for well
        # under a second; 10 s or 2000 calls means the loop never ends.
        from unittest import mock
        import mcpkit.buildlock as bl
        real = bl._try_reclaim
        state = {"n": 0, "t0": None}

        def guarded(*a, **k):
            state["n"] += 1
            if state["t0"] is None:
                state["t0"] = time.monotonic()
            if state["n"] > 2000 or time.monotonic() - state["t0"] > 10.0:
                raise AssertionError(
                    f"build_lock kept reclaiming ({state['n']} calls in "
                    f"{time.monotonic() - state['t0']:.1f}s): busy-spin or "
                    "wait_timeout ignored")
            return real(*a, **k)

        patcher = mock.patch.object(bl, "_try_reclaim", guarded)
        patcher.start()
        self.addCleanup(patcher.stop)

    def _key(self):
        return f"fx5-{os.getpid()}-{time.time_ns()}"

    def test_unknown_start_time_holder_is_still_age_reclaimed(self):
        # Documented: alive but start time unknown (e.g. OpenProcess access
        # denied) is NOT confirmed alive, so the age path still applies.
        from unittest import mock
        import mcpkit.buildlock as bl
        key = self._key()
        path = _lock_path(key)
        with open(path, "w") as f:
            f.write("4242 1700000000 tok\n")
        old = time.time() - 5000
        os.utime(path, (old, old))
        real_info = bl._pid_info

        def info(pid):
            return (True, None) if pid == 4242 else real_info(pid)

        with mock.patch.object(bl, "_pid_info", info):
            with build_lock(key, wait_timeout=3, poll_interval=0.05, stale_after=10):
                self.assertNotIn("4242", open(path).read())

    def test_release_restore_never_overwrites_a_newer_lock(self):
        # Our lock was reclaimed and replaced by A's; between our rename-aside
        # and restore, B creates a lock. The restore must not clobber B's, and
        # the grabbed aside file must not be left behind.
        from unittest import mock
        import mcpkit.buildlock as bl
        key = self._key()
        path = _lock_path(key)
        with open(path, "w") as f:
            f.write("1 2 tokenA\n")
        real_rename = os.rename
        seen = {"n": 0}

        def rename(src, dst):
            real_rename(src, dst)
            seen["n"] += 1
            if seen["n"] == 1:
                with open(path, "w") as f:
                    f.write("3 4 tokenB\n")

        try:
            with mock.patch.object(bl.os, "rename", rename):
                bl._release(path, "1 2 mine")
            self.assertEqual(open(path).read().strip(), "3 4 tokenB")
            self.assertEqual([n for n in os.listdir(os.path.dirname(path))
                              if n.startswith(os.path.basename(path) + ".rel.")], [])
        finally:
            os.remove(path)

    def test_alive_holder_old_lock_is_never_age_reclaimed(self):
        key = self._key()
        path = _lock_path(key)
        import mcpkit.buildlock as bl
        _a, start = bl._pid_info(os.getpid())
        with open(path, "w") as f:
            f.write(f"{os.getpid()} {int(start)} tok123\n")
        old = time.time() - 5000
        os.utime(path, (old, old))
        try:
            with self.assertRaises(BuildLockTimeout):
                with build_lock(key, wait_timeout=0.5, poll_interval=0.05, stale_after=10):
                    pass
            self.assertTrue(os.path.exists(path))
        finally:
            os.remove(path)

    def test_old_legacy_lock_still_age_reclaimed(self):
        key = self._key()
        path = _lock_path(key)
        with open(path, "w") as f:
            f.write(f"{os.getpid()} 1.0\n")
        old = time.time() - 5000
        os.utime(path, (old, old))
        with build_lock(key, wait_timeout=3, poll_interval=0.05, stale_after=10):
            pass

    def test_declined_reclaim_does_not_spin_and_honours_timeout(self):
        from unittest import mock
        import mcpkit.buildlock as bl
        key = self._key()
        path = _lock_path(key)
        _a, start = bl._pid_info(os.getpid())
        with open(path, "w") as f:
            f.write(f"{os.getpid()} {int(start)} tok\n")
        old = time.time() - 5000
        os.utime(path, (old, old))
        calls = []
        real = bl._try_reclaim
        def counting(*a, **k):
            calls.append(1)
            if len(calls) > 500:
                raise AssertionError("busy-spin: reclaim retried without sleeping")
            return real(*a, **k)
        try:
            with mock.patch.object(bl, "_try_reclaim", counting):
                t0 = time.monotonic()
                with self.assertRaises(BuildLockTimeout):
                    with build_lock(key, wait_timeout=0.5, poll_interval=0.1, stale_after=10):
                        pass
            self.assertLess(time.monotonic() - t0, 3)
            self.assertLess(len(calls), 20)
        finally:
            os.remove(path)

    def test_stale_sidecar_is_broken_and_live_one_is_kept(self):
        import mcpkit.buildlock as bl
        key = self._key()
        path = _lock_path(key)
        side = path + ".reclaim"
        with open(path, "w") as f:
            f.write("999999999 1.0\n")
        with open(side, "w") as f:
            f.write("x")
        # live sidecar: must survive a break attempt
        bl._break_stale_sidecar(side)
        self.assertTrue(os.path.exists(side))
        old = time.time() - 600
        os.utime(side, (old, old))
        with build_lock(key, wait_timeout=5, poll_interval=0.05):
            pass
        self.assertFalse(os.path.exists(side))

    def test_sidecar_break_restores_a_live_sidecar_grabbed_in_the_race(self):
        from unittest import mock
        import mcpkit.buildlock as bl
        side = _lock_path(self._key()) + ".reclaim"
        with open(side, "w") as f:
            f.write("x")
        try:
            # First stat says old (we decide to break); by the time we have
            # renamed it aside the file is fresh: a new reclaimer's sidecar.
            with mock.patch.object(bl.os.path, "getmtime",
                                   side_effect=[time.time() - 600, time.time()]):
                bl._break_stale_sidecar(side)
            self.assertTrue(os.path.exists(side), "deleted a live sidecar")
        finally:
            if os.path.exists(side):
                os.remove(side)

    def test_release_puts_back_a_lock_it_does_not_own(self):
        import mcpkit.buildlock as bl
        key = self._key()
        path = _lock_path(key)
        with open(path, "w") as f:
            f.write("1 2 othertoken\n")
        try:
            bl._release(path, "1 2 mine")
            self.assertEqual(open(path).read().strip(), "1 2 othertoken")
            self.assertEqual([n for n in os.listdir(os.path.dirname(path))
                              if n.startswith(os.path.basename(path) + ".rel.")], [])
        finally:
            os.remove(path)


class BuildLockReleaseAndReclaimGuards(unittest.TestCase):
    def test_release_does_not_remove_a_lock_it_no_longer_owns(self):
        key = f"test-token-{os.getpid()}-{time.time_ns()}"
        path = _lock_path(key)
        with build_lock(key, wait_timeout=2.0):
            with open(path, "w", encoding="ascii") as handle:  # someone else's lock
                handle.write("1 0 othertoken\n")
        self.assertTrue(os.path.exists(path), "released a lock owned by another holder")
        os.remove(path)

    def test_reclaim_leaves_a_lock_replaced_since_it_was_judged(self):
        import mcpkit.buildlock as bl
        key = f"test-recheck-{os.getpid()}-{time.time_ns()}"
        path = _lock_path(key)
        with open(path, "w", encoding="ascii") as handle:
            handle.write(f"{os.getpid()} 0 livetoken\n")
        try:
            bl._try_reclaim(path, "99999999 0 deadtoken", force=False)
            self.assertTrue(os.path.exists(path), "removed a lock whose content changed")
            bl._try_reclaim(path, f"{os.getpid()} 0 livetoken", force=False)
            self.assertTrue(os.path.exists(path), "removed a live (not dead) holder's lock")
        finally:
            os.remove(path)
        self.assertFalse(os.path.exists(path + ".reclaim"), "sidecar leaked")

    def test_reclaim_requires_identical_content_even_if_new_holder_also_dead(self):
        """The judged file and the file about to be removed must be the same
        file: a different (even dead) lock is left for its own judgement."""
        import mcpkit.buildlock as bl
        key = f"test-samecontent-{os.getpid()}-{time.time_ns()}"
        path = _lock_path(key)
        with open(path, "w", encoding="ascii") as handle:
            handle.write("99999998 0 tokenB\n")
        try:
            bl._try_reclaim(path, "99999997 0 tokenA", force=False)
            self.assertTrue(os.path.exists(path))
            bl._try_reclaim(path, "99999998 0 tokenB", force=False)
            self.assertFalse(os.path.exists(path), "identical dead lock not reclaimed")
        finally:
            if os.path.exists(path):
                os.remove(path)

    def test_reclaim_sidecar_held_by_another_blocks_removal(self):
        import mcpkit.buildlock as bl
        key = f"test-sidecar-{os.getpid()}-{time.time_ns()}"
        path = _lock_path(key)
        text = "99999999 0 deadtoken"
        with open(path, "w", encoding="ascii") as handle:
            handle.write(text + "\n")
        with open(path + ".reclaim", "w") as handle:
            handle.write("")
        try:
            bl._try_reclaim(path, text, force=True)
            self.assertTrue(os.path.exists(path), "reclaimed without owning the sidecar")
        finally:
            os.remove(path)
            os.remove(path + ".reclaim")


@unittest.skipUnless(os.name == "nt", "Windows kernel32 paths")
class PidInfoBranches(unittest.TestCase):
    """REVIEW_BUILDFX3 LOW-2: each _pid_info branch, with kernel32 mocked."""

    def _call(self, k32, last_error=0):
        from unittest import mock
        import ctypes
        import mcpkit.buildgate as bg
        import mcpkit.buildlock as bl
        with mock.patch.object(bg, "_kernel32", k32), \
                mock.patch.object(ctypes, "get_last_error", return_value=last_error):
            return bl._pid_info(4242)

    def _k32(self, handle=1, exit_ok=True, code=259, times_ok=True):
        from unittest import mock
        k32 = mock.Mock()
        k32.OpenProcess.return_value = handle

        def exit_code(_h, ref):
            ref._obj.value = code
            return exit_ok

        def times(_h, c, *_rest):
            c._obj.dwHighDateTime = 0x01D00000
            c._obj.dwLowDateTime = 0
            return times_ok
        k32.GetExitCodeProcess.side_effect = exit_code
        k32.GetProcessTimes.side_effect = times
        return k32

    def test_access_denied_means_alive(self):
        self.assertEqual(self._call(self._k32(handle=0), last_error=5), (True, None))

    def test_other_open_failure_means_gone(self):
        self.assertEqual(self._call(self._k32(handle=0), last_error=87), (False, None))

    def test_exited_process_with_open_handle_is_dead(self):
        self.assertEqual(self._call(self._k32(code=1)), (False, None))

    def test_exit_code_query_failure_means_alive(self):
        self.assertEqual(self._call(self._k32(exit_ok=False)), (True, None))

    def test_get_process_times_failure_means_alive_unknown_start(self):
        self.assertEqual(self._call(self._k32(times_ok=False)), (True, None))

    def test_running_process_reports_start_time(self):
        alive, started = self._call(self._k32())
        self.assertTrue(alive)
        self.assertIsNotNone(started)


_HOLDER_SCRIPT = r"""
import sys
sys.path.insert(0, sys.argv[1])
from mcpkit import workbench
workbench._run("holder", [sys.executable, "-c",
    "import os,sys,time; open(sys.argv[1],'w').write(str(os.getpid())); time.sleep(120)",
    sys.argv[2]], timeout=300)
"""


@unittest.skipUnless(os.name == "nt", "Windows Job objects")
class BuildTreeDiesWithServer(unittest.TestCase):
    """REVIEW_BUILDFX3 MED-2: killing the process that runs the build must
    take the build child with it (Job object, KILL_ON_JOB_CLOSE)."""

    def test_child_dies_when_holder_is_killed(self):
        import subprocess
        import tempfile
        import mcpkit.buildlock as bl
        src = os.path.join(os.path.dirname(__file__), "..", "src")
        with tempfile.TemporaryDirectory() as tmp:
            pidfile = os.path.join(tmp, "child.pid")
            holder = subprocess.Popen([sys.executable, "-c", _HOLDER_SCRIPT,
                                       os.path.abspath(src), pidfile])
            child_pid = None
            try:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and child_pid is None:
                    try:
                        child_pid = int(open(pidfile).read().strip())
                    except (OSError, ValueError):
                        time.sleep(0.1)
                self.assertIsNotNone(child_pid, "build child never started")
                self.assertTrue(bl._pid_info(child_pid)[0])
                holder.kill()  # TerminateProcess: no cleanup runs in the holder
                holder.wait(10)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and bl._pid_info(child_pid)[0]:
                    time.sleep(0.1)
                self.assertFalse(bl._pid_info(child_pid)[0],
                                 "orphaned build child survived its server")
            finally:
                holder.kill()
                if child_pid:
                    subprocess.run(["taskkill", "/F", "/T", "/PID", str(child_pid)],
                                   capture_output=True)

    def test_run_assigns_every_child_to_the_job(self):
        from unittest import mock
        from mcpkit import workbench
        import mcpkit.jobtree as jt
        with mock.patch.object(jt, "assign_to_kill_on_close_job", return_value=True) as m:
            workbench._run("t", [sys.executable, "-c", "pass"], timeout=60)
        m.assert_called_once()


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
