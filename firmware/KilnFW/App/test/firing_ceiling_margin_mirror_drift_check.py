#!/usr/bin/env python3
"""firing_ceiling_margin_mirror_drift_check.py -- profile_executor_internal.h
mirrors SaftyFW's FIRING_MARGIN_C_DEFAULT (firmware/SaftyFW/src/safety_guards.c)
as PROFILE_EXECUTOR_FIRING_CEILING_MARGIN_C_MIRROR, so the ESP-side start-time
refusal in profile_executor_run.c (profile_firing_ceiling_would_trip_on_
start()) refuses a cool-down-only-profile-on-a-hot-kiln start using the SAME
100C margin the Pico's own S1 guard will actually apply after SET_FIRING_
CEILING (0x09) lands. CommonFW does not expose this constant (it is a Pico-
side safety param default, not part of the wire codec in kilnlink_ceiling.c/
.h, which only carries firing_max_c itself) -- there is no seam to share it
from a single definition, so it is duplicated by hand on the ESP side and
this check exists to catch the two values drifting apart silently.

WHAT IS COMPARED: the single float literal each `#define` assigns. Nothing
else about either definition (name, comment, surrounding code) is compared --
this is deliberately narrower than the extract-a-code-block technique
approach_rate_cap_mirror_drift_check.py/frame_a_offset_drift_check.py use,
because there is no shared arithmetic to normalize here, only one number that
must match exactly.

Usage: python firing_ceiling_margin_mirror_drift_check.py [repo_root]
Exit 0: both literals parse and are numerically equal.
Exit 1: either #define could not be found at all (fail closed -- same
        standing rule as every other mirror-drift check in this repo: a
        regex that stops matching its target is a failure, not a vacuous
        pass), or the two values differ.
"""
import re
import sys
from pathlib import Path

PROD_REL = "firmware/SaftyFW/src/safety_guards.c"
MIRROR_REL = "firmware/KilnFW/App/drivers/control/profile_executor_internal.h"

PROD_RE = re.compile(r"#define\s+FIRING_MARGIN_C_DEFAULT\s+([0-9.]+)f?\b")
MIRROR_RE = re.compile(r"#define\s+PROFILE_EXECUTOR_FIRING_CEILING_MARGIN_C_MIRROR\s+([0-9.]+)f?\b")


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    prod_path = repo_root / PROD_REL
    mirror_path = repo_root / MIRROR_REL

    for label, path in (("production", prod_path), ("mirror", mirror_path)):
        if not path.is_file():
            print("FIRING-CEILING-MARGIN MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    prod_text = prod_path.read_text(encoding="utf-8")
    mirror_text = mirror_path.read_text(encoding="utf-8")

    prod_match = PROD_RE.search(prod_text)
    if not prod_match:
        print("FIRING-CEILING-MARGIN MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate '#define FIRING_MARGIN_C_DEFAULT <value>' in {PROD_REL} --")
        print("  update this check's PROD_RE rather than letting it pass vacuously.")
        return 1

    mirror_match = MIRROR_RE.search(mirror_text)
    if not mirror_match:
        print("FIRING-CEILING-MARGIN MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate '#define PROFILE_EXECUTOR_FIRING_CEILING_MARGIN_C_MIRROR "
              f"<value>' in {MIRROR_REL} --")
        print("  update this check's MIRROR_RE rather than letting it pass vacuously.")
        return 1

    prod_val = float(prod_match.group(1))
    mirror_val = float(mirror_match.group(1))

    if prod_val != mirror_val:
        print("FIRING-CEILING-MARGIN MIRROR DRIFT CHECK: FAILED")
        print(f"  {PROD_REL}'s FIRING_MARGIN_C_DEFAULT is {prod_val}, but {MIRROR_REL}'s")
        print(f"  PROFILE_EXECUTOR_FIRING_CEILING_MARGIN_C_MIRROR is {mirror_val} -- the ESP-side")
        print("  start-time refusal is now checking against the wrong margin. Update the mirror")
        print("  to match the Pico's real S1 margin.")
        return 1

    print(f"FIRING-CEILING-MARGIN MIRROR DRIFT CHECK: OK (both sides agree: {prod_val})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
