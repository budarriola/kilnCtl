"""Saleae Logic 2 capture.

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
# Saleae Logic 2 -- independent wire-level capture, not the UART link above
#
# logic_capture.py wraps the Logic 2 automation gRPC API (127.0.0.1:10430,
# Preferences -> Enable automation server). Logic 2 also exposes its own MCP
# server on 127.0.0.1:10530 (registered as "saleae" in .mcp.json) for
# interactive capture -- use these instead when a capture has to be scripted
# alongside driving the board through the tools above, since the timing
# between "arm" and "start talking to the DUT" then lives in one process.
#
# tools/PcTools/TODO.md capability 2 also wants captures decoded as
# kilnlink frames, not raw transitions -- that decode step is NOT done here.
# Building it blind (no capture ever taken this session -- no Saleae
# hardware attached) risks a decoder nobody has run against a real capture,
# so this wraps arm/capture/list only; decode is left as an explicit
# follow-on for whoever has a board and a Logic analyzer both on the bench.
# ---------------------------------------------------------------------------
@_srv._tool()
def saleae_list_devices() -> str:
    """List Saleae devices Logic 2's automation server can see.

    Requires Logic 2 running with Preferences -> Enable automation server on;
    a clean "not running" error here (rather than a raw gRPC failure) means
    exactly that, not a hardware problem.
    """
    from . import logic_capture

    try:
        devs = logic_capture.list_devices()
    except logic_capture.LogicNotRunning as exc:
        return f"error: {exc}"
    if not devs:
        return "no Saleae devices connected"
    return "\n".join(
        f"{d.device_id}: {d.device_type}{' (sim)' if d.is_simulation else ''}" for d in devs
    )


@_srv._tool()
def saleae_capture(
    channels: str = "0,1", duration_seconds: float = 1.0,
    sample_rate: int = 25_000_000,
    out_dir: str = "logs/saleae", filename: str = "capture.sal",
) -> str:
    """Arm a timed digital capture on the given channels and save it as a
    ``.sal`` file (open in Logic 2 to view/export).

    ``channels`` is comma-separated digital channel numbers. ``sample_rate``
    must be one of the values reported for this channel count -- an
    unsupported rate comes back as a firmware-reported error listing the
    legal set, not a bare rejection.
    """
    from . import logic_capture

    try:
        chans = [int(c) for c in channels.split(",") if c.strip()]
    except ValueError:
        return f"error: could not parse channels {channels!r} as comma-separated integers"
    if not chans:
        return "error: no channels given"
    try:
        path = logic_capture.capture(
            digital_channels=chans, duration_seconds=duration_seconds,
            output_dir=out_dir, sample_rate=sample_rate, filename=filename,
        )
    except logic_capture.LogicNotRunning as exc:
        return f"error: {exc}"
    except Exception as exc:  # noqa: BLE001 - surface the automation API's own error text
        return f"error: capture failed: {exc}"
    return f"ok - saved {path}"


