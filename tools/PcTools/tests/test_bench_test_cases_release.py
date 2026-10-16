#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_release (suite `release`).

Every board call is faked; these confirm the phase logic, never that a real
board answers a certain way.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_release.py -q
"""
from __future__ import annotations

import os
import sys
import types

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_heat as H  # noqa: E402
from kilnctrl.bench_test import cases_release as R  # noqa: E402
from kilnctrl.bench_test import cases_smoke as CS  # noqa: E402
from kilnctrl.bench_test import registry  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402


@pytest.fixture(autouse=True)
def _no_board_http(monkeypatch):
    def _down(*_a, **_k):
        raise OSError("board unreachable (no network in tests)")

    monkeypatch.setattr(H, "_http_get_json", _down)
    monkeypatch.setattr(CS, "_http_get_json", _down)
    monkeypatch.setattr(R, "_http_get_json", _down)


GOOD_SNAP = {
    "identity": {"fw_build": "b1", "fw_version": "1.0.0"},
    "running_partition": "app",
    "crash_unacked": False,
    "readiness": [("estop_verified", "not_done"), ("safety_trip", "ok")],
    "diag": {"state": 2},
    "trip_reason": 0,
    "liveness": "RESULT: ok",
    "stacks": {"t": {"hwm": 2000, "alive": True, "level": 0}},
    "stack_hwm": {"t": 2000},
    "heap_min_free": 20000,
    "boot_persisted": 0,
    "link_errors": 0,
    "link_crc_errors": 0,
    "link_timeouts": 0,
    "thermo": [(0, 24.0, True)],
    "wifi": {"connected": True, "ip": "10.0.0.5"},
}


def snap(**over):
    d = dict(GOOD_SNAP)
    d.update(over)
    return d


def ctx_with(snapshot, **extra):
    c = {"_rl_snapshot_fn": lambda _c: snapshot, "host": "10.0.0.5",
         "_board_identity_fn": lambda h: (None, "10.0.0.5")}
    c.update(extra)
    return c


def run_ids(ctx, ids):
    out = {}
    ctx["_run_results"] = out
    for cid in ids:
        out[cid] = get_case(cid).judge(ctx)
    return out


# -- registry ----------------------------------------------------------------


def test_suite_order_and_verdict_last():
    assert registry.SUITES["release"] == ["RL-A01", "RL-B01", "RL-C01", "RL-D01", "RL-E01", "RL-F01", "RL-V01"]


def test_only_heat_case_is_c01():
    assert [c for c in registry.SUITES["release"] if get_case(c).heat] == ["RL-C01"]


# -- pure judges -------------------------------------------------------------


def test_heap_floor_boundary():
    assert R.judge_heap_floor(8192).verdict == Verdict.PASS
    assert R.judge_heap_floor(8191).verdict == Verdict.FAIL
    assert R.judge_heap_floor(None).verdict == Verdict.INCONCLUSIVE
    assert R.judge_heap_floor(True).verdict == Verdict.INCONCLUSIVE


def test_persisted_boot_count():
    assert R.judge_persisted_boot_count(0).verdict == Verdict.PASS
    assert R.judge_persisted_boot_count(2).verdict == Verdict.FAIL
    assert R.judge_persisted_boot_count(None).verdict == Verdict.INCONCLUSIVE


def test_readiness_allows_only_estop():
    assert R.judge_readiness([("estop_verified", "not_done")]).verdict == Verdict.PASS
    assert R.judge_readiness([("safety_commissioned", "not_done")]).verdict == Verdict.FAIL


def test_test_trip_text():
    ok = "ok - test trip latched: trip_reason=4 trip_mask=0x0008 trip_seq=3 (host=x)."
    assert R.judge_test_trip_text(ok).verdict == Verdict.PASS
    assert R.judge_test_trip_text(ok.replace("0x0008", "0x0020")).verdict == Verdict.FAIL
    assert R.judge_test_trip_text(ok.replace("trip_reason=4", "trip_reason=6")).verdict == Verdict.FAIL
    assert R.judge_test_trip_text("REFUSED: confirm").verdict == Verdict.FAIL
    ev = "ok - test trip latched (confirmed by TRIP_EVENT: trip_reason=4 trip_seq=3; x)"
    assert R.judge_test_trip_text(ev).verdict == Verdict.PASS
    assert R.judge_test_trip_text(ev.replace("reason=4", "reason=6")).verdict == Verdict.FAIL


def test_compare_regressions():
    assert R.compare_final_to_baseline(snap(), snap()) == []
    assert R.compare_final_to_baseline(snap(), snap(heap_min_free=1000))
    assert R.compare_final_to_baseline(snap(), snap(link_crc_errors=5))
    assert R.compare_final_to_baseline(snap(), snap(stack_hwm={"t": 10}))


# -- phase A / abort ordering ------------------------------------------------


def test_phase_a_pass_sets_baseline_no_abort():
    c = ctx_with(snap())
    r = get_case("RL-A01").judge(c)
    assert r.verdict == Verdict.PASS, r.reason
    assert "_release_abort" not in c
    assert c["_rl_baseline"]["running_partition"] == "app"


@pytest.mark.parametrize("bad", [
    snap(running_partition="recovery"),
    snap(crash_unacked=True),
    snap(heap_min_free=4000),
    snap(boot_persisted=3),
    snap(diag={"state": 3}),
    snap(trip_reason=1),
])
def test_phase_a_fail_aborts_and_later_cases_not_run(bad):
    c = ctx_with(bad, allow_heat=True)
    out = run_ids(c, ["RL-A01", "RL-B01", "RL-C01", "RL-D01", "RL-E01", "RL-F01"])
    assert out["RL-A01"].verdict == Verdict.FAIL
    for cid in ("RL-B01", "RL-C01", "RL-D01", "RL-E01", "RL-F01"):
        assert out[cid].verdict == Verdict.NOT_RUN, cid


def test_phase_a_inconclusive_aborts():
    c = ctx_with(snap(heap_min_free=None))
    r = get_case("RL-A01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE
    assert c.get("_release_abort")
    assert get_case("RL-B01").judge(c).verdict == Verdict.NOT_RUN


def test_crash_unreadable_is_inconclusive_via_real_snapshot(monkeypatch):
    from kilnctrl import dashboard_http_client as dhc
    heap = {"heap_internal": {"min_free": 20000, "free": 30000}, "unacknowledged_crash": False,
            "unacknowledged_crash_check_error": "http 500"}
    from kilnctrl import ota_http_client as och
    monkeypatch.setattr(och, "get_boot_guard_status", lambda *a, **k: (_ for _ in ()).throw(OSError("x")))
    monkeypatch.setattr(dhc, "get_heap_status", lambda host: heap)
    s = R._read_snapshot({"host": "h", "srv": types.SimpleNamespace()})
    assert s["crash_unacked"] is None
    heap2 = dict(heap, unacknowledged_crash_check_error=None)
    s2 = R._read_snapshot({"host": "h", "srv": types.SimpleNamespace()})
    assert s2["crash_unacked"] is None  # still the same heap object error
    monkeypatch.setattr(dhc, "get_heap_status", lambda host: heap2)
    s3 = R._read_snapshot({"host": "h", "srv": types.SimpleNamespace()})
    assert s3["crash_unacked"] is False
    c = ctx_with(snap(crash_unacked=None))
    assert get_case("RL-A01").judge(c).verdict != Verdict.PASS


# -- phase B -----------------------------------------------------------------


class _Srv:
    def __init__(self, trip_text="ok - test trip latched: trip_reason=4 trip_mask=0x0008 trip_seq=1"):
        self.trip_text = trip_text
        self.trip_calls = []
        self.hosts = []
        self.clears = 0

    def safety_test_trip(self, confirm=False, host=None):
        self.trip_calls.append(confirm)
        self.hosts.append(host)
        return self.trip_text

    def safety_clear_trip(self):
        self.clears += 1
        self.diag = "trip_reason: 0 | trip_mask: 0x0000"
        return "ok"

    diag = "trip_reason: 4 | trip_mask: 0x0008"

    def safety_get_diag(self):
        return self.diag


def _readiness(ctx, items=None):
    items = items if items is not None else [("estop_verified", "not_done"), ("safety_trip", "ok")]
    ctx["_http_get_json"] = lambda h, p: (200, {"items": [{"key": k, "status": s} for k, s in items]})


def test_phase_b_pass_clears_and_checks_heat_permitted():
    srv = _Srv()
    c = ctx_with(snap(), srv=srv)
    _readiness(c)
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.PASS, r.reason
    assert srv.trip_calls == [True] and srv.clears >= 1


def test_phase_b_wrong_reason_fails_but_still_clears():
    srv = _Srv(trip_text="ok - test trip latched: trip_reason=6 trip_mask=0x0020 trip_seq=1")
    c = ctx_with(snap(), srv=srv)
    _readiness(c)
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.FAIL
    assert srv.clears >= 1
    assert c.get("_release_abort")


def test_phase_b_heat_not_permitted_after_clear_fails():
    srv = _Srv()
    c = ctx_with(snap(), srv=srv)
    _readiness(c, [("safety_trip", "not_done")])
    assert get_case("RL-B01").judge(c).verdict == Verdict.FAIL
    assert c.get("_release_abort")


def test_phase_b_tool_absent_skips():
    c = ctx_with(snap(), srv=types.SimpleNamespace())
    assert get_case("RL-B01").judge(c).verdict == Verdict.SKIP


# -- phase C -----------------------------------------------------------------


class _Zone:
    def __init__(self, z, actual, duty=0.5, on=True):
        self.zone, self.actual_c, self.duty, self.relay_commanded_on = z, actual, duty, on
        self.actual_valid = True


class _Status:
    def __init__(self, state, zones=(), target=40.0, dwelling=False):
        self.state_name, self.zones, self.target_c = state, list(zones), target
        self.dwelling, self.zone_mask = dwelling, 1


class _Profiles:
    def __init__(self, seq, stop_fails=False):
        self.seq = list(seq)
        self.stops = 0
        self.deleted = []
        self.stop_fails = stop_fails

    def get_exec_status(self):
        return self.seq.pop(0) if len(self.seq) > 1 else self.seq[0]

    def stop(self):
        self.stops += 1
        if self.stop_fails:
            raise OSError("down")
        return types.SimpleNamespace(ok=True)

    def delete(self, pid):
        self.deleted.append(pid)
        return types.SimpleNamespace(ok=True)


def _heat_ctx(profiles, heap=20000, **extra):
    t = {"v": 0.0}
    srv = types.SimpleNamespace(
        _profiles=profiles,
        _thermo=types.SimpleNamespace(read=lambda: [types.SimpleNamespace(channel=0, temperature_c=24.0, valid=True)]))
    extra.setdefault("_get_zones_config", lambda host: {"zones": [{"max_temp_c": 150.0}]})
    extra.setdefault("_board_identity_fn", lambda host: (None, "10.0.0.5"))
    return ctx_with(snap(), srv=srv, allow_heat=True, _rl_heap_fn=lambda _c: heap,
                    _now=lambda: t["v"], _sleep=lambda s: t.__setitem__("v", t["v"] + s), **extra)


@pytest.fixture
def started(monkeypatch):
    monkeypatch.setattr(H, "_start_bench_profile", lambda ctx, mask, **kw: (True, "", 24.0))


def test_heat_without_allow_heat_skips_and_touches_nothing(started):
    p = _Profiles([_Status("idle")])
    c = _heat_ctx(p)
    c["allow_heat"] = False
    r = get_case("RL-C01").judge(c)
    assert r.verdict == Verdict.SKIP and p.stops == 0 and p.deleted == []


def test_heat_pass_and_teardown_stops_and_deletes(started):
    seq = [_Status("running", [_Zone(0, 30)]), _Status("running", [_Zone(0, 40)], dwelling=True),
           _Status("done", [_Zone(0, 40, 0.0, False)]), _Status("idle")]
    p = _Profiles(seq)
    c = _heat_ctx(p)
    r = get_case("RL-C01").judge(c)
    assert r.verdict == Verdict.PASS, r.reason
    assert p.stops >= 1 and p.deleted
    assert not c.get("_release_abort")


def test_heat_fault_fails_but_teardown_still_stops(started):
    p = _Profiles([_Status("running", [_Zone(0, 30)]), _Status("faulted", [_Zone(0, 30, 0.0, False)]),
                   _Status("idle")])
    c = _heat_ctx(p)
    r = get_case("RL-C01").judge(c)
    assert r.verdict == Verdict.FAIL
    assert p.stops >= 1 and p.deleted


def test_heat_teardown_runs_when_status_raises(started):
    class P(_Profiles):
        def get_exec_status(self):
            raise OSError("link down")

    p = P([])
    with pytest.raises(OSError):
        get_case("RL-C01").judge(_heat_ctx(p))
    assert p.stops >= 1 and p.deleted


def test_heat_low_heap_during_firing_fails(started):
    seq = [_Status("running", [_Zone(0, 40)], dwelling=True), _Status("done", [_Zone(0, 40)]), _Status("idle")]
    c = _heat_ctx(_Profiles(seq), heap=4096)
    r = get_case("RL-C01").judge(c)
    assert r.verdict == Verdict.FAIL and "heap" in r.reason


def test_heat_teardown_cannot_stop_taints_and_aborts(started):
    p = _Profiles([_Status("running", [_Zone(0, 30)], dwelling=True)], stop_fails=True)
    c = _heat_ctx(p)
    get_case("RL-C01").judge(c)
    assert c.get("_tainted") is True and c.get("_release_abort")


def test_heat_identity_mismatch_refuses_before_any_write(monkeypatch):
    calls = []
    monkeypatch.setattr(H, "_start_bench_profile", lambda *a, **k: calls.append(1) or (True, "", 24.0))
    for target, ceil in (("kiln", 150.0), ("bench", 1300.0)):
        p = _Profiles([_Status("idle")])
        c = _heat_ctx(p, target=target, _get_zones_config=lambda h, v=ceil: {"zones": [{"max_temp_c": v}]})
        r = get_case("RL-C01").judge(c)
        assert r.verdict == Verdict.INCONCLUSIVE, target
    c = _heat_ctx(_Profiles([_Status("idle")]), _get_zones_config=lambda h: (_ for _ in ()).throw(OSError("x")))
    assert get_case("RL-C01").judge(c).verdict == Verdict.INCONCLUSIVE
    assert calls == []


def test_kiln_heat_runs_ceiling_precheck_with_explicit_target(monkeypatch):
    seen = []
    monkeypatch.setattr(H, "_check_zone_ceilings", lambda ctx, mask, t: seen.append(t) or (False, "ceiling low"))
    started = []
    monkeypatch.setattr(H, "_start_bench_profile", lambda *a, **k: started.append(1) or (True, "", 24.0))
    c = _heat_ctx(_Profiles([_Status("idle")]), target="kiln",
                  _get_zones_config=lambda h: {"zones": [{"max_temp_c": 1300.0}]})
    r = get_case("RL-C01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE and seen and started == []


# -- phase D -----------------------------------------------------------------


def _ota_srv(**over):
    cleared = []
    d = dict(
        update_check=lambda allow_prerelease=False, host=None: "ok - release v1.0.0-pre.9 available",
        update_fetch_status=lambda host=None: "ok idle",
        update_status=lambda host=None: "ok - phase=idle, busy=False, staged=False, header=none",
        update_stage_release=lambda confirm=False, allow_prerelease=False, host=None: "ok staged, verified by read-back sha256 abc",
        update_stage_clear=lambda confirm=False, host=None: cleared.append(confirm) or "ok cleared",
    )
    d.update(over)
    s = types.SimpleNamespace(**d)
    s.cleared = cleared
    return s


def test_ota_skips_when_tool_absent():
    s = _ota_srv()
    del s.update_stage_release
    assert get_case("RL-D01").judge(ctx_with(snap(), srv=s)).verdict == Verdict.SKIP


def test_ota_pass_clears_stage_by_default():
    s = _ota_srv()
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s))
    assert r.verdict == Verdict.PASS, r.reason
    assert s.cleared == [True]


def test_ota_stage_without_readback_claim_fails_and_still_clears():
    s = _ota_srv(update_stage_release=lambda confirm=False, allow_prerelease=False, host=None: "ok staged")
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s))
    assert r.verdict == Verdict.FAIL and s.cleared == [True]


def test_ota_install_requested_but_tool_absent_skips_and_keeps_stage():
    s = _ota_srv()
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s, ota_install=True))
    assert r.verdict == Verdict.SKIP and s.cleared == []  # skipped before anything was staged


def test_ota_failed_stage_clear_fails_and_taints():
    for bad in ("FAILED: boom", "REFUSED: busy", "UNKNOWN", "error: x"):
        s = _ota_srv(update_stage_clear=lambda confirm=False, host=None, b=bad: b)
        c = ctx_with(snap(), srv=s)
        r = get_case("RL-D01").judge(c)
        assert r.verdict == Verdict.FAIL, bad
        assert c.get("_tainted") is True, bad


def test_ota_raising_stage_clear_fails_and_taints():
    def boom(confirm=False, host=None):
        raise OSError("down")
    c = ctx_with(snap(), srv=_ota_srv(update_stage_clear=boom))
    assert get_case("RL-D01").judge(c).verdict == Verdict.FAIL
    assert c.get("_tainted") is True


def test_ota_prestaged_image_refuses_without_touching_it():
    s = _ota_srv(update_status=lambda host=None: "ok - phase=idle, busy=False, staged=True, header=x")
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s))
    assert r.verdict == Verdict.INCONCLUSIVE and s.cleared == []


def test_ota_unreadable_stage_state_refuses():
    s = _ota_srv(update_status=lambda host=None: "error: down")
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s))
    assert r.verdict == Verdict.INCONCLUSIVE and s.cleared == []


def test_ota_install_failed_clears_stage():
    s = _ota_srv(update_install_staged=lambda confirm=False, host=None: "REFUSED: nope")
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s, ota_install=True))
    assert r.verdict == Verdict.FAIL and s.cleared == [True]


def test_ota_records_target_and_install_flag():
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=_ota_srv(), target="kiln"))
    assert r.observed["target"] == "kiln" and r.observed["install_requested"] is False


def test_unknown_target_raises_and_kiln_honoured():
    with pytest.raises(ValueError):
        R._target({"target": "kilm"})
    assert R._target({"target": "kiln"}) == "kiln"
    assert R._target({}) == "bench"


def test_trip_fn_raise_aborts():
    class S(_Srv):
        def safety_test_trip(self, confirm=False, host=None):
            raise OSError("link")
    c = ctx_with(snap(), srv=S())
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.FAIL and c.get("_release_abort")


def test_non_test_trip_is_not_cleared():
    srv = _Srv()
    srv.diag = "trip_reason: 6 | trip_mask: 0x0020"
    c = ctx_with(snap(), srv=srv)
    _readiness(c)
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.FAIL and srv.clears == 0 and c.get("_release_abort")


def test_link_timeouts_alone_are_not_regression():
    base, fin = snap(link_crc_errors=0, link_errors=0), snap(link_crc_errors=0, link_errors=7)
    assert not [x for x in R.compare_final_to_baseline(base, fin) if "link" in x]
    fin2 = snap(link_crc_errors=3, link_errors=3)
    assert [x for x in R.compare_final_to_baseline(base, fin2) if "CRC" in x]


# -- phase F / verdict -------------------------------------------------------


def test_final_rerun_flags_regression():
    snaps = [snap(), snap(heap_min_free=9000, stack_hwm={"t": 2000})]
    c = {"_rl_snapshot_fn": lambda _c: snaps.pop(0), "host": "h"}
    out = run_ids(c, ["RL-A01", "RL-F01"])
    assert out["RL-A01"].verdict == Verdict.PASS
    assert out["RL-F01"].verdict == Verdict.FAIL
    assert "heap" in out["RL-F01"].reason


def test_final_rerun_clean_passes():
    c = {"_rl_snapshot_fn": lambda _c: snap(), "host": "h"}
    out = run_ids(c, ["RL-A01", "RL-F01"])
    assert out["RL-F01"].verdict == Verdict.PASS


def test_verdict_line_one_per_phase():
    c = ctx_with(snap())
    rows = {cid: types.SimpleNamespace(verdict=Verdict.PASS) for cid in R.PHASE_IDS}
    c["_run_results"] = rows
    assert get_case("RL-V01").judge(c).verdict == Verdict.PASS
    assert c["_release_verdict_line"].startswith("RELEASE VERDICT: RL-A01=PASS")
    rows["RL-C01"] = types.SimpleNamespace(verdict=Verdict.SKIP)
    assert get_case("RL-V01").judge(c).verdict == Verdict.INCONCLUSIVE
    rows["RL-B01"] = types.SimpleNamespace(verdict=Verdict.FAIL)
    assert get_case("RL-V01").judge(c).verdict == Verdict.FAIL


# -- REVIEW_RELSUITEFX fixes -------------------------------------------------


class _Cancel:
    def __init__(self):
        self.calls = []

    def __call__(self, confirm=False, host=None):
        self.calls.append(confirm)
        return "ok cancelled"


DIRTY = "ok - phase=downloading, busy=True, staged=False, header=none"


def _dirty_after_stage(**over):
    """update_status: empty before the stage call, dirty afterwards."""
    state = {"n": 0}
    cancel = _Cancel()

    def status(host=None):
        state["n"] += 1
        return DIRTY if state["n"] == 2 else "ok - phase=idle, busy=False, staged=False, header=none"

    kw = dict(update_status=status, update_fetch_cancel=cancel)
    kw.update(over)
    s = _ota_srv(**kw)
    s.cancel = cancel
    return s


def _raise_stage(confirm=False, allow_prerelease=False, host=None):
    raise OSError("link dropped mid download")


@pytest.mark.parametrize("stage", [
    _raise_stage,
    lambda confirm=False, allow_prerelease=False, host=None: "FAILED: sha mismatch",
    lambda confirm=False, allow_prerelease=False, host=None: "UNKNOWN",
])
def test_ota_stage_any_bad_outcome_cancels_then_clears(stage):
    s = _dirty_after_stage(update_stage_release=stage)
    c = ctx_with(snap(), srv=s)
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL
    assert s.cancel.calls == [True] and s.cleared == [True]
    assert not c.get("_tainted")


def test_ota_stage_raise_clear_failure_taints():
    s = _dirty_after_stage(update_stage_release=_raise_stage,
                           update_stage_clear=lambda confirm=False, host=None: "FAILED: busy")
    c = ctx_with(snap(), srv=s)
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and c.get("_tainted") is True


def test_ota_stage_raise_with_clean_reread_does_not_clear():
    s = _ota_srv(update_stage_release=_raise_stage)
    assert get_case("RL-D01").judge(ctx_with(snap(), srv=s)).verdict == Verdict.FAIL
    assert s.cleared == []


@pytest.mark.parametrize("status", [
    "ok - phase=idle, busy=True, staged=False, header=none",
    "ok - phase=idle, busy=False, staged=False, header=abc123",
])
def test_ota_precheck_requires_not_busy_and_no_header(status):
    s = _ota_srv(update_status=lambda host=None, st=status: st)
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s))
    assert r.verdict == Verdict.INCONCLUSIVE and s.cleared == []


def _install_ctx(snapfn, t):
    return {"_rl_snapshot_fn": snapfn, "host": "h", "ota_install": True, "_rl_baseline": snap(heap_min_free=30000),
            "_board_identity_fn": lambda h: (None, "10.0.0.5"),
            "srv": _ota_srv(update_install_staged=lambda confirm=False, host=None: "ok installed"),
            "_now": lambda: t["v"], "_sleep": lambda s: t.__setitem__("v", t["v"] + s)}


def test_ota_install_rebaselines_for_final_rerun():
    state = {"n": 0}

    def snapfn(_c):
        state["n"] += 1
        return snap(identity={"fw_build": "b1" if state["n"] == 1 else "b2", "fw_version": "1.0.1"},
                    heap_min_free=30000 if state["n"] == 1 else 20000)

    c = _install_ctx(snapfn, {"v": 0.0})
    assert get_case("RL-D01").judge(c).verdict == Verdict.PASS
    assert c["_rl_baseline"]["heap_min_free"] == 20000
    assert not R.compare_final_to_baseline(c["_rl_baseline"], snap(heap_min_free=20000))


def test_ota_install_board_gone_clears_baseline():
    t = {"v": 0.0}
    calls = {"n": 0}

    def snapfn(_c):
        calls["n"] += 1
        if calls["n"] > 1:
            raise OSError("down")
        return snap()

    c = _install_ctx(snapfn, t)
    get_case("RL-D01").judge(c)
    assert c["_rl_baseline"] is None


def test_link_timeout_growth_bound():
    assert 0 < R.LINK_TIMEOUT_GROWTH_BOUND <= 50  # documented small bound, not unbounded
    base = snap(link_timeouts=5)
    assert not [x for x in R.compare_final_to_baseline(base, snap(link_timeouts=5 + 20)) if "timeout" in x]
    assert [x for x in R.compare_final_to_baseline(base, snap(link_timeouts=5 + 1000)) if "timeout" in x]


def test_unreadable_link_counter_is_inconclusive():
    s = snap(link_crc_errors=None, _unreadable={"link_crc_errors": "OSError: x"})
    v, fails, inc = R.judge_snapshot(s)
    assert v == Verdict.INCONCLUSIVE and not fails and any("link counters" in i for i in inc)


def _id_ctx(ceilings, **extra):
    return _heat_ctx(_Profiles([_Status("idle")]),
                     _get_zones_config=lambda h: {"zones": [{"max_temp_c": v} for v in ceilings]}, **extra)


def test_identity_ceiling_boundary_200_vs_just_over():
    assert R._board_identity_problem(_id_ctx([200.0]), False, 1) is None
    assert R._board_identity_problem(_id_ctx([200.1]), False, 1) is not None
    assert R._board_identity_problem(_id_ctx([200.0]), True, 1) is not None
    assert R._board_identity_problem(_id_ctx([200.1]), True, 1) is None


def test_identity_only_heated_zones_count():
    c = _id_ctx([150.0, 1300.0, 150.0])
    assert R._board_identity_problem(c, False, 0b101) is None      # kiln-class zone 1 not heated
    assert R._board_identity_problem(c, False, 0b011) is not None  # zone 1 heated
    assert R._board_identity_problem(c, False, 0b1000) is not None  # heated zone has no ceiling


def test_identity_uart_http_mismatch_refuses():
    c = _id_ctx([150.0], _board_identity_fn=lambda h: ("different board", "1.2.3.4"))
    assert "mismatch" in R._board_identity_problem(c, False, 1)

    def boom(h):
        raise OSError("unreadable")
    c = _id_ctx([150.0], _board_identity_fn=boom)
    assert R._board_identity_problem(c, False, 1) is not None


def test_heat_uart_http_mismatch_is_inconclusive_and_writes_nothing(monkeypatch):
    calls = []
    monkeypatch.setattr(H, "_start_bench_profile", lambda *a, **k: calls.append(1) or (True, "", 24.0))
    c = _heat_ctx(_Profiles([_Status("idle")]), _board_identity_fn=lambda h: ("other board", None))
    assert get_case("RL-C01").judge(c).verdict == Verdict.INCONCLUSIVE and calls == []


@pytest.mark.parametrize("diag", [
    "trip_reason: 4 | trip_mask: 0x0020",   # reason right, mask wrong
    "trip_reason: 6 | trip_mask: 0x0008",   # reason wrong, mask right
])
def test_b01_partial_match_trip_is_not_cleared(diag):
    srv = _Srv()
    srv.diag = diag
    c = ctx_with(snap(), srv=srv)
    _readiness(c)
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.FAIL and srv.clears == 0 and c.get("_release_abort")


def test_b01_no_trip_in_diag_fails_even_if_tool_text_says_ok():
    srv = _Srv()
    srv.diag = "trip_reason: 0 | trip_mask: 0x0000"
    c = ctx_with(snap(), srv=srv)
    _readiness(c)
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.FAIL and srv.clears == 0 and c.get("_release_abort")


def test_b01_unparseable_diag_does_not_pass():
    srv = _Srv()
    srv.diag = "garbage"
    c = ctx_with(snap(), srv=srv)
    _readiness(c)
    assert get_case("RL-B01").judge(c).verdict != Verdict.PASS


# -- MED-1 / LOW-1: host threading and identity pin --------------------------


def test_b01_passes_host_and_pins_resolved_ip():
    srv = _Srv()
    c = ctx_with(snap(), srv=srv, host="kiln.local", _board_identity_fn=lambda h: (None, "10.9.9.9"))
    _readiness(c)
    assert get_case("RL-B01").judge(c).verdict == Verdict.PASS
    assert srv.hosts == ["10.9.9.9"] and c["host"] == "10.9.9.9"


@pytest.mark.parametrize("idfn", [
    lambda h: ("different board", "1.2.3.4"),
    lambda h: (_ for _ in ()).throw(OSError("uart down")),
])
def test_b01_identity_mismatch_or_unreadable_is_inconclusive_and_trips_nothing(idfn):
    srv = _Srv()
    c = ctx_with(snap(), srv=srv, _board_identity_fn=idfn)
    r = get_case("RL-B01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE and srv.trip_calls == []


def test_b01_no_host_is_inconclusive():
    srv = _Srv()
    c = ctx_with(snap(), srv=srv)
    c["host"] = None
    assert get_case("RL-B01").judge(c).verdict == Verdict.INCONCLUSIVE and srv.trip_calls == []


def _host_ota_srv(seen, **over):
    def rec(name, ret):
        def f(*a, host=None, **k):
            seen.append((name, host))
            return ret
        return f

    d = dict(
        update_check=rec("check", "ok - release available"),
        update_fetch_status=rec("fetch_status", "ok idle"),
        update_status=rec("status", "ok - phase=idle, busy=False, staged=False, header=none"),
        update_stage_release=rec("stage", "ok staged, verified by read-back sha256 abc"),
        update_stage_clear=rec("clear", "ok cleared"),
        update_fetch_cancel=rec("cancel", "ok cancelled"),
        update_install_staged=rec("install", "REFUSED: nope"),
    )
    d.update(over)
    return types.SimpleNamespace(**d)


def test_d01_every_update_call_carries_the_pinned_host():
    seen = []
    c = ctx_with(snap(), srv=_host_ota_srv(seen), host="kiln.local", ota_install=True,
                 _board_identity_fn=lambda h: (None, "10.9.9.9"))
    get_case("RL-D01").judge(c)
    names = {n for n, _h in seen}
    assert {"status", "check", "fetch_status", "stage", "install", "cancel", "clear"} <= names
    assert all(h == "10.9.9.9" for _n, h in seen), seen


def test_d01_identity_mismatch_touches_nothing():
    seen = []
    c = ctx_with(snap(), srv=_host_ota_srv(seen), _board_identity_fn=lambda h: ("other", "1.1.1.1"))
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE and seen == []


def test_d01_identity_rechecked_before_install():
    seen = []
    calls = {"n": 0}

    def idfn(h):
        calls["n"] += 1
        return (None, "10.0.0.5") if calls["n"] == 1 else ("swapped board", "10.0.0.5")

    c = ctx_with(snap(), srv=_host_ota_srv(seen), ota_install=True, _board_identity_fn=idfn)
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE and "install" not in {n for n, _h in seen}
    assert "clear" in {n for n, _h in seen}  # stage still cleaned up


# -- LOW-3: cleanup taints --------------------------------------------------


@pytest.mark.parametrize("cancel", [None, "FAILED: x", "raise"])
def test_d01_bad_cancel_taints(cancel):
    def raiser(confirm=False, host=None):
        raise OSError("down")

    over = {}
    if cancel == "raise":
        over["update_fetch_cancel"] = raiser
    elif cancel:
        over["update_fetch_cancel"] = lambda confirm=False, host=None, t=cancel: t
    s = _dirty_after_stage(**over)
    if cancel is None:
        del s.update_fetch_cancel
    c = ctx_with(snap(), srv=s)
    get_case("RL-D01").judge(c)
    assert c.get("_tainted") is True


def test_d01_stage_still_dirty_after_clear_taints_and_fails():
    state = {"n": 0}

    def status(host=None):
        state["n"] += 1
        return "ok - phase=idle, busy=False, staged=False, header=none" if state["n"] == 1 else DIRTY

    s = _ota_srv(update_status=status, update_fetch_cancel=_Cancel())
    c = ctx_with(snap(), srv=s)
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and c.get("_tainted") is True
    assert "after clear" in r.reason


def test_d01_unreadable_status_after_clear_taints():
    state = {"n": 0}

    def status(host=None):
        state["n"] += 1
        if state["n"] == 1:
            return "ok - phase=idle, busy=False, staged=False, header=none"
        if state["n"] == 2:
            return DIRTY
        raise OSError("down")

    s = _ota_srv(update_status=status, update_fetch_cancel=_Cancel())
    c = ctx_with(snap(), srv=s)
    assert get_case("RL-D01").judge(c).verdict == Verdict.FAIL and c.get("_tainted") is True


# -- LOW-4 / LOW-5: install -------------------------------------------------


def _inst_ctx(snaps, install, t=None):
    """snaps: list of snapshots / exceptions consumed one per call; last repeats."""
    t = t or {"v": 0.0}
    st = {"i": 0}

    def snapfn(_c):
        v = snaps[min(st["i"], len(snaps) - 1)]
        st["i"] += 1
        if isinstance(v, Exception):
            raise v
        return v

    seen = []
    s = _host_ota_srv(seen, update_install_staged=install)
    c = _install_ctx(snapfn, t)
    c["srv"] = s
    c["_seen"] = seen
    return c


def _ident(build, up=None):
    d = {"fw_build": build, "fw_version": "1"}
    if up is not None:
        d["uptime_s"] = up
    return d


def test_install_raise_has_own_label_and_never_cancels_or_clears():
    def boom(confirm=False, host=None):
        raise OSError("link dropped")

    c = _inst_ctx([snap(identity=_ident("b1", 500)), OSError("down"),
                   snap(identity=_ident("b2", 5))], boom)
    r = get_case("RL-D01").judge(c)
    names = [n for n, _h in c["_seen"]]
    assert r.verdict == Verdict.FAIL and "raised" in r.reason
    assert "cancel" not in names and "clear" not in names
    assert c["_rl_baseline"] is not None


def test_install_poll_ignores_pre_reboot_snapshot_without_confirmed_reboot():
    # same build, uptime rising: board never rebooted within the window.
    c = _inst_ctx([snap(identity=_ident("b1", 100))] + [snap(identity=_ident("b1", 100 + 10 * i)) for i in range(1, 40)],
                  lambda confirm=False, host=None: "ok installed")
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and "no confirmed reboot" in r.reason
    assert c["_rl_baseline"] is None


def test_install_uptime_drop_confirms_reboot_even_with_same_build():
    c = _inst_ctx([snap(identity=_ident("b1", 500)), snap(identity=_ident("b1", 12))],
                  lambda confirm=False, host=None: "ok installed")
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and "unchanged" in r.reason  # reboot seen, build same
    assert c["_rl_baseline"]["identity"]["uptime_s"] == 12


def test_install_down_then_up_confirms_reboot():
    c = _inst_ctx([snap(identity=_ident("b1")), OSError("down"), snap(identity=_ident("b2"))],
                  lambda confirm=False, host=None: "ok installed")
    assert get_case("RL-D01").judge(c).verdict == Verdict.PASS


@pytest.mark.parametrize("field,val", [
    ("link_crc_errors", None), ("link_timeouts", None), ("heap_min_free", None),
    ("link_crc_errors", True), ("link_timeouts", "3"),
])
def test_install_unreadable_baseline_field_is_inconclusive(field, val):
    bad = snap(identity=_ident("b2", 5))
    bad[field] = val
    bad.setdefault("link_timeouts", 0)
    c = _inst_ctx([snap(identity=_ident("b1", 500), link_timeouts=0), bad],
                  lambda confirm=False, host=None: "ok installed")
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE, r.reason


def test_bool_link_counter_is_not_a_valid_baseline():
    assert R._valid_baseline(snap(link_crc_errors=True, link_timeouts=0)) == "link_crc_errors"
    assert R._valid_baseline(snap(link_crc_errors=0, link_timeouts=0)) is None


def test_rl_ids_only_in_release_suite():
    from kilnctrl.bench_test.registry import suite_case_ids
    for suite in ("smoke", "flash", "stack", "ota", "autotune", "heat", "web", "lcd", "safety", "nightly", "full"):
        assert not [c for c in suite_case_ids(suite) if c.startswith("RL-")], suite
    assert [c for c in suite_case_ids("release") if c.startswith("RL-")]


def test_runner_refuses_release_without_confirm():
    from kilnctrl.bench_test.runner import BenchTestRunner
    for conf in (False, 1, "yes"):
        with pytest.raises(ValueError):
            BenchTestRunner({}).run(suite="release", confirm=conf)


def test_allow_heat_defaults_off_for_release(monkeypatch):
    from kilnctrl import mcp_server_bench_test as M
    seen = {}

    class FakeRunner:
        def __init__(self, ctx):
            pass

        def run(self, **kw):
            seen.update(kw)
            raise ValueError("stop")

    monkeypatch.setattr(M, "BenchTestRunner", FakeRunner)
    M.bench_test_run(suite="release", confirm=True)
    assert seen["allow_heat"] is False and seen["confirm"] is True
    seen.clear()
    M.bench_test_run(suite="smoke")
    assert seen["allow_heat"] is True


# -- RELSUITEFX3 fixes -------------------------------------------------------


def test_pin_identity_real_board_identity_check_ipv4address_keeps_port(monkeypatch):
    """MED-1/LOW-1: drive the REAL _board_identity_check (returns IPv4Address)."""
    import ipaddress
    from kilnctrl import mcp_server_aux as A
    wifi = types.SimpleNamespace(connected=True, ip="10.0.0.7")
    monkeypatch.setattr(A, "_srv", types.SimpleNamespace(_info=types.SimpleNamespace(get_wifi_status=lambda: wifi)))
    mism, ip = A._board_identity_check("10.0.0.7:8080")
    assert mism is None and isinstance(ip, ipaddress.IPv4Address)
    c = {"host": "10.0.0.7:8080"}
    assert R._pin_identity(c) is None
    assert c["host"] == "10.0.0.7:8080" and isinstance(c["host"], str)
    c2 = {"host": "10.0.0.7"}
    assert R._pin_identity(c2) is None and c2["host"] == "10.0.0.7"


def test_pin_identity_ipv4address_from_fake_is_pinned_as_str_with_port():
    import ipaddress
    c = {"host": "kiln.local:8080", "_board_identity_fn": lambda h: (None, ipaddress.ip_address("10.9.9.9"))}
    assert R._pin_identity(c) is None
    assert c["host"] == "10.9.9.9:8080"


@pytest.mark.parametrize("res", [
    "UNKNOWN: board not confirmed, the recovery image may still be applying",
    "FAILED: install recorded a failure",
    "FAILED: install refused or lost: reply dropped",
])
def test_install_post_sent_non_ok_never_cancels_or_clears(res):
    c = _inst_ctx([snap(identity=_ident("b1", 500)), OSError("down"), snap(identity=_ident("b2", 5))],
                  lambda confirm=False, host=None, r=res: r)
    r = get_case("RL-D01").judge(c)
    names = [n for n, _h in c["_seen"]]
    assert r.verdict == Verdict.FAIL
    assert "cancel" not in names and "clear" not in names
    assert c["_rl_baseline"] is not None


def test_install_post_sent_non_ok_without_reboot_still_skips_cleanup():
    t = {"v": 0.0}
    c = _inst_ctx([snap(identity=_ident("b1", 100 + i)) for i in range(60)],
                  lambda confirm=False, host=None: "UNKNOWN: mid-apply", t)
    r = get_case("RL-D01").judge(c)
    names = [n for n, _h in c["_seen"]]
    assert r.verdict == Verdict.FAIL and "cancel" not in names and "clear" not in names
    # RELSUITEFX4 INFO-2: the diagnostics are pinned: the observed flag and the install text in the reason.
    assert r.observed.get("install_non_ok") is True
    assert "UNKNOWN: mid-apply" in r.reason and "returned" in r.reason


@pytest.mark.parametrize("res", [
    "error: could not read GET /api/update/status (host=h): down", "REFUSED: not staged",
    "FAILED: install refused or lost: HTTP 409 busy"])
def test_install_pre_post_refusal_cleans_up(res):
    c = _inst_ctx([snap(identity=_ident("b1", 500))], lambda confirm=False, host=None, r=res: r)
    r = get_case("RL-D01").judge(c)
    names = [n for n, _h in c["_seen"]]
    assert r.verdict == Verdict.FAIL and "cancel" in names and "clear" in names


def test_install_post_reboot_judge_snapshot_fail_is_fail():
    """LOW-2 (M14)."""
    c = _inst_ctx([snap(identity=_ident("b1", 500)), snap(identity=_ident("b2", 5), crash_unacked=True)],
                  lambda confirm=False, host=None: "ok installed")
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and r.reason.startswith("after install:")


def test_install_post_reboot_unreadable_fields_are_inconclusive():
    """INFO-1."""
    c = _inst_ctx([snap(identity=_ident("b1", 500)), snap(identity=_ident("b2", 5), liveness=None)],
                  lambda confirm=False, host=None: "ok installed")
    assert get_case("RL-D01").judge(c).verdict == Verdict.INCONCLUSIVE


@pytest.mark.parametrize("text", [
    "error: staged=False, busy=False, header=none",
    "UNKNOWN: staged=False busy=False header=none",
])
def test_stage_empty_requires_ok_prefix(text):
    """LOW-3 (M11)."""
    assert R._stage_empty(text) is False
    assert R._stage_empty("ok - phase=idle, busy=False, staged=False, header=none") is True
    s = _ota_srv(update_status=lambda host=None, tx=text: tx)
    assert get_case("RL-D01").judge(ctx_with(snap(), srv=s)).verdict == Verdict.INCONCLUSIVE


def test_install_poll_deadline_is_300_seconds_on_fake_clock():
    """INFO-2 (M3)."""
    t = {"v": 0.0}
    n = {"c": 0}

    def snapfn(_c):
        n["c"] += 1
        return snap(identity=_ident("b1", 100 + t["v"]))

    c = _install_ctx(snapfn, t)
    c["srv"] = _host_ota_srv([], update_install_staged=lambda confirm=False, host=None: "ok installed")
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and "300 s" in r.reason
    assert 300.0 <= t["v"] <= 310.0 and n["c"] <= 33


def test_install_without_pre_install_fw_build_is_inconclusive_and_never_installs():
    """INFO-3 (M15)."""
    ran = []
    c = _inst_ctx([snap(identity={"fw_version": "1"}), snap(identity=_ident("b2", 5))],
                  lambda confirm=False, host=None: ran.append(1) or "ok installed")
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE and ran == []


@pytest.mark.parametrize("res", [
    "error: unexpected OSError: reset after the POST",
    "FAILED: install refused or lost: HTTP 500 internal error",
    "FAILED: install refused or lost: HTTP 503 busy",
    "FAILED: install refused or lost: reply dropped",
    "BUSY: another confirmed install call for this host is already in flight in this server (host=h)",
])
def test_install_non_definite_error_is_may_have_run_and_never_cleans_up(res):
    """RELSUITEFX4 LOW-1/LOW-4: only a pre-POST error or a definite 4xx lets cleanup run."""
    c = _inst_ctx([snap(identity=_ident("b1", 500)), OSError("down"), snap(identity=_ident("b2", 5))],
                  lambda confirm=False, host=None, r=res: r)
    r = get_case("RL-D01").judge(c)
    names = [n for n, _h in c["_seen"]]
    assert r.verdict == Verdict.FAIL and "cancel" not in names and "clear" not in names


def test_install_raised_then_unreadable_baseline_is_fail_not_inconclusive():
    """RELSUITEFX4 LOW-2: a raised install must not be lost behind an INCONCLUSIVE."""
    def boom(confirm=False, host=None):
        raise OSError("link dropped")

    bad = snap(identity=_ident("b2", 5), link_timeouts=None)
    c = _inst_ctx([snap(identity=_ident("b1", 500), link_timeouts=0), OSError("down"), bad], boom)
    r = get_case("RL-D01").judge(c)
    assert r.verdict == Verdict.FAIL and "raised" in r.reason and "unproven" in r.reason
