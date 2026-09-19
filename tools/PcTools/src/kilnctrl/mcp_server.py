#!/usr/bin/env python3
"""MCP server exposing the KilnCtrl board's UART control link as tools.

Speaks streamable HTTP by default (``--transport stdio`` is still available for
headless runs) and publishes a five-tool search facade rather than all 133 tools
below -- see ``mcpkit/serve.py`` and ``mcpkit/registry.py`` for both decisions,
and ``docs/MCP_SERVERS.md`` for how the servers get started and stopped.


Wraps a single process-wide :class:`~kilnctrl.serial_link.UartLink` so an
agent can discover the board's USB-UART bridge port, connect, and drive the
kiln controller's hardware over the hardened UART protocol:

* **THERMO** (task 1) -- three MAX31856 thermocouple channels on J6
* **IO** (task 2) -- the SX1509: four relays, seven digital I/O, DRDY inputs
* **DISPLAY** (task 4) -- the ILI9488 480x320 TFT on J2
* **SAFETY** (task 7) -- the opto-isolated link to the RP2040 safety processor
* **INFO/LOG/SYSTEM** (3/5/6) -- pin config, firmware version, console, recovery
* **CONTROL/PROFILES/AUTOTUNE** (8/9/10) -- zone PID/model config, fire
  profile CRUD + execution (start/stop/pause/resume a firing), PID autotune
* **WIFI** (11) -- status/scan/provision/forget, works even with Wi-Fi down
* **GPIO_PROBE** (12) -- raw ESP32 pin control, only on a firmware built with
  `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` (default off)

Also exposes ``flash_firmware()``/``kill_openocd_sessions()`` -- unrelated to
the UART link above, these drive OpenOCD directly over JTAG to (re)flash the
board. See that section's own comment for why they exist: use them instead
of improvising raw ``openocd`` commands by hand.

Alongside those, a generic ``debug_*`` tool family (``debug_program``,
``debug_reset``, ``debug_halt``, ``debug_resume``, ``debug_step``,
``debug_read_memory``, ``debug_write_memory``, ``debug_read_registers``, plus
``set_openocd_path``/``get_openocd_status``) covers program/reset/halt/step/
memory/register access for **both** processors on this board -- the ESP32-S3
main controller over JTAG and the RP2040 safety processor (A1,
``firmware/SaftyFW/``) over SWD via a CMSIS-DAP debug probe -- through one
shared OpenOCD wrapper (``openocd_util.py``/``debug_probe.py``). See
``tools/PcTools/TODO.md``'s "One substrate covers both: OpenOCD" section for
the design rationale and guard rails.

Every *command* tool returns the SendResult as text:
  ``ok`` / ``undeliverable`` / ``timeout`` / ``not_connected``.

Important limitation: ``ok`` only means the frame was delivered to the ESP
task's inbox. uart_bridge.c ACKs at the protocol layer and never sends an
application-level status frame back for a command, so there is no way from the
PC to learn whether e.g. the I2C write to the SX1509 actually succeeded. The
*query* tools (thermo_read, io_read, safety_get_status,
get_pin_config, get_fw_version) are the exception -- those return real device
data.
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

try:
    # mcp >= 2.0 renamed FastMCP to MCPServer (same decorator/run API).
    from mcp.server.mcpserver import MCPServer as _McpServer
except ImportError:  # pragma: no cover - mcp 1.x
    from mcp.server.fastmcp import FastMCP as _McpServer

from mcpkit import workbench
from mcpkit.registry import check_staleness, collapse
from mcpkit.serve import serve

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

log = logging.getLogger(__name__)

mcp = _McpServer("kilnctrl")

#: Shared with any other pc_tools process (the GUI, another MCP server)
#: already running -- see link_hub.py. Whichever process asks first owns the
#: physical port; this one becomes a client of it either way. The protocol
#: allows only one outstanding send-and-await-ack cycle anyway (serialized
#: inside the hub's own UartLink).
_link = get_shared_link()

#: File-based session logging, same rollover policy as the GUI (new file on
#: connect, and on a detected device reboot) -- see session_log.py. This is
#: separate from the stderr logging set up in main(): that's for this
#: process's own diagnostics, this is a persistent per-session record of
#: what was sent to the device, matching what the GUI keeps.
_session_log = SessionLogger(name="kilnctrl.mcp_session")
# Also persist kilnctrl's own module-level warnings (serial read failures,
# link-hub connection loss) into the session file, matching the GUI. main()'s
# stderr logging only reaches whoever is watching the process's console, which
# for an MCP server launched by a client is nobody.
_session_log.capture_package_logs()


def _on_boot_push(version) -> None:  # devices.FirmwareVersion, avoid an import cycle in the annotation
    """Unsolicited version push = the device just (re)booted -- roll the log."""
    _session_log.info("device reboot detected (unsolicited FW version push)")
    _session_log.start_session(f"device reboot - {version.describe()}")
    if not version.compatible:
        log.error(
            "UART protocol version mismatch: device v%d, pc_tools v%d -- "
            "device commands will be refused until this is resolved",
            version.protocol_version,
            devices.UART_PROTOCOL_VERSION,
        )


#: Owns task INFO (3) so query replies -- and the firmware's boot version push
#: -- have somewhere to land. Registered at import, before any connect, for the
#: same reason the GUI does it early: a push that arrives with no registered
#: task is NACKed and lost.
_info = InfoClient(_link, on_boot_push=_on_boot_push)

#: Owns task SYSTEM (6) so GET_WATCHDOG_PANIC_DISABLED query replies have
#: somewhere to land. Registered at import for the same reason as _info.
_system = SystemClient(_link)

#: Recent firmware log lines (task LOG), for the get_device_log tool below --
#: an MCP client has no GUI terminal to watch live, so this is the pull-based
#: equivalent of the GUI's Device Console window. Each entry is
#: (pc_arrival_unix_time, LogLine): the device has no RTC the PC trusts, so
#: PC arrival is the only common clock, same reasoning as SAFETY_LINK.md's
#: "age" field -- see get_device_log_json below.
#: Byte-budgeted rather than count-limited: a burst of short lines (the
#: wifi-init/uart_owner spam this board produces at boot) used to evict a
#: count-capped buffer's useful entries (e.g. NS2009 touch diagnostics)
#: within a couple of seconds. 1 MiB of line text holds tens of thousands of
#: lines even during a spam burst, comfortably covering a multi-minute
#: diagnostic session.
_DEVICE_LOG_MAX_BYTES = 1024 * 1024
_device_log_history: "deque[tuple[float, LogLine]]" = deque()
_device_log_bytes = 0

#: Guards the three history buffers below. They are written from the device
#: clients' consumer threads and read from whichever thread is serving a tool
#: call, so every read takes a snapshot under this lock rather than iterating
#: a deque another thread may be appending to.
_history_lock = threading.Lock()


def _snapshot(buffer: deque, n: int) -> list:
    """Last ``n`` entries of a push buffer, copied under the lock."""
    with _history_lock:
        return list(buffer)[-n:]


_DEVICE_LOG_SESSION_METHOD = {
    LogLevel.ERROR: _session_log.error,
    LogLevel.WARN: _session_log.warning,
}


def _on_device_log_line(line: LogLine) -> None:
    """LogClient's consumer thread: buffer for get_device_log, and mirror
    into the persistent session log so it's not lost between MCP calls."""
    global _device_log_bytes
    entry_bytes = len(line.text.encode("utf-8", errors="replace"))
    with _history_lock:
        _device_log_history.append((time.time(), line))
        _device_log_bytes += entry_bytes
        while _device_log_bytes > _DEVICE_LOG_MAX_BYTES and len(_device_log_history) > 1:
            _, evicted = _device_log_history.popleft()
            _device_log_bytes -= len(evicted.text.encode("utf-8", errors="replace"))
    method = _DEVICE_LOG_SESSION_METHOD.get(line.level, _session_log.info)
    method("device: %s", line.text)


#: Owns task LOG (5) so the firmware's forwarded ESP_LOGx output (see
#: App/drivers/bridge/uart_log_bridge.c) has somewhere to land -- registered up
#: front for the same "don't miss the boot-time backlog" reason as _info.
_device_log = LogClient(_link, on_line=_on_device_log_line)

#: Most recent auto-report pushes, so an agent that turned reporting on can
#: read the stream back without issuing a query per sample. Same pull-based
#: shape as the device log above: an MCP client has nowhere to receive a push.
_THERMO_REPORT_HISTORY = 50
_thermo_reports: "deque[list]" = deque(maxlen=_THERMO_REPORT_HISTORY)
_io_reports: "deque[object]" = deque(maxlen=_THERMO_REPORT_HISTORY)

def _record_report(buffer: deque):
    """Auto-report push sink: append under _history_lock (these callbacks
    run on the clients' consumer threads, not on the tool-call thread)."""

    def append(value) -> None:
        with _history_lock:
            buffer.append(value)

    return append


#: Owns tasks THERMO (1), IO (2), DISPLAY (4) and SAFETY (7) so their query
#: replies -- and the two auto-report pushes -- have somewhere to land.
#: Registered up front like _info and _device_log: a push that arrives before
#: the task is registered is NACKed and lost.
_thermo = ThermoClient(_link, on_report=_record_report(_thermo_reports))
_io = IoClient(_link, on_report=_record_report(_io_reports))
_display = DisplayClient(_link)
#: Task 13 -- the NS2009 touch controller on the same J2 panel, plus the
#: screen_idle auto-blank state machine it feeds. touch_inject is the tool
#: that lets an MCP caller send synthetic touches, indistinguishable from a
#: real press to the firmware's idle timer and wake logic.
_touch = TouchClient(_link)
#: Task 14 -- LCD UI regression-test probe (current page / tap targets /
#: click-by-name). See ui_test_client.py; the web half of the same
#: framework (WebUiClient) needs no persistent client, just a base_url.
_ui_test = UiTestClient(_link)
_safety = SafetyClient(_link)
#: Task 12 -- only answered on a firmware built with
#: CONFIG_KILNCTL_ENABLE_GPIO_PROBE (default off). Registered unconditionally
#: like every other client here; on a build without the option every call
#: just times out, which the tools below report plainly rather than as a
#: crash.
_probe = ProbeClient(_link)
#: Task 11 -- status/scan/provision/forget. The GUI (gui.py) has driven this
#: since it was added; it was never wired into the MCP server, which left an
#: agent with no way to configure Wi-Fi at all except the HTTP endpoints
#: (which need Wi-Fi already up to reach). Closing exactly the kind of gap
#: capability 3 in tools/PcTools/TODO.md exists to find.
_wifi = WifiUartClient(_link)
#: Tasks 8/9/10 -- zone PID/model config, fire profile CRUD + execution
#: control, and PID autotune. Same gap as _wifi above: the GUI has driven all
#: three since they were added (control.py/profiles.py/autotune.py), none
#: were ever wired into the MCP server. profiles.start()/stop() are the ones
#: that actually begin/end a firing -- exposing them here is not a new,
#: less-governed path to do that: every relay command downstream of a running
#: profile still goes through relay_authority_on_blocked() exactly as it does
#: from the HTTP dashboard or the GUI (tools/PcTools/TODO.md "What this does
#: not become").
_control = ControlClient(_link)
_profiles = ProfilesClient(_link)
_autotune = AutotuneClient(_link)

#: Backs the generic press_button/list_buttons tools (actions.py) with the
#: same link/info/clients this module's own bespoke tools use, so both paths
#: hit the identical underlying state.
_action_ctx = actions.ActionContext(
    link=_link,
    info=_info,
    session_log=_session_log,
    thermo=_thermo,
    io=_io,
    display=_display,
    safety=_safety,
    system=_system,
)


#: Seconds a staleness verdict is reused before re-stat'ing the snapshot's
#: files. This runs on EVERY tool call (see _tool()'s wrapper below), so it
#: must stay cheap; 30s keeps the per-call cost effectively zero (a cache
#: hit is a dict lookup and a time.time() call) while still surfacing a
#: fresh edit within one interactive round-trip window -- staleness that
#: matters here is measured in days (docs/audits/
#: stale_mcp_server_window_recheck_2026-09-14.md: three days unrestarted),
#: not seconds, so nothing is lost by not re-checking on every single call.
_FRESHNESS_CACHE_INTERVAL_S = 30.0
#: checked_at/banner: cache of the last computed banner text ("" when
#: fresh). Module-level and unlocked -- a torn read under concurrent tool
#: calls means at worst one extra recompute or one call sees a banner one
#: interval late, never a crash or a wrong-server verdict, since
#: check_staleness() itself is pure and idempotent.
_freshness_cache: "dict[str, Any]" = {"checked_at": 0.0, "banner": ""}


def _stale_banner() -> str:
    """Cheap, cached per-tool-call staleness banner.

    Background: this server ran unrestarted for three days on a stale
    commit while ``kiln_help()`` and ``mcp_servers.ps1 status`` both already
    had the fresh/stale answer available from ``/health`` -- nobody
    consulted either, and every affected tool call (including
    ``control_get_zones``) went out with no indication anything might be
    stale (docs/audits/stale_mcp_server_window_recheck_2026-09-14.md,
    recommendation: an inline per-tool-call banner). This reuses the exact
    same ``mcpkit.registry.SourceSnapshot``/``check_staleness()`` machinery
    that backs those two -- not a second, competing notion of staleness.

    Deliberately does NOT auto-restart: a restart near an active firing is
    exactly what this project avoids, and only a human (or a tool that
    knows a firing is not active) should trigger one.

    Silent when fresh, on purpose: a banner that prints unconditionally
    becomes ignorable noise, which is the same failure mode being fixed
    here just moved into the tool output instead of a status command
    nobody runs. Loud (impossible to miss inline) only when actually stale.
    """
    snapshot = getattr(registry, "freshness", None) if "registry" in globals() else None
    if snapshot is None:
        return ""
    now = time.time()
    if now - _freshness_cache["checked_at"] < _FRESHNESS_CACHE_INTERVAL_S:
        return _freshness_cache["banner"]
    stale, changed = check_staleness(snapshot)
    if stale:
        plural = "" if changed == 1 else "s"
        banner = (
            f"\n\n[STALE MCP SERVER] {changed} file{plural} changed on disk "
            f"since this process started serving (commit {snapshot.commit}, "
            f"started {snapshot.started_at_human()}) -- this result may not "
            "reflect current source. Restart when no firing is active: "
            ".\\tools\\PcTools\\scripts\\mcp_servers.ps1 restart"
        )
    else:
        banner = ""
    _freshness_cache["checked_at"] = now
    _freshness_cache["banner"] = banner
    return banner


def _tool():
    """The ``mcp.tool()`` registration plus a blanket "never raise" guard.

    Every tool below builds its payload by calling a ``devices.py`` builder in
    its own argument list -- ``_send(TASK, devices.display_fill_rect(...))`` --
    so the builder's validation runs *before* ``_send`` is entered and its
    ValueError propagates straight out of the tool function. The tool contract
    here is that a bad argument comes back as an ``error: ...`` string the
    caller can read and correct, not as an exception the transport has to
    render; a model passing a negative width should be told what was wrong
    with it, not handed a traceback.

    The guard also catches the four query clients' errors and anything else
    unexpected, so a malformed reply or a link fault can never surface as a
    raised exception either.
    """
    register = mcp.tool()

    def decorate(fn):
        @functools.wraps(fn)  # keeps the signature/docstring FastMCP builds the schema from
        def wrapper(*args, **kwargs):
            try:
                result = fn(*args, **kwargs)
            except (
                ThermoQueryError,
                IoQueryError,
                DisplayQueryError,
                TouchQueryError,
                SafetyQueryError,
                InfoQueryError,
                SystemQueryError,
                BlitError,
            ) as exc:
                result = f"error: {exc}"
            except (ValueError, TypeError, OSError) as exc:
                _session_log.error("%s: rejected: %s", fn.__name__, exc)
                result = f"error: {exc}"
            except Exception as exc:  # noqa: BLE001 - a tool must always answer
                log.exception("unexpected error in tool %s", fn.__name__)
                result = f"error: unexpected {type(exc).__name__}: {exc}"
            # Applies to every tool through this one shared wrapper -- see
            # _stale_banner()'s docstring for why that is deliberate rather
            # than picked per-tool: every tool's result is produced by this
            # server's own Python (formatting/validation/error-handling
            # alone, even for tools that are mostly a passthrough), so any
            # of them can be affected by code that changed since this
            # process started. A cheap, cached, silent-when-fresh check at
            # the one choke point every tool already passes through beats
            # auditing which specific tools "depend on server code" (most
            # of them do, and that classification would itself go stale).
            if isinstance(result, str):
                banner = _stale_banner()
                if banner:
                    result = result + banner
            return result

        return register(wrapper)

    return decorate


def _send(dst_task: int, payload: bytes) -> str:
    """Send a command payload, using the same numeric id as our src_task.

    The PC mirrors the firmware's task numbering (uart_task_ids.h), so the
    thermocouples are task 1 on both sides and the expander is task 2.

    Refuses to send if the connected firmware's protocol version doesn't
    match ours (or isn't known yet) -- see UART_PROTOCOL_VERSION in
    protocol.py. This matters more on this board than it looks: a v1
    (unit-test fixture) firmware would happily accept task 1 traffic and
    interpret it as MCP4728 DAC commands. INFO queries themselves are exempt
    (see get_pin_config / get_fw_version below): that's how compatibility
    gets discovered.
    """
    if _info.compatible is not True:
        reason = (
            "protocol version mismatch"
            if _info.compatible is False
            else "firmware version not yet confirmed (call get_fw_version first)"
        )
        _session_log.error("refused send to task %d: %s", dst_task, reason)
        return f"error: refused - {reason}"
    try:
        result = _link.send(dst_task=dst_task, src_task=dst_task, payload=payload)
    except ValueError as exc:
        _session_log.error("send to task %d failed: %s", dst_task, exc)
        return f"error: {exc}"
    _session_log.info("send to task %d -> %s", dst_task, result.value)
    return f"{result.value} - {result.describe()}"



# ---------------------------------------------------------------------------
# The rest of this module used to live here inline. It is now split into
# per-tool-group submodules (pure refactor, zero behavior change) -- each
# import below both runs that group's @_tool() registrations against the
# shared `mcp` server object and re-exports its public names, so
# `kilnctrl.mcp_server.autotune_start` (etc) and every existing import site
# keep working unchanged.
# ---------------------------------------------------------------------------
from .mcp_server_link import *  # noqa: F401,F403
from .mcp_server_flash import *  # noqa: F401,F403
from .mcp_server_debug import *  # noqa: F401,F403
from .mcp_server_saleae import *  # noqa: F401,F403
from .mcp_server_thermo import *  # noqa: F401,F403
from .mcp_server_io import *  # noqa: F401,F403
from .mcp_server_touch import *  # noqa: F401,F403
from .mcp_server_safety import *  # noqa: F401,F403
from .mcp_server_gpio_probe import *  # noqa: F401,F403
from .mcp_server_pico_gpio_probe import *  # noqa: F401,F403
from .mcp_server_wifi import *  # noqa: F401,F403
from .mcp_server_ota import *  # noqa: F401,F403
from .mcp_server_control import *  # noqa: F401,F403
from .mcp_server_config_presets import *  # noqa: F401,F403
from .mcp_server_capability_preflight import *  # noqa: F401,F403
from .mcp_server_ui_test import *  # noqa: F401,F403
from .mcp_server_profiles import *  # noqa: F401,F403
from .mcp_server_autotune import *  # noqa: F401,F403
from .mcp_server_adaptive_tune import *  # noqa: F401,F403
from .mcp_server_ramp_assist import *  # noqa: F401,F403
from .mcp_server_zones_current_sweep import *  # noqa: F401,F403
from .mcp_server_codec import *  # noqa: F401,F403
from .mcp_server_info import *  # noqa: F401,F403
from .mcp_server_actions import *  # noqa: F401,F403
from .mcp_server_log_analysis import *  # noqa: F401,F403
from .mcp_server_repo_grep import *  # noqa: F401,F403
from .mcp_server_page_structure import *  # noqa: F401,F403
from .mcp_server_plant_sim import *  # noqa: F401,F403
from .mcp_server_coupled_ident import *  # noqa: F401,F403
from .mcp_server_fixture import _close_fixture  # noqa: F401
from .mcp_server_fixture import *  # noqa: F401,F403
from .mcp_server_bench_test import *  # noqa: F401,F403

# ---------------------------------------------------------------------------
# facade + entry point
# ---------------------------------------------------------------------------
workbench.attach(_tool, ("dut", "common"))

#: Registered last, once every ``@_tool()`` above has run. From here on the
#: wire carries ``kiln_help`` / ``kiln_find`` / ``kiln_describe`` /
#: ``kiln_call`` / ``kiln_batch`` (plus ``connect``); everything else in this
#: module stays a plain importable function and stays reachable through
#: ``kiln_call``. See ``mcpkit/registry.py`` for why 200 published schemas was
#: not a workable default.
registry = collapse(
    mcp,
    prefix=mcp_facade.PREFIX,
    label=mcp_facade.LABEL,
    title=mcp_facade.TITLE,
    group_prefixes=mcp_facade.GROUP_PREFIXES,
    group_overrides=mcp_facade.GROUP_OVERRIDES,
    keywords=mcp_facade.KEYWORDS,
    synonyms=mcp_facade.SYNONYMS,
    keep=mcp_facade.KEEP,
    recipes=mcp_facade.RECIPES,
    # tools/PcTools/src -- covers this package and mcpkit itself, so an edit
    # to either shows up as staleness. See mcpkit/registry.py's "staleness /
    # freshness" section: kiln_help() surfaces this once at startup.
    source_root=os.path.normpath(os.path.join(os.path.dirname(__file__), "..")),
)


def _close() -> None:
    """Shut the query clients down and let go of the shared link."""
    for client in (_info, _system, _device_log, _thermo, _io, _display, _touch, _safety, _probe, _wifi,
                   _control, _profiles, _autotune):
        client.close()
    # close(), not disconnect(): this link may be shared with another process
    # (see link_hub.py) -- exiting shouldn't yank the physical port out from
    # under it. The disconnect MCP tool is the only thing that should do that.
    _link.close()
    _close_fixture()  # separate board/port -- own client, not in the tuple above
    _session_log.close()


def main() -> int:
    """Sync entry point for the ``kilnctrl-mcp-server`` console script."""
    return serve(mcp, name="kilnctrl", default_port=mcp_facade.DEFAULT_PORT, on_close=_close,
                freshness=registry.freshness)


if __name__ == "__main__":
    raise SystemExit(main())
