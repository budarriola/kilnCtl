"""link management.

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

from . import mcp_server_core as _core


# ---------------------------------------------------------------------------
# link management
# ---------------------------------------------------------------------------
@_core._tool()
def list_serial_ports() -> str:
    """List serial ports with a suitability score for the board's UART bridge.

    The ESP32-S3 exposes this UART on a separate USB-C port from its native
    USB-Serial-JTAG debug port; JTAG-looking ports are scored negatively.
    """
    infos = list_ports()
    if not infos:
        return "no serial ports found"
    lines = []
    for info in infos:
        tag = " (recommended)" if info.recommended else ""
        lines.append(
            f"{info.device}: {info.description or '<no description>'} "
            f"[manufacturer={info.manufacturer or '?'} hwid={info.hwid or '?'} "
            f"score={info.score}]{tag}"
        )
    return "\n".join(lines)


@_core._tool()
def connect(port: Optional[str] = None) -> str:
    """Open the serial link. Omit `port` to autodiscover the recommended one."""
    if _srv._link.is_connected:
        return f"already connected to {_srv._link.port}"
    try:
        opened = _srv._link.connect(port)
    except Exception as exc:
        _srv._session_log.error("connect to %s failed: %s", port or "<auto>", exc)
        return f"error: {exc}"
    suffix = "" if port else " (autodiscovered)"
    # New _srv.log session on every successful connect, same trigger as the GUI;
    # the other trigger (device reboot) is handled in _srv._on_boot_push above.
    _srv._session_log.start_session(f"connected to {opened} @ {_srv._link.baudrate} baud")
    settings.set_last_port(opened)
    return (
        f"connected to {opened} @ {_srv._link.baudrate} baud{suffix}\n"
        + _check_peer_version()
    )


def _check_peer_version() -> str:
    """Ask the newly-connected device its protocol version, and say so plainly.

    A version mismatch has to be refused *at the point of connection*, not
    discovered later by whichever tool happens to be called first: a v1 peer
    is the unit-test fixture, where task 1 is an MCP4728 DAC rather than three
    thermocouples, so every task id means something different. The gate in
    _srv._send() already blocks the traffic; this makes the reason visible in the
    connect result instead of leaving the caller to infer it from a refusal.

    Never raises: a device that doesn't answer at all is reported as unknown,
    which is treated exactly like a mismatch by _srv._send().
    """
    try:
        version = _srv._info.get_fw_version()
    except (InfoQueryError, ValueError, OSError) as exc:
        return (
            f"WARNING: could not read the device's protocol version ({exc}); "
            "device commands will be refused until get_fw_version succeeds"
        )
    if not version.compatible:
        return (
            f"REFUSING DEVICE COMMANDS: this device speaks UART protocol "
            f"v{version.protocol_version}, pc_tools speaks "
            f"v{devices.UART_PROTOCOL_VERSION}. Every task id can mean "
            "something different across a version bump (v1 is the unit-test "
            "fixture, where task 1 is a DAC, not three thermocouples), so "
            "only the INFO queries are allowed. Flash matching firmware or "
            "use a matching pc_tools."
        )
    return f"protocol v{version.protocol_version} matches - {version.describe()}"


@_core._tool()
def disconnect() -> str:
    """Close the serial link."""
    was_connected = _srv._link.is_connected
    port = _srv._link.port
    # Always send the op, even when the link reads closed: it latches the
    # hub's explicit-disconnect flag so no lazy reconnect reopens the port.
    _srv._link.disconnect()
    if not was_connected:
        return "not connected (disconnect latched; no automatic reopen until connect)"
    _srv._session_log.info("disconnected from %s", port)
    return f"disconnected from {port}"


@_core._tool()
def link_status() -> str:
    """Report connection state, port, baud rate and protocol settings."""
    status = _srv._link.status()
    recommended = recommend_port()
    status["recommended_port"] = recommended
    return "\n".join(f"{k}: {v}" for k, v in status.items())


@_core._tool()
def get_device_log(n: int = 50) -> str:
    """Return the last ``n`` lines of firmware console output (ESP_LOGx).

    These are forwarded over the reliable UART link instead of going to the
    USB-Serial-JTAG console (see App/drivers/bridge/uart_log_bridge.c), so they're
    visible here without a debugger/second cable attached -- this is the
    pull-based equivalent of the GUI's Device Console window. It is also the
    only place a *device-level* failure (an SPI or I2C transfer that didn't
    work) ever becomes visible to the PC.
    """
    if n <= 0:
        return "error: n must be positive"
    entries = _srv._snapshot(_srv._device_log_history, n)
    if not entries:
        return "(no device _srv.log lines received yet)"
    return "\n".join(f"{line.letter} {line.text}" for _ts, line in entries)


@_core._tool()
def get_device_log_json(n: int = 50, min_level: str = "info") -> str:
    """Structured form of get_device_log: JSON array of
    {"pc_time": unix seconds, "level": name, "text": ...}, oldest first.

    ``pc_time`` is when this PC process received the line, not a device-side
    timestamp -- the ESP32 has no RTC this tool trusts, so PC arrival is the
    only common clock (same reasoning as the safety link's "age" field).
    ``min_level`` filters to that level and everything more severe (one of
    "error", "warn", "info", "debug", "verbose" -- ESP-IDF's own ordering).
    """
    level_names = {lvl.name.lower(): lvl for lvl in LogLevel}
    min_lvl = level_names.get(min_level.strip().lower())
    if min_lvl is None:
        return f"error: min_level must be one of {sorted(level_names)}, got {min_level!r}"
    if n <= 0:
        return "error: n must be positive"
    entries = _srv._snapshot(_srv._device_log_history, n)
    records = [
        {"pc_time": ts, "level": line.level.name.lower(), "text": line.text}
        for ts, line in entries
        if line.level <= min_lvl
    ]
    return json.dumps(records, indent=2)


@_core._tool()
def close_server() -> str:
    """Gracefully shut down this MCP server process.

    Closes (not disconnects -- see link_hub.py) this process's own
    connection to the shared UART link, flushes the session _srv.log, and exits.
    The physical link stays up for anything else still using it (the GUI,
    another MCP server process): this only ever tears down *this* process.

    The response is sent back before the process actually exits (on a short
    delay, from a background thread) so the caller sees confirmation rather
    than the connection just dropping.

    Since this server is HTTP now, exiting leaves nothing listening on its
    port: every client's kilnctrl tools stop working until it is started
    again. That is the right lever for picking up edited server code, but it
    is not self-healing -- follow it with the editor's "MCP Restart" button,
    or ``tools\\PcTools\\scripts\\mcp_servers.ps1 restart -Server kilnctrl``.
    ``POST /shutdown`` is the same stop without going through a tool call.
    """
    def _shutdown() -> None:
        time.sleep(0.2)  # let the transport flush this tool's reply first
        for client in (_srv._info, _srv._system, _srv._device_log, _srv._thermo, _srv._io, _srv._display, _srv._touch, _srv._safety, _srv._probe, _srv._wifi,
                       _srv._control, _srv._profiles, _srv._autotune):
            client.close()
        _srv._link.close()
        _srv._session_log.close()
        os._exit(0)

    threading.Thread(target=_shutdown, daemon=True, name="mcp-shutdown").start()
    return "shutting down"


@_core._tool()
def restart_uart() -> str:
    """On-demand recovery lever for a stuck/desynced UART link.

    Tells the ESP to flush its UART RX ring buffer and reset its rx-error
    counter (SYSTEM_CMD_RESTART_UART) -- useful if the link seems wedged,
    without power-cycling the board. Does not disconnect or reopen the local
    serial port; that's what disconnect()/connect() are for.
    """
    return _srv._send(UART_TASK_ID_SYSTEM, devices.system_restart_uart())


@_core._tool()
def get_watchdog_panic_disabled() -> str:
    """Query whether the ESP task-watchdog's PANIC half is disabled.

    This is a dev-only bench escape hatch (watchdog_cfg.h): when disabled, a
    task that hangs no longer triggers a panic/reboot -- the watchdog still
    monitors and still logs the timeout, the RTC watchdog is completely
    untouched and stays armed regardless, but a hung task now leaves the
    board just sitting there hung, with the relays in whatever state they
    were last commanded, instead of rebooting to clear it. The setting
    persists across reboots (it lives in NVS, not RAM).
    """
    # Same protocol-version gate _srv._send() applies to every non-INFO send, hand
    # rolled here because this query goes through SystemClient rather than
    # _srv._send(). Only INFO is exempt from the gate (that is how compatibility is
    # discovered at all); SYSTEM is not, and a v1 unit-test-fixture firmware
    # would happily accept task-6 traffic and mean something else entirely by
    # it -- see _srv._send()'s docstring for that exact hazard.
    if _srv._info.compatible is not True:
        reason = (
            "protocol version mismatch"
            if _srv._info.compatible is False
            else "firmware version not yet confirmed (call get_fw_version first)"
        )
        _srv._session_log.error("refused SYSTEM watchdog query: %s", reason)
        return f"error: refused - {reason}"
    try:
        disabled = _srv._system.get_watchdog_panic_disabled()
    except SystemQueryError as exc:
        return f"error: {exc}"
    return f"watchdog panic disabled: {disabled}"


@_core._tool()
def set_watchdog_panic_disabled(disabled: bool) -> str:
    """Enable/disable the ESP task-watchdog's PANIC half.

    Development-only setting -- never leave this disabled on a board that
    will actually fire a kiln. With it disabled, a hung task no longer
    reboots the board; it just sits hung with the relays in whatever state
    they were last commanded. The watchdog's monitoring and logging keep
    running either way, and the RTC watchdog is untouched and stays armed
    regardless. The firmware will warn again before any firing is started
    while this is off (see WATCHDOG_CFG_FIRING_WARNING). Takes effect
    immediately and persists across reboots; no reply frame is sent for this
    command -- poll get_watchdog_panic_disabled() afterward to confirm the
    applied value.
    """
    return _srv._send(UART_TASK_ID_SYSTEM, devices.system_set_watchdog_panic_disabled(disabled))

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
