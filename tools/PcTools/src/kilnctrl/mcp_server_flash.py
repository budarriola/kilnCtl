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
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque
from typing import Any, Callable, Optional

from . import actions, capability_preflight, config_presets, coredump_fetch, debug_probe, devices, elf_archive, esp_app_desc, flash_provenance, host_resolve, mcp_facade, openocd_util, partition_http_client, partition_table, pico_gpio_probe, pico_image_freshness, recovery_flash, safety_cfg_http_client, serial_link, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
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
from . import http_auth
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

# ---------------------------------------------------------------------------
# App-image flash target vs. the board's OWN live partition table.
#
# HISTORY (2026-09-17 fix): this used to write the app image to a HARDCODED
# offset (APP_FLASH_OFFSET = 0x810000, the OLD dual-OTA-slot table's
# `factory` partition), left deliberately unretargeted when the 2026-09-16
# single-application-slot partitions.csv landed (docs/OTA_SINGLE_SLOT_PLAN.md
# step 3: `app`/ota_0 @0x210000/8 MiB, `recovery`/factory @0xA10000/1,966,080
# B). That gap bit for real: the app image (~2,334,000 B) was written at
# 0xa10000 -- `recovery`'s offset, reached via a separate, since-corrected
# hardcode -- overflowing `recovery` by roughly 360 KB into `coredump`. The
# board kept booting its old app image, and flash_firmware()'s own post-flash
# verification correctly failed loud.
#
# Fix: the write target is no longer hardcoded anywhere in this module. It is
# resolved fresh, per flash, from the ACTUAL `partitions.csv` next to the
# binaries being flashed (`<kiln_fw_root>/partitions.csv` -- so a
# `kiln_fw_root` override's own table is what's consulted, not the main
# tree's) via `_resolve_app_flash_target()` below, which looks up the
# partition named `APP_PARTITION_NAME` ("app") -- the OTA ota_0 slot the main
# application image belongs in, per the single-slot design. `recovery` is a
# SEPARATE, much smaller partition for a distinct recovery-image IDF project
# (`firmware/KilnFW_recovery/`, see `tools/check_recovery_image_size.py`) and
# is never a valid target for `KilnCtrl.bin`.
#
# _check_app_flash_offset_matches_chip() below independently re-confirms this
# resolved target against the board's own live table (over GET /api/
# partitions -- no JTAG, no reset) and refuses before ever calling OpenOCD if
# the two disagree, same refuse-rather-than-guess shape as before.
APP_PARTITION_NAME = "app"


def _resolve_app_flash_target(kiln_fw_root: str) -> partition_table.PartitionEntry:
    """Reads `<kiln_fw_root>/partitions.csv` and returns the
    :class:`partition_table.PartitionEntry` the main application image
    (`KilnCtrl.bin`) must be flashed into -- the partition named
    `APP_PARTITION_NAME` ("app", the ota_0 slot introduced by the
    2026-09-16 single-slot OTA redesign, docs/OTA_SINGLE_SLOT_PLAN.md).

    Reading the CSV that ships alongside the binaries being flashed (rather
    than a fixed path, or the main tree's own copy) matters for the
    `kiln_fw_root` override workflow: that worktree's own partitions.csv is
    the one describing what its own build actually targets.

    Raises `ValueError` if the CSV cannot be parsed, or has no partition
    named `APP_PARTITION_NAME` -- this is a refusal, not a guess: flashing
    the wrong partition here is exactly the incident this function exists
    to prevent (see this module's header comment above)."""
    csv_path = os.path.join(kiln_fw_root, "partitions.csv")
    try:
        entries = partition_table.parse_partitions_csv(csv_path)
    except (OSError, ValueError) as exc:
        raise ValueError(f"could not parse {csv_path}: {exc}") from exc
    for entry in entries:
        if entry.name == APP_PARTITION_NAME:
            return entry
    names = ", ".join(e.name for e in entries) or "(no partitions)"
    raise ValueError(
        f"{csv_path} has no partition named {APP_PARTITION_NAME!r} -- cannot determine where to "
        f"flash the main application image. Partitions found: {names}"
    )


DEFAULT_PARTITION_TABLE_OFFSET = 0x8000


def _resolve_partition_table_offset(kiln_fw_root: str) -> "tuple[int, str]":
    """Reads `CONFIG_PARTITION_TABLE_OFFSET=0x....` out of sdkconfig and
    returns `(offset, note)`.

    Tries `<kiln_fw_root>/sdkconfig` first, then `<kiln_fw_root>/build/sdkconfig`
    (the copy `build_kilnfw`/`idf.py` publishes as a sibling of the root
    config) ONLY if the first is genuinely absent -- a present-but-unreadable
    root sdkconfig is reported as-is, not silently overridden by the build
    copy. Falls back to `DEFAULT_PARTITION_TABLE_OFFSET` (IDF's own default)
    when neither file exists, when a file exists but has no such key, or
    when a found file cannot be read for some other reason (permissions,
    etc.) -- `note` always says which path was actually used, or why none
    was. If the key IS present but its value cannot be parsed as an integer,
    this raises `ValueError` rather than silently falling back: a
    present-but-garbled value is far more likely to mean the offset actually
    differs from the default than that the file is merely malformed, and
    guessing wrong here writes the partition table image over live flash at
    the wrong address -- the same class of incident
    `_resolve_app_flash_target()` exists to prevent for the app image."""
    candidates = [
        os.path.join(kiln_fw_root, "sdkconfig"),
        os.path.join(kiln_fw_root, "build", "sdkconfig"),
    ]
    tried_missing: "list[str]" = []
    for i, sdkconfig_path in enumerate(candidates):
        is_last = i == len(candidates) - 1
        try:
            f = open(sdkconfig_path, "r", encoding="utf-8", errors="replace")
        except FileNotFoundError:
            tried_missing.append(sdkconfig_path)
            if is_last:
                return (
                    DEFAULT_PARTITION_TABLE_OFFSET,
                    "note: none of " + ", ".join(tried_missing) + " were found -- "
                    f"using IDF's default CONFIG_PARTITION_TABLE_OFFSET "
                    f"(0x{DEFAULT_PARTITION_TABLE_OFFSET:x}).",
                )
            continue  # try the next candidate
        except OSError as exc:
            # Present but unreadable for some other reason (permissions,
            # etc.) -- fall back and say exactly why, rather than silently
            # trying the next candidate (which would mask a real problem
            # with the file the caller actually meant to use).
            return (
                DEFAULT_PARTITION_TABLE_OFFSET,
                f"note: {sdkconfig_path} could not be read ({exc}) -- using IDF's "
                f"default CONFIG_PARTITION_TABLE_OFFSET (0x"
                f"{DEFAULT_PARTITION_TABLE_OFFSET:x}).",
            )
        with f:
            for line in f:
                line = line.strip()
                if not line.startswith("CONFIG_PARTITION_TABLE_OFFSET="):
                    continue
                raw = line.split("=", 1)[1].strip()
                if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in ('"', "'"):
                    raw = raw[1:-1]
                try:
                    offset = int(raw, 0)
                except ValueError as exc:
                    raise ValueError(
                        f"{sdkconfig_path} has CONFIG_PARTITION_TABLE_OFFSET={raw!r}, "
                        f"which could not be parsed as an integer -- refusing to guess "
                        f"the partition-table flash offset rather than risk writing it "
                        f"to the wrong address."
                    ) from exc
                note = f"note: using CONFIG_PARTITION_TABLE_OFFSET from {sdkconfig_path}"
                if offset != DEFAULT_PARTITION_TABLE_OFFSET:
                    note += (
                        f": 0x{offset:x}, overriding IDF's default (0x"
                        f"{DEFAULT_PARTITION_TABLE_OFFSET:x})."
                    )
                else:
                    note += f" (0x{offset:x}, same as IDF's default)."
                return offset, note
        # File exists but has no such key -- fall back without trying the
        # next candidate: an explicit sdkconfig that simply doesn't set this
        # key means "use the default", not "keep looking elsewhere".
        return (
            DEFAULT_PARTITION_TABLE_OFFSET,
            f"note: {sdkconfig_path} has no CONFIG_PARTITION_TABLE_OFFSET key -- using "
            f"IDF's default (0x{DEFAULT_PARTITION_TABLE_OFFSET:x}).",
        )
    # Unreachable (the loop above always returns), but keeps type checkers happy.
    return DEFAULT_PARTITION_TABLE_OFFSET, ""


def _chip_app_target_partition(
    host: str, target_name: str, get_partitions_fn=None
) -> "tuple[Optional[partition_table.PartitionEntry], list[partition_table.PartitionEntry]]":
    """Reads `host`'s live partition table (GET /api/partitions, no JTAG, no
    reset) and returns (the entry named `target_name` if present else None,
    the full entry list). `get_partitions_fn` is the same injectable seam
    `partition_table.read_chip_partition_table_from_http` already exposes,
    threaded through here so tests can drive this against fake board data
    without a real socket or board -- see test_mcp_server_flash_partition_guard.py."""
    entries = partition_table.read_chip_partition_table_from_http(host, get_partitions_fn=get_partitions_fn)
    match = next((e for e in entries if e.name == target_name), None)
    return match, entries


def _check_app_flash_offset_matches_chip(
    host: Optional[str], app_target: partition_table.PartitionEntry, get_partitions_fn=None
) -> Optional[str]:
    """Refuse-rather-than-guess guard for the app-image write offset.

    Returns None when it is safe to proceed: either the board's live
    partition table confirms `app_target` (resolved from partitions.csv by
    `_resolve_app_flash_target()`) is exactly where its own same-named
    partition sits, or the board could not be reached at all (bring-up /
    verify=False case -- there is nothing to disagree with, so this is not a
    mismatch; _preflash_board_address()'s own "never observed up" vs.
    "observed up, now silent" distinction covers the meaningfully different
    bring-up case elsewhere).

    Returns a fully-formed `error: ...` string -- naming both the offset
    this tool is about to write and what the board itself reports, and
    saying which side is stale -- when the board IS reachable and its
    table disagrees. Never auto-corrects or proceeds on a mismatch; the
    caller (flash_firmware()) must refuse unless the caller passed an
    explicit override."""
    if not host:
        return None
    try:
        chip_match, entries = _chip_app_target_partition(host, app_target.name, get_partitions_fn=get_partitions_fn)
    except (partition_http_client.PartitionHttpError, ValueError, KeyError) as exc:
        _srv._session_log.warning(
            "flash_firmware: could not read chip partition table for the partition-offset guard "
            "(host=%s): %s -- proceeding without this confirmation", host, exc,
        )
        return None
    if chip_match is None:
        chip_summary = ", ".join(e.field_str() for e in entries) or "(no partitions reported)"
        return (
            f"error: refusing to flash -- board at {host} reports a live partition table with NO "
            f"partition named {app_target.name!r}, so the app image write offset "
            f"(0x{app_target.offset:x}, resolved from partitions.csv) cannot be confirmed safe. "
            f"Board's live table: {chip_summary}. The BOARD's table is the side that looks stale/"
            "unexpected here -- confirm with debug_check_partition_table() before proceeding. "
            "Pass allow_partition_offset_mismatch=True only after reviewing this by hand."
        )
    if chip_match.offset != app_target.offset:
        return (
            f"error: refusing to flash -- flash_firmware() is about to write the app image at "
            f"0x{app_target.offset:x} (per partitions.csv), but the board at {host} reports its "
            f"own {chip_match.name!r} partition at 0x{chip_match.offset:x} instead. One side is "
            f"stale: either partitions.csv was retargeted ahead of a board that has not actually "
            f"been migrated yet, or this board's own table was migrated (see "
            "docs/OTA_SINGLE_SLOT_PLAN.md) without a matching rebuild/reflash of partitions.csv. "
            "Refusing rather than guessing which side is right -- confirm which side is actually "
            "stale with debug_check_partition_table(), fix that side, and only then pass "
            "allow_partition_offset_mismatch=True if you have concluded the write is intentional "
            "anyway (e.g. mid-migration with a plan in hand)."
        )
    return None


# ---------------------------------------------------------------------------
# Data-partition erase, added 2026-09-21 (owner decision: reset the board's
# web-auth admin record, whose password is unknown, by erasing the DEFAULT
# `nvs` partition during the commission reflash -- see web_auth_store.c:18-31
# and partitions.csv's `nvs,data,nvs,0x9000,0x6000` row). No tool previously
# erased any data partition; a hand `flash erase_sector 0 0 last` once wiped
# the WHOLE chip (see this module's header comment above) -- so this is
# deliberately narrow: an explicit allowlist of DATA partitions only, never
# app/recovery/otadata/bootloader/partition-table/coredump, resolved fresh
# from the same partitions.csv `_resolve_app_flash_target()` already reads
# (so a `kiln_fw_root` override's own table is what is consulted, never a
# hardcoded offset), written in the SAME OpenOCD session as the app image
# (after it, before the final reset) so a partial multi-call sequence can
# never leave the board in a state where the app was reflashed but an
# intended erase silently did not happen (or vice versa).
# ---------------------------------------------------------------------------
ERASABLE_DATA_PARTITIONS = frozenset({"nvs", "kiln_nvs", "wifi_nvs", "profiles_nvs", "cfg"})


def _resolve_erase_targets(kiln_fw_root: str, names: "list[str]") -> "list[partition_table.PartitionEntry]":
    """Resolves each requested partition `names` entry against
    `<kiln_fw_root>/partitions.csv`, refusing (raising ValueError, naming the
    partition) unless it is BOTH in `ERASABLE_DATA_PARTITIONS` and actually
    present in that CSV. Never returns a partition outside the allowlist --
    this is the one gate standing between `erase_partitions` and the
    2026-08-11 full-chip-erase incident's much narrower, much safer cousin,
    so it fails loud rather than silently skipping an unknown/disallowed
    name."""
    csv_path = os.path.join(kiln_fw_root, "partitions.csv")
    try:
        entries = partition_table.parse_partitions_csv(csv_path)
    except (OSError, ValueError) as exc:
        raise ValueError(f"could not parse {csv_path}: {exc}") from exc
    by_name = {e.name: e for e in entries}
    resolved: "list[partition_table.PartitionEntry]" = []
    seen: "set[str]" = set()
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        if name not in ERASABLE_DATA_PARTITIONS:
            allowed = ", ".join(sorted(ERASABLE_DATA_PARTITIONS))
            raise ValueError(
                f"refusing to erase partition {name!r} -- not in the allowlist of erasable data "
                f"partitions ({allowed}). app/recovery/otadata/bootloader/partition-table/coredump "
                "are never erasable through this parameter."
            )
        entry = by_name.get(name)
        if entry is None:
            names_found = ", ".join(sorted(by_name)) or "(no partitions)"
            raise ValueError(
                f"{csv_path} has no partition named {name!r} -- cannot determine its offset/size "
                f"to erase. Partitions found: {names_found}"
            )
        resolved.append(entry)
    return resolved


def _write_blank_partition_file(entry: partition_table.PartitionEntry, tmp_dir: str) -> str:
    """Writes a 0xFF-filled file of exactly `entry.size` bytes into `tmp_dir`
    and returns its path -- 0xFF is erased-flash's read-back value, so
    `program_esp ... verify` writing this file over `entry`'s region has the
    same effect as erasing it (blank NVS/LittleFS), through the same
    byte-compare-on-write path every other image in this tool already uses,
    rather than a bare `flash erase_sector` call."""
    path = os.path.join(tmp_dir, f"erase_{entry.name}.bin")
    chunk = b"\xff" * (1024 * 1024)
    remaining = entry.size
    with open(path, "wb") as f:
        while remaining > 0:
            n = min(remaining, len(chunk))
            f.write(chunk[:n])
            remaining -= n
    return path


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
      3. `partition_http_client.PARTITION_AP_DEFAULT_HOST` -- whatever
         host_resolve.resolve_default_host() currently resolves to (a
         cached last-seen host, if any -- otherwise the AP fallback).
      4. `host_resolve.FALLBACK_HOST`, unconditionally -- the board's own
         AP address, ALWAYS tried regardless of what step 3 resolved to.
         Needed because step 3 is no longer guaranteed to be the AP
         address: once a last-seen host is cached, it dedupes against
         candidate 2 (both being the same LAN address) and the true AP
         fallback would never be tried at all, breaking genuine bring-up
         verification on a from-scratch board.

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
    _add(host_resolve.FALLBACK_HOST)
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
    misdirected verifier read as benign noise.

    A board that answers with the RECOVERY image's shape
    (partition_http_client.RecoveryImageResponse) still counts as
    "observed up" here: it answered GET /api/partitions, just not with the
    main app's table. Treating that as unreachable and falling through to
    the next candidate (or to None) would make a recovery-mode board that
    was up before AND after a flash read as "never observed up", which
    downgrades _verify_flash_landed()'s eventual failure from a hard error
    to its soft bring-up WARNING -- exactly the misdiagnosis this whole
    pre-flash probe exists to prevent."""
    for candidate in _resolve_verify_hosts(host):
        try:
            partition_http_client.get_partitions(candidate, timeout=_VERIFY_HTTP_TIMEOUT_S)
            return candidate
        except partition_http_client.RecoveryImageResponse:
            return candidate
        except partition_http_client.PartitionHttpError:
            continue
    return None


def _verify_flash_landed(
    host: Optional[str], bin_path: str, pre_flash_host: Optional[str] = None,
    app_partition_name: str = APP_PARTITION_NAME,
) -> str:
    """Post-flash confirmation that the binary just written to
    `app_partition_name` is the one actually RUNNING -- added after the
    recurring "flash reports OK but the board keeps running old code" failure
    mode (see this module's header comment and esp_app_desc.py's docstring
    for the full history). flash_firmware() writes the resolved
    `app_partition_name` partition (see `_resolve_app_flash_target()`) and
    does NOT write `otadata` (see flash_firmware()'s own docstring for why --
    a deliberate, explained gap, not an oversight): a blank/erased `otadata`
    makes the bootloader fall back to whatever partition holds the
    factory-subtype slot (`recovery` on the current table), NOT the `app`
    slot this tool just wrote, so a from-scratch flash can boot the wrong
    image even though the write itself landed correctly. OpenOCD's own
    "verified OK" during the write says nothing about which partition
    actually boots.

    Two independent checks, both against the RUNNING firmware, not the
    write itself:
      1. GET /api/partitions' "running" field must equal `app_partition_name`
         -- anything else means the bootloader is not even attempting to run
         what was just flashed.
      2. GET /api/status's fw_build must match the build timestamp parsed
         out of the .bin's esp_app_desc_t -- catches a stale/mismatched
         image passing check 1 (e.g. a previous flash that never got
         overwritten because a caller thought a build was newer than it
         was).

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
    recovery_exc: Optional[partition_http_client.RecoveryImageResponse] = None
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
            except partition_http_client.RecoveryImageResponse as exc:
                # The board answered -- just with the recovery image's
                # shape, not the main app's. That is itself the failure
                # this whole function exists to report (the board did not
                # boot the app image just flashed), so stop polling and
                # raise immediately below rather than retrying attempts
                # that will keep coming back the same way.
                recovery_exc = exc
                resolved_host = candidate
                break
            except partition_http_client.PartitionHttpError as exc:
                last_exc = exc
                continue
        if partitions_data is not None or recovery_exc is not None:
            break

    if recovery_exc is not None:
        # docs/audits/web_code_duplication_drift_2026-09-18.md section 2.3:
        # the recovery image answers GET /api/partitions with its own
        # smaller shape (running/running_offset/next_update, no partitions
        # array). partition_http_client.get_partitions() raises
        # RecoveryImageResponse for it rather than returning a normalized
        # dict or a generic "malformed response" error -- it is still the
        # same underlying fact this whole check exists to report: the
        # board did not boot the app image just flashed.
        raise RuntimeError(
            f"flash reported OK, but the board came up running the RECOVERY "
            f"image (partition {recovery_exc.running!r}, "
            f"next_update={recovery_exc.next_update!r}), not {app_partition_name!r}. "
            f"flash_firmware() wrote the app image to the {app_partition_name!r} "
            "partition, but the bootloader fell back to recovery -- see "
            "CLAUDE.md's boot_guard recovery-mode notes and "
            "docs/OTA_SINGLE_SLOT_PLAN.md for the blank/unset `otadata` gap "
            "this can indicate. ota_rollback_esp() does NOT fix this (it reverts "
            "between OTA images over the app's own HTTP API, which recovery does "
            "not run)."
        )

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
    if running != app_partition_name:
        # A recovery-shaped response never reaches this point -- it is
        # caught above (RecoveryImageResponse) and raised immediately with
        # its own message, before partitions_data is ever set. Anything
        # here is a main-app-shaped response reporting a genuinely
        # different running partition (e.g. a stale OTA pointer).
        raise RuntimeError(
            f"flash reported OK, but the board is running partition {running!r}, "
            f"not {app_partition_name!r} -- flash_firmware() wrote the app image to "
            f"the {app_partition_name!r} partition, but the bootloader booted "
            f"{running!r} instead. flash_firmware() intentionally does NOT write "
            "`otadata` (see its own docstring): generating a correct otadata blob "
            "by hand could not be verified against real hardware as part of this "
            "fix, and a wrong one risks a worse, silently-bricked boot than "
            "refusing loudly here. A blank/erased `otadata` makes the bootloader "
            f"fall back to the factory-subtype partition ({running!r} here), not "
            f"{app_partition_name!r} -- this is a KNOWN GAP (docs/"
            "OTA_SINGLE_SLOT_PLAN.md), not the stale-OTA-pointer case this "
            "message used to describe. ota_rollback_esp() does NOT fix this: it "
            "reverts a board that is ALREADY booting one OTA image back to a "
            "PREVIOUS one over its own HTTP API, and has no path to set an "
            "unset/blank otadata after a bare JTAG flash. Setting otadata "
            "correctly needs a hardware-verified follow-up; until then, a "
            "from-scratch flash on this table may not boot the image just "
            "written."
        )

    board = capability_preflight.get_board_info(resolved_host, timeout=_VERIFY_HTTP_TIMEOUT_S)
    if not board.reachable:
        return (
            f"WARNING: running partition confirmed {app_partition_name!r}, but "
            f"GET /api/status failed ({board.error}) so the build timestamp "
            "could not be checked."
        )
    if board.fw_build is None:
        # Distinct from an actual mismatch below: GET /api/status did not
        # report a usable fw_build, but that observation has more than one
        # possible cause -- capability_preflight.get_board_info() reads
        # `data.get("fw_build") or None`, which collapses a MISSING key, an
        # EMPTY string, and a genuinely REDACTED null (dashboard_status_http.c's
        # may_see_build_identity gate, when web auth is ON and this caller
        # cannot authenticate as admin) into the same value. It can equally
        # mean this board's firmware predates the fw_build field entirely
        # (e.g. a bench board many commits behind). Whichever it is, this is
        # a "cannot check" outcome, not evidence of a stale binary --
        # reporting it as a build mismatch (as this function used to, when
        # the pre-fix firmware gate hid fw_build from EVERYONE including a
        # default, auth-off board) points the next reader at exactly the
        # wrong thing, and asserting one specific cause as fact would repeat
        # that same mistake one layer out.
        return (
            f"WARNING: running partition confirmed {app_partition_name!r}, but "
            "GET /api/status did not report fw_build (absent or null) so the "
            "build timestamp could not be checked -- either web auth is "
            "enabled and this tool holds no admin session (build identity "
            "redacted), or this firmware predates the fw_build field "
            "entirely. This does NOT indicate the wrong build landed. "
            "Confirm build identity another way (e.g. an authenticated "
            "admin session, or GET /api/ota/esp/status) if certainty is "
            "needed."
        )
    if not esp_app_desc.build_timestamps_match(app_desc, board.fw_build):
        raise RuntimeError(
            f"flash reported OK and the board is running {app_partition_name!r}, "
            f"but its reported build ({board.fw_build!r}) does not match the "
            f"binary just flashed ({app_desc.build_timestamp!r}) -- the board is "
            f"running a DIFFERENT build than the one on disk. This can happen if "
            f"{app_partition_name!r} was flashed once, then a stale/leftover .bin "
            "got flashed again without a fresh build_kilnfw, or if verification "
            "is racing a boot that hasn't finished yet. Rebuild with "
            "build_kilnfw and reflash."
        )
    return ""


# Distinct from both None and any real int, so _fmt_persisted() below can
# tell apart "the GET itself raised" from "the GET succeeded but the field
# was absent" -- conflating them under one None used to print "unknown
# (older firmware)" even when the failure was a network/auth error that told
# us nothing about the firmware at all.
_BOOT_GUARD_GET_FAILED = object()


def _fmt_persisted(value: object) -> str:
    """Renders a persisted_count value (or its absence) for the
    boot_guard_reset report. Three distinct cases, per review -- must not be
    collapsed back into one:
      - _BOOT_GUARD_GET_FAILED: the GET /api/boot_guard call itself raised
        (network/auth failure) -- we have no idea what the field would say.
      - None: the GET succeeded but the response had no "persisted_count"
        key -- either older firmware that predates the field, or current
        firmware whose own strict read-back failed and correctly omitted the
        field rather than fabricate a value
        (boot_guard_get_persisted_count()).
      - anything else: a real reported value."""
    if value is _BOOT_GUARD_GET_FAILED:
        return "unknown (GET failed)"
    if value is None:
        return "not reported (older firmware or read failure)"
    return str(value)


def _maybe_reset_boot_guard(host: Optional[str], pre_flash_host: Optional[str],
                             reset_boot_guard: bool = True) -> str:
    """Called from flash_firmware()'s _post_flash() ONLY after
    _verify_flash_landed() returned "" -- i.e. full, unambiguous, verified
    success (running partition is 'factory' AND its build timestamp matches
    the .bin just flashed). Never called on a raise, a WARNING, or
    verify=False -- see boot_guard_reset_counter()'s header comment
    (firmware/KilnFW/App/drivers/persist/boot_guard.h) for why: a board just
    flashed with something broken, or whose landing was never actually
    confirmed, must still be free to walk into recovery mode on its own.

    `POST /api/ota/esp/boot_guard_reset` is ROUTE_TIER_ADMIN like the other 8
    routes named in WEB_AUTH_PLAN.md item 2b -- the AP-password HMAC
    challenge/response scheme it used to ALSO require was retired 2026-09-29
    (owner decision "Retire; open when login off"). This call therefore now
    goes through the same admin session (`http_auth.urlopen()`) every other
    admin route in this module already uses: it is default-on whenever an
    admin session is available (or web auth is off entirely), needs no
    separate credential, and there is no longer an "AP password erased by
    this same flash" skip case -- that only ever applied to the retired
    HMAC's signing key. Pass `reset_boot_guard=False` to opt out
    unconditionally.

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
    board into recovery mode; this is exactly the "eventually" case).

    Reports the counter's before value (a best-effort GET /api/boot_guard
    probe taken just before the reset call, purely informational -- its
    failure never blocks or fails the reset itself, and is reported as
    "unknown" rather than silently dropped) alongside the reset call's own
    verified-or-not after value -- RELEASE_HARDENING_PLAN.md blocker 6 calls
    for both, since a silent clear is not acceptable and a failed clear must
    be visible with enough context to judge it."""
    if not reset_boot_guard:
        return ""
    resolved = _preflash_board_address(host) or pre_flash_host
    if not resolved:
        return ("WARNING: boot_guard_reset skipped -- could not resolve a board address to call "
                "POST /api/ota/esp/boot_guard_reset at, even though post-flash verification just "
                "succeeded against one; the recovery-mode counter was NOT cleared by this flash.")
    # Best-effort "before" read via the admin-authenticated GET /api/boot_guard
    # diagnostics route (goes through http_auth.urlopen()'s ADMIN-session
    # seam; 401s once web auth is on), purely for the reported message -- never gates or
    # fails the reset call below. A board that answers this GET but then
    # fails the POST (a transient network blip between the two calls) still
    # gets a useful before/after report instead of losing the before value
    # entirely.
    # persisted_count (added alongside boot_guard_get_persisted_count(),
    # boot_guard.c) is what actually moves on a clear -- boot_count is this
    # boot's fixed in-RAM count (e.g. 1) and never changes across this call,
    # which used to make a genuinely successful reset read like a no-op.
    #
    # Two distinct "we don't have a number" cases must not be labeled the
    # same, per review: a GET that raised (network/auth failure -- we truly
    # know nothing) versus a GET that succeeded but returned no
    # "persisted_count" key (older firmware that predates the field, OR
    # current firmware whose own read-back failed and correctly omitted the
    # field rather than fabricate one -- see boot_guard_get_persisted_count()'s
    # strict reader). _BOOT_GUARD_GET_FAILED is a sentinel distinct from both
    # None and any real int, so _fmt_persisted() can tell all three apart.
    before_persisted: object = None
    try:
        before_data = ota_http.get_boot_guard_status(resolved)
        before_persisted = before_data.get("persisted_count")
    except (ota_http.OtaHttpError, http_auth.HttpAuthError) as exc:
        before_persisted = _BOOT_GUARD_GET_FAILED
        _srv._session_log.warning("flash_firmware: pre-reset GET /api/boot_guard failed (informational "
                                   "only, does not block the reset call): %s", exc)

    before_str = _fmt_persisted(before_persisted)
    try:
        body = ota_http.boot_guard_reset_esp(resolved)
    except http_auth.HttpAuthError as exc:
        # No admin session could be established (web auth is on and
        # KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD are unset, or the login
        # was refused) -- this is not a route failure, just "cannot try".
        # The flash itself already landed and verified by this point, so
        # this is reported as a skip, never as a lost flash result.
        _srv._session_log.warning("flash_firmware: boot_guard_reset skipped, no admin session: %s", exc)
        return "boot_guard reset skipped: no admin session"
    except ota_http.OtaHttpError as exc:
        _srv._session_log.warning("flash_firmware: boot_guard_reset call failed: %s", exc)
        return (f"WARNING: boot_guard_reset call failed ({exc}) -- the flash itself landed fine, "
                f"but the recovery-mode counter was NOT cleared by this flash (persisted counter "
                f"before this attempt: {before_str}).")
    after_str = _fmt_persisted(body.get("persisted_count"))
    if body.get("ok"):
        return (f"boot_guard_reset: persisted counter cleared and verified "
                f"(persisted before={before_str}, after={after_str}; this boot's own "
                f"boot_count={body.get('boot_count')}, unchanged by this call)")
    return (f"WARNING: boot_guard_reset did not verify (board reported {body!r}) -- the flash "
            "itself landed fine, but the recovery-mode counter was NOT confirmed cleared "
            f"(persisted before={before_str}, reported after={after_str}; this boot's own "
            f"boot_count={body.get('boot_count')}); a run of "
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
        # 2026-09-15: this used to log-only and return "" -- a failed archive
        # call was then indistinguishable, in the tool's own returned result,
        # from one that succeeded silently. The 2026-09-14 23:55:17Z flash's
        # ELF went missing from the archive by the next day for an unrelated
        # reason (build/ itself got wiped, see kiln_archive_dir()'s docstring),
        # but an archiving failure at flash time would have looked identical
        # to a clean success in the result text either way. Surface it in the
        # returned message so a failed archive is never mistaken for one that
        # worked.
        _srv._session_log.warning("flash_firmware: elf archiving failed (non-fatal): %s", exc)
        return f"\nWARNING: elf archiving FAILED (flash itself succeeded): {exc}"


def _pico_image_provenance_note(app_bin_path: str) -> str:
    """One-line note recording what the ESP application binary about to be
    flashed believes about its embedded Pico (SaftyFW) image(s), per the
    2026-09-20 owner decision that the ESP embeds both SaftyFW slot images
    and auto-updates the Pico at boot (docs/PICO_AUTO_UPDATE_PLAN.md).

    Read-only and best-effort: this NEVER changes flash behavior or blocks a
    flash -- it only appends a line to the provenance report so a flash's
    Pico expectation is on the record. Uses the same scanning parser as
    check_embedded_pico_image_fresh.ps1
    (tools/PcTools/src/kilnctrl/pico_image_freshness.py) rather than
    duplicating the struct layout here.
    """
    try:
        if not os.path.isfile(app_bin_path):
            return f"pico image: no embedded SaftyFW identity (app binary not found: {app_bin_path})"
        with open(app_bin_path, "rb") as f:
            data = f.read()
        records = pico_image_freshness.find_all_identities(data)
    except Exception as exc:  # noqa: BLE001 - provenance note must never block a flash
        return f"pico image: could not inspect embedded SaftyFW identity ({exc})"
    if not records:
        return "pico image: no embedded SaftyFW identity record found in app binary (KilnFW built without embedded Pico images, or embedding not yet wired up)"
    distinct = sorted({(r.commit, r.dirty, r.config_format_version, r.link_protocol_version) for r in records})
    if len(distinct) == 1:
        commit, dirty, cfg_ver, link_proto_ver = distinct[0]
        dirty_note = " (DIRTY build)" if dirty else ""
        return (f"pico image: embedded SaftyFW identity commit={commit}{dirty_note}, "
                f"config_format_version={cfg_ver}, link_protocol_version={link_proto_ver}")
    return f"pico image: embedded SaftyFW identities disagree across records: {distinct}"


@_srv._tool()
def flash_firmware(
    board_cfg: str = "board/esp32s3-builtin.cfg",
    retry_once: bool = True,
    allow_stale: bool = False,
    verify: bool = True,
    host: Optional[str] = None,
    allow_sensitive_dirty: bool = False,
    kiln_fw_root: Optional[str] = None,
    reset_boot_guard: bool = True,
    allow_partition_offset_mismatch: bool = False,
    erase_partitions: Optional["list[str]"] = None,
    confirm_erase: bool = False,
) -> str:
    """Flashes KilnFW/build/{bootloader,partition_table,KilnCtrl}.bin to the
    board over JTAG via OpenOCD -- the ONLY sanctioned way to flash this
    board (never esptool/`idf.py flash`, per CLAUDE.md). Always writes all
    three images (bootloader @0x0, partition table at the offset resolved
    fresh per flash from sdkconfig's `CONFIG_PARTITION_TABLE_OFFSET`
    -- default 0x8000 when that key/file is absent, see
    `_resolve_partition_table_offset()` -- and the app image
    at the offset/size of the `APP_PARTITION_NAME` ("app") partition, resolved
    fresh per flash from `<kiln_fw_root>/partitions.csv` -- see
    `_resolve_app_flash_target()`), each with `program_esp ... verify` (which
    only erases/rewrites a region if its content doesn't already match -- it
    does NOT do a bare full-chip erase), ending in a reset so the board boots
    the new app immediately. A pre-flight size check refuses, naming both
    byte counts, if `KilnCtrl.bin` is larger than that partition.

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
    `APP_PARTITION_NAME` ("app") partition resolved from partitions.csv (see
    above). If an earlier OTA left the boot target pointed elsewhere,
    every future flash_firmware() would otherwise report success
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
    KilnFW/flash_provenance.json (a sibling of KilnFW/elf_archive/ and
    KilnFW/build/, since 2026-09-15 -- see elf_archive.kiln_provenance_path())
    so "what was actually on the board at <time>" is answerable later from
    disk, even after a `build/` wipe.

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
    `flash_provenance.json` (always the MAIN tree's, per
    elf_archive.kiln_provenance_path() -- never the override worktree's,
    since that worktree and its build/ can be deleted right after flashing)
    records `kiln_fw_root_override` so a later reader knows this
    flash did not come from the ordinary path. Post-flash verification
    (`verify=True`) compares against THAT tree's `.bin`, unchanged
    otherwise.

    `reset_boot_guard`: Owner decision 2026-09-19 -- the post-flash
    boot_guard counter reset is DEFAULT ON, not opt-in per call. When
    `verify=True` and `reset_boot_guard=True` (the default), and ONLY once
    post-flash verification confirms full success (the board is running
    `factory` with the exact build just flashed -- `_verify_flash_landed()`
    returned "", not a WARNING and not a raise), this calls
    `POST /api/ota/esp/boot_guard_reset` (ota_http_client.boot_guard_reset_esp())
    to clear `boot_guard`'s recovery-mode counter directly -- the tool-driven
    fix for docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md:
    boot_confirm_is_healthy()'s own auto-clear depends on a one-shot NVS
    snapshot that can be wrong for a boot or two right after a flash-induced
    reset, and a run of ordinary development reflashes can otherwise walk a
    perfectly healthy board into recovery mode for a reason that has nothing
    to do with whether the firmware can boot.

    `POST /api/ota/esp/boot_guard_reset` is ROUTE_TIER_ADMIN, like the other
    8 routes named in WEB_AUTH_PLAN.md item 2b -- the AP-password HMAC
    challenge/response scheme it used to ALSO require was retired 2026-09-29
    (owner decision "Retire; open when login off"). This call needs no
    separate credential any more: it goes through the same admin session
    every other admin route this module calls already uses, and runs
    whenever that session is available (or web auth is off entirely).
    Passing `reset_boot_guard=False` opts out unconditionally.

    Deliberately gated on FULL verified success, never on a failed,
    unverified, or merely-warned-about flash (`verify=False`, an unreachable
    board, a wrong-partition/wrong-build mismatch, or a soft bring-up
    WARNING) -- a board that was just flashed with something broken, or
    whose landing was never actually confirmed, must still be free to walk
    into recovery mode on its own; see boot_guard_reset_counter()'s own
    header comment on why this must never run except from a tool that KNOWS
    a deliberate, confirmed-good flash just happened. A failure to reach or
    verify this call is reported as a WARNING appended to the result, never
    raised -- the flash itself already succeeded and landed by this point.
    The result always reports the counter's before and after values (or the
    skip reason), never a silent clear.

    `allow_partition_offset_mismatch`: before touching OpenOCD, this tool
    parses `<kiln_fw_root>/partitions.csv` to find the partition named
    `APP_PARTITION_NAME` ("app") and resolves the write offset/size from
    THAT, not a hardcoded constant (see `_resolve_app_flash_target()` --
    this used to be a hardcoded APP_FLASH_OFFSET, which is exactly what let
    a table redesign silently retarget the write into the wrong, undersized
    partition; see docs/OTA_SINGLE_SLOT_PLAN.md). A hard size pre-flight
    check separately refuses, naming both byte counts, if `KilnCtrl.bin` is
    larger than that partition. Additionally, if the board is reachable
    right now, this tool reads the board's OWN live partition table
    (GET /api/partitions -- no JTAG, no reset) and refuses if the resolved
    partition's offset disagrees with what the board itself reports at that
    name. Never auto-corrects the address; a mismatch is refused, naming
    both offsets and which side looks stale, unless this is passed True
    after reviewing the mismatch by hand (e.g. debug_check_partition_table()).
    A board that cannot be reached at all is not a mismatch (nothing to
    compare against) and this check is a no-op in that case, same as
    verify=False's own bring-up allowance.

    Deliberate gap: this tool does NOT write `otadata`. On this table, a
    blank/erased `otadata` makes the bootloader boot the factory-subtype
    partition (`recovery`), not the `app` partition just written -- see
    `_verify_flash_landed()`'s RuntimeError message for the full explanation
    and why `ota_rollback_esp()` cannot substitute for this. A board whose
    otadata already points at `app` (e.g. from a previous correctly-booted
    OTA) is unaffected; a from-scratch/never-OTA'd board may need that
    resolved before this tool's own post-flash verification will pass.

    `erase_partitions` / `confirm_erase`: owner decision 2026-09-21 -- erase
    one or more DATA partitions in the SAME OpenOCD session as the app image
    (written after it, before the final reset), by writing a 0xFF-filled
    (erased-flash-equivalent) file over that partition's exact offset/size
    with the same `program_esp ... verify` byte-compare every other image in
    this tool already uses. Each name is resolved fresh from
    `<kiln_fw_root>/partitions.csv` (the same file `_resolve_app_flash_target()`
    reads) and refused unless it is in `ERASABLE_DATA_PARTITIONS` -- `nvs`,
    `kiln_nvs`, `wifi_nvs`, `profiles_nvs`, `cfg` -- never `app`, `recovery`,
    `otadata`, `bootloader`, the partition table, or `coredump`. Requesting a
    name outside that allowlist, or one absent from the CSV, refuses BEFORE
    touching OpenOCD, naming the offending name. Requires
    `confirm_erase=True` passed alongside `erase_partitions` -- omitting it
    refuses, naming every requested partition, so an erase can never happen
    as a side effect of a call that only meant to flash firmware.

    Erasing `nvs` (`0x9000`, 0x6000 B) destroys: the web-auth admin record
    for BOTH roles (`web_auth_store.c`'s `kiln_auth` namespace -- so both the
    LCD PIN and the web admin password revert to unset/first-run), the auth
    policy (auth reverts to OFF), and any pre-2026-08-13 legacy remnants
    still stored there. It does NOT touch Wi-Fi credentials (`wifi_nvs`),
    zones/profiles config (`kiln_nvs`/`profiles_nvs`), boot_guard or
    crash_report state (also `kiln_nvs`), or the `cfg` LittleFS partition's
    own data unless `cfg` is separately named.

    The result names each erased partition (name, offset, size) and the
    write's verify outcome, and this is persisted to `flash_provenance.json`
    (`erased_partitions`) alongside the rest of that flash's record -- an
    erase is never silent."""
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
        stale_publish_names = ("bootloader.bin", "partition_table" + os.sep + "partition-table.bin")
        stale_publish_hits = [p for p in missing if p.endswith(stale_publish_names)]
        app_bin_missing = any(p.endswith("KilnCtrl.bin") for p in missing)
        stale_publish_note = ""
        if stale_publish_hits and not app_bin_missing:
            # KilnCtrl.bin present means the tree WAS built; only then can a
            # missing bootloader.bin/partition-table.bin be explained by the
            # pre-2026-09-23 publish step. If KilnCtrl.bin is also missing the
            # tree was never built at all, and this note would have no
            # antecedent ("it") to refer to -- suppress it in that case.
            stale_publish_note = (
                " A build published by check_00_kilnfw_target_build.ps1 before "
                "2026-09-23 did not include bootloader.bin or partition-table.bin."
            )
        return (
            f"error: missing build output(s){override_note}, run `idf.py build` first: "
            + ", ".join(missing) + stale_publish_note
        )

    # Resolve the write target from the actual partitions.csv for this tree
    # (docs/OTA_SINGLE_SLOT_PLAN.md) rather than a hardcoded offset -- this is
    # exactly the class of bug that overflowed the `recovery` partition when
    # the table was redesigned out from under a hardcoded offset.
    try:
        app_target = _resolve_app_flash_target(effective_kiln_fw_root)
    except ValueError as exc:
        return f"error: {exc}"

    try:
        partition_table_offset, partition_table_offset_note = _resolve_partition_table_offset(
            effective_kiln_fw_root
        )
    except ValueError as exc:
        return f"error: {exc}"

    app_bin_path = os.path.join(build_dir, "KilnCtrl.bin")
    app_bin_size = os.path.getsize(app_bin_path)
    if app_bin_size > app_target.size:
        return (
            f"error: refusing to flash -- build/KilnCtrl.bin is {app_bin_size} bytes, "
            f"which does not fit in the {app_target.name!r} partition "
            f"({app_target.size} bytes, at 0x{app_target.offset:x} per "
            f"{effective_kiln_fw_root}/partitions.csv). Writing it anyway would "
            f"overflow into whatever partition follows and corrupt it."
        )

    # erase_partitions validation happens BEFORE the adapter/OpenOCD refusal
    # below on purpose: a bad or unconfirmed erase request should never even
    # get as far as checking whether the board is plugged in.
    erase_targets: "list[partition_table.PartitionEntry]" = []
    if erase_partitions:
        if not confirm_erase:
            return (
                "error: refusing to erase partition(s) " + ", ".join(erase_partitions) +
                " without confirm_erase=True -- pass confirm_erase=True alongside "
                "erase_partitions once you have reviewed exactly what this will destroy "
                "(see flash_firmware()'s erase_partitions docstring)."
            )
        try:
            erase_targets = _resolve_erase_targets(effective_kiln_fw_root, erase_partitions)
        except ValueError as exc:
            return f"error: {exc}"

    adapter_refusal = _refuse_if_adapter_absent(MAIN_BOARD_JTAG_SERIAL, "main board (ESP32-S3)")
    if adapter_refusal:
        return adapter_refusal

    # M1 (2026-09-15 review): flash_provenance.json used to live at
    # <build_dir>/flash_provenance.json -- inside build/, exactly like the
    # ELF archive was before the a347e726 move, and just as vulnerable to
    # being silently wiped by an `idf.py fullclean`/reconfigure of build/
    # (see kiln_archive_dir()'s docstring for the incident this class of bug
    # caused for the ELF archive itself). It also lived under the OVERRIDE
    # tree's own build/ when kiln_fw_root was used, so it was deleted along
    # with that worktree -- see elf_archive.kiln_provenance_path()'s
    # docstring. Always the MAIN tree's location now, a sibling of
    # kiln_archive_dir(), regardless of kiln_fw_root.
    provenance_path = elf_archive.kiln_provenance_path()
    elf_archive._guard_against_test_write(provenance_path)
    # L3 (2026-09-15 fixes review): pick up a file left behind at the OLD
    # (pre-2026-09-15) build/flash_provenance.json path -- see
    # elf_archive.migrate_legacy_provenance()'s docstring. Best-effort: a
    # write below always follows regardless of whether this moved anything.
    try:
        elf_archive.migrate_legacy_provenance()
    except Exception:  # noqa: BLE001 - migration must never block a flash
        pass
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
    provenance_note += "\n" + _pico_image_provenance_note(app_bin_path)
    if partition_table_offset_note:
        provenance_note += "\n" + partition_table_offset_note
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
    # Probed unconditionally (not just when verify=True) because the
    # partition-offset guard just below needs it too: refusing an unsafe
    # write is not something verify=False should be able to skip just
    # because post-flash confirmation was also turned off for this call.
    partition_guard_host = _preflash_board_address(host)
    pre_flash_host = partition_guard_host if verify else None
    if verify:
        _srv._session_log.info("flash_firmware: pre-flash board HTTP address: %s", pre_flash_host or "(not answering)")

    # Refuse-rather-than-guess: if the board is reachable right now, its own
    # live partition table must agree with the offset this tool is about to
    # write the app image to (app_target, resolved from partitions.csv above).
    # A board that cannot be
    # reached at all (bring-up, or verify=False for a board whose HTTP stack
    # genuinely isn't up yet) has nothing to disagree with, so this check is
    # a no-op in that case -- see _check_app_flash_offset_matches_chip()'s
    # own docstring. This never auto-corrects the write address; it only
    # ever refuses or proceeds unchanged.
    if not allow_partition_offset_mismatch:
        partition_mismatch = _check_app_flash_offset_matches_chip(partition_guard_host, app_target)
        if partition_mismatch:
            _srv._session_log.warning("flash_firmware: %s", partition_mismatch)
            flash_provenance.write_provenance_json(
                tree_state, provenance_path,
                outcome=flash_provenance.OUTCOME_REFUSED_PARTITION_MISMATCH,
                detail=partition_mismatch,
                kiln_fw_root_override=kiln_fw_root,
            )
            return partition_mismatch

    kill_openocd_sessions()  # a stale session holding the JTAG interface looks identical to a flash failure

    # Data-partition erase (2026-09-21): written in the SAME OpenOCD session,
    # after the app image and before the final reset, so it can never be
    # left half-applied relative to the app flash. Each blank (0xFF-filled)
    # file lives in a temp dir for the duration of this OpenOCD session only,
    # cleaned up in the `finally` below regardless of outcome.
    erase_tmp_dir = tempfile.mkdtemp(prefix="kilnctl_erase_") if erase_targets else None
    erase_note_entries: "list[dict]" = []
    tcl_parts = [
        f"adapter serial {MAIN_BOARD_JTAG_SERIAL}",
        "program_esp build/bootloader/bootloader.bin 0x0 verify",
        # Resolved from <kiln_fw_root>/sdkconfig's CONFIG_PARTITION_TABLE_OFFSET
        # (falling back to IDF's default, 0x8000, only when that key is absent)
        # -- see _resolve_partition_table_offset(). Previously hardcoded to
        # 0x8000 regardless of sdkconfig, unlike app_target.offset above (which
        # IS read from partitions.csv); this closes that gap the same way.
        f"program_esp build/partition_table/partition-table.bin 0x{partition_table_offset:x} verify",
        f"program_esp build/KilnCtrl.bin 0x{app_target.offset:x} verify",
    ]
    for entry in erase_targets:
        blank_path = _write_blank_partition_file(entry, erase_tmp_dir)
        tcl_parts.append(f'program_esp "{blank_path.replace(chr(92), "/")}" 0x{entry.offset:x} verify')
        erase_note_entries.append({"name": entry.name, "offset": entry.offset, "size": entry.size})
    tcl_parts[-1] += " reset exit"
    tcl = "; ".join(tcl_parts)

    stale_prefix = f"WARNING: flashed a stale binary anyway ({stale.reason})\n" if (stale.stale and allow_stale) else ""

    def _erase_note(verify_outcome: str) -> str:
        if not erase_note_entries:
            return ""
        erase_lines = "\n".join(
            f"  - {e['name']}: offset=0x{e['offset']:x} size=0x{e['size']:x} ({e['size']} B)"
            for e in erase_note_entries
        )
        return (
            f"erased partitions (written 0xFF, same OpenOCD session as the app image, "
            f"verify {verify_outcome}):\n{erase_lines}\n"
        )

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
            landed_note = _verify_flash_landed(
                host, app_bin_path, pre_flash_host, app_partition_name=app_target.name,
            )
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
        boot_guard_note = _maybe_reset_boot_guard(host, pre_flash_host, reset_boot_guard)
        suffix = f"\n{boot_guard_note}" if boot_guard_note else ""
        return (f"{base_msg}, and post-flash verification confirmed the board is running "
                f"{app_target.name!r} with the matching build{suffix}")

    try:
        ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=effective_kiln_fw_root, timeout_s=90)
        if ok:
            flash_provenance.write_provenance_json(
                tree_state, provenance_path, outcome=flash_provenance.OUTCOME_FLASHED_OK,
                kiln_fw_root_override=kiln_fw_root,
                erased_partitions=erase_note_entries or None,
            )
            archive_note = _archive_flashed_elf(build_dir, app_bin_path, tree_state, kiln_fw_root)
            return _post_flash(stale_prefix + _erase_note("OK") + "flashed and verified OK (bootloader + partition table + app), board reset and running" + archive_note)

        if retry_once:
            _srv._session_log.warning("flash_firmware: first attempt failed, retrying once (known benign quirk)")
            ok2, output2 = _run_openocd(openocd_exe, board_cfg, tcl, cwd=effective_kiln_fw_root, timeout_s=90)
            if ok2:
                flash_provenance.write_provenance_json(
                    tree_state, provenance_path, outcome=flash_provenance.OUTCOME_FLASHED_OK,
                    kiln_fw_root_override=kiln_fw_root,
                    erased_partitions=erase_note_entries or None,
                )
                archive_note = _archive_flashed_elf(build_dir, app_bin_path, tree_state, kiln_fw_root)
                return _post_flash(stale_prefix + _erase_note("OK") + "flashed and verified OK on retry (first attempt hit the known benign Verify-Failed quirk)" + archive_note)
            output = output2

        tail = "\n".join(output.strip().splitlines()[-25:])
        flash_provenance.write_provenance_json(
            tree_state, provenance_path,
            outcome=flash_provenance.OUTCOME_FLASH_FAILED,
            detail=tail,
            kiln_fw_root_override=kiln_fw_root,
            erased_partitions=erase_note_entries or None,
        )
        return (
            "error: flash failed" + (" twice" if retry_once else "") + f":\n{tail}\n\n"
            + _erase_note("FAILED (whole OpenOCD session failed -- see output above)") +
            "Do not run a raw `flash erase_sector`/full-chip-erase recovery by hand -- "
            "that combination (partial reflash after a full erase) is what caused the "
            "2026-08-11 incident this tool exists to prevent. If this persists, a full "
            "USB power cycle of the board (not just a JTAG reset) has resolved a "
            "flash-write-protect-stuck state before."
        )
    finally:
        if erase_tmp_dir:
            shutil.rmtree(erase_tmp_dir, ignore_errors=True)


def _recovery_board_state_refusals(host: Optional[str], allow_link_down: bool = False
                                   ) -> "tuple[list[str], list[str], list[str]]":
    """Live precondition read for flash_recovery(): returns (hazards,
    unreadable, notes) -- see recovery_flash.board_state_refusals(). Imports
    are lazy -- those modules import this package's mcp_server, which is
    mid-import when this module is loaded. Any read that raises is
    "unreadable", never a pass."""
    from . import mcp_server_coordinated_gpio_test as gpio_tool  # noqa: PLC0415
    from . import mcp_server_ota_matrix as ota_matrix  # noqa: PLC0415

    try:
        pf = gpio_tool._gpio_test_preflight(gpio_tool._gpio_test_resolve_host(host))
    except Exception as exc:  # noqa: BLE001 -- unreadable
        return [], [f"board state (profile/ARMED/interlock/link) could not be read: {exc}"], []
    notes: "list[str]" = []
    hazards, unreadable = recovery_flash.board_state_refusals(
        pf, ota_matrix._read_armed_latch_conditions, allow_link_down=allow_link_down, notes=notes)
    return hazards, unreadable, notes


def _probe_board_partitions(host: Optional[str]):
    """One live GET /api/partitions read for flash_recovery()'s pre-write
    checks. Returns (address, chip_entries, running):
      * answered with the main app's table -> (addr, [PartitionEntry...], running)
      * answered with the RECOVERY image's shape -> (addr, None, exc.running)
      * nothing answered (or the body was unusable) -> (None, None, None)
    A usable running name with an unusable table body is (addr, None, running)."""
    for candidate in _resolve_verify_hosts(host):
        try:
            data = partition_http_client.get_partitions(candidate, timeout=_VERIFY_HTTP_TIMEOUT_S)
        except partition_http_client.RecoveryImageResponse as exc:
            return candidate, None, exc.running
        except partition_http_client.PartitionHttpError:
            continue
        running = data.get("running")
        try:
            entries = [
                partition_table.PartitionEntry(
                    name=i["label"], type=int(i["type"]), subtype=int(i["subtype"]),
                    offset=int(i["offset"]), size=int(i["size"]))
                for i in data["partitions"]
            ]
        except (KeyError, TypeError, ValueError):
            return candidate, None, running if isinstance(running, str) else None
        return candidate, entries, running if isinstance(running, str) else None
    return None, None, None


def _observe_boot_after_recovery_flash(host: Optional[str], pre_flash_host: Optional[str]) -> str:
    """Post-write, post-reset: poll GET /api/partitions and report what
    actually booted. Never raises; one line, starting with `app`, `recovery`,
    `other` or `unreachable`."""
    tried: "list[str]" = []
    last: Optional[str] = None
    for attempt in range(_VERIFY_POLL_ATTEMPTS):
        if attempt:
            time.sleep(_VERIFY_POLL_INTERVAL_S)
        for candidate in _resolve_verify_hosts(host, pre_flash_host):
            if candidate not in tried:
                tried.append(candidate)
            try:
                data = partition_http_client.get_partitions(candidate, timeout=_VERIFY_HTTP_TIMEOUT_S)
            except partition_http_client.RecoveryImageResponse as exc:
                return (f"recovery -- the board answered at {candidate} with the recovery image's "
                        f"shape (running={exc.running!r}, next_update={exc.next_update!r})")
            except partition_http_client.PartitionHttpError as exc:
                last = str(exc)
                continue
            running = data.get("running")
            if running == APP_PARTITION_NAME:
                return f"app -- the board answered at {candidate} running {running!r}"
            if running == recovery_flash.RECOVERY_PARTITION_NAME:
                return f"recovery -- the board answered at {candidate} running {running!r}"
            return f"other -- the board answered at {candidate} running {running!r}"
    return (f"unreachable -- no answer at {', '.join(tried) or '(no candidate address)'} after "
            f"{_VERIFY_POLL_ATTEMPTS} attempts ({last}); what booted is UNKNOWN")


@_srv._tool()
def flash_recovery(
    recovery_bin: Optional[str] = None,
    kiln_fw_root: Optional[str] = None,
    confirm: bool = False,
    dry_run: bool = False,
    board_cfg: str = "board/esp32s3-builtin.cfg",
    retry_once: bool = True,
    host: Optional[str] = None,
    allow_unreadable_board_state: bool = False,
    allow_unconfirmed_partition_table: bool = False,
    allow_reset_into_recovery: bool = False,
    allow_stale: bool = False,
    allow_link_down: bool = False,
) -> str:
    """Writes the ESP32-S3 RECOVERY image (firmware/KilnFW_recovery's
    recovery.bin) into the `recovery` (factory-subtype) partition over JTAG
    via OpenOCD -- the sanctioned way to do it (never esptool). A separate
    tool from flash_firmware() on purpose: that one writes bootloader +
    partition table + `app` with HTTP landing verification and a boot_guard
    reset, none of which apply here, and its erase allowlist deliberately
    forbids `recovery`; this one writes exactly one range and nothing else.

    Offset and size come from the partition named `recovery` in
    `<kiln_fw_root>/partitions.csv` (resolved fresh, never hardcoded; a
    duplicate `recovery` row refuses). `recovery_bin` defaults to
    `<kiln_fw_root>/../KilnFW_recovery/build/recovery.bin`, where
    check_00_kilnfw_recovery_target_build.ps1 publishes it. OpenOCD is pinned
    to the main board's JTAG adapter serial (refuses before OpenOCD if it is
    not enumerated), same as flash_firmware().

    `dry_run=True` resolves and validates everything and reports what would
    be written -- zero board access, no `confirm` needed.

    Refuses (naming the reason) when: `confirm` is not exactly True; the
    image path contains any of `[ ] $ { } "` (Tcl injection); the image is
    missing, empty, larger than the partition, wrong magic (not 0xE9), wrong
    chip id (not ESP32-S3), or its esp_app_desc_t is missing or its
    project_name is not `recovery`; the image's mtime predates the newest
    git-tracked file (all files if git cannot answer) under the
    firmware/KilnFW_recovery tree holding the image (excluding build/) -- override with
    `allow_stale=True` (the image age is always printed); the JTAG adapter is
    absent; the board reports `ota_interlock` not ok or the safety link down
    (the link-down case alone is waivable with `allow_link_down=True`, below).

    Board-state gate, split in two on purpose. POSITIVELY OBSERVED hazards --
    a running/paused profile, ARMED with autotune active / a relay energized /
    a latched trip, OTA interlock busy, link down -- ALWAYS refuse, no flag
    waives them. Reasons that are only "could not be read" (HTTP/UART link
    not up, probe raised) refuse unless `allow_unreadable_board_state=True`,
    the bring-up escape hatch; it waives nothing else.

    `allow_link_down=True` (separate flag) waives ONLY a safety link that is
    down: `link_up` False, and an OTA-interlock refusal that is needs_ack AND
    whose reason is "safety link is down". With the ESP-Pico link down heat is
    already cut (a live Pico trips S6b and drops K4; a dead one cannot drive
    K4), and without this flag a broken Pico would block recovery forever. The
    interlock short-circuits at link-down, so its heater-commanded and
    over-temperature checks are NOT run; in this mode the tool instead reads
    autotune idle, expander relays off, K4 off and no latched trip whether or
    not ARMED, keeps the profile-idle check a hazard, and says so in the
    result. It never waives a running profile, an energized relay, or any
    other interlock refusal.

    Live partition-table gate: before writing, GET /api/partitions must show
    the chip's single `recovery` entry matching the CSV target in offset,
    size, type and subtype. A readable table that DISAGREES always refuses.
    If the board does not answer, or answers with the recovery image's shape
    (no table), the match cannot be confirmed and the call refuses unless
    `allow_unconfirmed_partition_table=True` (separate from every other flag).

    Reset-into-recovery gate: the write ends with a reset. otadata is
    untouched, and a blank/erased otadata makes the bootloader boot the
    factory-subtype partition -- which IS `recovery` -- so on such a board the
    reset boots the brand-new, untested image. The RUNNING partition is read
    first; if it is `recovery` or cannot be read, the call refuses unless
    `allow_reset_into_recovery=True`.

    Never writes otadata, app, nvs, the bootloader or the partition table:
    exactly one `program_esp ... verify` command, for the recovery range.
    After the write the tool polls /api/partitions and reports what actually
    booted: `app`, `recovery`, `other` or `unreachable`. The write itself is
    JTAG read-back-verified; whether the new image boots/works is only as
    proven as that poll says -- a recovery boot proof beyond it is an
    owner-present step. S6a: this resets the ESP (halted during the JTAG
    write), so before safety_clear_trip() the safety status must show
    trip_reason 6 (SAFETY_TRIP_MAIN_FAULT) with trip_mask 0x0020 only;
    anything else (e.g. reason 7 LINK_DEAD, mask 0x0040) needs investigating,
    not clearing.

    Provenance (hash, size, app_desc version/build time, git HEAD/dirty set)
    goes to firmware/KilnFW/recovery_flash_provenance.json. A recovery.elf
    next to the image is archived under firmware/KilnFW/recovery_elf_archive/
    only when its embedded esp_app_desc_t build timestamp and version match
    the image's; otherwise a warning says why and nothing is archived."""
    if kiln_fw_root is not None:
        if not os.path.isabs(kiln_fw_root):
            return f"error: kiln_fw_root must be an absolute path, got {kiln_fw_root!r}"
        if not os.path.isdir(kiln_fw_root):
            return f"error: kiln_fw_root does not exist or is not a directory: {kiln_fw_root}"
        effective_root = kiln_fw_root
    else:
        effective_root = _kiln_fw_root()

    try:
        target = recovery_flash.resolve_recovery_target(effective_root)
        bin_path = recovery_bin or recovery_flash.default_recovery_bin(effective_root)
        image = recovery_flash.validate_image(bin_path, target)
        age_note = recovery_flash.image_age_note(
            image.path,
            recovery_flash.recovery_tree_for_image(
                image.path, os.path.normpath(os.path.join(effective_root, "..", "KilnFW_recovery"))),
            allow_stale,
        )
    except recovery_flash.RecoveryFlashRefusal as exc:
        return f"error: refusing to flash recovery -- {exc}"

    summary = (
        f"image: {image.path}\n"
        f"  size {image.size} B, sha256 {image.sha256}\n"
        f"  {age_note}\n"
        f"  app_desc: project={image.app_desc.project_name} version={image.app_desc.version} "
        f"build={image.app_desc.build_timestamp}\n"
        f"target: partition {target.name!r} offset=0x{target.offset:x} size=0x{target.size:x} "
        f"({target.size} B) per {os.path.join(effective_root, 'partitions.csv')}"
    )
    if dry_run:
        return (
            "dry run -- no board access, nothing written.\n" + summary + "\n"
            "would send to OpenOCD: " + recovery_flash.build_tcl(MAIN_BOARD_JTAG_SERIAL, image.path, target)
            + "\nthat is the ONLY write; otadata/app/nvs/bootloader/partition table are not touched."
        )

    if confirm is not True:
        return (
            "error: refusing to flash recovery without confirm=True (exactly True). "
            "Use dry_run=True to preview.\n" + summary
        )

    openocd_exe = _find_openocd_exe()
    if not openocd_exe:
        return "error: openocd.exe not found under ~/.espressif/tools/openocd-esp32/ or C:\\Espressif\\ -- is it installed?"
    adapter_refusal = _refuse_if_adapter_absent(MAIN_BOARD_JTAG_SERIAL, "main board (ESP32-S3)")
    if adapter_refusal:
        return adapter_refusal

    hazards, unreadable, state_notes = _recovery_board_state_refusals(host, allow_link_down)
    if hazards:
        return ("error: refusing to flash recovery -- observed hazard (no flag overrides this): "
                + "; ".join(hazards))
    if unreadable and allow_unreadable_board_state is not True:
        return ("error: refusing to flash recovery -- board state could not be read: "
                + "; ".join(unreadable)
                + ". Pass allow_unreadable_board_state=True only for bring-up of a board whose "
                "HTTP/UART link is not up.")

    pre_host, chip_entries, running = _probe_board_partitions(host)
    if chip_entries is None:
        if allow_unconfirmed_partition_table is not True:
            why = ("the board did not answer GET /api/partitions" if pre_host is None else
                   f"the board at {pre_host} answered without a usable partition table "
                   f"(recovery-image shape or malformed; running={running!r})")
            return (f"error: refusing to flash recovery -- {why}, so the `recovery` partition "
                    f"(0x{target.offset:x}, 0x{target.size:x}, app/factory) cannot be confirmed "
                    "against the chip's live table. Pass allow_unconfirmed_partition_table=True "
                    "only after confirming the table another way (debug_check_partition_table).")
    else:
        mismatch = recovery_flash.compare_chip_recovery_row(chip_entries, target)
        if mismatch is not None:
            return (f"error: refusing to flash recovery -- the board at {pre_host} disagrees with "
                    f"partitions.csv on the `recovery` partition: {mismatch}. One side is stale; "
                    "confirm with debug_check_partition_table(). No flag overrides a readable "
                    "table that disagrees.")
    if running is None or running == target.name:
        if allow_reset_into_recovery is not True:
            state = ("could not be read" if running is None
                     else f"is already {running!r} (the board is running the recovery image)")
            return (f"error: refusing to flash recovery -- the RUNNING partition {state}. This tool "
                    "does not write otadata, and a blank/erased otadata makes the bootloader boot "
                    "the factory-subtype partition, which is `recovery`: the post-write reset "
                    "would boot the brand-new, untested recovery image. Pass "
                    "allow_reset_into_recovery=True only if that is acceptable.")

    provenance_path = recovery_flash.recovery_provenance_path(elf_archive._repo_root())
    archive_dir = recovery_flash.recovery_archive_dir(elf_archive._repo_root())
    elf_archive._guard_against_test_write(provenance_path)
    elf_archive._guard_against_test_write(archive_dir)
    provenance_repo_root = (
        os.path.normpath(os.path.join(kiln_fw_root, "..", "..")) if kiln_fw_root else None
    )
    tree_state = flash_provenance.capture_tree_state(repo_root=provenance_repo_root)
    recovery_flash.write_provenance(provenance_path, image, target, tree_state, outcome="pending")

    kill_openocd_sessions()
    tcl = recovery_flash.build_tcl(MAIN_BOARD_JTAG_SERIAL, image.path, target)
    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=effective_root, timeout_s=90)
    if not ok and retry_once:
        _srv._session_log.warning("flash_recovery: first attempt failed, retrying once")
        ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=effective_root, timeout_s=90)
    if not ok:
        tail = "\n".join(output.strip().splitlines()[-25:])
        recovery_flash.write_provenance(provenance_path, image, target, tree_state,
                                        outcome="flash_failed", detail=tail)
        return (
            f"error: recovery flash failed{' twice' if retry_once else ''}:\n{tail}\n"
            "Do not run a raw `flash erase_sector` recovery by hand."
        )

    archive_note = ""
    archived: Optional[str] = None
    try:
        archived, note = recovery_flash.archive_recovery_elf(image.path, archive_dir, image, tree_state.head)
        archive_note = f"\nelf archived: {archived}" if archived else f"\nelf archive: {note}"
    except Exception as exc:  # noqa: BLE001 -- surfaced, never fails the flash
        archive_note = f"\nWARNING: recovery elf archiving FAILED (flash itself succeeded): {exc}"
    recovery_flash.write_provenance(provenance_path, image, target, tree_state,
                                    outcome="flashed_ok", elf_archived=archived)
    boot = _observe_boot_after_recovery_flash(host, pre_host)
    return (
        "recovery image WRITTEN and READ-BACK-VERIFIED over JTAG (OpenOCD program_esp ... verify, "
        "recovery range only), then the board was reset.\n"
        f"post-reset observation (GET /api/partitions): {boot}\n"
        + summary + archive_note + "\n"
        f"provenance: {provenance_path} (git head {tree_state.head}, "
        f"{len(tree_state.dirty_files)} dirty file(s))\n"
        + "".join(f"NOTE: {n}\n" for n in state_notes) +
        "NOTE: this reset the ESP; before safety_clear_trip(), safety_get_status must show "
        "trip_reason 6 (SAFETY_TRIP_MAIN_FAULT) with trip_mask 0x0020 only. Anything else "
        "(e.g. reason 7 LINK_DEAD, mask 0x0040 -- plausible, the ESP was halted during the JTAG "
        "write) needs investigating, not clearing."
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

    fixture_root = _unit_test_fixture_fw_root()

    images: "list[tuple[str, str]]" = []  # (path, flash offset)
    if bootloader_bin:
        images.append((bootloader_bin, "0x0"))
    partition_table_offset_note = ""
    if partition_table_bin:
        # Same _resolve_partition_table_offset() helper flash_firmware() uses
        # for the main board, so the two can never drift independently --
        # both used to hardcode 0x8000 (IDF's default) regardless of what
        # this project's own sdkconfig actually says.
        try:
            fixture_partition_table_offset, partition_table_offset_note = _resolve_partition_table_offset(fixture_root)
        except ValueError as exc:
            return f"error: {exc}"
        images.append((partition_table_bin, f"0x{fixture_partition_table_offset:x}"))
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

    note_suffix = f"\n{partition_table_offset_note}" if partition_table_offset_note else ""

    ok, output = _run_openocd(openocd_exe, board_cfg, tcl, cwd=fixture_root, timeout_s=90)
    if ok:
        return "flashed and verified OK (" + ", ".join(os.path.basename(p) for p, _ in images) + "), board reset" + note_suffix

    if retry_once:
        _srv._session_log.warning("fixture_flash: first attempt failed, retrying once (known benign quirk)")
        ok2, output2 = _run_openocd(openocd_exe, board_cfg, tcl, cwd=fixture_root, timeout_s=90)
        if ok2:
            return "flashed and verified OK on retry (" + ", ".join(os.path.basename(p) for p, _ in images) + "), board reset" + note_suffix
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
    `firmware/KilnFW/elf_archive/` (a sibling of build/, not inside it --
    see elf_archive.kiln_archive_dir()'s 2026-09-15 note) by hand.

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
    failure mode (confident wrong line numbers) this tool exists to prevent.

    CAVEAT (2026-09-16): this keys the lookup off the board's CURRENTLY
    RUNNING `fw_build` -- correct for symbolizing a panic that just
    happened, on a board that has not been reflashed since. It is WRONG for
    a coredump that has been sitting in the coredump partition (or in the
    durable coredump archive) since an earlier boot: the board can be
    running a different build by the time you call this, and this function
    has no way to know that from `fw_build` alone. For a STORED coredump,
    use `find_crash_elf_for_coredump()` instead, which verifies against the
    coredump's own embedded SHA256 rather than trusting any externally
    reported build identity."""
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
def find_crash_elf_for_coredump(coredump_path: str) -> str:
    """Finds the ELF that actually produced a STORED coredump file (one
    already fetched via `read_esp_coredump()`, or a durable archive copy
    under `firmware/KilnFW/coredump_archive/`), by verifying against the
    coredump's own embedded app SHA256 -- never by trusting any externally
    reported build identity.

    Use this instead of `find_crash_elf()` whenever the coredump was NOT
    just now pulled from a board you know hasn't been reflashed since: a
    coredump persists in the coredump partition (or in the durable archive)
    across later flashes/OTAs/reboots, so the board's CURRENTLY running
    `fw_build` -- and even a coredump archive sidecar's recorded
    `fw_build_reported` (explicitly documented in coredump_fetch.py as "what
    was running when we fetched it", not "what panicked") -- can both name
    the wrong build. Confirmed live: one bench-board coredump's own embedded
    SHA256 began `0965ca575...` while the board's then-current app's began
    `1204ef146...`.

    Tries every ELF `elf_archive` currently has archived for KilnFW against
    esp_coredump's own SHA256 check (the strongest available identifier of
    a dump's true origin) and returns the first exact match, symbolized.

    Fails LOUD and DISTINCTLY on two different outcomes, never conflated:
      - an ENVIRONMENT problem (wrong Python interpreter, IDF_PATH unset,
        espcoredump.py missing) aborts immediately, naming the problem --
        this says nothing about whether any archived ELF matches;
      - every archived ELF was tried and NONE matched is reported as
        PERMANENTLY UNSYMBOLIZABLE: a legitimate outcome (the build that
        produced this dump was never archived, or its ELF was since
        pruned), not a tooling failure and not a "guess the closest one"
        situation -- do not retry with a substitute ELF chosen by hand."""
    if not os.path.isfile(coredump_path):
        return f"error: coredump file does not exist: {coredump_path!r}"
    candidates = elf_archive.list_all_kiln_elf_paths()
    if not candidates:
        return (
            "error: the KilnFW ELF archive currently has zero candidate ELFs "
            f"(checked {elf_archive.kiln_archive_dir()}) -- nothing to search, "
            "not a verdict about this coredump"
        )
    try:
        elf_path, symbolized = coredump_fetch.find_matching_archived_elf(coredump_path, candidates)
    except coredump_fetch.CoredumpSymbolizeError as exc:
        return f"error: {exc}"
    return f"elf={elf_path}\n{symbolized}"


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
    OS temp dir, overwritten on repeated calls -- a scratch working copy,
    NOT the durable record) and, if `symbolize` (default True), also finds
    the matching archived ELF via `find_crash_elf()` and runs espcoredump
    against both. Fails LOUD -- never a plausible-looking wrong backtrace --
    on: no coredump present, a truncated transfer, no archived ELF found for
    the board's reported fw_build, or an ELF that does not match the
    coredump (espcoredump's own SHA256 check, propagated verbatim). Pass
    `elf_path` to symbolize against a specific ELF instead of looking one up.
    Pass `symbolize=False` to only fetch the raw file (e.g. no ESP-IDF
    toolchain available on this machine) -- still gets you the coredump off
    the board and onto disk, uninspected.

    Every successful fetch is ALSO copied into a durable, content-addressed
    archive (`coredump_fetch.coredump_archive_dir()`, a sibling of build/ so
    `idf.py fullclean` cannot wipe it -- same rationale as
    `elf_archive.kiln_archive_dir()`'s 2026-09-15 move) with a provenance
    sidecar naming the host, the board's fw_build AT FETCH TIME, and when it
    was fetched. This tool never clears or deletes anything, here or on the
    board -- `/api/coredump/info`'s presence flag is untouched by a read."""
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

    # Best-effort fw_build read for the provenance record -- reused below
    # for ELF lookup when symbolizing, but fetched here unconditionally so
    # the archive record is written even when symbolize=False.
    fw_build_for_provenance = None
    try:
        fw_build_for_provenance = capability_preflight.get_board_info(resolved).fw_build
    except Exception:  # noqa: BLE001 - provenance is best-effort, never blocks the archive copy
        pass

    try:
        archived = coredump_fetch.archive_coredump(
            out_path, host=resolved, fw_build_reported=fw_build_for_provenance
        )
        result += f"\narchived: {archived.path} (sha256={archived.sha256[:12]}, provenance={archived.provenance_path})"
    except coredump_fetch.CoredumpFetchError as exc:
        # Loud, but does not discard an already-successful fetch -- the raw
        # file at out_path is still valid and reported above.
        result += f"\nwarning: could not write durable archive copy: {exc}"

    if not symbolize:
        return result

    if elf_path is not None:
        try:
            symbolized = coredump_fetch.symbolize_coredump(out_path, elf_path, fw_build="<given explicitly>")
        except coredump_fetch.CoredumpSymbolizeError as exc:
            return f"{result}\nerror: {exc}"
        return f"{result}\nelf={elf_path}\n{symbolized}"

    # No elf_path given: this coredump was JUST fetched over HTTP, so the
    # board's currently-reported fw_build is the most likely origin build --
    # try it first as a fast path. But it is NOT guaranteed correct (the
    # board could have been reflashed between the panic that produced this
    # coredump and this fetch, without the coredump partition being
    # cleared), so a mismatch here falls through to a full content-based
    # search across every archived ELF rather than being reported as a
    # hard failure. This is the 2026-09-16 fix for the gap find_crash_elf()
    # names in its own docstring: never trust an externally-reported build
    # identity as the final word on what produced a given coredump -- verify
    # against the coredump's own embedded SHA256 instead.
    fw_build = None
    fast_path_elf = None
    try:
        info = capability_preflight.get_board_info(resolved)
        fw_build = info.fw_build
    except Exception:  # noqa: BLE001 - fast path is best-effort; fall through to full search either way
        pass
    if fw_build:
        fast_path_elf, _msg = elf_archive.find_kiln_elf_for_build(fw_build)

    candidates = elf_archive.list_all_kiln_elf_paths()
    if fast_path_elf is not None:
        # Try the likely candidate first (fast, and the common case), but
        # keep it in the full candidate list too so find_matching_archived_elf
        # still finds it if ordering here ever changes.
        ordered = [fast_path_elf] + [p for p in candidates if p != fast_path_elf]
    else:
        ordered = candidates

    try:
        elf_path, symbolized = coredump_fetch.find_matching_archived_elf(out_path, ordered)
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
