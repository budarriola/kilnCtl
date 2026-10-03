"""AT-* cases -- autotune on the 4W fixture (plan doc section 3.5, Wave 3
part A).

Every case that starts an autotune run refuses unless the board has RESTED
per the plan's rest gate (``cases_heat._rest_gate``, reused rather than
duplicated -- residual heat biases the fitted gain low, memory
project_autotune_needs_rested_baseline) and MUST leave autotune idle in
``finally`` (``_cleanup_autotune``: abort if still running, never raises).
Ramp assist must be off for a fit (AT-01's stated precondition). A case that
finds it on reads the original value (``GET /api/ramp_assist``), disables it
for the case (``_with_ramp_assist_off``), and restores the original in the
same case's ``finally`` (plan section 6 rule 2); a restore that cannot be
confirmed sets ``ctx["_tainted"]`` so the run is marked ``tainted``. A route
that cannot be read at all is not a hard gate (nothing was changed, nothing to
restore); an enabled assist that cannot be turned off SKIPs the case.

AT-02 (abort immediacy) and AT-03 (accept-guard-on-an-unsettled-fit) are,
in the plan's table, described against "AT-01 running" / "an unsettled fit
(AT-01 aborted early)" -- i.e. sharing a single live window with another
case. This package's runner executes cases sequentially in one process over
one board connection, the same constraint every other case here already
lives under (HP-04's pause/resume is one case doing both steps itself, not
two cases sharing a window) -- so AT-02 starts and aborts its OWN short
step run early (never AT-01's, which is left alone to reach a full,
converged fit) and AT-03 ``depends_on`` AT-02 (plan section 5.3: at most
one dependency, small in scope) and reads back the unsettled state AT-02
left behind rather than starting or aborting anything itself. AT-05
``depends_on`` AT-01 for the same reason: it reads
``GET /api/autotune/matrix`` after AT-01's zone-0 fit has populated it.

Never calls ``autotune_accept(ack_unsettled=True)`` (AT-03's single guarded
probe always passes the default ``False`` -- plan section 6 rule 3), never
edits ``RULE_TABLE``/schedule tables, never flashes, never writes
``abs_max_temp_c``.
"""
from __future__ import annotations

import time
from typing import Any, Dict, Optional, Tuple

from . import judgments as J
from .cases_heat import _capability_preflight_ok, _rest_gate, _srv, _zone_temps
from .cases_smoke import _http_get_json
from .cases_web_rw import _get_json as _authed_get_json, _post_json as _authed_post_json
from .registry import CaseResult, Verdict, get_case

#: Bench-appropriate step size for AT-01/AT-02 (plan section 3.5: "with the
#: bench-appropriate step size") -- large enough to produce a clean fit on
#: a ~4W fixture (memory project_bench_is_a_4w_test_fixture) without
#: chasing the full-scale duty range.
AT_STEP_DUTY = 0.5
#: AT-04's relay-method target setpoint/amplitude/hysteresis, same
#: bench-scale reasoning as AT_STEP_DUTY.
AT_RELAY_SETPOINT_OFFSET_C = 15.0
AT_RELAY_DUTY_AMPLITUDE = 0.5
AT_RELAY_HYSTERESIS_C = 1.0

MAX_TEMP_LIMIT_C = 70.0
REST_BAND_C = 2.0
AT02_ABORT_AFTER_S = 30.0
AT02_TIMEOUT_S = 5.0


_RAMP_ASSIST_PATH = "/api/ramp_assist"


def _ramp_assist_enabled(ctx: dict) -> Tuple[Optional[bool], str]:
    """Current ``enabled`` flag, or ``(None, why)`` when the route cannot be
    read (no host, non-200, unexpected shape)."""
    if not ctx.get("host") and ctx.get("http_get_json") is None:
        return None, "no host in ctx"
    status, body = _authed_get_json(ctx, _RAMP_ASSIST_PATH)
    if status != 200 or not isinstance(body, dict) or "enabled" not in body:
        return None, f"GET {_RAMP_ASSIST_PATH}: status={status}"
    return bool(body["enabled"]), ""


def _ramp_assist_set(ctx: dict, enabled: bool) -> bool:
    """POST the value and confirm by read-back; never raises. The read-back
    alone decides success: a failed POST that leaves the board already at
    the wanted value (state never changed) is not a failure."""
    try:
        _authed_post_json(ctx, _RAMP_ASSIST_PATH, {"enabled": "1" if enabled else "0"})
    except Exception:
        pass
    try:
        now_enabled, _why = _ramp_assist_enabled(ctx)
    except Exception:
        return False
    return now_enabled is enabled


def _with_ramp_assist_off(ctx: dict, body_fn) -> CaseResult:
    """Run ``body_fn(ctx)`` with ramp assist off. If it was on, disable it
    first and restore it in ``finally`` (if the restore cannot be confirmed:
    ``ctx["_tainted"]``, and the verdict becomes FAIL with the note in the
    reason, as ``cases_heat`` does for HP-03/HP-07). If the route is
    unreadable, proceed unchanged (the fit itself fails if assist interfered)."""
    original, why = _ramp_assist_enabled(ctx)
    changed = False
    try:
        if original:
            changed = True  # restore even if the disable only half-applied
            if not _ramp_assist_set(ctx, False):
                return CaseResult(
                    Verdict.SKIP, reason="ramp assist is enabled and could not be disabled for the case"
                )
        result = body_fn(ctx)
    finally:
        restored = True
        if changed:
            restored = _ramp_assist_set(ctx, True)
            if not restored:
                ctx["_tainted"] = True
    if changed and not restored:
        note = "ramp assist was enabled before the case and could NOT be restored -- run tainted"
        combined = f"{result.reason}; {note}" if result.reason else note
        return CaseResult(
            Verdict.FAIL, reason=combined, observed=result.observed,
            expected=result.expected, evidence=list(result.evidence) + [note],
        )
    return result


def _autotune_idle(ctx: dict) -> Tuple[bool, str]:
    srv = _srv(ctx)
    try:
        st = srv._autotune.get_status()
    except Exception as exc:
        return False, f"autotune_get_status raised {type(exc).__name__}: {exc}"
    if st.state_name != "idle":
        return False, f"autotune is not idle (state={st.state_name})"
    return True, ""


def _cleanup_autotune(ctx: dict) -> None:
    """Best-effort: abort whatever is left running. Never raises -- mirrors
    ``cases_heat._cleanup_bench_profile``'s discipline (a cleanup failure
    must not mask the case's own verdict)."""
    srv = _srv(ctx)
    try:
        srv._autotune.abort()
    except Exception:
        pass


def _relays_off(ctx: dict) -> Optional[bool]:
    """``io_read()``'s relay bitmask, all-zero -- None (not a crash) if the
    read fails, so callers can report INCONCLUSIVE for that half of a
    check rather than a silent PASS."""
    srv = _srv(ctx)
    try:
        state = srv._io.read()
    except Exception:
        return None
    return state.relays == 0


def _at_preflight(ctx: dict) -> Tuple[bool, str]:
    """AT-01/AT-02/AT-04 precondition chain: capability_preflight ok, then
    autotune idle -- in that order, so the reason reported names whichever
    gate actually failed. Ramp assist is handled separately
    (``_with_ramp_assist_off``): it is disabled for the case, not a gate."""
    ok, reason = _capability_preflight_ok(ctx)
    if not ok:
        return False, reason
    ok, reason = _autotune_idle(ctx)
    if not ok:
        return False, reason
    return True, ""


def _poll_autotune(ctx: dict, timeout_s: float, poll_s: float = 5.0) -> Tuple[Any, Optional[float], str]:
    """Poll ``autotune_get_status()`` to ``done``/``aborted``, tracking the
    highest zone temperature seen so the 70C ceiling (plan section 6 rule
    8) can abort mid-run rather than only being checked after the fact."""
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    deadline = now() + timeout_s
    max_temp: Optional[float] = None
    st = None
    while now() < deadline:
        st = srv._autotune.get_status()
        temps = _zone_temps(ctx)
        if temps:
            cur_max = max(temps.values())
            max_temp = cur_max if max_temp is None else max(max_temp, cur_max)
        if max_temp is not None and max_temp >= MAX_TEMP_LIMIT_C:
            _cleanup_autotune(ctx)
            return st, max_temp, ""
        if st.state_name in ("done", "aborted"):
            return st, max_temp, ""
        sleep(poll_s)
    return st, max_temp, f"did not reach done/aborted within {timeout_s:.0f}s"


def _case_at01(ctx: dict) -> CaseResult:
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    ok, reason = _at_preflight(ctx)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=reason)
    return _with_ramp_assist_off(ctx, _at01_body)


def _at01_body(ctx: dict) -> CaseResult:
    zone_temps = _zone_temps(ctx)
    if not zone_temps:
        return CaseResult(Verdict.FAIL, reason="no valid thermo reading to use as the baseline reference")
    ambient_ref = min(zone_temps.values())
    srv = _srv(ctx)
    try:
        ok_start, err = srv._autotune.start(zone=0, method=0, step_duty_or_setpoint_c=AT_STEP_DUTY)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"autotune.start raised {type(exc).__name__}: {exc}")
    if not ok_start:
        return CaseResult(Verdict.FAIL, reason=f"autotune.start refused: {err}")
    try:
        st, max_temp, timeout_reason = _poll_autotune(ctx, timeout_s=1200.0)
        ctx["_at01"] = {"status": st, "max_temp_c": max_temp, "ambient_ref": ambient_ref}
        if st is None:
            return CaseResult(Verdict.FAIL, reason="autotune_get_status never returned a result")
        if timeout_reason:
            return CaseResult(
                Verdict.FAIL, reason=timeout_reason, observed={"state": st.state_name, "max_temp_c": max_temp}
            )
        tripped = False
        try:
            diag = srv._safety.get_diag()
            tripped = bool(getattr(diag, "trip_reason", 0))
        except Exception:
            pass
        return J.judge_autotune_fit(
            method="step",
            model_valid=st.model_valid,
            baseline_c=st.actual_c if st.actual_valid else None,
            ambient_ref=ambient_ref,
            k_gain_c_per_duty=st.model.k_gain_c_per_duty,
            tau_s=st.model.tau_s,
            max_temp_c=max_temp,
            tripped=tripped,
        )
    finally:
        _cleanup_autotune(ctx)


def _case_at02(ctx: dict) -> CaseResult:
    """Starts its own short step run (see module docstring) and aborts it
    ~30s in, then confirms the abort was immediate: idle within 5s, every
    zone duty 0, heater relays off per ``io_read()``."""
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    ok, reason = _at_preflight(ctx)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=reason)
    return _with_ramp_assist_off(ctx, _at02_body)


def _at02_body(ctx: dict) -> CaseResult:
    srv = _srv(ctx)
    sleep = ctx.get("_sleep", time.sleep)
    now = ctx.get("_now", time.monotonic)
    try:
        ok_start, err = srv._autotune.start(zone=0, method=0, step_duty_or_setpoint_c=AT_STEP_DUTY)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"autotune.start raised {type(exc).__name__}: {exc}")
    if not ok_start:
        return CaseResult(Verdict.FAIL, reason=f"autotune.start refused: {err}")
    try:
        sleep(AT02_ABORT_AFTER_S)
        abort_result = srv._autotune.abort()
        if not abort_result:
            return CaseResult(
                Verdict.FAIL, reason=f"autotune_abort() refused: {getattr(abort_result, 'reason', '')}"
            )
        t0 = now()
        st = srv._autotune.get_status()
        while st.state_name != "idle" and now() - t0 < AT02_TIMEOUT_S:
            sleep(0.2)
            st = srv._autotune.get_status()
        elapsed = now() - t0
        relays_off = _relays_off(ctx)
        ctx["_at02"] = {"status": st}
        return J.judge_autotune_abort_immediate(
            state_name=st.state_name, duties=[st.duty], relays_off=relays_off,
            elapsed_since_abort_s=elapsed, timeout_s=AT02_TIMEOUT_S,
        )
    finally:
        _cleanup_autotune(ctx)


def _case_at03(ctx: dict) -> CaseResult:
    """Reads back the state AT-02 (its declared dependency) left behind:
    an aborted, unsettled fit. Calls ``autotune_accept()`` with the
    default ``ack_unsettled=False`` and confirms it is refused and that
    ``control_get_zones``' gains for zone 0 did not change."""
    srv = _srv(ctx)
    try:
        _count, _mask, zones_before = srv._control.get_zones()
        gains_before = _zone0_gains(zones_before)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"control_get_zones raised {type(exc).__name__}: {exc}")
    try:
        accept_result = srv._autotune.accept(ack_unsettled=False)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"autotune.accept raised {type(exc).__name__}: {exc}")
    try:
        _count, _mask, zones_after = srv._control.get_zones()
        gains_after = _zone0_gains(zones_after)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"control_get_zones raised {type(exc).__name__}: {exc}")
    return J.judge_autotune_accept_guarded(
        accept_ok=bool(accept_result), accept_reason=getattr(accept_result, "reason", ""),
        gains_before=gains_before, gains_after=gains_after,
    )


def _zone0_gains(zones) -> tuple:
    for z in zones:
        if getattr(z, "index", None) == 0:
            return (z.pid_kp, z.pid_ki, z.pid_kd)
    return ()


def _case_at04(ctx: dict) -> CaseResult:
    """Relay-feedback (Astrom-Hagglund) test, rested 25 min after AT-01
    (the same ``_rest_gate`` -- the plan's "25 min after AT-01" is the rest
    gate's own timeout, not a second, separate wait)."""
    rested, rest_reason = _rest_gate(ctx)
    if not rested:
        return CaseResult(Verdict.SKIP, reason=f"rest gate: {rest_reason}")
    ok, reason = _at_preflight(ctx)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=reason)
    return _with_ramp_assist_off(ctx, _at04_body)


def _at04_body(ctx: dict) -> CaseResult:
    zone_temps = _zone_temps(ctx)
    if not zone_temps:
        return CaseResult(Verdict.FAIL, reason="no valid thermo reading to use as the baseline reference")
    ambient_ref = min(zone_temps.values())
    setpoint_c = ambient_ref + AT_RELAY_SETPOINT_OFFSET_C
    srv = _srv(ctx)
    try:
        ok_start, err = srv._autotune.start(
            zone=0, method=1, step_duty_or_setpoint_c=setpoint_c,
            relay_d=AT_RELAY_DUTY_AMPLITUDE, relay_h_c=AT_RELAY_HYSTERESIS_C,
        )
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"autotune.start raised {type(exc).__name__}: {exc}")
    if not ok_start:
        return CaseResult(Verdict.FAIL, reason=f"autotune.start refused: {err}")
    try:
        st, max_temp, timeout_reason = _poll_autotune(ctx, timeout_s=1200.0)
        if st is None:
            return CaseResult(Verdict.FAIL, reason="autotune_get_status never returned a result")
        if timeout_reason:
            return CaseResult(
                Verdict.FAIL, reason=timeout_reason, observed={"state": st.state_name, "max_temp_c": max_temp}
            )
        tripped = False
        try:
            diag = srv._safety.get_diag()
            tripped = bool(getattr(diag, "trip_reason", 0))
        except Exception:
            pass
        return J.judge_autotune_fit(
            method="relay",
            model_valid=st.model_valid,
            baseline_c=st.actual_c if st.actual_valid else None,
            ambient_ref=ambient_ref,
            k_gain_c_per_duty=st.model.k_gain_c_per_duty,
            tau_s=st.model.tau_s,
            max_temp_c=max_temp,
            tripped=tripped,
            relay_valid=st.relay_valid,
            relay_amplitude_c=st.relay.amplitude_c if st.relay is not None else None,
        )
    finally:
        _cleanup_autotune(ctx)


def _case_at05(ctx: dict) -> CaseResult:
    """Reads back ``GET /api/autotune/matrix`` after AT-01 (its declared
    dependency) has populated zone 0's row."""
    host = ctx.get("host")
    if not host:
        return CaseResult(Verdict.INCONCLUSIVE, reason="no host in ctx")
    status, body = _http_get_json(host, "/api/autotune/matrix")
    if status != 200:
        return CaseResult(Verdict.FAIL, reason=f"GET /api/autotune/matrix: status={status}", observed={"body": body})
    matrix = body.get("matrix") if isinstance(body, dict) else body
    return J.judge_autotune_matrix(matrix, zone_row=0)


_CASE_FUNCS = {
    "AT-01": _case_at01,
    "AT-02": _case_at02,
    "AT-03": _case_at03,
    "AT-04": _case_at04,
    "AT-05": _case_at05,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
