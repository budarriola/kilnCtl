#!/usr/bin/env python3
"""Confirms the partition table actually flashed on the board matches the
repo's ``firmware/KilnFW/partitions.csv`` -- FLASH_BUDGET_PLAN.md section 8
item 3.

``flash_firmware()`` reports "flashed and verified OK (bootloader +
partition table + app)" after a program, but that is OpenOCD's own
byte-compare during the flash operation, not something re-checkable later --
and nothing reachable over this board's HTTP surface reports the on-chip
partition table at all. ``check_flash_partition_map.ps1`` only validates the
CSV; it never touches hardware. This script is the missing independent
confirmation: it reads the partition-table bytes back off the chip over
JTAG (same OpenOCD/``debug_probe`` substrate every other debug tool in this
repo uses -- never esptool, per CLAUDE.md) and diffs them against the CSV
entry by entry.

Read-only. Halts the ESP core for the ~1-2s the JTAG memory read takes, then
resumes it -- same as any other ``debug_read_memory`` call. Safe to run
while the board is idle and powered (nothing is mid-firing, PID loop, or
autotune); do not run it while a fire profile is active, same caveat as any
other debug_* JTAG operation (it briefly stops relay control and telemetry).

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/check_chip_partition_table.py
    ... --csv path/to/other/partitions.csv
    ... --peer esp --address 0x8000 --size 0x1000

Exit status is 0 on a match, 1 on any mismatch or read/parse error.
"""
from __future__ import annotations

import argparse
import sys

from kilnctrl import partition_table


def _int_arg(text: str) -> int:
    return int(text, 0)  # accepts "0x8000" or "32768"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--peer", default="esp", help="debug_probe peer to read from (default: esp)")
    parser.add_argument("--csv", dest="csv_path", default=None,
                        help="partitions.csv to diff against (default: firmware/KilnFW/partitions.csv)")
    parser.add_argument("--address", type=_int_arg, default=partition_table.DEFAULT_TABLE_ADDRESS,
                        help="flash address of the partition table (default: 0x8000)")
    parser.add_argument("--size", type=_int_arg, default=partition_table.DEFAULT_TABLE_SIZE,
                        help="bytes to read (default: 0x1000, one flash sector)")
    args = parser.parse_args(argv)

    try:
        diff, chip_entries, csv_entries = partition_table.check_chip_partition_table(
            peer=args.peer, csv_path=args.csv_path, address=args.address, size=args.size,
        )
    except (ValueError, RuntimeError, FileNotFoundError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"Read {len(chip_entries)} entries from {args.peer} flash at 0x{args.address:x}, "
          f"{len(csv_entries)} entries from CSV.")
    print(diff.report())
    return 0 if diff.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
