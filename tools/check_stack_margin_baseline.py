#!/usr/bin/env python3
"""Stack-depth tripwire -- opus review of the display path (UI_PLAN.md,
'Context rules for the display path' section), scan #3 of the three the
review specified. e7b8efc was a stack overflow because a deep call chain
(profile_executor_get_status()/dashboard_get_status()/autotune_engine_
is_active()) landed behind screen_idle_task's 20 Hz poll and nobody resized
its stack until the board corrupted its own heap on the bench. The other two
scans this pass added (test_display_power_wiring.c sections 7-9) are pure
source-text scans that run with no hardware at all; this one cannot be --
"a chain got deeper" is a fact about run-time stack usage, not about what the
source text says, so it needs a real uxTaskGetStackHighWaterMark() reading
from actual silicon.

WHAT THIS CHECKS, OFFLINE, WITH NO BOARD ATTACHED:
  1. Every checked-in baseline file's stored `level` actually matches what
     stack_margin_calc.h's own 15%/30% classification produces from its
     `hwm_bytes`/`configured_stack_bytes` pair -- catches a hand-edited or
     stale baseline file whose numbers and label disagree.
  2. No entry in ANY checked-in baseline is CRITICAL. A CRITICAL reading
     checked in and then silently tolerated forever is worse than not
     capturing a baseline at all.
  3. No entry is LOW unless it is named in KNOWN_LOW_ALLOWLIST below, with a
     comment saying why. A previously-OK task (lvgl, screen_idle, ...)
     showing up LOW in a freshly captured baseline fails loud, by name --
     that is the "chain got deeper and nobody resized it" case this scan
     exists for.

WHAT THIS CANNOT CATCH, AND WHY:
  - A regression that happened on hardware SINCE the last capture. This
    script only ever sees whatever JSON was last checked in under
    firmware/KilnFW/App/test/baselines/ -- it has no way to know today's
    real headroom on a board nobody has re-measured. It is a gate on a
    STALE number, not a live sensor. Re-running
    `kiln_call(name="get_stack_margin")` (kilnctrl MCP, read-only, no
    firing/config/reset needed) and writing a fresh baseline file is a
    manual step; nothing here reminds anyone to do it after a change that
    plausibly deepens one of these poll-tick call chains.
  - Anything about WHICH load condition was captured. A single "idle"
    capture (this repo's first checked-in one) is, by
    tools/PcTools/src/kilnctrl/stack_margin_baseline.py's own docstring, a
    floor -- "the SMALLEST plausible worst case" -- not proof a task
    survives mid_firing or web_ui_open. This script does not know or
    enforce that all three conditions have ever been captured; it only
    grades whatever baseline files exist.
  - A task that was never registered with stack_margin_register() in the
    first place (see tools/check_stack_margin_registration.ps1, a
    different, source-text-only check for that gap).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# Mirrors App/drivers/stack_margin_calc.h's STACK_MARGIN_CRITICAL_PCT/
# STACK_MARGIN_LOW_PCT exactly -- if that header's cut points are ever
# retuned "from what the bench actually shows" (its own comment's rule),
# this pair must move with it or check #1 above starts failing every
# baseline for a reason that has nothing to do with a real regression.
CRITICAL_PCT = 15
LOW_PCT = 30

# Tasks a checked-in baseline is allowed to show LOW today, and why. Adding a
# name here must cite a reason, the same way this codebase requires an
# explicit TODO for anything else source-scan-allowlisted (see
# test_display_power_wiring.c's own allowlist comments) -- silently growing
# this list defeats the whole point of check #3 in this file's docstring.
KNOWN_LOW_ALLOWLIST = {
    # Both first observed LOW in the 2026-09-04 idle capture
    # (stack_margin_idle_2bcdc2d_20260904T215500Z.json) captured for this
    # task. Neither has a known deep call chain behind it the way screen_
    # idle/lvgl do (system_uart_bridge/telemetry_log are plain UART framing
    # and ring-buffer writers) -- flagged here as pre-existing so this
    # script's own first run is not a false alarm, not because either has
    # been individually investigated and cleared. Follow-up: measure under
    # mid_firing/web_ui_open too (stack_margin_baseline.py's three
    # conditions) before trusting either number long-term.
    "system_uart_bridge": "pre-existing LOW at first capture, not this pass's regression -- not yet investigated",
    "telemetry_log": "pre-existing LOW at first capture, not this pass's regression -- not yet investigated",
}


def classify(hwm_bytes: int, configured_stack_bytes: int) -> str:
    if configured_stack_bytes <= 0:
        return "CRITICAL"
    if hwm_bytes > configured_stack_bytes:
        return "CRITICAL"
    pct = (hwm_bytes * 100) // configured_stack_bytes
    if pct < CRITICAL_PCT:
        return "CRITICAL"
    if pct < LOW_PCT:
        return "LOW"
    return "OK"


def check_file(path: Path) -> list[str]:
    """Returns a list of failure strings (empty means the file is clean)."""
    failures: list[str] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 -- report, don't crash the whole run
        return [f"{path}: could not parse as JSON ({exc})"]

    entries = data.get("entries")
    if not isinstance(entries, list) or not entries:
        return [f"{path}: no 'entries' list -- an empty/malformed baseline proves nothing"]

    for e in entries:
        name = e.get("name", "<unnamed>")
        hwm = e.get("hwm_bytes")
        configured = e.get("configured_stack_bytes")
        stored_level = e.get("level")
        alive = e.get("alive", True)

        if not alive:
            # A dead task's hwm/level are meaningless (stack_margin.c zeroes
            # them) -- nothing to classify, and NOT the same thing as OK.
            continue

        if not isinstance(hwm, int) or not isinstance(configured, int):
            failures.append(f"{path}: entry '{name}' has non-integer hwm_bytes/configured_stack_bytes")
            continue

        recomputed = classify(hwm, configured)
        if stored_level != recomputed:
            failures.append(
                f"{path}: entry '{name}' stores level={stored_level!r} but "
                f"hwm_bytes={hwm}/configured_stack_bytes={configured} classifies as "
                f"{recomputed} under stack_margin_calc.h's {CRITICAL_PCT}%/{LOW_PCT}% cut points -- "
                f"the baseline file disagrees with the arithmetic it is supposed to record."
            )
            continue  # don't double-report the same entry below

        if recomputed == "CRITICAL":
            failures.append(
                f"{path}: entry '{name}' is CRITICAL ({hwm}B free of {configured}B) -- "
                f"a CRITICAL reading must never be checked in and left; if this is a real "
                f"capture, that task needs a stack increase before this baseline is committed, "
                f"not an allowlist entry."
            )
        elif recomputed == "LOW" and name not in KNOWN_LOW_ALLOWLIST:
            failures.append(
                f"{path}: entry '{name}' is LOW ({hwm}B free of {configured}B, "
                f"{hwm * 100 // configured}% headroom) and is not in KNOWN_LOW_ALLOWLIST -- "
                f"if this is a NEW LOW (a task that was OK in an earlier baseline), this is "
                f"exactly 'a chain got deeper and nobody resized it' surfacing as intended; "
                f"either fix the stack size and recapture, or add '{name}' to the allowlist in "
                f"this script with a reason, the same way test_display_power_wiring.c's source "
                f"scans require an explicit TODO for an allowlisted violation."
            )

    return failures


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    baseline_dir = repo_root / "firmware" / "KilnFW" / "App" / "test" / "baselines"

    if not baseline_dir.is_dir():
        print(f"FAILED: baseline directory {baseline_dir} does not exist -- "
              "either it moved (update this script) or no baseline has ever been captured "
              "(see this script's own top comment for how: kiln_call(name=\"get_stack_margin\")).")
        return 2

    files = sorted(baseline_dir.glob("stack_margin_*.json"))
    if not files:
        print(f"FAILED: no stack_margin_*.json files found under {baseline_dir} -- "
              "this tripwire has nothing to check. A missing baseline is not the same as a "
              "passing one; capture at least one before trusting this script's exit code.")
        return 2

    all_failures: list[str] = []
    for f in files:
        all_failures.extend(check_file(f))

    if all_failures:
        print(f"FAILED: {len(all_failures)} problem(s) across {len(files)} baseline file(s):")
        for fail in all_failures:
            print(f"  - {fail}")
        return 1

    print(f"PASSED: {len(files)} baseline file(s) under {baseline_dir}, "
          f"{sum(len(json.loads(f.read_text(encoding='utf-8'))['entries']) for f in files)} "
          "total task entries, no CRITICAL and no unexplained LOW.")
    print("NOTE: this only grades what was last captured and checked in -- see this script's "
          "own top comment for what it cannot catch (a regression since the last capture, an "
          "un-measured load condition). It is a gate on a stale number, not a live sensor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
