#!/usr/bin/env python3
"""Host tests for the two scripts the owner's next real firing depends on:
bench_firing_abort_stopwatch.py (30 s link-silence abort) and
stability_soak.py --firing-in-progress. Fake clients and a fake clock only;
no board, no serial port, no sleeping."""
from __future__ import annotations

import os
import sys
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import bench_firing_abort_stopwatch as sw  # noqa: E402
import stability_soak as soak  # noqa: E402


# ---------------------------------------------------------------- stopwatch
class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


def run_measure(fault_at=None, relays_off_at=None, unreadable_status=False,
                unreadable_relays=False, poll=1.0, max_wait=60.0):
    """Board fake: FAULTED once clock >= t0+fault_at, relays off once
    clock >= t0+relays_off_at (None = never)."""
    clk = FakeClock()
    t0 = clk.t

    def silence():
        return "reset ok"

    def status():
        if unreadable_status:
            return None, None
        if fault_at is not None and clk.t - t0 >= fault_at:
            return sw.PROFILE_EXEC_FAULTED, 0
        return sw.PROFILE_EXEC_RUNNING, 0

    def relays():
        if unreadable_relays:
            return None
        return relays_off_at is not None and clk.t - t0 >= relays_off_at

    meas = sw.measure_abort(poll, max_wait, clock=clk.now, sleep=clk.sleep,
                            silence=silence, exec_status=status,
                            relays_all_off=relays)
    return sw.judge_abort(meas["t0"], meas["t_faulted"], meas["t_relays_off"],
                          poll, max_wait), meas


def test_abort_at_30s_passes():
    (ok, report), meas = run_measure(fault_at=30.0, relays_off_at=30.0)
    assert ok, report
    assert any("PASS" in line for line in report)
    assert 30.0 <= meas["t_faulted"] - meas["t0"] <= 31.0


def test_abort_slightly_late_within_window_passes():
    (ok, _), _ = run_measure(fault_at=31.0, relays_off_at=31.0)
    assert ok


def test_no_abort_fails():
    (ok, report), meas = run_measure(fault_at=None)
    assert not ok
    assert meas["t_faulted"] is None
    assert any("never reached FAULTED" in line for line in report)


def test_abort_too_early_flagged():
    (ok, report), _ = run_measure(fault_at=5.0, relays_off_at=5.0)
    assert not ok
    assert any(line.lstrip().startswith("FAIL") for line in report)


def test_abort_too_late_flagged():
    (ok, _), _ = run_measure(fault_at=45.0, relays_off_at=45.0)
    assert not ok


def test_relays_never_confirmed_off_fails():
    (ok, report), _ = run_measure(fault_at=30.0, relays_off_at=None)
    assert not ok
    assert any("never confirmed off" in line for line in report)


def test_unreadable_status_is_not_a_pass():
    (ok, _), meas = run_measure(unreadable_status=True, relays_off_at=0.0)
    assert not ok
    assert meas["t_faulted"] is None


def test_unreadable_relays_is_not_a_pass():
    (ok, _), _ = run_measure(fault_at=30.0, unreadable_relays=True)
    assert not ok


def test_threshold_constant_is_30s():
    assert sw.ABORT_THRESHOLD_S == 30.0


def test_exec_status_swallows_exception(monkeypatch):
    class Boom:
        def get_exec_status(self):
            raise OSError("link down")
    monkeypatch.setattr(sw.m, "_profiles", Boom())
    assert sw._exec_status() == (None, None)
    assert sw.require_running() is False


def test_require_running_accepts_only_running(monkeypatch):
    for state, want in ((1, True), (0, False), (2, False), (4, False)):
        st = types.SimpleNamespace(state=state, fault_guard=0)
        monkeypatch.setattr(sw.m, "_profiles",
                            types.SimpleNamespace(get_exec_status=lambda st=st: st))
        assert sw.require_running() is want


def test_relays_all_off_reads_each_relay(monkeypatch):
    class Io:
        def __init__(self, on):
            self.on = on

        def read(self):
            return self

        def relay(self, n):
            return n in self.on
    monkeypatch.setattr(sw.m, "_io", Io(set()))
    assert sw._relays_all_off() is True
    monkeypatch.setattr(sw.m, "_io", Io({3}))
    assert sw._relays_all_off() is False

    class Dead:
        def read(self):
            raise OSError
    monkeypatch.setattr(sw.m, "_io", Dead())
    assert sw._relays_all_off() is None


def _stopwatch_main(monkeypatch, argv, running=True):
    monkeypatch.setattr(sys, "argv", ["sw"] + argv)
    monkeypatch.setattr(sw.m, "connect", lambda: "connected")
    monkeypatch.setattr(sw, "require_running", lambda: running)
    called = []
    monkeypatch.setattr(sw, "measure_abort",
                        lambda *a, **k: called.append(1) or
                        {"t0": 0.0, "t_faulted": 30.5, "t_relays_off": 30.5,
                         "fault_guard": 0, "reset_result": "x", "log": []})
    return called


def test_main_refuses_without_ack_flag(monkeypatch):
    called = _stopwatch_main(monkeypatch, [])
    assert sw.main() == 2
    assert not called  # never silenced the link


def test_main_refuses_when_no_firing(monkeypatch):
    called = _stopwatch_main(monkeypatch, ["--i-am-aborting-a-real-firing"], running=False)
    assert sw.main() == 1
    assert not called


def test_main_pass_exit_code(monkeypatch):
    called = _stopwatch_main(monkeypatch, ["--i-am-aborting-a-real-firing"])
    assert sw.main() == 0
    assert called


# --------------------------------------------------------------------- soak
def es(state, name, guard=0):
    return types.SimpleNamespace(state=state, state_name=name, fault_guard=guard)


def test_preflight_idle_ok():
    assert soak.preflight_mode(es(0, "IDLE"), False) == (None, "idle")


def test_preflight_firing_without_flag_refused():
    for st in (1, 2):
        rc, _ = soak.preflight_mode(es(st, "RUNNING"), False)
        assert rc == 2


def test_preflight_firing_with_flag_proceeds():
    assert soak.preflight_mode(es(1, "RUNNING"), True) == (None, "firing-in-progress")


def test_preflight_flag_but_idle_warns_and_proceeds(capsys):
    rc, mode = soak.preflight_mode(es(0, "IDLE"), True)
    assert (rc, mode) == (None, "firing-in-progress")
    assert "WARNING" in capsys.readouterr().out


def good_row(t, heap=60000, crc=0, timeouts=0, bd=0, pct=40.0):
    return {"t_s": t, "heap_internal_free": heap, "crc_errors": crc,
            "timeouts": timeouts, "broadcast_dropped": bd,
            "stack_min_headroom_pct": pct, "stack_worst_task": "x"}


def verdict(rows, problems=None):
    return soak.summarize_and_verdict(rows, list(problems or []), "firing-in-progress",
                                      0.0, "x.csv")


def test_verdict_pass_flat():
    assert verdict([good_row(i * 60) for i in range(8)]) == 0


def test_verdict_fail_on_sample_problem():
    assert verdict([good_row(i * 60) for i in range(8)], ["t=0s: PROBLEM"]) == 1


def test_verdict_fail_on_climbing_counter():
    for name in ("crc", "timeouts", "bd"):
        rows = [good_row(i * 60) for i in range(8)]
        rows[-1] = good_row(420, **{name: 3})
        assert verdict(rows) == 1, name


def test_verdict_fail_on_heap_trend_down():
    rows = [good_row(i * 600, heap=80000 - i * 2000) for i in range(8)]
    assert verdict(rows) == 1


def test_verdict_fail_on_stack_trend_down():
    rows = [good_row(i * 600, pct=40.0 - i * 2.0) for i in range(8)]
    assert verdict(rows) == 1


def test_verdict_few_samples_not_a_trend_fail():
    rows = [good_row(i * 600, heap=80000 - i * 20000) for i in range(3)]
    assert verdict(rows) == 0


# --- main() end to end with a fake board
def _soak_main(monkeypatch, tmp_path, exec_status, samples, flag=True):
    monkeypatch.setattr(sys, "argv", ["soak", "--out", str(tmp_path / "o.csv"),
                                      "--max-samples", str(max(len(samples), 1)),
                                      "--interval", "0"]
                        + (["--firing-in-progress"] if flag else []))
    monkeypatch.setattr(soak.m, "connect", lambda: "connected")
    monkeypatch.setattr(soak, "_ota_resolve_host", lambda h: "1.2.3.4")

    def get():
        if isinstance(exec_status, Exception):
            raise exec_status
        return exec_status
    monkeypatch.setattr(soak.m, "_profiles", types.SimpleNamespace(get_exec_status=get))
    it = iter(samples)

    def fake_sample(host, baseline, t0):
        row, problems = next(it)
        return soak.Sample(row=row, problems=problems)
    monkeypatch.setattr(soak, "sample_once", fake_sample)
    monkeypatch.setattr(soak.time, "sleep", lambda s: None)


def _full(row):
    return {**{k: "" for k in soak.CSV_FIELDS}, **row}


def test_main_firing_pass(monkeypatch, tmp_path):
    samples = [(_full(good_row(i * 60)), []) for i in range(6)]
    _soak_main(monkeypatch, tmp_path, es(1, "RUNNING"), samples)
    assert soak.main() == 0
    assert (tmp_path / "o.csv").read_text().count("\n") >= 7


def test_main_firing_problem_fails(monkeypatch, tmp_path):
    samples = [(_full(good_row(i * 60)),
                ["profile executor fault_guard=3"] if i == 2 else [])
               for i in range(6)]
    _soak_main(monkeypatch, tmp_path, es(1, "RUNNING"), samples)
    assert soak.main() == 1


def test_main_exec_status_unreadable_is_setup_error(monkeypatch, tmp_path):
    _soak_main(monkeypatch, tmp_path, OSError("dead"), [])
    assert soak.main() == 2


def test_main_firing_without_flag_refused(monkeypatch, tmp_path):
    _soak_main(monkeypatch, tmp_path, es(1, "RUNNING"), [], flag=False)
    assert soak.main() == 2


# --- sample_once with fakes: the per-sample problem detection
class _Dead:
    def __getattr__(self, n):
        def f(*a, **k):
            raise OSError("dead")
        return f


def test_sample_once_flags_dead_everything(monkeypatch):
    monkeypatch.setattr(soak.m, "_info", _Dead())
    monkeypatch.setattr(soak.m, "_safety", _Dead())
    monkeypatch.setattr(soak.m, "_profiles", _Dead())

    def http(host):
        raise soak.dashboard_http_client.DashboardHttpError("down")
    monkeypatch.setattr(soak.dashboard_http_client, "get_heap_status", http)
    s = soak.sample_once("h", soak.Baseline(), 0.0)
    joined = " | ".join(s.problems)
    for needle in ("HTTP /api/status failed", "get_fw_version failed",
                   "get_stack_margin failed", "safety status/link_stats/diag failed",
                   "profiles_get_exec_status failed"):
        assert needle in joined


def test_sample_once_flags_fault_guard_and_crash_and_low_heap(monkeypatch):
    monkeypatch.setattr(soak.m, "_info", _Dead())
    monkeypatch.setattr(soak.m, "_safety", _Dead())
    monkeypatch.setattr(soak.m, "_profiles", types.SimpleNamespace(
        get_exec_status=lambda: es(1, "RUNNING", guard=7)))
    monkeypatch.setattr(soak.dashboard_http_client, "get_heap_status", lambda h: {
        "reset_reason": "poweron", "uptime_s": 5,
        "unacknowledged_crash": {"exc_task": "t", "exc_cause_str": "c"},
        "heap_internal": {"free": 1000, "min_free": 900}})
    s = soak.sample_once("h", soak.Baseline(), 0.0)
    joined = " | ".join(s.problems)
    assert "UNACKNOWLEDGED CRASH" in joined
    assert "below floor" in joined
    assert "fault_guard=7" in joined
