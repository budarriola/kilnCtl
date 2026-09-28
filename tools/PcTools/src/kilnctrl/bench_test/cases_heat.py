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

#: Safety margin `_case_hp07` requires between `limit_c` and the highest
#: OTHER (non-`target_zone`) zone's `max_temp_c` before it will lower
#: `target_zone`'s ceiling at all. `abs_max_temp_c` on the Pico safety
#: processor is not per-zone -- `safety_ceiling_policy_target_c()`
#: (firmware/KilnFW/App/drivers/safety/safety_ceiling_policy.c) mirrors it
#: to the MAX of every zone's `max_temp_c`, never the minimum -- so as long
#: as some other commissioned zone's ceiling clears this margin above
#: `limit_c`, lowering `target_zone` alone leaves the Pico's mirrored
#: ceiling unchanged and comfortably above where this case intends to
#: provoke the ESP's own `thermal_guard.c` trip. Refusing here, rather than
#: lowering and hoping, is how this case avoids a SEPARATE, unintended Pico
#: S1 (`abs_max_temp_c`) trip racing (or beating) the ESP software guard it
#: is actually trying to provoke. Note this is distinct from the trip this
#: case DOES intend to provoke: `escalate_guard_trip()`
#: (profile_executor_relay_io.c) classifies `THERMAL_GUARD_TRIP_MAX_TEMP` as
#: a GLOBAL fault, so even the intended ESP thermal_guard trip asserts the
#: safety-link fault line and the Pico latches its own S6a
#: (`SAFETY_TRIP_MAIN_FAULT`) -- `_case_hp07` must call `safety_clear_trip()`
#: for that expected S6a too, not just `profiles.stop()`. This margin exists
#: only to prevent a SECOND, different (S1) Pico trip from also firing.
_HP07_PICO_CEILING_MARGIN_C = 5.0


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


def _hp07_pico_ceiling_headroom_ok(
    snapshot: dict, target_zone: int, limit_c: float, margin_c: float = _HP07_PICO_CEILING_MARGIN_C,
) -> Tuple[bool, str]:
    """Refuses HP-07 outright if lowering `target_zone`'s `max_temp_c` to
    `limit_c` would leave the Pico's mirrored `abs_max_temp_c` -- the MAX of
    every zone's `max_temp_c`, per `safety_ceiling_policy_target_c()` -- at
    or near `limit_c` itself. HP-07 relies on some OTHER commissioned zone's
    ceiling staying comfortably above `limit_c` so lowering only
    `target_zone` never moves the Pico's own ceiling; if no other zone is
    commissioned high enough (e.g. only `target_zone` has a real ceiling, or
    all zones are already near ambient), the Pico's `abs_max_temp_c` would
    drop to ~`limit_c` right alongside the ESP zone ceiling and its
    independent S1 guard could trip at the same threshold the ESP's
    `thermal_guard.c` is deliberately being driven past -- a race this case
    must not run, since a wrongly-provoked S1 would misreport a harness
    design issue as a firmware safety fault and would need its own
    `safety_get_diag()`/`safety_clear_trip()` handling distinct from the S6a
    this case DOES expect (see `_case_hp07`'s docstring: the intended ESP
    thermal_guard trip is a GLOBAL fault and also latches the Pico's S6a,
    which `_case_hp07` clears after confirming the reported reason/mask
    match). Best-effort like `_ceiling_problem`: a zone with no `max_temp_c`
    at all is simply not a candidate, not an error."""
    best_other = 0.0
    for zone in snapshot.get("zones", []) or []:
        idx = zone.get("index")
        if idx is None or idx == target_zone:
            continue
        max_temp_c = zone.get("max_temp_c")
        if max_temp_c is None or not (max_temp_c > 0.0):
            continue
        if max_temp_c > best_other:
            best_other = max_temp_c
    if best_other < limit_c + margin_c:
        return False, (
            f"no other commissioned zone's max_temp_c clears {limit_c:.1f}C + {margin_c:.1f}C "
            f"margin (highest other zone: {best_other:.1f}C) -- lowering zone {target_zone} "
            f"alone would pull the Pico's mirrored abs_max_temp_c (MAX across zones) down to "
            f"~{limit_c:.1f}C too, risking a Pico S1 trip racing the ESP thermal_guard trip "
            f"this case means to provoke"
        )
    return True, ""


def _start_bench_profile(
    ctx: dict, zone_mask: int, target_offset_c: float = 15.0,
    ramp_c_per_hr: float = 600.0, dwell_min: int = 2,
    target_c: Optional[float] = None,
    on_off_rules: "Optional[list]" = None,
) -> Tuple[bool, str, Optional[float]]:
    """Refuses to start (never touches the board) if `capability_preflight`
    is not ok. Otherwise saves a fresh one-segment profile into
    BENCH_PROFILE_SLOT_ID -- target = the current ambient reference +
    `target_offset_c`, plan doc section 3.6's "target = ambient + 15C, ramp
    600C/h, dwell 2min" -- and starts it. Returns (ok, reason,
    ambient_reference_c). An explicit `target_c` overrides the
    ambient + `target_offset_c` computation: HP-07 pins the target to a
    limit it already derived from an EARLIER ambient reading, and a fresh
    re-read here that drifted up would otherwise put target above limit.

    `on_off_rules`, when given (a list of
    `profile_edit_http_client.OnOffRule`), is HP-03's hook for attaching a
    per-(zone,segment) on/off trigger rule to the saved profile -- something
    the raw UART PROFILES SAVE command (`srv._profiles.save()`, this
    function's default path) cannot carry at all: `profiles_handle_message()`
    (`uart_bridge_ext_control.c`) never reads rule bytes off that wire, so a
    profile saved that way always has `on_off_rule_count == 0` regardless of
    a zone's `zone_type`. When `on_off_rules` is given, the save goes over
    `POST /api/profile` instead (ADMIN tier, same `http_auth` session every
    other admin-tier PC tool uses) -- the one save path that already parses
    `rule%u_*` form fields into `profile_on_off_rule_t` -- and the profile is
    then started the normal way (UART `profiles.start()`), same as every
    other caller of this function."""
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
    if on_off_rules:
        host = ctx.get("host")
        if not host:
            return False, "no host in ctx to POST /api/profile (on/off rule save)", ambient
        from .. import profile_edit_http_client as _pehc

        try:
            _pehc.post_profile(
                host, BENCH_PROFILE_SLOT_ID, BENCH_PROFILE_NAME, zone_mask, segments,
                on_off_rules=on_off_rules,
            )
        except Exception as exc:
            return False, f"POST /api/profile (on/off rule save) failed: {exc}", ambient
    else:
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


def _zone_diag_snapshot(ctx: dict) -> Dict[str, Any]:
    """Read-only per-zone diagnostic snapshot for the heat-suite polling
    loop (`_hp_run`). HP-02 failed on the bench (zone 2 rising only 1.88C)
    with no per-zone data available to explain why, so each poll tick now
    also captures `duty`/`heat_blocked`/`heat_blocked_sources` per zone from
    `GET /api/profile_exec` (`dashboard_json.c`'s `append_zone_status_json()`,
    emitted on both `/api/control` and `/api/profile_exec`; the else/no-
    control_fields branch used here also carries `firing_stats`) plus the
    single top-level `zone_blocked_mask` bitmask from `GET /api/status`
    (`dashboard_status_http.c:654`, `relay_authority_latched_blocked_mask()`
    in `dashboard_http.c:572` -- NOT present on `/api/profile_exec` or
    `/api/control`, so it needs its own read). Both reads are plain GETs
    through the same `_http_get_json` seam `_cleanup_bench_profile` already
    uses; either failing (no host configured, board unreachable, malformed
    body) yields an empty/partial snapshot rather than raising, since this
    is diagnostic-only and must never affect whether a case passes or
    fails on its own account."""
    snapshot: Dict[str, Any] = {"zones": {}, "zone_blocked_mask": None}
    host = ctx.get("host")
    if not host:
        return snapshot
    get_json = ctx.get("_http_get_json", _http_get_json)
    try:
        status, body = get_json(host, "/api/profile_exec")
        if status == 200 and isinstance(body, dict):
            for zone in body.get("zones", []) or []:
                idx = zone.get("zone", zone.get("index"))
                if idx is None:
                    continue
                snapshot["zones"][idx] = {
                    "duty": zone.get("duty"),
                    "relay_on": zone.get("relay_on"),
                    "heat_blocked": zone.get("heat_blocked"),
                    "heat_blocked_sources": zone.get("heat_blocked_sources"),
                    # HP-02 root-cause follow-up (2026-09-27): firmware now
                    # names WHY a zone's relay is off while it asks for heat
                    # (`relay_denied_reason`, profile_executor.h's
                    # profile_exec_relay_denied_t: 1 on/off zone with no rule,
                    # 2 load cap, 3 authority block) and how long duty has
                    # been high with the relay never commanded on
                    # (`relay_starved_s`). Absent on older firmware -> None.
                    "relay_starved_s": zone.get("relay_starved_s"),
                    "relay_denied_reason": zone.get("relay_denied_reason"),
                }
    except Exception:
        pass
    try:
        status, body = get_json(host, "/api/status")
        if status == 200 and isinstance(body, dict) and "zone_blocked_mask" in body:
            snapshot["zone_blocked_mask"] = body.get("zone_blocked_mask")
    except Exception:
        pass
    return snapshot


def _hp_run(ctx: dict, zone_mask: int, timeout_s: float = 480.0, poll_s: float = 2.0) -> Dict[str, Any]:
    """Common HP flow: rest gate, start, poll to DONE, collect start/end
    zone temps, K4-energized samples, per-zone diagnostic snapshots and
    link-stats before/after. Always tears down (stop + delete the bench
    slot) in `finally`. Stores its result under `ctx["_hp01"]`/`ctx["_hp02"]`
    (by `zone_mask`) so SP-03 and SP-06's observer cases can read it back
    within the same run."""
    result: Dict[str, Any] = {
        "ok": False, "reason": "", "start_zones": {}, "end_zones": {},
        "energized_samples": [], "zone_diag_samples": [],
        "link_stats_before": {}, "link_stats_after": {},
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
            result["zone_diag_samples"].append(_zone_diag_snapshot(ctx))
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


def _blocked_while_energized_zones(zone_diag_samples: List[Dict[str, Any]]) -> List[int]:
    """Names any zone whose `duty` was reported > 0 while `heat_blocked` was
    simultaneously reported set, across every polled diagnostic snapshot
    (`_zone_diag_snapshot`). Duty>0-and-blocked is nonsensical in a healthy
    run (a blocked zone should read duty 0), so this is a targeted symptom
    check for a low-rise failure like HP-02's zone 2 (1.88C observed on the
    bench with no per-zone data to explain it), not a general judge."""
    flagged = set()
    for snapshot in zone_diag_samples or []:
        for idx, zone in (snapshot.get("zones") or {}).items():
            duty = zone.get("duty")
            if duty is not None and duty > 0 and zone.get("heat_blocked"):
                flagged.add(idx)
    return sorted(flagged)


_RELAY_DENIED_REASON_NAMES = {
    0: "none",
    1: "on/off zone with no on/off rule for this segment",
    2: "max_simultaneous_relays load cap",
    3: "relay authority block",
}

#: `relay_starved_s` above which a zone counts as starved for the HP-02
#: symptom check. PWM timing alone (first window, a min-off hold) accounts
#: for a few tens of seconds at most with the bench's default window; the
#: bench failure itself showed duty 1.00 with the relay never on for 76 s.
_RELAY_STARVED_MIN_S = 60.0


def _starved_zones(zone_diag_samples: List[Dict[str, Any]]) -> Dict[int, str]:
    """HP-02 root-cause symptom (2026-09-25..27): a zone that keeps asking
    for heat and never gets its relay. Names every zone whose polled
    `/api/profile_exec` snapshot ever reported a non-zero
    `relay_denied_reason`, or a `relay_starved_s` at or above
    `_RELAY_STARVED_MIN_S`, mapped to a one-line explanation (the last
    non-zero reason seen wins). Firmware older than the fix carries neither
    field, so this returns {} there rather than guessing."""
    flagged: Dict[int, str] = {}
    for snapshot in zone_diag_samples or []:
        for idx, zone in (snapshot.get("zones") or {}).items():
            reason = zone.get("relay_denied_reason")
            starved_s = zone.get("relay_starved_s")
            if reason:
                name = _RELAY_DENIED_REASON_NAMES.get(reason, f"reason {reason}")
                flagged[idx] = f"relay denied: {name}"
            elif starved_s is not None and starved_s >= _RELAY_STARVED_MIN_S and idx not in flagged:
                flagged[idx] = f"duty high with relay never on for {starved_s:.0f}s"
    return dict(sorted(flagged.items()))


def _on_off_typed_zones(snapshot: dict, zone_mask: int) -> List[int]:
    """Zones in `zone_mask` whose GET /api/zones `zone_type` is not 0
    (heater). The bench profile has no on/off rules, so such a zone can
    never heat under it (docs/ON_OFF_ZONE_PLAN.md sec 3 rule 6) -- the
    HP-02 bench failure's actual root cause was zone 2 left at zone_type 1
    by an earlier HP-03/HP-07 run whose restore was never read back."""
    found = []
    for zone in snapshot.get("zones", []) or []:
        idx = zone.get("index")
        if idx is None or not (zone_mask & (1 << idx)):
            continue
        if zone.get("zone_type", 0) != 0:
            found.append(idx)
    return found


def _case_hp02(ctx: dict) -> CaseResult:
    zone_mask = 0b111
    host = ctx.get("host")
    if host:
        # Precondition, read-only: every masked zone must be a heater.
        # Same seam/tolerance as `_check_zone_ceilings` -- an unreadable
        # snapshot passes through so a tooling failure never masquerades as
        # a board finding; the run itself then reports whatever it sees.
        from .. import zones_http_client

        get_zones = ctx.get("_get_zones_config", zones_http_client.get_zones)
        try:
            snapshot = get_zones(host)
        except Exception:
            snapshot = None
        if snapshot is not None:
            on_off = _on_off_typed_zones(snapshot, zone_mask)
            if on_off:
                return CaseResult(
                    Verdict.FAIL,
                    reason=(
                        f"zone(s) {on_off} are typed on/off (zone_type != 0) on the board; the bench "
                        f"profile has no on/off rules, so they could never heat (firmware now refuses "
                        f"such a start). Leftover of an HP-03/HP-07 zone-config restore that did not "
                        f"land -- restore zone_type 0 by hand before re-running"
                    ),
                    observed={"zone_types": {z.get("index"): z.get("zone_type") for z in snapshot.get("zones", []) or []}},
                    expected={"zone_type": 0},
                )
    run = _hp_run(ctx, zone_mask=zone_mask)
    ctx["_hp02"] = run
    if not run["ok"]:
        return CaseResult(Verdict.FAIL, reason=run["reason"], observed={"start": run["start_zones"], "end": run["end_zones"]})
    result = J.judge_all_zones_rise(_rises(run["start_zones"], run["end_zones"]), zone_mask=zone_mask, min_rise=5.0)
    if result.verdict != Verdict.PASS:
        blocked = _blocked_while_energized_zones(run["zone_diag_samples"])
        starved = _starved_zones(run["zone_diag_samples"])
        observed = dict(result.observed or {})
        observed["zone_diag_samples"] = run["zone_diag_samples"]
        notes = []
        if blocked:
            notes.append(f"zone(s) {blocked} had duty>0 while heat_blocked")
        if starved:
            detail = "; ".join(f"zone {idx}: {why}" for idx, why in starved.items())
            notes.append(f"relay starved -- {detail}")
        if notes:
            return CaseResult(
                Verdict.FAIL,
                reason=f"{result.reason} ({'; '.join(notes)})",
                observed=observed,
                expected=result.expected,
            )
        return CaseResult(Verdict.FAIL, reason=result.reason, observed=observed, expected=result.expected)
    return result


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
        restore_error = _post_zones_restore(ctx, host, restore_body, expected_snapshot=snapshot)
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
    Split out so `_case_hp03` owns the restore and can surface it.

    Attaches a heating on/off trigger rule for (segment 0, `target_zone`) to
    the saved profile -- without one, `profile_resolve_on_off_rule()`
    (`profile_executor.c`) finds no rule for this segment/zone and the zone's
    relay can never be commanded ("no rule for segment", precedence level 6:
    `desired` stays at its fail-safe default forever), regardless of the
    on/off zone config just written. phase_mask/direction_mask are both left
    0 (tautology: any phase, any direction) since this profile is one
    ramp-then-dwell segment covering the whole run -- narrowing either axis
    would only risk suppressing the relay during part of it for no reason.
    temp_cmp=BELOW + temp_source=MEASURED_THIS_ZONE reproduces a standard
    heating on/off thermostat: relay ON while this zone's own TC reads below
    the threshold (with the configured hysteresis band), OFF above it."""
    from .. import profile_edit_http_client as _pehc

    rule = _pehc.OnOffRule(
        zone_index=target_zone, segment_index=0, enable=True,
        temp_cmp=_pehc.ON_OFF_TEMP_CMP_BELOW,
        temp_source=_pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE,
        temp_threshold_c=target_c,
    )
    # `_start_bench_profile` re-reads ambient itself and runs its own ceiling
    # check against the SEGMENT's target_c -- but `rule.temp_threshold_c` was
    # computed by the caller from an earlier ambient read and is never passed
    # through as `target_c` (that would disable the check entirely: an
    # explicit `target_c` is HP-07's "pinned to an already-checked limit"
    # signal, not a substitute for one). If ambient drifted between the two
    # reads the segment's own target could clear the ceiling while the rule's
    # threshold does not, leaving a relay thermostat armed against a ceiling
    # nothing has verified it. Check the rule's own threshold explicitly.
    ceiling_ok, ceiling_reason = _check_zone_ceilings(ctx, 1 << target_zone, rule.temp_threshold_c)
    if not ceiling_ok:
        return CaseResult(Verdict.FAIL, reason=ceiling_reason)
    ok, reason, _ambient = _start_bench_profile(
        ctx, zone_mask=1 << target_zone, target_offset_c=10.0,
        on_off_rules=[rule],
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

#: dwell_min passed to `_start_bench_profile` for HP-07 (default is 2) --
#: long enough that a held-ON relay reliably crosses `limit_c` on this ~4W
#: fixture before the profile's own dwell segment ends and it reaches DONE
#: without ever tripping the guard. Advisory fix, 2026-09-25 review.
_HP07_DWELL_MIN = 15

#: How far ABOVE the lowered `limit_c` HP-07's on/off trigger rule's
#: threshold is set (2026-09-25 redesign -- see `_run_hp07_profile`'s
#: docstring for why the segment's own RAMP target cannot be set above
#: `limit_c` at all). A high, practically-unreachable threshold on this
#: ~4W bench fixture (`project_bench_is_a_4w_test_fixture`) means the rule
#: never sees `measured_c >= threshold` and therefore never turns the relay
#: off -- full, continuous duty for the whole run, so the approach is not
#: gated on a PID converging near `limit_c` (observed on hardware
#: 2026-09-25, run 20260925T171042Z_heat: plateaued at 30.48C against a
#: 31.18C limit and never crossed it in 240s).
_HP07_TRIGGER_MARGIN_C = 50.0


#: Retry count/delay for restoring a zone's `max_temp_c` after HP-07 --
#: separate from `_HP07_MARGIN_C`. A board that rebooted mid-run (observed
#: on hardware 2026-09-24, alongside the exception-swallowing bug this
#: constant's caller exists to fix) may not answer the first restore POST;
#: a short wait and one retry gives it a chance to come back before this
#: case gives up and surfaces the failure.
_HP07_RESTORE_ATTEMPTS = 2
_HP07_RESTORE_RETRY_DELAY_S = 5.0


def _restore_readback_mismatch(expected_snapshot: dict, actual_snapshot: dict) -> Optional[str]:
    """Compares the persistent-config fields HP-03/HP-07 change (`zone_type`,
    `max_temp_c`) between the pre-case GET snapshot and a post-restore GET.
    Returns None when every zone matches, else one line naming the first
    mismatch. Only fields present in BOTH snapshots are compared, so an
    older firmware that omits one never trips this."""
    actual_by_idx = {z.get("index"): z for z in actual_snapshot.get("zones", []) or []}
    for zone in expected_snapshot.get("zones", []) or []:
        idx = zone.get("index")
        if idx is None or idx not in actual_by_idx:
            continue
        actual = actual_by_idx[idx]
        for key in ("zone_type", "max_temp_c"):
            if key not in zone or key not in actual:
                continue
            exp, act = zone[key], actual[key]
            if isinstance(exp, float) or isinstance(act, float):
                if exp is None or act is None or abs(float(exp) - float(act)) > 0.05:
                    return f"read-back: zone {idx} {key} is {act}, expected {exp}"
            elif exp != act:
                return f"read-back: zone {idx} {key} is {act}, expected {exp}"
    return None


def _post_zones_restore(
    ctx: dict, host: str, restore_body: str, expected_snapshot: Optional[dict] = None,
) -> Optional[str]:
    """POSTs a GET-snapshot-derived `restore_body` back to /api/zones,
    retrying once after `_HP07_RESTORE_RETRY_DELAY_S`. Returns None on an
    "ok" response, else the last attempt's error text. Shared by HP-03 and
    HP-07, the two cases that change persistent zone config.

    With `expected_snapshot` (the pre-case GET), an "ok" is only believed
    once a fresh GET /api/zones reads back the same `zone_type`/`max_temp_c`
    per zone -- HP-02's bench failure (2026-09-25..27) was zone 2 left at
    zone_type 1 by a restore whose "ok" text was trusted without a
    read-back. A mismatch counts as a failed attempt and is retried like a
    refusal."""
    from .. import zones_http_client

    get_zones = ctx.get("_get_zones_config", zones_http_client.get_zones)
    sleep = ctx.get("_sleep", time.sleep)
    last_error = ""
    for attempt in range(1, _HP07_RESTORE_ATTEMPTS + 1):
        try:
            restore_result = zones_http_client.post_zones(host, restore_body)
        except Exception as exc:
            last_error = f"{type(exc).__name__}: {exc}"
        else:
            if restore_result == "ok":
                if expected_snapshot is None:
                    return None
                try:
                    actual = get_zones(host)
                except Exception as exc:
                    last_error = f"read-back GET /api/zones failed: {type(exc).__name__}: {exc}"
                else:
                    mismatch = _restore_readback_mismatch(expected_snapshot, actual)
                    if mismatch is None:
                        return None
                    last_error = mismatch
            else:
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
    expected_snapshot: Optional[dict] = None,
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
    last_error = _post_zones_restore(ctx, host, restore_body, expected_snapshot=expected_snapshot)
    if last_error is None:
        return None
    original_note = f"{original_max_temp_c:.1f}C" if original_max_temp_c is not None else "unknown"
    return (
        f"zone {target_zone} max_temp_c restore failed after {_HP07_RESTORE_ATTEMPTS} "
        f"attempt(s) ({last_error}) -- left at {lowered_limit_c:.1f}C, should be {original_note}"
    )


def _run_hp07_profile(ctx: dict, target_zone: int, limit_c: float) -> CaseResult:
    """The heat/poll/ack body of HP-07, once the zone's `max_temp_c` limit is
    already lowered (to `_HP07_MARGIN_C` above ambient), `target_zone` has
    already been typed `zone_type=1` (ZONE_TYPE_ON_OFF) in that SAME preset
    POST -- required because `validate_on_off_rules()` (profiles_http.c)
    refuses to save an on/off rule on a zone that is not so typed, and the
    executor's tick loop only actuates on/off logic for zones with
    `zone_on_off[zi]` set -- and the hidden slot has not yet been started.
    A single on/off zone in `zone_mask` never approaches
    `max_simultaneous_relays` (`profile_executor_run.c`'s start-time
    on/off-zone-count refusal): HP-03 already runs the identical
    one-zone-typed-on/off shape successfully, and HP-07 mirrors it exactly.

    2026-09-25 redesign. Every prior run (most recently 20260925T171042Z_heat)
    FAILed the same way: the segment's RAMP target was pinned to exactly
    `limit_c` and a converging PID simply plateaus a fraction of a degree
    below it on this ~4W bench fixture (observed: 30.48C against a 31.18C
    limit, 240s, never crossing) -- there is no PID overshoot to rely on to
    cross the ceiling live.

    Checked in firmware first, per the task: can the segment's own RAMP
    target simply be set a few degrees ABOVE `limit_c` instead, so the
    setpoint itself sits past the ceiling? No --
    `profile_executor_run.c`'s start-time re-validation (around line 430,
    "PID_EXPANSION_PLAN.md sec 7.2: ... a target above the kiln's permitted
    maximum is refused and NEVER stretched") walks every
    `PROFILE_SEG_KIND_ZONE_RAMP` segment and REFUSES outright (never clamps)
    if `target > zone_max_c` for any zone in `zone_mask`. This applies
    unconditionally to the segment's `target_c`, so a target above the
    just-lowered `limit_c` cannot be used to start the run at all -- the
    same "refused, not clamped" behaviour this module's own earlier comments
    already document.

    That check (profile_executor_run.c ~429-450) only walks
    `PROFILE_SEG_KIND_ZONE_RAMP` segments and inspects `seg->target_c`; it
    never reads `profile_on_off_rule_t.temp_threshold_c` at all (confirmed
    by grep: no start-time or save-time comparison of `temp_threshold_c`
    against a zone's `max_temp_c` exists anywhere in
    `firmware/KilnFW/App/drivers/http/profiles_http.c` or
    `profile_executor_run.c` -- only a `PROFILE_TARGET_C_MIN`/`MAX` sanity
    range applies, 0-2015C). And once an on/off trigger rule is attached to
    a (segment, zone) pair, `profile_resolve_on_off_rule()` /
    `on_off_trigger_decide()` (`profile_executor.c`) drive that zone's relay
    from the rule's own BELOW/ABOVE comparison against `temp_threshold_c`,
    not from the ramp target's PID at all -- so the RAMP segment's
    `target_c` (checked against `max_temp_c`) and the relay's actual trigger
    threshold (not checked against anything but the sanity range) are two
    independent fields.

    So the segment target stays pinned at `limit_c` (satisfies the ramp
    ceiling check, same as before), and an on/off rule is attached with
    `temp_threshold_c = limit_c + _HP07_TRIGGER_MARGIN_C` -- a threshold
    this fixture cannot practically reach. `temp_cmp=BELOW` means the rule
    commands the relay ON whenever `measured_c < threshold`, which is always
    true here, so the relay is held full ON for the whole run instead of
    backing off as a PID would near its setpoint. That deterministically
    drives `measured_c` up through `limit_c` (rather than plateauing near
    it), tripping `thermal_guard.c`'s runtime check
    (`measurement_c >= max_temp_c`, evaluated every guard tick) instead of
    depending on PID overshoot that this fixture does not reliably produce.

    No zone config is written after the run starts (per owner decision, all
    zone writes are refused 409 while RUNNING/PAUSED) -- the on/off rule is
    attached via the SAME pre-start `POST /api/profile` save `_start_bench_profile`
    already uses for HP-03, before `profiles.start()` is ever called."""
    from .. import profile_edit_http_client as _pehc

    rule = _pehc.OnOffRule(
        zone_index=target_zone, segment_index=0, enable=True,
        temp_cmp=_pehc.ON_OFF_TEMP_CMP_BELOW,
        temp_source=_pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE,
        temp_threshold_c=limit_c + _HP07_TRIGGER_MARGIN_C,
    )
    # dwell_min lengthened (default 2min) so a converged on/off hold has
    # enough real time to actually cross `limit_c` on this ~4W bench fixture
    # before the profile's own dwell segment ends and it reaches DONE on its
    # own -- advisory fix, 2026-09-25 review.
    ok, reason, ambient_at_start = _start_bench_profile(
        ctx, zone_mask=1 << target_zone, target_c=limit_c, on_off_rules=[rule],
        dwell_min=_HP07_DWELL_MIN,
    )
    if not ok:
        return CaseResult(Verdict.FAIL, reason=reason)
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    #: Long enough to cover `_HP07_DWELL_MIN` (plus ramp time and margin) so
    #: the poll loop does not itself time out before a lengthened dwell would
    #: give the fixture a chance to cross `limit_c`.
    poll_timeout_s = max(240.0, _HP07_DWELL_MIN * 60.0 + 60.0)
    deadline = now() + poll_timeout_s
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
        if state in ("faulted", "done"):
            break
        sleep(2)
    observed = {"limit_c": limit_c, "ambient_at_start": ambient_at_start, "actual_c_samples": actual_c_samples}
    if st is not None and state == "done":
        # Distinct from the generic timeout below: the profile ran to
        # completion on its own without ever tripping the guard -- e.g.
        # dwell_min still too short, or the fixture's held-ON relay didn't
        # reach limit_c in time. Advisory fix, 2026-09-25 review.
        return CaseResult(
            Verdict.FAIL,
            reason="profile reached DONE without ever tripping the over-max-temp guard",
            observed=observed,
        )
    if st is None or state != "faulted":
        return CaseResult(
            Verdict.FAIL,
            reason=f"never reached FAULTED within {poll_timeout_s:.0f}s (last state={state})",
            observed=observed,
        )
    zone_status = _zone_status(st, target_zone)
    faulted = bool(getattr(zone_status, "faulted", False))
    fault_guard = getattr(zone_status, "fault_guard", None)
    # This ack (srv._profiles.stop(), which reaches profile_executor_halt() ->
    # clear_this_runs_faults() on the board) is not what makes the Pico's own
    # S6a mainFault latch reliable -- that's KilnFW's SAFETY_FAULT_MIN_HOLD_MS
    # (firmware/KilnFW/App/drivers/safety/safety_link.h, 300ms), which holds
    # the isolated fault line asserted regardless of how quickly this ack
    # arrives. This case's logic is unchanged and does not depend on ack
    # timing either way; the comment is here only so a future reader does not
    # assume this ack's speed is load-bearing for the backup processor's trip.
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
    if result.verdict != Verdict.PASS:
        return CaseResult(result.verdict, reason=result.reason, observed=merged_observed)

    # The ack (`profiles.stop()`) confirmed FAULTED and cleared cleanly on
    # the ESP side, but `escalate_guard_trip()` classifies
    # THERMAL_GUARD_TRIP_MAX_TEMP as a GLOBAL fault: it also asserted the
    # safety-link fault line, which the Pico latches as its own S6a
    # (SAFETY_TRIP_MAIN_FAULT, trip_reason 6). Confirm the ESP side released
    # its fault source, then check the Pico's own diag and only clear an
    # EXACT reason==6/mask==0x0020 match -- following cases_fl.py's
    # `_case_fl11` pattern.
    diag_text = srv.safety_get_diag()
    current_fault_sources = J.parse_current_fault_sources(diag_text)
    merged_observed["current_fault_sources_after_ack"] = current_fault_sources
    if current_fault_sources:
        return CaseResult(
            Verdict.FAIL,
            reason=(
                f"ESP fault source(s) still asserted after ack (current_fault_sources="
                f"0x{current_fault_sources:02x}) -- escalate_guard_trip's global fault was "
                f"not released by profiles.stop()"
            ),
            observed=merged_observed,
        )
    trip_reason = J.parse_trip_reason(diag_text)
    trip_mask = J.parse_trip_mask(diag_text)
    expected_reason = 6  # S6a, SAFETY_TRIP_MAIN_FAULT
    expected_mask = J.safety_trip_mask_for_reason(expected_reason)
    merged_observed["trip_reason_after_ack"] = trip_reason
    merged_observed["trip_mask_after_ack"] = trip_mask

    cleared_after: Optional[bool] = None
    if trip_reason == expected_reason and trip_mask == expected_mask:
        srv.safety_clear_trip()
        after_reason = J.parse_trip_reason(srv.safety_get_diag())
        merged_observed["trip_reason_after_clear"] = after_reason
        if after_reason is not None:
            cleared_after = after_reason == 0

    trip_result = J.judge_operator_trip(trip_reason, expected_reason, cleared_after, trip_mask=trip_mask)
    if trip_result.observed:
        merged_observed.update(trip_result.observed)
    if trip_result.verdict != Verdict.PASS:
        return CaseResult(trip_result.verdict, reason=trip_result.reason, observed=merged_observed)
    return CaseResult(result.verdict, reason=result.reason, observed=merged_observed)


def _case_hp07(ctx: dict) -> CaseResult:
    """HP-01's profile, but the target zone's `max_temp_c` limit is lowered
    to `_HP07_MARGIN_C` above ambient BEFORE the profile starts (while
    IDLE), and an on/off trigger rule is attached whose threshold sits well
    above that lowered limit -- holding the relay full ON for the whole run
    instead of letting a PID converge and plateau just under the ceiling.
    The same preset POST that lowers the limit also types `target_zone`
    `zone_type=1` (ZONE_TYPE_ON_OFF), since `validate_on_off_rules()`
    (profiles_http.c) refuses to save an on/off rule on a zone not so typed;
    the restore below puts `zone_type` back along with everything else. See
    `_run_hp07_profile`'s docstring for the full 2026-09-25 redesign (why the
    segment's own RAMP target cannot be pinned above the limit, and why the
    on/off rule's threshold is a separate, unchecked field that can be).
    Expects FAULTED with fault_guard naming the over-max-temp guard. Because
    `escalate_guard_trip()` classifies `THERMAL_GUARD_TRIP_MAX_TEMP` as a
    GLOBAL fault, this ALSO asserts the safety-link fault line and the Pico
    latches its own S6a (`SAFETY_TRIP_MAIN_FAULT`) -- after acknowledging via
    `profiles.stop()` (mirroring the web UI's POST /api/profile_exec/stop),
    this case confirms the ESP fault sources released, reads
    `srv.safety_get_diag()`, and ONLY if `trip_reason == 6` and
    `trip_mask == J.safety_trip_mask_for_reason(6)` (0x0020) calls
    `srv.safety_clear_trip()` and verifies reason 0 afterward; any other
    reported reason/mask is surfaced as a case failure instead (following
    `_case_fl11`'s pattern in `cases_fl.py`), never auto-cleared. Restores
    the zone's original max_temp_c (and zone_type, etc.) in `finally`. No
    zone config is written once the profile is running or paused (refused
    409 by design) -- the lowered limit is set once, while IDLE, before
    `profiles.start()`.

    Found on hardware 2026-09-24 (run 20260924T072516Z_heat), TWICE, and a
    third time 2026-09-25 (run 20260925T171042Z_heat) after those two fixes:

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

    3. The second fix (lower the limit while IDLE, pin the RAMP target to
       exactly that limit) reaches the runtime guard's code path but never
       trips it: a converging PID plateaus a fraction of a degree below its
       setpoint on this ~4W bench fixture and never actually reaches
       `measurement_c >= max_temp_c` (observed: 30.48C against a 31.18C
       limit, 240s). Fixed by attaching an on/off trigger rule (see above)
       so the relay is held full ON instead of backing off near the
       setpoint -- deterministic on this fixture, not dependent on PID
       overshoot that does not reliably occur.

    Also refuses outright (before touching zone config at all) if lowering
    `target_zone` alone would leave the Pico's mirrored `abs_max_temp_c`
    (the MAX of every zone's `max_temp_c`, never the minimum) too close to
    `limit_c` -- see `_hp07_pico_ceiling_headroom_ok`'s docstring. That
    would risk the Pico's own S1 guard tripping additionally to, or racing,
    the intended S6a described above -- a second, different trip this case
    is not equipped to distinguish from the expected one, so it refuses
    rather than risk misreporting."""
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
    # `validate_on_off_rules()` (profiles_http.c) refuses an on/off rule
    # attached to any zone whose `zone_type` is not ZONE_TYPE_ON_OFF (1) at
    # save time, and the executor's tick loop only evaluates on/off actuation
    # (`zone_on_off[zi]`, profile_executor.c) for zones so typed -- so the
    # SAME preset that lowers `max_temp_c` must also type `target_zone` as
    # on/off, in the same POST, as HP-03 does. `failsafe_state=False` and
    # `min_on_s`/`min_off_s`=0 avoid any on/off gating delaying the relay
    # from following the rule immediately. The restore below POSTs the whole
    # original snapshot (`build_post_body(snapshot, {})`), which already puts
    # `zone_type` (and these other fields) back -- no separate restore logic
    # is needed for them.
    preset = {"zones": [{
        "index": target_zone, "max_temp_c": limit_c, "zone_type": 1,
        "failsafe_state": False, "min_on_s": 0, "min_off_s": 0,
    }]}
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
    pico_ok, pico_reason = _hp07_pico_ceiling_headroom_ok(snapshot, target_zone, limit_c)
    if not pico_ok:
        # INCONCLUSIVE, not FAIL: this is an unmet bench precondition (no
        # other commissioned zone clears the required headroom today), not a
        # genuine case failure. Advisory fix, 2026-09-25 review.
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"refusing to lower: {pico_reason}")

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
            expected_snapshot=snapshot,
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
