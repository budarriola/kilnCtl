"""IO / SX1509 expander tools (task 2).

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
# IO -- SX1509 expander at 0x3E (task 2)
#
# Relay numbering is the schematic's, and it does NOT match the K
# designators: Relay1=K3/J8, Relay2=K1/J3, Relay3=K2/J4, Relay4=K5/J11.
# Every tool below repeats that mapping rather than assuming the caller
# remembers it.
# ---------------------------------------------------------------------------
@_core._tool()
def io_read() -> str:
    """Read the expander: relays, digital I/O levels/directions, DRDY, raw regs.

    Returns real device data (a query). This is also the only place the three
    thermocouple ~DRDY lines are visible, since they go to the expander rather
    than to an ESP32 GPIO.
    """
    try:
        state = _srv._io.read()
    except IoQueryError as exc:
        return f"error: {exc}"
    return state.describe()


@_core._tool()
def io_set_relay(relay: int, on: bool) -> str:
    """Switch one relay on or off.

    Relay numbering follows the schematic and does not match the K
    designators: 1=K3/J8, 2=K1/J3, 3=K2/J4, 4=K5/J11. `on` energizes the 12 V
    coil.

    Waits briefly for a refusal reply before reporting success: the
    firmware can refuse this because a running profile owns the relay, a
    safety fault is asserted, or an OTA is in progress -- distinguishable
    reasons that a bare transport ACK cannot tell apart from "energized".

    Relay4's expander bit does NOT control K4/J11: K4 is closed only by the
    RP2040 safety processor in response to SAFETY_CMD_REQUEST_ENABLE (see
    firmware/KilnFW/App/drivers/control/heat_enable.h). Driving this bit
    directly cannot energize the heater and would read back as "on" with no
    heat ever granted, so this refuses -- use safety_request_enable instead.
    """
    if relay == 4:
        return (
            "refused - relay 4's expander bit is NOT K4/heat state; K4 is "
            "Pico-owned via SAFETY_CMD_REQUEST_ENABLE -- use "
            "safety_request_enable(true) instead"
        )
    try:
        result = _srv._io.set_relay(relay, on)
    except IoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - relay {relay} {'on' if on else 'off'}"
    return f"refused ({result.refusal.value}) - relay {relay} not changed" + (
        f": {result.reason_text}" if result.reason_text and result.refusal is devices.RelayRefusal.OTHER else ""
    )


@_core._tool()
def io_set_relay_mask(mask: int, value: int) -> str:
    """Switch several relays in one atomic register write.

    Bits 0-3 are Relay1..Relay4 (K3/J8, K1/J3, K2/J4, K5/J11) in both `mask`
    (which relays to change) and `value` (their new levels).

    Same refusal-aware wait as :func:`io_set_relay` -- see its docstring,
    including why bit 3 (Relay4/K4) is refused here too: that expander bit is
    not heat state, K4 is Pico-owned via SAFETY_CMD_REQUEST_ENABLE, and this
    tool refuses whole-call rather than silently no-op'ing on that one bit.
    """
    if mask & 0x08:
        return (
            "refused - mask bit 3 (relay 4) is NOT K4/heat state; K4 is "
            "Pico-owned via SAFETY_CMD_REQUEST_ENABLE -- use "
            "safety_request_enable(true) instead, and retry this call "
            "without bit 3 for the other relays"
        )
    try:
        result = _srv._io.set_relay_mask(mask, value)
    except IoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - relay mask 0x{mask:02X} set to 0x{value:02X}"
    return f"refused ({result.refusal.value}) - relay mask not changed" + (
        f": {result.reason_text}" if result.reason_text and result.refusal is devices.RelayRefusal.OTHER else ""
    )


def _io_mutating(label: str, call: "Callable[[], devices.OkReason]") -> str:
    """Run one plain IO write (not SET_RELAY/SET_RELAY_MASK, which keep their
    own RelayResult-based formatting above) and format its :class:`OkReason`.

    Same shape as _display_mutating()/_touch_mutating() -- see
    IoClient._write_style().
    """
    try:
        result = call()
    except IoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - {label}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - {label}{detail}"


@_core._tool()
def io_all_relays_off() -> str:
    """De-energize all four relays unconditionally.

    The same state the firmware falls back to on link loss or a safety fault.
    """
    return _io_mutating("all relays off", lambda: _srv._io.all_relays_off())


@_core._tool()
def io_set_output(io: int, level: bool) -> str:
    """Drive digital I/O 1-7 high or low (only meaningful when it is an output).

    IO1 = opto-isolated input from J24, IO2 = opto-isolated output to J25,
    IO3/IO4 = J20 pins 1/2, IO5/IO6 = J21 pins 1/2, IO7 = J23 pin 1.
    """
    return _io_mutating(f"IO{io} set {'high' if level else 'low'}", lambda: _srv._io.set_io(io, level))


@_core._tool()
def io_set_direction(io: int, is_input: bool, pullup: bool = False) -> str:
    """Set digital I/O 1-7 as an input (with optional pull-up) or an output."""
    return _io_mutating(
        f"IO{io} direction set", lambda: _srv._io.set_io_dir(io, is_input, pullup)
    )


@_core._tool()
def io_set_auto_report(period_ms: int = 500) -> str:
    """Have the firmware push expander state every period_ms (0 = off).

    Pushes also happen immediately on every ~INT edge, so an input change is
    reported without waiting out the period. Read them with io_get_reports().
    """
    return _io_mutating(
        f"auto-report period {period_ms}ms", lambda: _srv._io.set_auto_report(period_ms)
    )


@_core._tool()
def io_get_reports(n: int = 10) -> str:
    """Return the last `n` expander auto-report pushes.

    The pull-based view of a stream an MCP client cannot otherwise receive --
    turn it on with io_set_auto_report() first.
    """
    if n <= 0:
        return "error: n must be positive"
    states = _srv._snapshot(_srv._io_reports, n)
    if not states:
        return "(no auto-report pushes received yet -- call io_set_auto_report first)"
    return "\n".join(s.describe() for s in states)


@_core._tool()
def io_scan() -> str:
    """Probe 0x3E/0x3F/0x70/0x71 for an SX1509; report which addresses answered.

    The board straps ADDR1/ADDR0 to GND, so only 0x3E should answer -- anything
    else is a different part or a strapping error.
    """
    try:
        found = _srv._io.scan()
    except IoQueryError as exc:
        return f"error: {exc}"
    if not found:
        return "no device answered (expected 0x3E)"
    return ", ".join(f"0x{a:02X}" for a in found)


@_core._tool()
def expander_read_reg(reg: int, length: int = 1) -> str:
    """Raw SX1509 register read (debug), 1-16 bytes."""
    try:
        registers = _srv._io.read_reg(reg, length)
    except IoQueryError as exc:
        return f"error: {exc}"
    return registers.describe()


@_core._tool()
def expander_write_reg(reg: int, value: int) -> str:
    """Raw SX1509 register write (debug)."""
    return _io_mutating(f"reg 0x{reg:02X} <- 0x{value:02X}", lambda: _srv._io.sx_write_reg(reg, value))


@_core._tool()
def expander_set_dir(mask: int) -> str:
    """Write RegDir: u16, bit N = 1 makes expander pin N an input."""
    return _io_mutating(f"RegDir <- 0x{mask:04X}", lambda: _srv._io.sx_set_dir(mask))


@_core._tool()
def expander_set_pullup(mask: int) -> str:
    """Write RegPullUp: u16, bit N = 1 enables pin N's pull-up."""
    return _io_mutating(f"RegPullUp <- 0x{mask:04X}", lambda: _srv._io.sx_set_pullup(mask))


@_core._tool()
def expander_set_opendrain(mask: int) -> str:
    """Write RegOpenDrain: u16, bit N = 1 makes pin N open-drain."""
    return _io_mutating(f"RegOpenDrain <- 0x{mask:04X}", lambda: _srv._io.sx_set_opendrain(mask))


@_core._tool()
def expander_set_debounce(enable_mask: int, config: int = 0) -> str:
    """Enable debounce on the masked pins; `config` 0-7 selects 0.5ms << config."""
    return _io_mutating(
        f"debounce mask 0x{enable_mask:04X} config {config}",
        lambda: _srv._io.sx_set_debounce(enable_mask, config),
    )


@_core._tool()
def expander_set_int_mask(mask: int, sense: int = 0) -> str:
    """Write RegInterruptMask (bit N = 1 DISABLES pin N's interrupt) and RegSense.

    `sense` is 2 bits per pin *pair*, exactly as the part encodes them.
    """
    return _io_mutating(
        f"RegInterruptMask <- 0x{mask:04X}, sense 0x{sense:04X}",
        lambda: _srv._io.sx_set_int_mask(mask, sense),
    )


@_core._tool()
def expander_led_driver(pin: int, enable: bool, intensity: int = 0) -> str:
    """Enable the SX1509's LED driver on one pin (0-15).

    `intensity` 0-255, where 0 is *full on* for this part's sink driver.
    """
    return _io_mutating(
        f"pin {pin} LED driver {'on' if enable else 'off'} @ {intensity}",
        lambda: _srv._io.sx_led_driver(pin, enable, intensity),
    )


@_core._tool()
def expander_reset(hard: bool = False) -> str:
    """Reset the SX1509: software reset via RegReset, or pulse ~RESET (GPIO10)."""
    return _io_mutating(f"SX1509 {'hard' if hard else 'soft'} reset", lambda: _srv._io.sx_reset(hard))


def _touch_mutating(label: str, call: "Callable[[], devices.OkReason]") -> str:
    """Run one TOUCH write (INJECT/SET_TAP_DUMP/LOG_TAP_TARGETS) and format
    its :class:`OkReason` -- see TouchClient._write().

    (Used to say "same shape as :func:`_display_mutating`" -- that helper and
    the twelve-plus ``display_*`` MCP tools it backed were removed as stale:
    the firmware's ``display_bridge_task`` is intentionally dead code now that
    LVGL owns the ILI9488 outright. See firmware/KilnFW/TODO.md sec 10.1.
    ``_srv._display`` (the :class:`DisplayClient` instance) stays -- it still
    backs the generic ``press_button``/``list_buttons`` DISPLAY actions in
    ``actions.py``, which also drive ``gui.py``'s Display panel and were left
    out of this cleanup as a separate, still-referenced front end.)
    """
    try:
        result = call()
    except TouchQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - {label}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - {label}{detail}"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
