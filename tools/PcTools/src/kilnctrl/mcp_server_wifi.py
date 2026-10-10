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

from . import actions, config_presets, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, wifi_prov_http_client, zones_http_client
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


def _wifi_write_refusal(confirm: object, what: str, allow_running: object = False) -> Optional[str]:
    """Common gate for Wi-Fi writes: confirm is True exactly, and not mid-firing
    (a join/mode switch can drop the AP/STA link the operator is watching the
    firing through)."""
    if confirm is not True:
        return f"error: {what} refused without confirm=True -- it changes the board's network configuration"
    if allow_running is not True:
        try:
            status = _srv._profiles.get_exec_status(timeout=2.0)
        except Exception as exc:  # noqa: BLE001
            return (f"error: {what} refused -- profile executor state could not be read ({exc}), so a "
                    "firing cannot be ruled out. Pass allow_running=True (exactly True) to override.")
        if status is None:
            return (f"error: {what} refused -- profile executor state could not be read (no answer). "
                    "Pass allow_running=True (exactly True) to override.")
        if status.state not in (0, 3, 4):
            return (f"error: {what} refused while a profile is {status.state_name} (running/paused/unknown) -- a Wi-Fi change can "
                    "drop the link mid-firing. Stop the profile first or pass allow_running=True.")
        try:
            at = _srv._autotune.get_status()
        except Exception as exc:  # noqa: BLE001
            return (f"error: {what} refused -- autotune state could not be read ({exc}). "
                    "Pass allow_running=True (exactly True) to override.")
        if at.state not in (0, 5, 6):
            return (f"error: {what} refused while autotune is {at.state_name!r} -- a Wi-Fi change can "
                    "drop the link mid-run. Pass allow_running=True to override.")
    return None


def _wifi_readback_networks(ssid: str) -> "Optional[bool]":
    """True/False if ssid is in the saved list, None if unreadable."""
    try:
        entries, _trunc = _srv._wifi.get_networks()
    except Exception:  # noqa: BLE001
        return None
    return any(e.ssid == ssid for e in entries)


# ---------------------------------------------------------------------------
# WIFI (task 11) -- status/scan/provision/forget over UART
#
# Mirrors wifi_provision_http.c's HTTP endpoints, reachable over a link that
# still works when Wi-Fi itself is down or unconfigured -- the whole reason
# this task exists is to get a board onto a network without ever needing a
# browser pointed at its AP. The GUI has driven this since it was added
# (gui.py); these tools close the matching MCP-side gap.
# ---------------------------------------------------------------------------
@_core._tool()
def wifi_get_status(host: Optional[str] = None) -> str:
    """Report Wi-Fi mode (home/ap), connection state, SSID(s), IP and RSSI.

    The board's own AP password is never rendered here -- only whether one is
    set (2026-09-21 fix: the UART GET_STATUS reply carries it in plaintext,
    same as the wire always has, but no tool output should repeat the value).

    `state` is printed by name (e.g. "connected"), not the raw wire enum
    byte -- devices_wifi_uart.UartWifiStatus.state_name.

    `ap_pending_teardown` (be7bcad4, 2026-09-28 -- true while home Wi-Fi is
    back up but the fallback AP is deliberately being kept alive because a
    session is logged in) is NOT carried over the UART link this tool
    otherwise uses -- only GET /status (wifi_provision_http.c) serializes it,
    so it is fetched over HTTP, best-effort. Host resolution follows the
    other tools' convention (`_ota_resolve_host`): an explicit `host` wins;
    otherwise the STA IP this same UART read just reported is used when
    `sta_connected` (the only state in which a teardown can be pending). With
    neither, no network call is made. Unreachable, not JSON, or missing the
    key (older firmware) all print "unknown (...)" rather than failing the
    whole call. As of be7bcad4, GET
    /status does not separately expose an `ap_fallback_active` field either
    (that flag is internal-only, never serialized) -- nothing else to add
    here without a firmware change.
    """
    try:
        status = _srv._wifi.get_status()
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    ap_password_state = "[set]" if status.ap_password else "[unset]"
    http_host = host or (status.sta_ip if status.sta_connected and status.sta_ip else None)
    ap_pending_teardown = "unknown (no host given and no STA IP)"
    if http_host:
        try:
            # An explicit caller-supplied host is not established as the board:
            # trusted=False sends no credential and never records it as default.
            data = wifi_prov_http_client.get_status(http_host, trusted=host is None)
        except wifi_prov_http_client.WifiProvHttpError:
            ap_pending_teardown = "unknown (unreachable over HTTP)"
        else:
            if isinstance(data, dict) and "ap_pending_teardown" in data:
                ap_pending_teardown = str(bool(data["ap_pending_teardown"]))
            else:
                ap_pending_teardown = "unknown (older firmware)"
    return (
        f"mode={status.mode_name} state={status.state_name} "
        f"sta_connected={status.sta_connected} ssid={status.ssid!r} "
        f"ap_ssid={status.ap_ssid!r} ap_password={ap_password_state} "
        f"sta_ip={status.sta_ip!r} "
        f"sta_rssi={status.sta_rssi} ap_clients={status.ap_clients} "
        f"ap_pending_teardown={ap_pending_teardown}"
    )


@_core._tool()
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


@_core._tool()
def wifi_add_network(
    ssid: Optional[str] = None, password: Optional[str] = None, confirm: bool = False, allow_running: bool = False
) -> str:
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
    fails with a clear error if nothing has been saved that way yet.

    Requires confirm=True (exactly) and refuses while a profile is running or
    paused (allow_running=True overrides): joining can drop the link. After a
    success the saved list is read back. Passwords are never echoed (note: the
    password travels as a tool parameter, which MCP clients may log -- prefer
    the settings page or an env-var-fed path for a real credential).
    """
    refusal = _wifi_write_refusal(confirm, "wifi_add_network", allow_running)
    if refusal is not None:
        return refusal
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
        present = _wifi_readback_networks(ssid)
        if present is False:
            return f"FAILED - board reported ok for {ssid!r} but it is not in the saved list on read-back"
        note = "" if present else " (read-back unavailable; unverified)"
        return f"ok - saved {ssid!r}{note}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not save {ssid!r}{detail}"


@_core._tool()
def wifi_set_mode(mode: str, confirm: bool = False, allow_running: bool = False) -> str:
    """Switch between "home" (join a saved network) and "ap" (host the
    provisioning access point) mode. Requires confirm=True and refuses
    mid-firing (allow_running=True overrides); the mode is read back."""
    refusal = _wifi_write_refusal(confirm, "wifi_set_mode", allow_running)
    if refusal is not None:
        return refusal
    mode_map = {"home": WIFI_MODE_HOME, "ap": WIFI_MODE_AP}
    mode_val = mode_map.get(mode.strip().lower())
    if mode_val is None:
        return f"error: mode must be 'home' or 'ap', got {mode!r}"
    try:
        result = _srv._wifi.set_mode(mode_val)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        try:
            got = _srv._wifi.get_status().mode_name
        except Exception:  # noqa: BLE001
            return f"ok - mode set to {mode} (read-back unavailable; unverified)"
        if got != mode.strip().lower():
            return f"FAILED - board reported ok but mode reads back as {got!r}, wanted {mode!r}"
        return f"ok - mode set to {mode}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set mode to {mode}{detail}"


@_core._tool()
def wifi_set_ap_identity(ap_ssid: Optional[str] = None, ap_password: Optional[str] = None) -> str:
    """Rename the board's own provisioning AP and/or change its password.
    Leave either argument unset (None) to keep it unchanged.

    Not confirm-gated by design: the AP identity is re-settable; the AP password is never echoed (note: it travels as a tool parameter, which MCP clients may log).
    """
    try:
        result = _srv._wifi.set_ap_identity(ap_ssid, ap_password)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - AP identity updated"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not update AP identity{detail}"


@_core._tool()
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


@_core._tool()
def wifi_forget(ssid: str, confirm: bool = False, allow_running: bool = False) -> str:
    """Delete a saved network. Requires confirm=True and refuses mid-firing
    (allow_running=True overrides); the saved list is read back."""
    refusal = _wifi_write_refusal(confirm, "wifi_forget", allow_running)
    if refusal is not None:
        return refusal
    try:
        result = _srv._wifi.forget(ssid)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        present = _wifi_readback_networks(ssid)
        if present is True:
            return f"FAILED - board reported ok but {ssid!r} is still in the saved list"
        if present is None:
            return (f"FAILED - board reported ok for forgetting {ssid!r} but the saved list could not be "
                    "read back; state UNVERIFIED")
        return f"ok - forgot {ssid!r}"
    detail = f": {result.reason}" if result.reason else " (no reason given; the board may simply not have that network saved)"
    return f"refused - could not forget {ssid!r}{detail}"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
