"""generic debug (OpenOCD program/reset/halt/step/memory/registers).

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


# Cap on how much raw OpenOCD output gets written to the session log per
# failed invocation -- generous enough to hold a whole failed reset/program
# transcript, small enough that a wedged board spamming retries can't blow
# the log file up.
_OPENOCD_LOG_TRUNCATE_BYTES = 16 * 1024

# Lines matching one of these (checked in order) are the ones worth quoting
# on their own as "the decisive line" in a tool's returned error -- these are
# the substrings OpenOCD/this codebase's own checks (see
# openocd_util.run_openocd's `ok = ...` line) treat as failure markers, plus
# a couple of common Tcl-level failures seen in practice (invalid command
# name against a not-yet-inited target, "Polling failed" for a probe/board
# that vanished mid-session).
_DECISIVE_MARKERS = ("Verify Failed", "Error:", "invalid command name", "Polling failed")


def _decisive_openocd_line(output: str) -> str:
    """Picks the one line out of a (possibly long) OpenOCD transcript that
    most likely explains a failure, for use in a tool's returned `error`
    message. Falls back to the last non-blank line, then a fixed string, so
    this never returns an empty message."""
    lines = [ln.strip() for ln in (output or "").splitlines() if ln.strip()]
    for marker in _DECISIVE_MARKERS:
        for line in lines:
            if marker in line:
                return line
    return lines[-1] if lines else "(no OpenOCD output captured)"


def _log_openocd_result(op: str, ok: bool, output: str) -> None:
    """Persists an OpenOCD invocation's outcome to the session log.

    On success this is a single short line -- ``ok=True`` alone was already
    logged by most callers before this helper existed, and a healthy board
    doing routine debug_reset/debug_halt calls should not spam the session
    log with full OpenOCD transcripts.

    On failure, the FULL stdout+stderr capture (``output``, as returned by
    ``openocd_util.run_openocd`` via debug_probe.py) is written to the
    session log, truncated to ``_OPENOCD_LOG_TRUNCATE_BYTES`` -- this is the
    only place that transcript is ever persisted; the MCP tool's return value
    is not logged anywhere by itself, so without this a failed reset/program/
    halt could only be diagnosed after the fact by reproducing it again.
    """
    if ok:
        _srv._session_log.warning("%s: ok", op)
        return
    text = output or "(no output captured)"
    if len(text) > _OPENOCD_LOG_TRUNCATE_BYTES:
        text = text[-_OPENOCD_LOG_TRUNCATE_BYTES:]
        text = "...[truncated]...\n" + text
    _srv._session_log.warning("%s: FAILED\n%s", op, text)


def _openocd_error_message(action_desc: str, output: str) -> str:
    """Builds the string an MCP tool returns for a failed OpenOCD call:
    the decisive line up front (so it's the first thing read), then the last
    ~20 lines of raw output for context."""
    decisive = _decisive_openocd_line(output)
    tail = "\n".join((output or "").strip().splitlines()[-20:])
    return f"error: {action_desc}: {decisive}\n\n{tail}"


# ---------------------------------------------------------------------------
# Generic debug (OpenOCD program/reset/halt/step/memory/registers), both
# peers. See debug_probe.py's module docstring for the full design rationale
# (tools/PcTools/TODO.md "One substrate covers both: OpenOCD") and the guard
# rails that are and are not implemented. This section only adds the MCP
# tool wrappers + the policy pieces debug_probe.py deliberately doesn't know
# about (session logging, the ESP-halt-during-profile guard, the
# write-requires-confirm gate).
# ---------------------------------------------------------------------------
@_srv._tool()
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


@_srv._tool()
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


@_srv._tool()
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

    peer="esp" is REFUSED outright (before touching OpenOCD at all): this
    generic single-ELF program path reliably fails flash-bank detection/
    verify on this board (confirmed repeatedly) -- flash_firmware() is the
    sanctioned, working path for the ESP. This used to fail downstream after
    wasted time instead of refusing immediately; see CLAUDE.md's
    flash_firmware section. peer="pico" is unaffected -- the RP2040 has no
    bootloader/partition table to get wrong and this path works fine there.

    For peer="pico" (SaftyFW), same staleness refusal as flash_firmware(): the
    ELF's build-identity header (saftyfw_build_info.h) is checked against
    current HEAD and, if firmware/SaftyFW or firmware/CommonFW have
    uncommitted changes, against those changed files' mtimes -- see
    stale_check.py. Pass allow_stale=True to flash anyway. Only checked when
    using the peer's default ELF and default build-info location; an explicit
    elf_path bypasses the check (nothing to compare it against)."""
    if peer == debug_probe.PEER_ESP:
        return (
            "error: debug_program(peer=\"esp\") is refused -- use flash_firmware(); "
            "debug_program esp path fails flash-bank detection on this board -- see CLAUDE.md"
        )

    if not confirm:
        return "error: flash write refused without confirm=True -- this writes flash on a live board"

    stale_prefix = ""
    if peer == debug_probe.PEER_PICO and elf_path is None:
        stale = stale_check.check_saftyfw_stale(debug_probe._safty_fw_root())
        if stale.stale:
            _srv._session_log.warning("debug_program: stale ELF detected for peer=%s: %s", peer, stale.reason)
            if not allow_stale:
                return (
                    "error: refusing to flash a stale ELF -- " + stale.reason + "\n\n"
                    "Rebuild with build_saftyfw_host_tests/the SaftyFW build first, or pass "
                    "allow_stale=True to flash this ELF anyway."
                )
            stale_prefix = f"WARNING: flashed a stale binary anyway ({stale.reason})\n"

    ok, output = debug_probe.program(peer, elf_path)
    _log_openocd_result(f"debug_program(peer={peer})", ok, output)
    if ok:
        return stale_prefix + f"programmed {peer} OK, reset and running"
    return _openocd_error_message(f"program failed for {peer}", output)


@_srv._tool()
def debug_reset(peer: str, mode: str = "run") -> str:
    """Resets `peer` ("esp"/"pico"). `mode` is "run" (default, resumes
    execution), "halt" (resets and halts), or "init" (resets and runs any
    OpenOCD target init sequence, then halts)."""
    ok, output = debug_probe.reset(peer, mode)
    _log_openocd_result(f"debug_reset(peer={peer}, mode={mode})", ok, output)
    if ok:
        return f"reset {peer} ({mode}) OK"
    return _openocd_error_message(f"reset failed for {peer}", output)


@_srv._tool()
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
            status = _srv._profiles.get_exec_status(timeout=2.0)
        except Exception:  # noqa: BLE001 - unreachable ESP must not block the halt
            status = None
        if status is not None and status.state in (1, 2):
            return (
                f"error: refusing to halt ESP while a profile is {status.state_name} -- "
                "this would freeze relay control mid-firing. Use debug_reset/profiles "
                "stop tools if you really need to interrupt it."
            )
    ok, output = debug_probe.halt(peer)
    _log_openocd_result(f"debug_halt(peer={peer})", ok, output)
    if ok:
        return f"halted {peer}"
    return _openocd_error_message(f"halt failed for {peer}", output)


@_srv._tool()
def debug_resume(peer: str) -> str:
    """Resumes `peer`'s core from a halt."""
    ok, output = debug_probe.resume(peer)
    _log_openocd_result(f"debug_resume(peer={peer})", ok, output)
    if ok:
        return f"resumed {peer}"
    return _openocd_error_message(f"resume failed for {peer}", output)


@_srv._tool()
def debug_step(peer: str) -> str:
    """Single-steps `peer`'s core one instruction. If it was running, this
    halts it first (it does not resume running after the step -- it stays
    halted at the next instruction)."""
    ok, output = debug_probe.step(peer)
    _log_openocd_result(f"debug_step(peer={peer})", ok, output)
    if ok:
        return f"stepped {peer}:\n{output.strip()}"
    return _openocd_error_message(f"step failed for {peer}", output)


@_srv._tool()
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
    _log_openocd_result(f"debug_read_memory(peer={peer}, address=0x{address:x})", ok, output)
    if ok:
        return output.strip()
    return _openocd_error_message(f"read_memory failed for {peer}", output)


@_srv._tool()
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
    _log_openocd_result(f"debug_read_symbol(peer={peer}, symbol={symbol})", ok, output)
    if ok:
        return output.strip()
    return _openocd_error_message(f"read_symbol failed for {peer}", output)


@_srv._tool()
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


@_srv._tool()
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
            _srv._session_log.warning(
                "debug_write_memory: REFUSED peer=pico address=0x%x armed_state=%s detail=%s",
                address, armed, detail,
            )
            if armed is None:
                return f"error: write refused -- could not confidently determine Pico ARMED state ({detail})"
            return f"error: write refused -- Pico is ARMED ({detail})"
    ok, output = debug_probe.write_memory(peer, address, value, width)
    if ok:
        _srv._session_log.warning(
            "debug_write_memory: peer=%s address=0x%x value=0x%x width=%d ok=True", peer, address, value, width
        )
        return f"wrote 0x{value:x} ({width}-bit) to {peer} 0x{address:x}"
    _log_openocd_result(
        f"debug_write_memory(peer={peer}, address=0x{address:x}, value=0x{value:x}, width={width})", ok, output
    )
    return _openocd_error_message(f"write_memory failed for {peer}", output)


@_srv._tool()
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
    _log_openocd_result(f"debug_read_registers(peer={peer}, target={target})", ok, output)
    if ok:
        return output.strip()
    return _openocd_error_message(f"read_registers failed for {peer}", output)


