"""check_readiness_gate_display_agreement -- the gate and the checklist must
not be able to disagree about a blocking item.

WHY THIS EXISTS
---------------
On 2026-09-09 the owner made four /api/readiness checklist items into a real
firing interlock (App/drivers/safety/readiness_gate.h). Before that the page
gated nothing; now `safety_trip`, `recovery_mode`, `crash_report` and
`estop_verified` each refuse a start.

That creates exactly the hazard CLAUDE.md's standing "reset one side of a
pair" note describes: two pieces of state -- what the operator SEES on the
readiness page, and what the board DOES at start time -- joined by a semantic
contract that nothing structural enforces. Written independently they drift,
and both failure directions are bad:

  * display green, gate blocking  -> "the board is broken, it just says no"
  * display red, gate allowing    -> a firing starts past a red safety item

The design closes it by construction: readiness_gate.h does not decide
anything itself, it asks the SAME readiness_*_status() pure predicate
readiness_http.c's JSON handler asks, and blocks iff that predicate says
READY_NOT_DONE. test_readiness_gate.c proves the resulting biconditional over
all 64 fact combinations.

This check guards the WIRING that test cannot see: that both files still route
each item's decision through the same predicate, and still call each item by
the same key. Change either side alone -- a new predicate on the display, an
open-coded `if (f->estop_verified)` in the gate, a renamed item key -- and
this fails.

WHAT IT DOES *NOT* CLAIM. It cannot prove the two agree at runtime (that is
test_readiness_gate.c's job) and it deliberately says nothing about the
advisory items (guard_max_temp, hardware, safety_context, cfg_fs, network,
commissioning): those are not gated, on purpose, and binding them here would
make adding an advisory item look like a safety change.

Negative-test it with --negative-test-drop-predicate <name>, which pretends
the named predicate is absent from the gate; that must report FAIL.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The item key -> predicate pairing, stated once, here. Both files are checked
# against it, so neither file is treated as the authority over the other: a
# change on either side alone fails.
PAIRS = {
    "recovery_mode": "readiness_recovery_mode_status",
    "safety_trip": "readiness_safety_trip_status",
    "crash_report": "readiness_crash_report_status",
    "estop_verified": "readiness_estop_verification_status",
    # 2026-09-14 owner decision: "the Pico ceiling must ALWAYS equal the
    # ESP's ... there should never be a way that the pico is not armed" --
    # divergence is a fault, not advisory, promoted into the gate the same
    # way estop_verified was on 2026-09-09.
    "safety_ceiling_match": "readiness_ceiling_match_status",
    # docs/PICO_AUTO_UPDATE.md owner decision: "on an unrecoverable
    # version mismatch the ESP refuses to fire until matched" -- a real
    # structural block, promoted into the gate the same way
    # safety_ceiling_match was on 2026-09-14.
    "pico_update": "readiness_pico_update_status",
    "ct_attribution": "readiness_ct_attribution_status",
    # Owner decision 2026-10-04 (M13 third-sweep follow-up): a guard-9 startup
    # failure blocks firing; the PC-link watchdog's stays advisory (inside the
    # "startup" item, not gated).
    "startup_guard9": "readiness_startup_guard9_status",
    "ct_leak_alarm": "readiness_ct_leak_alarm_status",
}

REPO_TEST_DIR = Path(__file__).resolve().parent
DRIVERS = REPO_TEST_DIR.parent / "drivers"
GATE_H = DRIVERS / "safety" / "readiness_gate.h"
READINESS_C = DRIVERS / "http" / "readiness_http.c"


def fail(msg: str) -> None:
    print(f"check_readiness_gate_display_agreement: FAIL -- {msg}")


def extract_evaluate_body(text: str) -> str | None:
    """The body of readiness_gate_evaluate(), by brace matching."""
    m = re.search(r"readiness_gate_evaluate\s*\([^)]*\)\s*\{", text, re.S)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1 : j]
    return None


def check_gate(drop: str | None) -> list[str]:
    """Every blocking branch in the gate must be `<predicate>(...) == READY_NOT_DONE`,
    the five predicates must all appear, and no OTHER readiness_*_status()
    predicate may appear (an advisory item silently promoted to blocking)."""
    problems: list[str] = []
    if not GATE_H.is_file():
        return [f"{GATE_H} is missing -- the firing interlock's decision has no source to check"]
    body = extract_evaluate_body(GATE_H.read_text(encoding="utf-8", errors="replace"))
    if body is None:
        return [f"could not find readiness_gate_evaluate()'s body in {GATE_H.name}"]

    called = set(re.findall(r"\b(readiness_\w*status)\s*\(", body))
    if drop:
        called.discard(drop)

    for key, pred in PAIRS.items():
        if pred not in called:
            problems.append(
                f"the gate no longer decides item '{key}' with {pred}() -- either it open-codes the "
                f"rule now (a second copy that will drift from the page) or the item stopped blocking"
            )
        # The comparison itself must be against READY_NOT_DONE, nothing else:
        # `!= READY_OK` would quietly start blocking on CANNOT_YET too.
        elif not re.search(
            re.escape(pred) + r"\s*\([^;]*?\)\s*==\s*READY_NOT_DONE", body, re.S
        ):
            problems.append(
                f"the gate calls {pred}() but does not block on `== READY_NOT_DONE` -- the gate must "
                f"block exactly when the page shows that item as not_done, no more and no less"
            )

    extra = called - set(PAIRS.values())
    for pred in sorted(extra):
        problems.append(
            f"the gate consults {pred}(), which is not one of the five items the owner made blocking "
            f"(2026-09-09/2026-09-14) -- adding an advisory item to the interlock is an owner decision, not a "
            f"refactor; update PAIRS here deliberately if that is what was intended"
        )
    return problems


def item_block_for_key(text: str, key: str) -> str | None:
    """The source of the readiness_http.c block that renders item `key`.

    Every item in that handler is written as a brace-delimited block ending in
    an append_item(..., "<key>", ...) call, so: find the call, walk backwards
    to the innermost enclosing `{`, and return what lies between.
    """
    m = re.search(r'append_item\s*\([^;]*?"' + re.escape(key) + r'"', text, re.S)
    if not m:
        return None
    depth = 0
    for j in range(m.start(), -1, -1):
        if text[j] == "}":
            depth += 1
        elif text[j] == "{":
            if depth == 0:
                return text[j + 1 : m.start()]
            depth -= 1
    return None


def check_display() -> list[str]:
    """Each gated item's rendered status must come from the same predicate."""
    problems: list[str] = []
    if not READINESS_C.is_file():
        return [f"{READINESS_C} is missing -- cannot confirm the page still agrees with the gate"]
    text = READINESS_C.read_text(encoding="utf-8", errors="replace")

    for key, pred in PAIRS.items():
        block = item_block_for_key(text, key)
        if block is None:
            problems.append(
                f"no append_item(..., \"{key}\", ...) call in {READINESS_C.name} -- the interlock "
                f"blocks on an item /api/readiness no longer reports, so an operator refused a firing "
                f"has nothing on the page to act on"
            )
            continue
        called = set(re.findall(r"\b(readiness_\w*status)\s*\(", block))
        if pred not in called:
            problems.append(
                f"item '{key}' is rendered without calling {pred}() -- the page and the firing "
                f"interlock would be deciding the same item two different ways (found: "
                f"{sorted(called) or 'no predicate call at all'})"
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--negative-test-drop-predicate",
        default=None,
        help="pretend this predicate is absent from the gate; proves this check can fail",
    )
    args = ap.parse_args()

    problems = check_gate(args.negative_test_drop_predicate) + check_display()
    if problems:
        for p in problems:
            fail(p)
        return 1

    print(
        "check_readiness_gate_display_agreement: OK -- "
        + ", ".join(f"{k} -> {v}()" for k, v in PAIRS.items())
        + " (one predicate each, shared by the gate and /api/readiness)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
