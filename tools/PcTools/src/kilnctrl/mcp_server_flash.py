"""firmware flashing (JTAG/OpenOCD).

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

from . import actions, capability_preflight, config_presets, coredump_fetch, debug_probe, devices, elf_archive, esp_app_desc, flash_provenance, mcp_facade, openocd_util, partition_http_client, partition_table, pico_gpio_probe, safety_cfg_http_client, serial_link, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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


def _unit_test_fixture_fw_root() -> str:
    """firmware/UnitTestFw/UnitTest/ project root (the UnitTestFixture
    board's firmware), mirroring _kiln_fw_root()."""
    return os.path.join(debug_probe._repo_root(), "firmware", "UnitTestFw", "UnitTest")


# ---------------------------------------------------------------------------
# Two ESP32-S3 boards are now permanently on the bench (2026-09-05): the main
# board and the UnitTestFixture. Both boards' native USB-Serial-JTAG
# interface shares VID:PID 303A:1001 -- OpenOCD's board/esp32s3-builtin.cfg
# with no `adapter serial` binds to whichever one it enumerates first, which
# is a real risk of flashing the wrong image onto the wrong board now that
# both are attached simultaneously. Pin each flash path to its own board by
# USB serial number (same anchors as serial_link.py's identity table) and
# refuse outright, before touching OpenOCD, if that serial isn't enumerated.
# ---------------------------------------------------------------------------
MAIN_BOARD_JTAG_SERIAL = serial_link.MAIN_BOARD_JTAG_SERIAL
FIXTURE_JTAG_SERIAL = serial_link.FIXTURE_JTAG_SERIAL


def _enumerated_jtag_serials() -> "list[str]":
    """Thin wrapper -- see serial_link.enumerated_303a_1001_serials() for the
    single source of truth (shared with debug_probe.py's PEER_ESP path)."""
    return serial_link.enumerated_303a_1001_serials()


def _refuse_if_adapter_absent(expected_serial: str, board_label: str) -> Optional[str]:
    """Thin wrapper -- see serial_link.refuse_if_jtag_serial_absent() for the
    single source of truth (case-insensitive serial comparison; shared with
    debug_probe.py's PEER_ESP path) and the "before OpenOCD is invoked at
    all" rationale."""
    return serial_link.refuse_if_jtag_serial_absent(expected_serial, board_label)


def _run_openocd(openocd_exe: str, board_cfg_relpath: str, tcl_commands: str, cwd: str, timeout_s: int) -> tuple[bool, str]:
    """Thin wrapper over openocd_util.run_openocd() for a single cfg file,
    preserving flash_firmware()'s existing call signature."""
    return openocd_util.run_openocd(openocd_exe, [board_cfg_relpath], tcl_commands, cwd, timeout_s)


@_srv._tool()
def kill_openocd_sessions() -> str:
    """Force-kills any running openocd.exe processes on this PC.

    Use before flash_firmware() (it already does this itself) or on its own
    if a manual/GDB OpenOCD session was left attached and is holding the
    JTAG interface -- a second openocd instance can't open the USB-JTAG
    device while one is already running, which looks like "the board isn't
    responding" but is actually just this. Safe to call when nothing is
    running (reports that plainly, not an error)."""
    return openocd_util.kill_openocd_sessions_impl()


#: Post-flash verification is intentionally quick -- the board is already
#: rebooting off the reset in the OpenOCD tcl sequence, so this is a short
#: poll for it to come back up over Wi-Fi, not a long wait. Chosen to be
#: comfortably longer than a normal boot-to-HTTP-ready time without turning
#: an unreachable-board case into a multi-minute hang.
_VERIFY_POLL_ATTEMPTS = 5
_VERIFY_POLL_INTERVAL_S = 2.0
_VERIFY_HTTP_TIMEOUT_S = 3.0


def _resolve_verify_hosts(host: Optional[str], pre_flash_host: Optional[str] = None) -> "list[str]":
    """Ordered list of addresses to try for the post-flash HTTP checks.

    An EXPLICIT `host` is the only candidate -- if a caller named an
    address, silently probing others would hide their mistake.

    Otherwise, in order:
      1. `pre_flash_host` -- the address the board was ACTUALLY answering
         at moments before the flash (captured by _preflash_board_address()
         while the old firmware was still up). This is first because it is
         the only candidate backed by evidence rather than inference.
      2. `_ota_resolve_host(None)` -- the package-wide resolution (STA IP
         from wifi_get_status() over UART, else the AP fallback).
      3. the AP fallback address itself.

    WHY THIS IS A LIST AND NOT ONE ADDRESS (the 2026-09-04 defect): a
    previous fix had verification call `_ota_resolve_host(host)` once. That
    is the right resolution, at the wrong MOMENT -- it runs immediately
    after the OpenOCD tcl sequence resets the board, when the board has not
    re-associated with the AP yet, so wifi_get_status() reports
    sta_connected=False and resolution falls through to the AP fallback
    192.168.4.1. The board was on the LAN at 192.168.1.156 and answering
    fine; verification polled an address nothing was listening on, timed
    out, and emitted its soft "could not reach the board" WARNING -- which
    is emitted identically whether the flash landed or not, i.e. it said
    nothing in exactly the case it exists for. Trying an ordered list, and
    re-resolving on every poll attempt (Wi-Fi may come back mid-poll),
    fixes both halves of that. The AP address stays in the list because
    during genuine bring-up it IS the correct address."""
    candidates: "list[str]" = []

    def _add(h: Optional[str]) -> None:
        if h and h not in candidates:
            candidates.append(h)

    if host:
        _add(host)
        return candidates
    _add(pre_flash_host)
    from .mcp_server_ota import _ota_resolve_host  # local: avoids a circular import
    try:
        _add(_ota_resolve_host(None))
    except Exception:  # pragma: no cover - resolution is best-effort here
        pass
    _add(partition_http_client.PARTITION_AP_DEFAULT_HOST)
    return candidates


def _preflash_board_address(host: Optional[str]) -> Optional[str]:
    """Address the board is answering GET /api/partitions at RIGHT NOW,
    before anything is flashed -- or None if nothing answers.

    Two jobs. It gives post-flash verification an evidence-backed address
    to try first (see _resolve_verify_hosts), and it is the observation
    that makes the post-flash failure semantics meaningful: a board that
    demonstrably served HTTP one minute ago and cannot be reached at any
    candidate address afterwards is a HARD verification failure, not the
    benign "HTTP isn't up yet during bring-up" case. Without this
    observation the two are indistinguishable, which is what let a
    misdirected verifier read as benign noise."""
    for candidate in _resolve_verify_hosts(host):
        try:
            partition_http_client.get_partitions(candidate, timeout=_VERIFY_HTTP_TIMEOUT_S)
            return candidate
        except partition_http_client.PartitionHttpError:
            continue
    return None


def _verify_flash_landed(host: Optional[str], bin_path: str, pre_flash_host: Optional[str] = None) -> str:
    """Post-flash confirmation that the binary just written to `factory` is
    the one actually RUNNING -- added after the recurring "flash reports OK
    but the board keeps running old code" failure mode (see this module's
    header comment and esp_app_desc.py's docstring for the full history:
    flash_firmware() only ever writes the `factory` partition and never
    touches `otadata`; if an OTA ever pointed the boot target at
    ota_0/ota_1, the bootloader keeps booting that stale image forever, and
    OpenOCD's own "verified OK" during the write says nothing about which
    partition actually boots).

    Two independent checks, both against the RUNNING firmware, not the
    write itself:
      1. GET /api/partitions' "running" field must be "factory" -- anything
         else means the bootloader is not even attempting to run what was
         just flashed.
      2. GET /api/status's fw_build must match the build timestamp parsed
         out of the .bin's esp_app_desc_t -- catches a stale/mismatched
         `factory` image passing check 1 (e.g. a previous factory flash that
         never got overwritten because a caller thought a build was newer
         than it was).

    Returns "" on success (nothing worth reporting), a "WARNING: ..." string
    if the board could not be reached at all (this is not treated as a
    verification failure -- see flash_firmware()'s verify parameter), and
    raises RuntimeError with an actionable message on an actual mismatch
    (wrong running partition, or a build-timestamp mismatch)."""
    try:
        app_desc = esp_app_desc.parse_app_desc_file(bin_path)
    except (OSError, esp_app_desc.AppDescError) as exc:
        return f"WARNING: post-flash verification skipped -- could not parse app descriptor from {bin_path}: {exc}"

    # Host resolution is a LIST tried in order, re-resolved on every poll
    # attempt -- see _resolve_verify_hosts() for the full writeup of the
    # 2026-09-04 defect (single resolution, taken at the one moment the
    # board is guaranteed not to be on Wi-Fi yet, landing on the AP
    # fallback while the board answered fine on the LAN).
    last_exc: Optional[Exception] = None
    partitions_data: Optional[dict] = None
    resolved_host: Optional[str] = None
    tried: "list[str]" = []
    for attempt in range(_VERIFY_POLL_ATTEMPTS):
        if attempt:
            time.sleep(_VERIFY_POLL_INTERVAL_S)
        for candidate in _resolve_verify_hosts(host, pre_flash_host):
            if candidate not in tried:
                tried.append(candidate)
            try:
                partitions_data = partition_http_client.get_partitions(candidate, timeout=_VERIFY_HTTP_TIMEOUT_S)
                resolved_host = candidate
                last_exc = None
                break
            except partition_http_client.PartitionHttpError as exc:
                last_exc = exc
                continue
        if partitions_data is not None:
            break

    if partitions_data is None:
        addresses = ", ".join(tried) if tried else "(no candidate address)"
        if pre_flash_host:
            # The board answered HTTP at pre_flash_host minutes ago, with
            # the OLD firmware. Silence now is not "bring-up, HTTP isn't up
            # yet" -- something regressed across this flash (bad image,
            # boot loop, Wi-Fi provisioning wiped). Failing loud here is the
            # whole point: a soft warning in this case is indistinguishable
            # from a verifier pointed at the wrong address, which is what
            # made the 2026-09-04 defect read as benign noise for as long
            # as it did.
            raise RuntimeError(
                "flash reported OK, but the board is NOT answering HTTP after the "
                f"flash -- it was answering GET /api/partitions at {pre_flash_host} "
                f"immediately BEFORE the flash, and now none of {addresses} answer "
                f"after {_VERIFY_POLL_ATTEMPTS} attempts ({last_exc}). This is a "
                "verification FAILURE, not a bring-up timeout: the board that just "
                "served HTTP has stopped. Check the board is booting (serial/JTAG), "
                "and if it is stuck consider ota_rollback_esp() / a reflash. Pass "
                "verify=False only if you intend to skip this check entirely."
            )
        return (
            "WARNING: post-flash verification skipped -- board did not answer "
            f"GET /api/partitions at any of {addresses} after "
            f"{_VERIFY_POLL_ATTEMPTS} attempts ({last_exc}). The board was not "
            "answering HTTP before the flash either, so this is expected during "
            "early bring-up (Wi-Fi not provisioned yet); pass verify=False to "
            "silence this warning. It does NOT confirm the flash landed."
        )

    running = partitions_data.get("running")
    if running != "factory":
        raise RuntimeError(
            f"flash reported OK, but the board is running partition {running!r}, "
            "not 'factory' -- flash_firmware() only ever writes the factory "
            "partition, so this means an earlier OTA left the boot target "
            "pointed at ota_0/ota_1 and the bootloader is still booting THAT "
            "old image, not the one just flashed. Fix: call ota_rollback_esp() "
            "to restore the factory boot target, then flash_firmware() again."
        )

    board = capability_preflight.get_board_info(resolved_host, timeout=_VERIFY_HTTP_TIMEOUT_S)
    if not board.reachable:
        return (
            "WARNING: running partition confirmed 'factory', but GET /api/status "
            f"failed ({board.error}) so the build timestamp could not be checked."
        )
    if not esp_app_desc.build_timestamps_match(app_desc, board.fw_build):
        raise RuntimeError(
            "flash reported OK and the board is running 'factory', but its "
            f"reported build ({board.fw_build!r}) does not match the binary just "
            f"flashed ({app_desc.build_timestamp!r}) -- the board is running a "
            "DIFFERENT build than the one on disk. This can happen if factory "
            "was flashed once, then a stale/leftover .bin got flashed again "
            "without a fresh build_kilnfw, or if verification is racing a boot "
            "that hasn't finished yet. Rebuild with build_kilnfw and reflash."
        )
    return ""


def _maybe_reset_boot_guard(host: Optional[str], pre_flash_host: Optional[str],
                             ap_password: Optional[str]) -> str:
    """Called from flash_firmware()'s _post_flash() ONLY after
    _verify_flash_landed() returned "" -- i.e. full, unambiguous, verified
    success (running partition is 'factory' AND its build timestamp matches
    the .bin just flashed). Never called on a raise, a WARNING, or
    verify=False -- see flash_firmware()'s own `ap_password` doc comment and
    boot_guard_reset_counter()'s header comment (firmware/KilnFW/App/
    drivers/persist/boot_guard.h) for why: a board just flashed with
    something broken, or whose landing was never actually confirmed, must
    still be free to walk into recovery mode on its own.

    No-op (returns "") when `ap_password` is not given -- this is an
    additive, opt-in behavior; a caller that omits it gets exactly the
    pre-existing flash_firmware() behavior.

    Resolves the board address the SAME way _verify_flash_landed() just
    confirmed one was reachable at (via _preflash_board_address(), which
    walks the same _resolve_verify_hosts() candidate list) rather than
    trusting a stale `host`/`pre_flash_host` value -- the board that
    verification just talked to is the one this call needs to reach too.
    Never raises: a failure here is reported as a WARNING string appended
    to flash_firmware()'s result, because the flash itself already
    succeeded and landed by this point -- losing the recovery-counter clear
    is a real but non-fatal degradation (docs/audits/
    boot_guard_post_flash_recovery_footgun_2026-09-08.md's residual is that
    ordinary reflashing WITHOUT this call can still, eventually, walk a
    board into recovery mode; this is exactly the "eventually" case)."""
    if not ap_password:
        return ""
    resolved = _preflash_board_address(host) or pre_flash_host
    if not resolved:
        return ("WARNING: boot_guard_reset skipped -- could not resolve a board address to call "
                "POST /api/ota/esp/boot_guard_reset at, even though post-flash verification just "
                "succeeded against one; the recovery-mode counter was NOT cleared by this flash.")
    try:
        body = ota_http.boot_guard_reset_esp(resolved, ap_password)
    except ota_http.OtaHttpError as exc:
        _srv._session_log.warning("flash_firmware: boot_guard_reset call failed: %s", exc)
        return (f"WARNING: boot_guard_reset call failed ({exc}) -- the flash itself landed fine, "
                "but the recovery-mode counter was NOT cleared by this flash.")
    if body.get("ok"):
        return (f"boot_guard_reset: recovery-mode counter cleared and verified "
                f"(boot_count now {body.get('boot_count')})")
    return (f"WARNING: boot_guard_reset did not verify (board reported {body!r}) -- the flash "
            "itself landed fine, but the recovery-mode counter was NOT confirmed cleared; a run of "
            "ordinary reflashes could still eventually walk this board into recovery mode.")


def _archive_flashed_elf(build_dir: str, app_bin_path: str, tree_state,
                          kiln_fw_root: Optional[str]) -> str:
    """Best-effort: archives the exact ELF that was just flashed into the
    CANONICAL (main-tree) elf_archive so a later panic can be symbolized,
    regardless of whether this flash came from a kiln_fw_root override whose
    own build/ directory (and elf_archive) may not outlive this call. Never
    raises into the caller and never fails the flash result -- see
    elf_archive.py's module docstring for why this exists and archive_elf.cmake's
    header comment for the build-time counterpart this complements. Returns a
    short suffix string to append to the success message (empty on failure,
    logged instead)."""
    elf_path = os.path.join(build_dir, "KilnCtrl.elf")
    try:
        app_desc = esp_app_desc.parse_app_desc_file(app_bin_path)
        source = f"flash_firmware(kiln_fw_root={kiln_fw_root})" if kiln_fw_root else "flash_firmware"
        result = elf_archive.archive_kiln_elf(elf_path, app_desc.build_timestamp, tree_state.head, source)
        return f"\nelf archived: {result.archived_path} (key {result.elf_key})"
    except Exception as exc:  # noqa: BLE001 - archiving is a diagnostic convenience, never fail the flash over it
        _srv._session_log.warning("flash_firmware: elf archiving failed (non-fatal): %s", exc)
        return ""


@_srv._tool()
def flash_firmware(
    board_cfg: str = "board/esp32s3-builtin.cfg",
    retry_once: bool = True,
    allow_stale: bool = False,
    verify: bool = True,
    host: Optional[str] = None,
    allow_sensitive_dirty: bool = False,
    kiln_fw_root: Optional[str] = None,
    ap_password: Optional[str] = None,
) -> str:
    """Flashes KilnFW/build/{bootloader,partition_table,KilnCtrl}.bin to the
    board over JTAG via OpenOCD -- the ONLY sanctioned way to flash this
    board (never esptool/`idf.py flash`, per CLAUDE.md). Always writes all
    three images (bootloader @0x0, partition table @0x8000, app @0x810000),
    each with `program_esp ... verify` (which only erases/rewrites a region
    if its content doesn't already match -- it does NOT do a bare full-chip
    erase), ending in a reset so the board boots the new app immediately.

    Requires `idf.py build` to have already produced KilnFW/build/*.bin --
    this tool does not build, only flashes.

    Two ESP32-S3 boards are on the bench (2026-09-05: the main board and the
    UnitTestFixture), and both boards' native USB-Serial-JTAG interface
    shares VID:PID 303A:1001 -- `board_cfg`'s cfg file alone cannot tell them
    apart, so this pins OpenOCD's `adapter serial` to the main board's
    (MAIN_BOARD_JTAG_SERIAL) and refuses BEFORE calling OpenOCD at all if
    that serial isn't currently enumerated, naming whichever 303A:1001
    serial(s) ARE seen instead. Flashing the fixture is `fixture_flash()`,
    pinned the same way to its own serial -- never this tool.

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
    exactly what caused the incident this tool exists to prevent.

    After OpenOCD reports the write verified, this ALSO independently
    confirms the flash actually LANDED (verify=True, the default): it polls
    the board's own HTTP API for the partition it is actually running and
    the build timestamp it reports, and FAILS LOUD if either disagrees with
    what was just flashed. This exists because OpenOCD's "verify" is only a
    byte-compare during the write -- it says nothing about which partition
    the bootloader actually boots, and flash_firmware() only ever writes the
    `factory` partition. If an earlier OTA left the boot target pointed at
    ota_0/ota_1, every future flash_firmware() would otherwise report success
    forever while the board keeps running old code (this is exactly the
    "my change vanished" failure mode CLAUDE.md's flash_firmware section
    warns about -- this check is what makes that fail at the tool instead of
    costing a debugging session). On a mismatch the error names the actual
    running partition/build and tells you to call ota_rollback_esp() first.

    `verify=False` is the escape hatch for bring-up when the board's HTTP
    stack is not expected to be up yet (e.g. Wi-Fi not provisioned) -- skips
    verification entirely, no warning.

    When verify=True (the default) and the board does not answer HTTP after
    the flash, what happens depends on whether it was answering BEFORE it:
    this tool probes the board's HTTP address once up front (no flashing
    involved) and remembers it. If the board was NOT answering beforehand,
    post-flash silence is the expected bring-up case and is reported as a
    WARNING. If it WAS answering beforehand and is silent afterwards, that
    is a hard FAILURE -- a board that was serving HTTP a minute ago and has
    stopped is a real regression, and reporting it as a warning would make
    it indistinguishable from a verifier looking at the wrong address (the
    exact defect fixed 2026-09-04). Wrong-partition/wrong-build mismatches
    always raise, as before.

    `host`: board IP/hostname for the verification HTTP calls. If given, it
    is the ONLY address probed. If omitted, an ordered candidate list is
    tried and re-resolved on every poll attempt (see
    `_resolve_verify_hosts`): the address the board was actually answering
    at just before the flash, then `_ota_resolve_host(None)`'s answer (STA
    IP via wifi_get_status()), then the fallback-AP address 192.168.4.1 for
    a board that is not on home Wi-Fi yet.

    Provenance / dirty-tree guard (added 2026-09-04 after an agent flashing
    for an unrelated diagnosis carried another session's in-progress
    zones_config schema-migration edits onto the board -- see
    flash_provenance.py's module docstring for the full incident and the
    guard-shape tradeoffs): every call records `git status --porcelain` and
    HEAD at the moment of the flash -- unscoped, the whole repo, since a
    shared working tree means the risk is not limited to KilnFW/CommonFW --
    and reports it in the result so an operator can see exactly what rode
    along, dirty tree or not. That capture is ALSO persisted to
    KilnFW/build/flash_provenance.json so "what was actually on the board at
    <time>" is answerable later from disk.

    This does NOT refuse on an ordinary dirty tree -- several sessions
    sharing one working tree is this project's normal state (CLAUDE.md), so
    a blanket dirty-tree refusal would block routine work daily and get
    disabled permanently. It refuses only when the dirty set touches a
    narrow, named sensitive list (config-schema/migration code, safety
    config -- see flash_provenance.SENSITIVE_PATTERNS), which is exactly
    what the incident above involved. Pass allow_sensitive_dirty=True only
    after you have actually looked at the named files and intend them to be
    on the board -- this is a deliberate, visible override, not a default.

    `kiln_fw_root`: overrides which `firmware/KilnFW`-shaped directory the
    build output (and provenance) is read from -- defaults to exactly the
    main working tree's `firmware/KilnFW` (unchanged behaviour when omitted).
    Use this for the sanctioned "build from a clean git worktree checked out
    at HEAD" workflow, when the main working tree carries another session's
    foreign WIP that would otherwise ride along (or trip the sensitive-dirty
    guard above) -- point this at that worktree's `firmware/KilnFW` instead
    of stashing/committing someone else's in-progress edits. Must be an
    ABSOLUTE path to an existing directory whose `build/` already holds the
    three expected binaries (bootloader.bin, partition-table.bin,
    KilnCtrl.bin) -- a relative path or a directory missing any of those is
    refused with a clear error before OpenOCD is touched. The git-provenance
    record and the sensitive-dirty-file guard are evaluated against THAT
    tree (its own `git status --porcelain`/HEAD, taken as two levels above
    this path), not the main tree, and the persisted
    `flash_provenance.json` (still written under the override's own
    `build/`) records `kiln_fw_root_override` so a later reader knows this
    flash did not come from the ordinary path. Post-flash verification
    (`verify=True`) compares against THAT tree's `.bin`, unchanged
    otherwise.

    `ap_password`: when given AND `verify=True`, and ONLY once post-flash
    verification confirms full success (the board is running `factory` with
    the exact build just flashed -- `_verify_flash_landed()` returned "",
    not a WARNING and not a raise), this also calls
    `POST /api/ota/esp/boot_guard_reset` (ota_http_client.boot_guard_reset_esp())
    to clear `boot_guard`'s recovery-mode counter directly -- the tool-driven
    fix for docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md:
    boot_confirm_is_healthy()'s own auto-clear depends on a one-shot NVS
    snapshot that can be wrong for a boot or two right after a flash-induced
    reset, and a run of ordinary development reflashes can otherwise walk a
    perfectly healthy board into recovery mode for a reason that has nothing
    to do with whether the firmware can boot. Deliberately gated on FULL
    verified success, never on a failed, unverified, or merely-warned-about
    flash (`verify=False`, an unreachable board, a wrong-partition/
    wrong-build mismatch, or a soft bring-up WARNING) -- a board that was
    just flashed with something broken, or whose landing was never actually
    confirmed, must still be free to walk into recovery mode on its own; see
    boot_guard_reset_counter()'s own header comment on why this must never
    run except from a tool that KNOWS a deliberate, confirmed-good flash just
    happened. A failure to reach or verify this call is reported as a
    WARNING appended to the result, never raised -- the flash itself already
    succeeded and landed by this point, and a caller who omits `ap_password`
    (the default) gets the same behavior as before this parameter existed:
    no attempt, no warning."""
    openocd_exe = _find_openocd_exe()
    if not openocd_exe:
        return "error: openocd.exe not found under ~/.espressif/tools/openocd-esp32/ or C:\\Espressif\\ -- is it installed?"

    if kiln_fw_root is not None:
        if not os.path.isabs(kiln_fw_root):
            return f"error: kiln_fw_root must be an absolute path, got {kiln_fw_root!r}"
        if not os.path.isdir(kiln_fw_root):
            return f"error: kiln_fw_root does not exist or is not a directory: {kiln_fw_root}"
        effective_kiln_fw_root = kiln_fw_root
    else:
        effective_kiln_fw_root = _kiln_fw_root()

    build_dir = os.path.join(effective_kiln_fw_root, "build")
    required = [
        os.path.join(build_dir, "bootloader", "bootloader.bin"),
        os.path.join(build_dir, "partition_table", "partition-table.bin"),
        os.path.join(build_dir, "KilnCtrl.bin"),
    ]
    missing = [p for p in required if not os.path.isfile(p)]
    if missing:
        override_note = " (kiln_fw_root override)" if kiln_fw_root else ""
        return f"error: missing build output(s){override_note}, run `idf.py build` first: " + ", ".join(missing)

    adapter_refusal = _refuse_if_adapter_absent(MAIN_BOARD_JTAG_SERIAL, "main board (ESP32-S3)")
    if adapter_refusal:
        return adapter_refusal

    provenance_path = os.path.join(build_dir, "flash_provenance.json")
    # An override tree's own git identity, not the main tree's: kiln_fw_root
    # is expected to be `<some-tree-root>/firmware/KilnFW`, so its tree root
    # is two levels up. See flash_firmware()'s `kiln_fw_root` docstring.
    provenance_repo_root = (
        os.path.normpath(os.path.join(kiln_fw_root, "..", "..")) if kiln_fw_root else None
    )
    tree_state = flash_provenance.capture_tree_state(repo_root=provenance_repo_root)
    guard_reason = flash_provenance.decide_guard(tree_state, allow_sensitive_dirty=allow_sensitive_dirty)
    if guard_reason:
        # Persist the REFUSAL itself, not just the tree snapshot that led to
        # it -- see flash_provenance.OUTCOME_* / format_last_flash_warning.
        # Before 2026-09-04 this file looked identical whether the flash
        # landed or was refused, which is exactly how a refused flash went
        # unnoticed for five hours: nothing on disk said "this didn't happen".
        flash_provenance.write_provenance_json(
            tree_state, provenance_path,
            outcome=flash_provenance.OUTCOME_REFUSED_SENSITIVE_DIRTY,
            detail=guard_reason,
            kiln_fw_root_override=kiln_fw_root,
        )
        _srv._session_log.warning("flash_firmware: refused -- sensitive dirty files: %s", tree_state.sensitive_files)
        return "error: " + guard_reason + "\n\n" + flash_provenance.format_report(tree_state)
    flash_provenance.write_provenance_json(
        tree_state, provenance_path, outcome=flash_provenance.OUTCOME_PENDING,
        kiln_fw_root_override=kiln_fw_root,
    )
    provenance_note = flash_provenance.format_report(tree_state)
    if kiln_fw_root:
        provenance_note += f"\nprovenance: kiln_fw_root override in use: {kiln_fw_root}"
    _srv._session_log.info("flash_firmware: %s", provenance_note.replace("\n", " | "))

    stale = stale_check.check_kilnfw_stale(effective_kiln_fw_root)
    if stale.stale:
        _srv._session_log.warning("flash_firmware: stale binary detected: %s", stale.reason)
        if not allow_stale:
            flash_provenance.write_provenance_json(
                tree_state, provenance_path,
                outcome=flash_provenance.OUTCOME_REFUSED_STALE_BINARY,
                detail=stale.reason,
                kiln_fw_root_override=kiln_fw_root,
            )
            return (
                "error: refusing to flash a stale binary -- " + stale.reason + "\n\n"
                "Rebuild with build_kilnfw first, or pass allow_stale=True to flash "
                "this binary anyway."
            )

    # Observe the board BEFORE touching it: if it is serving HTTP now, that
    # address is both the best candidate for post-flash verification and the
    # evidence that makes a post-flash silence a hard failure rather than a
    # bring-up warning (see _preflash_board_address / _verify_flash_landed).
    pre_flash_host = _preflash_board_address(host) if verify else None
    if verify:
        _srv._session_log.info("flash_firmware: pre-flash board HTTP address: %s", pre_flash_host or "(not answering)")

    kill_openocd_sessions()  # a stale session holding the JTAG interface looks identical to a flash failure

    tcl = (
        f"adapter serial {MAIN_BOARD_JTAG_SERIAL}; "
        "program_esp build/bootloader/bootloader.bin 0x0 verify; "
        "program_esp build/partition_table/partition-table.bin 0x8000 verify; "
        "program_esp build/KilnCtrl.bin 0x810000 verify reset exit"
    )
    stale_prefix = f"WARNING: flashed a stale binary anyway ({stale.reason})\n" if (stale.stale and allow_stale) else ""

    app_bin_path = os.path.join(build_dir, "KilnCtrl.bin")

    def _post_flash(base_msg: str) -> str:
        base_msg = (
            f"{base_msg}\n\n{provenance_note}\n\n"
            "NOTE: this reset the ESP. If the Pico was also reset around the same "
            "time (a dual reflash), expect a correct S6a (mainFault) trip while the "
            "ESP's safety link handshake is still coming up -- see docs/audits/"
            "s6a_startup_grace_revert_2026-09-07.md. Confirm the link is up "
            "(safety_get_status shows link up and FW_VERSION exchanged) before "
            "calling safety_clear_trip() -- it will just re-trip if the link isn't "
            "actually up yet."
        )
        if not verify:
            return base_msg
        try:
            landed_note = _verify_flash_landed(host, app_bin_path, pre_flash_host)
        except RuntimeError as exc:
            _srv._session_log.warning("flash_firmware: post-flash verification FAILED: %s", exc)
            return f"error: {exc}\n\n(the OpenOCD write itself reported OK -- {base_msg})"
        if landed_note:
            _srv._session_log.warning("flash_firmware: %s", landed_note)
            return f"{base_msg}\n{landed_note}"
        # Full verified success ONLY (landed_note == "") -- see
        # _maybe_reset_boot_guard()'s own doc comment for why this must
        # never run on a raise (handled above, already returned), a WARNING
        # (handled just above), or verify=False (already returned earlier).
        boot_guard_note = _maybe_reset_boot_guard(host, pre_flash_host, ap_password)
        suffix = f"\n{boot_guard_note}" if boot_guard_note else ""
        return (f"{base_msg}, and post-flash verification confirmed the board is running factory "
                f"with the matching build{suffix}")

    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=effective_kiln_fw_root, timeout_s=90)
    if ok:
        flash_provenance.write_provenance_json(
            tree_state, provenance_path, outcome=flash_provenance.OUTCOME_FLASHED_OK,
            kiln_fw_root_override=kiln_fw_root,
        )
        archive_note = _archive_flashed_elf(build_dir, app_bin_path, tree_state, kiln_fw_root)
        return _post_flash(stale_prefix + "flashed and verified OK (bootloader + partition table + app), board reset and running" + archive_note)

    if retry_once:
        _srv._session_log.warning("flash_firmware: first attempt failed, retrying once (known benign quirk)")
        ok2, output2 = _run_openocd(openocd_exe, board_cfg, tcl, cwd=effective_kiln_fw_root, timeout_s=90)
        if ok2:
            flash_provenance.write_provenance_json(
                tree_state, provenance_path, outcome=flash_provenance.OUTCOME_FLASHED_OK,
                kiln_fw_root_override=kiln_fw_root,
            )
            archive_note = _archive_flashed_elf(build_dir, app_bin_path, tree_state, kiln_fw_root)
            return _post_flash("flashed and verified OK on retry (first attempt hit the known benign Verify-Failed quirk)" + archive_note)
        output = output2

    tail = "\n".join(output.strip().splitlines()[-25:])
    flash_provenance.write_provenance_json(
        tree_state, provenance_path,
        outcome=flash_provenance.OUTCOME_FLASH_FAILED,
        detail=tail,
        kiln_fw_root_override=kiln_fw_root,
    )
    return (
        "error: flash failed" + (" twice" if retry_once else "") + f":\n{tail}\n\n"
        "Do not run a raw `flash erase_sector`/full-chip-erase recovery by hand -- "
        "that combination (partial reflash after a full erase) is what caused the "
        "2026-08-11 incident this tool exists to prevent. If this persists, a full "
        "USB power cycle of the board (not just a JTAG reset) has resolved a "
        "flash-write-protect-stuck state before."
    )


def _reject_kiln_fw_build_path(path: str) -> Optional[str]:
    """None if `path` does NOT resolve under the MAIN board's
    (KilnFW's) build directory; otherwise an error string.

    Guards against the easy mixup of pointing fixture_flash() at
    `firmware/KilnFW/build/KilnCtrl.bin` (the main board's own app image) by
    accident -- that would flash the MAIN board's firmware onto the
    FIXTURE's chip, which happens to run fine (both are plain ESP32-S3s) and
    so would not fail loudly; it would just leave the fixture running the
    wrong firmware with no relay-control task. Resolved via realpath so a
    relative path, a symlink, or `..` segments can't walk around this."""
    kiln_build_root = os.path.realpath(os.path.join(_kiln_fw_root(), "build"))
    real = os.path.realpath(path)
    try:
        common = os.path.commonpath([real, kiln_build_root])
    except ValueError:  # different drives on Windows -- definitely not under it
        return None
    if common == kiln_build_root:
        return (
            f"error: {path!r} resolves under the MAIN board's build directory "
            f"({kiln_build_root}) -- refusing to flash a KilnFW image onto the "
            "fixture. Pass allow_cross_board_path=True only if this is "
            "deliberate (e.g. testing that both boards run identical firmware)."
        )
    return None


def fixture_flash(
    bootloader_bin: Optional[str] = None,
    partition_table_bin: Optional[str] = None,
    app_bin: Optional[str] = None,
    board_cfg: str = "board/esp32s3-builtin.cfg",
    retry_once: bool = True,
    allow_cross_board_path: bool = False,
) -> str:
    """Flashes the UnitTestFixture board (also an ESP32-S3) over JTAG via
    OpenOCD, pinned to that board's own USB serial number
    (FIXTURE_JTAG_SERIAL) -- the counterpart to flash_firmware() for the
    MAIN board. Refuses immediately, before calling OpenOCD at all, if that
    serial is not currently enumerated (see _refuse_if_adapter_absent()),
    naming whichever 303A:1001 serial(s) ARE seen instead -- both boards
    share VID:PID 303A:1001 on their native USB-Serial-JTAG interface, so an
    unpinned `adapter serial` would otherwise let OpenOCD silently bind
    whichever board it enumerates first, same risk flash_firmware() guards
    against on the main board's side.

    `firmware/UnitTestFw/` has no build tooling wired into this codebase yet
    (no dedicated `build_*` MCP tool, per docs/UNIT_TEST_FIXTURE_PLAN.md), so
    unlike flash_firmware() this tool does not assume a fixed KilnFW-style
    `build/` layout -- pass explicit paths for whichever of the three images
    you built. Any omitted path is left out of the flash sequence entirely
    (useful for flashing just an updated app image without re-touching the
    bootloader/partition table), so at least one of the three must be given.

    Same 3-image safety rule as flash_firmware(): each image is written with
    `program_esp ... verify`, never a bare full-chip erase, and a single
    "Verify Failed" on the first attempt is retried once automatically
    (retry_once=True, the default) before this reports failure.

    Deliberately does NOT do flash_firmware()'s post-flash HTTP verification
    (partition/build-timestamp check) -- the fixture firmware's HTTP surface
    (if any) is out of scope for this task; this tool only confirms the
    OpenOCD write itself succeeded.

    Refuses any image path that resolves under the MAIN board's
    (`firmware/KilnFW/build/`) directory -- e.g. `KilnCtrl.bin` -- unless
    `allow_cross_board_path=True` is passed explicitly: both boards are
    plain ESP32-S3s, so a KilnFW image would flash and run "successfully" on
    the fixture while silently not being the fixture's own firmware at all
    (no relay-control task), a failure mode that would otherwise surface
    only much later, on the bench, as "why doesn't fixture_set_relay work"."""
    openocd_exe = _find_openocd_exe()
    if not openocd_exe:
        return "error: openocd.exe not found under ~/.espressif/tools/openocd-esp32/ or C:\\Espressif\\ -- is it installed?"

    images: "list[tuple[str, str]]" = []  # (path, flash offset)
    if bootloader_bin:
        images.append((bootloader_bin, "0x0"))
    if partition_table_bin:
        images.append((partition_table_bin, "0x8000"))
    if app_bin:
        images.append((app_bin, "0x810000"))
    if not images:
        return (
            "error: no image path given -- pass at least one of bootloader_bin, "
            "partition_table_bin, app_bin."
        )

    if not allow_cross_board_path:
        for path, _off in images:
            cross_board_refusal = _reject_kiln_fw_build_path(path)
            if cross_board_refusal:
                return cross_board_refusal

    missing = [p for p, _off in images if not os.path.isfile(p)]
    if missing:
        return "error: missing build output(s): " + ", ".join(missing)

    adapter_refusal = _refuse_if_adapter_absent(FIXTURE_JTAG_SERIAL, "UnitTestFixture board")
    if adapter_refusal:
        return adapter_refusal

    kill_openocd_sessions()  # a stale session holding the JTAG interface looks identical to a flash failure

    # See program()'s comment in debug_probe.py: forward slashes sidestep
    # Tcl's backslash escaping of a bare Windows path.
    parts = [f"adapter serial {FIXTURE_JTAG_SERIAL}"]
    for path, offset in images:
        parts.append(f'program_esp "{path.replace(chr(92), "/")}" {offset} verify')
    parts[-1] += " reset exit"
    tcl = "; ".join(parts)

    fixture_root = _unit_test_fixture_fw_root()
    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=fixture_root, timeout_s=90)
    if ok:
        return "flashed and verified OK (" + ", ".join(os.path.basename(p) for p, _ in images) + "), board reset"

    if retry_once:
        _srv._session_log.warning("fixture_flash: first attempt failed, retrying once (known benign quirk)")
        ok2, output2 = _run_openocd(openocd_exe, board_cfg, tcl, cwd=fixture_root, timeout_s=90)
        if ok2:
            return "flashed and verified OK on retry (" + ", ".join(os.path.basename(p) for p, _ in images) + "), board reset"
        output = output2

    tail = "\n".join(output.strip().splitlines()[-25:])
    return "error: fixture flash failed" + (" twice" if retry_once else "") + f":\n{tail}"


# ---------------------------------------------------------------------------
# On-chip partition-table confirmation -- FLASH_BUDGET.md section 8 item
# 3. flash_firmware() above reports "flashed and verified OK (bootloader +
# partition table + app)" after a program(), but that is OpenOCD's own
# byte-compare during the flash operation, not independently re-checkable
# later, and check_flash_partition_map.ps1 only validates the REPO's
# partitions.csv -- neither confirms what actually ended up on the chip.
#
# This tool ORIGINALLY read the partition-table bytes back over JTAG at
# flash offset 0x8000. That does not work on this chip -- 0x8000 is a FLASH
# offset, not a memory-mapped address OpenOCD's read_memory can reach; see
# partition_table.py's module docstring for the confirmed failure output.
# It now asks the RUNNING FIRMWARE for its own live partition table over
# GET /api/partitions (partition_info_http.c, via ESP-IDF's esp_partition
# iterator from inside the app) -- no JTAG, no core halt, works while the
# board is busy. See partition_table.py for the parsing/diff logic (unit-
# tested against synthetic blobs, no board required) and
# partition_http_client.py for the HTTP client.
# ---------------------------------------------------------------------------
@_srv._tool()
def debug_check_partition_table(host: Optional[str] = None, csv_path: Optional[str] = None) -> str:
    """Reads the RUNNING firmware's live partition table over
    GET /api/partitions and diffs it entry-by-entry against `csv_path`
    (defaults to firmware/KilnFW/partitions.csv). This is the way to
    independently confirm what partition table the board is actually using:
    neither flash_firmware()'s own "verified OK" nor
    check_flash_partition_map.ps1 read anything off the board itself.

    NOT a JTAG operation -- no core halt, safe to call even while a fire
    profile is running (this only exercises the board's existing HTTP
    server, same as get_dashboard_status or any other GET /api/* tool).

    Same host-resolution order as every ota_*/adaptive_tune_* tool
    (`_ota_resolve_host`): explicit `host` argument, else the STA IP if
    known, else the fallback AP address.

    Reports MATCH if every partition's type/subtype/offset/size the
    firmware reports agrees with the CSV, or a line-by-line MISMATCH
    otherwise (partitions only on the chip, only in the CSV, or present in
    both with differing fields)."""
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py

    resolved = _ota_resolve_host(host)
    try:
        diff, chip_entries, csv_entries = partition_table.check_chip_partition_table_via_http(
            host=resolved, csv_path=csv_path
        )
    except (ValueError, RuntimeError, FileNotFoundError) as exc:
        return f"error: {exc} (host={resolved})"
    _srv._session_log.warning(
        "debug_check_partition_table: host=%s chip_entries=%d csv_entries=%d ok=%s",
        resolved, len(chip_entries), len(csv_entries), diff.ok,
    )
    return f"host={resolved}\n{diff.report()}"


@_srv._tool()
def find_crash_elf(host: Optional[str] = None, fw_build: Optional[str] = None) -> str:
    """Finds the ELF that matches the ESP's CURRENTLY RUNNING firmware, so a
    coredump/panic backtrace can be symbolized against the right file instead
    of `build/KilnCtrl.elf` (whatever was built most recently -- confidently
    wrong once the board is running an older flash, per CLAUDE.md's firmware
    gotchas). One call in place of hunting through
    `firmware/KilnFW/build/elf_archive/` by hand.

    Looks up `elf_archive`'s manifest (populated by every flash_firmware()
    call -- see elf_archive.py -- plus archive_elf.cmake's own POST_BUILD
    step) keyed by the board's own reported build timestamp.

    `fw_build`: pass this directly if you already have it (e.g. from a
    crash_report or get_heap_status result) -- skips querying the board.
    Otherwise this queries `GET /api/status` at `host` (same resolution order
    as every ota_*/adaptive_tune_* tool: explicit host, else STA IP, else the
    fallback AP address).

    Fails LOUD with no match rather than falling back to KilnCtrl-latest.elf
    or the newest-by-mtime file -- either would silently reproduce the exact
    failure mode (confident wrong line numbers) this tool exists to prevent."""
    if fw_build is None:
        from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py
        resolved = _ota_resolve_host(host)
        try:
            info = capability_preflight.get_board_info(resolved)
        except Exception as exc:  # noqa: BLE001 - report as a normal tool error, not a crash
            return f"error: could not query board at {resolved}: {exc}"
        fw_build = info.fw_build
        if not fw_build:
            return f"error: board at {resolved} did not report fw_build in /api/status"
    path, message = elf_archive.find_kiln_elf_for_build(fw_build)
    if path is None:
        return f"error: {message}"
    return message


@_srv._tool()
def read_esp_coredump(host: Optional[str] = None, out_path: Optional[str] = None,
                       elf_path: Optional[str] = None, symbolize: bool = True) -> str:
    """Reads the ESP's coredump partition over HTTP (diagnostics_http.c's
    `/api/coredump/info` + `/api/coredump/chunk`, 2026-09-14) -- no JTAG, no
    OpenOCD, nothing that touches the board's execution state. This is the
    sanctioned replacement for probing the coredump over a debug interface:
    see docs/audits/esp_coredump_extraction_2026-09-14.md for the incident
    (a believed-read-only OpenOCD flash-bank query loaded an on-target
    flasher stub, hung, tripped the interrupt watchdog, and reset the board)
    that this tool exists to make unnecessary.

    Fetches into `out_path` (default: a coredump_<host>.bin file under the
    OS temp dir) and, if `symbolize` (default True), also finds the matching
    archived ELF via `find_crash_elf()` and runs espcoredump against both.
    Fails LOUD -- never a plausible-looking wrong backtrace -- on: no
    coredump present, a truncated transfer, no archived ELF found for the
    board's reported fw_build, or an ELF that does not match the coredump
    (espcoredump's own SHA256 check, propagated verbatim). Pass `elf_path`
    to symbolize against a specific ELF instead of looking one up.
    Pass `symbolize=False` to only fetch the raw file (e.g. no ESP-IDF
    toolchain available on this machine) -- still gets you the coredump off
    the board and onto disk, uninspected."""
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import with mcp_server_ota.py
    resolved = _ota_resolve_host(host)

    if out_path is None:
        import tempfile
        safe_host = resolved.replace(":", "_").replace("/", "_")
        out_path = os.path.join(tempfile.gettempdir(), f"coredump_{safe_host}.bin")

    try:
        n = coredump_fetch.fetch_coredump_over_http(resolved, out_path)
    except coredump_fetch.CoredumpFetchError as exc:
        return f"error: {exc}"

    result = f"fetched coredump: {n} bytes from {resolved} -> {out_path}"
    if not symbolize:
        return result

    fw_build = None
    if elf_path is None:
        try:
            info = capability_preflight.get_board_info(resolved)
        except Exception as exc:  # noqa: BLE001 - report as a normal tool error, not a crash
            return f"{result}\nerror: could not query board at {resolved} for fw_build to find a matching ELF: {exc}"
        fw_build = info.fw_build
        if not fw_build:
            return f"{result}\nerror: board at {resolved} did not report fw_build in /api/status -- pass elf_path explicitly"
        elf_path, message = elf_archive.find_kiln_elf_for_build(fw_build)
        if elf_path is None:
            return f"{result}\nerror: {message}"

    try:
        symbolized = coredump_fetch.symbolize_coredump(out_path, elf_path, fw_build=fw_build or "<given explicitly>")
    except coredump_fetch.CoredumpSymbolizeError as exc:
        return f"{result}\nerror: {exc}"
    return f"{result}\nelf={elf_path}\n{symbolized}"


@_srv._tool()
def find_safty_crash_elf(commit: str, build_date: Optional[str] = None, build_time: Optional[str] = None) -> str:
    """SaftyFW/Pico counterpart to find_crash_elf() -- looks up the archived
    ELF matching a given SAFTYFW_GIT_COMMIT (plus optional build date/time to
    disambiguate multiple builds from the same commit).

    SaftyFW has no HTTP API, so there is no automatic "ask the board" path
    the way find_crash_elf() has -- get `commit` from wherever the board's
    identity is already known for this diagnosis (a live SWD register/symbol
    read via debug_read_symbol, or a recent debug_program()/build log).

    Same loud-failure contract as find_crash_elf(): no match, or an
    ambiguous multi-build match, is reported as an error naming what was
    searched -- never a guessed substitute."""
    path, message = elf_archive.find_safty_elf_for_identity(commit, build_date, build_time)
    if path is None:
        return f"error: {message}"
    return message
