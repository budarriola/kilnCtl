"""INFO tools (task 3).

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
# INFO queries (task 3)
#
# Unlike the command tools these return real device data: INFO is a query
# channel, so the ACK only confirms delivery and the answer arrives in a
# separate DATA frame (see info.py).
# ---------------------------------------------------------------------------
@_srv._tool()
def get_pin_config() -> str:
    """Report which GPIOs the running firmware has wired up, and to what.

    Read live from the device, so it reflects what is actually flashed rather
    than a host-side table. Only real ESP32-S3 GPIOs appear here -- the relay
    drives, DRDY inputs and the display's D/C and ~RESET are expander pins,
    reported by io_read() instead.
    """
    try:
        entries = _srv._info.get_pin_config()
    except InfoQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "device reported no pin entries"
    return "\n".join(f"GPIO{e.gpio}: {e.label} [{e.abbrev}]" for e in entries)


@_srv._tool()
def get_stack_margin() -> str:
    """Report every instrumented task's stack high-water mark, live.

    This is the bench measurement `KilnFW/TODO.md` section 13 requires before
    any of the six internal-only task stacks it names may be resized. The
    figure is `uxTaskGetStackHighWaterMark()`: the SMALLEST free stack ever
    seen since that task started, not the current free amount -- so it is only
    as good as the worst path the task has actually taken since boot. Exercise
    a task's heavy path first (a big POST, an OTA, a config commit), then read
    this; a number taken from an idle board understates every stack.

    A task shown as "not running" was never created or has been deleted, and
    reports 0 rather than a stale earlier reading.
    """
    try:
        entries = _srv._info.get_stack_margin()
    except InfoQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "device reported no instrumented tasks"
    lines = []
    for e in entries:
        if not e.alive:
            lines.append(f"{e.name}: not running (configured {e.configured_stack_bytes} B)")
            continue
        pct = e.headroom_pct
        pct_txt = f"{pct:.1f}%" if pct is not None else "n/a"
        lines.append(
            f"{e.name}: {e.hwm_bytes} B free at worst of {e.configured_stack_bytes} B "
            f"({pct_txt} headroom) [{e.level.name}]"
        )
    return "\n".join(lines)


@_srv._tool()
def get_fw_version() -> str:
    """Report the running firmware's git commit, dirty flag, build time, and
    whether its UART protocol version matches this copy of pc_tools.

    Call this before any device tool: they all refuse to send until a
    compatible version has been confirmed this way (or via a boot push). This
    gate matters here -- a v1 firmware is the unit-test fixture, where task 1
    is a DAC rather than three thermocouples.
    """
    try:
        version = _srv._info.get_fw_version()
    except InfoQueryError as exc:
        return f"error: {exc}"
    compat = (
        "yes"
        if version.compatible
        else f"NO - device speaks v{version.protocol_version}, pc_tools speaks "
        f"v{devices.UART_PROTOCOL_VERSION}; device commands will be refused"
    )
    return (
        f"protocol_version: {version.protocol_version}\n"
        f"compatible: {compat}\n"
        f"commit: {version.commit}\n"
        f"tree: {'dirty' if version.dirty else 'clean'}\n"
        f"built: {version.built}"
    )


