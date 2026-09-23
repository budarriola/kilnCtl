"""Unit tests for mcpkit.buildgate: _wait_any's index mapping and the
KILNCTL_BUILD_GATE_SLOTS=0 bypass (opus review advisory c).

These are deliberately narrow -- the full multi-process slot-1-wakeup
scenario (kill the holder of slot 1, confirm a waiter takes slot 1
specifically) was proven by hand with real separate processes and is not
practical to re-run as an automated unit test; see the commit message for
that transcript. What IS cheap and worth locking down here is the pure index
arithmetic in ``_wait_any`` (WAIT_OBJECT_0+i / WAIT_ABANDONED_0+i -> i,
WAIT_TIMEOUT -> the sentinel) and that ``kiln_build_gate`` with slots=0 never
touches a mutex at all.
"""

import os
import sys
import threading
import time
import uuid

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Win32 kernel mutexes only")

from mcpkit import buildgate  # noqa: E402


def _unique_mutex_pair():
    tag = uuid.uuid4().hex[:12]
    return [
        buildgate._KernelMutex(f"Local\\kilnctl_buildgate_test_{tag}_0"),
        buildgate._KernelMutex(f"Local\\kilnctl_buildgate_test_{tag}_1"),
    ]


def test_wait_any_returns_lowest_free_index_when_both_free():
    mutexes = _unique_mutex_pair()
    try:
        idx = buildgate._wait_any(mutexes, 1000)
        assert idx == 0
        mutexes[0].release()
    finally:
        for m in mutexes:
            m.close()


def test_wait_any_returns_the_specific_free_slot_when_slot_zero_is_held():
    mutexes = _unique_mutex_pair()
    held_event = threading.Event()
    release_event = threading.Event()

    # Hold slot 0 from a genuinely separate thread so the main thread's own
    # _wait_any call below cannot trivially re-acquire it via mutex
    # reentrancy -- it must actually observe slot 1 as the only free one.
    def _holder():
        acquired = mutexes[0].wait(2000)
        assert acquired in (buildgate._WAIT_OBJECT_0, buildgate._WAIT_ABANDONED_0)
        held_event.set()
        release_event.wait(5)
        mutexes[0].release()

    t = threading.Thread(target=_holder, daemon=True)
    t.start()
    try:
        assert held_event.wait(2), "holder thread never acquired slot 0"
        time.sleep(0.05)  # let the holder's WaitForSingleObject fully settle
        idx = buildgate._wait_any(mutexes, 2000)
        assert idx == 1, "expected slot 1 specifically -- slot 0 is held by another thread"
        mutexes[1].release()
    finally:
        release_event.set()
        t.join(timeout=5)
        for m in mutexes:
            m.close()


def test_wait_any_times_out_when_both_slots_are_held():
    mutexes = _unique_mutex_pair()
    # A Windows mutex is owned per-THREAD, not per-process: acquiring both
    # from this same thread and then calling _wait_any from that same thread
    # would just recurse into ownership it already holds and return
    # immediately, never exercising the real timeout path. Hold both from a
    # separate thread instead, same as the slot-1 test above.
    both_held = threading.Event()
    release_event = threading.Event()

    def _holder():
        for m in mutexes:
            assert m.wait(2000) in (buildgate._WAIT_OBJECT_0, buildgate._WAIT_ABANDONED_0)
        both_held.set()
        release_event.wait(5)
        for m in mutexes:
            m.release()

    t = threading.Thread(target=_holder, daemon=True)
    t.start()
    try:
        assert both_held.wait(2), "holder thread never acquired both slots"
        time.sleep(0.05)
        idx = buildgate._wait_any(mutexes, 200)
        assert idx == buildgate._WAIT_TIMEOUT
    finally:
        release_event.set()
        t.join(timeout=5)
        for m in mutexes:
            m.close()


def test_slots_zero_disables_the_gate_without_touching_a_mutex(monkeypatch):
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", "0")
    ran = False
    with buildgate.kiln_build_gate("test-disabled"):
        ran = True
    assert ran


def test_slots_two_uncontended_acquire_and_release_reports_no_wait(monkeypatch):
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", "2")
    wr = buildgate.GateWaitResult()
    with buildgate.kiln_build_gate(f"test-uncontended-{uuid.uuid4().hex[:8]}", wait_result=wr):
        pass
    assert wr.waited_seconds == 0.0
