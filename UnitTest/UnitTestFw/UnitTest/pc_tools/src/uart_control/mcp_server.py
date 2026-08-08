#!/usr/bin/env python3
"""MCP server (stdio) exposing the ESP32-S3 UART control link as tools.

Wraps a single process-wide :class:`~uart_control.serial_link.UartLink` so an
agent can discover the board's USB-UART bridge port, connect, and drive the
MCP4728 DAC / AD9833 function generator over the hardened UART protocol.

Every command tool returns the SendResult as text:
  ``ok`` / ``undeliverable`` / ``timeout`` / ``not_connected``.

Important limitation: ``ok`` only means the frame was delivered to the ESP
task's inbox. uart_bridge.c ACKs at the protocol layer and never sends an
application-level status frame back, so there is no way from the PC to learn
whether e.g. the I2C write to the DAC actually succeeded.
"""

from __future__ import annotations

import asyncio
import logging
import os
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
from .info import InfoClient, InfoQueryError
from .link_hub import get_shared_link
from .protocol import (
    UART_TASK_ID_AD9833,
    UART_TASK_ID_DAC,
    UART_TASK_ID_OLED,
    UART_TASK_ID_SYSTEM,
    LogLevel,
)
from .serial_link import list_ports, recommend_port
from .session_log import SessionLogger

log = logging.getLogger(__name__)

mcp = _McpServer("uart-control")

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
_session_log = SessionLogger(name="uart_control.mcp_session")


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

_DEVICE_LOG_SESSION_METHOD = {
    LogLevel.ERROR: _session_log.error,
    LogLevel.WARN: _session_log.warning,
}


def _on_device_log_line(line: LogLine) -> None:
    """LogClient's consumer thread: buffer for get_device_log, and mirror
    into the persistent session log so it's not lost between MCP calls."""
    _device_log_history.append(line)
    method = _DEVICE_LOG_SESSION_METHOD.get(line.level, _session_log.info)
    method("device: %s", line.text)


#: Owns task LOG (5) so the firmware's forwarded ESP_LOGx output (see
#: App/drivers/uart_log_bridge.c) has somewhere to land -- registered up
#: front for the same "don't miss the boot-time backlog" reason as _info.
_device_log = LogClient(_link, on_line=_on_device_log_line)

#: Backs the generic press_button/list_buttons tools (actions.py) with the
#: same link/info/log this module's own bespoke tools use, so both paths
#: hit the identical underlying state.
_action_ctx = actions.ActionContext(link=_link, info=_info, session_log=_session_log)


def _send(dst_task: int, payload: bytes) -> str:
    """Send a command payload, using the same numeric id as our src_task.

    The PC mirrors the firmware's task numbering (uart_task_ids.h), so the DAC
    channel is task 1 on both sides and the AD9833 channel is task 2.

    Refuses to send if the connected firmware's protocol version doesn't
    match ours (or isn't known yet) -- see UART_PROTOCOL_VERSION in
    protocol.py. INFO queries themselves are exempt (see get_pin_config /
    get_fw_version below): that's how compatibility gets discovered.
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
@mcp.tool()
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


@mcp.tool()
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
    # the other trigger (device reboot) is handled in _on_boot_push below.
    _session_log.start_session(f"connected to {opened} @ {_link.baudrate} baud")
    settings.set_last_port(opened)
    return f"connected to {opened} @ {_link.baudrate} baud{suffix}"


@mcp.tool()
def disconnect() -> str:
    """Close the serial link."""
    if not _link.is_connected:
        return "not connected"
    port = _link.port
    _link.disconnect()
    _session_log.info("disconnected from %s", port)
    return f"disconnected from {port}"


@mcp.tool()
def link_status() -> str:
    """Report connection state, port, baud rate and protocol settings."""
    status = _link.status()
    recommended = recommend_port()
    status["recommended_port"] = recommended
    return "\n".join(f"{k}: {v}" for k, v in status.items())


@mcp.tool()
def get_device_log(n: int = 50) -> str:
    """Return the last ``n`` lines of firmware console output (ESP_LOGx).

    These are forwarded over the reliable UART link instead of going to the
    USB-Serial-JTAG console (see App/drivers/uart_log_bridge.c), so they're
    visible here without a debugger/second cable attached -- this is the
    pull-based equivalent of the GUI's Device Console window.
    """
    if n <= 0:
        return "error: n must be positive"
    lines = list(_device_log_history)[-n:]
    if not lines:
        return "(no device log lines received yet)"
    return "\n".join(f"{line.letter} {line.text}" for line in lines)


@mcp.tool()
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
        _info.close()
        _device_log.close()
        _link.close()
        _session_log.close()
        os._exit(0)

    threading.Thread(target=_shutdown, daemon=True, name="mcp-shutdown").start()
    return "shutting down"


@mcp.tool()
def restart_uart() -> str:
    """On-demand recovery lever for a stuck/desynced UART link.

    Tells the ESP to flush its UART RX ring buffer and reset its rx-error
    counter (SYSTEM_CMD_RESTART_UART) -- useful if the link seems wedged,
    without power-cycling the board. Does not disconnect or reopen the local
    serial port; that's what disconnect()/connect() are for.
    """
    return _send(UART_TASK_ID_SYSTEM, devices.system_restart_uart())


# ---------------------------------------------------------------------------
# MCP4728 DAC (task 1)
# ---------------------------------------------------------------------------
@mcp.tool()
def dac_set_channel_percent(channel: int, percent: float) -> str:
    """Set one DAC channel (0-3) to a 0-100 percent full-scale output."""
    return _send(UART_TASK_ID_DAC, devices.dac_set_channel_percent(channel, percent))


@mcp.tool()
def dac_set_all_percent(p0: float, p1: float, p2: float, p3: float) -> str:
    """Set all four DAC channels at once, each 0-100 percent full scale."""
    return _send(UART_TASK_ID_DAC, devices.dac_set_all_percent([p0, p1, p2, p3]))


@mcp.tool()
def dac_power_down(channel: int, mode: int = 0) -> str:
    """Power down DAC channel (0-3) with power-down mode 0-3."""
    return _send(UART_TASK_ID_DAC, devices.dac_power_down(channel, mode))


# ---------------------------------------------------------------------------
# AD9833 function generator (task 2)
# ---------------------------------------------------------------------------
@mcp.tool()
def ad9833_set_frequency(reg: int, freq_hz: float) -> str:
    """Write a frequency in Hz into FREQ0 (reg=0) or FREQ1 (reg=1)."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_set_frequency(reg, freq_hz))


@mcp.tool()
def ad9833_set_phase(reg: int, degrees: float) -> str:
    """Write a phase offset in degrees into PHASE0 (reg=0) or PHASE1 (reg=1)."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_set_phase(reg, degrees))


@mcp.tool()
def ad9833_set_waveform(waveform: int) -> str:
    """Set the output waveform: 0=sine, 1=triangle, 2=square, 3=square/2."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_set_waveform(waveform))


@mcp.tool()
def ad9833_select_freq_reg(reg: int) -> str:
    """Select which frequency register drives the output (0=FREQ0, 1=FREQ1)."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_select_freq_reg(reg))


@mcp.tool()
def ad9833_select_phase_reg(reg: int) -> str:
    """Select which phase register drives the output (0=PHASE0, 1=PHASE1)."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_select_phase_reg(reg))


@mcp.tool()
def ad9833_reset(hold: bool) -> str:
    """Assert (hold=true) or release (hold=false) the AD9833 reset bit."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_reset(hold))


@mcp.tool()
def ad9833_sleep(dac_power_down: bool, mclk_power_down: bool) -> str:
    """Power down the AD9833's internal DAC and/or its MCLK."""
    return _send(UART_TASK_ID_AD9833, devices.ad9833_sleep(dac_power_down, mclk_power_down))


# ---------------------------------------------------------------------------
# SSD1306 OLED (task 4)
# ---------------------------------------------------------------------------
@mcp.tool()
def oled_clear() -> str:
    """Clear the OLED's in-RAM framebuffer (does not touch the panel yet)."""
    return _send(UART_TASK_ID_OLED, devices.oled_clear())


@mcp.tool()
def oled_set_cursor(col: int, row: int) -> str:
    """Move the OLED's text cursor to (col, row) in text-cell coordinates."""
    return _send(UART_TASK_ID_OLED, devices.oled_set_cursor(col, row))


@mcp.tool()
def oled_print(text: str) -> str:
    """Write text into the OLED's framebuffer at the cursor (call oled_display to show it)."""
    return _send(UART_TASK_ID_OLED, devices.oled_print(text))


@mcp.tool()
def oled_display() -> str:
    """Flush the OLED's framebuffer to the panel."""
    return _send(UART_TASK_ID_OLED, devices.oled_display())


@mcp.tool()
def oled_set_contrast(contrast: int) -> str:
    """Set OLED contrast, 0-255."""
    return _send(UART_TASK_ID_OLED, devices.oled_set_contrast(contrast))


@mcp.tool()
def oled_set_invert(invert: bool) -> str:
    """Invert (or restore) the OLED's display polarity."""
    return _send(UART_TASK_ID_OLED, devices.oled_set_invert(invert))


@mcp.tool()
def oled_set_power(on: bool) -> str:
    """Turn the OLED panel on or off (sleep)."""
    return _send(UART_TASK_ID_OLED, devices.oled_set_power(on))


# ---------------------------------------------------------------------------
# INFO queries (task 3)
#
# Unlike every tool above, these return real device data rather than a
# SendResult: INFO is a query channel, so the ACK only confirms delivery and
# the answer arrives in a separate DATA frame (see info.py).
# ---------------------------------------------------------------------------
@mcp.tool()
def get_pin_config() -> str:
    """Report which GPIOs the running firmware has wired up, and to what.

    Read live from the device, so it reflects what is actually flashed rather
    than a host-side table.
    """
    try:
        entries = _info.get_pin_config()
    except InfoQueryError as exc:
        return f"error: {exc}"
    if not entries:
        return "device reported no pin entries"
    return "\n".join(f"GPIO{e.gpio}: {e.label} [{e.abbrev}]" for e in entries)


@mcp.tool()
def get_fw_version() -> str:
    """Report the running firmware's git commit, dirty flag, build time, and
    whether its UART protocol version matches this copy of pc_tools.

    Call this before any DAC/AD9833/OLED tool: they all refuse to send until
    a compatible version has been confirmed this way (or via a boot push).
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
# actions.py, without a matching @mcp.tool() having to be hand-written too.
# ---------------------------------------------------------------------------
@mcp.tool()
def list_buttons() -> str:
    """List every button/action press_button can invoke, with its parameters.

    Names match the GUI's own button/menu-item labels 1:1 (e.g. "OLED: Write
    & Show", "DAC: Set All"), grouped by popup/menu.
    """
    lines = []
    for name in sorted(actions.ACTIONS):
        action = actions.ACTIONS[name]
        params = ", ".join(f"{p}: {t.__name__}" for p, t in action.params.items())
        lines.append(f"{name}({params})\n    {action.description}")
    return "\n".join(lines)


@mcp.tool()
def press_button(name: str, params: Optional[dict[str, Any]] = None) -> str:
    """Press any GUI button/menu-item by name -- call list_buttons() first.

    `params` is a JSON object matching that action's parameter names, e.g.
    press_button("DAC: Set Channel Percent", {"channel": 0, "percent": 50.0})
    or press_button("OLED: Clear") for an action that takes none.
    """
    action = actions.ACTIONS.get(name)
    if action is None:
        return f"error: unknown action {name!r}. Call list_buttons() for the full list."
    try:
        return action.run(_action_ctx, **(params or {}))
    except TypeError as exc:
        return f"error: bad arguments for {name!r}: {exc}"
    except ValueError as exc:
        return f"error: {exc}"


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------
async def _run() -> None:
    await mcp.run_stdio_async()


def main() -> int:
    """Sync entry point for the ``uart-mcp-server`` console script."""
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
        _info.close()
        _device_log.close()
        # close(), not disconnect(): this link may be shared with another
        # process (see link_hub.py) -- exiting shouldn't yank the physical
        # port out from under it. The disconnect MCP tool is the only thing
        # that should do that.
        _link.close()
        _session_log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
