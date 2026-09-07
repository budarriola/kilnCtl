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
- **The Pico's "ARMED" state IS now readable from the PC, but only via SWD,
  never via the (still dead) UART link.** SaftyFW's ``relay_owner.c`` (Phase
  5) keeps its GRACE/ARMED/TRIPPED state machine in one file-local static,
  ``s_state`` (``relay_owner_state_t``, 1 byte after GCC's enum packing;
  ``RELAY_OWNER_STATE_ARMED == 2``) -- no mutex, single-writer, matching this
  codebase's established pattern for that class of scalar (see
  ``relay_owner.h``'s own doc comment). ``resolve_symbol()``/
  ``pico_armed_state()`` below resolve that symbol's address fresh from
  ``SaftyFW.elf`` on every call (via ``arm-none-eabi-nm -S``, never a
  hardcoded address -- BSS layout can shift on an unrelated recompile) and
  read it over SWD the same way ``read_memory()`` already does. ``confirm=True``
  on ``write_memory()`` for ``peer="pico"`` now goes through this check first
  and fails closed (refuses) if the symbol can't be resolved uniquely, isn't
  1 byte, or the SWD read itself fails -- an unreadable state is never treated
  as "not armed". This is still UART-link-independent and still bonds
  ``GND_Safty`` to PC ground for the duration (see the caveat below);
  it does *not* need the dead link to work, and it does not (and does not
  try to) query link_task's own DIAG/status protocol.
- ⚠️ **Standing caveat**: any PC debug connection into the safety domain (SWD
  to the Pico) bonds ``GND_Safty`` to PC ground, and if the ESP is on the same
  PC, bypasses the isolation barrier for the duration. Bench only, never with
  load wiring connected. Fully documented in
  ``firmware/SaftyFW/docs/HARDWARE.md`` §7b -- see that for the wiring detail,
  not repeated here.
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from typing import Callable, Optional

from . import openocd_util, serial_link

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
    # USB serial number of the specific debug probe this peer must be reached
    # through, or None to let OpenOCD pick whatever matching adapter it finds
    # first. None is only safe while exactly one matching probe is plugged in:
    # `interface/cmsis-dap.cfg` with no `adapter serial` binds to the first
    # CMSIS-DAP device enumerated, so a second probe on the bench makes the
    # choice silently arbitrary -- and "silently arbitrary" here means
    # flashing or halting the wrong board. Overridable at runtime by the
    # environment variable named in _PROBE_SERIAL_ENV below, for a bench where
    # the probe has been swapped for another unit.
    adapter_serial: Optional[str] = None
    # `nm` for this peer's toolchain, used by resolve_symbol() to turn a symbol
    # name into an address. Looked up on PATH; None means this peer has no
    # symbol lookup wired up.
    nm_tool: Optional[str] = None


_PEERS = {
    PEER_ESP: PeerConfig(
        cfg_args=["board/esp32s3-builtin.cfg"],
        default_elf=_kiln_fw_elf,
        # flash_firmware()'s existing TCL never sets `adapter speed`, so we
        # don't add one here either -- avoid changing existing ESP behavior.
        adapter_speed_khz=None,
        root=_kiln_fw_root(),
        # Two ESP32-S3 boards are on the bench (2026-09-05: the main board
        # and the UnitTestFixture), and both share USB VID:PID 303A:1001 on
        # their native USB-Serial-JTAG interface -- `board/esp32s3-builtin.cfg`
        # alone cannot tell them apart, so this MUST be pinned (an unpinned
        # peer here would let debug_reset/halt/resume/step/read_memory/
        # write_memory/read_symbol/read_registers bind whichever 303A:1001
        # unit OpenOCD finds first -- e.g. `debug_reset(peer="esp")`
        # resetting the FIXTURE instead of the main board, or vice versa).
        # Imported from serial_link.py -- the same single source of truth
        # mcp_server_flash.py's flash_firmware() pins its `adapter serial`
        # to -- rather than a second copy of the literal.
        adapter_serial=serial_link.MAIN_BOARD_JTAG_SERIAL,
    ),
    PEER_PICO: PeerConfig(
        cfg_args=["interface/cmsis-dap.cfg", "target/rp2040.cfg"],
        default_elf=_safty_fw_elf,
        adapter_speed_khz=5000,  # confirmed working this session
        root=_safty_fw_root(),
        # Raspberry Pi Debug Probe, USB VID 2E8A PID 000C, serial recorded
        # 2026-08-23 from the bench this firmware is developed on. Pinned
        # deliberately: a SECOND CMSIS-DAP probe would be indistinguishable
        # from this one to `interface/cmsis-dap.cfg` alone. (This probe's
        # VID:PID is its own family, distinct from either ESP32-S3 board's
        # 303A:1001 native JTAG, so nothing here can bind an ESP32-S3 board
        # by mistake -- but as of 2026-09-05 there are now TWO 303A:1001
        # boards on the bench, which is why PEER_ESP above also needs, and
        # now has, its own adapter_serial pin; do not read this comment as
        # implying the ESP side never needed one.) See adapter_serial's
        # comment on PeerConfig.
        adapter_serial="E66540F0A36C6E21",
        nm_tool="arm-none-eabi-nm",
    ),
}


def resolve_peer(peer: str) -> PeerConfig:
    cfg = _PEERS.get(peer)
    if cfg is None:
        raise ValueError(
            f"unknown peer {peer!r}, must be one of: {PEER_ESP!r}, {PEER_PICO!r}"
        )
    return cfg


#: Cache of (elf_path, mtime) -> {symbol: (address, size)}, so a burst of
#: symbol reads in one debugging pass runs `nm` once instead of once per read.
#: Keyed on mtime so a rebuild invalidates it automatically.
_SYMBOL_CACHE: "dict[tuple[str, float], dict[str, tuple[int, int]]]" = {}


def symbol_table(peer: str, elf_path: Optional[str] = None) -> "dict[str, tuple[int, int]]":
    """Returns ``{symbol_name: (address, size_bytes)}`` for ``peer``'s ELF.

    Uses ``nm -S``, so ``size_bytes`` is whatever the ELF records (0 for
    symbols with no recorded size, which is normal for assembly labels).
    Later definitions of a duplicated name overwrite earlier ones -- static
    symbols of the same name in different translation units are genuinely
    ambiguous here, so ``read_symbol()`` refuses those rather than guessing;
    see its docstring."""
    peer_cfg = resolve_peer(peer)
    if peer_cfg.nm_tool is None:
        raise ValueError(f"peer {peer!r} has no nm tool configured for symbol lookup")
    elf = elf_path or peer_cfg.default_elf()
    if not os.path.isfile(elf):
        raise FileNotFoundError(f"ELF not found for symbol lookup: {elf}")
    nm = shutil.which(peer_cfg.nm_tool)
    if nm is None:
        raise FileNotFoundError(
            f"{peer_cfg.nm_tool} not found on PATH -- needed to resolve symbol names for peer {peer!r}"
        )
    key = (os.path.abspath(elf), os.path.getmtime(elf))
    cached = _SYMBOL_CACHE.get(key)
    if cached is not None:
        return cached

    proc = subprocess.run([nm, "-S", elf], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{peer_cfg.nm_tool} failed on {elf}: {proc.stderr.strip()[:400]}")

    table: "dict[str, tuple[int, int]]" = {}
    duplicates: "set[str]" = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        # "addr size type name" (sized) or "addr type name" (unsized);
        # undefined symbols have no address at all and are skipped.
        if len(parts) == 4:
            addr_s, size_s, _type, name = parts
            size = int(size_s, 16)
        elif len(parts) == 3:
            addr_s, _type, name = parts
            size = 0
        else:
            continue
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        if name in table and table[name][0] != addr:
            duplicates.add(name)
        table[name] = (addr, size)
    for name in duplicates:
        # Recorded as an explicit ambiguity marker rather than silently
        # resolving to whichever definition nm listed last: two file-static
        # symbols sharing a name are different objects, and reading "the"
        # one would otherwise return a confidently wrong board state.
        table[name] = (-1, 0)
    _SYMBOL_CACHE[key] = table
    return table


def _openocd_exe_or_raise() -> str:
    exe = openocd_util._find_openocd_exe()
    if not exe:
        raise FileNotFoundError(
            "openocd.exe not found under ~/.espressif/tools/openocd-esp32/, "
            "C:\\Espressif\\, the OPENOCD_EXE env var, or the settings.json override "
            "-- is it installed, or does set_openocd_path() need to be called?"
        )
    return exe


#: Environment variable that overrides the compiled-in PeerConfig.adapter_serial
#: for the Pico. Set it to another probe's serial to use a different unit, or to
#: the empty string to go back to "first matching adapter wins".
_PROBE_SERIAL_ENV = "KILNCTL_PICO_PROBE_SERIAL"


def _adapter_serial(peer_cfg: PeerConfig) -> Optional[str]:
    """Which probe serial to bind to, environment override taking precedence.

    An explicitly empty environment variable means "do not pin", which is not
    the same as the variable being unset (fall back to the compiled-in value).
    """
    override = os.environ.get(_PROBE_SERIAL_ENV)
    if override is not None:
        override = override.strip()
        return override or None
    return peer_cfg.adapter_serial


def pico_probe_serial() -> Optional[str]:
    """Public wrapper on :func:`_adapter_serial` for ``PEER_PICO`` -- the USB
    serial number this codebase pins the SWD/JTAG path to (env-overridable
    via :data:`_PROBE_SERIAL_ENV`), for callers outside this module that need
    to tell *that* probe apart from another identical-model one on the same
    bench (e.g. ``console_capture.py``'s transport-availability report,
    matching against ``serial_link.debug_probe_hwid_serial``).
    """
    return _adapter_serial(resolve_peer(PEER_PICO))


def _adapter_prefix(peer_cfg: PeerConfig) -> str:
    """TCL preamble pinning the adapter and its speed, in that order.

    `adapter serial` must precede `init` (it selects which USB device to open),
    which every caller's TCL satisfies -- they all put this prefix first.
    """
    prefix = ""
    serial = _adapter_serial(peer_cfg)
    if serial:
        prefix += f"adapter serial {serial}; "
    if peer_cfg.adapter_speed_khz is not None:
        prefix += f"adapter speed {peer_cfg.adapter_speed_khz}; "
    return prefix


def _refuse_if_esp_adapter_absent(peer: str, peer_cfg: PeerConfig) -> Optional[str]:
    """None unless `peer` is PEER_ESP and its pinned adapter_serial is not
    currently enumerated -- see serial_link.refuse_if_jtag_serial_absent()
    (the single source of truth, shared with mcp_server_flash.py) for the
    "why" and the case-insensitive comparison. Checked in `_run()` so every
    ESP-peer command (program/reset/halt/resume/step/read_memory/
    write_memory/read_symbol/read_registers) refuses BEFORE OpenOCD is
    invoked, not just program()."""
    if peer != PEER_ESP:
        return None
    serial = _adapter_serial(peer_cfg)
    if not serial:
        return None  # env override explicitly cleared the pin -- caller's choice
    return serial_link.refuse_if_jtag_serial_absent(serial, "main board (ESP32-S3)")


def _run(peer: str, tcl_commands: str, timeout_s: int = 30) -> "tuple[bool, str]":
    peer_cfg = resolve_peer(peer)
    refusal = _refuse_if_esp_adapter_absent(peer, peer_cfg)
    if refusal:
        return False, refusal
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

    # Tcl only performs backslash escaping (\U, \b, \O, ...) inside a
    # double-quoted string -- a bare Windows path (C:\Users\...) passed
    # straight through corrupts itself the moment OpenOCD's Tcl interpreter
    # parses this -c argument (found flashing for real, 2026-08-18: `\b`
    # alone silently ate a character, producing a path OpenOCD then reported
    # as "file not found"). Forward slashes are accepted by both Tcl and
    # OpenOCD on Windows and sidestep the whole escaping question rather than
    # trying to double every backslash.
    elf_tcl = elf.replace("\\", "/")
    tcl = f'{_adapter_prefix(peer_cfg)}program "{elf_tcl}" verify reset exit'
    return _run(peer, tcl, timeout_s=90)


def reset(peer: str, mode: str = "run") -> "tuple[bool, str]":
    """``mode`` is one of "run", "halt", "init" (maps to OpenOCD's
    ``reset run``/``reset halt``/``reset init``)."""
    if mode not in _RESET_MODES:
        raise ValueError(f"mode must be one of {sorted(_RESET_MODES)}, got {mode!r}")
    peer_cfg = resolve_peer(peer)
    # `init` first: unlike program() (a built-in Tcl proc that inits itself
    # internally), a bare target-level command like `reset run` fails with
    # "invalid command name" against a fresh openocd.exe session that hasn't
    # examined the target yet -- found running this for real, 2026-08-18,
    # against the every function below that wasn't program(). None of this
    # module's non-program() functions had ever been exercised successfully
    # before that.
    #
    # Every target in the chain must be halted first, same reasoning as
    # `_RESUME_TCL` above (RP2040 SMP grouping: OpenOCD's `reset`/`resume`
    # against an SMP target fails, and leaves the chip in a stuck state, if
    # the two cores aren't in a matching state going in -- e.g. core1 left
    # halted from an earlier read_memory()/read_registers() call while core0
    # is still running). Found for real 2026-09-06: a `debug_reset(peer=
    # "pico")` right after a debug read locked the board up (fw_version/
    # boot_id/frame counters frozen); a second identical call recovered it,
    # because by then the halt-all-then-reset TCL below (missing on the
    # first call's code path) was no longer needed to reconcile mismatched
    # core states. Single-core peers (the ESP) have a one-element target
    # list, so this is a no-op there.
    tcl = (
        f"{_adapter_prefix(peer_cfg)}init; "
        f"foreach _kctl_t [target names] {{targets $_kctl_t; halt}}; "
        f"targets [lindex [target names] 0]; reset {mode}; exit"
    )
    return _run(peer, tcl)


def halt(peer: str) -> "tuple[bool, str]":
    peer_cfg = resolve_peer(peer)
    tcl = f"{_adapter_prefix(peer_cfg)}init; halt; exit"
    return _run(peer, tcl)


def resume(peer: str) -> "tuple[bool, str]":
    peer_cfg = resolve_peer(peer)
    tcl = f"{_adapter_prefix(peer_cfg)}init; resume; exit"
    return _run(peer, tcl)


def step(peer: str) -> "tuple[bool, str]":
    """Steps one instruction from wherever the core currently is.

    If the core was running, this halts it first (``halt; step; exit``) --
    that's a state change the caller should know about: a running core does
    not resume after the step, it stays halted at the next instruction.
    """
    peer_cfg = resolve_peer(peer)
    tcl = f"{_adapter_prefix(peer_cfg)}init; halt; step; exit"
    return _run(peer, tcl)


# --- ARMED-state read (Pico peer only), see module docstring ---------------

# relay_owner.c's single-writer state-machine static -- see the module
# docstring's "ARMED state IS now readable" section above. Verified unique in
# the repo (2026-08-19: `grep -rln s_state firmware/SaftyFW/src` returns only
# relay_owner.c) and confirmed 1 byte via `arm-none-eabi-nm -S` against a real
# build (`firmware/SaftyFW/build/SaftyFW.elf`: `2000ba33 00000001 b s_state`).
_ARMED_SYMBOL = "s_state"
_ARMED_SYMBOL_EXPECTED_SIZE = 1
# relay_owner.h: RELAY_OWNER_STATE_INIT=0, GRACE=1, ARMED=2, TRIPPED=3.
_ARMED_ENUM_VALUE = 2

_MEMRD_RE = re.compile(r"MEMRD\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)")


def _find_arm_nm_exe() -> Optional[str]:
    """Locates arm-none-eabi-nm, needed to resolve ``s_state``'s address
    fresh from the ELF on every call (see ``resolve_symbol()``). Mirrors
    ``openocd_util._find_openocd_exe()``'s resolution shape: an env var
    override first, then PATH, then the actual installed-tools layout found
    on this machine (Arm GNU Toolchain's default installer path)."""
    env_path = os.environ.get("ARM_NM_EXE")
    if env_path and os.path.isfile(env_path):
        return env_path

    which = shutil.which("arm-none-eabi-nm")
    if which:
        return which

    candidates = glob.glob(r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-nm.exe")
    candidates += glob.glob(r"C:\Program Files\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-nm.exe")
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def resolve_symbol(elf_path: str, symbol: str, nm_exe: Optional[str] = None) -> Optional["tuple[int, int]"]:
    """Resolves ``symbol``'s (address, size) in ``elf_path`` via
    ``arm-none-eabi-nm -S``, freshly on every call -- never a hardcoded
    address, because BSS layout can shift on an unrelated recompile even
    though the symbol name itself is stable.

    Fails closed (returns ``None``) rather than guessing: no nm executable
    found, the ELF is missing, nm errors out, the symbol is absent, or --
    critically -- the symbol name is found more than once (ambiguous; would
    silently resolve to the wrong global). Callers must treat ``None`` as
    "could not determine", never as an absence of the state it was checking.
    """
    nm = nm_exe or _find_arm_nm_exe()
    if not nm or not os.path.isfile(elf_path):
        return None
    try:
        proc = subprocess.run([nm, "-S", elf_path], capture_output=True, text=True, timeout=15)
    except Exception:  # noqa: BLE001 - any failure here means "can't resolve", not a crash
        return None
    if proc.returncode != 0:
        return None

    matches: "list[tuple[int, int]]" = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        # nm -S line shape for a sized symbol: "<addr> <size> <type> <name>".
        # Undefined/external symbols print only 3 fields (no size) and are
        # not candidates for a data read regardless.
        if len(parts) == 4 and parts[3] == symbol:
            try:
                matches.append((int(parts[0], 16), int(parts[1], 16)))
            except ValueError:
                continue
    if len(matches) != 1:
        return None
    return matches[0]


def pico_armed_state() -> "tuple[Optional[bool], str]":
    """Reads relay_owner.c's ``s_state`` over SWD and reports whether it
    currently reads ``RELAY_OWNER_STATE_ARMED``.

    Returns ``(True, detail)`` if armed, ``(False, detail)`` if confidently
    read as not-armed, or ``(None, detail)`` if the state could not be
    confidently determined -- callers (``debug_write_memory``'s MCP wrapper)
    MUST treat ``None`` as fail-closed (refuse the write), never as "assume
    not armed". Halts the Pico core as a side effect (same as any
    ``read_memory()`` call)."""
    elf = _safty_fw_elf()
    resolved = resolve_symbol(elf, _ARMED_SYMBOL)
    if resolved is None:
        return None, (
            f"could not resolve symbol {_ARMED_SYMBOL!r} in {elf} -- missing "
            "arm-none-eabi-nm (set ARM_NM_EXE or install the Arm GNU Toolchain), "
            "missing/stale ELF (build SaftyFW first), or the symbol was not "
            "found exactly once"
        )
    addr, size = resolved
    if size != _ARMED_SYMBOL_EXPECTED_SIZE:
        return None, (
            f"symbol {_ARMED_SYMBOL!r} resolved to {size} byte(s), expected "
            f"{_ARMED_SYMBOL_EXPECTED_SIZE} -- refusing to trust this read "
            "(relay_owner_state_t's underlying type may have changed)"
        )

    ok, output = read_memory(PEER_PICO, addr, count=1, width=8)
    if not ok:
        return None, f"SWD read of s_state at 0x{addr:x} failed:\n{output.strip()}"

    match = _MEMRD_RE.search(output)
    if not match:
        return None, f"could not parse a MEMRD line out of read_memory() output:\n{output.strip()}"

    value = int(match.group(1), 16)
    # relay_owner_state_t only ever holds 0..3 (INIT/GRACE/ARMED/TRIPPED).
    # Anything else means the read address doesn't actually hold this
    # variable right now -- e.g. the attached board isn't running the ELF
    # this address was resolved from (different/older firmware flashed,
    # RAM not yet initialized this boot) -- found for real doing this
    # session's hardware smoke test: a live read came back 0xb7, not a valid
    # enum value. Fail closed rather than reporting a bogus "not armed".
    if value not in (0, 1, 2, 3):
        return None, (
            f"s_state at 0x{addr:x} read back 0x{value:x}, not a valid "
            "relay_owner_state_t (0-3) -- the attached board likely isn't "
            "running the ELF this address was resolved from; refusing to "
            "trust this read"
        )
    armed = value == _ARMED_ENUM_VALUE
    return armed, f"s_state=0x{value:x} at 0x{addr:x} ({'ARMED' if armed else 'not armed'})"


# Resuming an RP2040 over OpenOCD's SMP grouping needs EVERY core in the group
# halted first, or `resume` fails with "resume of a SMP target failed" and the
# board is left stopped -- which is worse than not resuming at all, because it
# looks like a firmware freeze. Halting each target by name first is generic
# (single-core peers have a one-element list) and makes the resume reliable.
_RESUME_TCL = (
    "foreach _kctl_t [target names] {targets $_kctl_t; halt}; "
    "targets [lindex [target names] 0]; resume;"
)


def read_memory(peer: str, address: int, count: int = 1, width: int = 32,
                leave_halted: bool = False,
                target: "str | None" = None) -> "tuple[bool, str]":
    """Reads ``count`` ``width``-bit words starting at ``address`` (read-only,
    no ARMED/safety implications). ``count`` is capped at 4096 to stop a
    runaway dump, not as a safety gate.

    Uses ``mem2array``/``puts`` rather than ``mdw``/``mdh``/``mdb``: those
    display commands print through OpenOCD's interactive command-output
    channel, which a one-shot ``-c`` batch session with no attached telnet/gdb
    client never surfaces on stdout -- found running this for real,
    2026-08-18, against the Pico's SIO GPIO_IN register: the call reported
    success (returncode 0, no ``Error:``) with the OpenOCD banner and shutdown
    chatter as its entire output, and nothing that looked like a memory dump.
    ``puts`` is a plain Tcl stdout write and does not have that problem.

    ``target`` selects one core by its OpenOCD target name, exactly as
    ``read_registers`` does. It matters for more than tidiness on an SMP chip:
    the NVIC and parts of the SCB are BANKED PER CORE on the RP2040, so
    NVIC_ISER (0xE000E100) and VTOR (0xE000ED08) read through core 0 say
    nothing about core 1's copy. Reading them without naming a core produces an
    answer that looks definitive and is silently about the wrong core -- which
    is how "the UART interrupt is enabled" and "the UART interrupt is not being
    serviced" came to be believed simultaneously on 2026-08-25. Peripheral
    addresses outside the private peripheral bus (0xE0000000-0xE00FFFFF) are
    shared, so ``target`` is irrelevant for those."""
    if width not in _MEM_WIDTH_READ_CMDS:
        raise ValueError(f"width must be one of {sorted(_MEM_WIDTH_READ_CMDS)}, got {width!r}")
    if count <= 0:
        raise ValueError(f"count must be positive, got {count!r}")
    if count > _MAX_READ_COUNT:
        raise ValueError(f"count {count} exceeds the {_MAX_READ_COUNT}-word cap")
    peer_cfg = resolve_peer(peer)
    step = width // 8
    dump = (
        f"mem2array _kctl_arr {width} 0x{address:x} {count}; "
        f"for {{set _kctl_i 0}} {{$_kctl_i < {count}}} {{incr _kctl_i}} "
        f'{{puts [format "MEMRD 0x%08x 0x%0{step * 2}x" '
        f"[expr {{0x{address:x} + $_kctl_i * {step}}}] $_kctl_arr($_kctl_i)]}}"
    )
    tail = "" if leave_halted else f" {_RESUME_TCL}"
    select = f"targets {target}; " if target else ""
    tcl = f"{_adapter_prefix(peer_cfg)}init; {select}halt; {dump};{tail} exit"
    return _run(peer, tcl)


def read_symbol(peer: str, symbol: str, count: Optional[int] = None, width: int = 32,
                elf_path: Optional[str] = None,
                leave_halted: bool = False) -> "tuple[bool, str]":
    """Reads a named symbol out of ``peer``'s memory -- ``read_memory()`` with
    the address looked up from the ELF instead of hand-computed.

    ``count`` defaults to covering the symbol's whole recorded size at the
    requested ``width`` (so a 16-byte register image reads as 16 bytes with
    ``width=8``), or 1 word when the ELF records no size.

    Refuses a symbol name that is defined at more than one address (two
    file-static objects sharing a name): those are different objects and
    picking one silently would produce a confidently wrong answer. Pass the
    address to ``read_memory()`` directly if you know which one you want --
    ``symbol_table()`` lists them.

    Note this reads the symbol's raw bytes; it does NOT know struct layout. A
    struct member's offset has to come from DWARF (``objdump --dwarf=info``),
    not from here -- assuming a member sits at offset 0 is how a healthy
    register image got misread as garbage on 2026-08-25."""
    table = symbol_table(peer, elf_path)
    entry = table.get(symbol)
    if entry is None:
        return False, f"symbol {symbol!r} not found in {elf_path or resolve_peer(peer).default_elf()}"
    address, size = entry
    if address < 0:
        return False, (
            f"symbol {symbol!r} is defined at more than one address in this ELF (file-static "
            "in several translation units) -- resolve it yourself and call read_memory() with "
            "the address you mean"
        )
    if count is None:
        step = width // 8
        count = max(1, size // step) if size else 1
    return read_memory(peer, address, count=count, width=width, leave_halted=leave_halted)


def write_memory(peer: str, address: int, value: int, width: int = 32) -> "tuple[bool, str]":
    """Writes one ``width``-bit ``value`` at ``address``.

    NOTE: this bare function has no ARMED-state gate of its own -- that policy
    (peer="pico" only: refuse if ``pico_armed_state()`` reports armed, or
    can't confidently determine the state) lives one layer up, in
    ``mcp_server.py``'s ``debug_write_memory()`` MCP wrapper, matching where
    this codebase's other write-time policy (e.g. ``debug_halt``'s
    profile-running guard) already lives. Call ``pico_armed_state()``
    yourself first if you're calling this directly rather than through the
    MCP tool.
    """
    cmd = _MEM_WIDTH_WRITE_CMDS.get(width)
    if cmd is None:
        raise ValueError(f"width must be one of {sorted(_MEM_WIDTH_WRITE_CMDS)}, got {width!r}")
    peer_cfg = resolve_peer(peer)
    tcl = f"{_adapter_prefix(peer_cfg)}init; halt; {cmd} 0x{address:x} 0x{value:x}; exit"
    return _run(peer, tcl)


# `primask` and `control` are here for a reason, not for completeness. Without
# them this dump cannot distinguish the two states that look identical in the
# 19 core registers above: a core running normally, and a core running with
# interrupts globally masked. That distinction cost a bench session on
# 2026-08-25 chasing a SaftyFW bootloader handover where UART1's interrupt was
# enabled in the NVIC, unmasked in the peripheral (IMSC 0x50), ASSERTED right
# then (MIS 0x50), with a vector pointing at valid code -- and never taken. The
# one register that could confirm or kill the obvious explanation was the one
# register this function did not read.
#
# Cortex-M0+ only: `basepri` and `faultmask` do not exist on ARMv6-M and asking
# OpenOCD for them fails the whole `get_reg` call, so they are deliberately
# absent rather than forgotten. Anything added here must exist on EVERY peer
# this function is called for.
_CORE_REGS = (
    "r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 sp lr pc xpsr msp psp "
    "primask control"
)


def read_registers(peer: str, target: "str | None" = None,
                   leave_halted: bool = False) -> "tuple[bool, str]":
    """Reads the core registers. Needs the core halted to read registers, so
    this halts it as a side effect.

    ``target`` selects one core by its OpenOCD target name on a multi-core
    chip (the RP2040 exposes ``rp2040.core0`` and ``rp2040.core1``); the
    default reads whichever core the config file makes current, which is
    core 0 for the Pico.

    Uses ``get_reg`` + ``puts`` rather than OpenOCD's ``reg`` command for the
    same reason ``read_memory`` avoids ``mdw`` -- see its docstring. ``reg``
    prints through the interactive command-output channel, which a one-shot
    ``-c`` batch session with no telnet/gdb client attached never surfaces on
    stdout: the call returns success with the OpenOCD banner and shutdown
    chatter as its entire output and not one register value. Confirmed the
    hard way, 2026-08-23, chasing a Pico core-1 launch failure where the
    empty result read as "the probe can't see the core" rather than "this
    function has never worked". ``puts`` is a plain Tcl stdout write.

    Resumes the core afterwards unless ``leave_halted`` is set. A read has to
    halt the core -- OpenOCD requires it -- and a one-shot batch session that
    exits without resuming leaves it halted, so the board looks alive to a
    debugger and completely dead to everything else. That cost a bench session
    on 2026-08-23: SaftyFW's xTickCount appeared frozen and the isolated link
    appeared to stop transmitting, when the only thing that had stopped them
    was the previous read. Pass ``leave_halted=True`` when you genuinely want
    the core held, e.g. across a series of reads that must all observe the
    same frozen state.
    """
    peer_cfg = resolve_peer(peer)
    select = f"targets {target}; " if target else ""
    dump = (
        f"foreach {{_kctl_n _kctl_v}} [get_reg {{{_CORE_REGS}}}] "
        '{puts [format "REG %-5s %s" $_kctl_n $_kctl_v]}'
    )
    tail = "" if leave_halted else f" {_RESUME_TCL}"
    tcl = f"{_adapter_prefix(peer_cfg)}init; {select}halt; {dump};{tail} exit"
    return _run(peer, tcl)
