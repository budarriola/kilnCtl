#!/usr/bin/env python3
"""check_app_image_size.py -- hard size gate for the KilnFW application image
(docs/GITHUB_RELEASE_UPDATE_PLAN.md section 3 / WP2, docs/OTA_SINGLE_SLOT_PLAN.md).

The `app` partition is 0x400000 (4 MiB) because the other 4 MiB became the
`stage` partition. KilnCtrl.bin must fit `app`. ESP-IDF's own check_sizes.py
does NOT gate this: it only hard-fails when a binary fits NO app-type
partition, and a build that overflowed `app` would still print only a warning
if it happened to fit another one. This script is the independent gate.

Two bounds, both enforced:
  1. the `app` row of --partitions-csv (default firmware/KilnFW/partitions.csv),
     read via kilnctrl.partition_table.parse_partitions_csv, so a shrunken row
     is caught; and
  2. a pinned ceiling APP_SIZE_CEILING = 0x400000, so quietly growing the row
     back past 4 MiB (which would overlap `stage`) is also a failure of the
     table pin, reported here and in check_flash_partition_map.ps1.

Exit codes (run_all_checks.ps1 contract): 0 PASS, 3 SKIP (no image built yet,
or no `app` row), 1 FAIL. Under -Fast (KILNCTL_CHECKS_FAST) a missing image is
reported SKIP-FAST, same as check_recovery_image_size.py.
"""
from __future__ import annotations

import argparse
import os
import sys

_FAST = bool(os.environ.get("KILNCTL_CHECKS_FAST"))

_REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
_PCTOOLS_SRC = os.path.join(_REPO_ROOT, "tools", "PcTools", "src")
if _PCTOOLS_SRC not in sys.path:
    sys.path.insert(0, _PCTOOLS_SRC)

DEFAULT_PARTITIONS_CSV = os.path.join(_REPO_ROOT, "firmware", "KilnFW", "partitions.csv")
DEFAULT_APP_BIN = os.path.join(_REPO_ROOT, "firmware", "KilnFW", "build", "KilnCtrl.bin")
DEFAULT_PARTITION_NAME = "app"
APP_SIZE_CEILING = 0x400000  # 4 MiB: the plan's fixed app size; the other half is `stage`
_CRITICAL_FREE_FRACTION = 0.05


def _skip(reason: str) -> int:
    print(f"SKIP: {reason}")
    return 3


def _fail(reason: str) -> int:
    print(f"FAIL: {reason}", file=sys.stderr)
    return 1


def run(partitions_csv: str, app_bin: str, partition_name: str, ceiling: int = APP_SIZE_CEILING) -> int:
    from kilnctrl.partition_table import parse_partitions_csv  # noqa: PLC0415

    if not os.path.isfile(partitions_csv):
        return _fail(f"partition table not found at {partitions_csv!r} -- repo layout changed?")
    try:
        entries = parse_partitions_csv(partitions_csv)
    except ValueError as exc:
        return _fail(f"could not parse {partitions_csv}: {exc}")

    matches = [e for e in entries if e.name == partition_name]
    if not matches:
        return _fail(f"no {partition_name!r} partition in {partitions_csv}")
    if len(matches) > 1:
        return _fail(f"{len(matches)} rows named {partition_name!r} in {partitions_csv} -- ambiguous table")
    row_size = matches[0].size

    if row_size > ceiling:
        return _fail(
            f"{partition_name!r} row is {row_size} B (0x{row_size:x}), larger than the pinned "
            f"{ceiling} B (0x{ceiling:x}) ceiling -- it would overlap `stage` "
            "(docs/GITHUB_RELEASE_UPDATE_PLAN.md section 3)."
        )
    bound = min(row_size, ceiling)

    if not os.path.isfile(app_bin):
        reason = f"no application image at {app_bin!r} yet (bound would be {bound} B)."
        if _FAST:
            print(f"SKIP-FAST: {reason}")
            return 3
        return _skip(reason)

    image_size = os.path.getsize(app_bin)
    if image_size > bound:
        over = image_size - bound
        return _fail(
            f"application image {app_bin} is {image_size} B (0x{image_size:x}), which OVERFLOWS the "
            f"{bound} B (0x{bound:x}) {partition_name!r} bound by {over} B (0x{over:x})."
        )

    free = bound - image_size
    frac = free / bound if bound else 0.0
    print(
        f"PASS: application image {app_bin} is {image_size} B (0x{image_size:x}); "
        f"{partition_name!r} bound {bound} B (0x{bound:x}); {free} B ({frac:.0%}) free."
    )
    if frac < _CRITICAL_FREE_FRACTION:
        print(f"Warning: the {partition_name!r} partition is nearly full ({frac:.0%} free)!")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--partitions-csv", default=DEFAULT_PARTITIONS_CSV)
    parser.add_argument("--app-bin", default=DEFAULT_APP_BIN)
    parser.add_argument("--partition-name", default=DEFAULT_PARTITION_NAME)
    parser.add_argument("--ceiling", type=lambda s: int(s, 0), default=APP_SIZE_CEILING)
    args = parser.parse_args()
    return run(args.partitions_csv, args.app_bin, args.partition_name, args.ceiling)


if __name__ == "__main__":
    sys.exit(main())
