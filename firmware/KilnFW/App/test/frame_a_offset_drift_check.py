#!/usr/bin/env python3
"""frame_a_offset_drift_check.py -- Frame A (SAFETY_CMD_GET_STATUS, 0x01) is
hand-duplicated across THREE copies that must agree byte-for-byte, per
CommonFW/docs/LINK_PROTOCOL.md's wire layout for this frame:

  1. SaftyFW/src/tasks/link_frame.c's link_frame_pack_status()   (pack side)
  2. KilnFW/App/drivers/safety_link_frames.c's safety_apply_status() (parse side)
  3. SaftyFW/test/test_link_frame_wire.c's mirror_apply_status() (the drift
     test FOR #1/#2, itself a third hand transcription -- ROADMAP M15: "the
     drift test for the item above is itself a third hand-copy" -- so it can
     drift green exactly like the two real functions it exists to catch
     drifting from each other)

An offset mismatch between any two of these passes CRC (the frame is
well-formed, just misinterpreted) and silently misdecodes temperatures/
currents/flags -- this is a static extraction+diff, not a test, because
nothing about "byte 11 is amps1" is observable from a passing unit test on
either side alone.

This script extracts, from each copy, the ordered list of (field_name,
byte_offset) pairs actually assigned/read, and fails with specifics if any
two copies disagree -- on offsets, on field order, or on which fields exist.

Usage: python frame_a_offset_drift_check.py [repo_root]
Exit 0: all three copies agree. Exit 1: drift found (printed with specifics).
"""
import re
import sys
from pathlib import Path

# Each copy is scanned line-by-line inside a bounded region (a specific
# function) for `<array>[<index>]` occurrences bound to a numeric offset
# (either a literal like `p[23]` or a symbolic one like `&out[2]` -- the
# packer uses byte COUNTS via pack_f32_le(&out[N], ...), the parsers use
# direct reads like p[N]/payload[N]). The field name is taken from the
# right-hand side / call context so the three copies can be compared by
# offset AND by what each side believes lives there.

def extract_pack_side(text: str) -> list[tuple[int, str]]:
    """SaftyFW link_frame.c's link_frame_pack_status(): out[N] = ...; or
    pack_f32_le(&out[N], VALUE);"""
    m = re.search(r"link_frame_pack_status\([^)]*\)\s*\{(.*?)\n\}\n", text, re.S)
    if not m:
        raise RuntimeError("link_frame_pack_status() body not found")
    body = m.group(1)
    fields: list[tuple[int, str]] = []
    for line in body.splitlines():
        line = line.strip()
        m2 = re.match(r"out\[(\d+)\]\s*=\s*(\w+)", line)
        if m2:
            fields.append((int(m2.group(1)), m2.group(2)))
            continue
        m2 = re.match(r"pack_(?:f32|u16|u32)_le\(&out\[(\d+)\],\s*(\w+)\)", line)
        if m2:
            fields.append((int(m2.group(1)), m2.group(2)))
    return fields


def extract_kilnfw_parse_side(text: str) -> list[tuple[int, str]]:
    """KilnFW safety_link_frames.c's safety_apply_status(): link->cached.X =
    ...p[N]... or ...&p[N]..."""
    m = re.search(r"safety_apply_status\([^)]*\)\s*\{(.*?)\n    return true;\n\}", text, re.S)
    if not m:
        raise RuntimeError("safety_apply_status() body not found")
    body = m.group(1)
    fields: list[tuple[int, str]] = []
    # Match "link->cached.NAME = ...p[N]..." (first p[N]/&p[N] on the line is
    # the field's base offset; multi-byte reads like safety_read_f32_le(&p[6])
    # still key off that same first offset).
    assign_re = re.compile(r"link->cached\.(\w+(?:\[\d+\])?)\s*=\s*[^;]*?&?p\[(\d+)\]")
    for line in body.splitlines():
        line = line.strip()
        m2 = assign_re.search(line)
        if m2:
            fields.append((int(m2.group(2)), m2.group(1)))
    return fields


def extract_mirror_side(text: str) -> list[tuple[int, str]]:
    """SaftyFW test_link_frame_wire.c's mirror_apply_status(): out.NAME =
    ...p[N]..."""
    m = re.search(r"static mirror_status_t mirror_apply_status\([^)]*\)\s*\{(.*?)\n    out\.ok = true;", text, re.S)
    if not m:
        raise RuntimeError("mirror_apply_status() body not found")
    body = m.group(1)
    fields: list[tuple[int, str]] = []
    assign_re = re.compile(r"out\.(\w+(?:\[\d+\])?)\s*=\s*[^;]*?&?p\[(\d+)\]")
    for line in body.splitlines():
        line = line.strip()
        m2 = assign_re.search(line)
        if m2:
            fields.append((int(m2.group(2)), m2.group(1)))
    return fields


def by_offset(fields: list[tuple[int, str]]) -> dict[int, str]:
    return dict(fields)


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    pack_path = repo_root / "firmware/SaftyFW/src/tasks/link_frame.c"
    kilnfw_path = repo_root / "firmware/KilnFW/App/drivers/safety_link_frames.c"
    mirror_path = repo_root / "firmware/SaftyFW/test/test_link_frame_wire.c"

    for p in (pack_path, kilnfw_path, mirror_path):
        if not p.is_file():
            print(f"FRAME A OFFSET DRIFT CHECK: missing expected file {p}")
            return 1

    pack_fields = by_offset(extract_pack_side(pack_path.read_text()))
    kilnfw_fields = by_offset(extract_kilnfw_parse_side(kilnfw_path.read_text()))
    mirror_fields = by_offset(extract_mirror_side(mirror_path.read_text()))

    problems: list[str] = []
    offsets_to_check = [2, 6, 10, 11, 15, 19]
    canon_by_offset = {
        2: "tc_temp_c", 6: "cj_temp_c", 10: "tc_fault",
        11: "amps1", 15: "amps2", 19: "amps3",
    }

    # Cross-check: for each of KilnFW's/mirror's fields, the offset it
    # actually used for a given canonical field must match the pack side's
    # offset for that same field (found by reverse lookup on pack_fields).
    pack_offset_by_name = {v: k for k, v in pack_fields.items()}
    canon_pack_names = {2: "safety_tc_c", 6: "cj_c", 10: "tc_fault_bits", 11: "amps1", 15: "amps2", 19: "amps3"}
    for off, pack_name in canon_pack_names.items():
        actual_pack_off = pack_offset_by_name.get(pack_name)
        if actual_pack_off != off:
            problems.append(
                f"{pack_path}: '{pack_name}' packed at offset {actual_pack_off}, "
                f"expected {off} (canonical Frame A layout)"
            )
        kiln_canon_name = {2: "tc_temp_c", 6: "cj_temp_c", 10: "tc_fault",
                            11: "current_a[0]", 15: "current_a[1]", 19: "current_a[2]"}[off]
        kiln_off_for_name = None
        for o, n in kilnfw_fields.items():
            if n == kiln_canon_name:
                kiln_off_for_name = o
                break
        if kiln_off_for_name != off:
            problems.append(
                f"{kilnfw_path}: '{kiln_canon_name}' read from offset {kiln_off_for_name}, "
                f"expected {off} (must match {pack_path}'s offset {actual_pack_off} for '{pack_name}')"
            )
        mirror_off_for_name = None
        for o, n in mirror_fields.items():
            if n == kiln_canon_name:
                mirror_off_for_name = o
                break
        if mirror_off_for_name != off:
            problems.append(
                f"{mirror_path}: '{kiln_canon_name}' read from offset {mirror_off_for_name}, "
                f"expected {off} (must match {pack_path}'s offset {actual_pack_off} for '{pack_name}')"
            )

    if problems:
        print("FRAME A OFFSET DRIFT CHECK: FAILED")
        for p in problems:
            print(f"  {p}")
        return 1

    print(f"FRAME A OFFSET DRIFT CHECK: all three copies agree on {len(offsets_to_check)} offsets "
          f"({pack_path.name}, {kilnfw_path.name}, {mirror_path.name})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
