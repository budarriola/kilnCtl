"""Shared OpenOCD plumbing, used by both the ESP flash path (``mcp_server.py``'s
``flash_firmware()``) and the generic ``debug_probe.py`` program/reset/halt/
step/memory tools that cover the Pico (RP2040 safety processor) as well.

This module was split out of ``mcp_server.py`` so both peers -- the ESP32-S3
main controller (JTAG, one ``-f`` board cfg) and the RP2040 safety processor
(SWD via a CMSIS-DAP debug probe, two ``-f`` args: ``interface/cmsis-dap.cfg``
plus ``target/rp2040.cfg``) -- share one implementation instead of two copies
of the same subprocess-invocation logic drifting apart.

Every call here is a one-shot ``subprocess.run(openocd -c "cmd1; cmd2; exit")``
-- no persistent TCL/telnet or GDB socket is opened. That matches this
project's existing convention (established by ``flash_firmware()`` before this
module existed) and keeps every invocation independently inspectable in a
process list / log line, at the cost of ~1-2s of OpenOCD startup overhead per
call. Fine for this project's usage pattern (occasional agent-driven debug
actions, not a tight step-debug loop).
"""

from __future__ import annotations

import glob
import logging
import os
import subprocess
from typing import Optional

from . import settings

log = logging.getLogger(__name__)


def _find_openocd_exe() -> Optional[str]:
    """Locates openocd.exe without relying on `export.ps1` having run in this
    process's environment (the MCP server is a plain Python process, not a
    ESP-IDF shell).

    Resolution order:
      1. ``settings.get_openocd_path()`` -- a user-set override (persisted in
         settings.json via the ``set_openocd_path()`` MCP tool), for machines
         where autodetection below doesn't find it. If set but the file no
         longer exists (e.g. a stale path after a reinstall), this logs a
         warning and falls through to autodetection rather than silently
         failing the caller.
      2. The ``OPENOCD_EXE`` environment variable, if set and the file exists.
      3. The actual installed-tools layout on this machine
         (~/.espressif/tools/openocd-esp32/<version>/openocd-esp32/bin/), then
         the C:\\Espressif path .vscode/tasks.json's "Flash device" task
         hardcodes (kept only as a fallback since it was found to be stale/
         wrong for this machine when this tool was written).
    """
    override = settings.get_openocd_path()
    if override:
        if os.path.isfile(override):
            return override
        log.warning("configured openocd_exe path %r does not exist, falling back to autodetection", override)

    env_path = os.environ.get("OPENOCD_EXE")
    if env_path and os.path.isfile(env_path):
        return env_path

    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    candidates = glob.glob(
        os.path.join(home, ".espressif", "tools", "openocd-esp32", "*", "openocd-esp32", "bin", "openocd.exe")
    )
    candidates += glob.glob(r"C:\Espressif\tools\openocd-esp32\*\openocd-esp32\bin\openocd.exe")
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def run_openocd(
    openocd_exe: str, cfg_args: "list[str]", tcl_commands: str, cwd: str, timeout_s: int
) -> "tuple[bool, str]":
    """Runs one openocd invocation with the given ``-f`` config file(s) and a
    single ``-c`` TCL command string (semicolon-joined, ending in ``exit``).

    ``cfg_args`` is a list so both peers share this function: the ESP path
    passes a single board cfg (``["board/esp32s3-builtin.cfg"]``), the Pico
    path passes an interface + target pair
    (``["interface/cmsis-dap.cfg", "target/rp2040.cfg"]``).
    """
    scripts_dir = os.path.normpath(os.path.join(os.path.dirname(openocd_exe), "..", "share", "openocd", "scripts"))
    cmd = [openocd_exe, "-s", scripts_dir]
    for cfg in cfg_args:
        cmd += ["-f", cfg]
    cmd += ["-c", tcl_commands]
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


def kill_openocd_sessions_impl() -> str:
    """Force-kills any running openocd.exe processes on this PC.

    Factored out of the MCP tool wrapper so internal callers (e.g. program()
    in debug_probe.py, before grabbing the JTAG/SWD interface) can call it
    directly without going through the MCP tool decorator/registration.
    Safe to call when nothing is running (reports that plainly, not an
    error).
    """
    try:
        proc = subprocess.run(
            ["taskkill", "/F", "/IM", "openocd.exe"], capture_output=True, text=True, timeout=10
        )
    except Exception as exc:  # noqa: BLE001
        return f"error: could not run taskkill: {exc}"
    if proc.returncode == 0:
        return "killed running openocd.exe process(es)"
    return "no running openocd.exe process found"
