"""OT-B01 + SP-04, and OT-E01/E02/E03/E12 (plan doc section 3.4/3.9, Wave 2)
-- ESP OTA-over-Wi-Fi update/rollback/corrupt-image handling and the dual
safety-processor reset trip, using `ota_http_client` directly (never the
MCP-tool text wrapper, per the task's own instruction) so results stay
structured dicts a pure judge function can compare.

Every case here is gated on the executor being idle before touching OTA or
issuing a dual reset -- CLAUDE.md's "never flash/OTA during a firing" -- and
every case is thin: fetch/act, then delegate to judgments.py. Pico OTA
(`OT-P*`) is Wave 4 and out of scope for this module.
"""
from __future__ import annotations

from typing import Any, Optional

from . import judgments as J
from .registry import CaseResult, Verdict, get_case


def _srv(ctx: dict):
    srv = ctx.get("srv")
    if srv is None:
        from .. import mcp_server as srv  # local import: keeps this module importable with no board/MCP process
    return srv


def _ota_client(ctx: dict):
    client = ctx.get("ota_http_client")
    if client is None:
        from .. import ota_http_client as client  # local import: same reason as _srv
    return client


def _partition_client(ctx: dict):
    client = ctx.get("partition_http_client")
    if client is None:
        from .. import partition_http_client as client
    return client


def _zones_client(ctx: dict):
    client = ctx.get("zones_http_client")
    if client is None:
        from .. import zones_http_client as client
    return client


def _dashboard_client(ctx: dict):
    client = ctx.get("dashboard_http_client")
    if client is None:
        from .. import dashboard_http_client as client
    return client


def _is_idle(ctx: dict) -> "tuple[bool, str]":
    """CLAUDE.md: never flash/OTA during a firing -- gate on executor
    status idle. Any error reading status refuses rather than assumes
    idle."""
    srv = _srv(ctx)
    try:
        st = srv._profiles.get_exec_status()
    except Exception as exc:
        return False, f"could not read executor status: {type(exc).__name__}: {exc}"
    state = getattr(st, "state_name", None)
    if state != "idle":
        return False, f"executor state is {state!r}, not idle -- refusing to OTA/reset during a firing"
    return True, ""


def _running_partition(ctx: dict, host: str) -> Optional[str]:
    try:
        return _partition_client(ctx).get_partitions(host).get("running")
    except Exception:
        return None


def _fw_build(ctx: dict, host: str) -> Optional[str]:
    try:
        return _dashboard_client(ctx).get_status(host).get("fw_build")
    except Exception:
        return None


def _boot_guard_recovery_mode(ctx: dict, host: str) -> Optional[bool]:
    try:
        return _ota_client(ctx).get_boot_guard_status(host).get("recovery_mode")
    except Exception:
        return None


def _pid_gains(zones_json: dict) -> "Optional[dict]":
    """Extracts the per-zone pid_kp/pid_ki/pid_kd triple keyed by zone
    index, for an exact before/after comparison -- see
    judgments.judge_ota_rollback's ZONES_CFG_VERSION hazard docstring."""
    zones = zones_json.get("zones") if isinstance(zones_json, dict) else None
    if not isinstance(zones, list):
        return None
    out = {}
    for i, z in enumerate(zones):
        if not isinstance(z, dict):
            return None
        out[i] = {"pid_kp": z.get("pid_kp"), "pid_ki": z.get("pid_ki"), "pid_kd": z.get("pid_kd")}
    return out


def _default_clear_trip_fn(ctx: dict):
    """`safety_clear_trip()` is fire-and-forget over UART
    (`_srv._send(UART_TASK_ID_SAFETY, devices.safety_clear_trip())`,
    mcp_server_safety.py) -- no reply to check here, only the status poll
    that follows tells us whether it actually cleared."""
    srv = _srv(ctx)
    from .. import devices_safety as devices
    from ..protocol import UART_TASK_ID_SAFETY
    srv._send(UART_TASK_ID_SAFETY, devices.safety_clear_trip())


def _default_readiness_trip_ok_fn(ctx: dict) -> Optional[bool]:
    """Confirms the trip item on /api/readiness (via capability_preflight's
    same board report) no longer blocks, using the readiness_blocked list
    capability_preflight already exposes."""
    from .. import capability_preflight
    host = ctx.get("host") or capability_preflight.PREFLIGHT_AP_DEFAULT_HOST
    try:
        report = capability_preflight.run_preflight({}, host)
    except Exception:
        return None
    board = report.board
    blocked = getattr(board, "readiness_blocked", None)
    if not blocked:
        return True
    return not any("trip" in str(key).lower() or "trip" in str(label).lower() for key, label, _detail in blocked)


def _case_otb01(ctx: dict) -> CaseResult:
    """OT-B01: sw_reset_esp(confirm=True) resets both processors close
    together. Confirm S6a (SAFETY_TRIP_MAIN_FAULT) latches while the link
    handshake is still coming up -- expected, per CLAUDE.md, not a bug --
    then clear it and confirm readiness. Stashes the observed data into
    ctx["_otb01"] for SP-04's observer.

    Every board interaction past the initial reset is behind an injectable
    ctx seam (``_get_safety_status_fn``/``_get_safety_diag_fn``/
    ``_clear_trip_fn``/``_readiness_trip_ok_fn``) so this can be unit
    tested with fakes rather than a live link, matching the "unit/mock
    only" scope for this wave (the bench board carries an unacknowledged
    crash report)."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    ap_password = ctx.get("ap_password")
    srv = _srv(ctx)
    ota = _ota_client(ctx)
    if not ap_password:
        return CaseResult(Verdict.SKIP, reason="no ap_password credential available for sw_reset_esp")

    sw_reset_fn = ctx.get("_sw_reset_fn", lambda: ota.sw_reset(host, ap_password))
    get_status_fn = ctx.get("_get_safety_status_fn") or (lambda: srv._safety.get_status())
    get_diag_fn = ctx.get("_get_safety_diag_fn") or (lambda: srv._safety.get_diag())
    clear_trip_fn = ctx.get("_clear_trip_fn", lambda: _default_clear_trip_fn(ctx))
    readiness_trip_ok_fn = ctx.get("_readiness_trip_ok_fn", lambda: _default_readiness_trip_ok_fn(ctx))

    try:
        sw_reset_fn()
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"sw_reset_esp raised {type(exc).__name__}: {exc}")

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    deadline = now() + 30.0
    link_up = False
    diag = None
    while now() < deadline:
        try:
            status = get_status_fn()
            if getattr(status, "link_up", False):
                link_up = True
                diag = get_diag_fn()
                break
        except Exception:
            pass
        sleep(1.0)

    trip_reason = getattr(diag, "trip_reason", None) if diag is not None else None
    trip_mask = getattr(diag, "trip_mask", None) if diag is not None else None

    clear_ok = None
    readiness_trip_ok = None
    # Plan doc section 6 rule 5 / CLAUDE.md: NEVER clear a trip without first
    # confirming, from safety_get_status()/get_diag(), that the link is up and
    # trip_mask == 1 << (trip_reason - 1) with trip_reason == 6 and no other
    # bit set. Anything else stops the run for a human -- the judge's own
    # check runs too late to be that gate, since by then the clear already
    # happened. A non-S6a trip therefore leaves clear_ok None and the judge
    # FAILs on the reason/mask it observed.
    clear_allowed = (
        link_up and trip_reason == 6 and trip_mask == (1 << (6 - 1))
    )
    if clear_allowed:
        try:
            clear_trip_fn()
        except Exception:
            pass
        clear_deadline = now() + 10.0
        while now() < clear_deadline:
            try:
                status = get_status_fn()
                if getattr(status, "enabled", False):
                    clear_ok = True
                    break
            except Exception:
                pass
            sleep(1.0)
        if clear_ok is None:
            clear_ok = False
        try:
            readiness_trip_ok = readiness_trip_ok_fn()
        except Exception:
            readiness_trip_ok = None

    ctx["_otb01"] = {
        "link_up": link_up, "trip_reason": trip_reason, "trip_mask": trip_mask,
        "clear_ok": clear_ok, "readiness_trip_ok": readiness_trip_ok,
    }
    return J.judge_dual_reset_trip(link_up, trip_reason, trip_mask, clear_ok, readiness_trip_ok)


def _case_ote01(ctx: dict) -> CaseResult:
    """OT-E01: push a good image into `app` over Wi-Fi, poll to done, then
    confirm RUNNING/fw_build/fingerprint/boot_guard all landed correctly.
    Stashes fw_build/zones fingerprint pre-update for OT-E02 and OT-E12."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    ap_password = ctx.get("ap_password")
    image_path = ctx.get("ota_image_path")
    expected_build = ctx.get("ota_image_build")
    if not ap_password or not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path/ap_password not provided for OT-E01")

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)

    fw_build_before = _fw_build(ctx, host)
    try:
        zones_before = zones.get_zones(host)
    except Exception:
        zones_before = None
    ctx["_ote_pre_update"] = {"fw_build": fw_build_before, "zones": zones_before}

    try:
        push = ota.push_esp_image(host, image_path, ap_password)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"push_esp_image raised {type(exc).__name__}: {exc}")
    if not push.ok:
        return CaseResult(Verdict.FAIL, reason=f"push_esp_image refused: status={push.status_code} body={push.body!r}")

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    phase = None
    deadline = now() + 180.0
    while now() < deadline:
        try:
            phase = ota.get_esp_status(host).get("phase")
        except Exception:
            phase = None
        if phase in ("done", "failed"):
            break
        sleep(2.0)

    running = _running_partition(ctx, host)
    fw_build_after = _fw_build(ctx, host)
    fingerprint_identical = None
    try:
        zones_after = zones.get_zones(host)
        fingerprint_identical = (_pid_gains(zones_before) == _pid_gains(zones_after)) if zones_before else None
    except Exception:
        pass
    boot_guard_recovery_mode = _boot_guard_recovery_mode(ctx, host)

    ctx["_ote01"] = {"fw_build_after": fw_build_after, "running_after": running}
    return J.judge_ota_push_applied(
        phase, running, "app",
        fw_build_matches_image=(fw_build_after == expected_build) if expected_build else None,
        fingerprint_identical=fingerprint_identical,
        boot_guard_recovery_mode=boot_guard_recovery_mode,
    )


def _case_ote02(ctx: dict) -> CaseResult:
    """OT-E02: rollback, then read back control_get_zones -- the
    ZONES_CFG_VERSION hazard. Requires OT-E01 to have run (a previous image
    to roll back to)."""
    pre = ctx.get("_ote_pre_update")
    if not pre:
        return CaseResult(Verdict.NOT_RUN, reason="OT-E01 did not run in this session")

    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    ap_password = ctx.get("ap_password")
    if not ap_password:
        return CaseResult(Verdict.SKIP, reason="no ap_password credential available for ota_rollback_esp")

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)
    dashboard = _dashboard_client(ctx)

    pid_gains_before = _pid_gains(pre.get("zones")) if pre.get("zones") else None

    try:
        ota.rollback_esp(host, ap_password)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"rollback_esp raised {type(exc).__name__}: {exc}")

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep
    sleep(5.0)  # let the reboot land before polling

    fw_build_after = None
    zones_config_load_fault = None
    deadline = now() + 60.0
    while now() < deadline:
        try:
            status = dashboard.get_status(host)
            fw_build_after = status.get("fw_build")
            zones_config_load_fault = status.get("zones_config_load_fault")
            if fw_build_after:
                break
        except Exception:
            pass
        sleep(2.0)

    pid_gains_after = None
    try:
        pid_gains_after = _pid_gains(zones.get_zones(host))
    except Exception:
        pass

    return J.judge_ota_rollback(
        fw_build_matches_pre_update=(fw_build_after == pre.get("fw_build")),
        pid_gains_before=pid_gains_before, pid_gains_after=pid_gains_after,
        zones_config_load_fault=zones_config_load_fault,
    )


def _case_ote03(ctx: dict) -> CaseResult:
    """OT-E03: push the ST-04 image with the last 4 KB flipped -- must be
    refused before reboot, RUNNING/fw_build unchanged."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    ap_password = ctx.get("ap_password")
    image_path = ctx.get("ota_corrupt_image_path")
    if not ap_password or not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_corrupt_image_path/ap_password not provided for OT-E03")

    ota = _ota_client(ctx)
    running_before = _running_partition(ctx, host)
    fw_build_before = _fw_build(ctx, host)

    refused = None
    try:
        push = ota.push_esp_image(host, image_path, ap_password)
        refused = not push.ok
    except Exception:
        refused = True  # a raised transport/HMAC error is also a refusal

    running_after = _running_partition(ctx, host)
    fw_build_after = _fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after)


def _case_ote12(ctx: dict) -> CaseResult:
    """OT-E12: otadata state recorded after every OT-E case in this
    session. Observer -- NOT_RUN if none of OT-E01/E02/E03 ran."""
    host = ctx.get("host")
    if not any(k in ctx for k in ("_ote01", "_ote_pre_update")):
        return CaseResult(Verdict.NOT_RUN, reason="no OT-E case ran in this session")
    running = _running_partition(ctx, host)
    # OT-E01/OT-E02 both expect the board to be running `app` afterward;
    # OT-E03's refusal case leaves it wherever it already was (checked by
    # OT-E03's own judge), so this observer's job is the app-expectation.
    return J.judge_ota_partitions_state(running, "app")


_CASE_FUNCS = {
    "OT-B01": _case_otb01,
    "OT-E01": _case_ote01,
    "OT-E02": _case_ote02,
    "OT-E03": _case_ote03,
    "OT-E12": _case_ote12,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
