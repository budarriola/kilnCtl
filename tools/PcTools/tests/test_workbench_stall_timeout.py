"""_run's progress-based stall timeout with a ceiling (build_kilnfw)."""

import sys

import mcpkit.workbench as workbench

_PY = sys.executable


def _run(code, **kw):
    return workbench._run("stalltest", [_PY, "-u", "-c", code], poll_seconds=0.1, **kw)


def test_slow_but_progressing_build_is_not_killed():
    # Runs ~2.5 s total, but never silent for more than 0.5 s; stall limit 1.5 s.
    code = "import time\nfor i in range(5):\n print('step',i); time.sleep(0.5)"
    result = _run(code, timeout=60, stall_seconds=1.5)
    assert "stalltest: OK" in result
    assert "KILLED" not in result


def test_stalled_build_is_killed_with_reason():
    code = "import time\nprint('start')\ntime.sleep(60)"
    result = _run(code, timeout=60, stall_seconds=1.0)
    assert "TIMEOUT" in result
    assert "KILLED: stalled: no build output" in result


def test_ceiling_still_applies_to_a_progressing_build():
    code = "import time\nfor i in range(100):\n print('tick'); time.sleep(0.2)"
    result = _run(code, timeout=2, stall_seconds=30)
    assert "TIMEOUT" in result
    assert "absolute ceiling" in result


def _pid_alive(pid):
    from mcpkit.buildgate import pid_alive
    return pid_alive(pid)


def _spawn_parent_code(pidfile):
    return (
        "import subprocess, sys, time\n"
        "c = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])\n"
        f"open({str(pidfile)!r}, 'w').write(str(c.pid))\n"
        "print('start', flush=True)\n"
        "time.sleep(120)\n"
    )


def _wait_gone(pid, secs=10):
    import time
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if not _pid_alive(pid):
            return True
        time.sleep(0.2)
    return False


def test_stall_kill_takes_down_grandchildren(tmp_path):
    import os
    pidfile = tmp_path / "child.pid"
    result = _run(_spawn_parent_code(pidfile), timeout=60, stall_seconds=2.0)
    assert "KILLED: stalled" in result
    pid = int(pidfile.read_text())
    try:
        assert _wait_gone(pid), "grandchild survived the stall kill"
    finally:
        if _pid_alive(pid):
            os.kill(pid, 9)


def test_timeout_kill_takes_down_grandchildren(tmp_path):
    import os
    pidfile = tmp_path / "child.pid"
    result = workbench._run("tt", [_PY, "-u", "-c", _spawn_parent_code(pidfile)], timeout=3)
    assert "TIMEOUT" in result
    pid = int(pidfile.read_text())
    try:
        assert _wait_gone(pid), "grandchild survived the timeout kill"
    finally:
        if _pid_alive(pid):
            os.kill(pid, 9)


def test_build_ceiling_clamped_under_gate_hard_max(monkeypatch):
    monkeypatch.setenv("KILNCTL_BUILD_CEILING_S", "10800")
    monkeypatch.setenv("KILNCTL_BUILD_GATE_MAX_HOLD_HARD_SEC", "7200")
    assert workbench._build_ceiling_seconds() == 7140.0
