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
