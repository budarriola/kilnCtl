#!/usr/bin/env python3
"""approach_rate_cap_mirror_drift_check.py -- test_approach_rate_cap.c's own
header comment admits its cap_update_tick() is "a hand-written MIRROR" of the
per-zone approach-rate-cap update loop profile_executor.c's executor_task_
entry() runs every tick (the block added right before "Control mode, per
active zone (pass 1: decide, don't apply yet)"), because that loop lives
inside a real FreeRTOS task body with no seam to drive one tick at a time
from a host test.

Nothing was checking that the mirror still matches production -- if
profile_executor.c's cap arithmetic changes, the mirror keeps passing while
testing code that no longer ships. Same reasoning, and same
extract-normalize-diff technique, as frame_a_offset_drift_check.py and
power_diag_flag_mirror_drift_check.py (read those first): this is a static
diff, not a test, because "does the mirror still match" is not observable
from either side's own tests passing.

WHAT IS COMPARED: both sides' cap arithmetic reduces to the same core once
the two structurally-necessary differences are normalized away:
  - production reads cap_c_per_hr from zones_config_get_approach_rate_cap_c_
    per_hr() and reaches this code via a `for` loop over zones with an
    early `continue`; the mirror takes cap_c_per_hr as a parameter and
    reaches this code via a plain function with an early `return`. Both
    per-zone active/faulted gating and the cap lookup are stripped from
    production's side, and the return/continue keywords are folded to one
    token, before comparing.
  - production reads the shared target from `s_exec.target_c`; the mirror
    takes it as a `shared_target_c` parameter. Both are folded to one token.
Everything else -- the uncapped snap, the NAN/non-finite snap, the max_step_c
computation, the two-sided clamp, and the final `+=` -- must be BYTE
IDENTICAL (modulo whitespace and comments) after that folding, or this FAILS
naming the first differing normalized line on each side.

Usage: python approach_rate_cap_mirror_drift_check.py [repo_root]
Exit 0: the two normalized fragments match.
Exit 1: they differ, or either fragment could not be located at all (fail
        closed -- a regex that stops matching its target is a failure, not a
        vacuous pass, per this repo's own standing rule for this class of
        check).
"""
import re
import sys
from pathlib import Path

PROD_REL = "firmware/KilnFW/App/drivers/profile_executor.c"
MIRROR_REL = "firmware/KilnFW/App/test/test_approach_rate_cap.c"

# profile_executor.c has several loops over MAX31856_CHANNEL_COUNT (the
# want_relay_on pass, the guard_cap_c_per_hr read near line 1123), so a plain
# regex anchored only on the loop header risks a non-greedy `.*?` backtracking
# all the way past an EARLIER, unrelated for-loop to reach a later end anchor
# -- exactly the failure this check hit in development (it silently matched
# from a mode-invariant-check loop hundreds of lines away through to this
# block's own end anchor). Anchoring instead on the one line this block's
# final statement is known to be unique in the file (checked by
# _find_prod_body() below) and walking backward to the nearest enclosing
# `for` is immune to that.
PROD_END_ANCHOR = "z->effective_target_c += delta_c;"
PROD_FOR_HEADER = "for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {\n"


def find_prod_body(text: str):
    """Returns the loop body text (between the for-header's '{' and its
    matching close), or None if the anchor is not found exactly once, or the
    enclosing for-header cannot be found immediately before it."""
    if text.count(PROD_END_ANCHOR) != 1:
        return None
    end_anchor_pos = text.index(PROD_END_ANCHOR)
    header_pos = text.rfind(PROD_FOR_HEADER, 0, end_anchor_pos)
    if header_pos == -1:
        return None
    body_start = header_pos + len(PROD_FOR_HEADER)
    close_pos = text.find("\n        }\n", end_anchor_pos)
    if close_pos == -1:
        return None
    return text[body_start:close_pos]

MIRROR_RE = re.compile(
    r"static void cap_update_tick\([^)]*\)\n\{\n(.*?)\n\}\n",
    re.DOTALL,
)

# Lines that exist on exactly one side for structural reasons unrelated to
# the arithmetic itself (see module docstring). Stripped before comparison.
PROD_ONLY_LINE_RES = [
    re.compile(r"^\s*if \(!s_exec\.zones\[zi\]\.active \|\| s_exec\.zones\[zi\]\.faulted\) continue;\s*$"),
    re.compile(r"^\s*zone_runtime_t \*z = &s_exec\.zones\[zi\];\s*$"),
    re.compile(r"^\s*float cap_c_per_hr = 0\.0f;\s*$"),
    re.compile(r"^\s*\(void\)zones_config_get_approach_rate_cap_c_per_hr\(zi, &cap_c_per_hr\);\s*$"),
]
MIRROR_ONLY_LINE_RES = [
    re.compile(r"^\s*if \(!z->active \|\| z->faulted\) return;\s*$"),
]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def normalize(body: str, only_line_res: list) -> list:
    body = strip_comments(body)
    lines = []
    for raw_line in body.splitlines():
        if any(r.match(raw_line) for r in only_line_res):
            continue
        line = raw_line.strip()
        if not line:
            continue
        line = line.replace("s_exec.target_c", "TARGET")
        line = line.replace("shared_target_c", "TARGET")
        line = re.sub(r"\bcontinue;", "RET;", line)
        line = re.sub(r"\breturn;", "RET;", line)
        # Collapse internal whitespace so a reflow/reindent alone never
        # trips this check -- only an actual token-level change should.
        line = re.sub(r"\s+", " ", line)
        lines.append(line)
    return lines


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    prod_path = repo_root / PROD_REL
    mirror_path = repo_root / MIRROR_REL

    for label, path in (("production", prod_path), ("mirror", mirror_path)):
        if not path.is_file():
            print("APPROACH-RATE-CAP MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    prod_text = prod_path.read_text(encoding="utf-8")
    mirror_text = mirror_path.read_text(encoding="utf-8")

    prod_body = find_prod_body(prod_text)
    if prod_body is None:
        print("APPROACH-RATE-CAP MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the per-zone cap-update loop in {PROD_REL} --")
        print(f"  either '{PROD_END_ANCHOR}' is no longer unique in the file, or its")
        print("  enclosing for-loop moved. Update this check rather than letting it")
        print("  pass vacuously.")
        return 1

    mirror_match = MIRROR_RE.search(mirror_text)
    if not mirror_match:
        print("APPROACH-RATE-CAP MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate cap_update_tick() in {MIRROR_REL} --")
        print("  update this check's MIRROR_RE rather than letting it pass vacuously.")
        return 1

    prod_lines = normalize(prod_body, PROD_ONLY_LINE_RES)
    mirror_lines = normalize(mirror_match.group(1), MIRROR_ONLY_LINE_RES)

    if not prod_lines or not mirror_lines:
        print("APPROACH-RATE-CAP MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  Normalization left an empty fragment on one side -- fail closed rather")
        print("  than compare against nothing.")
        return 1

    if prod_lines != mirror_lines:
        print("APPROACH-RATE-CAP MIRROR DRIFT CHECK: FAILED")
        print(f"  {MIRROR_REL}'s cap_update_tick() no longer matches the per-zone cap-")
        print(f"  update loop in {PROD_REL} -- the mirror is now testing arithmetic")
        print("  that production does not run. Update the mirror to match.")
        print()
        print("  --- normalized production ---")
        for line in prod_lines:
            print(f"    {line}")
        print("  --- normalized mirror ---")
        for line in mirror_lines:
            print(f"    {line}")
        return 1

    print(
        f"APPROACH-RATE-CAP MIRROR DRIFT CHECK: OK ({len(prod_lines)} normalized lines "
        "match between production and the host-test mirror)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
