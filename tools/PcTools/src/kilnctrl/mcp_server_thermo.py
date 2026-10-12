"""THERMO tools (task 1).

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
# THERMO -- 3x MAX31856 (task 1)
#
# The parts sit on the thermocouple daughterboard behind J6, on the shared SPI
# bus (CS0/CS1/CS2 = channels 0/1/2). Their ~DRDY outputs do NOT reach the
# ESP32 -- they go to the SX1509, so "is a conversion ready" is reported by
# io_read(), not here.
# ---------------------------------------------------------------------------
@_core._tool()
def thermo_read(channel: int = THERMO_CHANNEL_ALL) -> str:
    """Read thermocouple temperature, cold junction and decoded faults.

    `channel` 0-2 for one channel, or omit for all three. Returns real device
    data (this is a query, not a fire-and-forget command). A channel whose SPI
    read failed still appears, flagged, with NaN temperatures.
    """
    try:
        readings = _srv._thermo.read(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if not readings:
        return "device reported no channels"
    return "\n".join(r.describe() for r in readings)


@_core._tool()
def thermo_read_faults(channel: int = THERMO_CHANNEL_ALL) -> str:
    """Read the fault status (SR) and MASK registers, decoded to text.

    `channel` 0-2, or omit for all three. Unlike thermo_read this does not
    depend on a conversion having happened.
    """
    try:
        faults = _srv._thermo.read_faults(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if not faults:
        return "device reported no channels"
    return "\n".join(f.describe() for f in faults)


def _write_gate(what: str, confirm: object) -> Optional[str]:
    """Shared gate for thermo writers: confirm must be exactly True and no firing/autotune may be live.

    Fail closed: an unreadable run state refuses. Changing MAX31856 config mid-firing alters the
    very reading the control loop and safety guards act on (the firmware-side mode gate is a
    separate task; this is the tool-side half).
    """
    if confirm is not True:
        return (f"refused: {what} rewrites MAX31856 configuration; pass confirm=True "
                f"(exactly True) to proceed")
    from .mcp_server_control import _profile_or_autotune_running_reason  # local: avoids an import cycle
    running = _profile_or_autotune_running_reason()
    if running is not None:
        return f"refused: {what} not allowed while a run is live: {running}"
    return None


@_core._tool()
def thermo_config_channel(
    channel: int,
    tc_type: int = 3,
    avg_mode: int = 0,
    filter_50hz: bool = False,
    auto_convert: bool = True,
    confirm: bool = False,
) -> str:
    """Configure one thermocouple channel.

    `tc_type`: 0=B 1=E 2=J 3=K 4=N 5=R 6=S 7=T (8/12 are raw voltage modes).
    K is what this kiln ships with. `avg_mode`: 0=1, 1=2, 2=4, 3=8, 4=16
    samples averaged. `filter_50hz` picks 50 Hz mains rejection instead of
    60 Hz. `auto_convert` false selects one-shot mode, where nothing converts
    until thermo_one_shot(). Writes MAX31856 config: needs `confirm=True` and refuses
    while a profile or autotune is live.
    """
    gate = _write_gate("thermo_config_channel", confirm)
    if gate:
        return gate
    try:
        result = _srv._thermo.config_channel(channel, tc_type, avg_mode, filter_50hz, auto_convert)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} configured"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not configure channel {channel}{detail}"


@_core._tool()
def thermo_set_thresholds(
    channel: int, tc_high: float, tc_low: float, cj_high: int, cj_low: int, confirm: bool = False
) -> str:
    """Set one channel's TC high/low trip temperatures and CJ high/low limits (degC).

    Needs `confirm=True`; refuses while a profile or autotune is live."""
    gate = _write_gate("thermo_set_thresholds", confirm)
    if gate:
        return gate
    try:
        result = _srv._thermo.set_thresholds(channel, tc_high, tc_low, cj_high, cj_low)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} thresholds set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set channel {channel} thresholds{detail}"


@_core._tool()
def thermo_set_cj_offset(channel: int, offset_c: float, confirm: bool = False) -> str:
    """Set one channel's cold-junction offset in degC (-8..+8).

    Needs `confirm=True`; refuses while a profile or autotune is live."""
    gate = _write_gate("thermo_set_cj_offset", confirm)
    if gate:
        return gate
    try:
        result = _srv._thermo.set_cj_offset(channel, offset_c)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} CJ offset set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set channel {channel} CJ offset{detail}"


@_core._tool()
def thermo_one_shot(channel: int) -> str:
    """Trigger a single conversion on one channel (poll with thermo_read after)."""
    try:
        result = _srv._thermo.one_shot(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - one-shot triggered on channel {channel}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not trigger one-shot on channel {channel}{detail}"


@_core._tool()
def thermo_clear_faults(channel: int, confirm: bool = False) -> str:
    """Pulse CR0.FAULTCLR on one channel.

    Only meaningful in the part's interrupt fault mode; in the comparator mode
    this driver uses, fault bits clear themselves when the condition clears.
    Needs `confirm=True`; refuses while a profile or autotune is live.
    """
    gate = _write_gate("thermo_clear_faults", confirm)
    if gate:
        return gate
    try:
        result = _srv._thermo.clear_faults(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} faults cleared"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not clear channel {channel} faults{detail}"


@_core._tool()
def thermo_set_auto_report(channel_mask: int = 0x07, period_ms: int = 1000) -> str:
    """Have the firmware push readings for the masked channels every period_ms.

    Bit N of `channel_mask` selects channel N; `period_ms` 0 turns it off.
    Pushed readings are buffered here -- read them with thermo_get_reports().
    """
    try:
        result = _srv._thermo.set_auto_report(channel_mask, period_ms)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - auto-report mask 0x{channel_mask:02X} period {period_ms} ms"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set auto-report{detail}"


@_core._tool()
def thermo_get_reports(n: int = 10) -> str:
    """Return the last `n` auto-report pushes received since reporting was on.

    An MCP client cannot receive an unsolicited push, so this is the pull-based
    view of that stream -- turn it on with thermo_set_auto_report() first.
    """
    if n <= 0:
        return "error: n must be positive"
    batches = _srv._snapshot(_srv._thermo_reports, n)
    if not batches:
        return "(no auto-report pushes received yet -- call thermo_set_auto_report first)"
    return "\n".join(
        "; ".join(r.describe() for r in batch) for batch in batches
    )


@_core._tool()
def thermo_read_reg(channel: int, reg: int, length: int = 1) -> str:
    """Raw MAX31856 register read on one channel (debug), 1-16 bytes."""
    try:
        registers = _srv._thermo.read_reg(channel, reg, length)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    return registers.describe()


@_core._tool()
def thermo_write_reg(channel: int, reg: int, value: int, confirm: bool = False) -> str:
    """Raw MAX31856 register write on one channel (debug).

    Needs `confirm=True`; refuses while a profile or autotune is live. Reads the register back
    and reports a mismatch (volatile/status registers may legitimately differ)."""
    gate = _write_gate("thermo_write_reg", confirm)
    if gate:
        return gate
    try:
        result = _srv._thermo.write_reg(channel, reg, value)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        try:
            back = _srv._thermo.read_reg(channel, reg, 1)
            got = list(getattr(back, "values", None) or getattr(back, "data", None) or [])
        except ThermoQueryError as exc:
            return (f"FAILED - channel {channel} reg 0x{reg:02X}: write acknowledged but the read-back "
                    f"failed ({exc}); state UNVERIFIED")
        if not got:
            return (f"FAILED - channel {channel} reg 0x{reg:02X}: write acknowledged but the read-back "
                    "returned no value; state UNVERIFIED")
        if got[0] != value:
            return (f"FAILED - channel {channel} reg 0x{reg:02X} written but read back "
                    f"0x{got[0]:02X}, wanted 0x{value:02X}")
        return f"ok - channel {channel} reg 0x{reg:02X} written"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not write channel {channel} reg 0x{reg:02X}{detail}"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
