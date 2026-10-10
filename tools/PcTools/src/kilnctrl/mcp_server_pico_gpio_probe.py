"""Pico GPIO probe over SWD.

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
# PICO GPIO PROBE -- raw RP2040 pin control over SWD (tools/PcTools/TODO.md
# capability 1b), the Pico-side mirror of the ESP GPIO_PROBE task above.
#
# No firmware agent needed on the Pico: RP2040 GPIO is memory-mapped (SIO +
# IO_BANK0 + PADS_BANK0), read/written directly over the existing OpenOCD/SWD
# connection via debug_probe.py. See pico_gpio_probe.py's module docstring
# for the register approach and exactly what is and is not guarded.
#
# GPIO6 (saftyRelay -> K4 pilot relay coil) is refused for SET_MODE and
# WRITE unconditionally, with no confirm/override -- enforced in
# pico_gpio_probe.py itself, not just here. READ is allowed for GPIO6.
#
# Unlike the ESP tools, there is no "profile running" or "ARMED" gate here:
# SaftyFW exposes no protocol yet for the PC to query relay state (same gap
# debug_write_memory() already documents for peer="pico"). GPIO6's hard deny
# is what stands in for that guard rail today.
# ---------------------------------------------------------------------------
@_core._tool()
def pico_gpio_set_mode(gpio_num: int, mode: str, confirm: bool = False) -> str:
    """Configure a Pico (RP2040) GPIO as input / input_pullup / input_pulldown
    / output, over SWD -- no SaftyFW build flag needed, this works against
    any Pico regardless of firmware.

    ``mode`` is one of "input", "input_pullup", "input_pulldown", "output".
    Refused unconditionally for GPIO6 (`saftyRelay`, drives K4's pilot relay
    coil) -- there is no override. Halts the core to perform the register
    writes (same as debug_write_memory). Requires confirm=True (exactly),
    like debug_write_memory.
    """
    if confirm is not True:
        return "error: pico_gpio_set_mode refused without confirm=True -- it halts the Pico core and rewrites pad registers"
    mode_val = mode.strip().lower()
    try:
        pico_gpio_probe.set_mode(gpio_num, mode_val)
    except pico_gpio_probe.PicoGpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ValueError as exc:
        return f"error: {exc}"
    except RuntimeError as exc:
        return f"error: {exc}"
    _srv._session_log.warning("pico_gpio_set_mode: gpio%d set to %s", gpio_num, mode_val)
    return f"ok - pico gpio{gpio_num} set to {mode_val}"


@_core._tool()
def pico_gpio_write(gpio_num: int, level: bool, confirm: bool = False) -> str:
    """Drive a Pico (RP2040) GPIO high or low, over SWD.

    Refused unconditionally for GPIO6. Refused for any other pin not already
    configured OUTPUT via :func:`pico_gpio_set_mode` in this same process.
    Requires confirm=True (exactly). The input level is read back afterwards;
    a mismatch is reported as FAILED (a loaded pin can legitimately differ,
    but is still not the commanded level).
    """
    if confirm is not True:
        return "error: pico_gpio_write refused without confirm=True -- it drives a live Pico pin"
    try:
        pico_gpio_probe.write(gpio_num, level)
    except pico_gpio_probe.PicoGpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except RuntimeError as exc:
        return f"error: {exc}"
    _srv._session_log.warning("pico_gpio_write: gpio%d = %s", gpio_num, "high" if level else "low")
    note = ""
    try:
        if bool(pico_gpio_probe.read(gpio_num)) != bool(level):
            return (f"FAILED - pico gpio{gpio_num} commanded {'high' if level else 'low'} but the input level "
                    "reads back different (pin loaded, or the write did not take)")
    except Exception:  # noqa: BLE001
        note = " (read-back unavailable; UNVERIFIED)"
    return f"ok - pico gpio{gpio_num} = {'high' if level else 'low'}{note}"


@_core._tool()
def pico_gpio_read(gpio_num: int) -> str:
    """Read a Pico (RP2040) GPIO's current input level, over SWD.

    Passive (one register read, `SIO_GPIO_IN`) -- does not reconfigure the
    pin, safe on any pin including GPIO6 and pins currently owned by
    firmware. This is the one pico_gpio_* call allowed on GPIO6.
    """
    try:
        level = pico_gpio_probe.read(gpio_num)
    except RuntimeError as exc:
        return f"error: {exc}"
    return f"pico gpio{gpio_num} = {'high' if level else 'low'}"


@_core._tool()
def pico_gpio_read_all() -> str:
    """Read every Pico GPIO this connection has configured via
    pico_gpio_set_mode, in this process. There is no firmware-side tracking
    table on the Pico (unlike the ESP probe) -- this reflects only what this
    PC session has configured since it started."""
    try:
        pins = pico_gpio_probe.read_all()
    except RuntimeError as exc:
        return f"error: {exc}"
    if not pins:
        return "no pins configured yet (call pico_gpio_set_mode first)"
    return "\n".join(f"gpio{n}: {mode} = {'high' if level else 'low'}" for n, mode, level in pins)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
