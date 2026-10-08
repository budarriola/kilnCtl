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


def test_light_lane_zero_disables_but_heavy_zero_is_ignored(monkeypatch):
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_SLOTS", "0")
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", "0")
    assert buildgate._slot_count("light") == 0
    assert buildgate._slot_count("heavy") == 4  # heavy 0 refused, config wins
    ran = False
    with buildgate.kiln_build_gate("test-disabled", lane="light"):
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
        pytest.fail("powershell not available -- parity tests must not skip")
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
    assert "acquired light slot" in r.stderr


def test_default_names_and_slot_counts_agree_between_python_and_powershell(monkeypatch):
    import shutil
    import subprocess

    ps = shutil.which("powershell")
    if ps is None:
        pytest.fail("powershell not available -- parity tests must not skip")
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
    assert buildgate._slot_count("heavy") == 4 and buildgate._slot_count("light") == 4
    assert buildgate._mutex_name(0, "heavy") != buildgate._mutex_name(0, "light")


# ---- 2026-10-07 gate rewrite: records, tickets, max hold, status, parity -----

import json  # noqa: E402
import shutil  # noqa: E402
import subprocess  # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
_GATE_PS1 = os.path.join(_REPO, "tools", "build_gate.ps1")
_PY = getattr(sys, "_base_executable", sys.executable)  # venv launcher would add a hop
_SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")


@pytest.fixture(autouse=True)
def _private_gate_dir(tmp_path, monkeypatch):
    """Never touch the machine-wide C:\\wt\\.buildgate from a unit test."""
    monkeypatch.setenv("KILNCTL_BUILD_GATE_DIR", str(tmp_path / "gate"))
    monkeypatch.delenv("KILNCTL_BUILD_GATE_HELD", raising=False)


def _prefixes(monkeypatch, heavy=2, light=2):
    tag = uuid.uuid4().hex[:12]
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", str(heavy))
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_SLOTS", str(light))
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MUTEX_PREFIX", f"Local\\kg_{tag}_h_")
    monkeypatch.setenv("KILNCTL_LIGHT_GATE_MUTEX_PREFIX", f"Local\\kg_{tag}_l_")


def _child(code, env=None, args=()):
    e = dict(os.environ if env is None else env)
    e["PYTHONPATH"] = _SRC
    return subprocess.Popen([_PY, "-c", code, *args], env=e, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)


_HOLD_CODE = (
    "import time\nfrom mcpkit import buildgate as b\n"
    "with b.kiln_build_gate('child'):\n"
    "    print('HELD', flush=True)\n    time.sleep(60)\n")


def test_config_is_authoritative_env_can_only_lower(monkeypatch, tmp_path):
    monkeypatch.delenv("KILNCTL_BUILD_GATE_SLOTS", raising=False)
    gd = tmp_path / "gate"
    gd.mkdir()
    (gd / "config.json").write_text('{"heavy_slots": 3, "light_slots": 1}')
    assert buildgate._slot_count("heavy") == 3
    assert buildgate._slot_count("light") == 1
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", "2")
    assert buildgate._slot_count("heavy") == 2  # lowering allowed
    monkeypatch.setenv("KILNCTL_BUILD_GATE_SLOTS", "8")
    assert buildgate._slot_count("heavy") == 3  # raising refused: clamped to config


def test_config_json_created_with_defaults_when_missing(monkeypatch, tmp_path):
    monkeypatch.delenv("KILNCTL_BUILD_GATE_SLOTS", raising=False)
    assert buildgate._slot_count("heavy") == 4
    assert json.loads((tmp_path / "gate" / "config.json").read_text()) == {"heavy_slots": 4, "light_slots": 4}


def test_slot_count_widens_to_live_record_index(monkeypatch):
    _prefixes(monkeypatch, heavy=2)
    d = buildgate._lane_dir("heavy")
    d.mkdir(parents=True)
    rec = {"pid": os.getpid(), "proc_start": buildgate.process_start_epoch(os.getpid()),
           "cmdline": "x", "label": "x", "lane": "heavy", "slot": 3, "phase": "compile",
           "started_epoch": time.time(), "worktree": "."}
    (d / "slot3.json").write_text(json.dumps(rec))
    assert buildgate._slot_count("heavy") == 4


def test_record_written_during_hold_and_removed_after(monkeypatch):
    _prefixes(monkeypatch)
    with buildgate.kiln_build_gate("rec-test", log=lambda m: None):
        recs = buildgate.read_records("heavy")
        assert len(recs) == 1 and recs[0]["pid"] == os.getpid() and recs[0]["label"] == "rec-test"
        assert recs[0]["alive"] and recs[0]["phase"] == "compile"
    assert buildgate.read_records("heavy") == []


def test_record_removed_when_body_raises(monkeypatch):
    _prefixes(monkeypatch)
    with pytest.raises(ValueError):
        with buildgate.kiln_build_gate("rec-raise", log=lambda m: None):
            raise ValueError("boom")
    assert buildgate.read_records("heavy") == []


def test_reentrant_same_thread_takes_one_slot(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    with buildgate.kiln_build_gate("outer", log=lambda m: None):
        with buildgate.kiln_build_gate("inner", timeout_seconds=1, log=lambda m: None):
            assert len(buildgate.read_records("heavy")) == 1
        assert len(buildgate.read_records("heavy")) == 1  # inner exit did not release
    assert buildgate.read_records("heavy") == []


def test_child_env_makes_child_process_reentrant(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    code = ("from mcpkit import buildgate as b\n"
            "with b.kiln_build_gate('child', timeout_seconds=2, log=print):\n    print('OK')\n")
    with buildgate.kiln_build_gate("parent", log=lambda m: None):
        p = _child(code, env=buildgate.child_env())
        out, err = p.communicate(timeout=30)
        assert "OK" in out and "re-entrant" in out, (out, err)
        # Without the env marker the single slot is taken -> child times out.
        p2 = _child(code)
        out2, err2 = p2.communicate(timeout=30)
        assert "OK" not in out2 and "TimeoutError" in err2


def test_no_record_exists_outside_the_with_block(monkeypatch):
    """Lock-wait / setup happen outside the gate: nothing is held then."""
    _prefixes(monkeypatch, heavy=1)
    assert buildgate.read_records("heavy") == []
    with buildgate.kiln_build_gate("x", log=lambda m: None):
        pass
    assert buildgate.read_records("heavy") == []


def test_abandoned_mutex_is_acquired_and_logged(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    keep = buildgate._KernelMutex(buildgate._mutex_name(0))  # an open handle is what makes a dead owner 'abandoned'
    p = _child(_HOLD_CODE)
    assert p.stdout.readline().strip() == "HELD"
    p.kill()
    p.wait(timeout=10)
    msgs = []
    with buildgate.kiln_build_gate("reclaim", timeout_seconds=10, log=msgs.append):
        pass
    assert any("ABANDONED" in m for m in msgs), msgs


def test_dead_pid_record_is_stale_and_ignored(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    d = buildgate._lane_dir("heavy")
    d.mkdir(parents=True)
    p = _child("pass")
    p.wait(timeout=30)
    rec = {"pid": p.pid, "proc_start": 1, "cmdline": "dead", "label": "dead", "lane": "heavy",
           "slot": 0, "phase": "compile", "started_epoch": time.time() - 100, "worktree": "."}
    (d / "slot0.json").write_text(json.dumps(rec))
    row = [r for r in buildgate.get_status() if r["lane"] == "heavy" and r["slot"] == 0][0]
    assert row["state"] == "stale" and row["pid_alive"] is False
    with buildgate.kiln_build_gate("ignores-stale", timeout_seconds=2, log=lambda m: None):
        pass  # not blocked by the stale record


def test_status_shows_live_holder_free_slot_and_compiler_child(monkeypatch, tmp_path):
    _prefixes(monkeypatch, heavy=2)
    exe = tmp_path / "cl.exe"  # a process whose image name matches the compiler regex
    shutil.copy(_PY, exe)
    code = ("import subprocess,sys,time\nfrom mcpkit import buildgate as b\n"
            "with b.kiln_build_gate('statusproc'):\n"
            "    subprocess.Popen([sys.argv[1], '-c', 'import time; time.sleep(30)'])\n"
            "    print('HELD', flush=True)\n    time.sleep(30)\n")
    p = _child(code, args=(str(exe),))
    try:
        assert p.stdout.readline().strip() == "HELD"
        time.sleep(0.5)
        rows = [r for r in buildgate.get_status() if r["lane"] == "heavy"]
        held = [r for r in rows if r["state"] == "held"]
        assert len(held) == 1 and held[0]["pid"] == p.pid and held[0]["pid_alive"]
        assert held[0]["age_sec"] >= 0
        assert held[0]["compiling"] is True and "cl.exe" in held[0]["compilers"]
        assert [r for r in rows if r["state"] == "free"]
    finally:
        buildgate._kill_descendants(p.pid)
        p.kill()
        p.wait(timeout=10)


def test_max_hold_releases_slot_and_fails_loud(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MAX_HOLD_SEC", "0.5")
    msgs = []
    with pytest.raises(RuntimeError, match=r"heavy slot 0 .*max hold"):
        with buildgate.kiln_build_gate("overrun", log=msgs.append):
            time.sleep(1.5)
    assert any("MAX HOLD EXCEEDED" in m for m in msgs)
    assert buildgate.read_records("heavy") == []
    with buildgate.kiln_build_gate("after", timeout_seconds=2, log=lambda m: None):
        pass  # slot really is free again


def test_max_hold_kills_only_registered_tree_not_sibling(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MAX_HOLD_SEC", "1.5")
    bystander = subprocess.Popen([_PY, "-c", "import time; time.sleep(30)"])
    code = ("import subprocess,sys,time\nfrom mcpkit import buildgate as b\n"
            "c = s = None\n"
            "try:\n"
            "  with b.kiln_build_gate('overrun2', log=lambda m: None):\n"
            "    c = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])\n"
            "    s = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])\n"
            "    b.register_compile_pid(c.pid)\n"
            "    print(c.pid, s.pid, flush=True)\n    time.sleep(5)\n"
            "except RuntimeError as e:\n"
            "  for _ in range(50):\n"
            "    if not b.pid_alive(c.pid): break\n"
            "    time.sleep(0.1)\n"
            "  print('RESULT', b.pid_alive(c.pid), b.pid_alive(s.pid), 'max hold' in str(e), flush=True)\n"
            "  b._kill_pid(s.pid)\n")
    holder = _child(code)
    sibling = 0
    try:
        cpid, sibling = (int(x) for x in holder.stdout.readline().split())
        out, err = holder.communicate(timeout=90)
        assert "RESULT False True True" in out, (out, err)  # compile killed, sibling alive, failed loud
        assert bystander.poll() is None  # unrelated process untouched
    finally:
        bystander.kill()
        if sibling:
            buildgate._kill_pid(sibling)


def test_max_hold_with_nothing_registered_kills_nothing(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MAX_HOLD_SEC", "0.5")
    kid = subprocess.Popen([_PY, "-c", "import time; time.sleep(30)"])
    msgs = []
    try:
        with pytest.raises(RuntimeError, match="max hold"):
            with buildgate.kiln_build_gate("noreg", log=msgs.append):
                time.sleep(1.5)
        assert any("killing nothing" in m for m in msgs)
        assert kid.poll() is None
    finally:
        kid.kill()


def test_unparsable_record_counts_as_held(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    d = buildgate._lane_dir("heavy")
    d.mkdir(parents=True)
    (d / "slot0.json").write_text("{not json")
    recs = buildgate.read_records("heavy")
    assert len(recs) == 1 and recs[0]["slot"] == 0 and recs[0]["alive"]


def test_waiters_never_kill_holder_and_all_get_through(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    holder = _child(_HOLD_CODE)
    assert holder.stdout.readline().strip() == "HELD"
    order_code = (
        "import sys\nfrom mcpkit import buildgate as b\n"
        "with b.kiln_build_gate(sys.argv[1], timeout_seconds=90, poll_interval_seconds=1, log=lambda m: None):\n"
        "    print('GOT', sys.argv[1], flush=True)\n")
    waiters = []
    try:
        for n in ("w1", "w2", "w3"):
            waiters.append(_child(order_code, args=(n,)))
            deadline = time.time() + 20
            while time.time() < deadline and len(buildgate.live_tickets("heavy")) < len(waiters):
                time.sleep(0.1)
            assert len(buildgate.live_tickets("heavy")) == len(waiters)
        time.sleep(1.5)
        assert holder.poll() is None  # waiters did not kill the holder
        holder.kill()
        holder.wait(timeout=10)
        for w in waiters:
            out, err = w.communicate(timeout=90)
            assert "GOT" in out, (out, err)
    finally:
        for w in waiters + [holder]:
            if w.poll() is None:
                w.kill()


def test_wake_order_follows_ticket_order(monkeypatch):
    """The head of the queue is the OLDEST live ticket."""
    _prefixes(monkeypatch, heavy=1)
    names = []
    for _ in range(3):
        names.append(buildgate._new_ticket("heavy", "t"))
        time.sleep(0.01)
    assert buildgate.live_tickets("heavy") == names
    for n in names:
        buildgate._remove_ticket("heavy", n)


def test_dead_waiter_ticket_is_pruned(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    p = _child("from mcpkit import buildgate as b\nb._new_ticket('heavy','ghost')\n")
    p.wait(timeout=30)
    assert len(list(buildgate._queue_dir("heavy").glob("*.ticket"))) == 1
    assert buildgate.live_tickets("heavy") == []
    assert list(buildgate._queue_dir("heavy").glob("*.ticket")) == []


def test_parity_ps_and_python_agree_on_config_status_and_records(monkeypatch, tmp_path):
    _prefixes(monkeypatch, heavy=2)
    monkeypatch.delenv("KILNCTL_BUILD_GATE_SLOTS")
    monkeypatch.delenv("KILNCTL_LIGHT_GATE_SLOTS")
    gd = tmp_path / "gate"
    gd.mkdir(exist_ok=True)
    (gd / "config.json").write_text('{"heavy_slots": 3, "light_slots": 2}')
    ps_script = (
        f". '{_GATE_PS1}'; "
        "Write-Output ('heavy=' + (Get-KilnBuildGateSlotCount -Lane heavy)); "
        "Write-Output ('light=' + (Get-KilnBuildGateSlotCount -Lane light)); "
        "Write-Output ('maxhold=' + (Get-KilnBuildGateMaxHoldSeconds)); "
        "$g = Enter-KilnBuildGate -Label 'ps-holder' -TimeoutSeconds 5; "
        "$rec = Get-Content (Get-KilnBuildGateRecordPath -Lane heavy -Slot $g.SlotIndex) -Raw; "
        "Write-Output ('REC=' + $rec); "
        "Start-Sleep -Seconds 6; Exit-KilnBuildGate -Gate $g")
    p = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=os.environ.copy())
    try:
        deadline = time.time() + 30
        recs = []
        while time.time() < deadline and not recs:
            recs = buildgate.read_records("heavy")
            time.sleep(0.1)
        assert recs, "python cannot read the record the PowerShell holder wrote"
        assert recs[0]["label"] == "ps-holder" and recs[0]["alive"] and recs[0]["slot"] == 0
        # Python sees PS's slot as busy: it must take another one.
        with buildgate.kiln_build_gate("py-after-ps", timeout_seconds=5, log=lambda m: None):
            assert sorted(r["slot"] for r in buildgate.read_records("heavy")) == [0, 1]
            py_keys = set(json.loads(buildgate._record_path("heavy", 1).read_text()))
        out, err = p.communicate(timeout=60)
    finally:
        if p.poll() is None:
            p.kill()
    assert "heavy=3" in out and "light=2" in out and "maxhold=2700" in out, (out, err)
    assert buildgate._slot_count("heavy") == 3 and buildgate._slot_count("light") == 2
    assert buildgate.read_records("heavy") == []  # PS removed its record on release
    rec = json.loads([l for l in out.splitlines() if l.startswith("REC=")][0][4:])
    assert set(rec) == py_keys



def test_ps_max_hold_kills_children_started_after_acquire_not_older_sibling(monkeypatch):
    _prefixes(monkeypatch, heavy=1)
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MAX_HOLD_SEC", "2")
    ps_script = (
        f". '{_GATE_PS1}'; "
        "$older = Start-Process -PassThru -WindowStyle Hidden ping -ArgumentList '-n','40','127.0.0.1'; "
        "Start-Sleep -Seconds 2; "
        "$g = Enter-KilnBuildGate -Label 'ps-overrun' -TimeoutSeconds 5; "
        "$newer = Start-Process -PassThru -WindowStyle Hidden ping -ArgumentList '-n','40','127.0.0.1'; "
        "Start-Sleep -Seconds 6; "
        "$msg = ''; try { Exit-KilnBuildGate -Gate $g } catch { $msg = $_.Exception.Message }; "
        "Write-Output ('RESULT newer_alive=' + (-not $newer.HasExited) + ' older_alive=' + (-not $older.HasExited) + ' threw=' + ($msg -match 'max hold')); "
        "Stop-Process -Id $older.Id -Force -ErrorAction SilentlyContinue")
    p = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
                       capture_output=True, text=True, env=os.environ.copy(), timeout=120)
    assert "RESULT newer_alive=False older_alive=True threw=True" in p.stdout, (p.stdout, p.stderr)
