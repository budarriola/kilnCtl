#!/usr/bin/env python3
"""ramp_lock_decision_mirror_drift_check.py -- test_ramp_lock_onesided.c's own
header comment admits lock_lagging_mask() (its own name for what its header
calls `lock_held()`) is a "hand-written MIRROR" of profile_executor.c's
per-tick ramp-lock decision loop -- not a call into it, since that loop lives
directly inside executor_task_entry()'s FreeRTOS `for (;;)` body with no seam
to drive one tick at a time from a host test. Its only stated safeguard was a
manual promise to "diff whenever either changes", which is not a check. Same
extract-normalize-diff technique as approach_rate_cap_mirror_drift_check.py,
frame_a_offset_drift_check.py and power_diag_flag_mirror_drift_check.py (read
those first).

SCOPE, DELIBERATELY NARROWER THAN THE MIRROR FILE'S OWN HEADER CLAIMS: that
header names TWO fragments as mirrored -- the ramp-lock decision loop
(lock_lagging_mask()) and the ramp-stepping gate (step_schedule()). This
check binds ONLY the first. step_schedule() is NOT covered, and no check for
it is being added: profile_executor.c's stepping gate has grown a
stretched_this_tick / auto-stretch branch (PID_EXPANSION_PLAN.md sec 7.2,
`} else if (lock_ok || stretched_this_tick)`, not the `else if (lock_ok)` the
mirror file's own header still cites) plus per-branch dwelling-entry and
target-rate bookkeeping that step_schedule()'s mirror does not attempt at
all -- it hard-codes the pre-sec-7.2 two-argument gate and a fixed
ramp_c_per_hr. Normalizing away stretched_this_tick and the dwelling
machinery to force a byte-for-byte match would not be folding a structurally
-required difference (the pattern every other normalization in this family
follows) -- it would be deleting the exact logic step_schedule() has already,
silently, stopped covering. That is real drift already in effect, not
something a normalization rule can honestly paper over. See this repo's own
standing rule (task instructions, `feedback_*` memory) that a check aggressive
enough to force a match on a paraphrase stops detecting the drift it exists
for -- so step_schedule() is left unbound rather than faked, and this
docstring records why for the next person who considers doing so.

WHAT IS COMPARED (lock_lagging_mask() only): both sides reduce to the same
per-zone loop once two structurally-required differences are normalized away:
  - production reads the per-zone sensor validity from a separate local
    array, `sensor_ok[zi]` (computed earlier in executor_task_entry() from
    the raw channel readings); the mirror carries it as a field on its own
    mirror_zone_t, `zones[zi].sensor_ok`. Both are folded to the same token
    -- this is a storage-location difference (array element vs struct field)
    forced by the mirror not reconstructing s_exec/the channel-reading
    machinery, not a difference in what the lock actually checks.
  - production's band width is EXEC_RAMP_LOCK_BAND_C(zi), a per-zone macro
    (`exec_threshold((zi), 3)`, i.e. configurable per zone, "25C for every
    shipped config" per profile_executor_ramp_assist.c's own comment); the
    mirror hard-codes TEST_RAMP_LOCK_BAND_C = 25.0f. Folded to one token --
    this check does not verify every zone's configured band actually equals
    25.0f (that is a config-default concern for a different check), only
    that the same comparison shape (`target - actual > BAND`, one-sided) is
    used.
Everything else -- the active/faulted skip, the `!sensor_ok || (target -
actual) > BAND` one-sided condition (the exact fix this test file's own
defect writeup is about: NOT fabsf, NOT the reverse subtraction order), and
the lagging-mask bit-set -- must be BYTE IDENTICAL (modulo whitespace/
comments) after that folding, or this FAILS naming the first divergent line.

Usage: python ramp_lock_decision_mirror_drift_check.py [repo_root]
Exit 0: the two normalized fragments match.
Exit 1: they differ, or either fragment could not be located at all (fail
        closed -- an anchor that stops matching its target is a failure, not
        a vacuous pass, per this repo's standing rule for this class of
        check).
"""
import re
import sys
from pathlib import Path

PROD_REL = "firmware/KilnFW/App/drivers/profile_executor.c"
MIRROR_REL = "firmware/KilnFW/App/test/test_ramp_lock_onesided.c"

# Anchored on the one-sided condition itself (the exact fix this defect is
# about), not a line number -- profile_executor.c's own header comment in the
# mirror file already cites stale line numbers (~358-370), which is exactly
# the failure mode a line-anchored check would inherit silently.
PROD_END_ANCHOR = "if (!sensor_ok[zi] || (s_exec.target_c - s_exec.zones[zi].actual_c) > EXEC_RAMP_LOCK_BAND_C(zi)) {"
PROD_FOR_HEADER = "for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {\n"


def find_prod_body(text: str):
    """Returns the for-loop body text containing the one-sided ramp-lock
    condition (between the nearest enclosing for-header's '{' and its own
    matching close), or None if the anchor is not unique or the enclosing
    for-loop can't be found/matched by brace counting."""
    if text.count(PROD_END_ANCHOR) != 1:
        return None
    anchor_pos = text.index(PROD_END_ANCHOR)
    header_pos = text.rfind(PROD_FOR_HEADER, 0, anchor_pos)
    if header_pos == -1:
        return None
    body_start = header_pos + len(PROD_FOR_HEADER)
    depth = 1
    i = body_start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        return None
    return text[body_start:i - 1]


MIRROR_SIG_RE = re.compile(
    r"static uint8_t lock_lagging_mask\([^)]*\)\n\{\n(.*?)\n\}\n",
    re.DOTALL,
)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


FOLDS = [
    (re.compile(r"\bs_exec\.zones\[zi\]\.active\b"), "ZONE_ACTIVE"),
    (re.compile(r"\bzones\[zi\]\.active\b"), "ZONE_ACTIVE"),
    (re.compile(r"\bs_exec\.zones\[zi\]\.faulted\b"), "ZONE_FAULTED"),
    (re.compile(r"\bzones\[zi\]\.faulted\b"), "ZONE_FAULTED"),
    (re.compile(r"\bsensor_ok\[zi\](?=\W|$)"), "ZONE_SENSOR_OK"),
    (re.compile(r"\bzones\[zi\]\.sensor_ok\b"), "ZONE_SENSOR_OK"),
    (re.compile(r"\bs_exec\.target_c\b"), "TARGET"),
    (re.compile(r"\btarget_c\b"), "TARGET"),
    (re.compile(r"\bs_exec\.zones\[zi\]\.actual_c\b"), "ACTUAL"),
    (re.compile(r"\bzones\[zi\]\.actual_c\b"), "ACTUAL"),
    (re.compile(r"\bEXEC_RAMP_LOCK_BAND_C\(zi\)"), "BAND"),
    (re.compile(r"\bTEST_RAMP_LOCK_BAND_C\b"), "BAND"),
    (re.compile(r"\blagging\b"), "LAGGING"),
    (re.compile(r"\bLAGGING \|= \(uint8_t\)\(1u << zi\);"), "LAGGING |= (uint8_t)(1u << zi);"),
]

# The mirror's `held`-variable indirection (an if/else choosing between the
# pre-fix fabsf formula and the fix, gated on its own old_fabsf test
# parameter that does not exist in production at all) is test-harness
# scaffolding for running the defect-reproduction case in the same file --
# production only ever runs the fixed, one-sided formula. Only the FIXED
# (old_fabsf == false) arm is compared; the pre-fix arm and the branch that
# selects between them are production-irrelevant by construction, not a
# folded structural difference.
MIRROR_OLD_FABSF_ARM_RE = re.compile(
    r"if \(old_fabsf\) \{\n\s*held = [^\n]*;\n\s*\} else \{\n\s*held = ([^\n]*);\n\s*\}",
)


def normalize_prod(body: str) -> list:
    body = strip_comments(body)
    lines = []
    for raw_line in body.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        # production's version keeps `lagging |= ...` and `lock_ok = false;`
        # under the same `if`, but assembles the mask via a bare `lagging`
        # bit-set exactly like the mirror's `if (held) lagging |= ...` --
        # `lock_ok` itself is production-only bookkeeping (s_exec.ramp_
        # lock_held is derived from it elsewhere) that has no mirror
        # counterpart to compare against, so it's dropped rather than
        # forced to match something that isn't there.
        if line.startswith("lock_ok = false;"):
            continue
        for pattern, repl in FOLDS:
            line = pattern.sub(repl, line)
        line = re.sub(r"\s+", " ", line)
        lines.append(line)
    return lines


MIRROR_FOR_HEADER_RE = re.compile(r"for \(uint8_t zi = 0; zi < TEST_ZONE_COUNT; zi\+\+\) \{\n")

# After splicing in the fixed condition (see below), the mirror reads
# `held = COND; ... if (held) LAGGING |= X;` -- an extra `held` local exists
# only because this file also has to run the PRE-FIX fabsf arm for its own
# defect-reproduction test (see MIRROR_OLD_FABSF_ARM_RE's comment); production
# never had that indirection; it just writes `if (COND) { LAGGING |= X; }`
# directly. Collapsed here into the same inlined shape production uses, once
# the fixed condition is known.
MIRROR_HELD_INLINE_RE = re.compile(
    r"bool held; held = (.+?); if \(held\) (lagging \|= [^;]+;)",
)


def find_mirror_loop_body(func_body: str):
    """Returns just the for-loop's inner body from lock_lagging_mask()'s
    full function text (discarding `uint8_t lagging = 0;`, the for-header,
    and `return lagging;`) so it lines up with production's extraction,
    which is likewise scoped to the for-loop body only."""
    header_match = MIRROR_FOR_HEADER_RE.search(func_body)
    if not header_match:
        return None
    body_start = header_match.end()
    depth = 1
    i = body_start
    while i < len(func_body) and depth > 0:
        if func_body[i] == "{":
            depth += 1
        elif func_body[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        return None
    return func_body[body_start:i - 1]


def normalize_mirror(func_body: str) -> list:
    func_body = strip_comments(func_body)
    m = MIRROR_OLD_FABSF_ARM_RE.search(func_body)
    if not m:
        return None
    fixed_condition = m.group(1)
    # Splice the fixed arm's condition in place of the whole if/else, then
    # inline the resulting `held = COND; ... if (held) LAGGING |= X;` into
    # `if (COND) { LAGGING |= X; }` so it matches production's shape exactly
    # (see MIRROR_HELD_INLINE_RE's comment for why this indirection exists
    # at all and is safe to collapse).
    spliced = func_body[:m.start()] + f"held = {fixed_condition};" + func_body[m.end():]
    loop_body = find_mirror_loop_body(spliced)
    if loop_body is None:
        return None
    flat = re.sub(r"\s+", " ", loop_body).strip()
    inlined = MIRROR_HELD_INLINE_RE.sub(r"if (\1) { \2 }", flat)
    if inlined == flat:
        return None  # the inline substitution didn't fire -- fail closed, see caller

    lines = []
    for chunk in re.split(r"(?<=[;{}])\s+", inlined):
        line = chunk.strip()
        if not line or line in ("bool held;",):
            continue
        for pattern, repl in FOLDS:
            line = pattern.sub(repl, line)
        line = re.sub(r"\s+", " ", line)
        lines.append(line)
    return lines


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    prod_path = repo_root / PROD_REL
    mirror_path = repo_root / MIRROR_REL

    for label, path in (("production", prod_path), ("mirror", mirror_path)):
        if not path.is_file():
            print("RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    prod_text = prod_path.read_text(encoding="utf-8")
    mirror_text = mirror_path.read_text(encoding="utf-8")

    prod_body = find_prod_body(prod_text)
    if prod_body is None:
        print("RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the ramp-lock decision loop in {PROD_REL} -- either")
        print(f"  '{PROD_END_ANCHOR}'")
        print("  is no longer unique in the file, or its enclosing for-loop moved. Update")
        print("  this check rather than letting it pass vacuously.")
        return 1

    mirror_match = MIRROR_SIG_RE.search(mirror_text)
    if not mirror_match:
        print("RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate lock_lagging_mask() in {MIRROR_REL} -- update this")
        print("  check's MIRROR_SIG_RE rather than letting it pass vacuously.")
        return 1

    prod_lines = normalize_prod(prod_body)
    mirror_lines = normalize_mirror(mirror_match.group(1))

    if mirror_lines is None:
        print("RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the old_fabsf/fixed if/else split inside lock_lagging_")
        print(f"  mask() in {MIRROR_REL} -- update MIRROR_OLD_FABSF_ARM_RE rather than")
        print("  letting it pass vacuously.")
        return 1

    if not prod_lines or not mirror_lines:
        print("RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  Normalization left an empty fragment on one side -- fail closed rather")
        print("  than compare against nothing.")
        return 1

    if prod_lines != mirror_lines:
        print("RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED")
        print(f"  {MIRROR_REL}'s lock_lagging_mask() no longer matches the ramp-lock")
        print(f"  decision loop in {PROD_REL} -- the mirror is now testing a lock")
        print("  decision production does not run. Update the mirror to match.")
        print()
        n = max(len(prod_lines), len(mirror_lines))
        for i in range(n):
            p = prod_lines[i] if i < len(prod_lines) else "<missing>"
            m2 = mirror_lines[i] if i < len(mirror_lines) else "<missing>"
            if p != m2:
                print(f"  first divergent normalized line ({i}):")
                print(f"    production: {p}")
                print(f"    mirror:     {m2}")
                break
        print()
        print("  --- normalized production ---")
        for line in prod_lines:
            print(f"    {line}")
        print("  --- normalized mirror (fixed arm only) ---")
        for line in mirror_lines:
            print(f"    {line}")
        return 1

    print(
        f"RAMP-LOCK DECISION MIRROR DRIFT CHECK: OK ({len(prod_lines)} normalized lines "
        "match between production's ramp-lock decision loop and the host-test mirror "
        "lock_lagging_mask()). step_schedule() is NOT covered -- see module docstring."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
