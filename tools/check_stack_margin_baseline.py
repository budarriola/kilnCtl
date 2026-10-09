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

  4. No entry names a task as OK (cleared) in KNOWN_LOW_ALLOWLIST using ONLY
     idle-condition evidence. An idle capture is a FLOOR (stack_margin_
     baseline.py's own docstring: "the SMALLEST plausible worst case"), so an
     idle-only "OK" for a task whose known deep call chain runs only under a
     firing/autotune is not evidence the task is safe -- it is evidence the
     deep chain never executed. See check #4 below and CHECK_4_REQUIRES_LOAD.

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
  - Anything about WHICH load condition was captured, BEYOND what a file's
    own structured `load` field (kilnctrl.stack_margin_baseline.LoadSnapshot
    -- firing_active/autotune_active/zones_heating/observations, read from
    the board's own ProfileExecStatus/AutotuneStatus, never a caller flag)
    says. A single "idle" capture (this repo's first checked-in one) is, by
    tools/PcTools/src/kilnctrl/stack_margin_baseline.py's own docstring, a
    floor -- "the SMALLEST plausible worst case" -- not proof a task
    survives mid_firing or web_ui_open. This script does not enforce that
    all three DRAM_PSRAM_STATUS.md conditions have ever been captured; it only
    grades whatever baseline files exist, now load-aware via check #4.
  - A task that was never registered with stack_margin_register() in the
    first place (see tools/check_stack_margin_registration.ps1, a
    different, source-text-only check for that gap).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# Mirrors App/drivers/common/stack_margin_calc.h's STACK_MARGIN_CRITICAL_PCT/
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
    # (stack_margin_idle_2bcdc2d_20260904T215500Z.json). Investigated
    # 2026-09-04 (opus review) -- the "no known deep call chain" note this
    # allowlist originally carried was WRONG for both:
    #
    #   system_uart_bridge (3072 B configured): reachable path
    #   SYSTEM_CMD_FACTORY_RESET -> factory_reset_execute() ->
    #   execute_scope() -> ESP-IDF's nvs_flash_erase_partition(), a library
    #   call whose internal stack cost has never been measured. The idle
    #   capture that produced this LOW reading never took that path (no
    #   factory-reset command was in flight), so its 900 B free / 28.8%
    #   headroom number says nothing about that chain's real cost.
    #
    #   telemetry_log (4096 B configured, PSRAM stack): holds a 320-byte
    #   `line` buffer and, ONLY while a firing or autotune is active, chains
    #   several float snprintf() calls per zone plus the longest format
    #   string in the file on AUTOTUNE_ENGINE_DONE. The idle capture ran
    #   with observations=0 (no firing, no autotune), so that deep chain
    #   never executed either -- the 1064 B free / 25.6% headroom number is
    #   a floor, not this task's worst case.
    #
    # Both stay LOW-allowlisted (not resized -- an unmeasured increase just
    # moves memory around) until a baseline captured with `load.firing_active`
    # or `load.autotune_active` true exists for each. check_file()'s check #4
    # below refuses to let either be marked OK using idle-only evidence, so a
    # future capture cannot silently "clear" them without actually exercising
    # the deep chain.
    "system_uart_bridge": "LOW under idle; real chain is SYSTEM_CMD_FACTORY_RESET -> nvs_flash_erase_partition(), unmeasured -- needs a mid_firing/web_ui_open (or a captured factory-reset) load baseline before trusting this number long-term",
    "telemetry_log": "LOW under idle (observations=0); real chain is the per-zone/AUTOTUNE_ENGINE_DONE snprintf cascade, only reachable with load.firing_active or load.autotune_active true -- needs a mid_firing baseline before trusting this number long-term",
}

# Tasks whose allowlist reason above documents a chain reachable only when
# load.firing_active or load.autotune_active is true. Check #4 (see
# check_file()) refuses to accept an idle-only "OK" reading for these as
# clearing them off the allowlist -- listed separately from
# KNOWN_LOW_ALLOWLIST's keys so a name can be dropped from the allowlist
# (task fixed/resized) without silently also dropping this requirement, and
# vice versa, until both are updated deliberately together.
REQUIRES_LOAD_EVIDENCE_TO_CLEAR = frozenset(KNOWN_LOW_ALLOWLIST)


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


def _file_load_is_idle(data: dict) -> bool:
    """True if this file's structured `load` (kilnctrl.stack_margin_baseline.
    LoadSnapshot) says nothing was actually running, i.e. it cannot exercise
    any deep call chain gated on a firing/autotune/heating zone. Falls back
    to the freeform `condition` string for a baseline captured before `load`
    existed -- `condition == "idle"` is the only legacy signal available, and
    treating anything else as "unknown, assume loaded" avoids a false
    positive against an old mid_firing/web_ui_open file that never got the
    new field backfilled."""
    load = data.get("load")
    if isinstance(load, dict) and "firing_active" in load:
        return (
            not load.get("firing_active")
            and not load.get("autotune_active")
            and int(load.get("zones_heating", 0) or 0) == 0
        )
    return data.get("condition") == "idle"


def _file_has_load_evidence(data: dict) -> bool:
    """True if this file's `load` shows a firing or autotune actually
    running -- the minimum needed for a reading in it to say anything about
    a load-gated deep call chain. The inverse of _file_load_is_idle() for a
    file that HAS structured load data; a legacy file with no `load` key
    never counts as evidence (its `condition` string is caller-asserted, the
    exact thing this whole feature exists to stop trusting)."""
    load = data.get("load")
    if isinstance(load, dict) and "firing_active" in load:
        return bool(load.get("firing_active")) or bool(load.get("autotune_active"))
    return False


def check_file(path: Path, has_load_evidence: "set[str]") -> list[str]:
    """Returns a list of failure strings (empty means the file is clean).

    ``has_load_evidence`` is the set of task names for which SOME baseline
    file (any of them, not necessarily this one) shows load.firing_active or
    load.autotune_active true -- see check #4 below and main()'s first pass
    that computes it."""
    failures: list[str] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 -- report, don't crash the whole run
        return [f"{path}: could not parse as JSON ({exc})"]

    entries = data.get("entries")
    if not isinstance(entries, list) or not entries:
        return [f"{path}: no 'entries' list -- an empty/malformed baseline proves nothing"]

    file_is_idle = _file_load_is_idle(data)

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
        elif (
            recomputed == "OK"
            and name in REQUIRES_LOAD_EVIDENCE_TO_CLEAR
            and file_is_idle
            and name not in has_load_evidence
        ):
            # Check #4: a task on the LOW allowlist because its known deep
            # call chain only runs under load must not be waved through as
            # OK/cleared by an idle-only reading -- exactly the "idle
            # capture mistaken for a worst case" bug this whole feature
            # exists to close. This does not fail merely because a LOW
            # allowlisted task is idle-LOW (that is the expected, honest
            # state today); it fails only when someone tries to report the
            # SAME task OK using ONLY idle evidence anywhere in the checked-in
            # set, which would silently look like the task got fixed when
            # nothing exercised the chain that made it risky.
            failures.append(
                f"{path}: entry '{name}' reads OK ({hwm}B free of {configured}B) but this "
                f"capture's load is idle-only (no firing_active, no autotune_active, no "
                f"zone heating) and no OTHER checked-in baseline shows load evidence for "
                f"'{name}' either -- KNOWN_LOW_ALLOWLIST documents a deep call chain for "
                f"this task that only runs under a firing/autotune, so an idle 'OK' proves "
                f"the chain never executed, not that the task is safe. Capture a "
                f"mid_firing/web_ui_open baseline (or one taken while the relevant command "
                f"is actually in flight) before treating this as clearing the allowlist entry."
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

    # First pass: which tasks have SOME checked-in baseline showing real load
    # (a firing or autotune actually running)? check_file()'s check #4 needs
    # this computed ACROSS every file, not just the one it's grading, since
    # the load evidence that clears a task might live in a different capture
    # than the OK reading being judged.
    has_load_evidence: "set[str]" = set()
    parsed: list[dict] = []
    for f in files:
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001 -- check_file() reports the parse failure properly below
            continue
        parsed.append(data)
        if _file_has_load_evidence(data):
            for e in data.get("entries", []):
                if e.get("alive", True):
                    has_load_evidence.add(e.get("name", ""))

    all_failures: list[str] = []
    idle_only_low_notes: list[str] = []
    for f, data in zip(files, parsed):
        all_failures.extend(check_file(f, has_load_evidence))
        if _file_load_is_idle(data):
            low_here = sorted(
                e.get("name")
                for e in data.get("entries", [])
                if e.get("alive", True) and e.get("level") == "LOW"
            )
            if low_here:
                idle_only_low_notes.append(f"{f.name}: {', '.join(low_here)}")

    if all_failures:
        print(f"FAILED: {len(all_failures)} problem(s) across {len(files)} baseline file(s):")
        for fail in all_failures:
            print(f"  - {fail}")
        return 1

    print(f"PASSED: {len(files)} baseline file(s) under {baseline_dir}, "
          f"{sum(len(json.loads(f.read_text(encoding='utf-8'))['entries']) for f in files)} "
          "total task entries, no CRITICAL and no unexplained LOW.")
    if idle_only_low_notes:
        print(
            "PROVISIONAL: the following LOW entries are backed ONLY by idle-condition "
            "captures (load.firing_active/autotune_active both false, or a legacy file with "
            "condition==\"idle\") -- allowlisted and passing, but NOT yet proven safe under "
            "the load their own known call chain actually needs. See KNOWN_LOW_ALLOWLIST's "
            "comments for what each chain requires:"
        )
        for note in idle_only_low_notes:
            print(f"  - {note}")
    print("NOTE: this only grades what was last captured and checked in -- see this script's "
          "own top comment for what it cannot catch (a regression since the last capture, an "
          "un-measured load condition). It is a gate on a stale number, not a live sensor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
