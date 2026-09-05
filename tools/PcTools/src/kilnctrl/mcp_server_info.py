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

from . import actions, config_presets, dashboard_http_client, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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
def get_heap_status(host: Optional[str] = None) -> str:
    """Report internal-DRAM, PSRAM, and DMA-capable-internal-memory heap
    figures, live, over HTTP GET /api/status.

    DRAM_PSRAM_PLAN.md Phase 0 (4.1): the one MCP-side gap that plan's
    section identified -- dashboard_http.c has served heap_internal/
    heap_spiram/heap_dma in every /api/status response since before this
    tool existed, but nothing under tools/PcTools parsed those keys, so the
    data was reachable by curl or a browser and nowhere else. `min_free`
    (the low-water mark since boot) is the number every acceptance criterion
    in that plan is written against -- the instantaneous `free` figure is
    nearly useless, since the exhaustion event this plan exists to catch is
    transient and load-dependent. `heap_dma` is a STRICT SUBSET of
    `heap_internal` (both draw from MALLOC_CAP_INTERNAL), not new internal-
    DRAM information -- it answers "how much of that internal memory is
    DMA-capable", which matters once Phase 1 starts lowering
    SPIRAM_MALLOC_ALWAYSINTERNAL.

    Same host-resolution order as every ota_*/adaptive_tune_* tool
    (`_ota_resolve_host`): explicit `host` argument, else the STA IP if
    Wi-Fi reports one connected, else the board's own softAP address. This
    is a single GET per call, not a poll -- call it again for a fresh
    reading rather than looping here.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    resolved = _ota_resolve_host(host)
    try:
        heap = dashboard_http_client.get_heap_status(resolved)
    except dashboard_http_client.DashboardHttpError as exc:
        return f"error: {exc} (host={resolved})"
    lines = [f"host={resolved}"]
    crash = heap.get("unacknowledged_crash")
    if crash:
        lines.append(
            "!!! UNACKNOWLEDGED CRASH REPORT !!! exc_task="
            f"{crash.get('exc_task')!r} exc_cause_str={crash.get('exc_cause_str')!r} "
            f"reset_reason={crash.get('found_on_boot_reset_reason')!r} -- "
            "this board panicked and nobody has reviewed it yet "
            "(GET /api/crash_report). Do not assume this run/board is healthy."
        )
    reset_reason = heap.get("reset_reason")
    if reset_reason in dashboard_http_client.UNCLEAN_RESET_REASONS:
        lines.append(f"!!! reset_reason={reset_reason!r} (unclean boot) uptime_s={heap.get('uptime_s')}")
    else:
        lines.append(f"reset_reason={reset_reason!r} uptime_s={heap.get('uptime_s')}")
    check_err = heap.get("unacknowledged_crash_check_error")
    if check_err:
        lines.append(f"(could not check /api/crash_report: {check_err})")
    for key in ("heap_internal", "heap_spiram", "heap_dma"):
        h = heap[key]
        lines.append(
            f"{key}: free={h['free']} B, largest_free_block={h['largest_free_block']} B, "
            f"min_free={h['min_free']} B (low-water since boot), total={h['total']} B"
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
    lines = [
        f"protocol_version: {version.protocol_version}",
        f"compatible: {compat}",
        f"commit: {version.commit}",
        f"tree: {'dirty' if version.dirty else 'clean'}",
        f"built: {version.built}",
    ]
    # Board-vs-HEAD gap and last-refused-flash checks (added 2026-09-04: a
    # refused flash -- the fix never reached the board -- went unnoticed for
    # five hours because nothing compared what the board reports against
    # what the tree actually has, and the refusal itself left no durable
    # trace. Both checks are best-effort and must never break this tool.
    from . import flash_provenance  # local import: avoids a circular import with mcp_server_flash.py
    try:
        lines.append(flash_provenance.describe_head_gap(version.commit))
    except Exception as exc:  # noqa: BLE001 - this line is a bonus, not the tool's job
        lines.append(f"board/HEAD comparison: error computing it ({exc})")
    try:
        prov = flash_provenance.read_provenance_json(
            os.path.join(debug_probe._kiln_fw_root(), "build", "flash_provenance.json")
        )
        warning = flash_provenance.format_last_flash_warning(prov)
        if warning:
            lines.append(warning)
    except Exception as exc:  # noqa: BLE001 - same, bonus info
        lines.append(f"(could not check last flash outcome: {exc})")
    return "\n".join(lines)


