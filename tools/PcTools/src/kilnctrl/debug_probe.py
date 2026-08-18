"""Generic program/reset/halt/resume/step/memory/register access for both
processors on this board, over OpenOCD (see ``openocd_util.py``).

Design rationale: ``tools/PcTools/TODO.md``, section "One substrate covers
both: OpenOCD". That section observes there is an ESP32 MCP surface (build/
flash/target only, via the espressif.esp-idf-extension) but nothing for the
RP2040 safety processor (designator A1, ``firmware/SaftyFW/``) at all, and no
generic reset/halt/step for either chip -- and that OpenOCD's TCL command set
(``program``, ``reset run/halt/init``, ``halt``/``resume``/``step``,
``mdw``/``mdh``/``mdb``, ``mww``/``mwh``/``mwb``, ``reg``) already covers both
peers through one substrate: JTAG to the ESP32-S3, SWD (via a CMSIS-DAP debug
probe) to the RP2040.

This module is a thin, testable OpenOCD command builder -- it has no
dependency on the UART link machinery (``serial_link.py``, ``profiles.py``,
etc.) and knows nothing about ``_session_log``. The policy that belongs one
layer up, in ``mcp_server.py``'s ``debug_*`` tool wrappers, per the TODO's own
guard-rail list:

- **Halting the ESP mid-profile stops its telemetry to the Pico.** The
  (not-yet-written) SaftyFW firmware would correctly read that as a dead main
  controller and trip -- that's enforced in ``mcp_server.py``'s
  ``debug_halt()`` (it consults ``ProfilesClient.get_exec_status()``), not
  here.
- **There is currently no way to query the Pico's "ARMED" state from the PC**
  -- SaftyFW is skeleton-only today, no such protocol exists yet. The TODO's
  "refuse any write to the Pico while ARMED" guard rail therefore **cannot be
  implemented today**. ``write_memory()`` below and its MCP wrapper both say
  this plainly rather than pretending an ARMED check exists: the ``confirm``
  flag on the MCP tool is the only gate, for both peers, right now.
- ⚠️ **Standing caveat**: any PC debug connection into the safety domain (SWD
  to the Pico) bonds ``GND_Safty`` to PC ground, and if the ESP is on the same
  PC, bypasses the isolation barrier for the duration. Bench only, never with
  load wiring connected. Fully documented in
  ``firmware/SaftyFW/docs/HARDWARE.md`` §7b -- see that for the wiring detail,
  not repeated here.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Callable, Optional

from . import openocd_util

PEER_ESP = "esp"
PEER_PICO = "pico"

_MEM_WIDTH_READ_CMDS = {8: "mdb", 16: "mdh", 32: "mdw"}
_MEM_WIDTH_WRITE_CMDS = {8: "mwb", 16: "mwh", 32: "mww"}

_MAX_READ_COUNT = 4096  # sanity cap on a runaway dump, not a safety gate (read-only)

_RESET_MODES = {"run", "halt", "init"}


def _repo_root() -> str:
    """This file lives at tools/PcTools/src/kilnctrl/, so the repo root is
    four levels up (mirrors mcp_server.py's ``_kiln_fw_root()``)."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


def _kiln_fw_root() -> str:
    return os.path.join(_repo_root(), "firmware", "KilnFW")


def _safty_fw_root() -> str:
    return os.path.join(_repo_root(), "firmware", "SaftyFW")


def _kiln_fw_elf() -> str:
    return os.path.join(_kiln_fw_root(), "build", "KilnCtrl.elf")


def _safty_fw_elf() -> str:
    return os.path.join(_safty_fw_root(), "build", "SaftyFW.elf")


@dataclass
class PeerConfig:
    cfg_args: "list[str]"
    default_elf: Callable[[], str]  # lazily resolves a repo-relative default path
    adapter_speed_khz: Optional[int]
    root: str


_PEERS = {
    PEER_ESP: PeerConfig(
        cfg_args=["board/esp32s3-builtin.cfg"],
        default_elf=_kiln_fw_elf,
        # flash_firmware()'s existing TCL never sets `adapter speed`, so we
        # don't add one here either -- avoid changing existing ESP behavior.
        adapter_speed_khz=None,
        root=_kiln_fw_root(),
    ),
    PEER_PICO: PeerConfig(
        cfg_args=["interface/cmsis-dap.cfg", "target/rp2040.cfg"],
        default_elf=_safty_fw_elf,
        adapter_speed_khz=5000,  # confirmed working this session
        root=_safty_fw_root(),
    ),
}


def resolve_peer(peer: str) -> PeerConfig:
    cfg = _PEERS.get(peer)
    if cfg is None:
        raise ValueError(f"unknown peer {peer!r}, must be one of: {PEER_ESP!r}, {PEER_PICO!r}")
    return cfg


def _openocd_exe_or_raise() -> str:
    exe = openocd_util._find_openocd_exe()
    if not exe:
        raise FileNotFoundError(
            "openocd.exe not found under ~/.espressif/tools/openocd-esp32/, "
            "C:\\Espressif\\, the OPENOCD_EXE env var, or the settings.json override "
            "-- is it installed, or does set_openocd_path() need to be called?"
        )
    return exe


def _speed_prefix(peer_cfg: PeerConfig) -> str:
    if peer_cfg.adapter_speed_khz is None:
        return ""
    return f"adapter speed {peer_cfg.adapter_speed_khz}; "


def _run(peer: str, tcl_commands: str, timeout_s: int = 30) -> "tuple[bool, str]":
    peer_cfg = resolve_peer(peer)
    exe = _openocd_exe_or_raise()
    return openocd_util.run_openocd(exe, peer_cfg.cfg_args, tcl_commands, cwd=peer_cfg.root, timeout_s=timeout_s)


def program(peer: str, elf_path: Optional[str] = None) -> "tuple[bool, str]":
    """Flashes ``elf_path`` (or the peer's default build output) and resets.

    Kills any stale openocd session first (same reasoning as
    ``flash_firmware()``'s existing stale-session guard: a second openocd
    instance can't open the JTAG/SWD interface while one is already running).
    """
    peer_cfg = resolve_peer(peer)
    elf = elf_path or peer_cfg.default_elf()
    if not os.path.isfile(elf):
        return False, f"missing build output, run the build first: {elf}"

    openocd_util.kill_openocd_sessions_impl()

    tcl = f'{_speed_prefix(peer_cfg)}program "{elf}" verify reset exit'
    return _run(peer, tcl, timeout_s=90)


def reset(peer: str, mode: str = "run") -> "tuple[bool, str]":
    """``mode`` is one of "run", "halt", "init" (maps to OpenOCD's
    ``reset run``/``reset halt``/``reset init``)."""
    if mode not in _RESET_MODES:
        raise ValueError(f"mode must be one of {sorted(_RESET_MODES)}, got {mode!r}")
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}reset {mode}; exit"
    return _run(peer, tcl)


def halt(peer: str) -> "tuple[bool, str]":
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}halt; exit"
    return _run(peer, tcl)


def resume(peer: str) -> "tuple[bool, str]":
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}resume; exit"
    return _run(peer, tcl)


def step(peer: str) -> "tuple[bool, str]":
    """Steps one instruction from wherever the core currently is.

    If the core was running, this halts it first (``halt; step; exit``) --
    that's a state change the caller should know about: a running core does
    not resume after the step, it stays halted at the next instruction.
    """
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}halt; step; exit"
    return _run(peer, tcl)


def read_memory(peer: str, address: int, count: int = 1, width: int = 32) -> "tuple[bool, str]":
    """Reads ``count`` ``width``-bit words starting at ``address`` (read-only,
    no ARMED/safety implications). ``count`` is capped at 4096 to stop a
    runaway dump, not as a safety gate."""
    cmd = _MEM_WIDTH_READ_CMDS.get(width)
    if cmd is None:
        raise ValueError(f"width must be one of {sorted(_MEM_WIDTH_READ_CMDS)}, got {width!r}")
    if count <= 0:
        raise ValueError(f"count must be positive, got {count!r}")
    if count > _MAX_READ_COUNT:
        raise ValueError(f"count {count} exceeds the {_MAX_READ_COUNT}-word cap")
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}halt; {cmd} 0x{address:x} {count}; exit"
    return _run(peer, tcl)


def write_memory(peer: str, address: int, value: int, width: int = 32) -> "tuple[bool, str]":
    """Writes one ``width``-bit ``value`` at ``address``.

    NOTE: there is no ARMED-state check for the Pico here (see module
    docstring) -- SaftyFW does not yet expose any protocol to query it from
    the PC, so this function (and its MCP wrapper's ``confirm`` flag) cannot
    enforce the TODO's "refuse any write to the Pico while ARMED" guard rail.
    ``confirm=True`` at the MCP layer is the only gate today, for both peers.
    """
    cmd = _MEM_WIDTH_WRITE_CMDS.get(width)
    if cmd is None:
        raise ValueError(f"width must be one of {sorted(_MEM_WIDTH_WRITE_CMDS)}, got {width!r}")
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}halt; {cmd} 0x{address:x} 0x{value:x}; exit"
    return _run(peer, tcl)


def read_registers(peer: str) -> "tuple[bool, str]":
    """Reads all core registers. Needs the core halted first to read
    registers, so this halts it as a side effect (``halt; reg; exit``)."""
    peer_cfg = resolve_peer(peer)
    tcl = f"{_speed_prefix(peer_cfg)}halt; reg; exit"
    return _run(peer, tcl)
