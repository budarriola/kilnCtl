#!/usr/bin/env python3
"""Confirms the partition table the RUNNING firmware is actually using
matches the repo's ``firmware/KilnFW/partitions.csv`` -- FLASH_BUDGET.md
section 8 item 3.

``flash_firmware()`` reports "flashed and verified OK (bootloader +
partition table + app)" after a program, but that is OpenOCD's own
byte-compare during the flash operation, not independently re-checkable
later, and ``check_flash_partition_map.ps1`` only validates the CSV; it
never touches hardware.

ORIGINAL APPROACH (no longer used): read the partition-table bytes back off
the chip over JTAG at flash offset 0x8000. Confirmed broken against the real
board -- 0x8000 is a FLASH offset, not a memory-mapped address OpenOCD's
``read_memory`` can reach ("failed to read 4096 B from esp flash at 0x8000"
/ "failed to read memory"). See ``kilnctrl.partition_table``'s module
docstring for the full writeup.

CURRENT APPROACH: GET /api/partitions from the board's own HTTP server
(``partition_info_http.c``), which reports the live table ESP-IDF's
``esp_partition`` iterator already parsed for the running app. Not a JTAG
operation -- no core halt, safe to run even while a fire profile is active.

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/check_chip_partition_table.py
    ... --host 192.168.1.156
    ... --csv path/to/other/partitions.csv

Exit status is 0 on a match, 1 on any mismatch or read/parse error.
"""
from __future__ import annotations

import argparse
import sys

from kilnctrl import partition_http_client, partition_table


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=partition_http_client.PARTITION_AP_DEFAULT_HOST,
                        help="board host/IP to GET /api/partitions from "
                             f"(default: {partition_http_client.PARTITION_AP_DEFAULT_HOST}, the fallback AP)")
    parser.add_argument("--csv", dest="csv_path", default=None,
                        help="partitions.csv to diff against (default: firmware/KilnFW/partitions.csv)")
    args = parser.parse_args(argv)

    try:
        diff, chip_entries, csv_entries = partition_table.check_chip_partition_table_via_http(
            host=args.host, csv_path=args.csv_path,
        )
    except (ValueError, RuntimeError, FileNotFoundError) as exc:
        print(f"error: {exc} (host={args.host})", file=sys.stderr)
        return 1

    print(f"Read {len(chip_entries)} entries from {args.host} GET /api/partitions, "
          f"{len(csv_entries)} entries from CSV.")
    print(diff.report())
    return 0 if diff.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
