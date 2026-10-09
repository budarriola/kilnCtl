"""WEB-DASH-02..12: dashboard (`/`) judges
(docs/BENCH_TEST_WEB_JUDGES_PLAN.md section 4.1).

No JavaScript runs here (plan section 2 rule 1): each case judges the server
input the page's JS consumes plus the static presence of the element ids /
functions in the served HTML or app.js. Observers (DASH-03/06/08) read window
probes registered through ``CaseSpec.window_probe``; they never start a firing
and never clear a trip. Board access goes through the ``cases_web_rw`` seams
(``_get_json``/``_get_text``/``_post_json``/``_post_raw``) so fake-board tests
inject ``ctx["http_get_json"]`` etc.

Owner ruling 2026-10-09: DASH-04, -07, -12 take the plan's recommended
(read-only / static) interpretation.
"""
from __future__ import annotations

import time
from typing import Any, Dict, List, Optional, Tuple

from . import cases_web
from . import cases_web_rw as W
from .board_lock import READ_ONLY_SUITES
from .registry import CaseResult, Verdict, get_case

BENCH_PROFILE_SLOT_ID = 7
#: done/aborted are terminal (engine not running) and persist after a tune.
_AUTOTUNE_NOT_RUNNING = ("idle", "done", "aborted")
_EXEC_STATES = ("idle", "running", "paused", "done", "faulted")
_FEAS_VALUES = ("ok", "too_fast", "unreachable", "unknown")
_LAST_RUN_PHASES = ("none", "running", "paused", "done", "halted", "faulted")
_READINESS_STATUSES = ("ok", "not_done", "cannot_yet", "deliberately_off")


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _fetch_page(ctx: dict, path: str) -> Tuple[Optional[str], Optional[str]]:
    """Return (html, error)."""
    try:
        return cases_web._web_client(ctx).goto(path), None
    except Exception as exc:  # noqa: BLE001
        return None, f"ERROR: GET {path} failed: {type(exc).__name__}: {exc}"


def _static_missing(ctx: dict, path: str, tokens: List[str]) -> Tuple[List[str], Optional[str]]:
    html, err = _fetch_page(ctx, path)
    if err is not None:
        return [], err
    return [t for t in tokens if t not in (html or "")], None


def _static_fail(path: str, missing: List[str], observed: dict) -> CaseResult:
    observed = dict(observed)
    observed["missing_static_tokens"] = missing
    return CaseResult(Verdict.FAIL, reason=f"{path} is missing expected token(s): {missing}", observed=observed)


def _exec_probe(ctx: dict) -> dict:
    status, body = W._get_json(ctx, "/api/profile_exec")
    if status != 200 or not isinstance(body, dict):
        return {"error": f"GET /api/profile_exec status={status}"}
    return {"state": body.get("state"), "profile_id": body.get("profile_id")}


def _mutating_gate(ctx: dict) -> Optional[str]:
    """Standard mutating gate (plan section 2 rule 3). Returns a reason when
    the gate does not hold, else None."""
    suite = ctx.get("suite")
    if suite is None or suite in READ_ONLY_SUITES:
        return f"suite {suite!r} is not a mutating suite"
    st, body = W._get_json(ctx, "/api/profile_exec")
    if st != 200 or not isinstance(body, dict) or body.get("state") != "idle":
        return f"executor not idle (status={st}, state={(body or {}).get('state') if isinstance(body, dict) else None})"
    st, body = W._get_json(ctx, "/api/autotune")
    at_state = body.get("state") if isinstance(body, dict) else None
    if st != 200 or at_state not in _AUTOTUNE_NOT_RUNNING:
        return f"autotune running or unknown (status={st}, state={at_state})"
    return None


def group_profiles_for_picker(profiles: list, fav_ids: list, recent_limit: int = 5) -> dict:
    """Python port of main_page.html ``groupProfilesForPicker``."""
    fav = {str(i) for i in fav_ids}
    favorites = [p for p in profiles if str(p.get("id")) in fav]
    non_fav = [p for p in profiles if str(p.get("id")) not in fav]
    recent = sorted((p for p in non_fav if p.get("last_run_started_unix_s")),
                    key=lambda p: -p["last_run_started_unix_s"])[:recent_limit]
    recent_ids = {str(p.get("id")) for p in recent}
    rest = [p for p in non_fav if str(p.get("id")) not in recent_ids]
    return {"favorites": favorites, "recent": recent, "all": rest}


# ---------------------------------------------------------------------------
# WEB-DASH-02
# ---------------------------------------------------------------------------

def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _case_dash02(ctx: dict) -> CaseResult:
    fs, fav = W._get_json(ctx, "/api/profiles/favorites")
    if fs != 200 or not isinstance(fav, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/profiles/favorites bad response (status={fs})",
                          observed={"status": fs, "body": fav})
    ids = fav.get("ids")
    if not (_is_int(fav.get("user_mask")) and _is_int(fav.get("builtin_mask"))
            and isinstance(ids, list) and all(_is_int(i) for i in ids)):
        return CaseResult(Verdict.FAIL, reason="favorites body malformed (user_mask/builtin_mask/ids)",
                          observed={"body": fav})
    ps, profiles = W._get_json(ctx, "/api/profiles")
    if ps != 200 or not isinstance(profiles, list):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/profiles bad response (status={ps})",
                          observed={"status": ps})
    listed = {p.get("id") for p in profiles if isinstance(p, dict)}
    orphans = [i for i in ids if i < 128 and i not in listed]
    if orphans:
        return CaseResult(Verdict.FAIL, reason=f"favorite user id(s) {orphans} not listed in /api/profiles",
                          observed={"orphans": orphans, "ids": ids})
    bad_builtin = [i for i in ids if i >= 128 and not (fav["builtin_mask"] >> (i - 128)) & 1]
    if bad_builtin:
        return CaseResult(Verdict.FAIL, reason=f"builtin favorite id(s) {bad_builtin} have no builtin_mask bit",
                          observed={"builtin_mask": fav["builtin_mask"], "ids": ids})
    groups = group_profiles_for_picker([p for p in profiles if isinstance(p, dict)], ids)
    fav_listed = [i for i in ids if i in listed]
    in_g1 = {p["id"] for p in groups["favorites"]}
    stray = [i for i in fav_listed if i not in in_g1]
    leaked = [p["id"] for p in groups["recent"] + groups["all"] if p["id"] in set(ids)]
    if stray or leaked:
        return CaseResult(Verdict.FAIL, reason=f"grouping port wrong: not in favorites {stray}, leaked {leaked}",
                          observed={"ids": ids})
    missing, err = _static_missing(ctx, "/", ['id="profileSelect"', "/api/profiles/favorites", "appendGroup('Favorites'"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed={})
    obs = {"ids": ids, "favorites_group": sorted(in_g1)}
    if missing:
        return _static_fail("/", missing, obs)
    if not ids:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no favorites on this bench; ordering cannot be observed",
                          observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-03 (observer of HP-01, window hp01_tick)
# ---------------------------------------------------------------------------

def _case_dash03(ctx: dict) -> CaseResult:
    hp01 = ctx.get("_hp01")
    samples = ((ctx.get("_probe_results") or {}).get("hp01_tick") or {}).get("WEB-DASH-03")
    if not isinstance(hp01, dict) or not hp01.get("ok") or not samples:
        return CaseResult(Verdict.NOT_RUN, reason="HP-01 did not run/pass or recorded no web samples")
    valid = [s for s in samples if isinstance(s, dict) and "error" not in s]
    if not valid:
        return CaseResult(Verdict.FAIL, reason=f"ERROR: every /api/profile_exec probe failed: {samples[:3]}",
                          observed={"samples": samples})
    states = [s.get("state") for s in valid]
    obs = {"states": states, "profile_ids": [s.get("profile_id") for s in valid]}
    bad = [s for s in states if s not in _EXEC_STATES]
    if bad:
        return CaseResult(Verdict.FAIL, reason=f"state outside vocabulary: {bad}", observed=obs)
    if not any(s.get("state") == "running" and s.get("profile_id") == BENCH_PROFILE_SLOT_ID for s in valid):
        return CaseResult(Verdict.FAIL,
                          reason=f"HP-01 ran but no web sample read running with profile_id {BENCH_PROFILE_SLOT_ID}",
                          observed=obs)
    if states[-1] != "done":
        return CaseResult(Verdict.FAIL, reason=f"last web sample before cleanup is {states[-1]!r}, not 'done'",
                          observed=obs)
    missing, err = _static_missing(ctx, "/", ['id="runBtn"', 'id="profileSelect"', "/api/profile_exec/start"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed=obs)
    if missing:
        return _static_fail("/", missing, obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-04 (owner 2026-10-09: accepted recommendation)
# ---------------------------------------------------------------------------

def _case_dash04(ctx: dict) -> CaseResult:
    ps, profiles = W._get_json(ctx, "/api/profiles")
    if ps != 200 or not isinstance(profiles, list):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/profiles bad response (status={ps})", observed={"status": ps})
    ids = [p.get("id") for p in profiles if isinstance(p, dict) and "id" in p]
    if not ids:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no profiles listed", observed={})
    expected_popup: Dict[Any, bool] = {}
    for pid in ids:
        st, body = W._get_json(ctx, f"/api/profile?id={pid}")
        if st != 200 or not isinstance(body, dict):
            return CaseResult(Verdict.FAIL, reason=f"GET /api/profile?id={pid} status={st}",
                              observed={"id": pid, "status": st})
        top = body.get("feasibility")
        if top not in _FEAS_VALUES:
            return CaseResult(Verdict.FAIL, reason=f"profile {pid} feasibility {top!r} missing or outside {_FEAS_VALUES}",
                              observed={"id": pid, "feasibility": top})
        for seg in body.get("segments") or []:
            sf = seg.get("feasibility") if isinstance(seg, dict) else None
            if sf not in _FEAS_VALUES:
                return CaseResult(Verdict.FAIL, reason=f"profile {pid} segment feasibility {sf!r} outside {_FEAS_VALUES}",
                                  observed={"id": pid, "segment_feasibility": sf})
        expected_popup[pid] = top != "ok"
    obs = {"expected_popup": expected_popup}
    missing, err = _static_missing(ctx, "/", ['id="feasPopupOverlay"', "feasPopupProceedBtn", "feasSummarize("])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed=obs)
    if missing:
        return _static_fail("/", missing, obs)
    if not any(expected_popup.values()):
        return CaseResult(Verdict.INCONCLUSIVE, reason="every profile reads feasibility 'ok'; popup branch not showable",
                          observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-05 (mutating: zone 0 Kp, restored in finally)
# ---------------------------------------------------------------------------

_PID_TOL = {"kp": 1e-6, "ki": 1e-9, "kd": 1e-6}


def _gains(zone: dict) -> Optional[Dict[str, float]]:
    try:
        return {k: float(zone[f"pid_{k}"]) for k in ("kp", "ki", "kd")}
    except (KeyError, TypeError, ValueError):
        return None


def _gains_match(a: Dict[str, float], b: Dict[str, float]) -> bool:
    return all(abs(a[k] - b[k]) <= _PID_TOL[k] for k in a)


def _read_zone0(ctx: dict) -> Tuple[Optional[int], Optional[Dict[str, float]]]:
    st, body = W._get_json(ctx, "/api/zones")
    zones = body.get("zones") if isinstance(body, dict) else None
    if st != 200 or not isinstance(zones, list) or not zones or not isinstance(zones[0], dict):
        return st, None
    return st, _gains(zones[0])


def _fmt(v: float) -> str:
    return "%.9g" % v


def _case_dash05(ctx: dict) -> CaseResult:
    reason = _mutating_gate(ctx)
    if reason:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"mutating gate not held, nothing written: {reason}")
    st, orig = _read_zone0(ctx)
    if orig is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"zone 0 gains not readable (status={st}); nothing written")
    missing, err = _static_missing(ctx, "/", ['id="pidPopupOverlay"', "pidPopupApplyBtn", "/api/zones/pid"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed={})
    if missing:
        return _static_fail("/", missing, {"original": orig})
    test_kp = orig["kp"] + 0.125
    if test_kp > 1000:
        test_kp = orig["kp"] - 0.125
    obs: Dict[str, Any] = {"original": orig, "test_kp": test_kp}
    write_ok = False
    after: Optional[Dict[str, float]] = None
    try:
        ws, wb = W._post_json(ctx, "/api/zones/pid", {
            "zone": "0", "kp": _fmt(test_kp), "ki": _fmt(orig["ki"]), "kd": _fmt(orig["kd"])})
        write_ok = ws == 200 and isinstance(wb, dict) and wb.get("ok") is True
        _s, after = _read_zone0(ctx)
    finally:
        rs, rb = W._post_json(ctx, "/api/zones/pid", {
            "zone": "0", "kp": _fmt(orig["kp"]), "ki": _fmt(orig["ki"]), "kd": _fmt(orig["kd"])})
        restore_ok = rs == 200 and isinstance(rb, dict) and rb.get("ok") is True
        _s2, restored = _read_zone0(ctx)
        restore_matches = restored is not None and _gains_match(restored, orig)
        obs["restore"] = {"post_ok": restore_ok, "readback": restored, "matches": restore_matches}
    if not (restore_ok and restore_matches):
        return CaseResult(Verdict.FAIL, reason="RESTORE FAILED: zone 0 gains did not round-trip to the original",
                          observed=obs)
    obs["after_write"] = after
    if not write_ok:
        return CaseResult(Verdict.FAIL, reason="POST /api/zones/pid did not answer 200 {ok:true}", observed=obs)
    expected = dict(orig, kp=test_kp)
    if after is None or not _gains_match(after, expected):
        return CaseResult(Verdict.FAIL, reason="read-back after write does not match the written gains", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-06 (observer of HP-04, window hp04_states)
# ---------------------------------------------------------------------------

def _case_dash06(ctx: dict) -> CaseResult:
    hp04 = ctx.get("_hp04")
    samples = ((ctx.get("_probe_results") or {}).get("hp04_states") or {}).get("WEB-DASH-06")
    if not isinstance(hp04, dict) or not samples:
        return CaseResult(Verdict.NOT_RUN, reason="HP-04 did not run or recorded no web states")
    errs = [s for s in samples if not isinstance(s, dict) or "error" in s]
    if len(errs) == len(samples):
        return CaseResult(Verdict.FAIL, reason=f"ERROR: every /api/profile_exec probe failed: {samples[:2]}",
                          observed={"samples": samples})
    states = [s.get("state") if isinstance(s, dict) else None for s in samples]
    obs = {"web_states": states, "mcp_paused_state": hp04.get("paused_state")}
    bad = [s for s in states if s is not None and s not in _EXEC_STATES]
    if bad:
        return CaseResult(Verdict.FAIL, reason=f"state outside vocabulary: {bad}", observed=obs)
    if len(states) < 2 or states[1] is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="paused sample not captured", observed=obs)
    if hp04.get("paused_state") == "paused" and states[1] != "paused":
        return CaseResult(Verdict.FAIL,
                          reason=f"MCP says paused but the web state at the pause point reads {states[1]!r}",
                          observed=obs)
    if states[0] != "running":
        return CaseResult(Verdict.FAIL, reason=f"first web sample reads {states[0]!r}, expected 'running'", observed=obs)
    missing, err = _static_missing(ctx, "/app.js", [
        "kc-stop-bar", "kc-pause-btn", "STOP FIRING", "ACKNOWLEDGE (firing complete)", "/api/profile_exec/pause"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed=obs)
    if missing:
        return _static_fail("/app.js", missing, obs)
    if not any(s in ("done", "faulted") for s in states[2:]):
        return CaseResult(Verdict.INCONCLUSIVE, reason="no done/faulted reading captured; ACK branch unobservable",
                          observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-07 (owner 2026-10-09: read-only; ack only a bench-slot record)
# ---------------------------------------------------------------------------

def _case_dash07(ctx: dict) -> CaseResult:
    st, body = W._get_json(ctx, "/api/profile_exec")
    if st != 200 or not isinstance(body, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/profile_exec status={st}", observed={"status": st})
    lr = body.get("last_run")
    if not isinstance(lr, dict) or not isinstance(lr.get("present"), bool):
        return CaseResult(Verdict.FAIL, reason="last_run missing or without boolean 'present'", observed={"body": body})
    obs: Dict[str, Any] = {"last_run": lr}
    if lr["present"]:
        if (not isinstance(lr.get("interrupted"), bool) or lr.get("phase") not in _LAST_RUN_PHASES
                or not _is_int(lr.get("profile_id"))):
            return CaseResult(Verdict.FAIL, reason="present last_run has missing/invalid interrupted/phase/profile_id",
                              observed=obs)
        if lr["interrupted"] and lr["phase"] not in ("running", "paused"):
            return CaseResult(Verdict.FAIL, reason="interrupted=true while phase is not running/paused", observed=obs)
        obs["card_expected_visible"] = bool(lr["interrupted"] or lr["phase"] == "faulted")
    missing, err = _static_missing(ctx, "/", ['id="lastRunBanner"', "ackLastRunBtn", "/api/profile_exec/ack_last_run"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed=obs)
    if missing:
        return _static_fail("/", missing, obs)
    if not lr["present"]:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no last_run record on this boot; card cannot be observed",
                          observed=obs)
    # Ack is permanent: press only the harness's own bench-slot record, and
    # only under the standard mutating gate.
    if lr["profile_id"] == BENCH_PROFILE_SLOT_ID and _mutating_gate(ctx) is None:
        as_, atext = W._post_raw(ctx, "/api/profile_exec/ack_last_run", {})
        s2, b2 = W._get_json(ctx, "/api/profile_exec")
        after = (b2 or {}).get("last_run") if isinstance(b2, dict) else None
        obs["ack"] = {"status": as_, "text": atext, "after": after}
        if as_ != 200 or not isinstance(after, dict) or after.get("present") is not False:
            return CaseResult(Verdict.FAIL, reason="ack_last_run did not answer 200 with present:false afterwards",
                              observed=obs)
    else:
        obs["ack"] = "not pressed (record is not the bench slot's, or gate not held)"
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-08 (observer of OT-B01, windows otb01_tripped / otb01_cleared)
# ---------------------------------------------------------------------------

def _banner_predicate(b: Any) -> bool:
    return (isinstance(b, dict) and bool(b.get("diag_ever_received")) and b.get("diag_state") == 4
            and isinstance(b.get("diag_age_ms"), (int, float)) and b["diag_age_ms"] < 1500)


def _status_probe(ctx: dict) -> dict:
    sleep = ctx.get("_sleep", time.sleep)
    bodies: List[Any] = []
    for i in range(3):
        st, body = W._get_json(ctx, "/api/status")
        bodies.append(body if st == 200 and isinstance(body, dict) else {"error": f"status={st}"})
        if _banner_predicate(bodies[-1]):
            break
        if i < 2:
            sleep(1.0)
    return {"bodies": bodies}


def _status_probe_once(ctx: dict) -> dict:
    st, body = W._get_json(ctx, "/api/status")
    return {"body": body if st == 200 and isinstance(body, dict) else {"error": f"status={st}"}}


def _case_dash08(ctx: dict) -> CaseResult:
    otb = ctx.get("_otb01")
    pr = ctx.get("_probe_results") or {}
    if not isinstance(otb, dict):
        return CaseResult(Verdict.NOT_RUN, reason="OT-B01 did not run")
    outcome = otb.get("outcome")
    if outcome != "s6a_latched":
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"no trip window this run (outcome={outcome!r})",
                          observed={"outcome": outcome})
    tripped = (pr.get("otb01_tripped") or {}).get("WEB-DASH-08")
    if tripped is None:
        return CaseResult(Verdict.NOT_RUN, reason="otb01_tripped probe result absent")
    if "error" in tripped:
        return CaseResult(Verdict.FAIL, reason=f"ERROR: tripped probe raised: {tripped['error']}", observed={})
    bodies = tripped.get("bodies") or []
    obs: Dict[str, Any] = {"tripped_bodies": bodies}
    hit = [b for b in bodies if _banner_predicate(b) and b.get("diag_trip_reason") == 6]
    if not hit:
        return CaseResult(Verdict.FAIL, reason="MCP saw reason 6 latched but no web /api/status body meets the banner predicate",
                          observed=obs)
    cleared = (pr.get("otb01_cleared") or {}).get("WEB-DASH-08")
    if cleared is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason="clear was not confirmed; cleared-state body not captured",
                          observed=obs)
    if "error" in cleared:
        return CaseResult(Verdict.FAIL, reason=f"ERROR: cleared probe raised: {cleared['error']}", observed=obs)
    cb = cleared.get("body")
    obs["cleared_body"] = cb
    if not isinstance(cb, dict) or "error" in cb:
        return CaseResult(Verdict.FAIL, reason=f"ERROR: cleared /api/status unreadable: {cb}", observed=obs)
    if _banner_predicate(cb):
        return CaseResult(Verdict.FAIL, reason="cleared body still meets the trip-banner predicate", observed=obs)
    missing, err = _static_missing(ctx, "/", ['id="safetyTripBanner"', "clearTripBtn", "/api/safety/clear_trip"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed=obs)
    if missing:
        return _static_fail("/", missing, obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-09
# ---------------------------------------------------------------------------

_HISTORY_HEADER = ("elapsed_s,desired_c"
                   + "".join(f",z{i}_actual_c,z{i}_duty,z{i}_guard" for i in range(3)) + ",zone_mask")


def _case_dash09(ctx: dict) -> CaseResult:
    hp01 = ctx.get("_hp01")
    if not isinstance(hp01, dict) or not hp01.get("ok"):
        return CaseResult(Verdict.NOT_RUN, reason="HP-01 did not run/pass")
    st, text = W._get_text(ctx, "/api/history.csv")
    if st != 200 or text is None:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/history.csv status={st}", observed={"status": st})
    lines = [ln for ln in text.replace("\r", "").split("\n") if ln.strip()]
    if not lines or lines[0].strip() != _HISTORY_HEADER:
        return CaseResult(Verdict.FAIL, reason="history.csv header does not match the 3-zone emitter format",
                          observed={"header": lines[0] if lines else None})
    rows = lines[1:]
    obs: Dict[str, Any] = {"row_count": len(rows)}
    if len(rows) < 2:
        return CaseResult(Verdict.FAIL, reason=f"history has {len(rows)} data row(s); need at least 2", observed=obs)
    elapsed: List[float] = []
    masks: List[int] = []
    width = len(_HISTORY_HEADER.split(","))
    for n, row in enumerate(rows):
        f = row.split(",")
        try:
            vals = [float(x) for x in f]
        except ValueError:
            return CaseResult(Verdict.FAIL, reason=f"row {n} has a non-numeric field: {row!r}", observed=obs)
        if len(f) < 11 or len(f) != width:
            return CaseResult(Verdict.FAIL, reason=f"row {n} has {len(f)} fields, expected {width}", observed=obs)
        elapsed.append(vals[0])
        masks.append(int(vals[-1]))
    if any(b < a for a, b in zip(elapsed, elapsed[1:])):
        return CaseResult(Verdict.FAIL, reason="elapsed_s is not non-decreasing", observed=obs)
    if not any(m & 1 for m in masks):
        return CaseResult(Verdict.FAIL, reason="no row has zone 0 in its zone_mask", observed=obs)
    return CaseResult(Verdict.PASS, observed=obs)


# ---------------------------------------------------------------------------
# WEB-DASH-10 / 11 / 12
# ---------------------------------------------------------------------------

def _case_dash10(ctx: dict) -> CaseResult:
    st, body = W._get_json(ctx, "/api/ota/esp/status")
    if st != 200 or not isinstance(body, dict):
        return CaseResult(Verdict.FAIL, reason=f"GET /api/ota/esp/status status={st}", observed={"status": st})
    rm = body.get("recovery_mode")
    if not isinstance(rm, bool):
        return CaseResult(Verdict.FAIL, reason="recovery_mode missing or not a boolean", observed={"body": body})
    missing, err = _static_missing(ctx, "/app.js", ["kc-recovery-banner", "/api/ota/esp/status", "st.recovery_mode"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed={"recovery_mode": rm})
    if missing:
        return _static_fail("/app.js", missing, {"recovery_mode": rm})
    if rm:
        return CaseResult(Verdict.INCONCLUSIVE, reason="board is in recovery mode; that branch belongs to OT-E11",
                          observed={"recovery_mode": rm})
    return CaseResult(Verdict.PASS, observed={"recovery_mode": rm})


def _case_dash11(ctx: dict) -> CaseResult:
    st, body = W._get_json(ctx, "/api/readiness")
    items = body.get("items") if isinstance(body, dict) else None
    if st != 200 or not isinstance(items, list) or not items:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/readiness status={st} or no items list",
                          observed={"status": st})
    for it in items:
        if not isinstance(it, dict) or not it.get("key") or it.get("status") not in _READINESS_STATUSES:
            return CaseResult(Verdict.FAIL, reason=f"readiness item invalid: {it!r}", observed={"item": it})
    expected = any(it["status"] == "not_done" for it in items)
    obs = {"banner_expected": expected, "item_count": len(items)}
    missing, err = _static_missing(ctx, "/app.js", ["kc-setup-banner", "/api/readiness", "'not_done'"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed=obs)
    if missing:
        return _static_fail("/app.js", missing, obs)
    return CaseResult(Verdict.PASS, observed=obs)


def _case_dash12(ctx: dict) -> CaseResult:
    st, body = W._get_json(ctx, "/api/profile_exec")
    if st != 200 or not isinstance(body, dict) or "state" not in body:
        return CaseResult(Verdict.FAIL, reason=f"heartbeat route /api/profile_exec status={st} or no state",
                          observed={"status": st})
    missing, err = _static_missing(ctx, "/app.js", [
        "FAILURES_BEFORE_BANNER = 2", "kc-conn-banner", "fetch('/api/profile_exec')",
        "consecutiveFailures >= FAILURES_BEFORE_BANNER"])
    if err:
        return CaseResult(Verdict.FAIL, reason=err, observed={})
    if missing:
        return _static_fail("/app.js", missing, {})
    return CaseResult(Verdict.PASS, observed={"state": body.get("state")})


_CASE_FUNCS = {
    "WEB-DASH-02": _case_dash02,
    "WEB-DASH-03": _case_dash03,
    "WEB-DASH-04": _case_dash04,
    "WEB-DASH-05": _case_dash05,
    "WEB-DASH-06": _case_dash06,
    "WEB-DASH-07": _case_dash07,
    "WEB-DASH-08": _case_dash08,
    "WEB-DASH-09": _case_dash09,
    "WEB-DASH-10": _case_dash10,
    "WEB-DASH-11": _case_dash11,
    "WEB-DASH-12": _case_dash12,
}

for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn

get_case("WEB-DASH-03").window_probe = ("hp01_tick", _exec_probe)
get_case("WEB-DASH-06").window_probe = ("hp04_states", _exec_probe)
get_case("WEB-DASH-08").extra_window_probes = (("otb01_cleared", _status_probe_once),)
get_case("WEB-DASH-08").window_probe = ("otb01_tripped", _status_probe)
