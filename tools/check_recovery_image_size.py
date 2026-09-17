#!/usr/bin/env python3
"""check_recovery_image_size.py -- hard build-time size gate for the OTA
recovery image (docs/OTA_SINGLE_SLOT_PLAN.md).

THE GAP THIS CLOSES. ESP-IDF's own `check_sizes.py` only hard-fails a build
when a binary is too large for EVERY matching `app`-type partition
(`components/partition_table/check_sizes.py`, `check_partition()`: the
`SystemExit` only fires when `len(partitions) == len(too_small_partitions)`,
around line 98; when the binary fits at least one `app` partition but not
all of them it falls through to `print('Warning: ' + msg)` and returns,
i.e. exit 0). Measured directly against the IDF version installed at
`C:\\esp\\v6.0.2\\esp-idf` and recorded in the plan: an 8 MiB `app` next to a
1,966,080-byte `recovery` (both type `app`, both matched together because
neither build path passes `--subtype`) builds an oversized `recovery` image
CLEAN, with only a non-fatal warning. That is precisely backwards for the
one image whose entire value is having almost no surface to fail on --
nothing upstream currently gates its size at all. This script is that gate.

WHAT EXISTS TODAY VS WHAT IS STILL PLANNED (checked at run time, not
assumed): as of 2026-09-16 neither half of the plan had landed --
`firmware/KilnFW/partitions.csv` had no `recovery` row yet (still the
dual-OTA-slot table), and there was no separate recovery-image IDF project
in the tree (`docs/OTA_SINGLE_SLOT_PLAN.md` section 8 listed both as future
steps: step 3 for the table, step 1 for the image). This script legitimately
SKIP'd in that state -- there was nothing yet to bound -- and was written
ahead of both landing precisely so the day either one landed the gate was
already live and enforcing, rather than being a follow-up someone had to
remember. See run_all_checks.ps1's SKIP contract: a skip must name its
reason and is NOT the same as a silent pass.

Both halves have since landed (partition row: `e88bd9ee`'s plan and its
2026-09-16 step-3 commit; `firmware/KilnFW_recovery/` itself: `5e07eeca`).
That closed one SKIP reason but exposed a second: nothing in provisioning
or the check suite ever actually BUILT `recovery.bin`, so a fresh clone
still had no artifact to measure and this script still SKIP'd, on every
machine, permanently -- a gap found and fixed 2026-09-17.
`firmware/KilnFW/App/test/check_00_kilnfw_recovery_target_build.ps1` now
builds `firmware/KilnFW_recovery` and publishes `recovery.bin` on any
machine with the ESP-IDF toolchain, so this script now normally PASSes with
a real measured figure. It still legitimately SKIPs (both reasons below
remain live code paths, not dead ones) on a machine that genuinely lacks
the ESP-IDF toolchain -- the same category of prerequisite-missing SKIP
`check_00_kilnfw_target_build.ps1` already uses for the main application
build, not a new or wider exemption.

WHERE THE BOUND COMES FROM. The byte bound is read directly out of
`--partitions-csv` (default `firmware/KilnFW/partitions.csv`) for the row
named `--partition-name` (default `recovery`), via
`kilnctrl.partition_table.parse_partitions_csv` -- the same parser
`debug_check_partition_table()`/`check_chip_partition_table_via_http()`
already use to read this file, so there is exactly one CSV parser for this
table in the repo, not a second hand-rolled one that could drift from it.
Nothing here hardcodes 1,966,080 or any other byte count: if the table's
`recovery` row is ever resized, this gate's bound moves with it on the next
run, by construction. (Contrast with the mirror-drift bug class this repo
keeps finding -- a hardcoded number nobody remembers to update when the
table changes.)

WHERE THE IMAGE COMES FROM. `docs/OTA_SINGLE_SLOT_PLAN.md` section 1 commits
to building the recovery image as a *separate* IDF project specifically so
its much smaller content list can never accidentally become a copy of the
main OTA app image -- so it will not appear under `firmware/KilnFW/build/`.
No such project exists in the tree yet, so `--recovery-bin`'s default
(`firmware/KilnFW_recovery/build/recovery.bin`) is this script's own
proposed convention, chosen only to mirror the existing
`firmware/KilnFW/build/KilnCtrl.bin` sibling-naming pattern -- it is NOT
settled elsewhere in the plan. Whoever lands section 8 step 1 should either
build to this path or pass `--recovery-bin` to match wherever it actually
lands; either way, update this default rather than leaving it to drift.

EXIT CODES (run_all_checks.ps1's check_*.ps1 contract):
    0  PASS -- recovery partition and image both exist, image fits.
    3  SKIP -- recovery partition row and/or the image do not exist yet.
       Always prints a line containing "SKIP" and the specific reason.
    1  FAIL -- the image exists and does NOT fit the recovery partition.
"""
from __future__ import annotations

import argparse
import os
import sys

_REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
_PCTOOLS_SRC = os.path.join(_REPO_ROOT, "tools", "PcTools", "src")
if _PCTOOLS_SRC not in sys.path:
    sys.path.insert(0, _PCTOOLS_SRC)

DEFAULT_PARTITIONS_CSV = os.path.join(_REPO_ROOT, "firmware", "KilnFW", "partitions.csv")
DEFAULT_RECOVERY_BIN = os.path.join(_REPO_ROOT, "firmware", "KilnFW_recovery", "build", "recovery.bin")
DEFAULT_PARTITION_NAME = "recovery"

# Same critical-headroom threshold check_sizes.py itself warns at
# (components/partition_table/check_sizes.py: free_size_relative_critical =
# 0.05), reproduced here (not imported -- that module lives in the IDF tree,
# not this repo) so a recovery image that technically fits but is nearly
# full is at least visible, not just silently green.
_CRITICAL_FREE_FRACTION = 0.05


def _skip(reason: str) -> int:
    print(f"SKIP: {reason}")
    return 3


def _fail(reason: str) -> int:
    print(f"FAIL: {reason}", file=sys.stderr)
    return 1


def run(partitions_csv: str, recovery_bin: str, partition_name: str) -> int:
    from kilnctrl.partition_table import parse_partitions_csv  # noqa: PLC0415

    if not os.path.isfile(partitions_csv):
        return _fail(f"partition table not found at {partitions_csv!r} -- repo layout changed?")

    try:
        entries = parse_partitions_csv(partitions_csv)
    except ValueError as exc:
        return _fail(f"could not parse {partitions_csv}: {exc}")

    matches = [e for e in entries if e.name == partition_name]
    if not matches:
        return _skip(
            f"no {partition_name!r} partition defined yet in {partitions_csv} -- "
            "see docs/OTA_SINGLE_SLOT_PLAN.md section 1 (table) / section 8 step 3 "
            "(landing it). This gate activates automatically once that row exists."
        )
    if len(matches) > 1:
        return _fail(f"{len(matches)} rows named {partition_name!r} in {partitions_csv} -- ambiguous table")

    entry = matches[0]
    bound = entry.size

    if not os.path.isfile(recovery_bin):
        return _skip(
            f"{partition_name!r} partition is defined ({bound} B / 0x{bound:x} at "
            f"{partitions_csv}) but no recovery image was found at {recovery_bin!r} yet -- "
            "see docs/OTA_SINGLE_SLOT_PLAN.md section 8 step 1 (building it). "
            "This gate activates automatically once that image is built at this path "
            "(or pass --recovery-bin to match wherever it actually lands)."
        )

    image_size = os.path.getsize(recovery_bin)

    if image_size > bound:
        overflow = image_size - bound
        return _fail(
            f"recovery image {recovery_bin} is {image_size} B (0x{image_size:x}), which "
            f"OVERFLOWS the {partition_name!r} partition's {bound} B (0x{bound:x}) bound "
            f"from {partitions_csv} by {overflow} B (0x{overflow:x}). "
            "This is exactly the case ESP-IDF's own check_sizes.py lets through silently "
            "(it only hard-fails when a binary is too large for EVERY matching app "
            "partition, not just this one) -- see this script's module docstring."
        )

    free = bound - image_size
    free_fraction = (free / bound) if bound else 0.0
    print(
        f"PASS: recovery image {recovery_bin} is {image_size} B (0x{image_size:x}). "
        f"{partition_name!r} partition is {bound} B (0x{bound:x}) per {partitions_csv}. "
        f"{free} B ({free_fraction:.0%}) free."
    )
    if free_fraction < _CRITICAL_FREE_FRACTION:
        print(
            f"Warning: the {partition_name!r} partition is nearly full "
            f"({free_fraction:.0%} free space left)!"
        )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--partitions-csv", default=DEFAULT_PARTITIONS_CSV)
    parser.add_argument("--recovery-bin", default=DEFAULT_RECOVERY_BIN)
    parser.add_argument("--partition-name", default=DEFAULT_PARTITION_NAME)
    args = parser.parse_args()
    return run(args.partitions_csv, args.recovery_bin, args.partition_name)


if __name__ == "__main__":
    sys.exit(main())
