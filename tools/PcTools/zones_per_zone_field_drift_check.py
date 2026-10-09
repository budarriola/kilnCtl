#!/usr/bin/env python3
"""Standalone entry point for zones_per_zone_field_table_checks() (see
selfcheck_zones_fields.py's own header for the full rationale and extraction
strategy) -- wired into tools/run_all_checks.ps1's recursive check_*.ps1
discovery via check_zones_per_zone_field_drift.ps1 in this directory, so a
firmware zones field added without a matching zones_http_client.py entry
fails the standard check sweep, not just an ad hoc selfcheck.py run.

Usage: python tools/PcTools/zones_per_zone_field_drift_check.py
Exit 0 = clean, 1 = drift found (see stdout for which field(s)).
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

import selfcheck_common
from selfcheck_zones_fields import zones_per_zone_field_table_checks


def main() -> int:
    zones_per_zone_field_table_checks()
    if selfcheck_common._failures:
        print("\nFAILED:")
        for f in selfcheck_common._failures:
            print(f"  - {f}")
        return 1
    print("\nzones per-zone field table: no drift from firmware.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
