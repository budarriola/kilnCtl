#!/usr/bin/env python3
"""frame_a_offset_drift_check.py -- Frame A (SAFETY_CMD_GET_STATUS, 0x01) used
to be hand-duplicated across THREE independent literal offset tables:

  1. SaftyFW/src/tasks/link_frame.c's link_frame_pack_status()   (pack side)
  2. KilnFW/App/drivers/safety_link_frames.c's safety_apply_status() (parse side)
  3. SaftyFW/test/test_link_frame_wire.c's mirror_apply_status() (the drift
     test FOR #1/#2, itself formerly a third hand transcription -- ROADMAP
     M15: "the drift test for the item above is itself a third hand-copy")

An offset mismatch between any two of these passed CRC (the frame is
well-formed, just misinterpreted) and would have silently misdecoded
temperatures/currents/flags. ROADMAP M15 "Frame A's field layout is
hand-duplicated across firmwares" fixed the root cause: all three sites now
index through the shared KILNLINK_FRAME_A_OFF_*/KILNLINK_FRAME_A_LEN_*
macros in firmware/CommonFW/include/kilnlink/kilnlink_frame_a_offsets.h
instead of a literal `p[N]`/`out[N]`. Offset drift between the three sites is
now a compile-time impossible state (they share one #define), not something
only a periodic diff could catch.

That changes this check's job: it no longer extracts and diffs three offset
tables (there is only one table left, in the CommonFW header, and the C
compiler already enforces that both firmwares agree on it). Instead it
guards against the ROOT CAUSE reappearing -- a literal numeric byte offset
(`p[23]`, `out[6]`, `&x[19]`, etc.) creeping back into any of the three call
sites' Frame A functions instead of the shared macro. That would be exactly
as dangerous as the original bug (a literal offset can silently disagree
with the macro-driven ones elsewhere) and exactly as invisible to a passing
unit test, so it stays a static extraction+regex check, not a runtime test.

Usage: python frame_a_offset_drift_check.py [repo_root]
Exit 0: no bare literal Frame A offset found in any of the three functions.
Exit 1: a bare literal offset was found (printed with file/function/line), or
        one of the three functions/files could not be located at all (fail
        closed -- if this script's own regex stops matching the function it
        is supposed to be scanning, that is itself a reason to fail, not to
        silently pass zero findings).
"""
import re
import sys
from pathlib import Path

# Each site is a (label, relative_path, function_name_regex, body_terminator)
# tuple describing how to isolate the Frame A function's body text out of its
# file, the same bounded-region approach the original version of this check
# used. Kept intentionally narrow (one specific function per file) so this
# check does not fire on unrelated array indexing elsewhere in a 4000+ line
# driver file.
SITES = [
    (
        "pack side",
        "firmware/SaftyFW/src/tasks/link_frame.c",
        r"size_t link_frame_pack_status\([^)]*\)\s*\{(.*?)\n\}\n",
    ),
    (
        "parse side",
        "firmware/KilnFW/App/drivers/safety_link_frames.c",
        r"\bbool safety_apply_status\([^)]*\)\s*\{(.*?)\n    return true;\n\}",
    ),
    (
        "drift-test mirror",
        "firmware/SaftyFW/test/test_link_frame_wire.c",
        r"static mirror_status_t mirror_apply_status\([^)]*\)\s*\{(.*?)\n    out\.ok = true;",
    ),
]

# A bare literal index: `identifier[<digits>]`, where identifier is `out`,
# `p`, or `payload` (the buffer names every one of the three functions above
# uses for this frame) -- e.g. `out[23]`, `p[6]`, `&payload[11]`. Deliberately
# does NOT match `out[KILNLINK_FRAME_A_OFF_...]` (the index there is an
# identifier, not digits) or unrelated indexing like `current_a[0]` /
# `ct_cal[channel]` (wrong base identifier). This is precisely the shape the
# removed hand-written offset tables had.
#
# Offset 0 (the command byte) is excluded deliberately: it is not part of the
# drift-prone field table -- it is always the frame's fixed command id
# (KILNLINK_FRAME_A_CMD == LINK_FRAME_STATUS_CMD), the same single-byte
# convention every other frame in this codebase uses, and never encodes a
# numeric field whose offset could silently disagree between sites. Flagging
# `out[0]`/`payload[0]` here would just be noise on every one of these
# functions' first line.
LITERAL_OFFSET_RE = re.compile(r"\b(out|p|payload)\[\s*(\d+)\s*\]")


def _is_flagged(match: "re.Match[str]") -> bool:
    return match.group(2) != "0"


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    problems: list[str] = []

    for label, rel_path, func_re in SITES:
        path = repo_root / rel_path
        if not path.is_file():
            problems.append(f"{rel_path}: file not found")
            continue
        text = path.read_text()
        m = re.search(func_re, text, re.S)
        if not m:
            # Fail closed: if the target function can no longer be located
            # (renamed, restructured, deleted), that is a reason to fail this
            # check, not to silently report zero findings against nothing.
            problems.append(
                f"{rel_path}: could not locate the {label} function this check "
                f"is supposed to scan (regex no longer matches) -- update "
                f"frame_a_offset_drift_check.py's SITES entry rather than "
                f"letting this check pass vacuously"
            )
            continue
        body = m.group(1)
        body_start_line = text.count("\n", 0, m.start(1)) + 1
        for line_offset, line in enumerate(body.splitlines()):
            hit = next((h for h in LITERAL_OFFSET_RE.finditer(line) if _is_flagged(h)), None)
            if hit:
                line_no = body_start_line + line_offset
                problems.append(
                    f"{rel_path}:{line_no}: {label} uses a bare literal offset "
                    f"'{hit.group(0)}' instead of a KILNLINK_FRAME_A_OFF_*/"
                    f"KILNLINK_FRAME_A_LEN_* macro from kilnlink_frame_a_offsets.h "
                    f"-- this is exactly the hand-duplicated-offset-table bug "
                    f"ROADMAP M15 fixed; use the shared macro instead of a new "
                    f"literal"
                )

    if problems:
        print("FRAME A OFFSET DRIFT CHECK: FAILED")
        for p in problems:
            print(f"  {p}")
        return 1

    print(
        "FRAME A OFFSET DRIFT CHECK: no bare literal Frame A offsets found in any "
        "of the three sites (pack side, parse side, drift-test mirror) -- all "
        "three still index through kilnlink_frame_a_offsets.h's shared macros."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
