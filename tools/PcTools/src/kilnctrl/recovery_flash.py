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
    for entry in entries:
        if entry.name == RECOVERY_PARTITION_NAME:
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


def validate_image(bin_path: str, target: partition_table.PartitionEntry) -> RecoveryImage:
    """Every image-level refusal. Raises RecoveryFlashRefusal naming the reason."""
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


def board_state_refusals(preflight, armed_conditions_fn) -> "list[str]":
    """Refusal reasons from a GpioTestPreflight-shaped snapshot (tri-state
    fields; None = could not read = refuse, fail closed). ARMED alone is
    acceptable (the safety processor's latched idle state, owner decision
    2026-10-01); ARMED with a firing in progress (profile running/paused, or
    autotune active/relay energized per `armed_conditions_fn`) is not."""
    reasons: "list[str]" = []
    if preflight.profile_running_or_paused is not False:
        reasons.append(
            f"a profile is running or paused, or its state could not be confirmed "
            f"(profile_state={preflight.profile_state_name!r}) -- no flash during a firing"
        )
    if preflight.safety_armed is None:
        reasons.append("safety ARMED state could not be confirmed (safety_armed=None)")
    elif preflight.safety_armed is True:
        try:
            extra = armed_conditions_fn()
        except Exception as exc:  # noqa: BLE001 -- unreadable must refuse
            extra = [f"ARMED-state conditions could not be read: {exc}"]
        reasons.extend(f"safety is ARMED and {r}" for r in extra)
    return reasons


def build_tcl(adapter_serial: str, bin_path: str, target: partition_table.PartitionEntry) -> str:
    """The ONLY OpenOCD script this tool sends: pin the adapter, one
    `program_esp ... verify` over exactly the recovery range, reset, exit.
    `bin_path` is passed through with forward slashes (Tcl)."""
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
                "note": "written and read-back-verified over JTAG only; never booted by this tool",
                "elf_archived": elf_archived,
            }, f, indent=2)
    except (OSError, ValueError):
        pass


def archive_recovery_elf(bin_path: str, archive_dir: str, image: RecoveryImage,
                         git_head: Optional[str]) -> Optional[str]:
    """Archives `recovery.elf` (the ELF next to `bin_path`) as
    `recovery-<sha12>.elf` plus a one-line manifest entry, so a future
    recovery-image panic can be symbolized. Returns the archived path, or
    None if no ELF sits alongside the image. Deliberately NOT routed through
    elf_archive._archive(): that helper migrates a legacy `build/elf_archive`
    next to the archive dir, which for this directory would be KilnFW's, not
    ours. Raises OSError on a copy failure (the caller reports, not hides)."""
    elf = os.path.splitext(bin_path)[0] + ".elf"
    if not os.path.isfile(elf):
        return None
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
    return dest
