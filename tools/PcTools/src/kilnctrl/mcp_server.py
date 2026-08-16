#!/usr/bin/env python3
"""MCP server (stdio) exposing the KilnCtrl board's UART control link as tools.

Wraps a single process-wide :class:`~kilnctrl.serial_link.UartLink` so an
agent can discover the board's USB-UART bridge port, connect, and drive the
kiln controller's hardware over the hardened UART protocol:

* **THERMO** (task 1) -- three MAX31856 thermocouple channels on J6
* **IO** (task 2) -- the SX1509: four relays, seven digital I/O, DRDY inputs
* **DISPLAY** (task 4) -- the ILI9488 480x320 TFT on J2
* **SAFETY** (task 7) -- the opto-isolated link to the RP2040 safety processor
* **INFO/LOG/SYSTEM** (3/5/6) -- pin config, firmware version, console, recovery

Also exposes ``flash_firmware()``/``kill_openocd_sessions()`` -- unrelated to
the UART link above, these drive OpenOCD directly over JTAG to (re)flash the
board. See that section's own comment for why they exist: use them instead
of improvising raw ``openocd`` commands by hand.

Every *command* tool returns the SendResult as text:
  ``ok`` / ``undeliverable`` / ``timeout`` / ``not_connected``.

Important limitation: ``ok`` only means the frame was delivered to the ESP
task's inbox. uart_bridge.c ACKs at the protocol layer and never sends an
application-level status frame back for a command, so there is no way from the
PC to learn whether e.g. the I2C write to the SX1509 actually succeeded. The
*query* tools (thermo_read, io_read, display_read_id, safety_get_status,
get_pin_config, get_fw_version) are the exception -- those return real device
data.
"""

from __future__ import annotations

import asyncio
import functools
import glob
import logging
import os
import subprocess
import sys
import threading
import time
from collections import deque
from typing import Any, Optional

try:
    # mcp >= 2.0 renamed FastMCP to MCPServer (same decorator/run API).
    from mcp.server.mcpserver import MCPServer as _McpServer
except ImportError:  # pragma: no cover - mcp 1.x
    from mcp.server.fastmcp import FastMCP as _McpServer

from . import actions, devices, settings
from .device_log import LogClient
from .devices import LogLine
from .display import BlitError, DisplayClient, DisplayQueryError
from .info import InfoClient, InfoQueryError
from .io_expander import IoClient, IoQueryError
from .link_hub import get_shared_link
from .protocol import (
    THERMO_CHANNEL_ALL,
    UART_TASK_ID_DISPLAY,
    UART_TASK_ID_IO,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    UART_TASK_ID_THERMO,
    LogLevel,
)
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

#: Recent firmware log lines (task LOG), for the get_device_log tool below --
#: an MCP client has no GUI terminal to watch live, so this is the pull-based
#: equivalent of the GUI's Device Console window.
_DEVICE_LOG_HISTORY = 200
_device_log_history: "deque[LogLine]" = deque(maxlen=_DEVICE_LOG_HISTORY)

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
    with _history_lock:
        _device_log_history.append(line)
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
_safety = SafetyClient(_link)

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
                SafetyQueryError,
                InfoQueryError,
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
    lines = _snapshot(_device_log_history, n)
    if not lines:
        return "(no device log lines received yet)"
    return "\n".join(f"{line.letter} {line.text}" for line in lines)


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
    """
    def _shutdown() -> None:
        time.sleep(0.2)  # let the stdio transport flush this tool's reply first
        for client in (_info, _device_log, _thermo, _io, _display, _safety):
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
    """Locates openocd.exe without relying on `export.ps1` having run in this
    process's environment (the MCP server is a plain Python process, not a
    ESP-IDF shell). Checks the actual installed-tools layout on this machine
    first (~/.espressif/tools/openocd-esp32/<version>/openocd-esp32/bin/), then
    falls back to the C:\\Espressif path .vscode/tasks.json's "Flash device"
    task hardcodes (kept only as a fallback since it was found to be stale/
    wrong for this machine when this tool was written)."""
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    candidates = glob.glob(
        os.path.join(home, ".espressif", "tools", "openocd-esp32", "*", "openocd-esp32", "bin", "openocd.exe")
    )
    candidates += glob.glob(r"C:\Espressif\tools\openocd-esp32\*\openocd-esp32\bin\openocd.exe")
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def _kiln_fw_root() -> str:
    """firmware/KilnFW/ project root.

    This file lives at tools/PcTools/src/kilnctrl/, so the repo root is four
    levels up. These tools serve both processors and no longer sit inside the
    main firmware, which is why this is an explicit path rather than a walk up
    to the parent directory.
    """
    repo_root = os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
    )
    return os.path.join(repo_root, "firmware", "KilnFW")


def _run_openocd(openocd_exe: str, board_cfg_relpath: str, tcl_commands: str, cwd: str, timeout_s: int) -> tuple[bool, str]:
    scripts_dir = os.path.normpath(os.path.join(os.path.dirname(openocd_exe), "..", "share", "openocd", "scripts"))
    cmd = [openocd_exe, "-s", scripts_dir, "-f", board_cfg_relpath, "-c", tcl_commands]
    try:
        proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired as exc:
        return False, f"openocd timed out after {timeout_s}s\n{exc.stdout or ''}\n{exc.stderr or ''}"
    output = (proc.stdout or "") + (proc.stderr or "")
    # A region whose content already matches the file skips straight to
    # "Resetting Target" without ever printing "Verify OK" -- that's still a
    # real success (found the hard way: this check originally required
    # "Verify OK" to appear, which false-negatived on an otherwise-correct
    # run where nothing needed rewriting). returncode==0 with no failure
    # marker is what actually signals success here.
    ok = proc.returncode == 0 and "Verify Failed" not in output and "Error:" not in output
    return ok, output


@_tool()
def kill_openocd_sessions() -> str:
    """Force-kills any running openocd.exe processes on this PC.

    Use before flash_firmware() (it already does this itself) or on its own
    if a manual/GDB OpenOCD session was left attached and is holding the
    JTAG interface -- a second openocd instance can't open the USB-JTAG
    device while one is already running, which looks like "the board isn't
    responding" but is actually just this. Safe to call when nothing is
    running (reports that plainly, not an error)."""
    try:
        proc = subprocess.run(
            ["taskkill", "/F", "/IM", "openocd.exe"], capture_output=True, text=True, timeout=10
        )
    except Exception as exc:  # noqa: BLE001
        return f"error: could not run taskkill: {exc}"
    if proc.returncode == 0:
        return "killed running openocd.exe process(es)"
    return "no running openocd.exe process found"


@_tool()
def flash_firmware(board_cfg: str = "board/esp32s3-builtin.cfg", retry_once: bool = True) -> str:
    """Flashes KilnFW/build/{bootloader,partition_table,KilnCtrl}.bin to the
    board over JTAG via OpenOCD -- the ONLY sanctioned way to flash this
    board (never esptool/`idf.py flash`, per CLAUDE.md). Always writes all
    three images (bootloader @0x0, partition table @0x8000, app @0x10000),
    each with `program_esp ... verify` (which only erases/rewrites a region
    if its content doesn't already match -- it does NOT do a bare full-chip
    erase), ending in a reset so the board boots the new app immediately.

    Requires `idf.py build` to have already produced KilnFW/build/*.bin --
    this tool does not build, only flashes.

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

    kill_openocd_sessions()  # a stale session holding the JTAG interface looks identical to a flash failure

    tcl = (
        "program_esp build/bootloader/bootloader.bin 0x0 verify; "
        "program_esp build/partition_table/partition-table.bin 0x8000 verify; "
        "program_esp build/KilnCtrl.bin 0x10000 verify reset exit"
    )
    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=kiln_fw_root, timeout_s=90)
    if ok:
        return "flashed and verified OK (bootloader + partition table + app), board reset and running"

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
    return _send(
        UART_TASK_ID_THERMO,
        devices.thermo_config_channel(channel, tc_type, avg_mode, filter_50hz, auto_convert),
    )


@_tool()
def thermo_set_thresholds(
    channel: int, tc_high: float, tc_low: float, cj_high: int, cj_low: int
) -> str:
    """Set one channel's TC high/low trip temperatures and CJ high/low limits (degC)."""
    return _send(
        UART_TASK_ID_THERMO,
        devices.thermo_set_thresholds(channel, tc_high, tc_low, cj_high, cj_low),
    )


@_tool()
def thermo_set_cj_offset(channel: int, offset_c: float) -> str:
    """Set one channel's cold-junction offset in degC (-8..+8)."""
    return _send(UART_TASK_ID_THERMO, devices.thermo_set_cj_offset(channel, offset_c))


@_tool()
def thermo_one_shot(channel: int) -> str:
    """Trigger a single conversion on one channel (poll with thermo_read after)."""
    return _send(UART_TASK_ID_THERMO, devices.thermo_one_shot(channel))


@_tool()
def thermo_clear_faults(channel: int) -> str:
    """Pulse CR0.FAULTCLR on one channel.

    Only meaningful in the part's interrupt fault mode; in the comparator mode
    this driver uses, fault bits clear themselves when the condition clears.
    """
    return _send(UART_TASK_ID_THERMO, devices.thermo_clear_faults(channel))


@_tool()
def thermo_set_auto_report(channel_mask: int = 0x07, period_ms: int = 1000) -> str:
    """Have the firmware push readings for the masked channels every period_ms.

    Bit N of `channel_mask` selects channel N; `period_ms` 0 turns it off.
    Pushed readings are buffered here -- read them with thermo_get_reports().
    """
    return _send(
        UART_TASK_ID_THERMO, devices.thermo_set_auto_report(channel_mask, period_ms)
    )


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
    return _send(UART_TASK_ID_THERMO, devices.thermo_write_reg(channel, reg, value))


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
    """
    return _send(UART_TASK_ID_IO, devices.io_set_relay(relay, on))


@_tool()
def io_set_relay_mask(mask: int, value: int) -> str:
    """Switch several relays in one atomic register write.

    Bits 0-3 are Relay1..Relay4 (K3/J8, K1/J3, K2/J4, K5/J11) in both `mask`
    (which relays to change) and `value` (their new levels).
    """
    return _send(UART_TASK_ID_IO, devices.io_set_relay_mask(mask, value))


@_tool()
def io_all_relays_off() -> str:
    """De-energize all four relays unconditionally.

    The same state the firmware falls back to on link loss or a safety fault.
    """
    return _send(UART_TASK_ID_IO, devices.io_all_relays_off())


@_tool()
def io_set_output(io: int, level: bool) -> str:
    """Drive digital I/O 1-7 high or low (only meaningful when it is an output).

    IO1 = opto-isolated input from J24, IO2 = opto-isolated output to J25,
    IO3/IO4 = J20 pins 1/2, IO5/IO6 = J21 pins 1/2, IO7 = J23 pin 1.
    """
    return _send(UART_TASK_ID_IO, devices.io_set_io(io, level))


@_tool()
def io_set_direction(io: int, is_input: bool, pullup: bool = False) -> str:
    """Set digital I/O 1-7 as an input (with optional pull-up) or an output."""
    return _send(UART_TASK_ID_IO, devices.io_set_io_dir(io, is_input, pullup))


@_tool()
def io_set_auto_report(period_ms: int = 500) -> str:
    """Have the firmware push expander state every period_ms (0 = off).

    Pushes also happen immediately on every ~INT edge, so an input change is
    reported without waiting out the period. Read them with io_get_reports().
    """
    return _send(UART_TASK_ID_IO, devices.io_set_auto_report(period_ms))


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
    return _send(UART_TASK_ID_IO, devices.sx_write_reg(reg, value))


@_tool()
def expander_set_dir(mask: int) -> str:
    """Write RegDir: u16, bit N = 1 makes expander pin N an input."""
    return _send(UART_TASK_ID_IO, devices.sx_set_dir(mask))


@_tool()
def expander_set_pullup(mask: int) -> str:
    """Write RegPullUp: u16, bit N = 1 enables pin N's pull-up."""
    return _send(UART_TASK_ID_IO, devices.sx_set_pullup(mask))


@_tool()
def expander_set_opendrain(mask: int) -> str:
    """Write RegOpenDrain: u16, bit N = 1 makes pin N open-drain."""
    return _send(UART_TASK_ID_IO, devices.sx_set_opendrain(mask))


@_tool()
def expander_set_debounce(enable_mask: int, config: int = 0) -> str:
    """Enable debounce on the masked pins; `config` 0-7 selects 0.5ms << config."""
    return _send(UART_TASK_ID_IO, devices.sx_set_debounce(enable_mask, config))


@_tool()
def expander_set_int_mask(mask: int, sense: int = 0) -> str:
    """Write RegInterruptMask (bit N = 1 DISABLES pin N's interrupt) and RegSense.

    `sense` is 2 bits per pin *pair*, exactly as the part encodes them.
    """
    return _send(UART_TASK_ID_IO, devices.sx_set_int_mask(mask, sense))


@_tool()
def expander_led_driver(pin: int, enable: bool, intensity: int = 0) -> str:
    """Enable the SX1509's LED driver on one pin (0-15).

    `intensity` 0-255, where 0 is *full on* for this part's sink driver.
    """
    return _send(UART_TASK_ID_IO, devices.sx_led_driver(pin, enable, intensity))


@_tool()
def expander_reset(hard: bool = False) -> str:
    """Reset the SX1509: software reset via RegReset, or pulse ~RESET (GPIO10)."""
    return _send(UART_TASK_ID_IO, devices.sx_reset(hard))


# ---------------------------------------------------------------------------
# DISPLAY -- ILI9488 480x320 on J2 (task 4)
#
# D/C and ~RESET are on the expander, so every command/data transition costs
# an I2C transfer -- full-screen work goes through fill_rect or a blit, never
# repeated small writes. Colors are RGB565 u16; build one with
# display_rgb565() if you think in 8-bit RGB.
# ---------------------------------------------------------------------------
@_tool()
def display_read_id() -> str:
    """Read the panel's RDDID bytes and its current (rotated) width/height.

    A query, and the only way to distinguish a wired-up panel from every
    drawing command vanishing into an unconnected connector.
    """
    try:
        ident = _display.read_id()
    except DisplayQueryError as exc:
        return f"error: {exc}"
    return ident.describe()


@_tool()
def display_rgb565(r: int, g: int, b: int) -> str:
    """Convert 8-bit R/G/B into the RGB565 integer the other display tools take.

    Host-side only -- no device round trip.
    """
    try:
        color = devices.rgb565(r, g, b)
    except ValueError as exc:
        return f"error: {exc}"
    return f"{color} (0x{color:04X})"


@_tool()
def display_reset(hard: bool = False) -> str:
    """Reset the panel: software reset command, or pulse ~RESET via the expander."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_reset(hard))


@_tool()
def display_set_power(on: bool) -> str:
    """Turn the panel on, or off (display-off + sleep-in)."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_set_power(on))


@_tool()
def display_set_rotation(rotation: int) -> str:
    """Set MADCTL rotation 0-3: 0/2 portrait 320x480, 1/3 landscape 480x320."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_set_rotation(rotation))


@_tool()
def display_set_invert(invert: bool) -> str:
    """Invert (or restore) the panel's display polarity."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_set_invert(invert))


@_tool()
def display_clear(color: int = 0x0000) -> str:
    """Fill the whole screen with one RGB565 color (default black)."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_clear(color))


@_tool()
def display_fill_rect(x: int, y: int, w: int, h: int, color: int) -> str:
    """Fill a rectangle with an RGB565 color."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_fill_rect(x, y, w, h, color))


@_tool()
def display_draw_rect(x: int, y: int, w: int, h: int, color: int) -> str:
    """Draw a 1px rectangle outline in an RGB565 color."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_draw_rect(x, y, w, h, color))


@_tool()
def display_draw_line(x0: int, y0: int, x1: int, y1: int, color: int) -> str:
    """Draw a line from (x0,y0) to (x1,y1) in an RGB565 color."""
    return _send(
        UART_TASK_ID_DISPLAY, devices.display_draw_line(x0, y0, x1, y1, color)
    )


@_tool()
def display_set_text_cursor(x: int, y: int) -> str:
    """Move the text cursor to a pixel position (top-left of the next glyph)."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_set_text_cursor(x, y))


@_tool()
def display_set_text_style(
    fg: int, bg: int = 0x0000, size: int = 1, opaque_background: bool = True
) -> str:
    """Set text colors (RGB565), integer scale 1-8, and background opacity."""
    return _send(
        UART_TASK_ID_DISPLAY,
        devices.display_set_text_style(fg, bg, size, opaque_background),
    )


@_tool()
def display_print(text: str) -> str:
    """Draw ASCII text at the cursor, which advances and wraps at the right edge."""
    return _send(UART_TASK_ID_DISPLAY, devices.display_print(text))


@_tool()
def display_send_image(
    path: str,
    x: int = 0,
    y: int = 0,
    width: int = devices.DISPLAY_NATIVE_WIDTH,
    height: int = devices.DISPLAY_NATIVE_HEIGHT,
    fit: bool = True,
) -> str:
    """Load an image file, scale it, and stream it to the panel.

    Uses BLIT_BEGIN/BLIT_DATA/BLIT_END. `fit` letterboxes to preserve aspect
    ratio; fit=false stretches to exactly width x height. Requires Pillow.

    This is slow by construction: RGB565 at 115200 baud is roughly 63 pixels
    per protocol frame, so a full 480x320 image is ~2440 frames and takes on
    the order of a minute, filling the panel top-to-bottom as it arrives.
    """
    if _info.compatible is not True:
        return "error: refused - firmware version not confirmed (call get_fw_version first)"
    from .display import image_to_rgb565

    try:
        pixels, out_w, out_h = image_to_rgb565(path, width, height, fit=fit)
    except RuntimeError as exc:  # Pillow missing -- carries the install hint
        return f"error: {exc}"
    except (OSError, ValueError) as exc:
        return f"error: could not load {path}: {exc}"
    try:
        frames = _display.blit(x, y, out_w, out_h, pixels)
    except (BlitError, ValueError) as exc:
        return f"error: {exc}"
    return f"ok - streamed {out_w}x{out_h} at ({x},{y}) in {frames} frames"


@_tool()
def display_test_pattern(
    x: int = 0,
    y: int = 0,
    width: int = devices.DISPLAY_NATIVE_WIDTH,
    height: int = devices.DISPLAY_NATIVE_HEIGHT,
) -> str:
    """Stream colour bars plus a grey ramp to the panel.

    Exercises the whole blit path with no image file and no Pillow: the
    saturated bars make a swapped or truncated colour channel obvious, and the
    ramp catches an RGB565->RGB666 expansion that has lost its low bits.
    """
    if _info.compatible is not True:
        return "error: refused - firmware version not confirmed (call get_fw_version first)"
    from .display import test_pattern_rgb565

    try:
        pixels = test_pattern_rgb565(width, height)
        frames = _display.blit(x, y, width, height, pixels)
    except (BlitError, ValueError) as exc:
        return f"error: {exc}"
    return f"ok - streamed {width}x{height} test pattern in {frames} frames"


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

    Advisory only: the Pico can refuse, and its own interlocks always win.
    """
    return _send(UART_TASK_ID_SAFETY, devices.safety_request_enable(enable))


@_tool()
def safety_ping() -> str:
    """Force an immediate poll of the safety processor."""
    return _send(UART_TASK_ID_SAFETY, devices.safety_ping())


@_tool()
def safety_set_poll_period(period_ms: int) -> str:
    """Set how often the ESP polls the safety processor, in ms (0 stops polling)."""
    return _send(UART_TASK_ID_SAFETY, devices.safety_set_poll_period(period_ms))


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
    return _send(UART_TASK_ID_SAFETY, devices.safety_set_fault_out(assert_fault))


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
# entry point
# ---------------------------------------------------------------------------
async def _run() -> None:
    await mcp.run_stdio_async()


def main() -> int:
    """Sync entry point for the ``kilnctrl-mcp-server`` console script."""
    logging.basicConfig(
        level=logging.INFO,
        stream=sys.stderr,  # stdout is the MCP transport; never log there
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        pass
    finally:
        for client in (_info, _device_log, _thermo, _io, _display, _safety):
            client.close()
        # close(), not disconnect(): this link may be shared with another
        # process (see link_hub.py) -- exiting shouldn't yank the physical
        # port out from under it. The disconnect MCP tool is the only thing
        # that should do that.
        _link.close()
        _session_log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
