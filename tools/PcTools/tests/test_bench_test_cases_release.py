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
    "thermo": [(0, 24.0, True)],
    "wifi": {"connected": True, "ip": "10.0.0.5"},
}


def snap(**over):
    d = dict(GOOD_SNAP)
    d.update(over)
    return d


def ctx_with(snapshot, **extra):
    c = {"_rl_snapshot_fn": lambda _c: snapshot, "host": "10.0.0.5"}
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
    assert R.compare_final_to_baseline(snap(), snap(link_errors=5))
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


def test_phase_a_inconclusive_does_not_abort():
    c = ctx_with(snap(heap_min_free=None))
    r = get_case("RL-A01").judge(c)
    assert r.verdict == Verdict.INCONCLUSIVE
    assert "_release_abort" not in c


# -- phase B -----------------------------------------------------------------


class _Srv:
    def __init__(self, trip_text="ok - test trip latched: trip_reason=4 trip_mask=0x0008 trip_seq=1"):
        self.trip_text = trip_text
        self.trip_calls = []
        self.clears = 0

    def safety_test_trip(self, confirm=False):
        self.trip_calls.append(confirm)
        return self.trip_text

    def safety_clear_trip(self):
        self.clears += 1
        return "ok"

    def safety_get_diag(self):
        return "trip_reason: 0 | trip_mask: 0x0000"


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


# -- phase D -----------------------------------------------------------------


def _ota_srv(**over):
    cleared = []
    d = dict(
        update_check=lambda allow_prerelease=False: "ok - release v1.0.0-pre.9 available",
        update_fetch_status=lambda: "ok idle",
        update_stage_release=lambda confirm=False, allow_prerelease=False: "ok staged, verified by read-back sha256 abc",
        update_stage_clear=lambda confirm=False: cleared.append(confirm) or "ok cleared",
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
    s = _ota_srv(update_stage_release=lambda confirm=False, allow_prerelease=False: "ok staged")
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s))
    assert r.verdict == Verdict.FAIL and s.cleared == [True]


def test_ota_install_requested_but_tool_absent_skips_and_keeps_stage():
    s = _ota_srv()
    r = get_case("RL-D01").judge(ctx_with(snap(), srv=s, ota_install=True))
    assert r.verdict == Verdict.SKIP and s.cleared == []


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
