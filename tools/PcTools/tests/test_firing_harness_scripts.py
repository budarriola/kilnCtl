#!/usr/bin/env python3
"""Host tests for the two scripts the owner's next real firing depends on:
bench_firing_abort_stopwatch.py (30 s link-silence abort) and
stability_soak.py --firing-in-progress. Fake clients and a fake clock only;
no board, no serial port, no sleeping."""
from __future__ import annotations

import os
import sys
import types

import pytest

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


class FakePico:
    """Records halt/resume calls; scriptable failures."""

    def __init__(self, halt_ok=True, resume_ok=True, link_ok=True):
        self.halt_ok, self.resume_ok, self.link_ok = halt_ok, resume_ok, link_ok
        self.halts = 0
        self.resumes = 0

    def halt(self):
        self.halts += 1
        return self.halt_ok, "halted"

    def resume(self):
        self.resumes += 1
        return self.resume_ok, "resumed"

    def link_alive(self):
        return self.link_ok


def run_measure(fault_at=None, relays_off_at=None, unreadable_status=False,
                unreadable_relays=False, poll=1.0, pico=None, status_exc=None):
    """Board fake: FAULTED once clock >= t0+fault_at, relays off once
    clock >= t0+relays_off_at (None = never)."""
    clk = FakeClock()
    t0 = clk.t
    pico = pico or FakePico()

    def status():
        if status_exc is not None and clk.t - t0 >= 5.0:
            raise status_exc
        if unreadable_status:
            return None, None
        if fault_at is not None and clk.t - t0 >= fault_at:
            return sw.PROFILE_EXEC_FAULTED, 0
        return sw.PROFILE_EXEC_RUNNING, 0

    def relays():
        if unreadable_relays:
            return None
        return relays_off_at is not None and clk.t - t0 >= relays_off_at

    meas = sw.measure_abort(poll, clock=clk.now, sleep=clk.sleep,
                            halt=pico.halt, resume=pico.resume,
                            link_alive=pico.link_alive,
                            exec_status=status, relays_all_off=relays)
    return sw.judge_abort(meas["t0"], meas["t_faulted"], meas["t_relays_off"],
                          poll, sw.max_halt_s(poll) - sw.ABORT_THRESHOLD_S), meas


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


def test_relays_off_before_fault_then_on_is_not_a_pass():
    """Relays read off during the pre-abort window (PWM off-phase or the
    1.5 s link-fault refusal) and back ON after FAULTED must FAIL: only a
    reading at or after FAULTED confirms the abort dropped them."""
    clk = FakeClock()
    t0 = clk.t

    def status():
        if clk.t - t0 >= 30.0:
            return sw.PROFILE_EXEC_FAULTED, 0
        return sw.PROFILE_EXEC_RUNNING, 0

    pico = FakePico()
    meas = sw.measure_abort(1.0, clock=clk.now, sleep=clk.sleep,
                            halt=pico.halt, resume=pico.resume,
                            link_alive=pico.link_alive, exec_status=status,
                            relays_all_off=lambda: clk.t - t0 < 30.0)
    assert meas["t_relays_off"] is None
    ok, report = sw.judge_abort(meas["t0"], meas["t_faulted"], meas["t_relays_off"],
                                1.0, 6.0)
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
                         "fault_guard": 0, "halt_ok": True, "halt_detail": "",
                         "halt_call_s": 0.5, "halt_duration_s": 31.0,
                         "resume_ok": True, "resume_detail": "", "log": []})
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


# ----------------------------------------------- halt / resume safety net
def test_pass_path_halts_then_resumes_once_and_reports_duration():
    pico = FakePico()
    (ok, _), meas = run_measure(fault_at=30.0, relays_off_at=30.0, pico=pico)
    assert ok
    assert (pico.halts, pico.resumes) == (1, 1)
    assert meas["resume_ok"] is True
    assert 30.0 <= meas["halt_duration_s"] <= sw.max_halt_s(1.0)
    vok, report = sw.judge_run(meas, 1.0)
    assert vok
    assert any("halted for" in line for line in report)
    assert any("S6b" in line for line in report)  # expected-trip note, no auto-clear


def test_resume_on_exception():
    pico = FakePico()
    with pytest.raises(OSError):
        run_measure(fault_at=30.0, relays_off_at=30.0, pico=pico,
                    status_exc=OSError("link down"))
    assert pico.resumes == 1


def test_resume_on_keyboard_interrupt():
    pico = FakePico()
    with pytest.raises(KeyboardInterrupt):
        run_measure(fault_at=30.0, relays_off_at=30.0, pico=pico,
                    status_exc=KeyboardInterrupt())
    assert pico.resumes == 1


def test_resume_failure_is_fail_with_operator_message():
    pico = FakePico(resume_ok=False)
    emitted = []
    clk = FakeClock()
    meas = sw.measure_abort(1.0, clock=clk.now, sleep=clk.sleep, halt=pico.halt,
                            resume=pico.resume, link_alive=pico.link_alive,
                            exec_status=lambda: (sw.PROFILE_EXEC_FAULTED, 0),
                            relays_all_off=lambda: True, emit=emitted.append)
    assert meas["resume_ok"] is False
    assert sw.OPERATOR_RESUME_FAILED in emitted
    ok, report = sw.judge_run(meas, 1.0)
    assert not ok
    assert any("profiles_stop()" in line and "debug_reset" in line for line in report)


def test_resume_ok_but_link_never_answers_is_fail():
    pico = FakePico(link_ok=False)
    (_, _), meas = run_measure(fault_at=30.0, relays_off_at=30.0, pico=pico)
    assert meas["resume_ok"] is False
    assert not sw.judge_run(meas, 1.0)[0]


def test_halt_failure_is_fail_with_no_measurement():
    pico = FakePico(halt_ok=False)
    (_, _), meas = run_measure(fault_at=0.0, relays_off_at=0.0, pico=pico)
    assert meas["t_faulted"] is None and meas["log"] == []
    ok, report = sw.judge_run(meas, 1.0)
    assert not ok
    assert any("could not halt" in line for line in report)
    assert pico.resumes == 1  # a partial halt is still undone


def test_halt_is_bounded():
    """Never faults: the polling loop must stop at max_halt_s, not run on."""
    pico = FakePico()
    (_, _), meas = run_measure(fault_at=None, pico=pico)
    assert meas["halt_duration_s"] <= sw.max_halt_s(1.0) + 1.0
    assert meas["halt_duration_s"] >= sw.max_halt_s(1.0) - 3.0
    assert pico.resumes == 1
    assert sw.max_halt_s(1.0) == 38.0  # 30 + 2 fw watchdog + 1 poll + 5


def test_default_halt_names_both_cores(monkeypatch):
    calls = []
    monkeypatch.setattr(sw.m, "debug_halt",
                        lambda **k: calls.append(("halt", k)) or "halted pico",
                        raising=False)
    monkeypatch.setattr(sw.m, "debug_read_registers",
                        lambda **k: calls.append(("regs", k)) or "r0=0",
                        raising=False)
    assert sw._halt_pico()[0] is True
    assert calls[1][1]["target"] == "rp2040.core1" and calls[1][1]["leave_halted"]
    monkeypatch.setattr(sw.m, "debug_halt", lambda **k: "error: halt failed",
                        raising=False)
    assert sw._halt_pico()[0] is False


def test_abort_within_firmware_watchdog_period_passes():
    """The firmware evaluates the 30 s check only every 2 s
    (WATCHDOG_CHECK_PERIOD_MS), so a fault seen at 32.5 s with an instant halt
    call is correct firmware, not a FAIL."""
    ok, report = sw.judge_abort(0.0, 32.5, 32.5, 1.0, 7.0, halt_call_s=0.0)
    assert ok, report
    ok, _ = sw.judge_abort(0.0, 34.5, 34.5, 1.0, 7.0, halt_call_s=0.0)
    assert not ok


def test_sleep_never_overshoots_halt_bound():
    """A long poll period must not keep the Pico halted past max_halt_s."""
    pico = FakePico()
    (_, _), meas = run_measure(fault_at=None, pico=pico, poll=10.0)
    assert meas["halt_duration_s"] <= sw.max_halt_s(10.0)


class _Status:
    def __init__(self, link_up, age_ms, never=False):
        self.link_up, self.age_ms, self.never_received = link_up, age_ms, never


def test_link_alive_needs_fresh_link_up_not_just_an_answer(monkeypatch):
    """safety_get_status answers from the ESP cache with link_up=0 while the
    Pico is halted; that must NOT count as the Pico being back."""
    def use(st):
        monkeypatch.setattr(sw.m, "_safety",
                            types.SimpleNamespace(get_status=lambda: st))
    use(_Status(False, 40000))
    assert sw._link_alive() is False
    use(_Status(True, 5000))  # stale frame
    assert sw._link_alive() is False
    use(_Status(True, 0xFFFF, never=True))
    assert sw._link_alive() is False
    use(_Status(True, 120))
    assert sw._link_alive() is True

    def boom():
        raise OSError("uart")
    monkeypatch.setattr(sw.m, "_safety", types.SimpleNamespace(get_status=boom))
    assert sw._link_alive() is False


class _InterruptOncePico(FakePico):
    """First resume attempt is Ctrl-C'd (OpenOCD child killed with it)."""

    def resume(self):
        self.resumes += 1
        if self.resumes == 1:
            raise KeyboardInterrupt()
        return True, "resumed"


def test_keyboard_interrupt_during_resume_still_resumes_then_reraises():
    pico = _InterruptOncePico()
    clk = FakeClock()
    with pytest.raises(KeyboardInterrupt):  # held, then re-raised
        sw.measure_abort(1.0, clock=clk.now, sleep=clk.sleep,
                         halt=pico.halt, resume=pico.resume,
                         link_alive=pico.link_alive,
                         exec_status=lambda: (sw.PROFILE_EXEC_FAULTED, 0),
                         relays_all_off=lambda: True)
    assert pico.resumes == 2  # retried after the interrupted attempt


def test_keyboard_interrupt_during_resume_records_resume_ok():
    pico = _InterruptOncePico()
    clk = FakeClock()
    captured = []
    with pytest.raises(KeyboardInterrupt):
        sw.measure_abort(1.0, clock=clk.now, sleep=clk.sleep, halt=pico.halt,
                         resume=pico.resume, link_alive=pico.link_alive,
                         exec_status=lambda: (sw.PROFILE_EXEC_FAULTED, 0),
                         relays_all_off=lambda: True, emit=captured.append)
    assert any("resume pico -> ok=True" in line for line in captured)
    assert sw.OPERATOR_RESUME_FAILED not in captured


def test_resume_retries_are_bounded():
    pico = FakePico(resume_ok=False)
    (_, _), meas = run_measure(fault_at=30.0, relays_off_at=30.0, pico=pico)
    assert pico.resumes == sw.RESUME_ATTEMPTS >= 2
    assert meas["resume_ok"] is False


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


# ------------------------------------------------------- pico reboot mid-firing
import bench_pico_reboot_midfiring as pr  # noqa: E402


class FakeRebootBoard:
    """2 s link-down window after reset, then recovery (scriptable)."""

    def __init__(self, clk, running=True, down_s=2.0, recover=True,
                 relay_refused=True, firing_faults_at=None, trip_after=0,
                 relays=(1,)):
        self.clk, self.running, self.down_s, self.recover = clk, running, down_s, recover
        self.relay_refused, self.firing_faults_at = relay_refused, firing_faults_at
        self.trip_after, self.relays = trip_after, list(relays)
        self.t_reset = None
        self.resets = 0

    def _since(self):
        return None if self.t_reset is None else self.clk.t - self.t_reset

    def connect(self):
        return "connected"

    def exec_status(self):
        s = self._since()
        if not self.running:
            state = 0
        elif s is not None and self.firing_faults_at is not None and s >= self.firing_faults_at:
            state = pr.PROFILE_EXEC_FAULTED
        else:
            state = pr.PROFILE_EXEC_RUNNING
        return {"state": state, "segment": 1, "segment_elapsed_s": 5, "zone_temps": [500.0]}

    def link(self):
        s = self._since()
        if s is not None and s < self.down_s or (s is not None and not self.recover):
            return {"link_up": False, "age_ms": 3000}
        return {"link_up": True, "age_ms": 100}

    def pico_version(self):
        s = self._since()
        if s is None:
            return {"known": True, "commit": "abc", "boot_id": 1}
        if s < self.down_s or not self.recover:
            return {"known": False, "commit": "", "boot_id": 1}
        return {"known": True, "commit": "abc", "boot_id": 2}

    def counters(self):
        s = self._since()
        tr = self.trip_after if (s is not None and s >= self.down_s) else 0
        return {"trip_reason": tr, "trip_mask": 0}

    def relays_on(self):
        return self.relays

    def relay_on_refused(self, relay):
        return self.relay_refused

    def reset_pico(self):
        self.resets += 1
        self.t_reset = self.clk.t
        return "reset ok"


def run_reboot_main(board, clk, argv=("--i-am-rebooting-the-pico-mid-firing",)):
    return pr.main(list(argv), board=board, clock=clk.now, sleep=clk.sleep)


def test_pico_reboot_happy_path(tmp_path):
    clk = FakeClock()
    b = FakeRebootBoard(clk)
    out = tmp_path / "ev.json"
    rc = run_reboot_main(b, clk, ("--i-am-rebooting-the-pico-mid-firing", "--out", str(out)))
    assert rc == 0 and b.resets == 1
    assert out.exists()


def test_pico_reboot_requires_confirm_flag():
    clk = FakeClock()
    b = FakeRebootBoard(clk)
    assert run_reboot_main(b, clk, ()) == 2
    assert b.resets == 0


def test_pico_reboot_refuses_without_running_firing():
    clk = FakeClock()
    b = FakeRebootBoard(clk, running=False)
    assert run_reboot_main(b, clk) == 2
    assert b.resets == 0


def test_pico_reboot_link_never_recovers_fails():
    clk = FakeClock()
    assert run_reboot_main(FakeRebootBoard(clk, recover=False), clk) == 1


def test_pico_reboot_firing_aborts_fails():
    clk = FakeClock()
    assert run_reboot_main(FakeRebootBoard(clk, firing_faults_at=1.0), clk) == 1


def test_pico_reboot_relay_accepted_while_down_fails():
    clk = FakeClock()
    assert run_reboot_main(FakeRebootBoard(clk, relay_refused=False), clk) == 1


def test_pico_reboot_new_trip_fails():
    clk = FakeClock()
    assert run_reboot_main(FakeRebootBoard(clk, trip_after=7), clk) == 1
