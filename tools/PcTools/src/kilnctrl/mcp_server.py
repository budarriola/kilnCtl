#!/usr/bin/env python3
"""MCP server exposing the KilnCtrl board's UART control link as tools.

Speaks streamable HTTP by default (``--transport stdio`` is still available for
headless runs) and publishes a five-tool search facade rather than all 135 tools
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
from mcpkit.registry import collapse
from mcpkit.serve import serve

from . import actions, config_presets, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, settings, stale_check, wifi_credentials, zones_http_client
from .autotune import AutotuneClient, AutotuneQueryError
from .control import ControlClient, ControlQueryError
from .device_log import LogClient
from .devices import LogLine
from .display import BlitError, DisplayClient, DisplayQueryError
from .touch import TouchClient, TouchQueryError
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
#: App/drivers/uart_log_bridge.c) has somewhere to land -- registered up
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
                return fn(*args, **kwargs)
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
                return f"error: {exc}"
            except (ValueError, TypeError, OSError) as exc:
                _session_log.error("%s: rejected: %s", fn.__name__, exc)
                return f"error: {exc}"
            except Exception as exc:  # noqa: BLE001 - a tool must always answer
                log.exception("unexpected error in tool %s", fn.__name__)
                return f"error: unexpected {type(exc).__name__}: {exc}"

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
# link management
# ---------------------------------------------------------------------------
@_tool()
def list_serial_ports() -> str:
    """List serial ports with a suitability score for the board's UART bridge.

    The ESP32-S3 exposes this UART on a separate USB-C port from its native
    USB-Serial-JTAG debug port; JTAG-looking ports are scored negatively.
    """
    infos = list_ports()
    if not infos:
        return "no serial ports found"
    lines = []
    for info in infos:
        tag = " (recommended)" if info.recommended else ""
        lines.append(
            f"{info.device}: {info.description or '<no description>'} "
            f"[manufacturer={info.manufacturer or '?'} hwid={info.hwid or '?'} "
            f"score={info.score}]{tag}"
        )
    return "\n".join(lines)


@_tool()
def connect(port: Optional[str] = None) -> str:
    """Open the serial link. Omit `port` to autodiscover the recommended one."""
    if _link.is_connected:
        return f"already connected to {_link.port}"
    try:
        opened = _link.connect(port)
    except Exception as exc:
        _session_log.error("connect to %s failed: %s", port or "<auto>", exc)
        return f"error: {exc}"
    suffix = "" if port else " (autodiscovered)"
    # New log session on every successful connect, same trigger as the GUI;
    # the other trigger (device reboot) is handled in _on_boot_push above.
    _session_log.start_session(f"connected to {opened} @ {_link.baudrate} baud")
    settings.set_last_port(opened)
    return (
        f"connected to {opened} @ {_link.baudrate} baud{suffix}\n"
        + _check_peer_version()
    )


def _check_peer_version() -> str:
    """Ask the newly-connected device its protocol version, and say so plainly.

    A version mismatch has to be refused *at the point of connection*, not
    discovered later by whichever tool happens to be called first: a v1 peer
    is the unit-test fixture, where task 1 is an MCP4728 DAC rather than three
    thermocouples, so every task id means something different. The gate in
    _send() already blocks the traffic; this makes the reason visible in the
    connect result instead of leaving the caller to infer it from a refusal.

    Never raises: a device that doesn't answer at all is reported as unknown,
    which is treated exactly like a mismatch by _send().
    """
    try:
        version = _info.get_fw_version()
    except (InfoQueryError, ValueError, OSError) as exc:
        return (
            f"WARNING: could not read the device's protocol version ({exc}); "
            "device commands will be refused until get_fw_version succeeds"
        )
    if not version.compatible:
        return (
            f"REFUSING DEVICE COMMANDS: this device speaks UART protocol "
            f"v{version.protocol_version}, pc_tools speaks "
            f"v{devices.UART_PROTOCOL_VERSION}. Every task id can mean "
            "something different across a version bump (v1 is the unit-test "
            "fixture, where task 1 is a DAC, not three thermocouples), so "
            "only the INFO queries are allowed. Flash matching firmware or "
            "use a matching pc_tools."
        )
    return f"protocol v{version.protocol_version} matches - {version.describe()}"


@_tool()
def disconnect() -> str:
    """Close the serial link."""
    if not _link.is_connected:
        return "not connected"
    port = _link.port
    _link.disconnect()
    _session_log.info("disconnected from %s", port)
    return f"disconnected from {port}"


@_tool()
def link_status() -> str:
    """Report connection state, port, baud rate and protocol settings."""
    status = _link.status()
    recommended = recommend_port()
    status["recommended_port"] = recommended
    return "\n".join(f"{k}: {v}" for k, v in status.items())


@_tool()
def get_device_log(n: int = 50) -> str:
    """Return the last ``n`` lines of firmware console output (ESP_LOGx).

    These are forwarded over the reliable UART link instead of going to the
    USB-Serial-JTAG console (see App/drivers/uart_log_bridge.c), so they're
    visible here without a debugger/second cable attached -- this is the
    pull-based equivalent of the GUI's Device Console window. It is also the
    only place a *device-level* failure (an SPI or I2C transfer that didn't
    work) ever becomes visible to the PC.
    """
    if n <= 0:
        return "error: n must be positive"
    entries = _snapshot(_device_log_history, n)
    if not entries:
        return "(no device log lines received yet)"
    return "\n".join(f"{line.letter} {line.text}" for _ts, line in entries)


@_tool()
def get_device_log_json(n: int = 50, min_level: str = "info") -> str:
    """Structured form of get_device_log: JSON array of
    {"pc_time": unix seconds, "level": name, "text": ...}, oldest first.

    ``pc_time`` is when this PC process received the line, not a device-side
    timestamp -- the ESP32 has no RTC this tool trusts, so PC arrival is the
    only common clock (same reasoning as the safety link's "age" field).
    ``min_level`` filters to that level and everything more severe (one of
    "error", "warn", "info", "debug", "verbose" -- ESP-IDF's own ordering).
    """
    level_names = {lvl.name.lower(): lvl for lvl in LogLevel}
    min_lvl = level_names.get(min_level.strip().lower())
    if min_lvl is None:
        return f"error: min_level must be one of {sorted(level_names)}, got {min_level!r}"
    if n <= 0:
        return "error: n must be positive"
    entries = _snapshot(_device_log_history, n)
    records = [
        {"pc_time": ts, "level": line.level.name.lower(), "text": line.text}
        for ts, line in entries
        if line.level <= min_lvl
    ]
    return json.dumps(records, indent=2)


@_tool()
def close_server() -> str:
    """Gracefully shut down this MCP server process.

    Closes (not disconnects -- see link_hub.py) this process's own
    connection to the shared UART link, flushes the session log, and exits.
    The physical link stays up for anything else still using it (the GUI,
    another MCP server process): this only ever tears down *this* process.

    The response is sent back before the process actually exits (on a short
    delay, from a background thread) so the caller sees confirmation rather
    than the connection just dropping.

    Since this server is HTTP now, exiting leaves nothing listening on its
    port: every client's kilnctrl tools stop working until it is started
    again. That is the right lever for picking up edited server code, but it
    is not self-healing -- follow it with the editor's "MCP Restart" button,
    or ``tools\\PcTools\\scripts\\mcp_servers.ps1 restart -Server kilnctrl``.
    ``POST /shutdown`` is the same stop without going through a tool call.
    """
    def _shutdown() -> None:
        time.sleep(0.2)  # let the transport flush this tool's reply first
        for client in (_info, _system, _device_log, _thermo, _io, _display, _touch, _safety, _probe, _wifi,
                       _control, _profiles, _autotune):
            client.close()
        _link.close()
        _session_log.close()
        os._exit(0)

    threading.Thread(target=_shutdown, daemon=True, name="mcp-shutdown").start()
    return "shutting down"


@_tool()
def restart_uart() -> str:
    """On-demand recovery lever for a stuck/desynced UART link.

    Tells the ESP to flush its UART RX ring buffer and reset its rx-error
    counter (SYSTEM_CMD_RESTART_UART) -- useful if the link seems wedged,
    without power-cycling the board. Does not disconnect or reopen the local
    serial port; that's what disconnect()/connect() are for.
    """
    return _send(UART_TASK_ID_SYSTEM, devices.system_restart_uart())


@_tool()
def get_watchdog_panic_disabled() -> str:
    """Query whether the ESP task-watchdog's PANIC half is disabled.

    This is a dev-only bench escape hatch (watchdog_cfg.h): when disabled, a
    task that hangs no longer triggers a panic/reboot -- the watchdog still
    monitors and still logs the timeout, the RTC watchdog is completely
    untouched and stays armed regardless, but a hung task now leaves the
    board just sitting there hung, with the relays in whatever state they
    were last commanded, instead of rebooting to clear it. The setting
    persists across reboots (it lives in NVS, not RAM).
    """
    # Same protocol-version gate _send() applies to every non-INFO send, hand
    # rolled here because this query goes through SystemClient rather than
    # _send(). Only INFO is exempt from the gate (that is how compatibility is
    # discovered at all); SYSTEM is not, and a v1 unit-test-fixture firmware
    # would happily accept task-6 traffic and mean something else entirely by
    # it -- see _send()'s docstring for that exact hazard.
    if _info.compatible is not True:
        reason = (
            "protocol version mismatch"
            if _info.compatible is False
            else "firmware version not yet confirmed (call get_fw_version first)"
        )
        _session_log.error("refused SYSTEM watchdog query: %s", reason)
        return f"error: refused - {reason}"
    try:
        disabled = _system.get_watchdog_panic_disabled()
    except SystemQueryError as exc:
        return f"error: {exc}"
    return f"watchdog panic disabled: {disabled}"


@_tool()
def set_watchdog_panic_disabled(disabled: bool) -> str:
    """Enable/disable the ESP task-watchdog's PANIC half.

    Development-only setting -- never leave this disabled on a board that
    will actually fire a kiln. With it disabled, a hung task no longer
    reboots the board; it just sits hung with the relays in whatever state
    they were last commanded. The watchdog's monitoring and logging keep
    running either way, and the RTC watchdog is untouched and stays armed
    regardless. The firmware will warn again before any firing is started
    while this is off (see WATCHDOG_CFG_FIRING_WARNING). Takes effect
    immediately and persists across reboots; no reply frame is sent for this
    command -- poll get_watchdog_panic_disabled() afterward to confirm the
    applied value.
    """
    return _send(UART_TASK_ID_SYSTEM, devices.system_set_watchdog_panic_disabled(disabled))


# ---------------------------------------------------------------------------
# Firmware flashing (JTAG/OpenOCD only -- CLAUDE.md forbids esptool/idf.py
# flash for this board). Added 2026-08-11 after a debugging session where an
# ad-hoc `flash erase_sector 0 0 last` (meant to clear one region) wiped the
# *entire* chip including the bootloader and partition table, which were then
# only partially reflashed by hand -- the board couldn't boot until a full
# power cycle and a from-scratch 3-image reflash recovered it. This tool
# exists so nobody (agent or human) has to improvise that sequence from
# memory again: it always writes all three images in the one safe order and
# never runs a bare full-chip erase.
# ---------------------------------------------------------------------------
def _find_openocd_exe() -> Optional[str]:
    """Thin wrapper -- see openocd_util._find_openocd_exe() for resolution order
    (settings.json override -> OPENOCD_EXE env var -> autodetect globs)."""
    return openocd_util._find_openocd_exe()


def _kiln_fw_root() -> str:
    """firmware/KilnFW/ project root. See debug_probe._kiln_fw_root() -- kept
    as a thin wrapper here since flash_firmware() below already refers to it
    by this name."""
    return debug_probe._kiln_fw_root()


def _run_openocd(openocd_exe: str, board_cfg_relpath: str, tcl_commands: str, cwd: str, timeout_s: int) -> tuple[bool, str]:
    """Thin wrapper over openocd_util.run_openocd() for a single cfg file,
    preserving flash_firmware()'s existing call signature."""
    return openocd_util.run_openocd(openocd_exe, [board_cfg_relpath], tcl_commands, cwd, timeout_s)


@_tool()
def kill_openocd_sessions() -> str:
    """Force-kills any running openocd.exe processes on this PC.

    Use before flash_firmware() (it already does this itself) or on its own
    if a manual/GDB OpenOCD session was left attached and is holding the
    JTAG interface -- a second openocd instance can't open the USB-JTAG
    device while one is already running, which looks like "the board isn't
    responding" but is actually just this. Safe to call when nothing is
    running (reports that plainly, not an error)."""
    return openocd_util.kill_openocd_sessions_impl()


@_tool()
def flash_firmware(board_cfg: str = "board/esp32s3-builtin.cfg", retry_once: bool = True, allow_stale: bool = False) -> str:
    """Flashes KilnFW/build/{bootloader,partition_table,KilnCtrl}.bin to the
    board over JTAG via OpenOCD -- the ONLY sanctioned way to flash this
    board (never esptool/`idf.py flash`, per CLAUDE.md). Always writes all
    three images (bootloader @0x0, partition table @0x8000, app @0x810000),
    each with `program_esp ... verify` (which only erases/rewrites a region
    if its content doesn't already match -- it does NOT do a bare full-chip
    erase), ending in a reset so the board boots the new app immediately.

    Requires `idf.py build` to have already produced KilnFW/build/*.bin --
    this tool does not build, only flashes.

    Before flashing, refuses if KilnCtrl.bin looks stale relative to the
    current source tree (see stale_check.py's module docstring for the full
    mechanism: it compares the git commit recorded in build_info.h at build
    time against current HEAD, and -- if the tree has uncommitted changes in
    firmware/KilnFW or firmware/CommonFW -- whether any of those changed
    files are newer than the binary). This catches flashing a binary built
    before the commit/edit it claims to be. Pass allow_stale=True to flash
    anyway (deliberate reflash-the-existing-image or flash-without-rebuild
    cases) -- the refusal message is still returned as a warning line first.

    A single "Verify Failed" on the very first attempt is a known, benign,
    environment-specific quirk (documented in docs/PROJECT_STATUS.md) that
    reliably succeeds on an immediate identical retry; with retry_once=True
    (the default) this tool does that retry automatically and only reports
    failure if the SECOND attempt also fails. On a persistent failure this
    returns the openocd output tail for diagnosis rather than guessing --
    do not attempt a raw `flash erase_sector` recovery by hand; that is
    exactly what caused the incident this tool exists to prevent."""
    openocd_exe = _find_openocd_exe()
    if not openocd_exe:
        return "error: openocd.exe not found under ~/.espressif/tools/openocd-esp32/ or C:\\Espressif\\ -- is it installed?"

    kiln_fw_root = _kiln_fw_root()
    build_dir = os.path.join(kiln_fw_root, "build")
    required = [
        os.path.join(build_dir, "bootloader", "bootloader.bin"),
        os.path.join(build_dir, "partition_table", "partition-table.bin"),
        os.path.join(build_dir, "KilnCtrl.bin"),
    ]
    missing = [p for p in required if not os.path.isfile(p)]
    if missing:
        return "error: missing build output(s), run `idf.py build` first: " + ", ".join(missing)

    stale = stale_check.check_kilnfw_stale(kiln_fw_root)
    if stale.stale:
        _session_log.warning("flash_firmware: stale binary detected: %s", stale.reason)
        if not allow_stale:
            return (
                "error: refusing to flash a stale binary -- " + stale.reason + "\n\n"
                "Rebuild with build_kilnfw first, or pass allow_stale=True to flash "
                "this binary anyway."
            )

    kill_openocd_sessions()  # a stale session holding the JTAG interface looks identical to a flash failure

    tcl = (
        "program_esp build/bootloader/bootloader.bin 0x0 verify; "
        "program_esp build/partition_table/partition-table.bin 0x8000 verify; "
        "program_esp build/KilnCtrl.bin 0x810000 verify reset exit"
    )
    stale_prefix = f"WARNING: flashed a stale binary anyway ({stale.reason})\n" if (stale.stale and allow_stale) else ""

    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=kiln_fw_root, timeout_s=90)
    if ok:
        return stale_prefix + "flashed and verified OK (bootloader + partition table + app), board reset and running"

    if retry_once:
        _session_log.warning("flash_firmware: first attempt failed, retrying once (known benign quirk)")
        ok2, output2 = _run_openocd(openocd_exe, board_cfg, tcl, cwd=kiln_fw_root, timeout_s=90)
        if ok2:
            return "flashed and verified OK on retry (first attempt hit the known benign Verify-Failed quirk)"
        output = output2

    tail = "\n".join(output.strip().splitlines()[-25:])
    return (
        "error: flash failed" + (" twice" if retry_once else "") + f":\n{tail}\n\n"
        "Do not run a raw `flash erase_sector`/full-chip-erase recovery by hand -- "
        "that combination (partial reflash after a full erase) is what caused the "
        "2026-08-11 incident this tool exists to prevent. If this persists, a full "
        "USB power cycle of the board (not just a JTAG reset) has resolved a "
        "flash-write-protect-stuck state before."
    )


# ---------------------------------------------------------------------------
# Generic debug (OpenOCD program/reset/halt/step/memory/registers), both
# peers. See debug_probe.py's module docstring for the full design rationale
# (tools/PcTools/TODO.md "One substrate covers both: OpenOCD") and the guard
# rails that are and are not implemented. This section only adds the MCP
# tool wrappers + the policy pieces debug_probe.py deliberately doesn't know
# about (session logging, the ESP-halt-during-profile guard, the
# write-requires-confirm gate).
# ---------------------------------------------------------------------------
@_tool()
def set_openocd_path(path: str) -> str:
    """Sets a persisted override for openocd.exe, for machines where
    autodetection (settings.json override -> OPENOCD_EXE env var ->
    ~/.espressif/tools/openocd-esp32/ -> C:\\Espressif\\...) doesn't find it.

    Only needed if get_openocd_status() reports "not found". The path is
    validated (must exist and be a file) then persisted to settings.json and
    used first on every subsequent flash_firmware()/debug_*() call."""
    if not path or not os.path.isfile(path):
        raise ValueError(f"path does not exist or is not a file: {path!r}")
    settings.set_openocd_path(path)
    return f"openocd path set to {path} (persisted in settings.json)"


@_tool()
def get_openocd_status() -> str:
    """Reports the openocd.exe path that will actually be used, where it came
    from (explicit override / OPENOCD_EXE env var / autodetection / not
    found), and which peers' default ELF build outputs currently exist on
    disk -- a quick way to confirm what debug_program() will do before
    calling it."""
    override = settings.get_openocd_path()
    env_path = os.environ.get("OPENOCD_EXE")
    resolved = openocd_util._find_openocd_exe()

    if resolved is None:
        source_line = "openocd.exe: not found -- set one with set_openocd_path()"
    elif override and os.path.isfile(override) and resolved == override:
        source_line = f"openocd.exe: {resolved} (source: settings.json override)"
    elif env_path and os.path.isfile(env_path) and resolved == env_path:
        source_line = f"openocd.exe: {resolved} (source: OPENOCD_EXE env var)"
    else:
        source_line = f"openocd.exe: {resolved} (source: autodetected)"

    lines = [source_line]
    for peer, label, elf_fn in (
        (debug_probe.PEER_ESP, "esp (KilnFW)", debug_probe._kiln_fw_elf),
        (debug_probe.PEER_PICO, "pico (SaftyFW)", debug_probe._safty_fw_elf),
    ):
        elf = elf_fn()
        present = "found" if os.path.isfile(elf) else "MISSING -- pass elf_path explicitly or build first"
        lines.append(f"{label} default elf: {elf} ({present})")
    return "\n".join(lines)


@_tool()
def debug_program(peer: str, elf_path: Optional[str] = None, confirm: bool = False, allow_stale: bool = False) -> str:
    """Flashes an ELF to `peer` ("esp" or "pico") over OpenOCD and resets it.
    Writes flash on a live board -- refused unless `confirm=True` is passed
    explicitly (tools/PcTools/TODO.md's "flash writes require an explicit
    confirm" guard rail).

    Uses the peer's default build output (KilnFW/build/KilnCtrl.elf or
    SaftyFW/build/SaftyFW.elf) unless `elf_path` is given. For the ESP, prefer
    flash_firmware() instead -- it flashes the full three-image set
    (bootloader/partition-table/app) this tool does not; this generic path is
    for the RP2040, which ships one plain ELF with no bootloader.

    For peer="pico" (SaftyFW), same staleness refusal as flash_firmware(): the
    ELF's build-identity header (saftyfw_build_info.h) is checked against
    current HEAD and, if firmware/SaftyFW or firmware/CommonFW have
    uncommitted changes, against those changed files' mtimes -- see
    stale_check.py. Pass allow_stale=True to flash anyway. Only checked when
    using the peer's default ELF and default build-info location; an explicit
    elf_path bypasses the check (nothing to compare it against)."""
    if not confirm:
        return "error: flash write refused without confirm=True -- this writes flash on a live board"

    stale_prefix = ""
    if peer == debug_probe.PEER_PICO and elf_path is None:
        stale = stale_check.check_saftyfw_stale(debug_probe._safty_fw_root())
        if stale.stale:
            _session_log.warning("debug_program: stale ELF detected for peer=%s: %s", peer, stale.reason)
            if not allow_stale:
                return (
                    "error: refusing to flash a stale ELF -- " + stale.reason + "\n\n"
                    "Rebuild with build_saftyfw_host_tests/the SaftyFW build first, or pass "
                    "allow_stale=True to flash this ELF anyway."
                )
            stale_prefix = f"WARNING: flashed a stale binary anyway ({stale.reason})\n"

    ok, output = debug_probe.program(peer, elf_path)
    _session_log.warning("debug_program: %s peer=%s ok=%s", "programmed" if ok else "FAILED to program", peer, ok)
    if ok:
        return stale_prefix + f"programmed {peer} OK, reset and running"
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: program failed for {peer}:\n{tail}"


@_tool()
def debug_reset(peer: str, mode: str = "run") -> str:
    """Resets `peer` ("esp"/"pico"). `mode` is "run" (default, resumes
    execution), "halt" (resets and halts), or "init" (resets and runs any
    OpenOCD target init sequence, then halts)."""
    ok, output = debug_probe.reset(peer, mode)
    _session_log.warning("debug_reset: peer=%s mode=%s ok=%s", peer, mode, ok)
    if ok:
        return f"reset {peer} ({mode}) OK"
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: reset failed for {peer}:\n{tail}"


@_tool()
def debug_halt(peer: str) -> str:
    """Halts `peer`'s core.

    Guard: for peer="esp", refuses if a fire profile is currently running or
    paused (halting the ESP mid-profile freezes relay control and stops its
    telemetry to the Pico, which would correctly read it as a dead main
    controller and trip -- see debug_probe.py's module docstring). If the ESP
    doesn't answer the status query at all, the halt is allowed -- a board
    that isn't reachable over the UART link isn't running a profile you'd be
    interrupting, so that failure shouldn't block an unrelated JTAG halt.
    """
    if peer == debug_probe.PEER_ESP:
        try:
            status = _profiles.get_exec_status(timeout=2.0)
        except Exception:  # noqa: BLE001 - unreachable ESP must not block the halt
            status = None
        if status is not None and status.state in (1, 2):
            return (
                f"error: refusing to halt ESP while a profile is {status.state_name} -- "
                "this would freeze relay control mid-firing. Use debug_reset/profiles "
                "stop tools if you really need to interrupt it."
            )
    ok, output = debug_probe.halt(peer)
    if ok:
        _session_log.warning("debug_halt: halted %s", peer)
        return f"halted {peer}"
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: halt failed for {peer}:\n{tail}"


@_tool()
def debug_resume(peer: str) -> str:
    """Resumes `peer`'s core from a halt."""
    ok, output = debug_probe.resume(peer)
    if ok:
        _session_log.warning("debug_resume: resumed %s", peer)
        return f"resumed {peer}"
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: resume failed for {peer}:\n{tail}"


@_tool()
def debug_step(peer: str) -> str:
    """Single-steps `peer`'s core one instruction. If it was running, this
    halts it first (it does not resume running after the step -- it stays
    halted at the next instruction)."""
    ok, output = debug_probe.step(peer)
    if ok:
        _session_log.warning("debug_step: stepped %s", peer)
        return f"stepped {peer}:\n{output.strip()}"
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: step failed for {peer}:\n{tail}"


@_tool()
def debug_read_memory(peer: str, address: int, count: int = 1, width: int = 32,
                      leave_halted: bool = False,
                      target: str | None = None) -> str:
    """Reads `count` `width`-bit (8/16/32) words from `peer`'s memory starting
    at `address`. Read-only, no guard needed. Halts the core if it wasn't
    already (OpenOCD requires this for a memory read), then resumes it unless
    `leave_halted` is set -- a read that exits without resuming leaves the
    board halted, which reads as "the firmware froze" to everything except a
    debugger. Set `leave_halted` only when several reads must observe the same
    frozen state.

    `target` picks one core by OpenOCD target name ("rp2040.core0" /
    "rp2040.core1"), as `debug_read_registers` does. Pass it whenever the
    address is in the private peripheral bus (0xE0000000-0xE00FFFFF): the NVIC
    and parts of the SCB are banked per core on the RP2040, so NVIC_ISER
    (0xE000E100) or VTOR (0xE000ED08) read without naming a core describes
    core 0 only, whichever core you meant."""
    ok, output = debug_probe.read_memory(peer, address, count, width,
                                         leave_halted=leave_halted,
                                         target=target)
    if ok:
        return output.strip()
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: read_memory failed for {peer}:\n{tail}"


@_tool()
def debug_read_symbol(peer: str, symbol: str, count: Optional[int] = None, width: int = 32,
                      elf_path: Optional[str] = None, leave_halted: bool = False) -> str:
    """Reads a named symbol from `peer`'s memory ("esp" or "pico"). Same as
    debug_read_memory() but resolves the address from the peer's build ELF,
    so a counter or register image can be read by name without looking up an
    address by hand.

    `count` defaults to the symbol's whole recorded size at the requested
    `width` (a 16-byte array reads as 16 bytes with width=8), or one word if
    the ELF records no size.

    Two caveats worth knowing before trusting the numbers:
      - A symbol defined at more than one address (file-static objects of the
        same name in different translation units) is
        refused rather than resolved arbitrarily.
      - This returns raw bytes and knows nothing about struct layout. Get a
        member's offset from DWARF (`objdump --dwarf=info`); assuming offset 0
        is how a correct register image was misread as garbage on 2026-08-25.
    """
    try:
        ok, output = debug_probe.read_symbol(peer, symbol, count=count, width=width,
                                             elf_path=elf_path, leave_halted=leave_halted)
    except (ValueError, FileNotFoundError, RuntimeError) as exc:
        return f"error: {exc}"
    if ok:
        return output.strip()
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: read_symbol failed for {peer}:\n{tail}"


@_tool()
def debug_list_symbols(peer: str, pattern: str, elf_path: Optional[str] = None, limit: int = 40) -> str:
    """Lists symbols in `peer`'s build ELF whose name contains `pattern`
    (case-insensitive substring, not a regex), with address and recorded size.
    Use it to find what debug_read_symbol() can read -- e.g. pattern="g_dbg"
    for temporary bench counters.

    A symbol shown as "AMBIGUOUS" is defined at several addresses in this ELF
    and debug_read_symbol() will refuse it by name."""
    try:
        table = debug_probe.symbol_table(peer, elf_path)
    except (ValueError, FileNotFoundError, RuntimeError) as exc:
        return f"error: {exc}"
    needle = pattern.lower()
    hits = sorted(name for name in table if needle in name.lower())
    if not hits:
        return f"no symbol name contains {pattern!r}"
    lines = []
    for name in hits[:limit]:
        address, size = table[name]
        if address < 0:
            lines.append(f"{name}: AMBIGUOUS (defined at multiple addresses)")
        else:
            lines.append(f"0x{address:08x}  size {size:5d}  {name}")
    if len(hits) > limit:
        lines.append(f"... and {len(hits) - limit} more (raise limit or narrow the pattern)")
    return "\n".join(lines)


@_tool()
def debug_write_memory(peer: str, address: int, value: int, width: int = 32, confirm: bool = False) -> str:
    """Writes one `width`-bit (8/16/32) `value` at `address` in `peer`'s
    memory. Live RAM/flash-mapped memory write on a running board -- refused
    unless `confirm=True` is passed explicitly.

    Additive guard for peer="pico": even with confirm=True, this reads
    SaftyFW's relay_owner.c ARMED/GRACE/TRIPPED state directly over SWD first
    (debug_probe.pico_armed_state() -- UART-link-independent, see
    debug_probe.py's module docstring) and refuses the write if that state
    reads ARMED, OR if it could not be confidently determined at all (fail
    closed -- an unreadable state is never treated as "not armed"). This is
    additive to, never a replacement for, the confirm=True gate below."""
    if not confirm:
        return "error: memory write refused without confirm=True -- this writes live RAM/flash-mapped memory on a running board"
    if peer == debug_probe.PEER_PICO:
        armed, detail = debug_probe.pico_armed_state()
        if armed is not False:
            _session_log.warning(
                "debug_write_memory: REFUSED peer=pico address=0x%x armed_state=%s detail=%s",
                address, armed, detail,
            )
            if armed is None:
                return f"error: write refused -- could not confidently determine Pico ARMED state ({detail})"
            return f"error: write refused -- Pico is ARMED ({detail})"
    ok, output = debug_probe.write_memory(peer, address, value, width)
    _session_log.warning(
        "debug_write_memory: peer=%s address=0x%x value=0x%x width=%d ok=%s", peer, address, value, width, ok
    )
    if ok:
        return f"wrote 0x{value:x} ({width}-bit) to {peer} 0x{address:x}"
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: write_memory failed for {peer}:\n{tail}"


@_tool()
def debug_read_registers(peer: str, target: str | None = None,
                         leave_halted: bool = False) -> str:
    """Reads the core registers for `peer`. Needs the core halted to read
    registers, so this halts it as a side effect if it was running.

    `target` picks one core by OpenOCD target name on a multi-core chip --
    "rp2040.core0" / "rp2040.core1" for the Pico. Omit it to read whichever
    core the config makes current (core 0 on the Pico).

    Resumes the core afterwards unless `leave_halted` is set, for the same
    reason `debug_read_memory` does.
    """
    ok, output = debug_probe.read_registers(peer, target=target,
                                            leave_halted=leave_halted)
    if ok:
        return output.strip()
    tail = "\n".join(output.strip().splitlines()[-25:])
    return f"error: read_registers failed for {peer}:\n{tail}"


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
@_tool()
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


@_tool()
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


# ---------------------------------------------------------------------------
# THERMO -- 3x MAX31856 (task 1)
#
# The parts sit on the thermocouple daughterboard behind J6, on the shared SPI
# bus (CS0/CS1/CS2 = channels 0/1/2). Their ~DRDY outputs do NOT reach the
# ESP32 -- they go to the SX1509, so "is a conversion ready" is reported by
# io_read(), not here.
# ---------------------------------------------------------------------------
@_tool()
def thermo_read(channel: int = THERMO_CHANNEL_ALL) -> str:
    """Read thermocouple temperature, cold junction and decoded faults.

    `channel` 0-2 for one channel, or omit for all three. Returns real device
    data (this is a query, not a fire-and-forget command). A channel whose SPI
    read failed still appears, flagged, with NaN temperatures.
    """
    try:
        readings = _thermo.read(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if not readings:
        return "device reported no channels"
    return "\n".join(r.describe() for r in readings)


@_tool()
def thermo_read_faults(channel: int = THERMO_CHANNEL_ALL) -> str:
    """Read the fault status (SR) and MASK registers, decoded to text.

    `channel` 0-2, or omit for all three. Unlike thermo_read this does not
    depend on a conversion having happened.
    """
    try:
        faults = _thermo.read_faults(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if not faults:
        return "device reported no channels"
    return "\n".join(f.describe() for f in faults)


@_tool()
def thermo_config_channel(
    channel: int,
    tc_type: int = 3,
    avg_mode: int = 0,
    filter_50hz: bool = False,
    auto_convert: bool = True,
) -> str:
    """Configure one thermocouple channel.

    `tc_type`: 0=B 1=E 2=J 3=K 4=N 5=R 6=S 7=T (8/12 are raw voltage modes).
    K is what this kiln ships with. `avg_mode`: 0=1, 1=2, 2=4, 3=8, 4=16
    samples averaged. `filter_50hz` picks 50 Hz mains rejection instead of
    60 Hz. `auto_convert` false selects one-shot mode, where nothing converts
    until thermo_one_shot().
    """
    try:
        result = _thermo.config_channel(channel, tc_type, avg_mode, filter_50hz, auto_convert)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} configured"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not configure channel {channel}{detail}"


@_tool()
def thermo_set_thresholds(
    channel: int, tc_high: float, tc_low: float, cj_high: int, cj_low: int
) -> str:
    """Set one channel's TC high/low trip temperatures and CJ high/low limits (degC)."""
    try:
        result = _thermo.set_thresholds(channel, tc_high, tc_low, cj_high, cj_low)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} thresholds set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set channel {channel} thresholds{detail}"


@_tool()
def thermo_set_cj_offset(channel: int, offset_c: float) -> str:
    """Set one channel's cold-junction offset in degC (-8..+8)."""
    try:
        result = _thermo.set_cj_offset(channel, offset_c)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} CJ offset set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set channel {channel} CJ offset{detail}"


@_tool()
def thermo_one_shot(channel: int) -> str:
    """Trigger a single conversion on one channel (poll with thermo_read after)."""
    try:
        result = _thermo.one_shot(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - one-shot triggered on channel {channel}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not trigger one-shot on channel {channel}{detail}"


@_tool()
def thermo_clear_faults(channel: int) -> str:
    """Pulse CR0.FAULTCLR on one channel.

    Only meaningful in the part's interrupt fault mode; in the comparator mode
    this driver uses, fault bits clear themselves when the condition clears.
    """
    try:
        result = _thermo.clear_faults(channel)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} faults cleared"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not clear channel {channel} faults{detail}"


@_tool()
def thermo_set_auto_report(channel_mask: int = 0x07, period_ms: int = 1000) -> str:
    """Have the firmware push readings for the masked channels every period_ms.

    Bit N of `channel_mask` selects channel N; `period_ms` 0 turns it off.
    Pushed readings are buffered here -- read them with thermo_get_reports().
    """
    try:
        result = _thermo.set_auto_report(channel_mask, period_ms)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - auto-report mask 0x{channel_mask:02X} period {period_ms} ms"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set auto-report{detail}"


@_tool()
def thermo_get_reports(n: int = 10) -> str:
    """Return the last `n` auto-report pushes received since reporting was on.

    An MCP client cannot receive an unsolicited push, so this is the pull-based
    view of that stream -- turn it on with thermo_set_auto_report() first.
    """
    if n <= 0:
        return "error: n must be positive"
    batches = _snapshot(_thermo_reports, n)
    if not batches:
        return "(no auto-report pushes received yet -- call thermo_set_auto_report first)"
    return "\n".join(
        "; ".join(r.describe() for r in batch) for batch in batches
    )


@_tool()
def thermo_read_reg(channel: int, reg: int, length: int = 1) -> str:
    """Raw MAX31856 register read on one channel (debug), 1-16 bytes."""
    try:
        registers = _thermo.read_reg(channel, reg, length)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    return registers.describe()


@_tool()
def thermo_write_reg(channel: int, reg: int, value: int) -> str:
    """Raw MAX31856 register write on one channel (debug)."""
    try:
        result = _thermo.write_reg(channel, reg, value)
    except ThermoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - channel {channel} reg 0x{reg:02X} written"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not write channel {channel} reg 0x{reg:02X}{detail}"


# ---------------------------------------------------------------------------
# IO -- SX1509 expander at 0x3E (task 2)
#
# Relay numbering is the schematic's, and it does NOT match the K
# designators: Relay1=K3/J8, Relay2=K1/J3, Relay3=K2/J4, Relay4=K5/J11.
# Every tool below repeats that mapping rather than assuming the caller
# remembers it.
# ---------------------------------------------------------------------------
@_tool()
def io_read() -> str:
    """Read the expander: relays, digital I/O levels/directions, DRDY, raw regs.

    Returns real device data (a query). This is also the only place the three
    thermocouple ~DRDY lines are visible, since they go to the expander rather
    than to an ESP32 GPIO.
    """
    try:
        state = _io.read()
    except IoQueryError as exc:
        return f"error: {exc}"
    return state.describe()


@_tool()
def io_set_relay(relay: int, on: bool) -> str:
    """Switch one relay on or off.

    Relay numbering follows the schematic and does not match the K
    designators: 1=K3/J8, 2=K1/J3, 3=K2/J4, 4=K5/J11. `on` energizes the 12 V
    coil.

    Waits briefly for a refusal reply before reporting success: the
    firmware can refuse this because a running profile owns the relay, a
    safety fault is asserted, or an OTA is in progress -- distinguishable
    reasons that a bare transport ACK cannot tell apart from "energized".
    """
    try:
        result = _io.set_relay(relay, on)
    except IoQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - relay {relay} {'on' if on else 'off'}"
    return f"refused ({result.refusal.value}) - relay {relay} not changed" + (
        f": {result.reason_text}" if result.reason_text and result.refusal is devices.RelayRefusal.OTHER else ""
    )


@_tool()
def io_set_relay_mask(mask: int, value: int) -> str:
    """Switch several relays in one atomic register write.

    Bits 0-3 are Relay1..Relay4 (K3/J8, K1/J3, K2/J4, K5/J11) in both `mask`
    (which relays to change) and `value` (their new levels).

    Same refusal-aware wait as :func:`io_set_relay` -- see its docstring.
    """
    try:
        result = _io.set_relay_mask(mask, value)
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


@_tool()
def io_all_relays_off() -> str:
    """De-energize all four relays unconditionally.

    The same state the firmware falls back to on link loss or a safety fault.
    """
    return _io_mutating("all relays off", lambda: _io.all_relays_off())


@_tool()
def io_set_output(io: int, level: bool) -> str:
    """Drive digital I/O 1-7 high or low (only meaningful when it is an output).

    IO1 = opto-isolated input from J24, IO2 = opto-isolated output to J25,
    IO3/IO4 = J20 pins 1/2, IO5/IO6 = J21 pins 1/2, IO7 = J23 pin 1.
    """
    return _io_mutating(f"IO{io} set {'high' if level else 'low'}", lambda: _io.set_io(io, level))


@_tool()
def io_set_direction(io: int, is_input: bool, pullup: bool = False) -> str:
    """Set digital I/O 1-7 as an input (with optional pull-up) or an output."""
    return _io_mutating(
        f"IO{io} direction set", lambda: _io.set_io_dir(io, is_input, pullup)
    )


@_tool()
def io_set_auto_report(period_ms: int = 500) -> str:
    """Have the firmware push expander state every period_ms (0 = off).

    Pushes also happen immediately on every ~INT edge, so an input change is
    reported without waiting out the period. Read them with io_get_reports().
    """
    return _io_mutating(
        f"auto-report period {period_ms}ms", lambda: _io.set_auto_report(period_ms)
    )


@_tool()
def io_get_reports(n: int = 10) -> str:
    """Return the last `n` expander auto-report pushes.

    The pull-based view of a stream an MCP client cannot otherwise receive --
    turn it on with io_set_auto_report() first.
    """
    if n <= 0:
        return "error: n must be positive"
    states = _snapshot(_io_reports, n)
    if not states:
        return "(no auto-report pushes received yet -- call io_set_auto_report first)"
    return "\n".join(s.describe() for s in states)


@_tool()
def io_scan() -> str:
    """Probe 0x3E/0x3F/0x70/0x71 for an SX1509; report which addresses answered.

    The board straps ADDR1/ADDR0 to GND, so only 0x3E should answer -- anything
    else is a different part or a strapping error.
    """
    try:
        found = _io.scan()
    except IoQueryError as exc:
        return f"error: {exc}"
    if not found:
        return "no device answered (expected 0x3E)"
    return ", ".join(f"0x{a:02X}" for a in found)


@_tool()
def expander_read_reg(reg: int, length: int = 1) -> str:
    """Raw SX1509 register read (debug), 1-16 bytes."""
    try:
        registers = _io.read_reg(reg, length)
    except IoQueryError as exc:
        return f"error: {exc}"
    return registers.describe()


@_tool()
def expander_write_reg(reg: int, value: int) -> str:
    """Raw SX1509 register write (debug)."""
    return _io_mutating(f"reg 0x{reg:02X} <- 0x{value:02X}", lambda: _io.sx_write_reg(reg, value))


@_tool()
def expander_set_dir(mask: int) -> str:
    """Write RegDir: u16, bit N = 1 makes expander pin N an input."""
    return _io_mutating(f"RegDir <- 0x{mask:04X}", lambda: _io.sx_set_dir(mask))


@_tool()
def expander_set_pullup(mask: int) -> str:
    """Write RegPullUp: u16, bit N = 1 enables pin N's pull-up."""
    return _io_mutating(f"RegPullUp <- 0x{mask:04X}", lambda: _io.sx_set_pullup(mask))


@_tool()
def expander_set_opendrain(mask: int) -> str:
    """Write RegOpenDrain: u16, bit N = 1 makes pin N open-drain."""
    return _io_mutating(f"RegOpenDrain <- 0x{mask:04X}", lambda: _io.sx_set_opendrain(mask))


@_tool()
def expander_set_debounce(enable_mask: int, config: int = 0) -> str:
    """Enable debounce on the masked pins; `config` 0-7 selects 0.5ms << config."""
    return _io_mutating(
        f"debounce mask 0x{enable_mask:04X} config {config}",
        lambda: _io.sx_set_debounce(enable_mask, config),
    )


@_tool()
def expander_set_int_mask(mask: int, sense: int = 0) -> str:
    """Write RegInterruptMask (bit N = 1 DISABLES pin N's interrupt) and RegSense.

    `sense` is 2 bits per pin *pair*, exactly as the part encodes them.
    """
    return _io_mutating(
        f"RegInterruptMask <- 0x{mask:04X}, sense 0x{sense:04X}",
        lambda: _io.sx_set_int_mask(mask, sense),
    )


@_tool()
def expander_led_driver(pin: int, enable: bool, intensity: int = 0) -> str:
    """Enable the SX1509's LED driver on one pin (0-15).

    `intensity` 0-255, where 0 is *full on* for this part's sink driver.
    """
    return _io_mutating(
        f"pin {pin} LED driver {'on' if enable else 'off'} @ {intensity}",
        lambda: _io.sx_led_driver(pin, enable, intensity),
    )


@_tool()
def expander_reset(hard: bool = False) -> str:
    """Reset the SX1509: software reset via RegReset, or pulse ~RESET (GPIO10)."""
    return _io_mutating(f"SX1509 {'hard' if hard else 'soft'} reset", lambda: _io.sx_reset(hard))


def _touch_mutating(label: str, call: "Callable[[], devices.OkReason]") -> str:
    """Run one TOUCH write (INJECT/SET_TAP_DUMP/LOG_TAP_TARGETS) and format
    its :class:`OkReason` -- see TouchClient._write().

    (Used to say "same shape as :func:`_display_mutating`" -- that helper and
    the twelve-plus ``display_*`` MCP tools it backed were removed as stale:
    the firmware's ``display_bridge_task`` is intentionally dead code now that
    LVGL owns the ILI9488 outright. See firmware/KilnFW/TODO.md sec 10.1.
    ``_display`` (the :class:`DisplayClient` instance) stays -- it still
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


# ---------------------------------------------------------------------------
# TOUCH -- NS2009 touch controller on the same J2 panel (task 13)
#
# touch_inject is what lets this side "send touches as if from the screen":
# the firmware's screen_idle state machine treats it exactly like a real
# NS2009 press, resetting the auto-blank idle timer and waking the panel if
# it's currently blanked. touch_get_state is the query that shows the effect.
# ---------------------------------------------------------------------------
@_tool()
def touch_get_state() -> str:
    """Whether the panel is on right now, and how long it's been idle.

    A query answered from the firmware's in-memory screen_idle state, not a
    touch-controller round trip -- fast even if no NS2009 ever came up.
    """
    try:
        state = _touch.get_state()
    except TouchQueryError as exc:
        return f"error: {exc}"
    return state.describe()


@_tool()
def touch_inject(x: int, y: int, pressed: bool = True) -> str:
    """Send a synthetic touch at (x, y), as if the physical panel were pressed.

    x/y are SCREEN PIXEL coordinates (0,0 at the top-left, same space every
    ui_page_*.c file lays widgets out in) -- the firmware applies them
    directly to LVGL's input device, downstream of the NS2009 calibration
    transform, so this call hit-tests real buttons/containers exactly like a
    finger would, independent of whether this board has ever been
    touch-calibrated. It also resets the firmware's screen auto-blank idle
    timer and wakes the panel if it is currently blanked, exactly as a real
    touch would.

    pressed=True latches the touch: the firmware keeps reporting it at (x, y)
    on every LVGL input poll until a matching pressed=False call releases it
    (one press + one release == one click, same as a real tap), or until
    ~5s pass with no follow-up call, at which point the firmware auto-releases
    it on its own so a forgotten release can't wedge the UI. To drag/scroll,
    send one pressed=True call, then further pressed=True calls with updated
    (x, y) tracing the path, then a final pressed=False to release -- each
    call is one point along the gesture, there is no separate "move" verb.
    An injected press takes priority over the physical NS2009 for as long as
    it is held, so it will not race a stray touch on the bench.
    """
    return _touch_mutating(f"touch injected ({x},{y},{'down' if pressed else 'up'})",
                            lambda: _touch.inject(x, y, pressed))


@_tool()
def touch_set_tap_dump(enable: bool) -> str:
    """Turn the firmware's AUTOMATIC per-page-switch tap-target dump on/off.

    Off by default: kiln_ui_show() dumps every clickable widget's rectangle,
    centre point, and label on every page switch only while this is enabled,
    because doing it unconditionally floods the device log during ordinary
    navigation. touch_log_tap_targets() below is unaffected by this flag and
    always dumps on request -- reach for that first; only turn this on when
    driving a sequence of navigations and wanting every switch's targets
    logged without a separate call after each one.
    """
    return _touch_mutating(
        f"tap dump {'on' if enable else 'off'}", lambda: _touch.set_tap_dump(enable)
    )


@_tool()
def touch_log_tap_targets() -> str:
    """Request an immediate tap-target dump for whatever screen is loaded now.

    This is the only way to discover where a widget actually is on this
    panel -- there is no framebuffer readback. The dump covers the active
    screen plus lv_layer_top()/lv_layer_sys() (where modal overlays such as
    ui_confirm.c's confirmation dialogs and ui_num_pad.c's keypad live), and
    walks any open lv_keyboard's individual keys. The dump itself still
    arrives as ESP_LOGI "tap target ..." lines over the device log
    (get_device_log / get_device_log_json), not as this call's return value --
    but a driver-error refusal is no longer silent, see _touch_mutating().
    """
    return _touch_mutating("tap-target dump requested", lambda: _touch.log_tap_targets())


# ---------------------------------------------------------------------------
# SAFETY -- the opto-isolated RP2040 link (task 7)
#
# The Pico owns the safety thermocouple, the current sensors, the E-stop and
# relay K4. The ESP polls it and caches the answer, so a down link is a
# successful query reporting link_up = 0 -- not an error.
#
# The RP2040 firmware does not exist in this repository yet: link_up = 0 with
# "never received" is the expected steady state today.
# ---------------------------------------------------------------------------
@_tool()
def safety_get_status() -> str:
    """Read the cached safety status from the safety processor.

    Covers link state, the isolated fault line (which THIS firmware drives as
    an output), E-stop, safety relay K4, heating enable, the safety
    thermocouple and the three current-sense channels, plus how old the data
    is.

    Expected result today: no flags set and "never received" -- the RP2040
    firmware that would answer does not exist yet. That is not a fault.
    """
    try:
        status = _safety.get_status()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    return status.describe()


@_tool()
def safety_get_link_stats() -> str:
    """Read the ESP's own counters for the isolated UART.

    Frames sent and timeouts climbing while received stays at zero is exactly
    the signature of the missing Pico firmware.
    """
    try:
        stats = _safety.get_link_stats()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    return stats.describe()


@_tool()
def safety_request_enable(enable: bool) -> str:
    """Ask the safety processor to permit (or drop) heating.

    Advisory only: the Pico can refuse, and its own interlocks always win --
    that outcome never comes back here. What this DOES report is an ESP-side
    refusal (a truncated frame) caught before the request ever reached the
    Pico.
    """
    try:
        result = _safety.request_enable(enable)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - requested enable={enable}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not request enable={enable}{detail}"


@_tool()
def safety_ping() -> str:
    """Force an immediate poll of the safety processor."""
    return _send(UART_TASK_ID_SAFETY, devices.safety_ping())


@_tool()
def safety_set_poll_period(period_ms: int) -> str:
    """Set how often the ESP polls the safety processor, in ms (0 stops polling)."""
    try:
        result = _safety.set_poll_period(period_ms)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - poll period {period_ms} ms"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set poll period{detail}"


@_tool()
def safety_set_fault_out(assert_fault: bool) -> str:
    """Drive the isolated Fault line to the safety processor.

    This is an ESP **output** (GPIO6 -> optocoupler U1 -> the Pico's mainFault
    input): us telling the safety processor that the main controller has
    faulted. It is not a signal coming back from the Pico -- there is no
    hardware path for that at all.

    The firmware asserts this by itself on PC-link loss, a thermocouple fault
    or a watchdog trip; this tool is a manual override of that.
    """
    try:
        result = _safety.set_fault_out(assert_fault)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - fault out={assert_fault}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set fault out{detail}"


@_tool()
def safety_clear_trip() -> str:
    """Clear a latched safety trip on the safety processor.

    A trip LATCHES on the Pico: once `safety_guards_tick()` sets `is_tripped`
    it returns early and stops re-evaluating, so removing whatever caused the
    trip does not clear it by itself. A board that tripped because the
    isolated fault line went high stays tripped after the line goes low. This
    is the only way out short of a power cycle.

    Clearing is a request, not an order. `link_task_handle_clear_trip()` calls
    `safety_guards_try_clear()`, which re-checks the guard against live inputs
    and refuses while the condition still holds -- so this cannot be used to
    dismiss a fault that is still present. If the trip does not clear, the
    cause is still there.

    Fire-and-forget: no reply on the wire. Check `safety_get_status()`
    afterwards to see whether it actually cleared.
    """
    return _send(UART_TASK_ID_SAFETY, devices.safety_clear_trip())


@_tool()
def safety_set_tc_type(tc_type_name: str) -> str:
    """Commission the safety processor's thermocouple type (config_store.h).

    `tc_type_name` is one of "B", "E", "J", "K", "N", "R", "S", "T"
    (case-insensitive), mirroring firmware/SaftyFW/src/max31856.h's
    MAX31856_TC_TYPE_* ordering. Fire-and-forget, like clear_trip -- there is
    no reply on the wire; the outcome (accepted, or refused because the relay
    is ARMED / the value wasn't recognised) shows up on the Pico's log, and
    eventually in config_version advancing on the next GET_DIAG/GET_STATUS
    poll, not here.

    This tool only sends the wire command -- it does not build any GUI/LCD
    commissioning flow (out of scope, same as clear_trip's own LCD surface
    was deferred for the same budget/scope reason).
    """
    name = tc_type_name.strip().upper()
    if name not in devices.SAFETY_TC_TYPE_NAMES:
        known = ", ".join(sorted(devices.SAFETY_TC_TYPE_NAMES))
        return f"error: unknown tc_type_name {tc_type_name!r} -- expected one of {known}"
    try:
        result = _safety.set_config(devices.SAFETY_TC_TYPE_NAMES[name])
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - requested tc_type {name}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set tc_type{detail}"


@_tool()
def safety_set_ct_cal(channel: int, calibrated: bool, gain: float, offset: float) -> str:
    """Commission one channel of the safety processor's CT current-sense
    calibration (config_store.h's ct_cal record).

    `channel` is 0-2 (one of the three current-sense channels), `gain`/
    `offset` are the linear-fit constants a bench calibration run produces.
    Only one channel is written per call -- writing channel 0 never touches
    channel 1/2's stored constants.

    Fire-and-forget, like safety_clear_trip/safety_set_tc_type: there is no
    reply on the wire. Refused on the Pico side (relay currently ARMED, or an
    out-of-range channel) shows up only in the Pico's own log, not here --
    call safety_get_ct_cal() afterward to see whether it actually took.
    """
    try:
        result = _safety.set_ct_cal(channel, calibrated, gain, offset)
    except SafetyQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - requested CT ch{channel} calibration"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set CT ch{channel} calibration{detail}"


@_tool()
def safety_get_ct_cal() -> str:
    """Read the safety processor's three CT channels' stored calibration.

    UNLIKE safety_get_status/other SAFETY queries, this is a LIVE round trip:
    it is answered by the ESP asking the Pico right now, not from a cache, so
    it can take noticeably longer and a dead isolated link shows up here as
    an error (a timeout) rather than as a successful reply with stale data.

    Each channel's ``calibrated`` flag must be checked before trusting its
    gain/offset -- an uncalibrated channel's numbers are meaningless
    (current_sense.c never reads them).
    """
    try:
        cal = _safety.get_ct_cal()
    except SafetyQueryError as exc:
        return f"error: {exc}"
    lines = []
    for i, ch in enumerate(cal.channels):
        if ch.calibrated:
            lines.append(f"channel {i}: calibrated, gain={ch.gain:.6g}, offset={ch.offset:.6g}")
        else:
            lines.append(f"channel {i}: uncalibrated")
    return "; ".join(lines)


@_tool()
def ota_rollback_pico() -> str:
    """Explicitly revert the safety processor (RP2040/SaftyFW) to its
    PREVIOUS bootloader slot, right now.

    This is the Pico half of tools/PcTools/TODO.md's `ota_rollback(processor)`
    line -- the ESP half is ota_rollback_esp() above. Unlike the ESP, the Pico
    has no ESP-IDF-style rollback API: SaftyFW's own bootloader
    (firmware/SaftyFW/bootloader/) picks the active slot at every boot from
    versioned/CRC'd flash metadata, and this call is the explicit,
    operator/agent-triggered path to marking the CURRENTLY ACTIVE slot BAD
    and switching to the other one -- even though the currently running
    image is healthy. Ordinary recovery from a bad update needs no call here
    at all: an unconfirmed PENDING_VERIFY image is already reverted
    automatically by the bootloader's own boot_attempts fallback. Use this
    when the running image is valid but behaves worse in practice than the
    one it replaced.

    This travels over the already-authenticated safety UART bridge (PC -> ESP
    -> Pico, SAFETY_CMD_ROLLBACK = 0x17), the same authentication boundary as
    safety_set_tc_type()/safety_ping() -- NOT the HTTP OTA challenge/password
    path ota_rollback_esp() uses, since there is no separate HTTP surface on
    the Pico side at all; every Pico command already goes through the ESP's
    UART bridge.

    Refused entirely on SaftyFW's own say-so (fire-and-forget: this call
    cannot see the refusal, only that nothing changes):
      - the relay is currently ARMED (same gate config writes and updates
        use -- a rollback reboots into different code, exactly as disruptive
        as a push);
      - the OTHER bootloader slot is not currently VALID or PENDING_VERIFY.
        THIS IS THE PROPERTY THAT MATTERS MOST: unlike the ESP (which has
        esp_ota_check_rollback_is_possible()), SaftyFW's bootloader has only
        two slots total, so a rollback that proceeded while the other slot
        were EMPTY/STAGED/BAD would strand the board with zero bootable
        slots on the very next boot. bootloader/metadata.c's
        bootloader_decide_rollback() checks this BEFORE marking the current
        slot BAD, not after.

    Call ota_status(processor="pico") (once it exists -- still open per
    tools/PcTools/TODO.md) or safety_get_status()/safety_get_link_stats()
    afterward to confirm the outcome: a successful rollback shows up as the
    safety link dropping and recovering with a new boot_id; a refused one
    leaves the link and boot_id exactly as they were, with the reason only
    visible in the Pico's own log (log_task) if a debug probe is attached --
    nothing is returned to this call either way.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- no RP2040 attached in this
    environment; only the wire codec and host-tested decision logic are
    exercised here.
    """
    return _send(UART_TASK_ID_SAFETY, devices.safety_request_rollback())


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
@_tool()
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
        _probe.set_mode(gpio_num, mode_val)
    except devices.GpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ProbeQueryError as exc:
        return f"error: {exc}"
    return f"ok - gpio{gpio_num} set to {mode}"


@_tool()
def gpio_probe_write(gpio_num: int, level: bool) -> str:
    """Drive an ESP32-S3 GPIO high or low.

    Refused unless gpio_num was already configured OUTPUT via
    :func:`gpio_probe_set_mode`, is deny-listed, or a profile is running or
    paused.
    """
    try:
        _probe.write(gpio_num, level)
    except devices.GpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ProbeQueryError as exc:
        return f"error: {exc}"
    return f"ok - gpio{gpio_num} = {'high' if level else 'low'}"


@_tool()
def gpio_probe_read(gpio_num: int) -> str:
    """Read an ESP32-S3 GPIO's current level.

    Does not reconfigure the pin -- safe to call on a pin already owned by
    another peripheral, though deny-listed pins are still refused outright.
    """
    try:
        level = _probe.read(gpio_num)
    except devices.GpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ProbeQueryError as exc:
        return f"error: {exc}"
    return f"gpio{gpio_num} = {'high' if level else 'low'}"


@_tool()
def gpio_probe_read_all() -> str:
    """Read every GPIO this connection has configured via gpio_probe_set_mode.

    Cheap and side-effect-free -- the recommended way to check whether the
    probe capability exists on this firmware at all: a prompt reply (even
    "no pins configured yet") means the option is built in, while a timeout
    means the board almost certainly was not built with
    CONFIG_KILNCTL_ENABLE_GPIO_PROBE.
    """
    try:
        pins = _probe.read_all()
    except ProbeQueryError as exc:
        return f"error: {exc}"
    if not pins:
        return "no pins configured yet (call gpio_probe_set_mode first)"
    return "\n".join(f"gpio{p.gpio_num}: {p.mode_name} = {'high' if p.level else 'low'}" for p in pins)


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
@_tool()
def pico_gpio_set_mode(gpio_num: int, mode: str) -> str:
    """Configure a Pico (RP2040) GPIO as input / input_pullup / input_pulldown
    / output, over SWD -- no SaftyFW build flag needed, this works against
    any Pico regardless of firmware.

    ``mode`` is one of "input", "input_pullup", "input_pulldown", "output".
    Refused unconditionally for GPIO6 (`saftyRelay`, drives K4's pilot relay
    coil) -- there is no override. Halts the core to perform the register
    writes (same as debug_write_memory).
    """
    mode_val = mode.strip().lower()
    try:
        pico_gpio_probe.set_mode(gpio_num, mode_val)
    except pico_gpio_probe.PicoGpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except ValueError as exc:
        return f"error: {exc}"
    except RuntimeError as exc:
        return f"error: {exc}"
    _session_log.warning("pico_gpio_set_mode: gpio%d set to %s", gpio_num, mode_val)
    return f"ok - pico gpio{gpio_num} set to {mode_val}"


@_tool()
def pico_gpio_write(gpio_num: int, level: bool) -> str:
    """Drive a Pico (RP2040) GPIO high or low, over SWD.

    Refused unconditionally for GPIO6. Refused for any other pin not already
    configured OUTPUT via :func:`pico_gpio_set_mode` in this same process.
    """
    try:
        pico_gpio_probe.write(gpio_num, level)
    except pico_gpio_probe.PicoGpioProbeRefused as exc:
        return f"refused: {exc.reason}"
    except RuntimeError as exc:
        return f"error: {exc}"
    _session_log.warning("pico_gpio_write: gpio%d = %s", gpio_num, "high" if level else "low")
    return f"ok - pico gpio{gpio_num} = {'high' if level else 'low'}"


@_tool()
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


@_tool()
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


# ---------------------------------------------------------------------------
# WIFI (task 11) -- status/scan/provision/forget over UART
#
# Mirrors wifi_provision_http.c's HTTP endpoints, reachable over a link that
# still works when Wi-Fi itself is down or unconfigured -- the whole reason
# this task exists is to get a board onto a network without ever needing a
# browser pointed at its AP. The GUI has driven this since it was added
# (gui.py); these tools close the matching MCP-side gap.
# ---------------------------------------------------------------------------
@_tool()
def wifi_get_status() -> str:
    """Report Wi-Fi mode (home/ap), connection state, SSID(s), IP and RSSI."""
    try:
        status = _wifi.get_status()
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    return (
        f"mode={status.mode_name} state={status.state} "
        f"sta_connected={status.sta_connected} ssid={status.ssid!r} "
        f"ap_ssid={status.ap_ssid!r} sta_ip={status.sta_ip!r} "
        f"sta_rssi={status.sta_rssi} ap_clients={status.ap_clients}"
    )


@_tool()
def wifi_scan() -> str:
    """Scan for nearby APs. Slower than the other WIFI tools (~seconds);
    capped at 6 entries by the firmware, with a truncated flag if more were seen."""
    try:
        entries, truncated = _wifi.scan()
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "no networks found" + (" (truncated)" if truncated else "")
    lines = [f"{e.ssid!r}: rssi={e.rssi} secure={e.secure}" for e in entries]
    if truncated:
        lines.append("(truncated - more networks were seen than fit the reply)")
    return "\n".join(lines)


@_tool()
def wifi_add_network(ssid: Optional[str] = None, password: Optional[str] = None) -> str:
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
    fails with a clear error if nothing has been saved that way yet."""
    if ssid is None:
        saved = wifi_credentials.load()
        if saved is None:
            return "error: no ssid given and no saved credentials found (set Wi-Fi up once via the GUI first)"
        ssid, password = saved["ssid"], saved["password"]
    try:
        result = _wifi.add_network(ssid, password or "")
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - saved {ssid!r}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not save {ssid!r}{detail}"


@_tool()
def wifi_set_mode(mode: str) -> str:
    """Switch between "home" (join a saved network) and "ap" (host the
    provisioning access point) mode."""
    mode_map = {"home": WIFI_MODE_HOME, "ap": WIFI_MODE_AP}
    mode_val = mode_map.get(mode.strip().lower())
    if mode_val is None:
        return f"error: mode must be 'home' or 'ap', got {mode!r}"
    try:
        result = _wifi.set_mode(mode_val)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - mode set to {mode}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set mode to {mode}{detail}"


@_tool()
def wifi_set_ap_identity(ap_ssid: Optional[str] = None, ap_password: Optional[str] = None) -> str:
    """Rename the board's own provisioning AP and/or change its password.
    Leave either argument unset (None) to keep it unchanged."""
    try:
        result = _wifi.set_ap_identity(ap_ssid, ap_password)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - AP identity updated"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not update AP identity{detail}"


@_tool()
def wifi_get_networks() -> str:
    """List saved networks, each with in-range/rssi/secure/connected if seen
    in the last scan. Capped at 5 entries by the firmware."""
    try:
        entries, truncated = _wifi.get_networks()
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


@_tool()
def wifi_forget(ssid: str) -> str:
    """Delete a saved network."""
    try:
        result = _wifi.forget(ssid)
    except WifiUartQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - forgot {ssid!r}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - no such saved network {ssid!r}{detail}"


# ---------------------------------------------------------------------------
# OTA (firmware/CommonFW/docs/UPDATE_PROTOCOL.md, KilnFW/TODO.md 9.5/9.6) --
# HTTP, not the UART link. All four tools below drive ota_http.c's
# /api/ota/{challenge,esp,pico} + /api/ota/pico/status endpoints over the
# board's own web server -- request construction, HMAC signing, and response
# parsing live in ota_http_client.py (unit-tested there with mocked HTTP
# responses; see tools/PcTools/tests/test_ota_http_client.py). NEVER
# exercised against real hardware from here -- no board is attached in CI or
# in this pass's dev environment. Live-board verification (does a real ESP
# accept these bytes end to end, do the lockout/interlock paths behave as
# documented) is still outstanding.
#
# Host discovery mirrors gui.py's _wifi_default_host(): prefer the board's
# current station IP (from the UART-side WIFI tools, which always work even
# with Wi-Fi itself down or never provisioned), fall back to the board's own
# fallback-AP address. An explicit `host` argument always wins over both --
# useful for kilnctl.local (mDNS) or a host on a network the UART link can't see
# into (e.g. this MCP server's serial port is on a different PC than the one
# actually joined to the board's Wi-Fi).
#
# These are destructive-adjacent, safety-relevant operations (flashing a
# kiln controller and its safety processor). Nothing here retries a partial
# write on its own -- a failed push is left failed, and re-uploading is a
# separate, explicit tool call, never something a caller has to guess
# happened silently underneath these.
# ---------------------------------------------------------------------------
def _ota_resolve_host(host: Optional[str]) -> str:
    if host:
        return host
    try:
        status = _wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return ota_http.OTA_AP_DEFAULT_HOST


@_tool()
def ota_get_challenge(host: Optional[str] = None) -> str:
    """GET /api/ota/challenge -- issue a fresh single-use OTA auth nonce.

    Mostly a diagnostic/manual tool: ota_update_esp()/ota_update_pico() below
    already fetch their own challenge internally, so this is not a required
    first step for a normal push. Useful to confirm the board's OTA HTTP
    surface is reachable at all, or to hand-verify the HMAC scheme.

    `host`: board IP or hostname (e.g. "192.168.1.42" or "kilnctl.local").
    Defaults to the board's current station IP (via wifi_get_status()'s UART
    query) if connected, else the board's fallback-AP address 192.168.4.1.
    """
    resolved = _ota_resolve_host(host)
    try:
        nonce = ota_http.get_challenge(resolved)
    except ota_http.OtaHttpError as exc:
        return f"error: {exc} (host={resolved})"
    return f"ok - nonce={nonce.hex()} host={resolved} (single-use, 30s expiry)"


@_tool()
def ota_update_esp(image_path: str, password: str, host: Optional[str] = None) -> str:
    """Push a new ESP32-S3 firmware image over Wi-Fi -- POST /api/ota/esp.

    DESTRUCTIVE-ADJACENT: this streams `image_path` (a raw ESP-IDF .bin,
    e.g. KilnCtrl.bin) straight into the inactive OTA slot and sets it as
    the next boot partition on success. Refused by the board itself unless
    every interlock in UPDATE_PROTOCOL.md section 1 holds (kiln idle, no
    heater commanded, safety link healthy, temperature below the configured
    ceiling) -- a refusal comes back here as a specific error naming the
    unmet precondition, not a generic failure.

    `password`: the board's AP password (same one wifi_prov_get_ap_password()
    returns) -- used only to derive the challenge-response HMAC per
    CommonFW/docs/UPDATE_PROTOCOL.md section 2; the plaintext password is
    never sent over the wire.

    On success the image is written and set as the boot partition, but stays
    PENDING_VERIFY until the board reboots AND main.c's
    ota_rollback_confirm_task() confirms NVS/safety-link/web-server are all
    up post-reboot -- this call does not reboot the board itself, and does
    not wait for or confirm that verification. Nothing here retries a
    partial write: a failure mid-transfer is left failed on both sides
    (ota_esp_do_transfer()'s single cleanup path aborts the OTA handle and
    releases the update mutex), and re-uploading is a fresh, separate call.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment).
    """
    resolved = _ota_resolve_host(host)
    try:
        result = ota_http.push_esp_image(resolved, image_path, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not result.ok:
        return f"error: board reported failure: {result.body} (host={resolved})"
    b = result.body
    return (f"ok - wrote {b.get('bytes')} bytes to {b.get('partition')!r}, "
            f"new version={b.get('version')!r} (host={resolved}) -- "
            f"PENDING_VERIFY until the board reboots and self-confirms; "
            f"reboot required for the new image to run")


@_tool()
def ota_rollback_esp(password: str, host: Optional[str] = None) -> str:
    """Explicitly revert the ESP32-S3 to its PREVIOUS firmware image, right
    now -- POST /api/ota/esp/rollback.

    This is the OTHER half of the rollback story: ota_rollback_confirm_task()
    (App/main.c) already handles the "don't auto-revert a healthy new image"
    side (it confirms the currently-running new image as good once NVS/
    safety-link/web-server are all up post-reboot). This tool is the
    "operator/agent wants to go back to the previous image on purpose" side
    -- even though the currently running image is healthy and already
    confirmed. Genuinely rare: ordinary recovery from a BAD update needs no
    call here at all, since an unconfirmed PENDING_VERIFY image is already
    reverted automatically by the bootloader on the next boot. Use this when
    the running image is valid but behaves worse in practice than the one it
    replaced, and a deliberate revert is wanted.

    DESTRUCTIVE-ADJACENT, same class as ota_update_esp(): refused unless the
    same interlocks in UPDATE_PROTOCOL.md section 1 hold (kiln idle, no
    heater commanded, safety link healthy, temperature below the configured
    ceiling) -- the board reboots into different code either way, so a
    rollback is exactly as disruptive as a push. ALSO refused, with a
    specific "no previous valid image to roll back to" reason, if there is
    genuinely no earlier valid image to fall back to
    (esp_ota_check_rollback_is_possible() on the board) -- this is checked
    before the reboot is attempted, not discovered by a blind call that
    fails partway.

    `password`: the board's AP password, same HMAC scheme ota_update_esp()
    uses -- but signed over a DIFFERENT context ("esp-rollback", not "esp"),
    so a MAC captured for one action cannot be reused to authorize the
    other. The plaintext password is never sent over the wire.

    On success, the board has already persisted an ota_record (processor
    "esp", success=true, reason "rollback requested") and is rebooting into
    the previous image from a short-lived background task -- this call
    returns as soon as the board's response arrives, it does NOT wait for
    the reboot to finish or re-check the version afterward. Call this
    tool's caller's own version check (e.g. get_fw_version, once the board
    is back up) to confirm which image is actually running now.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only (see
    ota_http_client.py's module doc comment); no ESP32-S3 was available in
    this environment to actually trigger a reboot/rollback.
    """
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.rollback_esp(resolved, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - rollback accepted, was running version={body.get('version_before')!r} "
            f"(host={resolved}) -- board is rebooting into the previous image now; "
            f"re-check the version once it comes back up")


@_tool()
def ota_recovery_exit_esp(password: str, host: Optional[str] = None) -> str:
    """Ask the ESP32-S3 to reboot right now to exit boot_guard.h's recovery
    mode -- POST /api/ota/esp/recovery_exit.

    Recovery mode means a recent update did not self-confirm within its
    window; the board already self-clears the counter that put it there
    within OTA_CONFIRM_POLL_MS of any boot (recovery-mode boots included),
    so the NEXT reboot lands back in normal mode on its own. This tool is
    the impatient/uncertain case: reboot right now instead of waiting for
    that background self-clear (or a watchdog) to do it.

    `password`: same AP-password-derived HMAC scheme as ota_update_esp()/
    ota_rollback_esp() -- but signed over yet another distinct context
    ("recovery", not "esp" or "esp-rollback"), so a MAC captured for one
    action cannot be reused to authorize this one. Refused (403) on a wrong
    password/lockout, same as the other OTA routes, and ALSO refused (403,
    "board is not in recovery mode") if the board is not currently in
    recovery mode -- that check runs after auth specifically so a caller
    who never proves they hold the AP password cannot use this to probe
    whether the board is in recovery mode.

    On success, the board is already rebooting from a short-lived
    background task -- this call returns as soon as the response arrives,
    it does NOT wait for the reboot to finish.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/HMAC/
    response-parsing are unit-tested with mocked HTTP only; no ESP32-S3 was
    available in this environment to actually trigger recovery mode and
    exit it.
    """
    resolved = _ota_resolve_host(host)
    try:
        body = ota_http.recovery_exit_esp(resolved, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not body.get("ok"):
        return f"error: board reported failure: {body} (host={resolved})"
    return (f"ok - recovery-mode exit accepted (host={resolved}) -- "
            f"board is rebooting now")


@_tool()
def ota_update_pico(image_path: str, password: str, host: Optional[str] = None) -> str:
    """Push a new RP2040 safety-processor firmware image -- POST
    /api/ota/pico. Stages `image_path` (a raw SaftyFW .bin) into the ESP's
    `pico_img` partition at Wi-Fi speed, then hands off to a background task
    that relays it to the RP2040 over the isolated UART link (~35s+,
    unacknowledged UPDATE_DATA broadcast + gap-report retransmit -- see
    CommonFW/docs/UPDATE_PROTOCOL.md section 4).

    DESTRUCTIVE-ADJACENT, and this is the SAFETY PROCESSOR: refused unless
    the same interlocks as ota_update_esp() hold, checked independently by
    BOTH processors (the Pico does not take the ESP's word for it). A
    refusal (wrong password, an interlock, or a concurrent update already
    in progress) comes back as a specific board-reported reason.

    `password`: same AP-password-derived HMAC scheme as ota_update_esp() --
    see that tool's doc comment.

    IMPORTANT: a successful response here means "the image was staged and
    the relay STARTED" -- it does NOT mean the RP2040 is now running the new
    image. The relay itself takes ~35 seconds or more and happens in the
    background after this call returns (202 Accepted). Call
    ota_status(host) afterward, repeatedly, to learn whether the relay
    actually completed, failed, or is still in progress. Nothing here
    retries a partial write on its own -- ota_pico_do_stage()'s single
    cleanup path releases the update mutex on any staging failure, and the
    relay task itself caps retransmission rounds and fails cleanly rather
    than looping forever on a bad link.

    NOT YET VERIFIED AGAINST REAL HARDWARE -- neither the ESP HTTP path nor
    the RP2040 relay has been exercised against physical boards from this
    tool; only request construction/HMAC/response-parsing are unit-tested,
    with mocked HTTP.
    """
    resolved = _ota_resolve_host(host)
    try:
        result = ota_http.push_pico_image(resolved, image_path, password)
    except ota_http.OtaHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        return f"error: {exc}{status_bit} (host={resolved})"
    if not result.ok:
        return f"error: board reported failure: {result.body} (host={resolved})"
    b = result.body
    return (f"ok - staged {b.get('bytes')} bytes, crc32={b.get('crc32')!r}, "
            f"relay started (host={resolved}) -- this does NOT mean the "
            f"RP2040 has finished updating; poll ota_status(host) for the "
            f"actual relay outcome")


@_tool()
def ota_status(host: Optional[str] = None) -> str:
    """Poll OTA update progress -- both GET /api/ota/pico/status and
    GET /api/ota/esp/status.

    Reports the background Pico relay task's phase/percent/last_error
    (ota_pico_relay.c) -- e.g. "sending" at 60%, or "done"/"failed" once the
    relay has finished -- AND the ESP's own self-update transfer phase/
    percent (ota_http_get_esp_progress()) plus the persisted "last update"
    NVS record (ota_record.h: processor, version before/after, result,
    uptime_s), via the newer /api/ota/esp/status route. Both are queried
    independently and both are reported even if one of the two calls fails
    -- a Pico-only or ESP-only failure does not hide the other's result.

    Previously an HONEST GAP (through 2026-08-18): /api/ota/esp/status did
    not exist, so the ESP self-update's own progress and the persisted
    ota_record were real, C-level state with no HTTP route. That gap is now
    closed -- see ota_http_client.py's get_esp_status().

    NOT YET VERIFIED AGAINST REAL HARDWARE -- response parsing is
    unit-tested with mocked HTTP only.
    """
    resolved = _ota_resolve_host(host)

    try:
        pico = ota_http.get_pico_status(resolved)
        pico_str = (f"phase={pico.get('phase')!r} percent={pico.get('percent')} "
                    f"last_error={pico.get('last_error')!r}")
    except ota_http.OtaHttpError as exc:
        pico_str = f"error: {exc}"

    try:
        esp = ota_http.get_esp_status(resolved)
        last_update = esp.get("last_update")
        if last_update is None:
            last_update_str = "none (no ESP update has run this boot's NVS lifetime)"
        else:
            last_update_str = (f"processor={last_update.get('processor')!r} "
                                f"{last_update.get('version_before')!r}->"
                                f"{last_update.get('version_after')!r} "
                                f"success={last_update.get('success')} "
                                f"reason={last_update.get('reason')!r} "
                                f"uptime_s={last_update.get('uptime_s')}")
        esp_str = (f"phase={esp.get('phase')!r} percent={esp.get('percent')} "
                   f"last_update: {last_update_str}")
    except ota_http.OtaHttpError as exc:
        esp_str = f"error: {exc}"

    return f"pico relay: {pico_str} (host={resolved}) | esp self-update: {esp_str} (host={resolved})"


# ---------------------------------------------------------------------------
# CONTROL (task 8) -- zone PID/model config, mirrors zones_http.c
#
# Manual relay control is NOT here -- see io_set_relay/io_set_relay_mask.
# ---------------------------------------------------------------------------
@_tool()
def control_get_zones() -> str:
    """Read every zone's current PID/model config, calibration offset and
    temperature limits, plus the thermocouple and relay counts."""
    try:
        thermo_count, relay_count, zones = _control.get_zones()
    except ControlQueryError as exc:
        return f"error: {exc}"
    header = f"{thermo_count} thermocouple(s), {relay_count} relay(s)"
    if not zones:
        return header
    return header + "\n" + "\n".join(z.describe() for z in zones)


@_tool()
def control_set_zone_pid(zone: int, kp: float, ki: float, kd: float) -> str:
    """Set a zone's PID gains."""
    try:
        result = _control.set_zone_pid(zone, kp, ki, kd)
    except ControlQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - zone {zone} PID set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set zone {zone} PID{detail}"


@_tool()
def control_set_zone_model(zone: int, k_dc: float, tau_s: float, dead_time_s: float) -> str:
    """Set a zone's feedforward thermal model (steady-state gain, time
    constant, dead time), used for model feedforward and autotune seeding."""
    try:
        result = _control.set_zone_model(zone, k_dc, tau_s, dead_time_s)
    except ControlQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - zone {zone} model set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set zone {zone} model{detail}"


# ---------------------------------------------------------------------------
# CONFIG PRESETS -- known-good starting configs for a consistent test basis.
# Data-driven (tools/PcTools/config_presets/*.json), never compiled into
# firmware -- see config_presets.py's module docstring for the full SCOPE
# rationale (only PID/model are written; relay_mask/max_temp_c/control_mode
# are read-only over this link today, hook documented there).
# ---------------------------------------------------------------------------
@_tool()
def list_config_presets() -> str:
    """List every known-good config preset (name + description).

    Presets live as JSON under ``tools/PcTools/config_presets/`` -- never
    compiled into firmware, so a bench-only setting (like a fixture's
    lowered temperature ceiling) can never leak into a real kiln build.
    """
    presets = config_presets.list_presets()
    if not presets:
        return f"no presets found under {config_presets.presets_dir()}"
    return "\n".join(f"{p['name']}: {p['description']}" for p in presets)


@_tool()
def load_config_preset(name: str, host: Optional[str] = None) -> str:
    """Apply a known-good preset's zone config to the live board.

    PID gains and (when the preset carries one) the thermal model go over
    the UART CONTROL task, same as always. `host` (new): when given, ALSO
    writes relay_mask/control_mode/max_temp_c/min_temp_c/max_ramp_c_per_hr/
    thermo_count/relay_count over GET/POST /api/zones -- the fields the UART
    link has no setter for (zones_http_client.py). That write GETs the live
    config first and merges the preset onto it (POST /api/zones is a
    whole-page-submit endpoint: a naive partial POST would zero every field
    it doesn't mention, including max_temp_c -- see that module's docstring
    for why this matters with heating elements physically connected), then
    re-reads the config and confirms it actually landed -- a preset that
    reports success without confirming is the exact defect class this repo
    has been fighting elsewhere. Host resolution matches the OTA tools: see
    `_ota_resolve_host()`.

    `host` omitted (the default): those fields are reported as
    reference/expected data only, NOT written -- unchanged from before this
    parameter existed.

    Does NOT reset, does NOT touch relays, does NOT request enable.
    """
    try:
        preset = config_presets.load_preset_data(name)
    except config_presets.ConfigPresetError as exc:
        return f"error: {exc}"
    resolved = _ota_resolve_host(host) if host else None
    try:
        result = config_presets.apply_preset(_control, preset, zones_host=resolved)
    except ControlQueryError as exc:
        return f"error: {exc}"
    except zones_http_client.ZonesHttpError as exc:
        return f"error writing zones config over HTTP (host={resolved}): {exc}"
    return result.describe()


#: FACTORY_RESET reboots ~500ms after the ACK; ESP32-S3 boot to first
#: GET_FW_VERSION push is normally a few seconds. Generous on purpose --
#: this only runs when a caller explicitly asked for a reboot.
FACTORY_RESET_REBOOT_TIMEOUT_S = 20.0


@_tool()
def factory_default_then_load_preset(name: str, scope: int = FACTORY_RESET_SCOPE_KILN,
                                      host: Optional[str] = None) -> str:
    """Factory-default the board, then apply a known-good preset -- one
    callable step so a test always starts from the same place.

    ``scope`` picks exactly what gets wiped (firmware/KilnFW/App/drivers/
    factory_reset.c's ``kScopes``, one ``nvs_flash_erase_partition()`` per
    partition named -- there is no blanket erase-everything path):
      * WIFI (0): erases ``wifi_nvs`` only -- Wi-Fi credentials/AP config.
        Zone config, saved profiles, relay cycle counters are untouched.
      * KILN (1, the default here -- matches this tool's own "consistent
        test basis" purpose): erases ``kiln_nvs`` only -- the WHOLE
        partition, wholesale, not just zones_cfg_t: zone config, relay
        names, rules, relay-cycle counters, and run_state all live there
        (see kiln.py 8.1's partition split) and all come back at their
        firmware defaults. Wi-Fi credentials and saved profiles are
        untouched, so the board stays reachable and the operator's fire
        schedules survive.
      * PROFILES (2): erases ``profiles_nvs`` AND explicitly restores every
        shipped built-in schedule to visible (``profiles_builtin_restore_
        all()``) -- "reset fire profiles" means both halves of what the
        /profiles page shows, not just clearing the user's 8 saved slots.
      * ALL (3): all three partitions, plus the PROFILES restore above.
        Drops Wi-Fi too -- the board falls back to its AP address
        (``ota_http.OTA_AP_DEFAULT_HOST``) until re-provisioned.
      In every case: FACTORY_RESET reboots the board ~500ms after the ACK
      (execute_scope()'s reboot_task) -- this call waits for the post-reboot
      firmware-version push before applying the preset, so it isn't racing
      the boot. See devices.FACTORY_RESET_SCOPE_* for the numeric values.

    `host` (new, same as load_config_preset()'s): when given, ALSO writes
    relay_mask/control_mode/max_temp_c/min_temp_c/max_ramp_c_per_hr/
    thermo_count/relay_count over GET/POST /api/zones after the reboot, with
    the same GET-merge-POST-then-verify contract load_config_preset()
    documents. Omitted (the default): those fields are reported as
    reference data only, matching this tool's pre-existing behavior. The
    reboot the KILN/ALL scopes trigger drops the board off any Wi-Fi network
    it was on only if scope includes wifi -- host resolution below still
    runs `_ota_resolve_host()` after the reboot, at whatever address is
    live then.

    This is the disruptive lever in this module: it reboots the board and
    then writes PID/model gains (and, with `host`, zones config too). Never
    invoke it against a bench with a firing in progress or with the safety
    processor ARMED.
    """
    try:
        preset = config_presets.load_preset_data(name)
    except config_presets.ConfigPresetError as exc:
        return f"error: {exc}"
    send_result = _link.send(
        dst_task=UART_TASK_ID_SYSTEM, src_task=UART_TASK_ID_SYSTEM,
        payload=devices.system_factory_reset(scope), dst_device=Device.ESP,
    )
    if not send_result.ok:
        return f"error: factory reset request was not ACKed ({send_result})"
    try:
        _info.get_fw_version(timeout=FACTORY_RESET_REBOOT_TIMEOUT_S)
    except InfoQueryError as exc:
        return (
            f"error: factory reset sent, but the board did not come back up "
            f"within {FACTORY_RESET_REBOOT_TIMEOUT_S}s ({exc}) -- preset NOT applied"
        )
    resolved = _ota_resolve_host(host) if host else None
    try:
        result = config_presets.apply_preset(_control, preset, zones_host=resolved)
    except ControlQueryError as exc:
        return f"factory reset ok, but preset apply failed: {exc}"
    except zones_http_client.ZonesHttpError as exc:
        return f"factory reset ok, PID/model applied, but zones config write failed (host={resolved}): {exc}"
    return f"factory reset ok (scope={scope})\n" + result.describe()


# ---------------------------------------------------------------------------
# PROFILES (task 9) -- fire profile CRUD + execution control, mirrors
# profiles_http.c / dashboard_http.c's /api/profile_exec*
#
# start()/stop()/pause()/resume() are the tools that actually begin, end or
# hold a firing. They carry no extra gate here beyond what the firmware
# itself enforces (relay_authority_on_blocked(), the safety-link liveness
# rule once M6 lands) -- this is not a second, weaker control path, it is the
# same one the GUI and the HTTP dashboard already use.
# ---------------------------------------------------------------------------
@_tool()
def profiles_list() -> str:
    """List every profile on the board: id, name, zone_mask, segment count.

    Covers both id spaces -- the 8 writable user slots (ids 0-7) and the
    read-only firing schedules that ship in flash (ids 128+). The listing is
    paged over the link because the full catalogue does not fit in one frame;
    that is handled here. Schedules the user has hidden are not listed.
    """
    try:
        summaries = _profiles.list_all()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if not summaries:
        return "no saved profiles"
    user = [s for s in summaries if not s.builtin]
    builtin = [s for s in summaries if s.builtin]

    def fmt(s) -> str:
        tag = " [built-in, read-only]" if s.builtin else ""
        return (
            f"#{s.id} {s.name!r} zone_mask=0x{s.zone_mask:X} "
            f"segments={s.segment_count}{tag}"
        )

    lines = [fmt(s) for s in user] or ["(no user profiles saved)"]
    if builtin:
        lines.append(f"-- {len(builtin)} built-in schedule(s) --")
        lines.extend(fmt(s) for s in builtin)
    return "\n".join(lines)


@_tool()
def profiles_get(profile_id: int) -> str:
    """Read one profile's full segment list (target_c, ramp_c_per_hr, dwell_min per segment).

    ``profile_id`` is either a writable user slot (0-7) or one of the
    read-only firing schedules shipped in flash (ids 128 and up -- see
    profiles_list). A built-in reports the zone mask it would run with, taken
    from the configured zones.
    """
    try:
        detail = _profiles.get(profile_id)
    except (ProfilesQueryError, ValueError) as exc:
        return f"error: {exc}"
    if detail is None:
        return f"no such profile #{profile_id}"
    tag = " [built-in, read-only]" if detail.builtin else ""
    lines = [f"#{detail.id} {detail.name!r} zone_mask=0x{detail.zone_mask:X}{tag}"]
    for i, seg in enumerate(detail.segments):
        lines.append(
            f"  segment {i}: target={seg.target_c:.1f}C ramp={seg.ramp_c_per_hr:.1f}C/hr "
            f"dwell={seg.dwell_min}min"
        )
    return "\n".join(lines)


@_tool()
def profiles_save(profile_id: int, name: str, zone_mask: int, segments_json: str) -> str:
    """Create (profile_id=-1) or overwrite a user profile (slots 0-7).

    ``segments_json`` is a JSON array of
    ``{"target_c": .., "ramp_c_per_hr": .., "dwell_min": ..}`` objects, one per
    segment, in firing order.

    Passing a built-in id (128+) does NOT overwrite the shipped schedule --
    those are read-only flash -- it saves the submitted profile as a copy into
    the first free user slot. The reply names the slot it landed in.
    """
    try:
        raw_segments = json.loads(segments_json)
        if not isinstance(raw_segments, list):
            return "error: segments_json must be a JSON array"
        segments = [
            devices.ProfileSegment(
                target_c=float(s["target_c"]),
                ramp_c_per_hr=float(s["ramp_c_per_hr"]),
                dwell_min=int(s["dwell_min"]),
            )
            for s in raw_segments
        ]
    except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        return f"error: could not parse segments_json: {exc}"
    pid = PROFILES_SAVE_ID_NEW if profile_id < 0 else profile_id
    try:
        result = _profiles.save(pid, name, zone_mask, segments)
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if not result.ok:
        return f"refused: {result.error}"
    suffix = f", {result.warning_count} warning(s)" if result.warning_count else ""
    return f"ok - saved as #{result.id}{suffix}"


@_tool()
def profiles_delete(profile_id: int) -> str:
    """Delete a saved user profile (slots 0-7). Refused if it is the one currently running.

    Built-in schedules (ids 128+) cannot be deleted -- they are const data in
    flash. Hide one instead, via the web UI / POST /api/profile/builtin/hide,
    which is reversible.
    """
    if profile_id_is_builtin(profile_id):
        return (
            f"refused - #{profile_id} is a built-in read-only schedule and cannot be "
            f"deleted; hide it instead (web UI / POST /api/profile/builtin/hide)"
        )
    try:
        result = _profiles.delete(profile_id)
    except (ProfilesQueryError, ValueError) as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - deleted #{profile_id}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not delete #{profile_id}{detail}"


@_tool()
def profiles_get_exec_status() -> str:
    """Read the current (or last) run's state: which profile, which segment,
    dwell/ramp state, per-zone actuals and any fault."""
    try:
        st = _profiles.get_exec_status()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    lines = [
        f"state={st.state} profile=#{st.profile_id} {st.name!r} "
        f"segment={st.segment_index}/{st.segment_count} dwelling={st.dwelling} "
        f"target={st.target_c:.1f}C elapsed={st.segment_elapsed_s}s "
        f"dwell_remaining={st.dwell_remaining_s}s ramp_lock={st.ramp_lock_held} "
        f"fault_guard={st.fault_guard}"
    ]
    for z in st.zones:
        lines.append(
            f"  zone {z.zone}: mode={z.control_mode} "
            f"actual={z.actual_c:.1f}C {'(valid)' if z.actual_valid else '(invalid)'} "
            f"duty={z.duty:.2f} relay={'on' if z.relay_commanded_on else 'off'} "
            f"faulted={z.faulted}"
        )
    return "\n".join(lines)


@_tool()
def profiles_start(profile_id: int) -> str:
    """Start firing a profile. This is the tool that turns on heat --
    same interlocks as the GUI/HTTP start button, nothing weaker.

    ``profile_id`` is a user slot (0-7) or one of the read-only schedules
    shipped in flash (ids 128+); both run the same way. A built-in fires every
    configured zone, since the catalogue itself is zone-agnostic.
    """
    try:
        result = _profiles.start(profile_id)
    except (ProfilesQueryError, ValueError) as exc:
        return f"error: {exc}"
    if not result.ok:
        return f"refused: {result.error}"
    return f"ok - firing #{profile_id}"


@_tool()
def profiles_stop() -> str:
    """Stop the current firing. Relays off."""
    try:
        result = _profiles.stop()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - stopped"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing running to stop{detail}"


@_tool()
def profiles_pause() -> str:
    """Pause the current firing (holds state; does not turn off heat outright)."""
    try:
        result = _profiles.pause()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - paused"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing running to pause{detail}"


@_tool()
def profiles_resume() -> str:
    """Resume a paused firing."""
    try:
        result = _profiles.resume()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - resumed"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing paused to resume{detail}"


@_tool()
def profiles_ack_last_run() -> str:
    """Acknowledge the last completed/faulted run, clearing it so a new one can start."""
    try:
        result = _profiles.ack_last_run()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - acknowledged"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing to acknowledge{detail}"


# ---------------------------------------------------------------------------
# AUTOTUNE (task 10) -- PID autotune, mirrors dashboard_http.c's /api/autotune*
# ---------------------------------------------------------------------------
@_tool()
def autotune_get_status() -> str:
    """Read the autotune run's current state, model fit and proposed gains."""
    try:
        st = _autotune.get_status()
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    return (
        f"state={st.state_name} method={st.method} zone={st.zone} "
        f"elapsed={st.elapsed_s}s samples={st.sample_count} "
        f"actual={st.actual_c:.1f}C{'(valid)' if st.actual_valid else '(invalid)'} "
        f"duty={st.duty:.2f} model_valid={st.model_valid} "
        f"proposed_gains={st.proposed_gains} relay_valid={st.relay_valid}"
        + (f" abort_reason={st.abort_reason!r}" if st.abort_reason else "")
    )


@_tool()
def autotune_start(
    zone: int,
    method: str,
    step_duty_or_setpoint_c: float,
    relay_d: float = -1.0,
    relay_h_c: float = -1.0,
    rule: str = "tl",
) -> str:
    """Start autotune on a zone.

    ``method`` is "step" or "relay"; ``rule`` (relay method only) is "tl"
    (Tyreus-Luyben) or "zn" (Ziegler-Nichols). For the step method,
    ``step_duty_or_setpoint_c`` is the duty step (0..1); for the relay method
    it is the target setpoint in degC, and ``relay_d``/``relay_h_c`` (duty
    amplitude / hysteresis band) must also be given.
    """
    method_map = {"step": devices.AUTOTUNE_METHOD_STEP, "relay": devices.AUTOTUNE_METHOD_RELAY}
    rule_map = {"tl": devices.AUTOTUNE_RULE_TL, "zn": devices.AUTOTUNE_RULE_ZN}
    method_val = method_map.get(method.strip().lower())
    rule_val = rule_map.get(rule.strip().lower())
    if method_val is None:
        return f"error: method must be one of {sorted(method_map)}, got {method!r}"
    if rule_val is None:
        return f"error: rule must be one of {sorted(rule_map)}, got {rule!r}"
    try:
        ok, err = _autotune.start(zone, method_val, step_duty_or_setpoint_c, relay_d, relay_h_c, rule_val)
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    return "ok - autotune started" if ok else f"refused: {err}"


@_tool()
def autotune_abort() -> str:
    """Abort the running autotune."""
    try:
        result = _autotune.abort()
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - aborted"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing running to abort{detail}"


@_tool()
def autotune_accept() -> str:
    """Accept the finished autotune's proposed gains, writing them into the zone's PID config."""
    try:
        result = _autotune.accept()
    except AutotuneQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - gains accepted"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing to accept{detail}"


# ---------------------------------------------------------------------------
# CODEC -- encode/decode a uart_protocol frame from/to JSON, no board attached
#
# For reasoning about a byte sequence pulled from a Saleae capture or a log,
# without guessing -- tools/PcTools/TODO.md capability 6. Pure functions:
# neither of these touches the link or requires a connection.
# ---------------------------------------------------------------------------
_CODEC_MSG_TYPES = {"data": MsgType.DATA, "ack": MsgType.ACK, "nack": MsgType.NACK,
                    "broadcast": MsgType.BROADCAST}
_CODEC_DEVICES = {"esp": Device.ESP, "host": Device.HOST}


def _codec_parse_device(name: str) -> "Optional[int]":
    key = name.strip().lower()
    if key in _CODEC_DEVICES:
        return int(_CODEC_DEVICES[key])
    try:
        return int(name, 0)  # accept a raw numeric device id for an unlisted/future peer
    except ValueError:
        return None


@_tool()
def codec_encode_frame(
    msg_type: str, msg_index: int, src_device: str, src_task: int,
    dst_device: str, dst_task: int, payload_hex: str = "",
) -> str:
    """Build one on-wire ``uart_protocol`` frame (byte-stuffed, delimited,
    CRC16 appended) and return it as hex.

    ``msg_type`` is one of "data", "ack", "nack", "broadcast" (the last is not
    sent on the PC<->ESP link today, only decodable -- see protocol.py's
    MsgType.BROADCAST). ``src_device``/``dst_device`` are "esp" or "host", or
    a raw integer for an unlisted peer id. ``payload_hex`` is the raw payload
    bytes as hex, empty for none.
    """
    msg_type_val = _CODEC_MSG_TYPES.get(msg_type.strip().lower())
    if msg_type_val is None:
        return f"error: msg_type must be one of {sorted(_CODEC_MSG_TYPES)}, got {msg_type!r}"
    src_dev = _codec_parse_device(src_device)
    dst_dev = _codec_parse_device(dst_device)
    if src_dev is None:
        return f"error: unrecognized src_device {src_device!r}"
    if dst_dev is None:
        return f"error: unrecognized dst_device {dst_device!r}"
    try:
        payload = bytes.fromhex(payload_hex)
    except ValueError as exc:
        return f"error: payload_hex is not valid hex: {exc}"
    try:
        frame = Frame(
            msg_type=msg_type_val, msg_index=msg_index,
            src_device=src_dev, src_task=src_task,
            dst_device=dst_dev, dst_task=dst_task,
            payload=payload,
        )
    except ValueError as exc:
        return f"error: {exc}"
    return frame.to_wire().hex()


@_tool()
def codec_decode_frame(wire_hex: str) -> str:
    """Parse one on-wire ``uart_protocol`` frame (hex, delimiters optional --
    unstuffing accepts the body with or without its surrounding 0x7E bytes)
    and report every field, including the payload as hex.

    Validates length and CRC exactly like the firmware's ``handle_raw_frame``;
    a malformed or corrupt frame comes back as an ``error:`` string rather
    than a traceback, matching how the live RX path silently drops one.
    """
    try:
        raw_wire = bytes.fromhex(wire_hex)
    except ValueError as exc:
        return f"error: wire_hex is not valid hex: {exc}"
    try:
        raw = unstuff(raw_wire)
    except (IndexError, ValueError) as exc:
        return f"error: could not unstuff: {exc}"
    try:
        frame = Frame.from_raw(raw)
    except FrameError as exc:
        return f"error: {exc}"
    def dev_name(d) -> str:
        return d.name if isinstance(d, Device) else str(d)
    return (
        f"msg_type={frame.msg_type.name} msg_index={frame.msg_index} "
        f"src=({dev_name(frame.src_device)}/{frame.src_task}) "
        f"dst=({dev_name(frame.dst_device)}/{frame.dst_task}) "
        f"payload[{len(frame.payload)}]={frame.payload.hex()}"
    )


def _json_default(value):
    """json.dumps(default=...) for the dataclass fields get_board_state
    collects: bytes -> hex, anything else json doesn't already know -> str().
    IntEnum members serialize as plain ints without help from this."""
    if isinstance(value, (bytes, bytearray)):
        return value.hex()
    return str(value)


def _sanitize_nan(value):
    """Recursively replace NaN/Infinity floats with None (JSON null).

    Firmware readings carry NaN as "no valid reading" (see ThermoReading and
    SafetyStatus in devices.py -- an SPI-failed thermocouple channel or a
    safety processor that has never reported both arrive this way).
    ``json.dumps`` accepts NaN/Infinity by default and emits the invalid
    (non-standard) ``NaN``/``Infinity`` tokens rather than raising, which a
    strict JSON client can't parse. This repo's own convention (dashboard
    fields throughout the C firmware) is "null until valid", so board-state
    JSON follows the same rule instead of leaking a NaN literal.
    """
    if isinstance(value, float):
        return None if (math.isnan(value) or math.isinf(value)) else value
    if isinstance(value, dict):
        return {k: _sanitize_nan(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_sanitize_nan(v) for v in value]
    return value


def _snapshot_section(fn):
    """Runs one board_state section, returning its data or an {"error": ...}
    entry -- one subsystem timing out (e.g. no safety processor attached)
    must not blank out every other section's data."""
    try:
        result = fn()
    except Exception as exc:  # noqa: BLE001 - deliberately broad, see docstring
        return {"error": str(exc)}
    if dataclasses.is_dataclass(result):
        return dataclasses.asdict(result)
    if isinstance(result, list):
        return [dataclasses.asdict(x) if dataclasses.is_dataclass(x) else x for x in result]
    return result


def _control_zones_dict() -> dict:
    thermo_count, relay_count, zones = _control.get_zones()
    return {
        "thermo_count": thermo_count,
        "relay_count": relay_count,
        "zones": [dataclasses.asdict(z) for z in zones],
    }


@_tool()
def get_board_state() -> str:
    """One-call snapshot: firmware version, pin config, every thermocouple
    reading, IO/relay state, safety status + link stats, Wi-Fi status,
    zone config, profile execution status and autotune status -- as JSON.

    Most diagnosis starts by asking for all of it; one call here replaces
    calling get_fw_version/get_pin_config/thermo_read/io_read/
    safety_get_status/... separately. Each section fails independently --
    e.g. a board with no safety processor fitted still returns a full
    snapshot, with "safety" reporting its own link_up=false rather than the
    whole call erroring out.
    """
    state = {
        "fw_version": _snapshot_section(_info.get_fw_version),
        "pin_config": _snapshot_section(_info.get_pin_config),
        "thermo": _snapshot_section(_thermo.read),
        "io": _snapshot_section(_io.read),
        "safety_status": _snapshot_section(_safety.get_status),
        "safety_link_stats": _snapshot_section(_safety.get_link_stats),
        "wifi_status": _snapshot_section(_wifi.get_status),
        "control_zones": _snapshot_section(_control_zones_dict),
        "profiles_exec_status": _snapshot_section(_profiles.get_exec_status),
        "autotune_status": _snapshot_section(_autotune.get_status),
    }
    return json.dumps(_sanitize_nan(state), default=_json_default, indent=2)


# ---------------------------------------------------------------------------
# INFO queries (task 3)
#
# Unlike the command tools these return real device data: INFO is a query
# channel, so the ACK only confirms delivery and the answer arrives in a
# separate DATA frame (see info.py).
# ---------------------------------------------------------------------------
@_tool()
def get_pin_config() -> str:
    """Report which GPIOs the running firmware has wired up, and to what.

    Read live from the device, so it reflects what is actually flashed rather
    than a host-side table. Only real ESP32-S3 GPIOs appear here -- the relay
    drives, DRDY inputs and the display's D/C and ~RESET are expander pins,
    reported by io_read() instead.
    """
    try:
        entries = _info.get_pin_config()
    except InfoQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "device reported no pin entries"
    return "\n".join(f"GPIO{e.gpio}: {e.label} [{e.abbrev}]" for e in entries)


@_tool()
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
        entries = _info.get_stack_margin()
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


@_tool()
def get_fw_version() -> str:
    """Report the running firmware's git commit, dirty flag, build time, and
    whether its UART protocol version matches this copy of pc_tools.

    Call this before any device tool: they all refuse to send until a
    compatible version has been confirmed this way (or via a boot push). This
    gate matters here -- a v1 firmware is the unit-test fixture, where task 1
    is a DAC rather than three thermocouples.
    """
    try:
        version = _info.get_fw_version()
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


# ---------------------------------------------------------------------------
# generic button press (actions.py)
#
# Every tool above is also reachable through here by name -- this exists so
# an agent doesn't need a bespoke tool per GUI button/menu-item, and so a
# future GUI button automatically gets MCP coverage the moment it's added to
# actions.py, without a matching @_tool() having to be hand-written too.
# ---------------------------------------------------------------------------
@_tool()
def list_buttons() -> str:
    """List every button/action press_button can invoke, with its parameters.

    Names match the GUI's own button/menu-item labels 1:1 (e.g. "Thermo: Read
    All", "IO: Set Relay"), grouped by popup/menu.
    """
    lines = []
    for name in sorted(actions.ACTIONS):
        action = actions.ACTIONS[name]
        params = ", ".join(f"{p}: {t.__name__}" for p, t in action.params.items())
        lines.append(f"{name}({params})\n    {action.description}")
    return "\n".join(lines)


@_tool()
def press_button(name: str, params: Optional[dict[str, Any]] = None) -> str:
    """Press any GUI button/menu-item by name -- call list_buttons() first.

    `params` is a JSON object matching that action's parameter names, e.g.
    press_button("IO: Set Relay", {"relay": 1, "on": true})
    or press_button("Thermo: Read All") for an action that takes none.
    """
    action = actions.ACTIONS.get(name)
    if action is None:
        return f"error: unknown action {name!r}. Call list_buttons() for the full list."
    if params is not None and not isinstance(params, dict):
        return f"error: params must be a JSON object of named arguments, got {type(params).__name__}"
    try:
        return action.run(_action_ctx, **(params or {}))
    except TypeError as exc:
        return f"error: bad arguments for {name!r}: {exc}"
    except ValueError as exc:
        return f"error: {exc}"
    except Exception as exc:  # noqa: BLE001 - an action must never raise into the transport
        log.exception("unexpected error in action %r", name)
        return f"error: unexpected {type(exc).__name__} in {name!r}: {exc}"


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
    _session_log.close()


def main() -> int:
    """Sync entry point for the ``kilnctrl-mcp-server`` console script."""
    return serve(mcp, name="kilnctrl", default_port=mcp_facade.DEFAULT_PORT, on_close=_close)


if __name__ == "__main__":
    raise SystemExit(main())
