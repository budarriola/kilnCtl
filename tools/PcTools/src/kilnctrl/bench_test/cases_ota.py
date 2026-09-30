"""OT-B01 + SP-04, OT-E01/E02/E03/E12, and (Wave 4) OT-P01..05 -- ESP
OTA-over-Wi-Fi update/rollback/corrupt-image handling, the dual
safety-processor reset trip, and Pico OTA relayed over the isolated link
(plan doc section 3.4/3.9), using `ota_http_client` directly (never the
MCP-tool text wrapper, per the task's own instruction) so results stay
structured dicts a pure judge function can compare.

Every case here is gated on the executor being idle before touching OTA or
issuing a dual reset -- CLAUDE.md's "never flash/OTA during a firing" -- and
every case is thin: fetch/act, then delegate to judgments.py.

OT-P* (Wave 4) was blocked until the two Pico OTA defects closed (erase-time
watchdog reset, `e59b0328`; CRC-variant mismatch, `ota_image_crc.c`) -- both
are fixed and confirmed flashed as of `73c1da94` (ROADMAP.md row L,
2026-09-21), so this wave implements the case bodies; it has still only ever
been unit-tested against mocked clients, never against a real Pico relay
(constraint: an attempt on defective firmware watchdog-resets the safety
processor, and no hardware access is in scope for the change that added
these cases). The Pico has no partition/fw_build HTTP surface the way the
ESP does (TODO.md 9.6) -- identity comes from the ESP's cached
GET_FW_VERSION reply (`safety_get_fw_version()`'s text) and boot-reason
comes from `safety_get_diag()`'s text, parsed by the `parse_fw_version_*`/
`parse_diag_boot_reason` helpers in judgments.py.
"""
from __future__ import annotations

import os
from typing import Any, Optional

from .. import web_auth_setup_http_client as _wac
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


def _interlock_ok(ctx: dict, host: str) -> "tuple[bool, str]":
    """Plan doc section 6 rule 1: every OTA action must first confirm
    ``GET /api/ota/interlock`` reports ``ok:true`` -- checked immediately
    before the call, never assumed from a run-start snapshot. Any error
    reading it refuses rather than assuming ok, same discipline as
    ``_is_idle`` above. Injectable via ``ctx["_interlock_fn"]`` so this can
    be unit-tested without a real board (this wave is unit/mock only, per
    the standing unacknowledged crash report on the bench board)."""
    ota = _ota_client(ctx)
    interlock_fn = ctx.get("_interlock_fn") or (lambda: ota.get_interlock(host))
    try:
        body = interlock_fn()
    except Exception as exc:
        return False, f"could not read /api/ota/interlock: {type(exc).__name__}: {exc}"
    ok = bool(body.get("ok"))
    return ok, ("" if ok else str(body.get("reason", "")))


def _relays_energized(ctx: dict, host) -> Optional[bool]:
    """``dashboard_status_t.safety_relay_energized`` -- same accessor
    ``cases_heat._read_energized`` uses. Landed-review gap fix for OT-B01:
    the dual reset previously only gated on the executor being idle, which
    does not by itself guarantee no relay is latched on via some other
    path (a stuck relay, a manual bench test, etc.); dual-resetting both
    processors while a relay is still energized is exactly the situation
    CLAUDE.md's dual-reflash procedure exists to avoid surprises around.
    Returns None (not a crash) on any read failure -- callers must treat
    that as "cannot confirm", never as "de-energized"."""
    dashboard = _dashboard_client(ctx)
    try:
        status = dashboard.get_status(host)
    except Exception:
        return None
    return status.get("safety_relay_energized")


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
    srv = _srv(ctx)
    ota = _ota_client(ctx)

    energized = _relays_energized(ctx, host)
    if energized is None:
        return CaseResult(Verdict.SKIP, reason="could not confirm relays de-energized before dual reset")
    if energized:
        return CaseResult(Verdict.SKIP, reason="refusing dual reset: a relay is still energized")

    sw_reset_fn = ctx.get("_sw_reset_fn", lambda: ota.sw_reset(host))
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
    image_path = ctx.get("ota_image_path")
    expected_build = ctx.get("ota_image_build")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E01")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)

    fw_build_before = _fw_build(ctx, host)
    try:
        zones_before = zones.get_zones(host)
    except Exception:
        zones_before = None
    ctx["_ote_pre_update"] = {"fw_build": fw_build_before, "zones": zones_before}

    try:
        push = ota.push_esp_image(host, image_path)
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
        fingerprint_identical = J.pid_gains_match(_pid_gains(zones_before), _pid_gains(zones_after)) if zones_before else None
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

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to roll back: {ireason}")

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)
    dashboard = _dashboard_client(ctx)

    pid_gains_before = _pid_gains(pre.get("zones")) if pre.get("zones") else None

    try:
        ota.rollback_esp(host)
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
    image_path = ctx.get("ota_corrupt_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_corrupt_image_path not provided for OT-E03")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    running_before = _running_partition(ctx, host)
    fw_build_before = _fw_build(ctx, host)

    refused = None
    try:
        push = ota.push_esp_image(host, image_path)
        refused = not push.ok
    except Exception:
        refused = True  # a raised transport error is also a refusal

    running_after = _running_partition(ctx, host)
    fw_build_after = _fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after)


def _case_ote04(ctx: dict) -> CaseResult:
    """OT-E04: push the first 60% of the ST-04 image (a truncated file) --
    same refusal contract as OT-E03 (judge_ota_push_refused): refused
    before reboot, RUNNING/fw_build unchanged."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_truncated_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_truncated_image_path not provided for OT-E04")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    running_before = _running_partition(ctx, host)
    fw_build_before = _fw_build(ctx, host)

    refused = None
    try:
        push = ota.push_esp_image(host, image_path)
        refused = not push.ok
    except Exception:
        refused = True  # a raised transport error is also a refusal

    running_after = _running_partition(ctx, host)
    fw_build_after = _fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after)


def _case_ote05(ctx: dict) -> CaseResult:
    """OT-E05: push `recovery.bin` (or any non-KilnCtrl app_desc) to
    /api/ota/esp -- refused naming the project mismatch, or rejected at
    verify without ever changing RUNNING. Same unchanged-state contract as
    OT-E03/E04 (judge_ota_push_refused); this module never inspects the
    board's refusal text for a specific substring, only that RUNNING/
    fw_build never moved."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_wrong_build_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_wrong_build_image_path not provided for OT-E05")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    running_before = _running_partition(ctx, host)
    fw_build_before = _fw_build(ctx, host)

    refused = None
    try:
        push = ota.push_esp_image(host, image_path)
        refused = not push.ok
    except Exception:
        refused = True

    running_after = _running_partition(ctx, host)
    fw_build_after = _fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after)


def _case_ote06(ctx: dict) -> CaseResult:
    """OT-E06: power loss mid-write -- an ``--attended`` operator case.
    Needs a human (or a fixture relay a human has wired up) to physically
    cut the ESP's own supply power partway through the transfer and then
    restore it; there is no way to do that from software alone, so this
    case is gated on an injectable ``ctx["attended_prompt"]`` callable
    (``str -> True | False | None``) another concurrent change
    (``bench3b``) is wiring the real ``--attended`` CLI mechanism for.
    ``None`` (no callable at all -- the flag was not passed) or a False/
    None answer from the operator both SKIP/INCONCLUSIVE rather than ever
    touching the board, so this case merges cleanly ahead of that
    mechanism landing.

    push_fn is injectable (``ctx["_push_fn"]``) because the real
    push_esp_image() call blocks synchronously for the whole streamed
    transfer -- in a real run the fixture/operator cuts power to the ESP
    *while* this call is in flight (a background thread or a second
    process, outside this pure case's scope), so the call is expected to
    raise (connection dropped) rather than return normally. The judge
    (judge_ota_power_loss_mid_write) does not require that specific
    failure shape -- what it actually checks is what the plan cares about:
    fw_build/RUNNING/fingerprint all read back exactly as they were before
    the push was ever attempted, once power is restored."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E06")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    prompt = ctx.get("attended_prompt")
    if prompt is None:
        return CaseResult(Verdict.SKIP, reason="requires --attended")

    ready = prompt(
        "OT-E06: be ready to cut power to the ESP's own supply (fixture_set_relay, "
        "or by hand) at roughly 40% OTA progress, then restore it. Confirm ready to proceed."
    )
    if ready is not True:
        reason = "operator did not respond to the power-cut prompt" if ready is None else "operator declined to proceed"
        return CaseResult(Verdict.SKIP, reason=reason)

    ota = _ota_client(ctx)
    zones = _zones_client(ctx)

    fw_build_before = _fw_build(ctx, host)
    try:
        zones_before = zones.get_zones(host)
    except Exception:
        zones_before = None

    push_fn = ctx.get("_push_fn") or (lambda: ota.push_esp_image(host, image_path))
    try:
        push_fn()
    except Exception:
        pass  # expected: the connection drops when the fixture/operator cuts power mid-write

    restored = prompt("OT-E06: confirm ESP supply power has been restored and the board has rebooted.")
    if restored is not True:
        reason = "operator did not confirm power restoration" if restored is None else "operator reported power was not restored"
        return CaseResult(Verdict.INCONCLUSIVE, reason=reason)

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    running_after = None
    fw_build_after = None
    deadline = now() + 60.0
    while now() < deadline:
        running_after = _running_partition(ctx, host)
        fw_build_after = _fw_build(ctx, host)
        if fw_build_after:
            break
        sleep(2.0)

    fingerprint_identical = None
    try:
        zones_after = zones.get_zones(host)
        fingerprint_identical = J.pid_gains_match(_pid_gains(zones_before), _pid_gains(zones_after)) if zones_before else None
    except Exception:
        pass

    return J.judge_ota_power_loss_mid_write(
        fw_build_before, fw_build_after, running_after, "app", fingerprint_identical,
    )


def _case_update_refused_during_state(
    ctx: dict, get_exec_state_fn, expected_state: str, image_ctx_key: str = "ota_image_path",
) -> CaseResult:
    """Shared body of OT-E07 (during a firing)/OT-E08 (during autotune):
    read the exec/autotune state, read the live interlock, attempt a push,
    then confirm the state is unchanged. Deliberately does NOT gate on
    `_is_idle`/`_interlock_ok` the way every other case in this module
    does -- the entire point of this pair is exercising the busy path, so
    treating "not idle"/"interlock not ok" as a precondition failure would
    make the case SKIP the very thing it exists to test. Both cases start
    their own short HP/AT run per the plan's fixed order footnote
    ("OT-E07/E08 (re-using a short HP/AT)"); the run itself is started and
    torn down by an injectable ``ctx["_start_state_fn"]``/
    ``ctx["_stop_state_fn"]`` pair so this stays unit-testable without
    actually driving a firing or an autotune step from this module (that
    machinery already lives in cases_heat.py/the AT-* cases and is not
    duplicated here)."""
    host = ctx.get("host")
    image_path = ctx.get(image_ctx_key)
    if not image_path:
        return CaseResult(Verdict.SKIP, reason=f"{image_ctx_key} not provided")

    start_fn = ctx.get("_start_state_fn")
    stop_fn = ctx.get("_stop_state_fn")
    started_here = False
    state_before = get_exec_state_fn()
    if state_before != expected_state:
        if start_fn is None:
            return CaseResult(
                Verdict.SKIP,
                reason=f"state is {state_before!r}, not {expected_state!r}, and no _start_state_fn was provided to start one",
            )
        try:
            start_fn()
        except Exception as exc:
            return CaseResult(Verdict.SKIP, reason=f"could not start {expected_state}: {type(exc).__name__}: {exc}")
        started_here = True
        state_before = get_exec_state_fn()

    try:
        if state_before != expected_state:
            return CaseResult(
                Verdict.INCONCLUSIVE,
                reason=f"state_before={state_before!r} after starting one, expected {expected_state!r}",
            )

        interlock_ok, _ireason = _interlock_ok(ctx, host)

        ota = _ota_client(ctx)
        push_fn = ctx.get("_push_fn") or (lambda: ota.push_esp_image(host, image_path))
        push_refused = None
        try:
            push = push_fn()
            push_refused = not getattr(push, "ok", False)
        except Exception:
            push_refused = True

        state_after = get_exec_state_fn()

        return J.judge_ota_update_refused_during_state(
            interlock_ok, push_refused, state_before, state_after, expected_state,
        )
    finally:
        if started_here and stop_fn is not None:
            try:
                stop_fn()
            except Exception:
                pass


#: `devices_profiles.ProfileExecStatus.STATE_NAMES` -- the real, LOWERCASE
#: vocabulary `state_name` actually returns. OT-E07's default was written as
#: "RUNNING" (the plan document's prose casing), which no real status can
#: ever equal, so the case could only ever report INCONCLUSIVE on a board
#: while its unit tests -- which fed the same fabricated "RUNNING" back in
#: -- stayed green. `test_bench_test_cases_ota.py` now pins both defaults
#: against the device modules' own STATE_NAMES so the two cannot drift
#: apart again.
OTE07_DEFAULT_STATE = "running"

#: Autotune has no single "active" state: `devices_autotune.AutotuneStatus.
#: STATE_NAMES` reports settling/stepping/relay_approach/relay_cycling while
#: a run is in flight, and which one is current legitimately changes DURING
#: the case (settling -> stepping), so comparing a raw state_name before and
#: after the push would fail on an ordinary, correct run. OT-E08 therefore
#: normalizes any in-flight state to this one label and compares that; a run
#: that drops to idle/done/aborted across the refused push still fails the
#: judge's state_after check, which is the thing the case exists to catch.
OTE08_ACTIVE_STATES = frozenset({"settling", "stepping", "relay_approach", "relay_cycling"})
OTE08_ACTIVE_LABEL = "active"


def _normalize_autotune_state(state_name):
    """Map any in-flight autotune state to OTE08_ACTIVE_LABEL, leaving
    idle/done/aborted/None as-is so they still read as "not active"."""
    return OTE08_ACTIVE_LABEL if state_name in OTE08_ACTIVE_STATES else state_name


def _case_ote07(ctx: dict) -> CaseResult:
    """OT-E07: an OTA update attempted while an HP-01-shaped firing is
    running (and, per the plan, again while paused -- pass
    ``ctx["_ote07_expected_state"] = "paused"``) must be refused, and the
    firing must continue unaffected."""
    get_state_fn = ctx.get("_exec_state_fn")
    if get_state_fn is None:
        srv = _srv(ctx)
        get_state_fn = lambda: getattr(srv._profiles.get_exec_status(), "state_name", None)
    expected_state = ctx.get("_ote07_expected_state", OTE07_DEFAULT_STATE)
    return _case_update_refused_during_state(ctx, get_state_fn, expected_state)


def _case_ote08(ctx: dict) -> CaseResult:
    """OT-E08: an OTA update attempted while an AT-01-shaped autotune is
    active must be refused (`ota_interlock.c:50`), and autotune must
    continue unaffected. The raw state_name is normalized to "active"
    first -- see OTE08_ACTIVE_STATES."""
    raw_state_fn = ctx.get("_autotune_state_fn")
    if raw_state_fn is None:
        srv = _srv(ctx)
        raw_state_fn = lambda: getattr(srv._autotune.get_status(), "state_name", None)
    get_state_fn = lambda: _normalize_autotune_state(raw_state_fn())
    expected_state = ctx.get("_ote08_expected_state", OTE08_ACTIVE_LABEL)
    return _case_update_refused_during_state(ctx, get_state_fn, expected_state)


def _case_ote09(ctx: dict) -> CaseResult:
    """OT-E09: POST /api/ota/esp with no admin session at all -- 401/403,
    nothing written. The AP-password HMAC challenge/response scheme this
    case used to also probe was retired 2026-09-29 (WEB_AUTH_PLAN.md item
    2b); with web auth on, ROUTE_TIER_ADMIN alone is the gate this case now
    exercises. **With web auth OFF, this route is unauthenticated by design
    (same as every other ADMIN route once auth is off) -- an unauthenticated
    push would then actually flash the board instead of being refused, which
    is not what this case is testing.** So this case only runs with web auth
    confirmed on; it SKIPs otherwise rather than risk a real flash."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E09")

    web_enabled_fn = ctx.get("_web_auth_enabled_fn")
    if web_enabled_fn is None:
        def web_enabled_fn():
            cfg = _wac.get_auth_config(host)
            return bool(cfg.get("web_enabled"))
    try:
        web_enabled = web_enabled_fn()
    except Exception as exc:
        return CaseResult(Verdict.SKIP, reason=f"could not read GET /api/auth/config to confirm web auth is on: {exc}")
    if not web_enabled:
        return CaseResult(Verdict.SKIP, reason="web auth is off -- an unauthenticated push would really flash the "
                                                "board instead of being refused, so OT-E09 does not run")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    ota = _ota_client(ctx)
    running_before = _running_partition(ctx, host)
    fw_build_before = _fw_build(ctx, host)

    push_fn = ctx.get("_push_no_credential_fn") or (lambda: ota.push_esp_image_unauthenticated(host, image_path))
    refused = None
    try:
        push = push_fn()
        refused = not push.ok
    except Exception:
        refused = True

    running_after = _running_partition(ctx, host)
    fw_build_after = _fw_build(ctx, host)

    return J.judge_ota_push_refused(refused, running_before, running_after, fw_build_before, fw_build_after)


def _case_ote10(ctx: dict) -> CaseResult:
    """OT-E10: with web auth on (WEB-SEC-03 turned it on earlier in this
    run), OT-E01 repeated with an ADMIN session cookie must succeed, and
    again with a ``user``-tier session must be refused -- ROUTE_TIER_ADMIN is
    this route's only gate now that the AP-password HMAC has been retired
    (2026-09-29, WEB_AUTH_PLAN.md item 2b), so this case exercises that tier
    check directly rather than a scheme replacement. Credentials come only
    from ctx overrides or the
    ``KILNCTL_WEB_*`` environment variables (same discipline as
    cases_web_rw.py's WEB-SEC-03: never hardcoded, never logged) -- SKIP
    if either the admin or the user-tier credential pair is missing,
    since there is no separate operator-visible knob for the second,
    non-admin account this case specifically needs."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_image_path not provided for OT-E10")

    admin_user = ctx.get("web_admin_username") or os.environ.get("KILNCTL_WEB_USERNAME")
    admin_pass = ctx.get("web_admin_password") or os.environ.get("KILNCTL_WEB_PASSWORD")
    user_user = ctx.get("web_user_username") or os.environ.get("KILNCTL_WEB_USER_USERNAME")
    user_pass = ctx.get("web_user_password") or os.environ.get("KILNCTL_WEB_USER_PASSWORD")
    if not all([admin_user, admin_pass, user_user, user_pass]):
        return CaseResult(
            Verdict.SKIP,
            reason=(
                "requires KILNCTL_WEB_USERNAME/PASSWORD (admin) and "
                "KILNCTL_WEB_USER_USERNAME/PASSWORD (a distinct user-tier account) -- credentials not provided"
            ),
        )

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    login_fn = ctx.get("_login_fn")
    if login_fn is None:
        return CaseResult(Verdict.SKIP, reason="no _login_fn provided to establish a web-auth session")

    admin_status, admin_cookie = login_fn(admin_user, admin_pass)
    if admin_status != 200 or not admin_cookie:
        return CaseResult(Verdict.FAIL, reason=f"admin login failed: status={admin_status!r}")

    user_status, user_cookie = login_fn(user_user, user_pass)
    if user_status != 200 or not user_cookie:
        return CaseResult(Verdict.FAIL, reason=f"user login failed: status={user_status!r}")

    ota = _ota_client(ctx)
    push_with_session_fn = ctx.get("_push_with_session_fn") or (
        lambda cookie: ota.push_esp_image_with_session(host, image_path, cookie)
    )

    admin_ok = None
    try:
        admin_result = push_with_session_fn(admin_cookie)
        admin_ok = bool(getattr(admin_result, "ok", False))
    except Exception:
        admin_ok = False

    user_refused = None
    try:
        user_result = push_with_session_fn(user_cookie)
        user_refused = not getattr(user_result, "ok", False)
    except Exception:
        user_refused = True

    return J.judge_ota_session_auth_tiers(admin_ok, user_refused)


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


# ---------------------------------------------------------------------------
# OT-P* -- Pico OTA relayed over the isolated link (Wave 4). See this
# module's docstring for the blocked->unblocked history and why identity is
# read from safety_get_fw_version()/safety_get_diag() text rather than an
# HTTP status surface the Pico doesn't have.
# ---------------------------------------------------------------------------

def _pico_commit_and_boot_reason(ctx: dict) -> "tuple[Optional[str], Optional[str]]":
    srv = _srv(ctx)
    try:
        fw_text = srv.safety_get_fw_version()
    except Exception:
        fw_text = ""
    try:
        diag_text = srv.safety_get_diag()
    except Exception:
        diag_text = ""
    return J.parse_fw_version_commit(fw_text), J.parse_diag_boot_reason(diag_text)


def _pico_boot_id(ctx: dict) -> Optional[int]:
    """Read `boot_id` out of the same GET_FW_VERSION cache
    `_pico_commit_and_boot_reason` uses. Stashed alongside commit/boot
    reason by OT-P01/OT-P02 so a later reviewer (or a future OT-P case)
    can check a Pico reboot actually happened via boot_id changing, the
    same "a real reboot was observed on the wire" signal
    get_pico_rollback_status()'s own "rebooting" status is defined by."""
    srv = _srv(ctx)
    try:
        fw_text = srv.safety_get_fw_version()
    except Exception:
        return None
    return J.parse_fw_version_boot_id(fw_text)


def _pico_trip_reason_mask(ctx: dict) -> "tuple[Optional[int], Optional[int], Optional[str]]":
    """Returns (reason, mask, why). `why` is None whenever `reason` was
    read successfully (even if that reason is 0, "no trip"); it is only
    populated to explain why `reason` is None -- either the transport call
    itself raised, or the diag text came back but didn't parse. Callers
    must not conflate "reason is None" (unreadable) with "reason is 0"
    (readable, no trip pending) -- see OT-P05's history of doing exactly
    that."""
    srv = _srv(ctx)
    try:
        diag_text = srv.safety_get_diag()
    except Exception as exc:
        return None, None, f"safety_get_diag raised {type(exc).__name__}: {exc}"
    reason = J.parse_trip_reason(diag_text)
    mask = J.parse_trip_mask(diag_text)
    why = None if reason is not None else "trip_reason unparseable from diag text"
    return reason, mask, why


def _pico_trip_pending(ctx: dict) -> "tuple[Optional[bool], Optional[str]]":
    """Returns (pending, why). `pending` is None only when the trip state
    could not be determined at all -- distinct from `False`, which means
    the trip state WAS read and no trip is pending. `why` explains a None
    `pending`; it is None whenever `pending` is not None."""
    reason, _mask, why = _pico_trip_reason_mask(ctx)
    if reason is None:
        return None, why
    return reason != 0, None


def _trip_is_the_clearable_s6a(ctx: dict) -> bool:
    """Plan section 6 rule 5: a trip may only be cleared once it is
    confirmed to be S6a and nothing else -- `trip_reason == 6` with
    `trip_mask == 1 << (trip_reason - 1)` (0x0020) and no other bit. Any
    other latched trip stops the run for a human; the harness leaves it
    alone. Anything unreadable is treated as "do not clear"."""
    reason, mask, _why = _pico_trip_reason_mask(ctx)
    if reason != 6 or mask is None:
        return False
    return mask == J.safety_trip_mask_for_reason(reason)


def _commissioning(ctx: dict, host: str):
    from .. import safety_cfg_http_client
    fn = ctx.get("_commissioning_fn") or (lambda: safety_cfg_http_client.get_commissioning(host))
    try:
        return fn()
    except Exception:
        return None


def _poll_pico_phase(ctx: dict, host: str, deadline_s: float = 180.0) -> "tuple[Optional[str], Optional[str]]":
    """Polls GET /api/ota/pico/status to a terminal phase (done/failed),
    same shape as OT-E01's ESP polling loop. Returns (phase, last_error)."""
    ota = _ota_client(ctx)
    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    phase = None
    last_error = None
    deadline = now() + deadline_s
    while now() < deadline:
        try:
            status = ota.get_pico_status(host)
            phase = status.get("phase")
            last_error = status.get("last_error")
        except Exception:
            phase = None
        if phase in ("done", "failed"):
            break
        sleep(2.0)
    return phase, last_error


def _case_otp01(ctx: dict) -> CaseResult:
    """OT-P01: relay a good Pico image into the inactive slot over the
    isolated link, poll to a terminal phase, then confirm identity/boot
    reason/commissioning via the ESP's safety-link cache (see module
    docstring for why there is no Pico-side HTTP status surface)."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_pico_image_path")
    expected_commit = ctx.get("ota_pico_image_commit")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_pico_image_path not provided for OT-P01")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    trip_pending, trip_why = _pico_trip_pending(ctx)
    if trip_pending is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"trip state unreadable: {trip_why}; refusing to push for OT-P01 rather than assuming no trip")
    if trip_pending:
        return CaseResult(Verdict.SKIP, reason="a trip is currently pending, refusing to start OT-P01")

    commit_before, _ = _pico_commit_and_boot_reason(ctx)
    commissioning_before = _commissioning(ctx, host)

    ota = _ota_client(ctx)
    try:
        push = ota.push_pico_image(host, image_path)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"push_pico_image raised {type(exc).__name__}: {exc}")
    if not push.ok:
        return CaseResult(Verdict.FAIL, reason=f"push_pico_image refused: status={push.status_code} body={push.body!r}")

    phase, last_error = _poll_pico_phase(ctx, host)
    commit_after, boot_reason = _pico_commit_and_boot_reason(ctx)
    boot_id_after = _pico_boot_id(ctx)
    commissioning_after = _commissioning(ctx, host)
    commissioning_identical = (
        (commissioning_before == commissioning_after)
        if commissioning_before is not None and commissioning_after is not None
        else None
    )

    ctx["_otp01"] = {
        "commit_before": commit_before, "commit_after": commit_after,
        "phase": phase, "last_error": last_error, "boot_reason": boot_reason,
        "boot_id_after": boot_id_after,
    }
    return J.judge_ota_pico_push_applied(
        phase, commit_after, expected_commit, boot_reason, commissioning_identical,
        last_error=last_error,
    )


def _case_otp02(ctx: dict) -> CaseResult:
    """OT-P02: roll the safety processor's bootloader back to the slot
    OT-P01 relayed away from, and confirm the pre-P01 commit is running
    again. Requires OT-P01 to have run this session (a slot to roll back
    from, and the pre-update commit to compare against)."""
    pre = ctx.get("_otp01")
    if not pre:
        return CaseResult(Verdict.NOT_RUN, reason="OT-P01 did not run in this session")
    # Never ask a Pico to roll back a slot it was never updated into: on a
    # flat, bootloader-less bench unit OT-P01's relay ends at the
    # running-image-overlap refusal (state 9) and there is no previous slot
    # to revert to. Belt-and-braces with the runner's own depends_on gate,
    # which only runs this case when OT-P01 read PASS.
    if pre.get("phase") != "done":
        return CaseResult(
            Verdict.INCONCLUSIVE,
            reason=f"OT-P01's relay ended at phase={pre.get('phase')!r}, so there is no new slot to roll back from",
        )

    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to roll back: {ireason}")

    commissioning_before = _commissioning(ctx, host)
    ota = _ota_client(ctx)
    try:
        ota.rollback_pico(host)
    except Exception as exc:
        return CaseResult(Verdict.FAIL, reason=f"rollback_pico raised {type(exc).__name__}: {exc}")

    now = ctx.get("_now")
    sleep = ctx.get("_sleep")
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    rollback_status = None
    deadline = now() + 60.0
    while now() < deadline:
        try:
            rollback_status = ota.get_pico_rollback_status(host).get("status")
        except Exception:
            rollback_status = None
        if rollback_status not in (None, "idle", "pending"):
            break
        sleep(2.0)

    commit_after, _ = _pico_commit_and_boot_reason(ctx)
    commissioning_after = _commissioning(ctx, host)
    commissioning_identical = (
        (commissioning_before == commissioning_after)
        if commissioning_before is not None and commissioning_after is not None
        else None
    )
    return J.judge_ota_pico_rollback(
        rollback_status, commit_after, pre.get("commit_before"), commissioning_identical,
    )


def _case_otp03(ctx: dict) -> CaseResult:
    """OT-P03: a CRC-corrupted Pico image must be refused/fail the staged
    check before the bootloader ever switches slots -- same 'refused
    means truly untouched' contract as OT-E03, but for the Pico's commit
    and boot reason rather than the ESP's RUNNING/fw_build."""
    idle, reason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=reason)

    host = ctx.get("host")
    image_path = ctx.get("ota_pico_corrupt_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_pico_corrupt_image_path not provided for OT-P03")

    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    trip_pending, trip_why = _pico_trip_pending(ctx)
    if trip_pending is None:
        return CaseResult(Verdict.INCONCLUSIVE, reason=f"trip state unreadable: {trip_why}; refusing to push for OT-P03 rather than assuming no trip")
    if trip_pending:
        return CaseResult(Verdict.SKIP, reason="a trip is currently pending, refusing to start OT-P03")

    commit_before, _ = _pico_commit_and_boot_reason(ctx)

    ota = _ota_client(ctx)
    refused_or_failed = None
    last_error = None
    try:
        push = ota.push_pico_image(host, image_path)
        if not push.ok:
            refused_or_failed = True
        else:
            phase, last_error = _poll_pico_phase(ctx, host)
            refused_or_failed = phase == "failed"
    except Exception:
        refused_or_failed = True  # a raised transport/CRC error is also a refusal

    commit_after, boot_reason = _pico_commit_and_boot_reason(ctx)
    return J.judge_ota_pico_bad_image_fallback(
        refused_or_failed, commit_before, commit_after, boot_reason, last_error=last_error,
    )


def _case_otp04(ctx: dict) -> CaseResult:
    """OT-P04: judges OT-P01's own captured relay specifically for the
    2026-09-18 erase-time watchdog defect's two documented symptoms.
    Observer only -- NOT_RUN if OT-P01 did not run this session."""
    pre = ctx.get("_otp01")
    if not pre:
        return CaseResult(Verdict.NOT_RUN, reason="OT-P01 did not run in this session")
    return J.judge_ota_pico_no_watchdog_signature(pre.get("boot_reason"), pre.get("last_error"))


def _case_otp05(ctx: dict) -> CaseResult:
    """OT-P05: a Pico update attempted while a real trip is latched must be
    refused, with the running commit left exactly unchanged. Requires a
    trip already pending (e.g. FL-11's S6a, before it is cleared) --
    INCONCLUSIVE if none is, per judgments.judge_ota_pico_refused_with_
    trip_pending. The harness clears the trip afterward ONLY when it is
    confirmed to be S6a and nothing else (plan section 6 rule 5), via the
    same clear-trip path SP-08/SP-09 use, injectable as
    ctx['_clear_trip_fn'] for testing."""
    host = ctx.get("host")
    image_path = ctx.get("ota_pico_image_path")
    if not image_path:
        return CaseResult(Verdict.SKIP, reason="ota_pico_image_path not provided for OT-P05")

    # Plan section 6 rule 1 applies here exactly as to every other OTA
    # action in this module: executor idle and GET /api/ota/interlock ok,
    # checked immediately before the push, never assumed from the fact a
    # trip is latched.
    idle, ireason = _is_idle(ctx)
    if not idle:
        return CaseResult(Verdict.SKIP, reason=ireason)
    ok, ireason = _interlock_ok(ctx, host)
    if not ok:
        return CaseResult(Verdict.SKIP, reason=f"OTA interlock not ok, refusing to push: {ireason}")

    trip_pending, trip_why = _pico_trip_pending(ctx)
    commit_before, _ = _pico_commit_and_boot_reason(ctx)

    if trip_pending is None:
        return CaseResult(Verdict.SKIP, reason=f"trip state unreadable: {trip_why}")

    push_refused = None
    if trip_pending:
        ota = _ota_client(ctx)
        try:
            push = ota.push_pico_image(host, image_path)
            push_refused = not push.ok
        except Exception:
            push_refused = True

    commit_after, _ = _pico_commit_and_boot_reason(ctx)
    result = J.judge_ota_pico_refused_with_trip_pending(trip_pending, push_refused, commit_before, commit_after)

    if trip_pending and _trip_is_the_clearable_s6a(ctx):
        clear_fn = ctx.get("_clear_trip_fn") or (lambda: _default_clear_trip_fn(ctx))
        try:
            clear_fn()
        except Exception:
            pass
    return result


_CASE_FUNCS = {
    "OT-B01": _case_otb01,
    "OT-E01": _case_ote01,
    "OT-E02": _case_ote02,
    "OT-E03": _case_ote03,
    "OT-E04": _case_ote04,
    "OT-E05": _case_ote05,
    "OT-E06": _case_ote06,
    "OT-E07": _case_ote07,
    "OT-E08": _case_ote08,
    "OT-E09": _case_ote09,
    "OT-E10": _case_ote10,
    "OT-E12": _case_ote12,
    "OT-P01": _case_otp01,
    "OT-P02": _case_otp02,
    "OT-P03": _case_otp03,
    "OT-P04": _case_otp04,
    "OT-P05": _case_otp05,
}
for _cid, _fn in _CASE_FUNCS.items():
    get_case(_cid).judge = _fn
