"""RL suite -- the unattended release test set (docs/BENCH_TEST_SYSTEM_PLAN.md,
section "Release suite").

Runs on the bench fixture or on a real kiln with nobody present, through the
normal engine: ``bench_test_run(suite="release", ...)`` /
``bench_test_start(suite="release", ...)``. No new MCP tool.

Phases, in order (each phase is one case; RL-V01 closes the run):

* RL-A01 health baseline, read-only. A FAIL here sets ``ctx["_release_abort"]``
  and every later phase reports NOT_RUN: nothing heats and nothing flashes on
  a board that is not healthy to begin with.
* RL-B01 safety: ``safety_test_trip`` then clear. Never calls ``estop_verify``
  and never disables a guard.
* RL-C01 short heat profile (heat case: SKIP without ``allow_heat=True``,
  never PASS). Teardown always stops the firing and deletes the profile.
* RL-D01 OTA without a PC download: GitHub check, stage, sha256 read-back,
  then clear the stage (or install when ``ota_install=True`` and the
  ``update_install_staged`` tool exists).
* RL-E01 web and LCD smoke, read-only (composes existing cases).
* RL-F01 final health: phase A again, compared against the phase A baseline.
* RL-V01 verdict: one line per phase.

ctx keys: ``target`` ("bench" default, or "kiln"), ``allow_heat``,
``ota_install``. Test seams: ``_rl_snapshot_fn(ctx) -> dict`` replaces the
board reads of phases A and F.
"""
from __future__ import annotations

import re
import time as _time
from typing import Any, Dict, List, Optional, Tuple

from . import cases_heat as H
from . import judgments as J
from .cases_smoke import _http_get_json, _srv, wait_for_trip_clear
from .registry import CaseResult, SUITES, Verdict, _c, get_case, register

#: Owner decision 2026-10-01: internal heap min_free must stay at or above 8 KB.
HEAP_MIN_FREE_FLOOR_B = 8192
#: A final reading may sit this far under the baseline before it is a regression.
STACK_REGRESSION_TOLERANCE_B = 64
HEAP_REGRESSION_TOLERANCE_B = 512
#: Targets.
TARGETS = ("bench", "kiln")
#: Readiness keys that do not block a release verdict by themselves.
READINESS_ALLOWED_NOT_OK = {"estop_verified"}

#: Heat profile per target: (offset above ambient or absolute target, ramp
#: C/h, dwell min, overall timeout s).
BENCH_HEAT_OFFSET_C = 12.0
BENCH_HEAT_DWELL_MIN = 3
BENCH_HEAT_TIMEOUT_S = 600.0
KILN_HEAT_TARGET_C = 100.0
KILN_HEAT_RAMP_C_PER_HR = 300.0
KILN_HEAT_DWELL_MIN = 5
KILN_HEAT_TIMEOUT_S = 2700.0
HEAT_POLL_S = 10.0
#: Allowed deviation of a zone from its setpoint during the hold.
HOLD_TRACK_BAND_C = 8.0

PHASE_IDS = ["RL-A01", "RL-B01", "RL-C01", "RL-D01", "RL-E01", "RL-F01"]

_WEB_SMOKE = ["WEB-DASH-01", "WEB-PROF-01", "WEB-ZONE-01", "WEB-SAF-01", "WEB-DIAG-01", "WEB-OTA-01"]
_LCD_SMOKE = ["LCD-07", "LCD-15"]


def _c_rl(cid: str, desc: str, **kw):
    return register(_c(cid, "RL", desc, **kw))


_c_rl("RL-A01", "Release phase A: health baseline (identity, crash, readiness, safety, tasks, stacks, heap, boot_guard, thermo, Wi-Fi)",
      est_duration_s=60)
_c_rl("RL-B01", "Release phase B: safety_test_trip latches reason 4 / mask 0x0008, clear restores heat permission", est_duration_s=60)
_c_rl("RL-C01", "Release phase C: short heat profile reaches DONE with zones tracking, relays switching, heap floor held",
      heat=True, heat_skip_reason="allow_heat=False (release heat phase)", est_duration_s=900)
_c_rl("RL-D01", "Release phase D: GitHub check, stage release, sha256 read-back, stage clear (install only on request)",
      est_duration_s=300)
_c_rl("RL-E01", "Release phase E: web pages render, LCD panel non-dark (read-only)", est_duration_s=120)
_c_rl("RL-F01", "Release phase F: final health compared against the phase A baseline", est_duration_s=60)
_c_rl("RL-V01", "Release verdict: PASS/FAIL/SKIP/INCONCLUSIVE per phase", est_duration_s=1)

SUITES["release"] = [*PHASE_IDS, "RL-V01"]
from . import registry as _registry  # noqa: E402

_registry._enforce_always_last()


# -- small helpers ---------------------------------------------------------


def _target(ctx: dict) -> str:
    t = ctx.get("target", "bench")
    if t not in TARGETS:
        raise ValueError(f"unknown release target {t!r}; expected one of {sorted(TARGETS)}")
    return t


def _aborted(ctx: dict) -> Optional[CaseResult]:
    why = ctx.get("_release_abort")
    if why:
        return CaseResult(Verdict.NOT_RUN, reason=f"release run stopped: {why}")
    return None


def _abort(ctx: dict, why: str) -> None:
    ctx.setdefault("_release_abort", why)


def _hostname(ctx: dict) -> Optional[str]:
    return ctx.get("host")


def _get_json(ctx: dict, path: str):
    host = _hostname(ctx)
    if not host:
        raise RuntimeError("no host in ctx")
    fn = ctx.get("_http_get_json", _http_get_json)
    return fn(host, path)


# -- pure judges -----------------------------------------------------------


def judge_heap_floor(min_free: Optional[int], floor: int = HEAP_MIN_FREE_FLOOR_B) -> CaseResult:
    """Internal heap low-water mark must be at or above the owner floor."""
    if not isinstance(min_free, int) or isinstance(min_free, bool):
        return CaseResult(Verdict.INCONCLUSIVE, reason="internal heap min_free unreadable",
                          observed={"min_free": min_free}, expected={"min_free_at_least": floor})
    if min_free < floor:
        return CaseResult(Verdict.FAIL, reason=f"internal heap min_free {min_free} B < floor {floor} B",
                          observed={"min_free": min_free}, expected={"min_free_at_least": floor})
    return CaseResult(Verdict.PASS, observed={"min_free": min_free}, expected={"min_free_at_least": floor})


def judge_persisted_boot_count(persisted: Optional[int]) -> CaseResult:
    if persisted is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="boot_guard persisted_count not reported",
                          expected={"persisted_count": 0})
    if persisted != 0:
        return CaseResult(Verdict.FAIL, reason=f"boot_guard persisted_count={persisted}, expected 0",
                          observed={"persisted_count": persisted}, expected={"persisted_count": 0})
    return CaseResult(Verdict.PASS, observed={"persisted_count": 0}, expected={"persisted_count": 0})


def judge_readiness(items: "Optional[List[Tuple[str, str]]]") -> CaseResult:
    """items: [(key, status)]. estop_verified not_done is allowed (noted)."""
    if items is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="readiness unreadable")
    bad = [f"{k}={s}" for k, s in items if s != "ok" and k not in READINESS_ALLOWED_NOT_OK]
    noted = [f"{k}={s}" for k, s in items if s != "ok" and k in READINESS_ALLOWED_NOT_OK]
    obs = {"not_ok": bad, "allowed_not_ok": noted, "count": len(items)}
    if bad:
        return CaseResult(Verdict.FAIL, reason="readiness items not ok: " + ", ".join(bad), observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


_TEST_TRIP_RE = re.compile(r"trip_reason\s*=\s*(\d+)\s+trip_mask\s*=\s*(0x[0-9a-fA-F]+|\d+)")


def judge_test_trip_text(text: str) -> CaseResult:
    """Parse safety_test_trip's result: ok, reason 4, mask 0x0008."""
    if not isinstance(text, str) or not text.startswith("ok"):
        return CaseResult(Verdict.FAIL, reason=f"safety_test_trip did not succeed: {str(text)[:200]}",
                          observed={"text": str(text)[:300]})
    m = _TEST_TRIP_RE.search(text)
    if not m:
        # The tool confirms via TRIP_EVENT when DIAG has not refreshed; that text
        # carries the reason but no mask, and reason 4 alone is the test trip.
        e = re.search(r"confirmed by TRIP_EVENT: trip_reason\s*=\s*(\d+)", text)
        if e:
            r = int(e.group(1))
            if r != 4:
                return CaseResult(Verdict.FAIL, reason=f"test trip latched reason={r}, expected 4",
                                  observed={"trip_reason": r}, expected={"trip_reason": 4})
            return CaseResult(Verdict.PASS, observed={"trip_reason": r, "via": "TRIP_EVENT"},
                              expected={"trip_reason": 4})
        return CaseResult(Verdict.FAIL, reason="safety_test_trip ok text carries no trip_reason/trip_mask",
                          observed={"text": text[:300]})
    reason = int(m.group(1))
    mask = int(m.group(2), 0)
    obs = {"trip_reason": reason, "trip_mask": mask}
    if reason != 4 or mask != 0x0008:
        return CaseResult(Verdict.FAIL, reason=f"test trip latched reason={reason} mask=0x{mask:04x}, expected 4 / 0x0008",
                          observed=obs, expected={"trip_reason": 4, "trip_mask": 8})
    return CaseResult(Verdict.PASS, observed=obs, expected={"trip_reason": 4, "trip_mask": 8})


#: Timeouts may grow by this many over a run (trip, OTA and reset phases cause a
#: few legitimately); more than this is a regression. Chosen, not measured.
LINK_TIMEOUT_GROWTH_BOUND = 20


def compare_final_to_baseline(base: dict, final: dict) -> List[str]:
    """Regressions of the final snapshot against the baseline (empty = none)."""
    out: List[str] = []
    bh, fh = base.get("heap_min_free"), final.get("heap_min_free")
    if isinstance(bh, int) and isinstance(fh, int) and fh < bh - HEAP_REGRESSION_TOLERANCE_B:
        out.append(f"internal heap min_free fell {bh} -> {fh} B")
    bs, fs = base.get("stack_hwm") or {}, final.get("stack_hwm") or {}
    for name, b in bs.items():
        f = fs.get(name)
        if isinstance(b, int) and isinstance(f, int) and f < b - STACK_REGRESSION_TOLERANCE_B:
            out.append(f"stack {name} hwm fell {b} -> {f} B")
    # Only CRC errors (corrupted frames) count; timeouts grow legitimately
    # across the deliberate trip/OTA/reset phases of the run (REVIEW LOW-5).
    be, fe = base.get("link_crc_errors"), final.get("link_crc_errors")
    if isinstance(be, int) and isinstance(fe, int) and fe > be:
        out.append(f"safety link CRC errors grew {be} -> {fe}")
    # Timeouts grow legitimately across the deliberate trip/OTA/reset phases,
    # so only growth beyond LINK_TIMEOUT_GROWTH_BOUND counts (REVIEW LOW-3).
    bt, ft = base.get("link_timeouts"), final.get("link_timeouts")
    if isinstance(bt, int) and isinstance(ft, int) and ft - bt > LINK_TIMEOUT_GROWTH_BOUND:
        out.append(f"safety link timeouts grew {bt} -> {ft} (bound {LINK_TIMEOUT_GROWTH_BOUND})")
    return out


# -- health snapshot -------------------------------------------------------


def _try(snap: dict, key: str, fn):
    try:
        snap[key] = fn()
    except Exception as exc:  # noqa: BLE001 - an unreadable field is INCONCLUSIVE, not a crash
        snap.setdefault("_unreadable", {})[key] = f"{type(exc).__name__}: {exc}"
        snap[key] = None


def _read_snapshot(ctx: dict) -> dict:
    """Real board reads, every field independent. Missing -> None + _unreadable."""
    srv = _srv(ctx)
    snap: Dict[str, Any] = {}
    host = _hostname(ctx)

    def identity():
        st, body = _get_json(ctx, "/api/status")
        if st != 200 or not isinstance(body, dict):
            raise RuntimeError(f"/api/status http {st}")
        up = body.get("uptime_s")
        if isinstance(up, bool) or not isinstance(up, (int, float)):
            up = None
        return {"fw_build": body.get("fw_build"), "fw_version": body.get("fw_version") or body.get("version"),
                "uptime_s": up}

    _try(snap, "identity", identity)

    def partition():
        st, body = _get_json(ctx, "/api/partitions")
        if st != 200:
            raise RuntimeError(f"/api/partitions http {st}")
        rows = body.get("partitions", body) if isinstance(body, dict) else body
        for p in rows or []:
            if isinstance(p, dict) and (p.get("running") or p.get("is_running")):
                return p.get("label") or p.get("name")
        raise RuntimeError("no RUNNING partition marker")

    _try(snap, "running_partition", partition)

    def heap():
        from .. import dashboard_http_client as dhc

        return dhc.get_heap_status(host)

    _try(snap, "heap", heap)
    h = snap.get("heap") or {}
    hi = h.get("heap_internal") or {}
    snap["heap_min_free"] = hi.get("min_free")
    snap["heap_free"] = hi.get("free")
    snap["heap_largest"] = hi.get("largest_free_block")
    snap["psram_free"] = (h.get("heap_spiram") or {}).get("free")
    if snap.get("heap") is None or h.get("unacknowledged_crash_check_error") or h.get("unacknowledged_crash") is None:
        snap["crash_unacked"] = None  # unreadable crash report is never "no crash"
    else:
        snap["crash_unacked"] = bool(h.get("unacknowledged_crash"))

    def readiness():
        st, body = _get_json(ctx, "/api/readiness")
        if st != 200 or not isinstance(body, dict):
            raise RuntimeError(f"/api/readiness http {st}")
        return [(str(i.get("key")), str(i.get("status"))) for i in body.get("items", [])]

    _try(snap, "readiness", readiness)

    def diag():
        d = srv._safety.get_diag()
        return {"state": getattr(d, "state", None)}

    _try(snap, "diag", diag)
    _try(snap, "trip_reason", lambda: J.parse_trip_reason(srv.safety_get_diag()))

    def _counter(name):
        v = getattr(srv._safety.get_link_stats(), name, None)
        if isinstance(v, bool) or not isinstance(v, int):
            raise RuntimeError(f"link counter {name} unreadable ({v!r})")
        return v

    # An unreadable counter is None + _unreadable (INCONCLUSIVE), never 0.
    _try(snap, "link_crc_errors", lambda: _counter("crc_errors"))
    _try(snap, "link_timeouts", lambda: _counter("timeouts"))
    _try(snap, "link_errors", lambda: _counter("crc_errors") + _counter("timeouts"))
    _try(snap, "liveness", lambda: srv.check_task_liveness())

    def stacks():
        out = {}
        for e in srv._info.get_stack_margin():
            out[e.name] = {"hwm": e.hwm_bytes, "alive": e.alive, "level": int(e.level)}
        return out

    _try(snap, "stacks", stacks)
    snap["stack_hwm"] = {n: v["hwm"] for n, v in (snap.get("stacks") or {}).items() if v["alive"]}

    def boot():
        from .. import ota_http_client as ohc

        return ohc.get_boot_guard_status(host).get("persisted_count")

    _try(snap, "boot_persisted", boot)

    def thermo():
        rs = srv._thermo.read()
        return [(r.channel, r.temperature_c, bool(r.valid)) for r in rs]

    _try(snap, "thermo", thermo)

    def wifi():
        w = srv._info.get_wifi_status()
        return {"connected": bool(w.connected), "ip": w.ip}

    _try(snap, "wifi", wifi)
    return snap


def _snapshot(ctx: dict) -> dict:
    fn = ctx.get("_rl_snapshot_fn")
    return fn(ctx) if fn else _read_snapshot(ctx)


def judge_snapshot(snap: dict) -> "Tuple[Verdict, List[str], List[str]]":
    """-> (verdict, fail reasons, inconclusive reasons). Pure."""
    fails: List[str] = []
    inc: List[str] = []
    ident = snap.get("identity")
    if ident is None:
        inc.append("identity unreadable")
    elif not ident.get("fw_build") or not ident.get("fw_version"):
        fails.append(f"identity incomplete: {ident}")
    part = snap.get("running_partition")
    if part is None:
        inc.append("running partition unreadable")
    elif part != "app":
        fails.append(f"running partition {part!r}, expected 'app'")
    c = snap.get("crash_unacked")
    if c is None:
        inc.append("crash report unreadable")
    elif c:
        fails.append("unacknowledged crash report")
    r = judge_readiness(snap.get("readiness"))
    if r.verdict == Verdict.FAIL:
        fails.append(r.reason)
    elif r.verdict != Verdict.PASS:
        inc.append(r.reason)
    d = snap.get("diag")
    if d is None or d.get("state") is None:
        inc.append("safety state unreadable")
    elif d["state"] != 2:
        fails.append(f"safety state {d['state']}, expected ARMED (2)")
    tr = snap.get("trip_reason")
    if tr is None:
        inc.append("trip_reason unreadable")
    elif tr != 0:
        fails.append(f"safety trip latched (reason {tr})")
    live = snap.get("liveness")
    if live is None:
        inc.append("task liveness unreadable")
    elif "RESULT: ok" not in live:
        fails.append("task liveness check failed")
    st = snap.get("stacks")
    if st is None:
        inc.append("stack margins unreadable")
    else:
        low = [n for n, v in st.items() if v["alive"] and v["level"] != 0]
        if low:
            fails.append("stack margin below floor: " + ", ".join(sorted(low)))
    hr = judge_heap_floor(snap.get("heap_min_free"))
    if hr.verdict == Verdict.FAIL:
        fails.append(hr.reason)
    elif hr.verdict != Verdict.PASS:
        inc.append(hr.reason)
    br = judge_persisted_boot_count(snap.get("boot_persisted"))
    if br.verdict == Verdict.FAIL:
        fails.append(br.reason)
    elif br.verdict != Verdict.PASS:
        inc.append(br.reason)
    th = snap.get("thermo")
    if th is None:
        inc.append("thermocouples unreadable")
    else:
        badch = [ch for ch, _t, ok in th if not ok]
        if not th:
            fails.append("no thermocouple channels reported")
        elif badch:
            fails.append(f"thermocouple channels invalid: {badch}")
    w = snap.get("wifi")
    if w is None:
        inc.append("Wi-Fi status unreadable")
    elif not w.get("connected"):
        inc.append("Wi-Fi not connected as a station (AP-only is fine on the bench)")
    unread = snap.get("_unreadable") or {}
    bad_link = sorted(k for k in unread if k.startswith("link_"))
    if bad_link:
        inc.append("safety link counters unreadable: " + ", ".join(bad_link))
    verdict = Verdict.FAIL if fails else (Verdict.INCONCLUSIVE if inc else Verdict.PASS)
    return verdict, fails, inc


def _snapshot_observed(snap: dict) -> dict:
    return {
        "fw_build": (snap.get("identity") or {}).get("fw_build"),
        "fw_version": (snap.get("identity") or {}).get("fw_version"),
        "running_partition": snap.get("running_partition"),
        "heap_min_free": snap.get("heap_min_free"),
        "heap_free": snap.get("heap_free"),
        "heap_largest_block": snap.get("heap_largest"),
        "psram_free": snap.get("psram_free"),
        "link_errors": snap.get("link_errors"),
        "link_crc_errors": snap.get("link_crc_errors"),
        "wifi": snap.get("wifi"),
        "unreadable": snap.get("_unreadable"),
    }


def _result_from_snapshot(snap: dict) -> CaseResult:
    v, fails, inc = judge_snapshot(snap)
    obs = _snapshot_observed(snap)
    if v == Verdict.FAIL:
        return CaseResult(v, reason="; ".join(fails), observed=obs)
    if v == Verdict.INCONCLUSIVE:
        return CaseResult(v, reason="; ".join(inc), observed=obs)
    notes = [f"{k}={s}" for k, s in (snap.get("readiness") or []) if s != "ok"]
    if notes:
        obs["noted"] = notes
    return CaseResult(v, observed=obs)


# -- phase A / F -----------------------------------------------------------


def _case_rl_a01(ctx: dict) -> CaseResult:
    snap = _snapshot(ctx)
    ctx["_rl_baseline"] = snap
    res = _result_from_snapshot(snap)
    if res.verdict in (Verdict.FAIL, Verdict.INCONCLUSIVE):
        _abort(ctx, f"phase A health {res.verdict}: {res.reason}")
    return res


def _case_rl_f01(ctx: dict) -> CaseResult:
    ab = _aborted(ctx)
    if ab:
        return ab
    base = ctx.get("_rl_baseline")
    snap = _snapshot(ctx)
    res = _result_from_snapshot(snap)
    obs = dict(res.observed or {})
    if base is None:
        return CaseResult(Verdict.INCONCLUSIVE if res.verdict == Verdict.PASS else res.verdict,
                          reason=res.reason or "no phase A baseline to compare against", observed=obs)
    regs = compare_final_to_baseline(base, snap)
    obs["regressions"] = regs
    if res.verdict == Verdict.FAIL or regs:
        reason = "; ".join(([res.reason] if res.verdict == Verdict.FAIL else []) + regs)
        return CaseResult(Verdict.FAIL, reason=reason, observed=obs)
    return CaseResult(res.verdict, reason=res.reason, observed=obs)


# -- phase B: safety -------------------------------------------------------


def _case_rl_b01(ctx: dict) -> CaseResult:
    ab = _aborted(ctx)
    if ab:
        return ab
    srv = _srv(ctx)
    trip_fn = getattr(srv, "safety_test_trip", None)
    if trip_fn is None:
        return CaseResult(Verdict.SKIP, reason="safety_test_trip tool not present on this server")
    prob = _pin_identity(ctx)
    if prob:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"before the test trip: {prob}")
    try:
        text = trip_fn(confirm=True, host=ctx["host"])
    except Exception as exc:  # noqa: BLE001
        _abort(ctx, "safety_test_trip raised")
        return CaseResult(Verdict.FAIL, reason=f"safety_test_trip raised {type(exc).__name__}: {exc}")
    res = judge_test_trip_text(text)
    obs = dict(res.observed or {})
    # Only the trip this case injected (reason 4, mask 0x0008) may be cleared;
    # any other latched trip is a real fault and must stay latched.
    try:
        pre = srv.safety_get_diag()
        pre_reason, pre_mask = J.parse_trip_reason(pre), J.parse_trip_mask(pre)
    except Exception as exc:  # noqa: BLE001
        _abort(ctx, "phase B could not read the trip before clearing")
        return CaseResult(Verdict.FAIL, reason=f"diag before clear raised {type(exc).__name__}: {exc}", observed=obs)
    obs["trip_before_clear"] = {"reason": pre_reason, "mask": pre_mask}
    if pre_reason != 4 or pre_mask != 0x0008:
        _abort(ctx, "the UART diag does not show the test trip; nothing cleared")
        return CaseResult(Verdict.FAIL, reason=f"UART diag shows trip reason={pre_reason} mask={pre_mask}, expected the test trip (4, 0x0008); left as is", observed=obs)
    # The latched trip must always be cleared, even when the judge FAILed,
    # so the board is not left tripped for the heat phase.
    try:
        last, elapsed = wait_for_trip_clear(ctx, srv)
    except Exception as exc:  # noqa: BLE001
        _abort(ctx, "phase B could not clear the test trip")
        return CaseResult(Verdict.FAIL, reason=f"clear raised {type(exc).__name__}: {exc}", observed=obs)
    obs["clear_elapsed_s"] = round(float(elapsed), 2)
    obs["trip_reason_after_clear"] = last
    if last not in (0, None) or last is None:
        _abort(ctx, "test trip did not clear")
        return CaseResult(Verdict.FAIL, reason=f"trip_reason {last} after clear (waited {elapsed:.1f}s)", observed=obs)
    if res.verdict != Verdict.PASS:
        _abort(ctx, "safety_test_trip failed")
        return CaseResult(res.verdict, reason=res.reason, observed=obs)
    # Heat permission must be back.
    try:
        items = _readiness_items(ctx)
    except Exception as exc:  # noqa: BLE001
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"readiness after clear unreadable: {exc}", observed=obs)
    rj = judge_readiness(items)
    obs["readiness_after_clear"] = rj.observed
    if rj.verdict != Verdict.PASS:
        _abort(ctx, "heat not permitted after clear")
        return CaseResult(Verdict.FAIL, reason="after clear: " + rj.reason, observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _readiness_items(ctx: dict):
    st, body = _get_json(ctx, "/api/readiness")
    if st != 200 or not isinstance(body, dict):
        raise RuntimeError(f"/api/readiness http {st}")
    return [(str(i.get("key")), str(i.get("status"))) for i in body.get("items", [])]


# -- phase C: heat ---------------------------------------------------------


def _case_rl_c01(ctx: dict) -> CaseResult:
    ab = _aborted(ctx)
    if ab:
        return ab
    if ctx.get("allow_heat") is not True:
        return CaseResult(Verdict.SKIP, reason="allow_heat=False (release heat phase)")
    if ctx.get("estop_unverified"):
        return CaseResult(Verdict.SKIP, reason="estop_unverified")
    srv = _srv(ctx)
    now = ctx.get("_now", _time.monotonic)
    sleep = ctx.get("_sleep", _time.sleep)
    kiln = _target(ctx) == "kiln"
    timeout_s = KILN_HEAT_TIMEOUT_S if kiln else BENCH_HEAT_TIMEOUT_S
    started = False
    try:
        try:
            temps = H._zone_temps(ctx)
        except Exception as exc:  # noqa: BLE001
            return CaseResult(Verdict.INCONCLUSIVE, reason=f"thermo unreadable: {exc}")
        if not temps:
            return CaseResult(Verdict.INCONCLUSIVE, reason="no valid thermo reading")
        mask = 0
        for z in temps:
            mask |= 1 << z
        idp = _board_identity_problem(ctx, kiln, mask)
        if idp:
            return CaseResult(Verdict.INCONCLUSIVE, reason=f"heat refused: {idp}")
        if kiln:
            cok, creason = H._check_zone_ceilings(ctx, mask, KILN_HEAT_TARGET_C)
            if not cok:
                return CaseResult(Verdict.INCONCLUSIVE, reason=f"heat refused: {creason}")
            ok, reason, ambient = H._start_bench_profile(
                ctx, mask, ramp_c_per_hr=KILN_HEAT_RAMP_C_PER_HR, dwell_min=KILN_HEAT_DWELL_MIN,
                target_c=KILN_HEAT_TARGET_C)
        else:
            ok, reason, ambient = H._start_bench_profile(
                ctx, mask, target_offset_c=BENCH_HEAT_OFFSET_C, dwell_min=BENCH_HEAT_DWELL_MIN)
        if not ok:
            return CaseResult(Verdict.INCONCLUSIVE, reason=f"heat profile not started: {reason}")
        started = True
        deadline = now() + timeout_s
        states: List[str] = []
        heap_samples: List[int] = []
        max_duty = 0.0
        relay_seen_on = False
        hold_err: List[float] = []
        pause_reason = None
        final_state = None
        while True:
            st = srv._profiles.get_exec_status()
            final_state = st.state_name
            if not states or states[-1] != final_state:
                states.append(final_state)
            for zs in st.zones:
                max_duty = max(max_duty, float(getattr(zs, "duty", 0.0) or 0.0))
                relay_seen_on = relay_seen_on or bool(getattr(zs, "relay_commanded_on", False))
                if getattr(st, "dwelling", False) and getattr(zs, "actual_valid", False) and (st.zone_mask >> zs.zone) & 1:
                    hold_err.append(abs(float(zs.actual_c) - float(st.target_c)))
            pr = getattr(st, "pause_reason", None)
            if pr:
                pause_reason = pr
            hm = _heap_min(ctx)
            if hm is not None:
                heap_samples.append(hm)
            if final_state in ("done", "faulted", "idle") and "running" in states:
                break
            if final_state == "paused" or pause_reason:
                break
            if now() >= deadline:
                break
            sleep(HEAT_POLL_S)
        obs = {"states": states, "max_duty": max_duty, "relay_seen_on": relay_seen_on,
               "hold_samples": len(hold_err), "hold_max_err_c": max(hold_err) if hold_err else None,
               "heap_min_samples": (min(heap_samples) if heap_samples else None),
               "pause_reason": pause_reason, "target": _target(ctx)}
        if final_state == "faulted" or pause_reason:
            return CaseResult(Verdict.FAIL, reason=f"firing ended {final_state}, pause_reason={pause_reason}", observed=obs)
        if "running" not in states or final_state != "done":
            return CaseResult(Verdict.FAIL, reason=f"executor did not go RUNNING then DONE (states {states}, timeout {timeout_s:.0f}s)", observed=obs)
        if not hold_err:
            return CaseResult(Verdict.FAIL, reason="never sampled a zone during the hold", observed=obs)
        if max(hold_err) > HOLD_TRACK_BAND_C:
            return CaseResult(Verdict.FAIL, reason=f"zone off setpoint by {max(hold_err):.1f}C during hold (band {HOLD_TRACK_BAND_C}C)", observed=obs)
        if not relay_seen_on and max_duty <= 0:
            return CaseResult(Verdict.FAIL, reason="no relay ever commanded on and duty stayed 0", observed=obs)
        if not heap_samples:
            return CaseResult(Verdict.INCONCLUSIVE, reason="heap never sampled during the firing", observed=obs)
        hj = judge_heap_floor(min(heap_samples))
        if hj.verdict != Verdict.PASS:
            return CaseResult(hj.verdict, reason=hj.reason, observed=obs)
        return CaseResult(Verdict.PASS, observed=obs)
    finally:
        _release_teardown(ctx)


#: Zone ceilings above this mean a real kiln; at or below it a bench fixture.
BENCH_CEILING_MAX_C = 200.0


def _pin_identity(ctx: dict) -> Optional[str]:
    """UART-vs-HTTP identity check; on success pins the resolved IP into
    ctx["host"] so later calls never re-resolve the hostname. Returns a
    reason on a mismatch, an unreadable value or no host (fail closed)."""
    host = ctx.get("host")
    if not host:
        return "no host to verify the board identity against the target"
    idfn = ctx.get("_board_identity_fn")
    if idfn is None:
        from .. import mcp_server_aux

        idfn = mcp_server_aux._board_identity_check
    try:
        mism, ip = idfn(host)
    except Exception as exc:  # noqa: BLE001
        return f"could not tie the UART link to the HTTP host: {exc}"
    if mism:
        return f"UART/HTTP board mismatch: {mism}"
    if ip:
        from .. import mcp_server_aux

        # Keeps the port; str(ip) alone would drop it and accept IPv4Address.
        ctx["host"] = mcp_server_aux._pin_host(host, ip)
    return None


def _board_identity_problem(ctx: dict, kiln: bool, mask: int) -> Optional[str]:
    """Cross-check the named target against the board's own zone ceilings.
    Fail closed: no host, unreadable config, or a mismatch returns a reason."""
    from .. import zones_http_client

    # Heat runs over UART, ceilings are read over HTTP: both must be the same board.
    prob = _pin_identity(ctx)
    if prob:
        return prob
    host = ctx["host"]

    get_zones = ctx.get("_get_zones_config", zones_http_client.get_zones)
    try:
        snap = get_zones(host)
    except Exception as exc:  # noqa: BLE001
        return f"zones config unreadable, cannot verify target {_target(ctx)!r}: {exc}"
    # Only the zones that will be heated count (bit z of mask).
    zl = [z for z in (snap.get("zones") or []) if isinstance(z, dict)]
    ceilings = []
    for i in range(16):
        if (mask >> i) & 1:
            c = zl[i].get("max_temp_c") if i < len(zl) else None
            if isinstance(c, (int, float)) and not isinstance(c, bool) and c > 0:
                ceilings.append(c)
            else:
                return f"zone {i} (to be heated) has no readable ceiling, cannot verify the target"
    if not ceilings:
        return "no zone ceilings reported for the heated zones, cannot verify the target"
    top = max(ceilings)
    if kiln and top <= BENCH_CEILING_MAX_C:
        return f"target=kiln but highest zone ceiling is {top:.0f}C (looks like the bench fixture)"
    if not kiln and top > BENCH_CEILING_MAX_C:
        return f"target=bench but highest zone ceiling is {top:.0f}C (looks like a real kiln)"
    return None


def _heap_min(ctx: dict) -> Optional[int]:
    fn = ctx.get("_rl_heap_fn")
    if fn:
        return fn(ctx)
    try:
        from .. import dashboard_http_client as dhc

        return (dhc.get_heap_status(_hostname(ctx)).get("heap_internal") or {}).get("min_free")
    except Exception:  # noqa: BLE001
        return None


def _release_teardown(ctx: dict) -> None:
    """Always stop the firing and delete the profile; verify relays OFF."""
    H._cleanup_bench_profile(ctx)
    try:
        st = _srv(ctx)._profiles.get_exec_status()
        if st.state_name in ("running", "paused"):
            ctx["_tainted"] = True
            _abort(ctx, "heat teardown could not stop the firing")
        elif any(getattr(z, "relay_commanded_on", False) for z in st.zones):
            ctx["_tainted"] = True
            _abort(ctx, "relay still commanded ON after heat teardown")
    except Exception:  # noqa: BLE001 - cleanup must not mask the verdict
        ctx["_tainted"] = True


# -- phase D: OTA ----------------------------------------------------------


_STAGE_EMPTY_MARKS = ("staged=False", "busy=False", "header=none")


def _stage_empty(text: str) -> bool:
    return text.startswith("ok") and all(m in text for m in _STAGE_EMPTY_MARKS)


def _case_rl_d01(ctx: dict) -> CaseResult:
    ab = _aborted(ctx)
    if ab:
        return ab
    srv = _srv(ctx)
    want_install = ctx.get("ota_install") is True
    needed = ["update_check", "update_fetch_status", "update_status", "update_stage_release", "update_stage_clear"]
    if want_install:
        needed.append("update_install_staged")
    for name in needed:
        if getattr(srv, name, None) is None:
            return CaseResult(Verdict.SKIP, reason=f"{name} tool not present on this server")
    obs: Dict[str, Any] = {"target": _target(ctx), "install_requested": want_install}
    # UART-vs-HTTP identity, then the resolved IP is pinned for every call below.
    prob = _pin_identity(ctx)
    if prob:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"before the update steps: {prob}", observed=obs)
    host = ctx["host"]
    obs["host"] = host
    # A stage the operator filled before the run is theirs: never overwrite or erase it.
    pre = str(srv.update_status(host=host))
    obs["stage_before"] = pre[:300]
    if not _stage_empty(pre):
        return CaseResult(Verdict.INCONCLUSIVE,
                          reason=f"stage not provably empty before the run (clear or install it first): {pre[:160]}",
                          observed=obs)
    chk = srv.update_check(allow_prerelease=True, host=host)
    obs["check"] = str(chk)[:300]
    if not str(chk).startswith("ok"):
        return CaseResult(Verdict.INCONCLUSIVE if str(chk).startswith(("REFUSED", "UNKNOWN")) else Verdict.FAIL,
                          reason=f"update_check: {str(chk)[:200]}", observed=obs)
    obs["fetch_status"] = str(srv.update_fetch_status(host=host))[:300]
    staged = False
    install_ran = False
    stage_ok = False
    try:
        staged = True  # once invoked, a download may be in flight whatever it returns
        stg = srv.update_stage_release(confirm=True, allow_prerelease=True, host=host)
        obs["stage"] = str(stg)[:400]
        if not str(stg).startswith("ok"):
            res = CaseResult(Verdict.FAIL, reason=f"update_stage_release: {str(stg)[:200]}", observed=obs)
        else:
            stage_ok = True
            if "verified by read-back" not in str(stg):
                res = CaseResult(Verdict.FAIL, reason="stage result does not claim read-back verification", observed=obs)
            elif want_install:
                # Identity re-checked right before the install; a mismatch leaves
                # the stage to the cleanup below (nothing was installed).
                prob = _pin_identity(ctx)
                if prob:
                    res = CaseResult(Verdict.INCONCLUSIVE, reason=f"before the install: {prob}", observed=obs)
                else:
                    host = ctx["host"]
                    res, install_ran = _install_and_verify(
                        ctx, lambda **kw: srv.update_install_staged(host=host, **kw), obs)
            else:
                res = CaseResult(Verdict.PASS, observed=obs)
    except Exception as exc:  # noqa: BLE001
        res = CaseResult(Verdict.FAIL, reason=f"stage raised {type(exc).__name__}: {exc}", observed=obs)
    if install_ran:
        # The install tool consumed (or may have consumed) the stage and the board
        # rebooted: never cancel or clear while an install/reboot may be running.
        return res
    if staged:
        # Re-read the stage: after a non-ok or raising stage call the download may
        # still be running or half written. Cancel, then clear, then read back.
        try:
            post = str(srv.update_status(host=host))
        except Exception as exc:  # noqa: BLE001
            post = f"error: {type(exc).__name__}: {exc}"
        obs["stage_after"] = post[:300]
        if stage_ok or not _stage_empty(post):
            cancel = getattr(srv, "update_fetch_cancel", None)
            if cancel is None:
                obs["fetch_cancel"] = "tool missing"
                ctx["_tainted"] = True
            else:
                try:
                    cres = str(cancel(confirm=True, host=host))
                    obs["fetch_cancel"] = cres[:200]
                    if not cres.startswith("ok"):
                        ctx["_tainted"] = True
                except Exception as exc:  # noqa: BLE001
                    obs["fetch_cancel"] = f"raised {type(exc).__name__}: {exc}"
                    ctx["_tainted"] = True
            try:
                clr = str(srv.update_stage_clear(confirm=True, host=host))
            except Exception as exc:  # noqa: BLE001
                clr = f"FAILED: raised {type(exc).__name__}: {exc}"
            obs["stage_clear"] = clr[:300]
            if not clr.startswith("ok"):
                ctx["_tainted"] = True
                return CaseResult(Verdict.FAIL, reason=f"stage clear failed, image left in stage: {clr[:160]}", observed=obs)
            try:
                back = str(srv.update_status(host=host))
            except Exception as exc:  # noqa: BLE001
                back = f"error: {type(exc).__name__}: {exc}"
            obs["stage_after_clear"] = back[:300]
            if not _stage_empty(back):
                ctx["_tainted"] = True
                return CaseResult(Verdict.FAIL, reason=f"stage still staged or busy after clear: {back[:160]}", observed=obs)
    return res


def _valid_baseline(snap: dict) -> Optional[str]:
    """None when every field the new baseline needs is present and typed."""
    ident = snap.get("identity")
    if not isinstance(ident, dict) or not ident.get("fw_build"):
        return "identity/fw_build"
    for k in ("link_crc_errors", "link_timeouts", "heap_min_free"):
        v = snap.get(k)
        if isinstance(v, bool) or not isinstance(v, int):
            return k
    return None


def _install_and_verify(ctx: dict, install, obs: dict) -> Tuple[CaseResult, bool]:
    """Returns (result, install_may_have_run). The stage is only left alone when
    the install tool reported ok or raised (it may be installing/rebooting)."""
    before = _snapshot(ctx)
    bident = before.get("identity") or {}
    before_build = bident.get("fw_build")
    before_up = bident.get("uptime_s")
    if not before_build:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no fw_build before install; cannot prove a change", observed=obs), False
    raised = None
    try:
        res = install(confirm=True)
    except Exception as exc:  # noqa: BLE001
        raised = f"{type(exc).__name__}: {exc}"
        res = ""
    obs["install"] = (f"RAISED {raised}" if raised else str(res))[:400]
    if not raised and not str(res).startswith("ok"):
        # Only error:/REFUSED precede the install POST. UNKNOWN, FAILED: install
        # recorded a failure and FAILED: install refused or lost all come after
        # it was sent, so the recovery image may be applying: never clean up.
        if str(res).startswith(("error: could not read GET", "REFUSED")) or re.match(
                r"FAILED: install refused or lost: HTTP 4\d\d", str(res)):
            # Definite: the board answered the POST with a 4xx (or it was never sent).
            return CaseResult(Verdict.FAIL, reason=f"update_install_staged: {str(res)[:200]}", observed=obs), False
        raised_non_ok = True
    else:
        raised_non_ok = False
    sleep = ctx.get("_sleep", _time.sleep)
    now = ctx.get("_now", _time.monotonic)
    deadline = now() + 300.0
    after = None
    saw_down = False
    bad_field = None
    while now() < deadline:
        sleep(10.0)
        try:
            cand = _snapshot(ctx)
        except Exception:  # noqa: BLE001
            saw_down = True
            continue
        ident = cand.get("identity") or {}
        if not ident.get("fw_build"):
            saw_down = True
            continue
        up = ident.get("uptime_s")
        rebooted = (saw_down or ident["fw_build"] != before_build
                    or (isinstance(up, (int, float)) and isinstance(before_up, (int, float)) and up < before_up))
        if not rebooted:
            continue  # the pre-install board still answering: not a confirmed reboot
        bad_field = _valid_baseline(cand)
        if bad_field:
            continue
        after = cand
        break
    if raised:
        obs["install_raised"] = raised
    if raised_non_ok:
        obs["install_non_ok"] = True
    if after is None:
        ctx["_rl_baseline"] = None  # since-boot watermarks are gone; F01 cannot compare
        if bad_field and (raised or raised_non_ok):
            return CaseResult(Verdict.FAIL, reason=(
                f"update_install_staged {'raised ' + str(raised) if raised else 'returned ' + repr(str(res)[:80])}"
                f" and after the reboot the baseline field {bad_field} stayed unreadable; install state unproven"), observed=obs), True
        if bad_field:
            return CaseResult(Verdict.INCONCLUSIVE, reason=f"after the reboot the baseline field {bad_field} stayed unreadable", observed=obs), True
        why = "update_install_staged raised and " if raised else (
            f"update_install_staged returned {str(res)[:80]!r} and " if raised_non_ok else "")
        return CaseResult(Verdict.FAIL, reason=f"{why}no confirmed reboot with a readable baseline after install within 300 s", observed=obs), True
    # The reboot reset every since-boot watermark (heap min_free, stack hwm, link
    # counters): re-baseline so RL-F01 does not compare across the reboot.
    ctx["_rl_baseline"] = after
    obs["version_before"] = bident.get("fw_version")
    obs["version_after"] = (after.get("identity") or {}).get("fw_version")
    obs["build_before"] = before_build
    obs["build_after"] = after["identity"]["fw_build"]
    if raised:
        return CaseResult(Verdict.FAIL, reason=f"update_install_staged raised ({raised}) but the board rebooted; install state unproven", observed=obs), True
    if raised_non_ok:
        return CaseResult(Verdict.FAIL, reason=f"update_install_staged: {str(res)[:200]} (board rebooted; install state unproven)", observed=obs), True
    if obs["build_after"] == before_build:
        return CaseResult(Verdict.FAIL, reason=f"fw_build unchanged after install ({before_build})", observed=obs), True
    v, fails, _inc = judge_snapshot(after)
    if v == Verdict.FAIL:
        return CaseResult(Verdict.FAIL, reason="after install: " + "; ".join(fails), observed=obs), True
    if _inc:
        obs["after_inconclusive"] = [str(i)[:120] for i in _inc]
        return CaseResult(Verdict.INCONCLUSIVE, reason="after install, unreadable: " + "; ".join(str(i) for i in _inc), observed=obs), True
    return CaseResult(Verdict.PASS, observed=obs), True


def _case_rl_e01(ctx: dict) -> CaseResult:
    ab = _aborted(ctx)
    if ab:
        return ab
    parts: Dict[str, str] = {}
    fails: List[str] = []
    inc: List[str] = []
    ids = list(_WEB_SMOKE)
    if _target(ctx) == "bench" and ctx.get("lcd_camera", True):
        ids += _LCD_SMOKE
    for cid in ids:
        spec = get_case(cid)
        if spec.judge is None:
            inc.append(f"{cid} has no judge")
            continue
        try:
            r = spec.judge(ctx)
        except Exception as exc:  # noqa: BLE001
            r = CaseResult(Verdict.FAIL, reason=f"raised {type(exc).__name__}: {exc}")
        parts[cid] = str(r.verdict)
        if r.verdict == Verdict.FAIL:
            fails.append(f"{cid}: {r.reason}")
        elif r.verdict != Verdict.PASS:
            inc.append(f"{cid} {r.verdict}: {r.reason}")
    if _target(ctx) == "bench" and not ctx.get("lcd_camera", True):
        inc.append("LCD capture skipped (no camera)")
    # A kiln has no LCD camera by design, so that is not a missing check.
    obs = {"cases": parts, "target": _target(ctx), "lcd_checked": _target(ctx) == "bench" and bool(ctx.get("lcd_camera", True))}
    if fails:
        return CaseResult(Verdict.FAIL, reason="; ".join(fails), observed=obs)
    if inc:
        return CaseResult(Verdict.INCONCLUSIVE, reason="; ".join(inc), observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# -- verdict ---------------------------------------------------------------


def format_verdict_line(rows: Dict[str, str]) -> str:
    return "RELEASE VERDICT: " + "  ".join(f"{cid}={rows[cid]}" for cid in PHASE_IDS if cid in rows)


def _case_rl_v(ctx: dict) -> CaseResult:
    results = ctx.get("_run_results")
    if not isinstance(results, dict):
        return CaseResult(Verdict.FAIL, reason="ERROR: no run results in ctx")
    rows = {cid: str(getattr(results[cid], "verdict", results[cid])) for cid in PHASE_IDS if cid in results}
    # Verdict values may be enum members; normalise to their bare name.
    rows = {k: v.split(".")[-1] for k, v in rows.items()}
    line = format_verdict_line(rows)
    ctx["_release_verdict_line"] = line
    obs = {"phases": rows, "line": line}
    if len(rows) < len(PHASE_IDS):
        return CaseResult(Verdict.FAIL, reason="missing phase results: " + ", ".join(p for p in PHASE_IDS if p not in rows), observed=obs)
    vals = set(rows.values())
    if "FAIL" in vals:
        return CaseResult(Verdict.FAIL, reason=line, observed=obs)
    if vals == {"PASS"}:
        return CaseResult(Verdict.PASS, observed=obs)
    return CaseResult(Verdict.INCONCLUSIVE, reason=line, observed=obs)


_CASE_FUNCS = {
    "RL-A01": _case_rl_a01, "RL-B01": _case_rl_b01, "RL-C01": _case_rl_c01,
    "RL-D01": _case_rl_d01, "RL-E01": _case_rl_e01, "RL-F01": _case_rl_f01, "RL-V01": _case_rl_v,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
