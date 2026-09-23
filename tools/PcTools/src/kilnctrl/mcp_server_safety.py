"""SAFETY tools (task 7).

Part of the mcp_server.py split (pure refactor) -- moved verbatim, no
logic changes. See mcp_server.py's module docstring for the overall map.
"""
from __future__ import annotations

import asyncio
import dataclasses
import functools
import glob
import json
import math
import logging
import os
import subprocess
import sys
import threading
import time
from collections import deque
from typing import Any, Callable, Optional

from . import actions, config_presets, dashboard_http_client, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, safety_log_level_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
from .autotune import AutotuneClient, AutotuneQueryError
from .control import ControlClient, ControlQueryError
from .device_log import LogClient
from .devices import LogLine
from .display import BlitError, DisplayClient, DisplayQueryError
from .touch import TouchClient, TouchQueryError
from .ui_test_client import UiTestClient, UiTestQueryError
from .web_ui_client import WebUiClient
from .info import InfoClient, InfoQueryError
from .system import SystemClient, SystemQueryError
from .io_expander import IoClient, IoQueryError
from .link_hub import get_shared_link
from .profiles import ProfilesClient, ProfilesQueryError
from .protocol import (
    FACTORY_RESET_SCOPE_KILN,
    PROFILES_SAVE_ID_NEW,
    profile_id_is_builtin,
    THERMO_CHANNEL_ALL,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    WIFI_MODE_AP,
    WIFI_MODE_HOME,
    Device,
    Frame,
    FrameError,
    LogLevel,
    MsgType,
    unstuff,
)
from . import ota_http_client as ota_http
from . import probe
from .probe import ProbeClient, ProbeQueryError
from .wifi_uart import WifiUartClient, WifiUartQueryError
from .safety import SafetyClient, SafetyQueryError
from .serial_link import list_ports, recommend_port
from .session_log import SessionLogger
from .thermo import ThermoClient, ThermoQueryError

from . import mcp_server as _srv


# ---------------------------------------------------------------------------
# SAFETY -- the opto-isolated RP2040 link (task 7)
#
# The Pico owns the safety thermocouple, the current sensors, the E-stop and
# relay K4. The ESP polls it and caches the answer, so a down link is a
# successful query reporting link_up = 0 -- not an error.
#
# The RP2040 firmware does not exist in this repository yet: link_up = 0 with
# "never received" is the expected steady state today.
# ---------------------------------------------------------------------------
@_srv._tool()
def safety_get_status() -> str:
    """Read the cached safety status from the safety processor.

    Covers link state, the isolated fault line (which THIS firmware drives as
    an output), E-stop, safety relay K4, heating enable, the safety
    thermocouple and the three current-sense channels, plus how old the data
    is.

    Expected result today: no flags set and "never received" -- the RP2040
    firmware that would answer does not exist yet. That is not a fault.

    Also appends a borrowed-sensor note when GET /api/status's
    `safety_tc_is_separate_sensor` (dashboard_status_http.c) is reachable and
    reports False -- the ESP's own confirmed-borrowed predicate
    (safety_tc_is_separate_physical_sensor() in firmware/KilnFW/App/drivers/
    safety/safety_link.h), the same field every KilnFW display now gates on
    (b90fcb3). This is an EXTRA live HTTP round trip beyond the cache-only
    UART query above -- unlike safety_get_ct_cal, it fails silently (no note
    appended, temperature still reported) rather than erroring the whole
    call, since a board with no HTTP reachable (serial-only bench setup) must
    not lose the cached UART status just because the borrowed check could not
    run. Fail-to-shown: a missing field or a failed fetch means no note is
    added, same as the firmware's own "unknown must fail to shown" rule.

    CT_COMMISSIONING_PLAN.md step 4 (PcTools half, 8a124c44 left this open):
    the three current-sense channels are rendered "not fitted" rather than a
    fabricated 0.00 A when GET /api/status's `ct_fitted` (dashboard_status_
    http.c, param 0x031F's committed ct_topology) says so -- channels 0/1 in
    summed-CT topology, since only GPIO28/channel 2 is wired there.
    `ct_summed_attrib_zone` is surfaced alongside as "zone N" (exactly one
    zone presently commanded on) or "-" (none, or more than one). Since
    amps_valid never crosses the safety-link wire itself, this can only come
    from GET /api/status; if that call is unreachable, this falls back to
    the commissioning param's own `ct_topology` (GET /api/safety/
    commissioning) for the fitted/not-fitted split alone -- attribution to a
    specific zone is not available from that source, so it reads "-" in the
    fallback path. With neither source reachable, all three channels print
    their raw amps (old behaviour, topology unknown).
    """
    try:
        status = _srv._safety.get_status()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as safety_get_commissioning()
    try:
        host = _ota_resolve_host(None)
    except Exception:
        host = None
    http_status: "dict | None" = None
    if host is not None:
        try:
            http_status = dashboard_http_client.get_status(host)
        except Exception:
            http_status = None

    ct_fitted: "tuple[bool, bool, bool] | None" = None
    ct_summed_attrib_zone: "int | None" = None
    raw_fitted = http_status.get("ct_fitted") if isinstance(http_status, dict) else None
    if isinstance(raw_fitted, list) and len(raw_fitted) == 3:
        ct_fitted = (bool(raw_fitted[0]), bool(raw_fitted[1]), bool(raw_fitted[2]))
        raw_zone = http_status.get("ct_summed_attrib_zone")
        ct_summed_attrib_zone = raw_zone if isinstance(raw_zone, int) else None
    elif host is not None:
        # GET /api/status unavailable/missing the field -- fall back to the
        # commissioning param's own ct_topology. No per-zone attribution in
        # this path (ct_summed_attrib_zone stays None -> renders "-").
        try:
            commissioning = safety_cfg_http_client.get_commissioning(host)
        except Exception:
            commissioning = None
        if isinstance(commissioning, dict) and commissioning.get("unset_reporting_reliable"):
            params = safety_cfg_http_client.params_by_name(commissioning)
            ct_topology_p = params.get("ct_topology")
            if ct_topology_p and ct_topology_p.get("set"):
                summed = bool(ct_topology_p.get("value"))
                ct_fitted = (False, False, True) if summed else (True, True, True)

    text = status.describe(ct_fitted=ct_fitted, ct_summed_attrib_zone=ct_summed_attrib_zone)
    if isinstance(http_status, dict) and http_status.get("safety_tc_is_separate_sensor") is False:
        text += " | safety TC: borrowed from a zone probe (same probe, not a second sensor)"
    if isinstance(http_status, dict):
        # ct_counts: raw ADC counts per channel (dashboard_status_http.c,
        # LINK_PROTOCOL.md Frame E, 2026-09-06) -- independent of
        # calibration, closing CURRENT_SENSE.md sec 4's "no path from the
        # running board to raw ADC counts" gap. Only over HTTP (like the
        # borrowed-sensor note above): the UART GET_STATUS query this
        # function otherwise uses is Frame A, which never carried this
        # field -- it rides Frame E (POWER), cached ESP-side and surfaced on
        # GET /api/status. null (not a list) on a pre-protocol-11 Pico or a
        # board that has never received a POWER frame -- fail-to-shown, same
        # convention as the borrowed-sensor note.
        counts = http_status.get("ct_counts")
        if isinstance(counts, list) and len(counts) == 3:
            text += f" | ct_counts {counts[0]}, {counts[1]}, {counts[2]}"
    return text


@_srv._tool()
def safety_get_link_stats() -> str:
    """Read the ESP's own counters for the isolated UART.

    Frames sent and timeouts climbing while received stays at zero is exactly
    the signature of the missing Pico firmware.
    """
    try:
        stats = _srv._safety.get_link_stats()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    return stats.describe()


@_srv._tool()
def safety_get_diag() -> str:
    """Read the ESP's cache of the Pico's last DIAG (Frame B) push --
    guard state, warn/trip masks, PUSH_CONTEXT liveness, and (the reason
    this exists) the Pico's *last-boot reason*: power-on, watchdog, or
    brownout (kilnlink_diag.h's boot_reason bits). Before this tool, an
    unexpected Pico reboot had no PC-side way to say why -- the wire
    already carries the answer (link_task.c fills boot_reason on every DIAG
    push from watchdog_caused_reboot()/watchdog_enable_caused_reboot()) but
    nothing surfaced it. Cache-only, like safety_get_status/
    safety_get_link_stats: never a live round trip to the Pico.

    "never received" (ever_received=False) is the expected state with no
    Pico firmware attached or the link never up -- not an error.
    """
    try:
        diag = _srv._safety.get_diag()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    return diag.describe()


@_srv._tool()
def safety_get_fw_version() -> str:
    """Read the ESP's cache of the Pico's own build identity (FW_VERSION /
    Frame C): commit, build datetime, dirty bit, boot_id, and the active
    config version/CRC on the safety processor.

    This is the Pico-side counterpart to get_fw_version() (which reports the
    ESP's own build) -- the only place today that surfaces which firmware the
    RP2040 safety processor is actually running. Cache-only, like
    safety_get_status/safety_get_diag: never a live round trip to the Pico.
    An empty/unknown commit means the Pico has never reported a build
    identity (no Pico firmware attached, or link never up), not an error.
    """
    try:
        version = _srv._safety.get_fw_version()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    return version.describe()


@_srv._tool()
def safety_request_enable(enable: bool) -> str:
    """Ask the safety processor to permit (or drop) heating.

    Advisory only: the Pico can refuse, and its own interlocks always win --
    that outcome never comes back here. What this DOES report is an ESP-side
    refusal (a truncated frame) caught before the request ever reached the
    Pico.
    """
    try:
        result = _srv._safety.request_enable(enable)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - requested enable={enable}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not request enable={enable}{detail}"


@_srv._tool()
def safety_ping() -> str:
    """Force an immediate poll of the safety processor."""
    return _srv._send(UART_TASK_ID_SAFETY, devices.safety_ping())


@_srv._tool()
def safety_set_poll_period(period_ms: int) -> str:
    """Set how often the ESP polls the safety processor, in ms (0 stops polling)."""
    try:
        result = _srv._safety.set_poll_period(period_ms)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - poll period {period_ms} ms"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set poll period{detail}"


@_srv._tool()
def safety_set_fault_out(assert_fault: bool) -> str:
    """Drive the isolated Fault line to the safety processor.

    This is an ESP **output** (GPIO6 -> optocoupler U1 -> the Pico's mainFault
    input): us telling the safety processor that the main controller has
    faulted. It is not a signal coming back from the Pico -- there is no
    hardware path for that at all.

    The firmware asserts this by itself on PC-link loss, a thermocouple fault
    or a watchdog trip; this tool is a manual override of that.
    """
    try:
        result = _srv._safety.set_fault_out(assert_fault)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - fault out={assert_fault}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set fault out{detail}"


@_srv._tool()
def safety_clear_trip() -> str:
    """Clear a latched safety trip on the safety processor.

    A trip LATCHES on the Pico: once `safety_guards_tick()` sets `is_tripped`
    it returns early and stops re-evaluating, so removing whatever caused the
    trip does not clear it by itself. A board that tripped because the
    isolated fault line went high stays tripped after the line goes low. This
    is the only way out short of a power cycle.

    Clearing is a request, not an order. `link_task_handle_clear_trip()` calls
    `safety_guards_try_clear()`, which re-checks the guard against live inputs
    and refuses while the condition still holds -- so this cannot be used to
    dismiss a fault that is still present. If the trip does not clear, the
    cause is still there.

    Fire-and-forget: no reply on the wire. Check `safety_get_status()`
    afterwards to see whether it actually cleared.
    """
    return _srv._send(UART_TASK_ID_SAFETY, devices.safety_clear_trip())


@_srv._tool()
def safety_set_log_level(level: int, peer: Optional[str] = None, host: Optional[str] = None) -> str:
    """Set the safety processor's runtime UART log verbosity: 0=ERROR,
    1=WARN, 2=INFO, 3=DEBUG, 4=VERBOSE.

    Calls POST /api/safety/log_level (dashboard_settings_http.c's
    safety_log_level_post_handler(), ROUTE_TIER_ADMIN), which forwards a
    SET_LOG_LEVEL frame to the Pico over the isolated safety link. This
    route was found fully built and registered with no client anywhere
    during the 2026-09-20 "no client" audit -- this tool is that client.

    Diagnostic only: it changes what the Pico logs, nothing about its
    guards, trips, or relays. Fire-and-forget over the link, same as
    safety_clear_trip()/safety_set_poll_period() -- the ESP's {"ok":true}
    means the frame was queued, not that the Pico applied it; there is no
    read-back to confirm the level landed (the Pico does not currently
    report its own log level anywhere this tool can read).

    `peer` selects which log stream the level applies to: "safety" (the
    Pico itself, over the isolated link) or "relay" (a local ESP-side
    filter on the relayed Pico log stream). Leave it None to send no peer
    field, which the firmware handler treats as its historical "safety"
    default.

    Host is auto-resolved the same way the OTA/control tools do; pass
    `host` explicitly for kilnctl.local or a board reachable only from a
    different network than this link's serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as safety_get_commissioning()

    resolved = _ota_resolve_host(host)
    try:
        response = safety_log_level_http_client.set_safety_log_level(resolved, level, peer=peer)
    except safety_log_level_http_client.SafetyLogLevelHttpError as exc:
        return f"error setting safety log level over HTTP (host={resolved}): {exc}"
    if response.get("ok"):
        name = (safety_log_level_http_client.SAFETY_LOG_LEVEL_NAMES[level]
                if 0 <= level < len(safety_log_level_http_client.SAFETY_LOG_LEVEL_NAMES) else str(level))
        return (f"ok - queued log level {level} ({name}) for "
                f"{peer or 'safety'} (host={resolved})")
    return f"refused - board reported: {response} (host={resolved})"


@_srv._tool()
def safety_set_tc_type(tc_type_name: str, host: Optional[str] = None) -> str:
    """Commission the safety processor's thermocouple type (config_store.h's
    ``tc_type`` field, config_params.c id ``0x0105``).

    `tc_type_name` is one of "B", "E", "J", "K", "N", "R", "S", "T"
    (case-insensitive), mirroring firmware/SaftyFW/src/max31856.h's
    MAX31856_TC_TYPE_* ordering.

    Until 2026-09-17 this sent only the raw wire command (SAFETY_CMD_SET_CONFIG
    0x16, still available as ``devices.safety_set_config()``/
    ``SafetyClient.set_config()`` for anything that needs the bare nibble),
    which is fire-and-forget on the Pico's own say-so: no reply names success
    or failure, so a caller had no way to tell "commissioned" from "silently
    refused because the relay was ARMED" without a separate poll days later.
    That is exactly the failure class this project has been bitten by
    repeatedly (`flash_firmware()` once reported success while the board kept
    running the old image; a boot_guard NVS write once returned OK while the
    value never persisted) -- CLAUDE.md now names it explicitly.

    `tc_type` is ALSO one of GET/POST /api/safety/commissioning's known
    parameters (config_params.c id 0x0105, same underlying
    ``config_store_record_t.tc_type`` field the wire command writes,
    `CHECK_U8_MAX(7u)`-bounded exactly like SAFETY_TC_TYPE_NAMES's 0-7
    range), which already has a tested, independently-verifying write path
    (`safety_cfg_http_client.apply_safety_fields`, the same one
    `safety_set_rate_guard()`/`safety_set_commissioning_fields()` use). This
    tool now goes through THAT path instead of the raw wire command: it
    stages `tc_type` via SET_PARAM, commits, and -- with `verify=True` --
    independently re-reads the commissioning page afterward and confirms
    `tc_type` now reads back SET and equal to what was requested, rather than
    trusting the POST's own ``{"ok":true}``. A field that reads back UNSET or
    with a different value after the commit is reported as a failure here,
    not a pass.

    REFUSES if a profile is firing or an autotune run is in progress (same
    check safety_set_rate_guard()/safety_set_commissioning_fields() make,
    via profiles_get_exec_status()/autotune_get_status(), BEFORE anything is
    sent over the wire) -- config_store.c does allow a tc_type-ONLY change
    while ARMED as long as heat is currently off (`config_store_decide_write_ex`'s
    `tc_type_only_change` relaxation), but a live run is still not a safe
    time to change what sensor type the safety processor is decoding.

    If the write is refused because the board is not in the post-reset GRACE
    window and the tc_type-only relaxation above does not apply (heat is
    on), the error names that explicitly and says to call
    ``debug_reset(peer="pico")`` then retry within 60 seconds, same as the
    other commissioning-field tools.

    Host is auto-resolved the same way safety_get_commissioning()/
    safety_set_rate_guard() do; pass `host` explicitly for kilnctl.local or
    a board reachable only from a different network than this link's serial
    port.
    """
    name = tc_type_name.strip().upper()
    if name not in devices.SAFETY_TC_TYPE_NAMES:
        known = ", ".join(sorted(devices.SAFETY_TC_TYPE_NAMES))
        return f"error: unknown tc_type_name {tc_type_name!r} -- expected one of {known}"
    value = devices.SAFETY_TC_TYPE_NAMES[name]

    busy = _profile_or_autotune_running()
    if busy is not None:
        return f"refused: {busy} -- writing a new tc_type mid-run is not safe; stop it first"

    from .mcp_server_ota import _ota_resolve_host

    resolved = _ota_resolve_host(host)
    try:
        result = safety_cfg_http_client.apply_safety_fields(resolved, {"tc_type": value}, verify=True)
    except safety_cfg_http_client.SafetyCfgUnknownParamError as exc:
        return f"error: {exc}"
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return f"error writing tc_type over HTTP (host={resolved}): {exc}"

    if not result.ok:
        reason = result.post_reason or "; ".join(result.mismatches) or "unknown failure"
        if "ARMED" in reason:
            return (
                "refused: the safety processor rejected the write because the relay is ARMED "
                "-- config writes only land during the 60s post-reset GRACE window (or, for "
                "tc_type specifically, while heat is off). Call debug_reset(peer=\"pico\") and "
                "then call safety_set_tc_type(...) again within 60 seconds. "
                f"(board said: {reason})"
            )
        return f"failed: {reason}"

    return (f"ok - tc_type {name} written and confirmed by read-back: "
            f"tc_type={value} | board reports commissioned: {result.commissioned_after}")


@_srv._tool()
def safety_get_ct_cal() -> str:
    """Read the safety processor's three CT channels' stored end-to-end amps
    CORRECTION (config_store.h's legacy `ct_cal` gain/offset record --
    SAFETY_CMD_GET_CT_CAL / 0x22). Read-only survivor of a removed write
    surface, kept for visibility into whatever a channel's `ct_cal` record
    already holds from before the write path (formerly `safety_set_ct_cal`,
    which wrote `ct_cal[]` via wire command 0x19) was removed 2026-09-08.

    THIS IS NOT THE CT CALIBRATION PROCEDURE. It never was, despite the
    name: `ct_cal[]` is applied to `amps[]` AFTER the ADC-counts-to-amps
    conversion (current_sense.c) and only ever fed the S14/S15 WARN-only
    over/under-current display thresholds -- never S3 (LOAD_STUCK_ON) or any
    other guard that decides whether current is present at all. An agent
    clearing a latched S3 trip called the old `safety_set_ct_cal` expecting
    it to commission the CT and it silently did nothing relevant: the trip
    stayed latched until `zero_counts[2]` (a completely different field,
    `config_params.c` id 0x0304) was corrected instead. See
    `docs/CURRENT_SENSE.md` section 5 and
    `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md` for the real procedure:
    commission `A_fs`/`zero_mv` per channel (the commissioning page's CT
    rows, `POST /api/safety/commissioning/ct_cal` -- a DIFFERENT, ESP-local
    endpoint that despite sharing the word "ct_cal" writes `k_ct_v_per_a`/
    `zero_counts`, the fields the guards actually read), then auto-zero.

    UNLIKE safety_get_status/other SAFETY queries, this is a LIVE round trip:
    it is answered by the ESP asking the Pico right now, not from a cache, so
    it can take noticeably longer and a dead isolated link shows up here as
    an error (a timeout) rather than as a successful reply with stale data.

    Each channel's ``calibrated`` flag must be checked before trusting its
    gain/offset -- an uncalibrated channel's numbers are meaningless
    (current_sense.c never reads them).
    """
    try:
        cal = _srv._safety.get_ct_cal()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    lines = []
    for i, ch in enumerate(cal.channels):
        if ch.calibrated:
            lines.append(f"channel {i}: calibrated, gain={ch.gain:.6g}, offset={ch.offset:.6g}")
        else:
            lines.append(f"channel {i}: uncalibrated")
    return "; ".join(lines)


@_srv._tool()
def safety_get_param(param_id: int) -> str:
    """READ-ONLY: ask the safety processor for one CONFIG_REFERENCE.md field
    by its opaque u16 wire id (SAFETY_CMD_GET_PARAM / 0x23), and report
    exactly what came back -- the decoded value if found, "not found" if the
    Pico doesn't recognize the id, or a refusal, honestly, with no outcome
    disguised as another.

    GET-ONLY diagnostics ids are the intended use of this call -- e.g. a
    future 0x0505 (config RAM-integrity fail count, landing separately)
    rather than anything already reachable in bulk via GET_CONFIG_PAGE.

    WIRED END TO END since uart_bridge_safety.c's SAFETY_CMD_GET_PARAM case
    (safety_link_get_param()): uart_bridge_safety.c's
    safety_bridge_task() now has a SAFETY_CMD_GET_PARAM case that relays a
    live Pico round trip (safety_link_get_param() -- CommonFW's
    kilnlink_get_param.c/kilnlink_param.c), so a real request reaches the
    safety processor and this call reports its actual answer: the decoded
    value if found, "not found" if the Pico doesn't recognize the id, or a
    genuine refusal (e.g. an old Pico predating KILNLINK_PROTOCOL_VERSION 7).

    UNLIKE safety_get_ct_cal, a refusal is reported as plain text here
    rather than raised as an error, so a version-skewed or unrecognized id
    reads as a normal, non-exceptional result rather than an exception. A
    genuine communication failure (timeout, no reply at all) still raises.
    """
    try:
        result = _srv._safety.get_param(param_id)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if isinstance(result, devices.OkReason):
        if result.ok:
            # An OkReason with ok=True is not a refusal -- describe() would
            # say "ok" here, which read as "refused - ok" before this fix.
            # Nothing today produces this branch (every GET_PARAM reply is
            # either a SafetyGetParam or an ok=False refusal), but report it
            # plainly rather than mislabeling it a refusal if that changes.
            return f"param_id={param_id}: ok, no value ({result.describe()})"
        return f"param_id={param_id}: refused - {result.reason or 'unsupported'}"
    if not result.found:
        return f"param_id={param_id}: not found"
    return f"param_id={param_id}: found, type={result.type}, value={result.value!r}"


@_srv._tool()
def safety_get_ct_cal_raw(host: Optional[str] = None) -> str:
    """READ-ONLY: the REAL, LIVE per-channel CT commissioning record, raw and
    unrounded, as a JSON object -- ``k_ct_v_per_a[0..2]``, ``zero_counts[0..2]``,
    ``gain[0..2]``, ``ct_installed``, ``ct_topology``, ``i_present_a``.

    DO NOT CONFUSE THIS WITH safety_get_ct_cal(). That tool reads a
    DIFFERENT, UNRELATED, legacy table (`config_store.h`'s `ct_cal[]`
    gain/offset correction, applied to amps AFTER the ADC conversion and fed
    only to the S14/S15 WARN-only thresholds) and reports every channel
    "uncalibrated" on boards whose real calibration is fine -- an agent
    already made exactly that mistake and reported a false gap. THIS tool
    reads the record that actually gates commissioning and that
    current_sense.c's A-to-amps conversion actually uses: `A_fs`/`zero_mv`
    converted via `safety_ct_cal_convert()` into `k_ct_v_per_a`/
    `zero_counts`, alongside the front-end `gain` and the `ct_installed`/
    `ct_topology`/`i_present_a` scalars that go with it. Zero means
    uncalibrated for `k_ct_v_per_a` specifically -- everything else is
    returned exactly as the board reports it.

    Fetched over the SAME cached GET /api/safety/commissioning
    safety_get_commissioning() uses (safety_cfg_http_client.get_commissioning()),
    NOT a fresh live round trip to the Pico -- see that tool's own docstring
    for the cache/staleness caveat (`stale`/`live_config_crc` vs
    `cached_config_crc` in the raw response, not surfaced by this tool
    directly since it reports only the CT fields; call
    safety_get_commissioning() alongside this one if staleness matters).

    Each field is one of three distinguishable outcomes, never collapsed
    into each other:
      * ``{"set": true, "value": 0.0}`` -- the board reports this field
        SET, and its real value is exactly zero (e.g. an uncalibrated
        `k_ct_v_per_a` channel, or a genuinely CT-absent `ct_installed`).
      * ``{"set": false}`` -- the board's table has this field but reports
        it UNSET; no "value" key is present at all, so an unset field can
        never be mistaken for a numeric 0.
      * ``null`` -- the board's own parameter table has no such field at
        all (older/newer firmware). Different from both of the above.
    A whole-call failure (board unreachable, HTTP error, bad JSON) returns
    ``{"ok": false, "error": "..."}`` instead of any field -- never a
    default, never a guess, and never confused with a reachable board
    reporting unset/zero fields.

    ``i_present_a_manual`` (whether the idle-current baseline was set by
    hand vs auto-derived) is always ``null`` here with an explanatory
    ``i_present_a_manual_note``: SaftyFW's `i_present_a_manual` flag is
    internal-only and never serialized onto the wire by
    GET /api/safety/commissioning -- confirmed by reading
    firmware/SaftyFW/src/config_params.c, not inferred from a failed
    lookup. That is a firmware-side gap this tool cannot work around; no
    firmware route was added as part of closing this facade gap.

    Pure GET, no side effects -- safe to call at any time, including during
    a live firing. Host is auto-resolved the same way
    safety_get_commissioning() is; pass `host` explicitly for kilnctl.local
    or a board reachable only from a different network than this link's
    serial port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as safety_get_commissioning()

    resolved = _ota_resolve_host(host)
    try:
        data = safety_cfg_http_client.get_commissioning(resolved)
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return json.dumps({"ok": False, "error": f"error reading safety commissioning over HTTP (host={resolved}): {exc}"})
    raw = safety_cfg_http_client.ct_cal_raw(data)
    return json.dumps({"ok": True, "host": resolved, **raw})


@_srv._tool()
def safety_capture_ct_counts(seconds: float = 60.0, out_dir: Optional[str] = None,
                              host: Optional[str] = None) -> str:
    """Capture raw CT ADC counts over GET /api/status for `seconds`, as fast
    as the safety link's own 500 ms POWER (Frame E) cadence allows, and
    return per-channel mean/std/min/max plus the achieved sample rate.

    Closes CURRENT_SENSE.md sec 4's "Measured noise floor -- BLOCKED, not
    measured": that section's step 0 (a 60 s, 20 Hz raw-counts capture) could
    not be run because no path from the board to raw ADC counts existed at
    all -- see the file's own "Tooling gap" option 3, which asked for exactly
    this tool. The wire only carries a NEW sample once per POWER frame
    (500 ms, LINK_PROTOCOL.md sec 2), so polling faster than that just
    re-reads the same cached value -- this tool polls at 0.2 s intervals
    (comfortably under 500 ms without hammering the HTTP server).

    Records EVERY poll at that fixed cadence, not only polls where
    `ct_counts` changed from the previous one (Opus review of
    51c084f/c49bb0e, finding 8: the original version discarded exactly the
    quiet, unchanged-count samples a noise-floor measurement needs -- if the
    ADC is genuinely quiet, "value didn't change" IS the noise-floor
    observation, not a duplicate to drop). The returned/logged sample rate
    is the ACHIEVED rate of GET /api/status calls that returned a valid
    `ct_counts` (by poll count, not by value-change count) over the actual
    wall-clock elapsed time -- this can run a little under the nominal
    1/poll_interval_s if HTTP round-trips are slow, so it is measured, not
    assumed. Counts are per-channel u16 (0..4095) straight from GET
    /api/status's `ct_counts` (dashboard_status_http.c, 2026-09-06) --
    null/absent when the Pico is running a pre-`KILNLINK_PROTOCOL_VERSION`-11
    build or has never sent a POWER frame; this tool fails (does not
    silently substitute 0) in that case.

    A CSV (`ts_ms,ch0,ch1,ch2`, one row per poll) is written under `out_dir`
    if given (caller supplies the directory; this tool does not invent one)
    -- the raw log CURRENT_SENSE.md sec 4 wants preserved for the
    noise-floor writeup, not just the summary statistics. Without
    `out_dir`, only the summary is returned.

    This is a different tool from noise_floor.py's PID run-to-run repeat
    spread measurement (CURRENT_SENSE.md sec 4 already flags this exact
    naming collision to avoid) -- do not confuse the two.
    """
    if seconds <= 0:
        return "error: seconds must be > 0"
    from .mcp_server_ota import _ota_resolve_host
    resolved_host = _ota_resolve_host(host)

    poll_interval_s = 0.2
    samples: "list[tuple[int, int, int, int]]" = []  # (ts_ms, ch0, ch1, ch2)
    start = time.monotonic()
    deadline = start + seconds
    saw_valid_frame = False
    while time.monotonic() < deadline:
        loop_start = time.monotonic()
        try:
            status = dashboard_http_client.get_status(resolved_host)
        except Exception as exc:
            return f"error: GET /api/status failed: {exc}"
        counts = status.get("ct_counts") if isinstance(status, dict) else None
        if isinstance(counts, list) and len(counts) == 3:
            saw_valid_frame = True
            c0, c1, c2 = int(counts[0]), int(counts[1]), int(counts[2])
            samples.append((int((time.monotonic() - start) * 1000), c0, c1, c2))
        elapsed = time.monotonic() - loop_start
        remaining = poll_interval_s - elapsed
        if remaining > 0:
            time.sleep(remaining)

    if not saw_valid_frame:
        return ("error: ct_counts never present in GET /api/status -- Pico is "
                "either pre-protocol-11 or has never sent a POWER frame")
    if not samples:
        return "error: no ct_counts samples observed in the capture window"

    per_channel = [[s[1] for s in samples], [s[2] for s in samples], [s[3] for s in samples]]
    stats = []
    for ch, values in enumerate(per_channel):
        n = len(values)
        mean = sum(values) / n
        variance = sum((v - mean) ** 2 for v in values) / n
        std = math.sqrt(variance)
        stats.append((ch, mean, std, min(values), max(values)))

    actual_elapsed_s = max(time.monotonic() - start, 1e-9)
    achieved_hz = len(samples) / actual_elapsed_s
    lines = [f"{len(samples)} samples over {actual_elapsed_s:.1f} s "
             f"({achieved_hz:.2f} Hz achieved -- every {poll_interval_s}s poll recorded, "
             f"not just value changes)"]
    for ch, mean, std, lo, hi in stats:
        lines.append(f"ch{ch}: mean={mean:.2f} std={std:.3f} min={lo} max={hi}")

    csv_path = None
    if out_dir:
        try:
            os.makedirs(out_dir, exist_ok=True)
            csv_path = os.path.join(out_dir, f"ct_counts_capture_{int(start)}.csv")
            with open(csv_path, "w", encoding="utf-8") as f:
                f.write("ts_ms,ch0,ch1,ch2\n")
                for ts_ms, c0, c1, c2 in samples:
                    f.write(f"{ts_ms},{c0},{c1},{c2}\n")
        except OSError as exc:
            lines.append(f"warning: CSV write failed: {exc}")
            csv_path = None

    if csv_path:
        lines.append(f"csv: {csv_path}")
    return " | ".join(lines)


def _describe_commissioning(data: dict) -> str:
    """Render GET /api/safety/commissioning's JSON as an answer to the
    question people actually ask -- "which guards are armed, which are
    dormant, and why" -- rather than a raw field dump.

    Armed/dormant semantics were verified directly against
    firmware/SaftyFW/src/safety_guards.c (READ ONLY -- not this change's
    file to edit), not inferred from this endpoint's own field names:

    * S1 (SAFETY_TRIP_OVERTEMP, independent overtemp ceiling): guarded by
      ``if (cfg->abs_max_temp_c > 0.0f)`` (safety_guards.c line ~607) --
      0 (or unset, which the firmware also stores as 0) means "not
      commissioned yet", and that comment is explicit: "never trip, and
      never...". Any positive value arms it.
    * S8 (SAFETY_TRIP_RATE, rate-of-rise ceiling): guarded by
      ``if (cfg->max_rate_c_per_min > 0.0f)`` (line ~733); the surrounding
      comment says the guard "ships disabled: max_rate_c_per_min == 0" for
      the identical reason as S1. Same rule, same sentinel.
    * S14 (over-current vs measured normal, WARN only -- not a trip code):
      active per-channel only when ``i_normal_valid[ch] && i_normal_a[ch] >
      0.0f`` AND current sensing is not disabled (line ~941); a channel
      whose normal current was never measured is skipped entirely, not
      treated as passing. This tool reports that per the endpoint's
      ``ct_installed``/``i_normal_a[n]`` params, which is what commissions
      i_normal_a in the first place.

    A field the endpoint reports as unset is rendered the same as the
    firmware treats it operationally (0 / not commissioned), because that
    IS the live behaviour -- but the "(unset)" note is kept so a caller
    does not mistake an uncommissioned board for one someone deliberately
    disabled a guard on.
    """
    params = {p["name"]: p for p in data.get("params", []) if "name" in p}
    reliable = bool(data.get("unset_reporting_reliable"))

    def numeric(name: str) -> "tuple[float, bool]":
        """(value, is_set) -- unset (or unreliable-unset-reporting) reads as
        0.0, matching what the firmware itself falls back to."""
        p = params.get(name)
        if p is None:
            return 0.0, False
        is_set = bool(p.get("set")) and reliable
        if not is_set:
            return 0.0, False
        return float(p.get("value", 0.0)), True

    lines = []
    lines.append(f"link_up={data.get('link_up')}  commissioned={data.get('commissioned')}")
    if data.get("stale"):
        lines.append(f"WARNING: config CRC is STALE -- cached={data.get('cached_config_crc')} "
                     f"live={data.get('live_config_crc')} (this board's cache does not match "
                     "what the Pico is actually running; treat everything below as suspect "
                     "until a refetch)")
    else:
        lines.append(f"config CRC {data.get('cached_config_crc')} (matches live, not stale)")
    if not reliable:
        lines.append("WARNING: unset_reporting_reliable=false -- the Pico's protocol version "
                     "cannot distinguish 'never commissioned' from a genuine 0, so every "
                     "threshold below is shown as read even though it may just be unset")

    abs_max, abs_set = numeric("abs_max_temp_c")
    if abs_max > 0.0:
        lines.append(f"S1 abs_max_temp_c={abs_max:g}C  ARMED")
    else:
        lines.append(f"S1 abs_max_temp_c={abs_max:g}C  DORMANT "
                     f"({'0 = not commissioned, never trips' if not abs_set else '0 = never trips'})")

    rate, rate_set = numeric("max_rate_c_per_min")
    if rate > 0.0:
        lines.append(f"S8 max_rate_c_per_min={rate:g}C/min  ARMED")
    else:
        lines.append(f"S8 max_rate_c_per_min={rate:g}C/min  DORMANT "
                     f"(0 = never trips{'' if rate_set else ', not commissioned'}; ships disabled)")

    tc_source_p = params.get("tc_source")
    if tc_source_p is not None and tc_source_p.get("set") and reliable:
        lines.append(f"tc_source={tc_source_p.get('value')}")

    ct_installed_p = params.get("ct_installed")
    ct_installed_known = bool(ct_installed_p and ct_installed_p.get("set") and reliable)
    ct_installed = bool(ct_installed_p.get("value")) if ct_installed_known else None
    if ct_installed_known and not ct_installed:
        lines.append("S14 over-current (per channel): DORMANT (ct_installed=0 -- no CTs fitted)")
    else:
        chan_bits = []
        for ch in range(3):
            i_norm, i_set = numeric(f"i_normal_a[{ch}]")
            if i_set and i_norm > 0.0:
                chan_bits.append(f"ch{ch} i_normal_a={i_norm:g}A ARMED")
            else:
                chan_bits.append(f"ch{ch} DORMANT (i_normal_a not measured)")
        prefix = "S14 over-current (WARN only, per channel)" if ct_installed_known else \
            "S14 over-current (WARN only, per channel; ct_installed unknown)"
        lines.append(prefix + ": " + "; ".join(chan_bits))

    ct_topology_p = params.get("ct_topology")
    ct_topology_known = bool(ct_topology_p and ct_topology_p.get("set") and reliable)
    ct_topology_summed = bool(ct_topology_p.get("value")) if ct_topology_known else False
    if not ct_topology_known:
        lines.append("S15 under-current (WARN only, per zone): DORMANT (ct_topology unknown -- not commissioned)")
    elif not ct_topology_summed:
        lines.append("S15 under-current (WARN only, per zone): DORMANT (ct_topology=per_zone)")
    elif ct_installed_known and not ct_installed:
        lines.append("S15 under-current (per zone): DORMANT (ct_installed=0 -- no CTs fitted)")
    else:
        zone_bits = []
        for z in range(3):
            i_norm, i_set = numeric(f"i_normal_a[{z}]")
            if i_set and i_norm > 0.0:
                zone_bits.append(f"z{z} i_normal_a={i_norm:g}A ARMED")
            else:
                zone_bits.append(f"z{z} DORMANT (i_normal_a not measured)")
        lines.append("S15 under-current (WARN only, per zone, ct_topology=summed): " + "; ".join(zone_bits))

    if data.get("tc_not_installed"):
        lines.append("live flag: tc_not_installed (safety thermocouple reports not installed)")
    if data.get("tc_injected"):
        lines.append("live flag: tc_injected (safety thermocouple reading is injected/simulated)")
    if data.get("borrowed_known"):
        lines.append(f"borrowed={data.get('borrowed')}"
                     + (f" zone_index={data.get('borrowed_zone_index')}"
                        if data.get("borrowed") and "borrowed_zone_index" in data else ""))

    # 2026-09-09 (docs/audits/commissioning_gap_and_no_heat_2026-09-09.md
    # sec 4): these params were always present in the underlying GET
    # /api/safety/commissioning response but never rendered, which left
    # estop_active_level in particular unreadable by any tool during a
    # live investigation. Same numeric()/unset-reliability handling as
    # every other field above -- reported as "(unset)" rather than a bare
    # 0 when the board cannot distinguish "never commissioned" from a
    # genuine 0.
    def render_u8(name: str, label: str, decode=None) -> "str | None":
        p = params.get(name)
        if p is None:
            return None
        is_set = bool(p.get("set")) and reliable
        raw = p.get("value")
        if not is_set:
            return f"{label}={raw} (unset)"
        decoded = f" ({decode(raw)})" if decode else ""
        return f"{label}={raw}{decoded}"

    estop_line = render_u8(
        "estop_active_level", "estop_active_level",
        lambda v: "ACTIVE_LOW" if v else "ACTIVE_HIGH (default -- asserted when GPIO9 reads high; "
                                          "the only polarity under which a cut line is detectable)")
    if estop_line:
        lines.append(estop_line)

    borrowed_zone_line = render_u8("borrowed_zone_index", "borrowed_zone_index")
    if borrowed_zone_line and not data.get("borrowed_known"):
        # Printed above already when borrowed_known; this covers the case
        # (tc_source=OWN_J7) where the endpoint's own borrowed/borrowed_known
        # summary omits it even though the raw param is still set.
        lines.append(borrowed_zone_line)

    tc_placement_line = render_u8(
        "tc_placement_mode", "tc_placement_mode",
        lambda v: {0: "CHAMBER_AGREED", 1: "CHAMBER_INDEPENDENT"}.get(int(v), f"unknown({v})"))
    if tc_placement_line:
        lines.append(tc_placement_line)

    mains_p = params.get("mains_voltage_v")
    if mains_p is not None:
        mains_val, mains_set = numeric("mains_voltage_v")
        lines.append(f"mains_voltage_v={mains_val:g}" + ("" if mains_set else " (unset)"))

    tc_type_line = render_u8("tc_type", "tc_type")
    if tc_type_line:
        lines.append(tc_type_line)

    return "\n".join(lines)


@_srv._tool()
def safety_get_commissioning(host: Optional[str] = None) -> str:
    """READ-ONLY: fetch and render the safety processor's commissioned guard
    thresholds from GET /api/safety/commissioning (safety_cfg_http.c's
    commissioning_get_handler on the ESP, which itself is a cached view of
    the RP2040's config record -- not a live round trip to the Pico).

    Answers the question people actually ask ("which guards are ARMED right
    now, which are DORMANT, and why") rather than dumping raw JSON: reports
    S1 (abs_max_temp_c, the independent overtemp ceiling), S8
    (max_rate_c_per_min), S14 (per-channel over-current vs measured
    normal, WARN-only), and S15 (per-zone under-current vs measured normal
    in summed-CT topology, WARN-only) with their armed/dormant state, plus the
    `commissioned` flag, config CRC staleness, tc_source and the live
    tc_not_installed/tc_injected/borrowed flags. Armed/dormant thresholds
    (0 = never trips) were verified against firmware/SaftyFW/src/
    safety_guards.c, not guessed from field names -- see
    safety_get_commissioning's own docstring detail in
    mcp_server_safety.py's ``_describe_commissioning()`` for the exact
    lines.

    Pure GET, no side effects whatsoever -- safe to call at any time,
    including during a live firing. This is the counterpart
    safety_set_tc_type()/safety_set_ct_cal() have needed: those write
    commissioning config, this only ever reads it. It never writes,
    commits, or touches the Pico's config in any way.

    Host is auto-resolved the same way the OTA/control tools do (board's
    current Wi-Fi station IP over the UART link, falling back to the
    fallback-AP address); pass `host` explicitly for kilnctl.local or a
    board reachable only from a different network than this link's serial
    port.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as load_config_preset()

    resolved = _ota_resolve_host(host)
    try:
        data = safety_cfg_http_client.get_commissioning(resolved)
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return f"error reading safety commissioning over HTTP (host={resolved}): {exc}"
    return _describe_commissioning(data)


@_srv._tool()
def safety_set_commissioning_fields(fields: "dict[str, Any]", host: Optional[str] = None) -> str:
    """Commission ARBITRARY named fields on the safety processor's config
    record over GET/POST /api/safety/commissioning -- the MCP-facade
    replacement for the paste-ready ``safety_cfg_http_client.apply_safety_
    fields(host, {...}, verify=True)`` snippet CT_COMMISSIONING_PLAN.md step
    6a documented (``ffea6114``) because no dedicated tool existed. Per
    CLAUDE.md, anything that touches the boards belongs behind the MCP
    facade, not a snippet the operator pastes by hand.

    THIS WRITES THE SAFETY PROCESSOR'S (RP2040/SaftyFW) FLASH -- every
    accepted field is staged via SET_PARAM and landed with COMMIT_CONFIG,
    the same generic path safety_set_rate_guard()/safety_set_tc_type() use
    (safety_cfg_store.c / config_store.c). `fields` is a name -> value
    mapping using the board's OWN field names, exactly as
    GET /api/safety/commissioning reports them (``ct_installed``,
    ``ct_topology``, ``abs_max_temp_c``, ``max_rate_c_per_min``, ...) --
    call safety_get_commissioning() first to see the live names/types. This
    is INCREMENTAL, not a whole-page submit: a field not named in `fields`
    is left exactly as it was, set or unset (safety_cfg_http_client.py's own
    module docstring explains why echoing the whole page back would be
    wrong here).

    REFUSES if a profile is firing or an autotune run is in progress,
    checked via profiles_get_exec_status()/autotune_get_status() BEFORE
    anything is sent over the wire -- same check safety_set_rate_guard()
    uses, and for the same reason: the Pico's own relay_owner refuses
    config writes while ARMED anyway, but a live run is also not a safe
    time to change safety-guard commissioning out from under it.

    REQUIRES the Pico to be in its post-reset GRACE window (config writes
    are refused while ARMED -- relay_owner.h's INIT -> GRACE -> ARMED state
    machine, SAFTYFW_STARTUP_GRACE_MS, 60s default). This tool does **not**
    reset the Pico itself: a refusal for that reason names it explicitly
    and says to call ``debug_reset(peer="pico")`` then retry within 60
    seconds, rather than resetting behind the caller's back.

    FAILS LOUDLY, NAMING THE FIELD, if an independent read-back after the
    write disagrees with what was sent -- an ``{"ok":true}`` POST reply is
    never trusted alone (safety_cfg_http_client.apply_safety_fields()'s own
    verify=True path: a fresh GET, compared field by field). A field that
    reads back UNSET after the commit is reported as a mismatch, not a
    pass. An unrecognised field name is refused before anything is sent
    (SafetyCfgUnknownParamError, wrapped into the same 'error:' string
    here) rather than silently dropped.

    Host is auto-resolved the same way safety_get_commissioning()/
    safety_set_rate_guard() do; pass `host` explicitly for kilnctl.local or
    a board reachable only from a different network than this link's
    serial port.
    """
    if not fields:
        return "error: fields is empty -- nothing to write"

    busy = _profile_or_autotune_running()
    if busy is not None:
        return f"refused: {busy} -- writing commissioning fields mid-run is not safe; stop it first"

    from .mcp_server_ota import _ota_resolve_host

    resolved = _ota_resolve_host(host)
    try:
        result = safety_cfg_http_client.apply_safety_fields(resolved, dict(fields), verify=True)
    except safety_cfg_http_client.SafetyCfgUnknownParamError as exc:
        return f"error: {exc}"
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return f"error writing safety commissioning fields over HTTP (host={resolved}): {exc}"

    if not result.ok:
        reason = result.post_reason or "; ".join(result.mismatches) or "unknown failure"
        if "ARMED" in reason:
            return (
                "refused: the safety processor rejected the write because the relay is ARMED "
                "-- config writes only land during the 60s post-reset GRACE window. Call "
                'debug_reset(peer="pico") and then call safety_set_commissioning_fields(...) '
                f"again within 60 seconds. (board said: {reason})"
            )
        return f"failed: {reason}"

    lines = [f"ok - safety commissioning fields written and confirmed by read-back: "
             + ", ".join(f"{k}={v!r}" for k, v in fields.items())]
    lines.append(f"board reports commissioned: {result.commissioned_after}")
    if result.still_unset:
        lines.append("still UNSET (required for commissioning): " + ", ".join(result.still_unset))
    return " | ".join(lines)


#: safety_guards.c line ~733: ``if (cfg->max_rate_c_per_min > 0.0f)`` -- any
#: value at or below 0 leaves S8 dormant, matching _describe_commissioning()'s
#: own ARMED/DORMANT split above. Not a firmware constant, just the same
#: threshold spelled out here for the post-write report.
_RATE_GUARD_ARM_THRESHOLD_C_PER_MIN = 0.0


def _profile_or_autotune_running() -> Optional[str]:
    """None if neither a firing nor an autotune run is in progress, else a
    short reason string naming which one. Config writes to the safety
    processor are refused by the Pico itself while ARMED (relay energized is
    exactly what a running firing/autotune implies), but that refusal is
    reported to this PC tool only as a generic "relay is ARMED" string with
    no way to tell "someone forgot the 60s grace window" apart from "a
    firing is genuinely in progress" -- so this checks the ESP's own live
    exec status FIRST and gives the more useful answer before ever touching
    the wire."""
    try:
        prof = _srv._profiles.get_exec_status()
        if prof.state in (1, 2):  # running, paused (still armed/heating-capable)
            return f"a profile is currently {prof.state_name} (#{prof.profile_id} {prof.name!r})"
    except ProfilesQueryError:
        pass  # link down / no reply -- not this check's job to report that
    try:
        at = _srv._autotune.get_status()
        if at.state in (1, 2, 3, 4):  # settling, stepping, relay_approach, relay_cycling
            return f"an autotune run is currently {at.state_name} (zone {at.zone})"
    except AutotuneQueryError:
        pass
    return None


@_srv._tool()
def safety_get_rate_guard(host: Optional[str] = None) -> str:
    """READ-ONLY: report S8's stored guard/window (config_store ids 0x0204
    ``max_rate_c_per_min`` / 0x0205 ``rate_window_s``) and whether the guard
    is currently ARMED or DORMANT.

    Pure GET over GET /api/safety/commissioning (safety_cfg_http_client.
    get_commissioning) -- no side effects, safe to call at any time including
    during a live firing. ARMED/DORMANT follows safety_guards.c's own rule
    (``max_rate_c_per_min > 0.0f``), the same rule safety_get_commissioning()
    uses for its S8 line; this tool exists for the case where only S8 is
    wanted, plus the window value alongside it.

    Host is auto-resolved the same way safety_get_commissioning() does.
    """
    from .mcp_server_ota import _ota_resolve_host

    resolved = _ota_resolve_host(host)
    try:
        data = safety_cfg_http_client.get_commissioning(resolved)
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return f"error reading safety commissioning over HTTP (host={resolved}): {exc}"

    params = safety_cfg_http_client.params_by_name(data)
    reliable = bool(data.get("unset_reporting_reliable"))

    def field(name: str) -> "tuple[Any, bool]":
        p = params.get(name)
        if p is None or not reliable or not p.get("set"):
            return None, False
        return p.get("value"), True

    rate, rate_set = field("max_rate_c_per_min")
    window, window_set = field("rate_window_s")
    rate_num = float(rate) if rate_set else 0.0
    armed = rate_num > _RATE_GUARD_ARM_THRESHOLD_C_PER_MIN
    lines = [
        f"S8 max_rate_c_per_min={rate_num:g}C/min "
        f"({'ARMED' if armed else 'DORMANT'}{'' if rate_set else ', not commissioned'})",
        f"rate_window_s={window if window_set else 60}"
        f"{'' if window_set else ' (unset -- firmware default 60s)'}",
    ]
    if not reliable:
        lines.append("WARNING: unset_reporting_reliable=false -- this Pico's protocol version "
                     "cannot distinguish 'never commissioned' from a genuine 0")
    if data.get("stale"):
        lines.append(f"WARNING: config CRC is STALE -- cached={data.get('cached_config_crc')} "
                     f"live={data.get('live_config_crc')}")
    return " | ".join(lines)


@_srv._tool()
def safety_set_rate_guard(max_rate_c_per_min: float, rate_window_s: float = 60.0,
                           confirm: bool = False, host: Optional[str] = None) -> str:
    """Commission S8, the safety processor's temperature rate-of-rise guard
    (config_store ids 0x0204 ``max_rate_c_per_min`` / 0x0205
    ``rate_window_s``, safety_guards.c's SAFETY_TRIP_RATE block). Ships
    dormant (``max_rate_c_per_min == 0``); this is the write path ROADMAP.md's
    "S8 sanity rate" row says did not exist yet.

    REFUSES UNLESS ``confirm=True`` -- this arms a guard that can trip a live
    firing, and there is no undo cheaper than a second call.

    REFUSES if a profile is firing or an autotune run is in progress (checked
    via profiles_get_exec_status()/autotune_get_status() BEFORE anything is
    sent over the wire): writing this while ARMED is refused by the Pico
    itself anyway (relay_owner: config writes are rejected while ARMED), but
    a live run is also not a safe time to arm a NEW guard threshold the
    control loop has never run under.

    REQUIRES the Pico to be in its post-reset GRACE window (config writes are
    refused while ARMED -- see relay_owner.h's INIT -> GRACE -> ARMED state
    machine and SAFTYFW_STARTUP_GRACE_MS, 60s default). This tool does
    **not** reset the Pico itself: if the write is refused for that reason,
    the error names it explicitly and says to call ``debug_reset(peer="pico")``
    then retry this call within 60 seconds, rather than resetting on its own
    behind the caller's back.

    Validation mirrors the firmware's OWN bounds, not an invented tighter
    range: config_params.c's SET_PARAM handler checks only that
    ``max_rate_c_per_min`` is a finite f32 (``CHECK_F32_FINITE()``, no
    non-negative check -- a value at or below 0 simply leaves the guard
    DORMANT, same as unset) and that ``rate_window_s`` fits a u16 (0-65535).
    Those are the only bounds enforced here (via safety_cfg_http_client's
    ``format_value()``); COMMISSIONING.md sec 2's cross-field checks, if any
    ever apply to these two ids, are still the Pico's COMMIT_CONFIG's job to
    enforce, not this tool's to duplicate.

    On success, writes both params, sends COMMIT_CONFIG, and independently
    re-reads the config (safety_cfg_http_client.apply_safety_fields's own
    read-back verify -- an ACK is not proof, see that module's docstring) --
    an ``{"ok":true}`` alone is never enough to report success here.
    """
    if not confirm:
        return ("refused: pass confirm=True to actually write S8's rate-of-rise guard -- "
                f"this would set max_rate_c_per_min={max_rate_c_per_min:g}C/min, "
                f"rate_window_s={rate_window_s:g}s")

    busy = _profile_or_autotune_running()
    if busy is not None:
        return f"refused: {busy} -- writing a new S8 threshold mid-run is not safe; stop it first"

    from .mcp_server_ota import _ota_resolve_host

    resolved = _ota_resolve_host(host)
    fields = {"max_rate_c_per_min": float(max_rate_c_per_min), "rate_window_s": rate_window_s}
    try:
        result = safety_cfg_http_client.apply_safety_fields(resolved, fields, verify=True)
    except safety_cfg_http_client.SafetyCfgHttpError as exc:
        return f"error writing S8 rate guard over HTTP (host={resolved}): {exc}"

    if not result.ok:
        reason = result.post_reason or "; ".join(result.mismatches) or "unknown failure"
        if "ARMED" in reason:
            return (
                "refused: the safety processor rejected the write because the relay is ARMED "
                "-- config writes only land during the 60s post-reset GRACE window. Call "
                'debug_reset(peer="pico") and then call safety_set_rate_guard(...) again within '
                f"60 seconds. (board said: {reason})"
            )
        return f"failed: {reason}"

    rate_after = fields["max_rate_c_per_min"]
    armed = rate_after > _RATE_GUARD_ARM_THRESHOLD_C_PER_MIN
    return (
        f"ok - S8 rate guard written and confirmed by read-back: "
        f"max_rate_c_per_min={rate_after:g}C/min rate_window_s={rate_window_s:g}s "
        f"({'ARMED' if armed else 'DORMANT'})"
    )


@_srv._tool()
def ota_rollback_pico() -> str:
    """Explicitly revert the safety processor (RP2040/SaftyFW) to its
    PREVIOUS bootloader slot, right now.

    This is the Pico half of tools/PcTools/TODO.md's `ota_rollback(processor)`
    line -- the ESP half is ota_rollback_esp() above. Unlike the ESP, the Pico
    has no ESP-IDF-style rollback API: SaftyFW's own bootloader
    (firmware/SaftyFW/bootloader/) picks the active slot at every boot from
    versioned/CRC'd flash metadata, and this call is the explicit,
    operator/agent-triggered path to marking the CURRENTLY ACTIVE slot BAD
    and switching to the other one -- even though the currently running
    image is healthy. Ordinary recovery from a bad update needs no call here
    at all: an unconfirmed PENDING_VERIFY image is already reverted
    automatically by the bootloader's own boot_attempts fallback. Use this
    when the running image is valid but behaves worse in practice than the
    one it replaced.

    This travels over the already-authenticated safety UART bridge (PC -> ESP
    -> Pico, SAFETY_CMD_ROLLBACK = 0x17), the same authentication boundary as
    safety_ping()/safety_clear_trip() -- NOT the HTTP OTA challenge/password
    path ota_rollback_esp() uses, since there is no separate HTTP surface on
    the Pico side at all for a bootloader-slot rollback; every other Pico
    command goes through the ESP's UART bridge too, though
    safety_set_tc_type() specifically now writes over
    GET/POST /api/safety/commissioning instead (2026-09-17, verified
    read-back rather than fire-and-forget -- see that tool's own docstring).

    Refused entirely on SaftyFW's own say-so (fire-and-forget: this call
    cannot see the refusal, only that nothing changes):
      - the relay is currently ARMED (same gate config writes and updates
        use -- a rollback reboots into different code, exactly as disruptive
        as a push);
      - the OTHER bootloader slot is not currently VALID or PENDING_VERIFY.
        THIS IS THE PROPERTY THAT MATTERS MOST: unlike the ESP (which has
        esp_ota_check_rollback_is_possible()), SaftyFW's bootloader has only
        two slots total, so a rollback that proceeded while the other slot
        were EMPTY/STAGED/BAD would strand the board with zero bootable
        slots on the very next boot. bootloader/metadata.c's
        bootloader_decide_rollback() checks this BEFORE marking the current
        slot BAD, not after.

    Call ota_status(processor="pico") (once it exists -- still open per
    tools/PcTools/TODO.md) or safety_get_status()/safety_get_link_stats()
    afterward to confirm the outcome: a successful rollback shows up as the
    safety link dropping and recovering with a new boot_id; a refused one
    leaves the link and boot_id exactly as they were, with the reason only
    visible in the Pico's own _srv.log (log_task) if a debug probe is attached --
    nothing is returned to this call either way.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- no RP2040 attached in this
    environment; only the wire codec and host-tested decision logic are
    exercised here.
    """
    return _srv._send(UART_TASK_ID_SAFETY, devices.safety_request_rollback())


