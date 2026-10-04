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
import pathlib
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
# Saleae Logic 2 -- independent wire-level capture, not the UART link above
#
# logic_capture.py wraps the Logic 2 automation gRPC API (127.0.0.1:10430,
# Preferences -> Enable automation server). Logic 2 also exposes its own MCP
# server on 127.0.0.1:10530 (registered as "saleae" in .mcp.json) for
# interactive capture -- use these instead when a capture has to be scripted
# alongside driving the board through the tools above, since the timing
# between "arm" and "start talking to the DUT" then lives in one process.
#
# tools/PcTools/TODO.md capability 2's decode step landed separately, in
# kilnctrl.kilnlink_capture: it decodes a Saleae Async Serial "Export Table
# Data" CSV (or a raw binary capture) of the isolated ESP<->Pico kilnlink
# UART into a frame timeline -- framing, CRC, device/task, cmd, and payload
# fields, with malformed/partial frames reported at their byte offset
# rather than dropped. Built and tested purely against synthetic fixtures
# (kilnctrl.protocol.Frame + kilnlink_codec's own encoders) since no board +
# Logic analyzer have been on the bench together this session either -- see
# that module's docstring.
# ---------------------------------------------------------------------------
#: Anchored on __file__, not this MCP server process's CWD, which is
#: whatever launched it (not necessarily the repo root) and is not something
#: a tool caller can see or control. logic_capture.capture() does
#: os.makedirs(output_dir, exist_ok=True), so a bare relative "logs/saleae"
#: default never raises: it silently creates a fresh, empty logs/saleae
#: wherever the server happened to start from, and the capture "succeeds"
#: into it -- indistinguishable from a real one until someone goes looking
#: for the file. Computed independently of logic_capture.DEFAULT_OUT_DIR
#: (kept in sync manually) rather than importing it at module load time,
#: since logic_capture pulls in the optional `saleae` automation package
#: that the rest of this module deliberately imports lazily, function-local.
_DEFAULT_SALEAE_OUT_DIR = str(pathlib.Path(__file__).resolve().parents[2] / "logs" / "saleae")
@_core._tool()
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


@_core._tool()
def saleae_capture(
    channels: str = "0,1", duration_seconds: float = 1.0,
    sample_rate: int = 25_000_000,
    out_dir: str = _DEFAULT_SALEAE_OUT_DIR, filename: str = "capture.sal",
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


@_core._tool()
def saleae_decode_kilnlink(path: str, csv: bool = True) -> str:
    """Decode a captured kilnlink UART link (ESP32-S3 <-> RP2040) into a
    readable frame timeline: framing, msg type, device/task, sequence
    number, CRC verdict, and payload fields decoded per frame type where
    the protocol defines one.

    ``path`` is a Saleae Logic 2 Async Serial analyzer "Export Table Data"
    CSV by default (``csv=True``); pass ``csv=False`` for a raw binary
    capture (the concatenated wire bytes). Malformed or partial frames are
    reported inline with their byte offset in the capture, never silently
    dropped -- that is what this tool is for: explaining exactly where and
    how the link's framing broke, including recovering (resyncing) after
    garbage bytes rather than giving up on the rest of the capture.
    """
    from . import kilnlink_capture

    try:
        data = kilnlink_capture.load_saleae_csv(path) if csv else kilnlink_capture.load_bytes(path)
    except (OSError, ValueError) as exc:
        return f"error: could not load capture: {exc}"
    records = kilnlink_capture.decode_capture(data)
    return kilnlink_capture.format_timeline(records)

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
