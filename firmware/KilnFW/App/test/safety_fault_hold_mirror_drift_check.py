#!/usr/bin/env python3
"""safety_fault_hold_mirror_drift_check.py -- SAFETY_FAULT_MIN_HOLD_MS's own
comment (firmware/KilnFW/App/drivers/safety/safety_link.h) says it is derived
from the RP2040 safety processor's own debounce constant,
SAFTYFW_MAIN_FAULT_DEBOUNCE_MS (firmware/SaftyFW/src/debounce_policy.h): 300 =
200 + a 100 ms margin. The two firmware targets are built completely
separately (no shared header carries this timing value -- CommonFW only
carries the wire protocol), so nothing stops one side's constant from
changing without the other being revisited. This is exactly the "reset one
side of a pair" bug class named in CLAUDE.md, applied to a compile-time
constant rather than runtime state: same numeric-mirror-drift-check family as
approach_rate_cap_mirror_drift_check.py (read that file's docstring for the
general technique), just comparing two numbers instead of two code bodies.

WHAT IS COMPARED: SAFETY_FAULT_MIN_HOLD_MS (KilnFW) must equal
SAFTYFW_MAIN_FAULT_DEBOUNCE_MS (SaftyFW) plus a fixed 100 ms margin. If either
constant changes without the other being updated to match, this fails naming
both values -- never a silent pass.

Usage: python safety_fault_hold_mirror_drift_check.py [repo_root]
Exit 0: the documented relationship holds.
Exit 1: either constant could not be located at all (fail closed), or the
        relationship no longer holds.
"""
import re
import sys
from pathlib import Path

KILNFW_REL = "firmware/KilnFW/App/drivers/safety/safety_link.h"
SAFTYFW_REL = "firmware/SaftyFW/src/debounce_policy.h"

MARGIN_MS = 100

KILNFW_RE = re.compile(r"#define\s+SAFETY_FAULT_MIN_HOLD_MS\s+(\d+)u?")
SAFTYFW_RE = re.compile(r"#define\s+SAFTYFW_MAIN_FAULT_DEBOUNCE_MS\s+(\d+)u?")


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    kilnfw_path = repo_root / KILNFW_REL
    saftyfw_path = repo_root / SAFTYFW_REL

    for label, path in (("KilnFW", kilnfw_path), ("SaftyFW", saftyfw_path)):
        if not path.is_file():
            print("SAFETY FAULT HOLD MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    kilnfw_text = kilnfw_path.read_text(encoding="utf-8")
    saftyfw_text = saftyfw_path.read_text(encoding="utf-8")

    kilnfw_match = KILNFW_RE.search(kilnfw_text)
    if not kilnfw_match:
        print("SAFETY FAULT HOLD MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate SAFETY_FAULT_MIN_HOLD_MS in {KILNFW_REL} --")
        print("  update this check's KILNFW_RE rather than letting it pass vacuously.")
        return 1

    saftyfw_match = SAFTYFW_RE.search(saftyfw_text)
    if not saftyfw_match:
        print("SAFETY FAULT HOLD MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate SAFTYFW_MAIN_FAULT_DEBOUNCE_MS in {SAFTYFW_REL} --")
        print("  update this check's SAFTYFW_RE rather than letting it pass vacuously.")
        return 1

    hold_ms = int(kilnfw_match.group(1))
    debounce_ms = int(saftyfw_match.group(1))
    expected_hold_ms = debounce_ms + MARGIN_MS

    if hold_ms != expected_hold_ms:
        print("SAFETY FAULT HOLD MIRROR DRIFT CHECK: FAILED")
        print(f"  {KILNFW_REL}: SAFETY_FAULT_MIN_HOLD_MS = {hold_ms}")
        print(f"  {SAFTYFW_REL}: SAFTYFW_MAIN_FAULT_DEBOUNCE_MS = {debounce_ms}")
        print(f"  expected SAFETY_FAULT_MIN_HOLD_MS == {debounce_ms} + {MARGIN_MS} = {expected_hold_ms}")
        print("  One side changed without the other being revisited -- a fast ack could")
        print("  again starve the RP2040 safety processor's own debounce latch. Update")
        print("  SAFETY_FAULT_MIN_HOLD_MS (and its comment) to match.")
        return 1

    print(
        f"SAFETY FAULT HOLD MIRROR DRIFT CHECK: OK "
        f"(SAFETY_FAULT_MIN_HOLD_MS={hold_ms} == SAFTYFW_MAIN_FAULT_DEBOUNCE_MS={debounce_ms} + {MARGIN_MS})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
