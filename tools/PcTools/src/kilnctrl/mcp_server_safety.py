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

from . import actions, config_presets, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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
    """
    try:
        status = _srv._safety.get_status()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    return status.describe()


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
def safety_set_tc_type(tc_type_name: str) -> str:
    """Commission the safety processor's thermocouple type (config_store.h).

    `tc_type_name` is one of "B", "E", "J", "K", "N", "R", "S", "T"
    (case-insensitive), mirroring firmware/SaftyFW/src/max31856.h's
    MAX31856_TC_TYPE_* ordering. Fire-and-forget, like clear_trip -- there is
    no reply on the wire; the outcome (accepted, or refused because the relay
    is ARMED / the value wasn't recognised) shows up on the Pico's _srv.log, and
    eventually in config_version advancing on the next GET_DIAG/GET_STATUS
    poll, not here.

    This tool only sends the wire command -- it does not build any GUI/LCD
    commissioning flow (out of scope, same as clear_trip's own LCD surface
    was deferred for the same budget/scope reason).
    """
    name = tc_type_name.strip().upper()
    if name not in devices.SAFETY_TC_TYPE_NAMES:
        known = ", ".join(sorted(devices.SAFETY_TC_TYPE_NAMES))
        return f"error: unknown tc_type_name {tc_type_name!r} -- expected one of {known}"
    try:
        result = _srv._safety.set_config(devices.SAFETY_TC_TYPE_NAMES[name])
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - requested tc_type {name}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set tc_type{detail}"


@_srv._tool()
def safety_set_ct_cal(channel: int, calibrated: bool, gain: float, offset: float) -> str:
    """Commission one channel of the safety processor's CT current-sense
    calibration (config_store.h's ct_cal record).

    `channel` is 0-2 (one of the three current-sense channels), `gain`/
    `offset` are the linear-fit constants a bench calibration run produces.
    Only one channel is written per call -- writing channel 0 never touches
    channel 1/2's stored constants.

    Fire-and-forget, like safety_clear_trip/safety_set_tc_type: there is no
    reply on the wire. Refused on the Pico side (relay currently ARMED, or an
    out-of-range channel) shows up only in the Pico's own _srv.log, not here --
    call safety_get_ct_cal() afterward to see whether it actually took.
    """
    try:
        result = _srv._safety.set_ct_cal(channel, calibrated, gain, offset)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - requested CT ch{channel} calibration"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set CT ch{channel} calibration{detail}"


@_srv._tool()
def safety_get_ct_cal() -> str:
    """Read the safety processor's three CT channels' stored calibration.

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
    safety_set_tc_type()/safety_ping() -- NOT the HTTP OTA challenge/password
    path ota_rollback_esp() uses, since there is no separate HTTP surface on
    the Pico side at all; every Pico command already goes through the ESP's
    UART bridge.

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


