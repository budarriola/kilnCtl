#!/usr/bin/env python3
r"""Writes a stack-margin baseline record (docs/BENCH_TEST_SYSTEM_PLAN.md
SK-01/SK-02's committed-baseline directory, ``docs/stack_margin_baseline/``)
from an already-captured ``get_stack_margin()`` report, with NO board access
of its own.

This is the counterpart to ``capture_stack_margin_baseline.py`` for a caller
that must stay off the wire (e.g. an agent restricted to the read-only
``kiln_call(name="get_stack_margin")`` MCP facade while a firing is in
progress and must not open a second, competing connection to capture a
baseline the ordinary way). Everything this script does is pure text
parsing and file writing -- see ``kilnctrl.stack_margin_baseline.
parse_stack_margin_report_text`` for the actual parse, unit-tested against
fabricated report text, never a real board.

Usage: paste the exact ``result`` string a ``kiln_call(name="get_stack_margin")``
call returned into a text file, then:

    tools\PcTools\.venv\Scripts\python.exe tools\PcTools\scripts\write_stack_margin_baseline_from_text.py \
        --report-file report.txt --condition mid_firing --commit 75a5e459 \
        --notes "captured via read-only kiln_call while a firing was active"

``--fw-built``/``--fw-dirty`` are optional context, not re-verified here --
this script never queries the board, so it cannot confirm them itself; the
caller is responsible for having them right (or leaving them blank/"unknown").
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from kilnctrl.devices_info import FirmwareVersion  # noqa: E402
from kilnctrl.stack_margin_baseline import (  # noqa: E402
    LOAD_CONDITIONS,
    build_record,
    parse_stack_margin_report_text,
    write_record,
)

DEFAULT_OUT_DIR = Path(__file__).resolve().parents[3] / "docs" / "stack_margin_baseline"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report-file", type=Path, required=True, help="file holding the raw get_stack_margin() result text")
    ap.add_argument("--condition", required=True, choices=LOAD_CONDITIONS)
    ap.add_argument("--commit", required=True, help="ESP firmware git commit this reading was taken against")
    ap.add_argument("--fw-built", default="unknown", help="ESP firmware build timestamp, if known")
    ap.add_argument("--fw-dirty", action="store_true", help="pass if the firmware tree was known dirty at build time")
    ap.add_argument("--protocol-version", type=int, default=0, help="UART protocol version, if known (not load-bearing for this record)")
    ap.add_argument("--notes", default="")
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    args = ap.parse_args()

    text = args.report_file.read_text(encoding="utf-8")
    entries = parse_stack_margin_report_text(text)
    fw_version = FirmwareVersion(
        protocol_version=args.protocol_version,
        dirty=args.fw_dirty,
        commit=args.commit,
        built=args.fw_built,
    )
    record = build_record(args.condition, entries, fw_version, notes=args.notes)
    path = write_record(record, args.out_dir)
    print(f"parsed {len(entries)} task(s), wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
