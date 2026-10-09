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
    # kiln_build_gate's real mutex prefix is Global\ -- shared machine-wide
    # with any concurrent real build. Point this test at a private, unique
    # Local\ prefix instead so it can never flake while a real build holds
    # the Global\ slots (opus review of 171cc5bc, advisory 3).
    monkeypatch.setenv(
        "KILNCTL_BUILD_GATE_MUTEX_PREFIX",
        f"Local\\kilnctl_buildgate_test_{uuid.uuid4().hex[:12]}_slot_")
    wr = buildgate.GateWaitResult()
    with buildgate.kiln_build_gate(f"test-uncontended-{uuid.uuid4().hex[:8]}", wait_result=wr):
        pass
    assert wr.waited_seconds == 0.0


# ---- light lane (2026-10-05) ------------------------------------------------
# A seconds-long compile must never queue behind multi-minute heavy builds.

def _private_prefixes(monkeypatch):
    tag = uuid.uuid4().hex[:12]
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", "2")
    # 2 (not the default 4) so that, if the light lane were ever pointed at the
    # heavy prefix, it would see exactly the two held slots and the tests
    # below would fail -- with 4 it would just take the unused slots 2 and 3.
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_SLOTS", "2")
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MUTEX_PREFIX", f"Local\\kilnctl_buildgate_test_{tag}_heavy_")
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_MUTEX_PREFIX", f"Local\\kilnctl_buildgate_test_{tag}_light_")


class _HeavyHolder:
    """Holds every heavy slot from a separate thread (mutexes are per-thread)."""

    def __enter__(self):
        self.held = threading.Event()
        self.release = threading.Event()

        def _run():
            ms = [buildgate._KernelMutex(buildgate._mutex_name(i)) for i in range(2)]
            for m in ms:
                assert m.wait(2000) in (buildgate._WAIT_OBJECT_0, buildgate._WAIT_ABANDONED_0)
            self.held.set()
            self.release.wait(30)
            for m in ms:
                m.release()
                m.close()

        self.t = threading.Thread(target=_run, daemon=True)
        self.t.start()
        assert self.held.wait(5), "holder never took both heavy slots"
        return self

    def __exit__(self, *a):
        self.release.set()
        self.t.join(timeout=5)


def test_light_lane_acquires_while_both_heavy_slots_are_held(monkeypatch):
    _private_prefixes(monkeypatch)
    with _HeavyHolder():
        # Sanity: the heavy lane really is saturated.
        with pytest.raises(TimeoutError):
            with buildgate.kiln_build_gate("heavy-blocked", timeout_seconds=0.5, poll_interval_seconds=0.25):
                pass
        wr = buildgate.GateWaitResult()
        with buildgate.kiln_build_gate("light-ok", lane="light", timeout_seconds=2,
                                       poll_interval_seconds=0.25, wait_result=wr):
            pass
        assert wr.waited_seconds == 0.0


def test_light_lane_has_its_own_bounded_slot_count(monkeypatch):
    _private_prefixes(monkeypatch)
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_SLOTS", "1")
    held = threading.Event()
    release = threading.Event()

    def _run():
        with buildgate.kiln_build_gate("light-holder", lane="light"):
            held.set()
            release.wait(10)

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    try:
        assert held.wait(5)
        with pytest.raises(TimeoutError):
            with buildgate.kiln_build_gate("light-second", lane="light",
                                           timeout_seconds=0.5, poll_interval_seconds=0.25):
                pass
    finally:
        release.set()
        t.join(timeout=5)


def test_powershell_light_lane_acquires_while_both_heavy_slots_are_held(monkeypatch):
    import shutil
    import subprocess

    ps = shutil.which("powershell")
    if ps is None:
        pytest.skip("powershell not available")
    _private_prefixes(monkeypatch)
    gate_ps1 = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
        "tools", "build_gate.ps1")
    script = (
        f". '{gate_ps1}'; "
        "$g = Enter-KilnBuildGate -Label 'ps-light-test' -Lane light -TimeoutSeconds 3 -PollIntervalSeconds 1; "
        "Exit-KilnBuildGate -Gate $g; Write-Output 'LIGHT-ACQUIRED'"
    )
    with _HeavyHolder():
        r = subprocess.run([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                           capture_output=True, text=True, timeout=60, env=os.environ.copy())
    assert r.returncode == 0 and "LIGHT-ACQUIRED" in r.stdout, (r.stdout, r.stderr)
    assert "lane=light" in r.stderr


def test_default_names_and_slot_counts_agree_between_python_and_powershell(monkeypatch):
    import shutil
    import subprocess

    ps = shutil.which("powershell")
    if ps is None:
        pytest.skip("powershell not available")
    for name in ("KILNCTL_BUILD_GATE_MUTEX_PREFIX", "KILNCTL_LIGHT_GATE_MUTEX_PREFIX",
                 "KILNCTL_BUILD_GATE_SLOTS", "KILNCTL_LIGHT_GATE_SLOTS"):
        monkeypatch.delenv(name, raising=False)
    gate_ps1 = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
        "tools", "build_gate.ps1")
    script = (
        f". '{gate_ps1}'; "
        "foreach ($l in 'heavy','light') { "
        "Write-Output (\"$l slots=\" + (Get-KilnBuildGateSlotCount -Lane $l)); "
        "foreach ($i in 0,1) { Write-Output (\"$l name$i=\" + (Get-KilnBuildGateMutexName -SlotIndex $i -Lane $l)) } }"
    )
    r = subprocess.run([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                       capture_output=True, text=True, timeout=60, env=os.environ.copy())
    assert r.returncode == 0, r.stderr
    for lane in ("heavy", "light"):
        lines = [l.strip() for l in r.stdout.splitlines() if l.startswith(lane + " ")]
        assert f"{lane} slots={buildgate._slot_count(lane)}" in lines
        for i in (0, 1):
            assert f"{lane} name{i}={buildgate._mutex_name(i, lane)}" in lines
    assert buildgate._slot_count("heavy") == 2 and buildgate._slot_count("light") == 4
    assert buildgate._mutex_name(0, "heavy") != buildgate._mutex_name(0, "light")
