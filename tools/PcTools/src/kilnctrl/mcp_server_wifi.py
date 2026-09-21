"""WIFI tools (task 11).

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
# WIFI (task 11) -- status/scan/provision/forget over UART
#
# Mirrors wifi_provision_http.c's HTTP endpoints, reachable over a link that
# still works when Wi-Fi itself is down or unconfigured -- the whole reason
# this task exists is to get a board onto a network without ever needing a
# browser pointed at its AP. The GUI has driven this since it was added
# (gui.py); these tools close the matching MCP-side gap.
# ---------------------------------------------------------------------------
@_srv._tool()
def wifi_get_status() -> str:
    """Report Wi-Fi mode (home/ap), connection state, SSID(s), IP and RSSI.

    The board's own AP password is never rendered here -- only whether one is
    set (2026-09-21 fix: the UART GET_STATUS reply carries it in plaintext,
    same as the wire always has, but no tool output should repeat the value).
    """
    try:
        status = _srv._wifi.get_status()
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    ap_password_state = "[set]" if status.ap_password else "[unset]"
    return (
        f"mode={status.mode_name} state={status.state} "
        f"sta_connected={status.sta_connected} ssid={status.ssid!r} "
        f"ap_ssid={status.ap_ssid!r} ap_password={ap_password_state} "
        f"sta_ip={status.sta_ip!r} "
        f"sta_rssi={status.sta_rssi} ap_clients={status.ap_clients}"
    )


@_srv._tool()
def wifi_scan() -> str:
    """Scan for nearby APs. Slower than the other WIFI tools (~seconds);
    capped at 6 entries by the firmware, with a truncated flag if more were seen."""
    try:
        entries, truncated = _srv._wifi.scan()
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "no networks found" + (" (truncated)" if truncated else "")
    lines = [f"{e.ssid!r}: rssi={e.rssi} secure={e.secure}" for e in entries]
    if truncated:
        lines.append("(truncated - more networks were seen than fit the reply)")
    return "\n".join(lines)


@_srv._tool()
def wifi_add_network(ssid: Optional[str] = None, password: Optional[str] = None) -> str:
    """Save a network and immediately attempt to join it -- this is the
    one-step way to connect the board to a specific scanned network; no
    separate wifi_set_mode() call is needed first. If the board is currently
    in AP (provisioning) mode, submitting a network switches it to home mode
    automatically. Empty password = open network.

    Caveat: the firmware re-runs its scan-based tie-break before joining, so
    if multiple saved networks are in range it may connect to a stronger one
    instead of the one just added -- "ok" here means "saved and a join was
    attempted", not "connected to this exact SSID"; call wifi_get_status()
    afterward to see which network (if any) actually came up.

    Leave ssid unset to auto-connect using the credentials most recently
    saved from the GUI's Wi-Fi Settings popup (see wifi_credentials.py) --
    fails with a clear error if nothing has been saved that way yet."""
    if ssid is None:
        saved = wifi_credentials.load()
        if saved is None:
            return "error: no ssid given and no saved credentials found (set Wi-Fi up once via the GUI first)"
        ssid, password = saved["ssid"], saved["password"]
    try:
        result = _srv._wifi.add_network(ssid, password or "")
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - saved {ssid!r}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not save {ssid!r}{detail}"


@_srv._tool()
def wifi_set_mode(mode: str) -> str:
    """Switch between "home" (join a saved network) and "ap" (host the
    provisioning access point) mode."""
    mode_map = {"home": WIFI_MODE_HOME, "ap": WIFI_MODE_AP}
    mode_val = mode_map.get(mode.strip().lower())
    if mode_val is None:
        return f"error: mode must be 'home' or 'ap', got {mode!r}"
    try:
        result = _srv._wifi.set_mode(mode_val)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - mode set to {mode}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set mode to {mode}{detail}"


@_srv._tool()
def wifi_set_ap_identity(ap_ssid: Optional[str] = None, ap_password: Optional[str] = None) -> str:
    """Rename the board's own provisioning AP and/or change its password.
    Leave either argument unset (None) to keep it unchanged."""
    try:
        result = _srv._wifi.set_ap_identity(ap_ssid, ap_password)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - AP identity updated"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not update AP identity{detail}"


@_srv._tool()
def wifi_get_networks() -> str:
    """List saved networks, each with in-range/rssi/secure/connected if seen
    in the last scan. Capped at 5 entries by the firmware."""
    try:
        entries, truncated = _srv._wifi.get_networks()
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "no saved networks"
    lines = [
        f"{e.ssid!r}: saved={e.saved} in_range={e.in_range} rssi={e.rssi} "
        f"secure={e.secure} connected={e.connected}"
        for e in entries
    ]
    if truncated:
        lines.append("(truncated - more saved networks exist than fit the reply)")
    return "\n".join(lines)


@_srv._tool()
def wifi_forget(ssid: str) -> str:
    """Delete a saved network."""
    try:
        result = _srv._wifi.forget(ssid)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - forgot {ssid!r}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - no such saved network {ssid!r}{detail}"


