"""HP-* cases -- short heating profiles run on the hidden bench slot (plan
doc section 3.6, Wave 1b).

Every case here MAY heat the fixture (owner decision -- heat runs are
pre-authorized, memory feedback_heat_runs_preauthorized; the standing safety
obligations, including refusing to start on an unacknowledged crash report
or a latched trip, still apply and are enforced by `_capability_preflight_ok`
below before any profile is ever started) and MUST restore board state in
`finally`: stop the firing if it is still running and delete the hidden
bench profile it wrote (`_cleanup_bench_profile`).

Board access goes through the same in-process client objects the MCP tools
in `mcp_server_profiles.py` / `mcp_server_safety.py` / `mcp_server_io.py`
already wrap (`srv._profiles`, `srv._safety`, `srv._thermo`) -- never a
second MCP server, never hardware directly, and never a re-implementation of
what those modules already do. `_srv()` and `_http_get_json()` are the same
helpers cases_smoke.py defines; imported from there rather than duplicated.
"""
from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

from . import judgments as J
from .cases_smoke import _http_get_json, _srv
from .registry import CaseResult, Verdict, get_case

#: The hidden bench-profile slot (plan doc section 7, owner decision 3: "i
#: intended there to be 100 user profiles and 1 running profile for live
#: edits. please place this after that with no visibility to the user.").
#: `slots100` (a sibling change, not yet on `main` as of this wave) owns the
#: concrete slot layout and is expected to land PROFILES_MAX_COUNT=100 with
#: a live-edit slot at 100 and this hidden bench slot at 101. Until that
#: lands, today's PROFILES_MAX_COUNT is 8 (user slots 0-7); slot 7 is
#: reused here ONLY as an interim stand-in -- every case in this module
#: refers to this constant, never a second literal 7.
#: TODO(slots100): switch to 101 once PROFILES_MAX_COUNT == 100 on main.
BENCH_PROFILE_SLOT_ID = 7

BENCH_PROFILE_NAME = "BENCH_HP"
REST_BAND_C = 2.0
REST_TIMEOUT_S = 25 * 60.0
REST_POLL_S = 5.0


def _zone_temps(ctx: dict) -> Dict[int, float]:
    """{zone_index: temperature_c} for every *valid* thermo channel -- the
    same reading `thermo_read()` wraps, taken structured rather than parsed
    out of its display text."""
    srv = _srv(ctx)
    readings = srv._thermo.read()
    return {r.channel: r.temperature_c for r in readings if r.valid}


def _link_stats_dict(ctx: dict) -> Dict[str, Any]:
    srv = _srv(ctx)
    stats = srv._safety.get_link_stats()
    return {
        "crc_errors": getattr(stats, "crc_errors", None),
        "timeouts": getattr(stats, "timeouts", None),
        "broadcast_dropped": getattr(stats, "broadcast_dropped", None),
    }


def _read_energized(ctx: dict) -> Optional[bool]:
    """`dashboard_status_t.safety_relay_energized` -- null/true/false over
    HTTP (dashboard_status_http.c). Returns None (not a crash) if there is
    no host in ctx or the request fails; callers treat an all-None sample
    set as INCONCLUSIVE, never a silent PASS."""
    from .. import dashboard_http_client

    host = ctx.get("host")
    if not host:
        return None
    try:
        status = dashboard_http_client.get_status(host)
    except Exception:
        return None
    return status.get("safety_relay_energized")


def _http_post(host: str, path: str, timeout: float = 5.0) -> Tuple[Optional[int], Any]:
    """Bare, unauthenticated POST -- HP-06 needs to observe exactly what an
    anonymous client sees, so this deliberately does not attach a session."""
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_raw = resp.read()
            try:
                body = json.loads(body_raw) if body_raw else {}
            except ValueError:
                body = body_raw.decode("utf-8", "replace")
            return resp.status, body
    except urllib.error.HTTPError as exc:
        return exc.code, exc.reason
    except OSError as exc:
        return None, str(exc)


def _capability_preflight_ok(ctx: dict) -> Tuple[bool, str]:
    """Refuses to start a heat case if `capability_preflight` reports the
    board is not ready (unacknowledged crash report, a latched trip,
    E-stop, or simply unreachable) -- the same gate `run_preflight()` uses
    at the top of a whole run, applied again here per-case since a heat
    case may run standalone."""
    from .. import capability_preflight

    run_fn = ctx.get("capability_preflight_run", capability_preflight.run_preflight)
    host = ctx.get("host") or capability_preflight.PREFLIGHT_AP_DEFAULT_HOST
    try:
        report = run_fn({}, host)
    except Exception as exc:
        return False, f"capability_preflight raised {type(exc).__name__}: {exc}"
    if report.ok:
        return True, ""
    board = report.board
    if getattr(board, "crash_unacknowledged", False):
        return False, f"refusing to heat: unacknowledged crash report ({getattr(board, 'crash_summary', '')})"
    if getattr(board, "readiness_blocked", False):
        return False, "refusing to heat: capability_preflight reports the board is readiness-blocked"
    if not getattr(board, "reachable", True):
        return False, f"refusing to heat: board unreachable ({getattr(board, 'error', '')})"
    return False, "refusing to heat: capability_preflight reports the board is not ready"


#: Small margin above the planned target a zone's `max_temp_c` ceiling must
#: clear before a normal (ambient+offset) heat case is allowed to start --
#: distinguishes a genuine "the ceiling is too low" preflight FAIL from
#: `profiles.start()`'s own generic refusal, which reads exactly like any
#: other case failure and gives no hint that the real cause is a leftover
#: lowered ceiling (e.g. an HP-07 restore that silently failed -- see
#: `_case_hp07`'s "must ALWAYS be surfaced" restore-note handling).
_CEILING_PREFLIGHT_MARGIN_C = 1.0


def _check_zone_ceilings(ctx: dict, zone_mask: int, target_c: float) -> Tuple[bool, str]:
    """Reads the live zones config ONCE and refuses (before ever touching
    `profiles.save`/`profiles.start`) if any zone participating in
    `zone_mask` already has a `max_temp_c` ceiling below `target_c` plus
    `_CEILING_PREFLIGHT_MARGIN_C`. Never raises the ceiling itself -- this is
    a read-only diagnostic gate, not a repair. Best-effort: a missing host,
    a read failure, or a zones payload with no `max_temp_c` for a given zone
    all pass through (True) rather than mask a real error behind a
    preflight-tooling failure -- `profiles.save`/`profiles.start` still
    apply their own refusal in that case, just without this case's more
    specific reason."""
    host = ctx.get("host")
    if not host:
        return True, ""
    from .. import zones_http_client

    get_zones = ctx.get("_get_zones_config", zones_http_client.get_zones)
    try:
        snapshot = get_zones(host)
    except Exception:
        return True, ""
    return _ceiling_problem(snapshot, zone_mask, target_c)


def _ceiling_problem(snapshot: dict, zone_mask: int, target_c: float) -> Tuple[bool, str]:
    """The pure half of `_check_zone_ceilings`, against an already-fetched
    GET /api/zones snapshot (HP-07 reuses it on the snapshot it is about to
    bake into its own restore body). `max_temp_c <= 0` is skipped:
    0.0 is firmware's "no ceiling configured" sentinel, which
    `profile_executor_run.c`'s `profile_zones_have_ceiling()` refuses with
    its own specific message -- it is not a lowered leftover."""
    for zone in snapshot.get("zones", []) or []:
        idx = zone.get("index")
        if idx is None or not (zone_mask & (1 << idx)):
            continue
        max_temp_c = zone.get("max_temp_c")
        if max_temp_c is None or not (max_temp_c > 0.0):
            continue
        if max_temp_c < target_c + _CEILING_PREFLIGHT_MARGIN_C:
            return False, (
                f"zone {idx} max_temp_c {max_temp_c:.1f}C is below planned target "
                f"{target_c:.1f}C + {_CEILING_PREFLIGHT_MARGIN_C:.1f}C margin -- possibly a "
                f"leftover from a failed HP-07 restore; never raised automatically"
            )
    return True, ""


def _start_bench_profile(
    ctx: dict, zone_mask: int, target_offset_c: float = 15.0,
    ramp_c_per_hr: float = 600.0, dwell_min: int = 2,
    target_c: Optional[float] = None,
) -> Tuple[bool, str, Optional[float]]:
    """Refuses to start (never touches the board) if `capability_preflight`
    is not ok. Otherwise saves a fresh one-segment profile into
    BENCH_PROFILE_SLOT_ID -- target = the current ambient reference +
    `target_offset_c`, plan doc section 3.6's "target = ambient + 15C, ramp
    600C/h, dwell 2min" -- and starts it. Returns (ok, reason,
    ambient_reference_c). An explicit `target_c` overrides the
    ambient + `target_offset_c` computation: HP-07 pins the target to a
    limit it already derived from an EARLIER ambient reading, and a fresh
    re-read here that drifted up would otherwise put target above limit."""
    ok, reason = _capability_preflight_ok(ctx)
    if not ok:
        return False, reason, None
    zone_temps = _zone_temps(ctx)
    if not zone_temps:
        return False, "no valid thermo reading to use as an ambient reference", None
    ambient = min(zone_temps.values())
    explicit_target = target_c is not None
    if target_c is None:
        target_c = ambient + target_offset_c
    if not explicit_target:
        # Only the normal ambient+offset path is checked here -- HP-07 (via
        # `_run_hp07_profile`) passes an explicit `target_c` pinned to a
        # deliberately-lowered `max_temp_c` limit it just set itself, and
        # that intentional target == limit is exactly what this check would
        # otherwise flag.
        ceiling_ok, ceiling_reason = _check_zone_ceilings(ctx, zone_mask, target_c)
        if not ceiling_ok:
            return False, ceiling_reason, ambient
    srv = _srv(ctx)
    from .. import devices

    segments = [devices.ProfileSegment(target_c=target_c, ramp_c_per_hr=ramp_c_per_hr, dwell_min=dwell_min)]
    try:
        save_result = srv._profiles.save(BENCH_PROFILE_SLOT_ID, BENCH_PROFILE_NAME, zone_mask, segments)
    except Exception as exc:
        return False, f"profiles.save raised {type(exc).__name__}: {exc}", ambient
    if not save_result.ok:
        return False, f"profiles.save refused: {save_result.error}", ambient
    # From here on the hidden slot HAS been written, so every failure path
    # below must tear it down itself: the callers only enter their own
    # `finally: _cleanup_bench_profile(ctx)` once this function has returned
    # ok, so a start that refuses or raises would otherwise leave BENCH_HP
    # sitting in a user-visible slot forever.
    try:
        start_result = srv._profiles.start(BENCH_PROFILE_SLOT_ID)
    except Exception as exc:
        _cleanup_bench_profile(ctx)
        return False, f"profiles.start raised {type(exc).__name__}: {exc}", ambient
    if not start_result.ok:
        _cleanup_bench_profile(ctx)
        return False, f"profiles.start refused: {start_result.error}", ambient
    return True, "", ambient


#: Key `_cleanup_bench_profile` accumulates pre-delete firing-history
#: snapshots into (a list, since a suite runs several HP cases back to back
#: and each one's teardown adds its own snapshot). `_case_hp08` reads this
#: list rather than a live GET, since by the time it runs the hidden slot's
#: history has already been erased by every prior case's own cleanup.
HP_FIRING_HISTORY_SNAPSHOTS_KEY = "_hp_firing_history_snapshots"


def _cleanup_bench_profile(ctx: dict) -> None:
    """Best-effort restore, called from every case's `finally`: stop
    whatever is still running, snapshot the hidden slot's firing history,
    and delete the hidden bench slot. Never raises -- a cleanup failure must
    not mask the case's own verdict, and the next case's own preflight/
    rest-gate will catch a board left in a bad state.

    The history snapshot must happen BEFORE the delete: `profiles_http_
    delete()` erases the slot's firing history (`profiles_http.c:1004`,
    `firing_stats_erase`), so any case (e.g. HP-08) that wants to judge what
    a run actually recorded has to read it here, in the one place every HP
    case's teardown funnels through, or it will always find the record
    already gone."""
    srv = _srv(ctx)
    # Stop UNCONDITIONALLY, in its own try: "stopping the host does not stop
    # a firing" (memory project_stopping_host_does_not_stop_firing), so the
    # stop must not be conditional on a get_exec_status() that may itself
    # raise -- reading the state and stopping used to share one try, which
    # meant a failed status read silently skipped the stop and left the
    # fixture heating. profiles_stop() on an already-idle executor is a
    # harmless no-op, so there is nothing to gain from asking first.
    try:
        srv._profiles.stop()
    except Exception:
        pass
    host = ctx.get("host")
    if host:
        try:
            get_json = ctx.get("_http_get_json", _http_get_json)
            status, body = get_json(host, f"/api/firing_history?profile_id={BENCH_PROFILE_SLOT_ID}")
            if status == 200:
                ctx.setdefault(HP_FIRING_HISTORY_SNAPSHOTS_KEY, []).append(body)
        except Exception:
            pass
    try:
        srv._profiles.delete(BENCH_PROFILE_SLOT_ID)
    except Exception:
        pass


def _rest_gate(ctx: dict, timeout_s: float = REST_TIMEOUT_S, poll_s: float = REST_POLL_S) -> Tuple[bool, str]:
    """Plan doc section 2.4/5.2 rule 7: all zones within REST_BAND_C of an
    ambient reference (and of each other) before a heat case may start.
    `ctx["_now"]`/`ctx["_sleep"]` are injectable for tests; default to real
    wall-clock time."""
    now = ctx.get("_now", time.monotonic)
    sleep = ctx.get("_sleep", time.sleep)
    deadline = now() + timeout_s
    while True:
        temps = _zone_temps(ctx)
        if not temps:
            return False, "no valid thermo reading"
        ref = min(temps.values())
        if J.judge_rested(temps, ref, REST_BAND_C):
            return True, ""
        if now() >= deadline:
            return False, "not_rested"
        sleep(poll_s)


def _hp_run(ctx: dict, zone_mask: int, timeout_s: float = 480.0, poll_s: float = 2.0) -> Dict[str, Any]:
    """Common HP flow: rest gate, start, poll to DONE, collect start/end
    zone temps, K4-energized samples and link-stats before/after. Always
    tears down (stop + delete the bench slot) in `finally`. Stores its
    result under `ctx["_hp01"]`/`ctx["_hp02"]` (by `zone_mask`) so SP-03 and
    SP-06's observer cases can read it back within the same run."""
    result: Dict[str, Any] = {
        "ok": False, "reason": "", "start_zones": {}, "end_zones": {},
        "energized_samples": [], "link_stats_before": {}, "link_stats_after": {},
    }
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        result["reason"] = f"rest gate: {rest_reason}"
        return result
    result["start_zones"] = _zone_temps(ctx)
    try:
        result["link_stats_before"] = _link_stats_dict(ctx)
    except Exception:
        pass
    ok, reason, _ambient = _start_bench_profile(ctx, zone_mask)
    if not ok:
        result["reason"] = reason
        return result
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    try:
        deadline = now() + timeout_s
        state = "running"
        while now() < deadline:
            st = srv._profiles.get_exec_status()
            state = st.state_name
            result["energized_samples"].append((state, _read_energized(ctx)))
            if state in ("done", "faulted"):
                break
            sleep(poll_s)
        result["end_zones"] = _zone_temps(ctx)
        try:
            result["link_stats_after"] = _link_stats_dict(ctx)
        except Exception:
            pass
        if state != "done":
            result["reason"] = f"did not reach DONE within {timeout_s:.0f}s (state={state})"
            return result
        result["ok"] = True
        return result
    finally:
        _cleanup_bench_profile(ctx)


def _rises(start_zones: Dict[int, float], end_zones: Dict[int, float]) -> Dict[int, float]:
    return {z: end_zones[z] - start_zones[z] for z in end_zones if z in start_zones}


def _zone_status(st: Any, target_zone: int) -> Optional[Any]:
    """Look up a single zone's status by its `.zone` field.

    `ProfileExecStatus.zones` is a COMPACTED list of only the participating
    zones (`devices_profiles.py`'s `ZoneExecStatus`), not one entry per zone
    index -- `st.zones[target_zone]` is a positional index into that
    compacted list, which is wrong whenever `target_zone` is not literally
    zone 0 running alone, or whenever a lower-numbered participating zone is
    absent. Returns None if no entry names `target_zone`."""
    for z in st.zones:
        if z.zone == target_zone:
            return z
    return None


def _case_hp01(ctx: dict) -> CaseResult:
    run = _hp_run(ctx, zone_mask=0b001)
    ctx["_hp01"] = run
    if not run["ok"]:
        return CaseResult(Verdict.FAIL, reason=run["reason"], observed={"start": run["start_zones"], "end": run["end_zones"]})
    result = J.judge_zone_rise_ordering(_rises(run["start_zones"], run["end_zones"]), primary_zone=0, min_rise=5.0)
    if result.verdict != Verdict.PASS:
        return result
    return J.judge_relay_energized(run["energized_samples"])


def _case_hp02(ctx: dict) -> CaseResult:
    run = _hp_run(ctx, zone_mask=0b111)
    ctx["_hp02"] = run
    if not run["ok"]:
        return CaseResult(Verdict.FAIL, reason=run["reason"], observed={"start": run["start_zones"], "end": run["end_zones"]})
    return J.judge_all_zones_rise(_rises(run["start_zones"], run["end_zones"]), zone_mask=0b111, min_rise=5.0)


def _case_hp04(ctx: dict) -> CaseResult:
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    ok, reason, _ambient = _start_bench_profile(ctx, zone_mask=0b111)
    if not ok:
        return CaseResult(Verdict.FAIL, reason=reason)
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    try:
        sleep(60)
        pause_result = srv._profiles.pause()
        if not pause_result.ok:
            return CaseResult(Verdict.FAIL, reason=f"profiles.pause refused: {getattr(pause_result, 'reason', '')}")
        st = srv._profiles.get_exec_status()
        paused_state = st.state_name
        duties_while_paused = [z.duty for z in st.zones]
        pause_ui_targets: list = []
        try:
            pause_ui_targets = srv._ui_test.list_tap_targets().get("targets", [])
        except Exception:
            pause_ui_targets = []
        ctx["_hp04"] = {
            "paused_state": paused_state,
            "duties_while_paused": duties_while_paused,
            "pause_ui_targets": pause_ui_targets,
        }
        sleep(60)
        resume_result = srv._profiles.resume()
        if not resume_result.ok:
            return CaseResult(Verdict.FAIL, reason=f"profiles.resume refused: {getattr(resume_result, 'reason', '')}")
        deadline = now() + 420.0
        final_state = "running"
        while now() < deadline:
            st = srv._profiles.get_exec_status()
            final_state = st.state_name
            if final_state in ("done", "faulted"):
                break
            sleep(2)
        return J.judge_pause_resume(paused_state, duties_while_paused, final_state)
    finally:
        _cleanup_bench_profile(ctx)


def _case_hp05(ctx: dict) -> CaseResult:
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    ok, reason, _ambient = _start_bench_profile(ctx, zone_mask=0b111)
    if not ok:
        return CaseResult(Verdict.FAIL, reason=reason)
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    try:
        sleep(90)
        stop_result = srv._profiles.stop()
        if not stop_result.ok:
            return CaseResult(Verdict.FAIL, reason="profiles.stop refused")
        sleep(2)
        st = srv._profiles.get_exec_status()
        duties = [z.duty for z in st.zones]
        relays = [z.relay_commanded_on for z in st.zones]
        ack_result = srv._profiles.ack_last_run()
        return J.judge_stop(st.state_name, duties, relays, bool(ack_result.ok))
    finally:
        _cleanup_bench_profile(ctx)


def _case_hp06(ctx: dict) -> CaseResult:
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    ok, reason, _ambient = _start_bench_profile(ctx, zone_mask=0b001)
    if not ok:
        return CaseResult(Verdict.FAIL, reason=reason)
    host = ctx.get("host")
    sleep = ctx.get("_sleep", time.sleep)
    try:
        sleep(10)
        if not host:
            return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx to send the unauthenticated stop to")
        status, _body = _http_post(host, "/api/profile_exec/stop")
        sleep(2)
        srv = _srv(ctx)
        st = srv._profiles.get_exec_status()
        return J.judge_unauthenticated_stop(status, st.state_name)
    finally:
        _cleanup_bench_profile(ctx)


def _entries_from_body(body: Any) -> List[dict]:
    if isinstance(body, list):
        return body
    if isinstance(body, dict):
        return body.get("records") or body.get("runs") or body.get("entries") or []
    return []


def _case_hp08(ctx: dict) -> CaseResult:
    """`GET /api/firing_history` requires a `profile_id` query parameter
    (`firing_history_get_handler()`, `dashboard_exec_http.c`) -- it has no
    concept of "all profiles" and 400s with no id at all. HP-01..05 all ran
    their firing through the hidden `BENCH_PROFILE_SLOT_ID` slot, so that
    slot's own id is the one history to read back.

    Found on hardware 2026-09-24: reading it live here always finds it
    empty, because every HP case's own `finally` runs
    `_cleanup_bench_profile()`, which deletes the hidden slot --
    `profiles_http_delete()` erases that slot's firing history
    (`profiles_http.c:1004`, `firing_stats_erase`) as part of the delete.
    By the time this case runs (whether standalone after another HP case's
    teardown, or last in a suite), the record is already gone regardless of
    the `profile_id` param being correct. Fix: `_cleanup_bench_profile()`
    snapshots `GET /api/firing_history?profile_id=...` into
    `ctx[HP_FIRING_HISTORY_SNAPSHOTS_KEY]` BEFORE every delete, so this case
    judges whichever snapshot(s) a prior run in this same suite execution
    left behind. A live GET is kept as a fallback (e.g. this case run in
    isolation, immediately after starting a firing by hand, before any
    cleanup has run) but will normally see nothing, same as before this fix."""
    snapshots = ctx.get(HP_FIRING_HISTORY_SNAPSHOTS_KEY)
    if snapshots:
        entries: List[dict] = []
        for body in snapshots:
            entries.extend(_entries_from_body(body))
        return J.judge_firing_history(entries, expected_name_prefix=BENCH_PROFILE_NAME)
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx")
    status, body = _http_get_json(host, f"/api/firing_history?profile_id={BENCH_PROFILE_SLOT_ID}")
    if status != 200:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/firing_history: status={status}", observed={"body": body})
    return J.judge_firing_history(_entries_from_body(body), expected_name_prefix=BENCH_PROFILE_NAME)


def _case_hp03(ctx: dict) -> CaseResult:
    """A zone reconfigured `zonetype`=on/off with hysteresis cycles its
    relay at least twice and never strays far outside the hysteresis band
    (plan doc section 3.6). Zones config is read/POSTed via
    zones_http_client's whole-page GET-merge-POST pattern and restored in
    `finally` regardless of outcome -- this case is the only one in the
    package that touches persistent zone config, not just the hidden
    profile slot, so the restore is not optional."""
    from .. import zones_http_client

    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx")
    target_zone = ctx.get("_hp03_zone_index", 2)
    try:
        snapshot = zones_http_client.get_zones(host)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones failed: {type(exc).__name__}: {exc}")
    try:
        restore_body = zones_http_client.build_post_body(snapshot, {})
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"could not build a restore body from the snapshot: {exc}")
    ambient = min(_zone_temps(ctx).values()) if _zone_temps(ctx) else None
    if ambient is None:
        return CaseResult(Verdict.FAIL, reason="no valid thermo reading to use as an ambient reference")
    hyst_c = 2.0
    target_c = ambient + 10.0
    preset = {"zones": [{
        "index": target_zone, "zone_type": 1, "failsafe_state": False,
        "hyst_c": hyst_c, "min_on_s": 0, "min_off_s": 0,
    }]}
    try:
        on_off_body = zones_http_client.build_post_body(snapshot, preset)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"could not build the on/off preset body: {exc}")
    result: Optional[CaseResult] = None
    restore_note: Optional[str] = None
    inflight: Optional[BaseException] = None
    try:
        # Inside the try so the restore still runs if this POST raises after
        # firmware already committed it (e.g. a read timeout) -- same
        # reasoning as HP-07's lowering POST.
        try:
            post_result = zones_http_client.post_zones(host, on_off_body)
        except Exception as exc:
            result = CaseResult(Verdict.FAIL, reason=f"POST /api/zones (on/off config) failed: {exc}")
        else:
            if post_result != "ok":
                result = CaseResult(Verdict.FAIL, reason=f"POST /api/zones (on/off config) refused: {post_result}")
            else:
                try:
                    result = _run_hp03_profile(ctx, target_zone, target_c, hyst_c)
                except Exception as exc:
                    result = CaseResult(Verdict.FAIL, reason=f"HP-03 run raised {type(exc).__name__}: {exc}")
    except BaseException as exc:
        inflight = exc
        raise
    finally:
        _cleanup_bench_profile(ctx)
        restore_error = _post_zones_restore(ctx, host, restore_body)
        if restore_error is not None:
            restore_note = (
                f"zone {target_zone} zones config restore failed ({restore_error}) -- "
                f"may be left as on/off (zone_type 1); restore by hand"
            )
            _surface_restore_failure_on_inflight(inflight, restore_note)
    if restore_note:
        if result is None:
            return CaseResult(Verdict.FAIL, reason=restore_note)
        combined = f"{result.reason}; {restore_note}" if result.reason else restore_note
        return CaseResult(Verdict.FAIL, reason=combined, observed=result.observed)
    return result


def _run_hp03_profile(ctx: dict, target_zone: int, target_c: float, hyst_c: float) -> CaseResult:
    """HP-03's start/poll/judge body, once the on/off zone config is POSTed.
    Split out so `_case_hp03` owns the restore and can surface it."""
    ok, reason, _ambient = _start_bench_profile(
        ctx, zone_mask=1 << target_zone, target_offset_c=10.0,
    )
    if not ok:
        return CaseResult(Verdict.FAIL, reason=reason)
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    deadline = now() + 360.0
    relay_states: List[bool] = []
    temps_c: List[Optional[float]] = []
    state = "running"
    while now() < deadline:
        st = srv._profiles.get_exec_status()
        state = st.state_name
        zs = _zone_status(st, target_zone)
        if zs is not None:
            relay_states.append(bool(zs.relay_commanded_on))
        zone_temps = _zone_temps(ctx)
        temps_c.append(zone_temps.get(target_zone))
        if state in ("done", "faulted"):
            break
        sleep(2)
    return J.judge_on_off_zone_cycling(relay_states, temps_c, target_c=target_c, hyst_c=hyst_c)


#: Margin above ambient the lowered `max_temp_c` limit is set to (HP-07).
#: The profile's own target is then pinned to that exact `limit_c` value
#: (passed through as `target_c`), never recomputed from a second ambient
#: reading -- any upward drift between two reads would leave
#: `target_c > limit_c`, which `profile_executor_run.c` refuses outright at
#: start time.
_HP07_MARGIN_C = 3.0


#: Retry count/delay for restoring a zone's `max_temp_c` after HP-07 --
#: separate from `_HP07_MARGIN_C`. A board that rebooted mid-run (observed
#: on hardware 2026-09-24, alongside the exception-swallowing bug this
#: constant's caller exists to fix) may not answer the first restore POST;
#: a short wait and one retry gives it a chance to come back before this
#: case gives up and surfaces the failure.
_HP07_RESTORE_ATTEMPTS = 2
_HP07_RESTORE_RETRY_DELAY_S = 5.0


def _post_zones_restore(ctx: dict, host: str, restore_body: str) -> Optional[str]:
    """POSTs a GET-snapshot-derived `restore_body` back to /api/zones,
    retrying once after `_HP07_RESTORE_RETRY_DELAY_S`. Returns None on an
    "ok" response, else the last attempt's error text. Shared by HP-03 and
    HP-07, the two cases that change persistent zone config."""
    from .. import zones_http_client

    sleep = ctx.get("_sleep", time.sleep)
    last_error = ""
    for attempt in range(1, _HP07_RESTORE_ATTEMPTS + 1):
        try:
            restore_result = zones_http_client.post_zones(host, restore_body)
        except Exception as exc:
            last_error = f"{type(exc).__name__}: {exc}"
        else:
            if restore_result == "ok":
                return None
            last_error = f"refused: {restore_result}"
        if attempt < _HP07_RESTORE_ATTEMPTS:
            sleep(_HP07_RESTORE_RETRY_DELAY_S)
    return last_error


def _surface_restore_failure_on_inflight(inflight: Optional[BaseException], note: str) -> None:
    """A BaseException that is not an Exception (KeyboardInterrupt,
    SystemExit, a thread-abort) escapes the case's own `except Exception`
    and the runner's, so a restore failure computed in `finally` would be
    dropped with the return value that never happens. Attach it to the
    in-flight exception and write it to stderr so it can never be silent."""
    if inflight is None:
        return
    try:
        inflight.add_note(note)
    except Exception:
        pass
    try:
        sys.stderr.write(f"bench_test heat: {note}\n")
    except Exception:
        pass


def _restore_zone_limit(
    ctx: dict, host: str, restore_body: str, target_zone: int,
    lowered_limit_c: float, original_max_temp_c: Optional[float],
) -> Optional[str]:
    """POSTs `restore_body` to put zone `target_zone`'s `max_temp_c` back to
    its pre-HP-07 value, retrying once after a short delay in case the board
    rebooted mid-run and simply is not ready to accept an HTTP POST yet on
    the first attempt. Returns None on a confirmed "ok" restore, or a
    message naming the zone, the value the restore attempt(s) left behind,
    and the original value it should have restored -- callers must surface
    this loudly (never swallow it): a zone left at `lowered_limit_c` refuses
    every subsequent case's own profile start (see `_check_zone_ceilings`,
    which exists specifically to catch this class of leftover)."""
    last_error = _post_zones_restore(ctx, host, restore_body)
    if last_error is None:
        return None
    original_note = f"{original_max_temp_c:.1f}C" if original_max_temp_c is not None else "unknown"
    return (
        f"zone {target_zone} max_temp_c restore failed after {_HP07_RESTORE_ATTEMPTS} "
        f"attempt(s) ({last_error}) -- left at {lowered_limit_c:.1f}C, should be {original_note}"
    )


def _run_hp07_profile(ctx: dict, target_zone: int, limit_c: float) -> CaseResult:
    """The heat/poll/ack body of HP-07, once the zone's `max_temp_c` limit is
    already lowered (to `_HP07_MARGIN_C` above ambient) and the hidden slot
    has not yet been started. Starts a profile whose target is exactly
    `limit_c` (the value just POSTed) -- never above the just-lowered limit,
    since `profile_executor_run.c`'s start-time re-validation refuses
    outright (never clamps) a segment target greater than the zone's
    *current* max_temp_c -- and lets the setpoint's approach/overshoot cross
    that limit live, tripping `thermal_guard.c`'s runtime check
    (`measurement_c >= max_temp_c`, evaluated every guard tick)."""
    ok, reason, ambient_at_start = _start_bench_profile(
        ctx, zone_mask=1 << target_zone, target_c=limit_c,
    )
    if not ok:
        return CaseResult(Verdict.FAIL, reason=reason)
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    deadline = now() + 240.0
    st = None
    state = "running"
    #: Every poll's `actual_c` reading for the target zone -- kept so a
    #: future knife-edge failure (state never reaches "faulted" because the
    #: approach/overshoot never actually crossed `limit_c`) is diagnosable
    #: from the case's own `observed` output rather than needing a live
    #: rerun. Does not change the pass criterion.
    actual_c_samples: List[Optional[float]] = []
    while now() < deadline:
        st = srv._profiles.get_exec_status()
        state = st.state_name
        zs = _zone_status(st, target_zone)
        actual_c_samples.append(getattr(zs, "actual_c", None) if zs is not None else None)
        if state == "faulted":
            break
        sleep(2)
    observed = {"limit_c": limit_c, "ambient_at_start": ambient_at_start, "actual_c_samples": actual_c_samples}
    if st is None or state != "faulted":
        return CaseResult(
            Verdict.FAIL,
            reason=f"never reached FAULTED within 240s (last state={state})",
            observed=observed,
        )
    zone_status = _zone_status(st, target_zone)
    faulted = bool(getattr(zone_status, "faulted", False))
    fault_guard = getattr(zone_status, "fault_guard", None)
    ack_result = srv._profiles.stop()
    sleep(2)
    st_after = srv._profiles.get_exec_status()
    result = J.judge_faulted_run(
        state_name=state, faulted=faulted, fault_guard=fault_guard,
        ack_ok=bool(ack_result.ok), post_ack_state=st_after.state_name,
    )
    merged_observed = dict(observed)
    if result.observed:
        merged_observed.update(result.observed)
    return CaseResult(result.verdict, reason=result.reason, observed=merged_observed)


def _case_hp07(ctx: dict) -> CaseResult:
    """HP-01's profile, but the target zone's `max_temp_c` limit is lowered
    to 3C above ambient BEFORE the profile starts, and the profile's own
    target is set to exactly that (already-lowered) limit -- so the
    setpoint's approach/overshoot crosses the limit live and the software
    thermal guard trips it (a `dashboard` zone-config field, never a Pico/
    safety trip). Expects FAULTED with fault_guard naming the over-max-temp
    guard, and confirms the sticky bar's Acknowledge (`profiles.stop()`,
    mirroring the web UI's POST /api/profile_exec/stop) clears it. Restores
    the zone's original max_temp_c in `finally`.

    Found on hardware 2026-09-24 (run 20260924T072516Z_heat), TWICE:

    1. Lowering the limit BEFORE `profiles.start()` never reaches the
       runtime guard at all if the profile's OWN target is still above the
       old (wide) limit -- `profile_executor_run.c`'s start-time
       re-validation (`profile_executor_run.c:320-339`) refuses outright,
       naming "refused, not clamped", whenever a segment's target exceeds
       the zone's *current* max_temp_c at start time; deliberate,
       unconditional (PID_EXPANSION_PLAN.md sec 7.2 -- "a target above the
       kiln's permitted maximum is refused and NEVER stretched"), not a bug.

    2. The first fix (lower the limit AFTER confirming RUNNING) does not
       work either: `POST /api/zones` while RUNNING is refused 409 by
       `ota_http_check_interlocks()` (`zones_http_post.c:44-48`,
       `ota_interlock.c:56-57`) -- deliberate, not a bug, and there is no
       software surface to lower a limit on a zone that is actively firing.

    The only sequencing that reaches the runtime guard at all: lower the
    limit while IDLE (allowed), then start a profile whose OWN target
    equals that lowered limit (`profile_executor_run.c` refuses only a
    target ABOVE the zone's current max_temp_c, so target == limit is
    accepted), and let the live approach/overshoot cross it."""
    from .. import zones_http_client

    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx")
    target_zone = ctx.get("_hp07_zone_index", 0)
    zone_temps = _zone_temps(ctx)
    if not zone_temps:
        return CaseResult(Verdict.FAIL, reason="no valid thermo reading to use as an ambient reference")
    ambient = min(zone_temps.values())
    limit_c = ambient + _HP07_MARGIN_C
    try:
        snapshot = zones_http_client.get_zones(host)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/zones failed: {type(exc).__name__}: {exc}")
    try:
        restore_body = zones_http_client.build_post_body(snapshot, {})
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"could not build a restore body from the snapshot: {exc}")
    preset = {"zones": [{"index": target_zone, "max_temp_c": limit_c}]}
    try:
        limited_body = zones_http_client.build_post_body(snapshot, preset)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"could not build the lowered-limit preset body: {exc}")

    original_max_temp_c: Optional[float] = None
    for zone in snapshot.get("zones", []) or []:
        if zone.get("index") == target_zone:
            original_max_temp_c = zone.get("max_temp_c")
            break
    # The restore body is this snapshot, so a ceiling ALREADY left low by an
    # earlier failed restore would be "restored" to that same leftover value
    # and perpetuated; and one at/below `limit_c` would make the "lowering"
    # POST RAISE the limit. Refuse both rather than guess an original value:
    # anything that would fail the normal cases' own ceiling preflight
    # (ambient + 15C target + margin) needs an operator, not this case.
    ceiling_ok, ceiling_reason = _ceiling_problem(snapshot, 1 << target_zone, ambient + 15.0)
    if not ceiling_ok:
        return CaseResult(Verdict.FAIL, reason=f"refusing to lower/restore: {ceiling_reason}")

    restore_note: Optional[str] = None
    result: Optional[CaseResult] = None
    inflight: Optional[BaseException] = None
    try:
        # Lower the limit WHILE IDLE -- the profile has not been started yet,
        # so this POST is not subject to the RUNNING interlock 409 above.
        # Inside the try so the restore below still runs if this POST raises
        # after firmware already committed it (e.g. a read timeout).
        try:
            post_result = zones_http_client.post_zones(host, limited_body)
        except Exception as exc:
            result = CaseResult(Verdict.FAIL, reason=f"POST /api/zones (lowered max_temp_c) failed: {exc}")
        else:
            if post_result != "ok":
                result = CaseResult(Verdict.FAIL, reason=f"POST /api/zones (lowered max_temp_c) refused: {post_result}")
            else:
                # `_run_hp07_profile` itself is NOT wrapped in a try/except
                # of its own -- it must be caught here, since a raise (e.g.
                # the board rebooting mid-run) previously propagated straight
                # out of this function, past the `finally` block's restore
                # AND past the "if restore_note" check below, silently
                # dropping a failed limit restore (found on hardware
                # 2026-09-24: zone 0's max_temp_c was left stuck at ~36.4C).
                try:
                    result = _run_hp07_profile(ctx, target_zone, limit_c)
                except Exception as exc:
                    result = CaseResult(
                        Verdict.FAIL,
                        reason=f"_run_hp07_profile raised {type(exc).__name__}: {exc}",
                    )
    except BaseException as exc:
        # KeyboardInterrupt/SystemExit: the restore below still runs, but
        # its note can only reach anyone via the exception itself.
        inflight = exc
        raise
    finally:
        # Unconditional: stop whatever is still running and delete the
        # hidden slot before touching zone config again.
        _cleanup_bench_profile(ctx)
        restore_note = _restore_zone_limit(
            ctx, host, restore_body, target_zone, limit_c, original_max_temp_c,
        )
        if restore_note:
            _surface_restore_failure_on_inflight(inflight, restore_note)
    if restore_note:
        # A failed restore must surface in the verdict, never be silently
        # swallowed -- a zone left with a 3C-above-ambient max_temp_c would
        # refuse every subsequent case's own profile start.
        if result is None:
            return CaseResult(Verdict.FAIL, reason=restore_note)
        combined_reason = f"{result.reason}; {restore_note}" if result.reason else restore_note
        return CaseResult(Verdict.FAIL, reason=combined_reason, observed=result.observed)
    return result


_CASE_FUNCS = {
    "HP-01": _case_hp01,
    "HP-02": _case_hp02,
    "HP-03": _case_hp03,
    "HP-04": _case_hp04,
    "HP-05": _case_hp05,
    "HP-06": _case_hp06,
    "HP-07": _case_hp07,
    "HP-08": _case_hp08,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
