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

WARMUP RULE (owner, 2026-10-01): a reference baseline may only be captured on
a board that has been up at least 10 minutes AND has already run
``bench_test_run(suite="stack")`` once on that same boot. High-water marks
keep dropping during the first suite run on a boot (info_uart_bridge read
1624 B free at a fresh-idle capture and 1496 B later on the same boot, a
128 B drop against the 64 B SK-01 tolerance), so a pre-suite capture produces
false SK-01 FAILs. The warmup must ALSO include at least one thermo_read, thermo_read_faults and
io_read call on that boot: the stack suite never sends UART bridge commands,
and thermo_uart_bridge / io_uart_bridge sit at a pristine high-water mark until
the first bridge command reaches them (reply buffer + uart_protocol_send/ACK
wait on the task stack), after which they read ~1 KB lower (thermo 2796 -> 1660 B,
io 2544 -> 1552 B free, confirmed 2026-10-01 on eb83c1ac).
The full warmup list, all sent on the capture boot: get_board_state,
thermo_read, thermo_read_faults, io_read, get_fw_version, get_pin_config,
several get_stack_margin, then the stack suite. info_uart_bridge drops are
not tied to any single info command (leading candidate: ESP_LOGW formatting
via uart_log_vprintf under log-queue pressure), so SK-01/SK-02 also carry a
per-task tolerance table (judgments.py
_STACK_MARGIN_TASK_TOLERANCE_OVERRIDES): thermo_uart_bridge, io_uart_bridge
and info_uart_bridge get 384 B, every other task keeps 64 B, and the
absolute floor and dead-task FAIL are unchanged. Measured spread behind 384
B: thermo_uart_bridge 1660 vs 1856 B free across two boots, an unexplained
176 B info_uart_bridge drop, and the ~1 KB first-command step.
Record the uptime, the warmup run dir and the bridge commands sent in the notes.
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
    notes_lc = args.notes.lower()
    if "warmup" not in notes_lc or not all(c in notes_lc for c in ("thermo_read", "thermo_read_faults", "io_read", "get_board_state", "get_fw_version", "get_pin_config")):
        print("WARNING: --notes does not mention a warmup and each of thermo_read, thermo_read_faults, io_read, get_board_state, get_fw_version, get_pin_config (owner rule 2026-10-01: capture only on a board up >= 10 min that has already run bench_test_run(suite=stack) once on this boot AND been sent at least one thermo_read, thermo_read_faults and io_read, since the stack suite never exercises the thermo/io UART bridge tasks; record uptime, the warmup run dir and the bridge commands sent).", file=sys.stderr)
    record = build_record(args.condition, entries, fw_version, notes=args.notes)
    path = write_record(record, args.out_dir)
    print(f"parsed {len(entries)} task(s), wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
