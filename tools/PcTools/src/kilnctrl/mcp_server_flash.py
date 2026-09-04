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

from . import actions, capability_preflight, config_presets, debug_probe, devices, esp_app_desc, flash_provenance, mcp_facade, openocd_util, partition_http_client, partition_table, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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


def _verify_flash_landed(host: Optional[str], bin_path: str) -> str:
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

    # Use the SAME host resolution every other board-HTTP tool in this
    # package uses (explicit host, else the board's current STA IP via
    # wifi_get_status(), else the fallback-AP address) -- not a bare
    # PARTITION_AP_DEFAULT_HOST. A board on home Wi-Fi does not answer at
    # 192.168.4.1, so defaulting to the AP address made the normal
    # `flash_firmware()` call (no host argument -- the form CLAUDE.md
    # documents) poll an address nothing is listening on, time out, and
    # return the "could not reach the board" WARNING every single time:
    # the verification would never actually run on the very failure mode it
    # was added to catch. Local import mirrors debug_check_partition_table()
    # below -- it avoids a circular import with mcp_server_ota.py.
    from .mcp_server_ota import _ota_resolve_host
    resolved_host = _ota_resolve_host(host)

    last_exc: Optional[Exception] = None
    partitions_data: Optional[dict] = None
    for attempt in range(_VERIFY_POLL_ATTEMPTS):
        if attempt:
            time.sleep(_VERIFY_POLL_INTERVAL_S)
        try:
            partitions_data = partition_http_client.get_partitions(resolved_host, timeout=_VERIFY_HTTP_TIMEOUT_S)
            last_exc = None
            break
        except partition_http_client.PartitionHttpError as exc:
            last_exc = exc
            continue

    if partitions_data is None:
        return (
            "WARNING: post-flash verification skipped -- board did not answer "
            f"GET /api/partitions at {resolved_host} after {_VERIFY_POLL_ATTEMPTS} "
            f"attempts ({last_exc}). This does NOT confirm the flash landed -- "
            "if HTTP normally comes up on this board, treat that as suspicious; "
            "otherwise this is expected during early bring-up (pass verify=False "
            "to silence this warning)."
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


@_srv._tool()
def flash_firmware(
    board_cfg: str = "board/esp32s3-builtin.cfg",
    retry_once: bool = True,
    allow_stale: bool = False,
    verify: bool = True,
    host: Optional[str] = None,
    allow_sensitive_dirty: bool = False,
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
    verification entirely, no warning. When verify=True (the default) but the
    board simply does not answer HTTP within a short poll, this is NOT
    treated as a failure -- it's reported back as a WARNING line so the
    caller knows verification did not happen and why, distinct from an
    actual wrong-partition/wrong-build failure which always raises.

    `host`: board IP/hostname for the verification HTTP calls. Resolved
    exactly like every ota_*/debug_check_partition_table tool
    (`_ota_resolve_host`): the explicit argument if given, else the board's
    current station IP from wifi_get_status(), else the fallback-AP address
    192.168.4.1 for a board that is not on home Wi-Fi yet.

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
    on the board -- this is a deliberate, visible override, not a default."""
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

    tree_state = flash_provenance.capture_tree_state()
    guard_reason = flash_provenance.decide_guard(tree_state, allow_sensitive_dirty=allow_sensitive_dirty)
    flash_provenance.write_provenance_json(
        tree_state, os.path.join(build_dir, "flash_provenance.json")
    )
    if guard_reason:
        _srv._session_log.warning("flash_firmware: refused -- sensitive dirty files: %s", tree_state.sensitive_files)
        return "error: " + guard_reason + "\n\n" + flash_provenance.format_report(tree_state)
    provenance_note = flash_provenance.format_report(tree_state)
    _srv._session_log.info("flash_firmware: %s", provenance_note.replace("\n", " | "))

    stale = stale_check.check_kilnfw_stale(kiln_fw_root)
    if stale.stale:
        _srv._session_log.warning("flash_firmware: stale binary detected: %s", stale.reason)
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

    app_bin_path = os.path.join(build_dir, "KilnCtrl.bin")

    def _post_flash(base_msg: str) -> str:
        base_msg = f"{base_msg}\n\n{provenance_note}"
        if not verify:
            return base_msg
        try:
            landed_note = _verify_flash_landed(host, app_bin_path)
        except RuntimeError as exc:
            _srv._session_log.warning("flash_firmware: post-flash verification FAILED: %s", exc)
            return f"error: {exc}\n\n(the OpenOCD write itself reported OK -- {base_msg})"
        if landed_note:
            _srv._session_log.warning("flash_firmware: %s", landed_note)
            return f"{base_msg}\n{landed_note}"
        return f"{base_msg}, and post-flash verification confirmed the board is running factory with the matching build"

    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=kiln_fw_root, timeout_s=90)
    if ok:
        return _post_flash(stale_prefix + "flashed and verified OK (bootloader + partition table + app), board reset and running")

    if retry_once:
        _srv._session_log.warning("flash_firmware: first attempt failed, retrying once (known benign quirk)")
        ok2, output2 = _run_openocd(openocd_exe, board_cfg, tcl, cwd=kiln_fw_root, timeout_s=90)
        if ok2:
            return _post_flash("flashed and verified OK on retry (first attempt hit the known benign Verify-Failed quirk)")
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
# On-chip partition-table confirmation -- FLASH_BUDGET_PLAN.md section 8 item
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


