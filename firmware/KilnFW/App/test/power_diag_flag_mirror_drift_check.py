#!/usr/bin/env python3
"""power_diag_flag_mirror_drift_check.py -- ROADMAP.md M15: "safety_link.h
hand-mirrors CommonFW frame constants". firmware/KilnFW/App/drivers/
safety_link.h hand-duplicates the POWER (Frame E) and DIAG (Frame B) flag
bytes / frame lengths / channel counts / sentinels from CommonFW's
kilnlink_power.h and kilnlink_diag.h -- 9 "mirrored here" comments -- because
kilnlink_power.c/kilnlink_diag.c are not compiled into KilnFW's ESP-IDF
component today (see uart_task_ids.h's SAFETY_CMD_POWER/SAFETY_CMD_DIAG
comments and components/kilnlink/CMakeLists.txt), so safety_link_frames.c
hand-parses these frames byte-for-byte instead of calling
kilnlink_power_decode()/kilnlink_diag_decode().

A silent divergence between the mirror and the source of truth passes
compilation on both sides and misdecodes flags/lengths at runtime -- this is
a static extraction+diff, not a test, because nothing about "bit 1 is
ANY_CHANNEL_CLIPPED" is observable from a passing unit test on either side
alone (same reasoning as frame_a_offset_drift_check.py, which this script
follows closely).

This extracts NAME -> VALUE pairs from both sides for every constant listed
in MIRROR_MAP below and fails with specifics (name, both values) on any
disagreement. If a regex stops matching an expected symbol on either side,
this FAILS CLOSED (treated as a diff, not skipped) -- a check that silently
matches nothing is worse than none: that exact failure has shipped in this
repo before.

Usage: python power_diag_flag_mirror_drift_check.py [repo_root]
Exit 0: every mirrored constant agrees. Exit 1: drift found (or extraction
failure), printed with specifics.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

# (source-of-truth file, source symbol, mirror file, mirror symbol)
# Values are ints; both sides are parsed as C integer literals (0xNNu, NNu,
# or a bare enum member with an explicit '= <literal>').
MIRROR_MAP = [
    ("kilnlink_power.h", "KILNLINK_POWER_LEN", "safety_link.h", "SAFETY_LINK_POWER_FRAME_LEN"),
    ("kilnlink_power.h", "KILNLINK_POWER_CHANNELS", "safety_link.h", "SAFETY_LINK_POWER_CHANNELS"),
    ("kilnlink_power.h", "KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED",
     "safety_link.h", "SAFETY_LINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED"),
    ("kilnlink_power.h", "KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED",
     "safety_link.h", "SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED"),
    ("kilnlink_power.h", "KILNLINK_POWER_FLAG_CALIBRATED",
     "safety_link.h", "SAFETY_LINK_POWER_FLAG_CALIBRATED"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_LEN", "safety_link.h", "SAFETY_LINK_DIAG_FRAME_LEN"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN",
     "safety_link.h", "SAFETY_LINK_DIAG_FLAG_SIM_CONTEXT_SEEN"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_FLAG_CALIBRATION_MISSING",
     "safety_link.h", "SAFETY_LINK_DIAG_FLAG_CALIBRATION_MISSING"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT",
     "safety_link.h", "SAFETY_LINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_BOOT_POWERON", "safety_link.h", "SAFETY_LINK_DIAG_BOOT_POWERON"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_BOOT_WATCHDOG", "safety_link.h", "SAFETY_LINK_DIAG_BOOT_WATCHDOG"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_BOOT_BROWNOUT", "safety_link.h", "SAFETY_LINK_DIAG_BOOT_BROWNOUT"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_STATE_INIT", "safety_link.h", "SAFETY_LINK_DIAG_STATE_INIT"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_STATE_GRACE", "safety_link.h", "SAFETY_LINK_DIAG_STATE_GRACE"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_STATE_ARMED", "safety_link.h", "SAFETY_LINK_DIAG_STATE_ARMED"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_STATE_WARN", "safety_link.h", "SAFETY_LINK_DIAG_STATE_WARN"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_STATE_TRIPPED", "safety_link.h", "SAFETY_LINK_DIAG_STATE_TRIPPED"),
    ("kilnlink_diag.h", "KILNLINK_DIAG_CONTEXT_AGE_NEVER",
     "safety_link.h", "SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER"),
]

INT_RE = r"(0[xX][0-9a-fA-F]+|\d+)u?"


def _parse_int(literal: str) -> int:
    return int(literal, 0)


def extract_constants(text: str, names: set[str]) -> dict[str, int]:
    """Finds, for each name in `names`, either:
      #define NAME <int literal>
    or an enum member:
      NAME = <int literal>
    Returns {name: value} for every name actually found. Callers must check
    every requested name was found -- a name that silently matches nothing
    is exactly the failure mode this check exists to avoid.
    """
    found: dict[str, int] = {}
    for name in names:
        m = re.search(rf"#define\s+{re.escape(name)}\s+{INT_RE}", text)
        if not m:
            m = re.search(rf"\b{re.escape(name)}\s*=\s*{INT_RE}", text)
        if m:
            found[name] = _parse_int(m.group(1))
    return found


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    commonfw_dir = repo_root / "firmware" / "CommonFW" / "include" / "kilnlink"

    try:
        safety_link_path = resolve_driver_file(repo_root, "safety_link.h")
    except DriverFileError as exc:
        print("POWER/DIAG FLAG MIRROR DRIFT CHECK: FAILED (setup)")
        print(f"  {exc}")
        return 1

    file_paths = {
        "kilnlink_power.h": commonfw_dir / "kilnlink_power.h",
        "kilnlink_diag.h": commonfw_dir / "kilnlink_diag.h",
        "safety_link.h": safety_link_path,
    }

    texts: dict[str, str] = {}
    for label, path in file_paths.items():
        if not path.is_file():
            print(f"POWER/DIAG FLAG MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  Expected file not found: {path}")
            return 1
        texts[label] = path.read_text(encoding="utf-8")

    names_needed: dict[str, set[str]] = {"kilnlink_power.h": set(), "kilnlink_diag.h": set(), "safety_link.h": set()}
    for src_file, src_name, mirror_file, mirror_name in MIRROR_MAP:
        names_needed[src_file].add(src_name)
        names_needed[mirror_file].add(mirror_name)

    extracted: dict[str, dict[str, int]] = {}
    for label, names in names_needed.items():
        extracted[label] = extract_constants(texts[label], names)

    # Fail closed: every name we asked for must have been found. A name that
    # matches nothing means the regex (or the source file) drifted out from
    # under this check -- report it as a failure, not a silent skip.
    missing_report = []
    for label, names in names_needed.items():
        for name in sorted(names):
            if name not in extracted[label]:
                missing_report.append(f"  {label}: could not find/parse '{name}'")

    if missing_report:
        print("POWER/DIAG FLAG MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  One or more expected constants could not be extracted -- treating this as")
        print("  drift rather than skipping, since a regex that stops matching is exactly")
        print("  how this class of check has shipped vacuous before:")
        for line in missing_report:
            print(line)
        return 1

    mismatches = []
    for src_file, src_name, mirror_file, mirror_name in MIRROR_MAP:
        src_val = extracted[src_file][src_name]
        mirror_val = extracted[mirror_file][mirror_name]
        if src_val != mirror_val:
            mismatches.append(
                f"  {mirror_file}:{mirror_name} = {mirror_val!r} but "
                f"{src_file}:{src_name} = {src_val!r}"
            )

    if mismatches:
        print("POWER/DIAG FLAG MIRROR DRIFT CHECK: FAILED")
        print("  safety_link.h's hand-mirrored constant(s) disagree with the CommonFW")
        print("  source of truth (kilnlink_power.h / kilnlink_diag.h):")
        for line in mismatches:
            print(line)
        return 1

    print(f"POWER/DIAG FLAG MIRROR DRIFT CHECK: OK ({len(MIRROR_MAP)} mirrored constants agree)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
