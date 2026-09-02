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


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
