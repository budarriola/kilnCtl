"""ESP GPIO_PROBE tools (task 12).

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
# GPIO_PROBE -- raw ESP32-S3 pin control (task 12)
#
# Only answered on a firmware built with CONFIG_KILNCTL_ENABLE_GPIO_PROBE
# (default off). Exists to answer "is this net actually where the schematic
# says" without a one-off firmware -- tools/PcTools/TODO.md capability 1.
# The firmware enforces its own deny-list (SPI/I2C/SX1509/display/PC-link
# pins plus the isolated fault line GPIO6, but NOT the safety-link UART data
# pins) and refuses writes while a profile is running or paused;
# this layer only surfaces the refusal reason, it does not re-implement the
# policy.
# ---------------------------------------------------------------------------
@_core._tool()
def gpio_probe_set_mode(gpio_num: int, mode: str) -> str:
    """Configure an ESP32-S3 GPIO as input / input_pullup / input_pulldown / output.

    ``mode`` is one of "input", "input_pullup", "input_pulldown", "output".
    Refused for any pin on the firmware's deny-list (SPI, I2C, the SX1509
    IRQ/RESET pins, the display CS, the PC-link UART pins, and the isolated
    fault line GPIO6) and while a profile is running or paused. The
    safety-link UART data pins (SAFETY_TX_IO/SAFETY_RX_IO) are deliberately
    NOT denied -- the coordinated two-board GPIO test needs to drive them;
    see gpio_probe.c. Requires a firmware built with CONFIG_KILNCTL_ENABLE_GPIO_PROBE
    (default off) -- a timeout here most likely means that option is not set.
    """
    mode_map = {
        "input": probe.MODE_INPUT,
        "input_pullup": probe.MODE_INPUT_PULLUP,
        "input_pulldown": probe.MODE_INPUT_PULLDOWN,
        "output": probe.MODE_OUTPUT,
    }
    mode_val = mode_map.get(mode.strip().lower())
    if mode_val is None:
        return f"error: mode must be one of {sorted(mode_map)}, got {mode!r}"
    try:
        _srv._probe.set_mode(gpio_num, mode_val)
    except devices.GpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ProbeQueryError as exc:
        return f"error: {exc}"
    return f"ok - gpio{gpio_num} set to {mode}"


@_core._tool()
def gpio_probe_write(gpio_num: int, level: bool) -> str:
    """Drive an ESP32-S3 GPIO high or low.

    Refused unless gpio_num was already configured OUTPUT via
    :func:`gpio_probe_set_mode`, is deny-listed, or a profile is running or
    paused.
    """
    try:
        _srv._probe.write(gpio_num, level)
    except devices.GpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ProbeQueryError as exc:
        return f"error: {exc}"
    return f"ok - gpio{gpio_num} = {'high' if level else 'low'}"


@_core._tool()
def gpio_probe_read(gpio_num: int) -> str:
    """Read an ESP32-S3 GPIO's current level.

    Does not reconfigure the pin -- safe to call on a pin already owned by
    another peripheral, though deny-listed pins are still refused outright.
    """
    try:
        level = _srv._probe.read(gpio_num)
    except devices.GpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ProbeQueryError as exc:
        return f"error: {exc}"
    return f"gpio{gpio_num} = {'high' if level else 'low'}"


@_core._tool()
def gpio_probe_read_all() -> str:
    """Read every GPIO this connection has configured via gpio_probe_set_mode.

    Cheap and side-effect-free -- the recommended way to check whether the
    probe capability exists on this firmware at all: a prompt reply (even
    "no pins configured yet") means the option is built in, while a timeout
    means the board almost certainly was not built with
    CONFIG_KILNCTL_ENABLE_GPIO_PROBE.
    """
    try:
        pins = _srv._probe.read_all()
    except ProbeQueryError as exc:
        return f"error: {exc}"
    if not pins:
        return "no pins configured yet (call gpio_probe_set_mode first)"
    return "\n".join(f"gpio{p.gpio_num}: {p.mode_name} = {'high' if p.level else 'low'}" for p in pins)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
