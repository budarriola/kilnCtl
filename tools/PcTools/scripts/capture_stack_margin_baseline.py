#!/usr/bin/env python3

WARMUP RULE (owner, 2026-10-01): capture a reference baseline only on a board
that has been up at least 10 minutes AND has already run
bench_test_run(suite="stack") once on that same boot. High-water marks keep
dropping during the first suite run on a boot (info_uart_bridge: 1624 B free
fresh-idle, 1496 B later), so a pre-suite capture produces false SK-01 FAILs.
Record the uptime and the warmup run dir in --notes.
"""Capture one DRAM_PSRAM_STATUS.md section 4.3/7 stack-margin baseline reading
from a live board and record it where the plan can cite it.

This is the "moment the board is reflashed" measurement step section 7's
cap-raise pass left as the next pass's first job: pull a full (paginated)
get_stack_margin() reading, tag it with the build commit, and write it to
firmware/KilnFW/docs/stack_margin_baseline/ so the plan's section 3.1 table
can be regenerated from real files instead of hand-copied numbers.

Run ONCE PER LOAD CONDITION -- DRAM_PSRAM_STATUS.md section 4.3 requires all
three before any candidate task's number means anything (see
kilnctrl.stack_margin_baseline's module docstring for why each matters):

    uv run --project tools/PcTools python tools/PcTools/scripts/capture_stack_margin_baseline.py idle
    ... (start a firing, let it run a few minutes, then:)
    uv run --project tools/PcTools python tools/PcTools/scripts/capture_stack_margin_baseline.py mid_firing --notes "profile X, zone1+zone2 heating"
    ... (open the dashboard in two browser tabs, leave them polling, then:)
    uv run --project tools/PcTools python tools/PcTools/scripts/capture_stack_margin_baseline.py web_ui_open

After all three are captured:

    uv run --project tools/PcTools python tools/PcTools/scripts/capture_stack_margin_baseline.py --report

prints the combined worst-case-across-conditions table in the exact shape
DRAM_PSRAM_STATUS.md section 3.1 uses, ready to paste in.

All the actual UART round trips happen in main() below; everything that
decides what a "record" is and how to render one lives in
kilnctrl.stack_margin_baseline, which has no board dependency at all and is
unit-tested against fabricated data (tools/PcTools/tests/
test_stack_margin_baseline.py) -- that split is what makes this procedure
verifiable without hardware. This script itself is not meant to be run by an
agent with no hardware access; the DO-NOT-CALL-THE-BOARD rule in effect while
writing/testing this applies to the module, not to a human operator running
this file later.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from kilnctrl import mcp_server as m  # noqa: E402
from kilnctrl.stack_margin_baseline import (  # noqa: E402
    LOAD_CONDITIONS,
    LoadSnapshot,
    build_record,
    load_records,
    render_markdown_table,
    write_record,
)


def _read_load_snapshot() -> LoadSnapshot:
    """Reads the board's OWN reported state -- never a caller-supplied flag,
    which is one typo from lying about what condition a capture was actually
    taken under (2026-09-04 finding: the first checked-in baseline's filename
    said "idle" but nothing structural on the file did). Uses the same
    ProfileExecStatus/AutotuneStatus UART queries the GUI and MCP tools
    already use -- both are read-only, no firing/config/reset side effects."""
    exec_status = m._profiles.get_exec_status()
    autotune_status = m._autotune.get_status()

    firing_active = exec_status.state in (1, 2)  # PROFILE_EXEC_RUNNING, PROFILE_EXEC_PAUSED
    autotune_active = autotune_status.state != 0  # AUTOTUNE_ENGINE_IDLE
    zones_heating = sum(1 for z in exec_status.zones if z.relay_commanded_on)
    observations = autotune_status.sample_count if autotune_active else 0

    return LoadSnapshot(
        firing_active=firing_active,
        autotune_active=autotune_active,
        zones_heating=zones_heating,
        observations=observations,
    )

DEFAULT_OUT_DIR = (
    Path(__file__).resolve().parents[3] / "firmware" / "KilnFW" / "docs" / "stack_margin_baseline"
)


def capture(condition: str, out_dir: Path, notes: str) -> Path:
    print(m.connect())
    entries = m._info.get_stack_margin()
    fw_version = m._info.get_fw_version()
    load = _read_load_snapshot()
    if condition == "idle" and not load.is_idle:
        print(
            f"WARNING: --condition idle was requested but the board reports "
            f"firing_active={load.firing_active} autotune_active={load.autotune_active} "
            f"zones_heating={load.zones_heating} -- this is NOT an idle capture. "
            f"Recording the board's real state in 'load' regardless of the requested label."
        )
    elif condition in ("mid_firing", "web_ui_open") and load.is_idle:
        print(
            f"WARNING: --condition {condition!r} was requested but the board reports "
            f"no firing, no autotune, and no zone heating -- this looks like an IDLE "
            f"capture mislabelled as loaded. Recording the board's real (idle) state in "
            f"'load' regardless of the requested label."
        )
    record = build_record(condition, entries, fw_version, notes=notes, load=load)
    path = write_record(record, out_dir)
    print(
        f"captured {len(entries)} task(s) under condition={condition!r}, fw={fw_version.describe()}, "
        f"load={load.to_json_dict()}"
    )
    print(f"wrote {path}")
    return path


def report(out_dir: Path) -> int:
    records = load_records(out_dir)
    if not records:
        print(f"no captures found in {out_dir}")
        return 1
    conditions_seen = {r.condition for r in records}
    missing = set(LOAD_CONDITIONS) - conditions_seen
    if missing:
        print(
            f"WARNING: only have {sorted(conditions_seen)} -- missing "
            f"{sorted(missing)}. DRAM_PSRAM_STATUS.md section 4.3 requires all "
            f"three before a candidate task's number is trustworthy; the table "
            f"below is still the worst-of-what's-here, not a full baseline.\n"
        )
    print(render_markdown_table(records))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "condition", nargs="?", choices=LOAD_CONDITIONS,
        help="load condition this capture is taken under (required unless --report)",
    )
    ap.add_argument("--notes", default="", help="freeform context (profile name, client count, etc.)")
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    ap.add_argument("--report", action="store_true", help="print the combined table instead of capturing")
    args = ap.parse_args()

    if args.report:
        return report(args.out_dir)

    if not args.condition:
        ap.error("condition is required unless --report is given")

    capture(args.condition, args.out_dir, args.notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
