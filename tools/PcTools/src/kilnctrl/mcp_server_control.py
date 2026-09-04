"""CONTROL tools (task 8).

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
# CONTROL (task 8) -- zone PID/model config, mirrors zones_http.c
#
# Manual relay control is NOT here -- see io_set_relay/io_set_relay_mask.
# ---------------------------------------------------------------------------
def _control_resolve_host(host: Optional[str]) -> str:
    """Same discovery convention as ota_http.py's ``_ota_resolve_host()``:
    an explicit `host` always wins; otherwise prefer the board's current
    Wi-Fi station IP (read over the UART link, which works even with Wi-Fi
    down), falling back to the fixed fallback-AP address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return ota_http.OTA_AP_DEFAULT_HOST


def _describe_coupling_matrix(zones_json: dict) -> str:
    """Render the coupling matrix from a GET /api/zones JSON body.

    Orientation is c[i][j] = how much zone i's (the AFFECTED zone's)
    temperature moves per unit of zone j's (the STEPPED zone's) actuation --
    zones_http.c emits it that way, and it is the same orientation
    coupling_workflow.py/coupled_ident.py assume. The diagonal is always 0
    (the firmware force-ranges the diagonal cell to exactly 0 -- see
    zones_http_client.py's _ZONE_COUPLING_CELL_RE comment). This orientation
    is easy to get backwards -- test_zones_http_client.py carries
    test_TRANSPOSED_mapping_is_caught_by_this_test because it happened once
    already -- so the row/col meaning is spelled out here rather than left
    implicit."""
    zones = zones_json.get("zones", [])
    n = len(zones)
    lines = [
        "coupling matrix (row i = AFFECTED zone, column j = STEPPED zone; "
        "c[i][j] = how much zone i's temperature moves per unit of zone j's "
        "actuation; diagonal is always 0):"
    ]
    for i, z in enumerate(zones):
        cells = [z.get(f"coupling_c{j}") for j in range(n)]
        cells_str = ", ".join(
            "0" if c == 0 else (f"{c:.2f}" if isinstance(c, (int, float)) else "?")
            for c in cells
        )
        lines.append(f"  z{i}: [{cells_str}]")
    diag_bits = []
    for i, z in enumerate(zones):
        k_dc = z.get("coupling_diag_k_dc")
        if k_dc == 0.0:
            diag_bits.append(f"z{i}=0.0 (never identified on hardware)")
        else:
            diag_bits.append(
                f"z{i}={k_dc:.4f} (measured, but firmware's "
                "s_coupling_use_measured_diag_k_dc is compiled false -- not "
                "currently used even though present)"
            )
    lines.append("coupling_diag_k_dc: " + "  ".join(diag_bits))
    return "\n".join(lines)


@_srv._tool()
def control_get_zones(host: Optional[str] = None) -> str:
    """Read every zone's current PID/model config, calibration offset and
    temperature limits, plus the thermocouple and relay counts.

    Also fetches the per-zone coupling matrix (coupling_c0.., a first-class
    control parameter -- which matrix is live measurably changes tracking
    IAE) and coupling_diag_k_dc over HTTP GET /api/zones, since neither is
    on the UART CONTROL wire. Host is auto-resolved the same way the OTA
    tools do (board's Wi-Fi station IP, falling back to the fallback-AP
    address); pass `host` explicitly for kilnctl.local or a board reachable
    only from a different network than this link. If the HTTP fetch fails
    the PID/model section above is still returned, with the coupling
    section noting why it's missing."""
    try:
        thermo_count, relay_count, zones = _srv._control.get_zones()
    except ControlQueryError as exc:
        return f"error: {exc}"
    header = f"{thermo_count} thermocouple(s), {relay_count} relay(s)"
    body = header
    if zones:
        body += "\n" + "\n".join(z.describe() for z in zones)

    resolved_host = _control_resolve_host(host)
    try:
        zones_json = zones_http_client.get_zones(resolved_host)
    except zones_http_client.ZonesHttpError as exc:
        return body + f"\ncoupling matrix: unavailable ({exc}, host={resolved_host})"
    return body + "\n" + _describe_coupling_matrix(zones_json)


@_srv._tool()
def control_set_zone_pid(zone: int, kp: float, ki: float, kd: float) -> str:
    """Set a zone's PID gains."""
    try:
        result = _srv._control.set_zone_pid(zone, kp, ki, kd)
    except ControlQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - zone {zone} PID set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set zone {zone} PID{detail}"


@_srv._tool()
def control_set_zone_model(zone: int, k_dc: float, tau_s: float, dead_time_s: float) -> str:
    """Set a zone's feedforward thermal model (steady-state gain, time
    constant, dead time), used for model feedforward and autotune seeding."""
    try:
        result = _srv._control.set_zone_model(zone, k_dc, tau_s, dead_time_s)
    except ControlQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - zone {zone} model set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set zone {zone} model{detail}"


