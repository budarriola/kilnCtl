"""Host tests for scripts/firing_readiness.py with fake step runners."""
from __future__ import annotations

import json
import os
import sys
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))

import firing_readiness as fr  # noqa: E402


def args(**kw):
    base = dict(host=None, soak_duration=60.0, soak_interval=10.0)
    base.update(kw)
    return types.SimpleNamespace(**base)


class Runner:
    def __init__(self, rcs):
        self.rcs = rcs
        self.calls = []

    def __call__(self, argv, timeout):
        self.calls.append(argv)
        return self.rcs[len(self.calls) - 1], "out"


OK = lambda host: (True, "fine")  # noqa: E731


def test_all_pass():
    r = Runner([0, 0])
    v = fr.run_readiness(args(), crash_check=OK, runner=r)
    assert v["verdict"] == "PASS"
    assert [s["status"] for s in v["steps"]] == ["PASS"] * 3
    assert "--firing-in-progress" in r.calls[0]
    assert "--i-am-aborting-a-real-firing" in r.calls[1]


def test_crash_present_skips_rest_and_runs_nothing():
    r = Runner([])
    v = fr.run_readiness(args(), crash_check=lambda h: (False, "crash"), runner=r)
    assert v["verdict"] == "FAIL"
    assert [s["status"] for s in v["steps"]] == ["FAIL", "skipped", "skipped"]
    assert r.calls == []


def test_soak_fail_never_runs_stopwatch():
    r = Runner([1])
    v = fr.run_readiness(args(), crash_check=OK, runner=r)
    assert v["verdict"] == "FAIL"
    assert [s["status"] for s in v["steps"]] == ["PASS", "FAIL", "skipped"]
    assert len(r.calls) == 1


def test_stopwatch_fail_is_overall_fail():
    v = fr.run_readiness(args(), crash_check=OK, runner=Runner([0, 1]))
    assert v["verdict"] == "FAIL"
    assert v["steps"][2]["status"] == "FAIL" and v["steps"][2]["rc"] == 1


def test_soak_setup_error_rc2_is_fail():
    v = fr.run_readiness(args(), crash_check=OK, runner=Runner([2]))
    assert v["verdict"] == "FAIL"


def test_crash_check_unreadable_is_fail():
    def boom(host):
        raise OSError("down")
    ok, detail = fr.check_crash_absent("h", get_crash_report=boom)
    assert not ok and "unreadable" in detail


def test_crash_check_cases():
    assert fr.check_crash_absent("h", get_crash_report=lambda h: {"present": False})[0]
    assert fr.check_crash_absent(
        "h", get_crash_report=lambda h: {"present": True, "acknowledged": True})[0]
    assert not fr.check_crash_absent(
        "h", get_crash_report=lambda h: {"present": True, "acknowledged": False})[0]
    assert not fr.check_crash_absent("h", get_crash_report=lambda h: None)[0]


def test_main_refuses_without_ack(tmp_path):
    assert fr.main(["--out-dir", str(tmp_path)]) == 2
    assert list(tmp_path.iterdir()) == []


def test_main_writes_verdict_json_and_exit_codes(tmp_path, monkeypatch):
    for verdict, rc in (("PASS", 0), ("FAIL", 1)):
        out = tmp_path / verdict
        monkeypatch.setattr(fr, "run_readiness",
                            lambda a, v=verdict: {"verdict": v, "started": "x", "steps": []})
        assert fr.main(["--i-am-aborting-a-real-firing", "--out-dir", str(out)]) == rc
        files = list(out.glob("*.json"))
        assert len(files) == 1
        assert json.loads(files[0].read_text())["verdict"] == verdict
