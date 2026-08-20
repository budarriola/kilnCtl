#!/usr/bin/env python3
"""check_scenarios.py -- validates every firmware/SimFW/scenarios/*.yaml
against docs/PLAN.md section 8.1's scenario schema, matching this repo's
other tools/check_*.ps1 grep-style CI checks in spirit (fail loudly, name the
offending file, exit non-zero) even though this one is Python rather than
PowerShell -- picked per this pass's own instructions because the real
validation logic (fault-type/guard-ID/trigger-kind cross-referencing against
the firmware source) is far more natural to express here than as regex, and
tools/PcTools already carries a pyyaml dependency for exactly this purpose
(kilnsim's own scenario.py loader).

Two layers of checking:

1. Reuses tools/PcTools/src/kilnsim/scenario.py's real loader
   (load_scenario_text) for structural validation -- required keys, trigger/
   duration/repeat shape, duplicate fault ids, dangling expect->fault-slot
   references. This is the same code path the PC-side MCP/CLI tooling uses,
   so "loads clean here" and "loads clean for kilnsim" are the same claim by
   construction, not two hand-maintained copies that could drift.

2. Adds firmware-source-aware checks scenario.py's loader does not (and
   should not -- it has no business reading C headers): every `exercises:`
   guard ID is a real SaftyFW guard, and every fault `type:` string maps to
   an actual firmware/SimFW/src/tasks/fault_sched.h FAULT_SCHED_TYPE_* value
   (extracted from the real enum by regex, not hand-copied, so this check
   fails the moment the enum and this script's mapping table drift apart
   instead of silently going stale).

Usage: python firmware/SimFW/tools/check_scenarios.py
   (or via the PcTools venv: tools/PcTools/.venv/Scripts/python.exe ...)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SIMFW_ROOT = REPO_ROOT / "firmware" / "SimFW"
SCENARIOS_DIR = SIMFW_ROOT / "scenarios"
FAULT_SCHED_H = SIMFW_ROOT / "src" / "tasks" / "fault_sched.h"
KILNSIM_SRC = REPO_ROOT / "tools" / "PcTools" / "src"

sys.path.insert(0, str(KILNSIM_SRC))

try:
    import yaml  # noqa: E402
except ImportError:
    print(
        "check_scenarios.py: pyyaml not importable on this interpreter -- "
        "run with tools/PcTools/.venv/Scripts/python.exe (see pyproject.toml, "
        "kilnsim depends on pyyaml>=6.0)",
        file=sys.stderr,
    )
    sys.exit(2)

try:
    from kilnsim.scenario import ScenarioError, load_scenario_text  # noqa: E402
except ImportError as exc:
    print(f"check_scenarios.py: could not import kilnsim.scenario: {exc}", file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# Valid guard IDs (SaftyFW/docs/SAFETY_MODEL.md section 4's guard suite).
# S6 covers both a/b sub-signals (SAFETY_MODEL.md: "Two independent signals,
# treated independently... must never be collapsed into one") -- S6a/S6b are
# valid alongside bare S6, which some scenarios use when either sub-signal
# would do (e.g. `exercises: [S6]` in power_blip.yaml covers both S6a and S6b
# being discussed in that file's own comments).
# ---------------------------------------------------------------------------
VALID_GUARD_IDS = {f"S{n}" for n in range(1, 14)} | {"S6a", "S6b"}

VALID_TRIGGER_KINDS = {
    "at_sim_time",
    "at_zone_temp",
    "on_relay_edge",
    "on_event",
    "after_fault",
    "random_in",
    "manual",
}

# ---------------------------------------------------------------------------
# YAML fault `type:` string -> firmware/SimFW/src/tasks/fault_sched.h enum
# value it is meant to compile to. This mapping is scenario-authoring
# vocabulary, not something the firmware or the PC-side loader encodes
# anywhere yet (payloads.py currently passes fault_type through as a raw
# byte, scenario.py's FaultSpec.type is an opaque string) -- so it lives
# here, and this script's job is to keep it honest against the real enum
# (see `_load_fault_sched_enum` below) rather than let it silently drift.
# ---------------------------------------------------------------------------
FAULT_TYPE_TO_ENUM = {
    "disconnected_tc": "FAULT_SCHED_TYPE_TC_DISCONNECTED",
    "flaky_noise_tc": "FAULT_SCHED_TYPE_TC_NOISE",
    "stuck_tc": "FAULT_SCHED_TYPE_TC_STUCK",
    "dead_tc_ic": "FAULT_SCHED_TYPE_TC_DEAD_IC",
    "flaky_spi_tc_ic": "FAULT_SCHED_TYPE_TC_FLAKY_SPI",
    "spurious_fault_pin": "FAULT_SCHED_TYPE_TC_SPURIOUS_FAULT_PIN",
    "shorted_tc": "FAULT_SCHED_TYPE_TC_SHORTED",
    "drifting_tc": "FAULT_SCHED_TYPE_TC_DRIFT",
    "cj_fault": "FAULT_SCHED_TYPE_TC_CJ_FAULT",
    "main_safety_disagree": "FAULT_SCHED_TYPE_MAIN_SAFETY_DISAGREE",
    "welded_ssr": "FAULT_SCHED_TYPE_WELDED_RELAY",
    "stuck_open_relay": "FAULT_SCHED_TYPE_STUCK_OPEN_RELAY",
    "broken_heater_coil": "FAULT_SCHED_TYPE_BROKEN_ELEMENT",
    "partial_element_health": "FAULT_SCHED_TYPE_PARTIAL_ELEMENT",
    "half_wave_ssr": "FAULT_SCHED_TYPE_HALF_WAVE_SSR",
    "phase_loss": "FAULT_SCHED_TYPE_PHASE_LOSS",
    "welded_k4_current_persist": "FAULT_SCHED_TYPE_WELDED_K4_CURRENT_PERSIST",
    "estop_trip": "FAULT_SCHED_TYPE_ESTOP",
    "runaway_zone": "FAULT_SCHED_TYPE_RUNAWAY_ZONE",
    "ambient_shift": "FAULT_SCHED_TYPE_AMBIENT_SHIFT",
    "thermal_mass_surprise": "FAULT_SCHED_TYPE_THERMAL_MASS_SURPRISE",
    "tc_lag_stress": "FAULT_SCHED_TYPE_TC_LAG_STRESS",
    "dut_power_cut": "FAULT_SCHED_TYPE_DUT_POWER_CUT",
}


def _load_fault_sched_enum() -> set[str]:
    """Extract the real FAULT_SCHED_TYPE_* enum member names straight out of
    fault_sched.h -- read-only, this script never modifies SimFW's src/."""
    if not FAULT_SCHED_H.exists():
        raise SystemExit(f"check_scenarios.py: fault_sched.h not found at {FAULT_SCHED_H}")
    text = FAULT_SCHED_H.read_text(encoding="utf-8")
    return set(re.findall(r"\bFAULT_SCHED_TYPE_[A-Z0-9_]+\b", text))


def main() -> int:
    if not SCENARIOS_DIR.is_dir():
        print(f"check_scenarios.py: scenarios dir not found at {SCENARIOS_DIR}", file=sys.stderr)
        return 2

    real_enum = _load_fault_sched_enum()

    # Sanity-check the mapping table itself against the real enum first --
    # if this table has drifted (stale entry, typo), every scenario using it
    # would otherwise fail with a confusing "unknown fault type" instead of
    # pointing at the actual problem: this table.
    table_failures = []
    for yaml_name, enum_name in FAULT_TYPE_TO_ENUM.items():
        if enum_name not in real_enum:
            table_failures.append(
                f"FAULT_TYPE_TO_ENUM[{yaml_name!r}] = {enum_name!r}, which is not a real "
                f"FAULT_SCHED_TYPE_* value in {FAULT_SCHED_H}"
            )
    if table_failures:
        print("check_scenarios.py: fault-type mapping table is stale:", file=sys.stderr)
        for f in table_failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    scenario_files = sorted(SCENARIOS_DIR.glob("*.yaml"))
    if not scenario_files:
        print(f"check_scenarios.py: no *.yaml files found under {SCENARIOS_DIR}", file=sys.stderr)
        return 2

    failures: list[str] = []
    loaded = 0

    for path in scenario_files:
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = path.read_text(encoding="utf-8")

        # --- Layer 1: the real kilnsim loader (structural validation) -----
        try:
            scenario = load_scenario_text(text, source_path=path)
        except ScenarioError as exc:
            failures.append(f"{rel}: kilnsim scenario loader rejected this file: {exc}")
            continue
        loaded += 1

        # PLAN.md sec 8.1's required top-level keys beyond what the loader
        # itself enforces (name/version only) -- preset/timescale/seed/dut/
        # faults/expect/report_keep are all part of the documented shape.
        raw = scenario.raw
        for key in ("preset", "timescale", "seed", "dut", "faults", "expect", "report_keep"):
            if key not in raw:
                failures.append(f"{rel}: missing top-level key {key!r} required by PLAN.md sec 8.1's schema")

        if "exercises" not in raw:
            failures.append(f"{rel}: missing 'exercises' key (guard cross-reference, PLAN.md sec 8.1)")

        # --- Layer 2: guard IDs -------------------------------------------
        for guard in scenario.exercises:
            if guard not in VALID_GUARD_IDS:
                failures.append(
                    f"{rel}: exercises: lists {guard!r}, not a real SaftyFW guard ID "
                    f"(valid: {sorted(VALID_GUARD_IDS)})"
                )

        # Guard IDs also show up inside expect clauses' event bodies
        # ({type: guard_warn/guard_trip, guard: SN}) -- catch a typo there
        # too, not just in the summary `exercises:` list.
        for e in scenario.expect:
            for body in (getattr(e, "event", None), getattr(e, "then", {}).get("event") if hasattr(e, "then") else None):
                if isinstance(body, dict) and body.get("type") in ("guard_warn", "guard_trip"):
                    guard = body.get("guard")
                    if guard is not None and guard not in VALID_GUARD_IDS:
                        failures.append(
                            f"{rel}: expect {getattr(e, 'name', '?')!r} references guard {guard!r}, "
                            f"not a real SaftyFW guard ID"
                        )

        # --- Layer 2: fault type + trigger kind cross-check ----------------
        for f in scenario.faults:
            if f.type not in FAULT_TYPE_TO_ENUM:
                failures.append(
                    f"{rel}: fault {f.id!r} has type {f.type!r}, which is not in this script's "
                    f"known fault-type vocabulary (FAULT_TYPE_TO_ENUM) -- add it there once "
                    f"fault_sched.h actually implements it, or fix the typo"
                )
            elif FAULT_TYPE_TO_ENUM[f.type] not in real_enum:
                # Already caught by the table sanity-check above, but keep
                # this here too so a per-scenario error message is specific.
                failures.append(
                    f"{rel}: fault {f.id!r} type {f.type!r} maps to "
                    f"{FAULT_TYPE_TO_ENUM[f.type]!r}, which fault_sched.h does not implement"
                )

            trigger_kind = f.trigger.kind.value if hasattr(f.trigger.kind, "value") else str(f.trigger.kind)
            if trigger_kind not in VALID_TRIGGER_KINDS:
                failures.append(
                    f"{rel}: fault {f.id!r} trigger kind {trigger_kind!r} is not one of "
                    f"PLAN.md sec 7.2's trigger kinds ({sorted(VALID_TRIGGER_KINDS)})"
                )

    if failures:
        print("SCENARIO VALIDATION FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print(
            f"\n{len(failures)} scenario violation(s) found across {len(scenario_files)} file(s) "
            "-- see docs/PLAN.md section 8.1",
            file=sys.stderr,
        )
        return 1

    print(
        f"Scenario validation passed: {loaded}/{len(scenario_files)} scenario file(s) loaded clean "
        f"via kilnsim's real loader, every exercises:/guard: guard ID is real, every fault type maps "
        f"to an implemented fault_sched.h enum value, every trigger kind is valid."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
