"""recovery_flash.py -- pure (no board, no MCP) logic behind the
`flash_recovery` tool in mcp_server_flash.py.

WHY THIS EXISTS. The ESP32-S3 recovery image (firmware/KilnFW_recovery, a
SEPARATE ESP-IDF project, built into recovery.bin by
firmware/KilnFW/App/test/check_00_kilnfw_recovery_target_build.ps1 and
published to firmware/KilnFW_recovery/build/recovery.bin) had no sanctioned
writer: flash_firmware() only writes the bootloader, the partition table and
the `app` row, and its erase allowlist forbids `recovery`. This module holds
everything about that write that can be decided without touching hardware, so
the tool wrapper stays a thin sequence and every refusal is unit-testable.

SAFETY SHAPE (same as flash_firmware()'s `_resolve_app_flash_target()`):
  * the write target is resolved BY NAME (`recovery`, factory subtype) from
    `<kiln_fw_root>/partitions.csv`, never hardcoded -- `recovery` and `app`
    are BOTH factory-era app partitions, so a (type, subtype) match is not
    safe, and a hardcoded offset is exactly what once overflowed `recovery`
    into `coredump`;
  * the image must prove it is a recovery image (magic 0xE9, ESP32-S3 chip
    id, a valid esp_app_desc_t whose project_name is `recovery`, which is
    the `project(recovery)` line in firmware/KilnFW_recovery/CMakeLists.txt)
    so the main app image can never land here by a wrong path;
  * exactly ONE `program_esp ... verify` command is ever built, for exactly
    the recovery range: never otadata, app, nvs or the bootloader/table.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import time
from dataclasses import dataclass
from typing import Optional

from . import esp_app_desc, partition_table

#: Partition name in partitions.csv (type app, subtype factory).
RECOVERY_PARTITION_NAME = "recovery"
#: firmware/KilnFW_recovery/CMakeLists.txt: `project(recovery)`.
RECOVERY_PROJECT_NAME = "recovery"
#: esp_image_header_t.magic
ESP_IMAGE_MAGIC = 0xE9
#: esp_image_header_t.chip_id (uint16 at byte offset 12); ESP_CHIP_ID_ESP32S3.
ESP_CHIP_ID_ESP32S3 = 0x0009
_CHIP_ID_OFFSET = 12
#: partition_table's factory app subtype.
_APP_TYPE = 0x00
_SUBTYPE_FACTORY = 0x00


class RecoveryFlashRefusal(Exception):
    """A pre-flight refusal; str(exc) names the reason."""


@dataclass(frozen=True)
class RecoveryImage:
    path: str
    size: int
    sha256: str
    app_desc: esp_app_desc.AppDesc


def resolve_recovery_target(kiln_fw_root: str) -> partition_table.PartitionEntry:
    """The partition named `recovery`, from `<kiln_fw_root>/partitions.csv`.
    Refuses if the CSV cannot be parsed, has no such partition, or the entry
    is not an app/factory partition."""
    csv_path = os.path.join(kiln_fw_root, "partitions.csv")
    try:
        entries = partition_table.parse_partitions_csv(csv_path)
    except (OSError, ValueError) as exc:
        raise RecoveryFlashRefusal(f"could not parse {csv_path}: {exc}") from exc
    matches = [e for e in entries if e.name == RECOVERY_PARTITION_NAME]
    if len(matches) > 1:
        raise RecoveryFlashRefusal(
            f"{csv_path} has {len(matches)} partitions named {RECOVERY_PARTITION_NAME!r} -- "
            "refusing to guess which one is the recovery image's home"
        )
    for entry in matches:
        if entry.type != _APP_TYPE or entry.subtype != _SUBTYPE_FACTORY:
            raise RecoveryFlashRefusal(
                f"{csv_path}: partition {RECOVERY_PARTITION_NAME!r} is not an app/factory "
                f"partition (type=0x{entry.type:02x} subtype=0x{entry.subtype:02x}) -- "
                "refusing to guess where a recovery image belongs"
            )
        return entry
    names = ", ".join(e.name for e in entries) or "(no partitions)"
    raise RecoveryFlashRefusal(
        f"{csv_path} has no partition named {RECOVERY_PARTITION_NAME!r}. Partitions found: {names}"
    )


def default_recovery_bin(kiln_fw_root: str) -> str:
    """Where check_00_kilnfw_recovery_target_build.ps1 publishes the image:
    `<repo>/firmware/KilnFW_recovery/build/recovery.bin`, a sibling tree of
    `firmware/KilnFW` (so `<kiln_fw_root>/../KilnFW_recovery/build/`)."""
    return os.path.normpath(os.path.join(kiln_fw_root, "..", "KilnFW_recovery", "build", "recovery.bin"))


#: Characters refused in the image path (after backslash -> slash): the path
#: is spliced inside a double-quoted Tcl word in the OpenOCD script, where
#: these allow command/variable substitution or word breakout.
_TCL_UNSAFE_CHARS = '[]${}"`'


def check_tcl_safe_path(bin_path: str) -> None:
    """Refuses a path that could inject Tcl into the OpenOCD command."""
    path = bin_path.replace("\\", "/")
    bad = sorted({c for c in path if c in _TCL_UNSAFE_CHARS})
    if bad:
        raise RecoveryFlashRefusal(
            f"recovery image path contains character(s) {' '.join(bad)} that are unsafe inside "
            f"the OpenOCD Tcl command: {bin_path!r} -- move/rename the image"
        )


def compare_chip_recovery_row(chip_entries, target: partition_table.PartitionEntry) -> "Optional[str]":
    """None if the board's live table has exactly one `recovery` entry that
    matches `target` in offset, size, type and subtype; else a description of
    the disagreement. `chip_entries` are partition_table.PartitionEntry."""
    rows = [e for e in chip_entries if e.name == target.name]
    if not rows:
        return f"board's live table has no partition named {target.name!r}"
    if len(rows) > 1:
        return f"board's live table has {len(rows)} partitions named {target.name!r}"
    c = rows[0]
    diffs = []
    if c.offset != target.offset:
        diffs.append(f"offset board 0x{c.offset:x} vs csv 0x{target.offset:x}")
    if c.size != target.size:
        diffs.append(f"size board 0x{c.size:x} vs csv 0x{target.size:x}")
    if c.type != target.type:
        diffs.append(f"type board 0x{c.type:02x} vs csv 0x{target.type:02x}")
    if c.subtype != target.subtype:
        diffs.append(f"subtype board 0x{c.subtype:02x} vs csv 0x{target.subtype:02x}")
    return "; ".join(diffs) if diffs else None


def recovery_tree_for_image(bin_path: str, fallback_tree: str) -> str:
    """The recovery source tree holding `bin_path` when it sits at
    `<tree>/build/<file>` and `<tree>` is a `KilnFW_recovery` directory;
    otherwise `fallback_tree`."""
    build_dir = os.path.dirname(os.path.abspath(bin_path))
    tree = os.path.dirname(build_dir)
    if os.path.basename(build_dir) == "build" and os.path.basename(tree) == "KilnFW_recovery":
        return tree
    return fallback_tree


def _tracked_files(tree: str) -> "Optional[list[str]]":
    """Absolute paths of git-tracked files under `tree`, or None if git is
    unavailable / `tree` is not in a repo / nothing is tracked."""
    import subprocess  # noqa: PLC0415
    try:
        r = subprocess.run(["git", "-C", tree, "ls-files", "-z", "--", "."],
                           capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    names = [n for n in r.stdout.decode("utf-8", errors="replace").split("\0") if n]
    return [os.path.join(tree, n) for n in names] or None


def newest_source_mtime(recovery_tree: str) -> "Optional[tuple[float, str]]":
    """(mtime, path) of the newest source file under `recovery_tree`,
    skipping its top-level `build/` directory; None if there is nothing to
    compare. Only git-tracked files count (an untracked scratch/editor file
    is not a source change); if git cannot answer, every file counts."""
    build = os.path.normpath(os.path.join(recovery_tree, "build"))
    tracked = _tracked_files(recovery_tree)
    if tracked is not None:
        candidates = [f for f in tracked
                      if not os.path.normpath(f).startswith(build + os.sep)]
    else:
        candidates = []
        for dirpath, dirnames, filenames in os.walk(recovery_tree):
            if os.path.normpath(dirpath) == os.path.normpath(recovery_tree):
                dirnames[:] = [d for d in dirnames if d != "build"]
            candidates.extend(os.path.join(dirpath, n) for n in filenames)
    best: "Optional[tuple[float, str]]" = None
    for full in candidates:
        try:
            m = os.path.getmtime(full)
        except OSError:
            continue
        if best is None or m > best[0]:
            best = (m, full)
    return best


def image_age_note(bin_path: str, recovery_tree: str, allow_stale: bool, now: Optional[float] = None) -> str:
    """Always-returned age line; raises RecoveryFlashRefusal when the image
    predates the newest recovery source file and `allow_stale` is not True."""
    now = time.time() if now is None else now
    img_m = os.path.getmtime(bin_path)
    built = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(img_m))
    note = f"image age: {(now - img_m) / 60.0:.1f} min (built {built})"
    newest = newest_source_mtime(recovery_tree)
    if newest is not None and img_m < newest[0]:
        msg = (f"{bin_path} is OLDER than {newest[1]} (source newer by "
               f"{(newest[0] - img_m) / 60.0:.1f} min) -- the image is stale; rebuild it")
        if allow_stale is not True:
            raise RecoveryFlashRefusal(msg + ", or pass allow_stale=True")
        note += "\nWARNING: " + msg + " (allow_stale=True given)"
    return note


def validate_image(bin_path: str, target: partition_table.PartitionEntry) -> RecoveryImage:
    """Every image-level refusal. Raises RecoveryFlashRefusal naming the reason."""
    check_tcl_safe_path(bin_path)
    if not os.path.isfile(bin_path):
        raise RecoveryFlashRefusal(
            f"recovery image not found: {bin_path} (build it with "
            "check_00_kilnfw_recovery_target_build.ps1, or pass recovery_bin)"
        )
    size = os.path.getsize(bin_path)
    if size == 0:
        raise RecoveryFlashRefusal(f"recovery image is empty (0 bytes): {bin_path}")
    if size > target.size:
        raise RecoveryFlashRefusal(
            f"recovery image is {size} bytes, which does not fit in the {target.name!r} partition "
            f"({target.size} bytes at 0x{target.offset:x}); writing it would overflow into the "
            "next partition"
        )
    with open(bin_path, "rb") as f:
        data = f.read()
    if data[0] != ESP_IMAGE_MAGIC:
        raise RecoveryFlashRefusal(
            f"bad image magic 0x{data[0]:02X} (expected 0x{ESP_IMAGE_MAGIC:02X}) -- "
            f"{bin_path} is not an ESP app image"
        )
    if len(data) < _CHIP_ID_OFFSET + 2:
        raise RecoveryFlashRefusal(f"image is only {len(data)} bytes -- too short for an image header")
    (chip_id,) = struct.unpack_from("<H", data, _CHIP_ID_OFFSET)
    if chip_id != ESP_CHIP_ID_ESP32S3:
        raise RecoveryFlashRefusal(
            f"wrong chip id 0x{chip_id:04X} in image header (expected ESP32-S3, "
            f"0x{ESP_CHIP_ID_ESP32S3:04X})"
        )
    try:
        desc = esp_app_desc.parse_app_desc(data)
    except esp_app_desc.AppDescError as exc:
        raise RecoveryFlashRefusal(f"esp_app_desc_t missing or invalid: {exc}") from exc
    if desc.project_name != RECOVERY_PROJECT_NAME:
        raise RecoveryFlashRefusal(
            f"esp_app_desc_t project_name is {desc.project_name!r}, not the recovery project "
            f"{RECOVERY_PROJECT_NAME!r} -- this is not a recovery image (refusing, in particular, "
            "to write the main application image into the recovery partition)"
        )
    return RecoveryImage(path=bin_path, size=size, sha256=hashlib.sha256(data).hexdigest(), app_desc=desc)


#: Substrings in a `_read_armed_latch_conditions()` string that mean "the
#: read failed / was inconclusive", not "a hazard was observed".
_UNREADABLE_MARKERS = ("could not be read", "could not be confirmed", "no diag received yet")
#: ota_interlock.c's link-down reason (result OTA_INTERLOCK_REFUSED_NEEDS_ACK).
LINK_DOWN_INTERLOCK_REASON = "safety link is down"
#: mcp_server_ota_matrix._read_armed_latch_conditions()'s latched-trip line.
LATCHED_TRIP_REASON_PREFIX = "a safety trip is latched"
LINK_DOWN_NOTE = (
    "allow_link_down=True: the safety link was down; the OTA interlock short-circuits at "
    "link-down, so its heater-commanded and over-temperature checks were NOT run. Live reads "
    "made instead: profile idle, autotune idle, expander relays off. K4 off and no latched trip "
    "are CACHED values, the last Pico status/diag the ESP received before the link dropped "
    "(age unknown), not live reads."
)


def _is_unreadable_reason(reason: str) -> bool:
    return any(m in reason for m in _UNREADABLE_MARKERS)


def board_state_refusals(preflight, armed_conditions_fn, allow_link_down: bool = False,
                         notes: "Optional[list[str]]" = None) -> "tuple[list[str], list[str]]":
    """(hazards, unreadable) from a GpioTestPreflight-shaped snapshot
    (tri-state fields; None = could not read). `hazards` are POSITIVELY
    OBSERVED unsafe states -- profile running/paused, ARMED (or link-down
    mode) with autotune active / relay energized / latched trip, OTA interlock
    busy, link down -- and must refuse regardless of any flag except the one
    narrow waiver below. `unreadable` are reasons that are only "could not be
    read"; the caller may waive exactly those with allow_unreadable_board_state.
    ARMED alone is acceptable (the safety processor's latched idle state,
    owner decision 2026-10-01).

    `allow_link_down=True` waives ONLY (a) link_up is False and (b) an OTA
    interlock refusal that is needs_ack AND whose reason is the link-down one.
    With the link down, heat is already cut by the safety processor (it trips
    S6b and drops K4; a dead one cannot drive K4), and the interlock
    short-circuits before its heater/over-temperature checks, so in that mode
    the armed-latch conditions are read whether or not ARMED, and the profile
    check stays a hazard. A non-link interlock refusal is never waived. A
    NOTE is appended to `notes` when the waiver applied."""
    hazards: "list[str]" = []
    unreadable: "list[str]" = []
    link_down_mode = False
    if preflight.profile_running_or_paused is True:
        hazards.append(
            f"a profile is running or paused (profile_state={preflight.profile_state_name!r}) "
            "-- no flash during a firing"
        )
    elif preflight.profile_running_or_paused is None:
        unreadable.append(
            f"profile state could not be read (profile_state={preflight.profile_state_name!r})"
        )
    if preflight.ota_interlock_ok is False:
        link_only = (getattr(preflight, "ota_interlock_needs_ack", None) is True
                     and str(preflight.ota_interlock_reason).strip() == LINK_DOWN_INTERLOCK_REASON)
        if allow_link_down is True and link_only:
            link_down_mode = True
        else:
            hazards.append(f"OTA interlock is not idle: {preflight.ota_interlock_reason}")
    elif preflight.ota_interlock_ok is not True:
        unreadable.append(f"OTA interlock could not be confirmed: {preflight.ota_interlock_reason}")
    if preflight.link_up is False:
        if allow_link_down is True:
            link_down_mode = True
        else:
            hazards.append("safety link is down (link_up=False)")
    elif preflight.link_up is not True:
        unreadable.append(f"safety link state could not be confirmed (link_up={preflight.link_up!r})")
    if preflight.safety_armed is None:
        unreadable.append("safety ARMED state could not be read (safety_armed=None)")
    if preflight.safety_armed is True or link_down_mode:
        prefix = "safety is ARMED and " if preflight.safety_armed is True else "with the safety link down, "
        try:
            extra = armed_conditions_fn()
        except Exception as exc:  # noqa: BLE001 -- unreadable
            unreadable.append(f"{prefix}its conditions could not be read: {exc}")
        else:
            for r in extra:
                if (link_down_mode and preflight.safety_armed is not True
                        and r.startswith(LATCHED_TRIP_REASON_PREFIX)):
                    # A latched trip is the SAFE state (heat de-energized), and a dead link is
                    # normally accompanied by one (S6b / dual-reset S6a) that clear_trip cannot
                    # clear without the link -- refusing would block the very recovery path
                    # allow_link_down exists for. Cached value: a note, never a hazard.
                    if notes is not None:
                        notes.append(f"{prefix}{r} (cached; a latched trip is the safe state, not a flash blocker)")
                    continue
                (unreadable if _is_unreadable_reason(r) else hazards).append(prefix + r)
    if link_down_mode and notes is not None:
        notes.append(LINK_DOWN_NOTE)
    return hazards, unreadable


def build_tcl(adapter_serial: str, bin_path: str, target: partition_table.PartitionEntry) -> str:
    """The ONLY OpenOCD script this tool sends: pin the adapter, one
    `program_esp ... verify` over exactly the recovery range, reset, exit.
    `bin_path` is passed through with forward slashes (Tcl)."""
    check_tcl_safe_path(bin_path)
    path = bin_path.replace("\\", "/")
    return (
        f"adapter serial {adapter_serial}; "
        f'program_esp "{path}" 0x{target.offset:x} verify reset exit'
    )


def recovery_provenance_path(repo_root: str) -> str:
    """Sibling of flash_provenance.json, under firmware/KilnFW/ (gitignored)."""
    return os.path.join(repo_root, "firmware", "KilnFW", "recovery_flash_provenance.json")


def recovery_archive_dir(repo_root: str) -> str:
    """Sibling of firmware/KilnFW/elf_archive/ (gitignored)."""
    return os.path.join(repo_root, "firmware", "KilnFW", "recovery_elf_archive")


def write_provenance(path: str, image: RecoveryImage, target: partition_table.PartitionEntry,
                     tree_state, outcome: str, detail: Optional[str] = None,
                     elf_archived: Optional[str] = None) -> None:
    """Point-in-time record of the LAST recovery flash attempt (overwrites,
    like flash_provenance.json). Best-effort: never raises."""
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump({
                "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "outcome": outcome,
                "detail": detail,
                "image_path": image.path,
                "image_size": image.size,
                "image_sha256": image.sha256,
                "app_desc_version": image.app_desc.version,
                "app_desc_project_name": image.app_desc.project_name,
                "app_desc_build_timestamp": image.app_desc.build_timestamp,
                "partition": {"name": target.name, "offset": target.offset, "size": target.size},
                "head": tree_state.head,
                "dirty_files": tree_state.dirty_files,
                "git_available": tree_state.git_available,
                "booted_and_verified": False,
                "note": "written and read-back-verified over JTAG; the boot outcome, if observed, is reported in the tool result only",
                "elf_archived": elf_archived,
            }, f, indent=2)
    except (OSError, ValueError):
        pass


def archive_recovery_elf(bin_path: str, archive_dir: str, image: RecoveryImage,
                         git_head: Optional[str]) -> "tuple[Optional[str], str]":
    """Archives `recovery.elf` (the ELF next to `bin_path`) as
    `recovery-<sha12>.elf` plus a one-line manifest entry, so a future
    recovery-image panic can be symbolized. Returns (archived_path_or_None,
    note). Nothing is archived (path None, note says why) when no ELF sits
    alongside the image, or when the ELF's own embedded esp_app_desc_t does
    not match the image's build timestamp AND version -- a stale ELF filed
    under this image's key would symbolize crashes wrongly. Deliberately NOT
    routed through elf_archive._archive(): that helper migrates a legacy
    `build/elf_archive` next to the archive dir, which for this directory
    would be KilnFW's, not ours. Raises OSError on a copy failure."""
    elf = os.path.splitext(bin_path)[0] + ".elf"
    if not os.path.isfile(elf):
        return None, "no recovery.elf next to the image, nothing archived"
    want_ts = esp_app_desc.normalize_build_timestamp(image.app_desc.build_timestamp)
    try:
        descs = esp_app_desc.scan_elf_for_app_descs(elf)
    except Exception as exc:  # noqa: BLE001
        return None, f"WARNING: could not scan {elf} for its app descriptor ({exc}); NOT archived"
    if not any(esp_app_desc.normalize_build_timestamp(d.build_timestamp) == want_ts
               and d.version == image.app_desc.version for d in descs):
        got = ", ".join(f"{d.version!r}@{d.build_timestamp!r}" for d in descs) or "none found"
        return None, (f"WARNING: {elf} does not match the flashed image (image version "
                      f"{image.app_desc.version!r} built {image.app_desc.build_timestamp!r}; ELF "
                      f"descriptors: {got}) -- NOT archived (stale ELF?)")
    h = hashlib.sha256()
    with open(elf, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    key = h.hexdigest()[:12]
    os.makedirs(archive_dir, exist_ok=True)
    dest = os.path.join(archive_dir, f"recovery-{key}.elf")
    if not os.path.exists(dest):
        import shutil  # noqa: PLC0415
        shutil.copyfile(elf, dest)
    manifest_path = os.path.join(archive_dir, "manifest.json")
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        manifest = {}
    manifest[esp_app_desc.normalize_build_timestamp(image.app_desc.build_timestamp)] = {
        "elf_key": key,
        "bin_sha256": image.sha256,
        "git_commit": git_head,
        "source": "flash_recovery",
        "archived_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    return dest, ""
