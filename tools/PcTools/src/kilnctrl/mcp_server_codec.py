"""CODEC -- frame encode/decode helpers.

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
from .devices_common import redact_secret_fields
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

from . import mcp_server_core as _core


# ---------------------------------------------------------------------------
# CODEC -- encode/decode a uart_protocol frame from/to JSON, no board attached
#
# For reasoning about a byte sequence pulled from a Saleae capture or a _srv.log,
# without guessing -- tools/PcTools/TODO.md capability 6. Pure functions:
# neither of these touches the link or requires a connection.
# ---------------------------------------------------------------------------
_CODEC_MSG_TYPES = {"data": MsgType.DATA, "ack": MsgType.ACK, "nack": MsgType.NACK,
                    "broadcast": MsgType.BROADCAST}
_CODEC_DEVICES = {"esp": Device.ESP, "host": Device.HOST}


def _codec_parse_device(name: str) -> "Optional[int]":
    key = name.strip().lower()
    if key in _CODEC_DEVICES:
        return int(_CODEC_DEVICES[key])
    try:
        return int(name, 0)  # accept a raw numeric device id for an unlisted/future peer
    except ValueError:
        return None


@_core._tool()
def codec_encode_frame(
    msg_type: str, msg_index: int, src_device: str, src_task: int,
    dst_device: str, dst_task: int, payload_hex: str = "",
) -> str:
    """Build one on-wire ``uart_protocol`` frame (byte-stuffed, delimited,
    CRC16 appended) and return it as hex.

    ``msg_type`` is one of "data", "ack", "nack", "broadcast" (the last is not
    sent on the PC<->ESP link today, only decodable -- see protocol.py's
    MsgType.BROADCAST). ``src_device``/``dst_device`` are "esp" or "host", or
    a raw integer for an unlisted peer id. ``payload_hex`` is the raw payload
    bytes as hex, empty for none.
    """
    msg_type_val = _CODEC_MSG_TYPES.get(msg_type.strip().lower())
    if msg_type_val is None:
        return f"error: msg_type must be one of {sorted(_CODEC_MSG_TYPES)}, got {msg_type!r}"
    src_dev = _codec_parse_device(src_device)
    dst_dev = _codec_parse_device(dst_device)
    if src_dev is None:
        return f"error: unrecognized src_device {src_device!r}"
    if dst_dev is None:
        return f"error: unrecognized dst_device {dst_device!r}"
    try:
        payload = bytes.fromhex(payload_hex)
    except ValueError as exc:
        return f"error: payload_hex is not valid hex: {exc}"
    try:
        frame = Frame(
            msg_type=msg_type_val, msg_index=msg_index,
            src_device=src_dev, src_task=src_task,
            dst_device=dst_dev, dst_task=dst_task,
            payload=payload,
        )
    except ValueError as exc:
        return f"error: {exc}"
    return frame.to_wire().hex()


@_core._tool()
def codec_decode_frame(wire_hex: str) -> str:
    """Parse one on-wire ``uart_protocol`` frame (hex, delimiters optional --
    unstuffing accepts the body with or without its surrounding 0x7E bytes)
    and report every field, including the payload as hex.

    Validates length and CRC exactly like the firmware's ``handle_raw_frame``;
    a malformed or corrupt frame comes back as an ``error:`` string rather
    than a traceback, matching how the live RX path silently drops one.
    """
    try:
        raw_wire = bytes.fromhex(wire_hex)
    except ValueError as exc:
        return f"error: wire_hex is not valid hex: {exc}"
    try:
        raw = unstuff(raw_wire)
    except (IndexError, ValueError) as exc:
        return f"error: could not unstuff: {exc}"
    try:
        frame = Frame.from_raw(raw)
    except FrameError as exc:
        return f"error: {exc}"
    def dev_name(d) -> str:
        return d.name if isinstance(d, Device) else str(d)
    return (
        f"msg_type={frame.msg_type.name} msg_index={frame.msg_index} "
        f"src=({dev_name(frame.src_device)}/{frame.src_task}) "
        f"dst=({dev_name(frame.dst_device)}/{frame.dst_task}) "
        f"payload[{len(frame.payload)}]={frame.payload.hex()}"
    )


def _json_default(value):
    """json.dumps(default=...) for the dataclass fields get_board_state
    collects: bytes -> hex, anything else json doesn't already know -> str().
    IntEnum members serialize as plain ints without help from this."""
    if isinstance(value, (bytes, bytearray)):
        return value.hex()
    return str(value)


def _sanitize_nan(value):
    """Recursively replace NaN/Infinity floats with None (JSON null).

    Firmware readings carry NaN as "no valid reading" (see ThermoReading and
    SafetyStatus in devices.py -- an SPI-failed thermocouple channel or a
    safety processor that has never reported both arrive this way).
    ``json.dumps`` accepts NaN/Infinity by default and emits the invalid
    (non-standard) ``NaN``/``Infinity`` tokens rather than raising, which a
    strict JSON client can't parse. This repo's own convention (dashboard
    fields throughout the C firmware) is "null until valid", so board-state
    JSON follows the same rule instead of leaking a NaN literal.
    """
    if isinstance(value, float):
        return None if (math.isnan(value) or math.isinf(value)) else value
    if isinstance(value, dict):
        return {k: _sanitize_nan(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_sanitize_nan(v) for v in value]
    return value


def _snapshot_section(fn):
    """Runs one board_state section, returning its data or an {"error": ...}
    entry -- one subsystem timing out (e.g. no safety processor attached)
    must not blank out every other section's data."""
    try:
        result = fn()
    except Exception as exc:  # noqa: BLE001 - deliberately broad, see docstring
        return {"error": str(exc)}
    if dataclasses.is_dataclass(result):
        return dataclasses.asdict(result)
    if isinstance(result, list):
        return [dataclasses.asdict(x) if dataclasses.is_dataclass(x) else x for x in result]
    return result


def _control_zones_dict() -> dict:
    thermo_count, relay_count, zones = _srv._control.get_zones()
    return {
        "thermo_count": thermo_count,
        "relay_count": relay_count,
        "zones": [dataclasses.asdict(z) for z in zones],
    }


@_core._tool()
def get_board_state() -> str:
    """One-call snapshot: firmware version, pin config, every thermocouple
    reading, IO/relay state, safety status + link stats, Wi-Fi status,
    zone config, profile execution status and autotune status -- as JSON.

    Most diagnosis starts by asking for all of it; one call here replaces
    calling get_fw_version/get_pin_config/thermo_read/io_read/
    safety_get_status/... separately. Each section fails independently --
    e.g. a board with no safety processor fitted still returns a full
    snapshot, with "safety" reporting its own link_up=false rather than the
    whole call erroring out.
    """
    state = {
        "fw_version": _snapshot_section(_srv._info.get_fw_version),
        "pin_config": _snapshot_section(_srv._info.get_pin_config),
        "thermo": _snapshot_section(_srv._thermo.read),
        "io": _snapshot_section(_srv._io.read),
        "safety_status": _snapshot_section(_srv._safety.get_status),
        "safety_link_stats": _snapshot_section(_srv._safety.get_link_stats),
        "safety_fw_version": _snapshot_section(_srv._safety.get_fw_version),
        "wifi_status": _snapshot_section(_srv._wifi.get_status),
        "control_zones": _snapshot_section(_control_zones_dict),
        "profiles_exec_status": _snapshot_section(_srv._profiles.get_exec_status),
        "autotune_status": _snapshot_section(_srv._autotune.get_status),
    }
    # 2026-09-21 fix: "wifi_status" carries UartWifiStatus.ap_password
    # (devices_wifi_uart.py) straight through dataclasses.asdict() -- the
    # board's own AP Wi-Fi password in plaintext. Redact every
    # password/psk/passphrase-shaped field to a "[set]"/"[unset]" marker
    # before this ever leaves the process, not just before logging it (see
    # redact_secret_fields()'s doc comment in devices_common.py).
    state = redact_secret_fields(state)
    return json.dumps(_sanitize_nan(state), default=_json_default, indent=2)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
