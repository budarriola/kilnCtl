#!/usr/bin/env python3
"""ramp_stepping_gate_mirror_drift_check.py -- companion to
ramp_lock_decision_mirror_drift_check.py (read that one first for the shared
extract-normalize-diff technique). That check binds test_ramp_lock_onesided.
c's lock_lagging_mask() to profile_executor.c's ramp-lock decision loop but
declined to bind step_schedule(), the file's OTHER mirrored fragment, because
production's ramp-stepping gate had grown a stretched_this_tick / auto-
stretch branch (PID_EXPANSION_PLAN.md sec 7.2) that step_schedule() did not
reproduce at all.

step_schedule() has since been brought back into correspondence with that
branch: it now takes stretched_this_tick and stretch_rate_c_per_s as
caller-supplied parameters (exactly as lock_ok already was) and every call
site in the test file passes stretched_this_tick=false / stretch_rate_c_
per_s=-1.0f, production's own "no stretch" sentinel -- so this check binds
that corrected shape rather than papering over the original gap.

SCOPE: only the ZONE_RAMP ramp sub-case of the gate is compared -- the
`} else if (lock_ok || stretched_this_tick) { s_exec.segment_elapsed_s +=
...; if (!s_exec.dwelling) { <ramp math> ... } }` fragment, stopping before
the `s_exec.target_c = new_target;` assignment and everything after it
(the dwelling-ENTRY transition: `if (s_exec.target_c == seg->target_c) {
s_exec.dwelling = true; ...dwell credit spend... }`). That dwelling-entry
bookkeeping, the already-dwelling `else` branch, and the io_blocking/relay-
segment branch are all still out of scope -- step_schedule() does not
attempt them (see the mirror file's own header comment for why: none of
that state is exercised by what this file's tests check, and reconstructing
it would need s_exec itself). Normalizing THAT gap away would be exactly the
kind of drift-hiding ramp_lock_decision_mirror_drift_check.py refused to do,
so this check's extraction point stops before it, same discipline.

WHAT IS COMPARED: both sides reduce to the same shape once these
structurally-required differences are normalized away:
  - production's gate condition is `} else if (lock_ok || stretched_this_
    tick) {` (an else-if attached to the io_blocking branch above it); the
    mirror is a free-standing function that inverts the same disjunct into
    an early return (`if (!lock_ok && !stretched_this_tick) { return; }`,
    De Morgan's negation of the same condition). Both sides are folded to
    the token GATE_OK for `lock_ok || stretched_this_tick` (respectively its
    negation) so the comparison is over the disjunct itself, not which
    control-flow shape carries it.
  - production reads/writes `s_exec.segment_elapsed_s`, `s_exec.target_c`,
    `seg->target_c`, `seg->ramp_c_per_hr`; the mirror carries the same
    quantities as parameters/locals (`*segment_elapsed_s`, `*target_c`,
    `seg_target_c`, `ramp_c_per_hr`). Storage-location differences forced by
    the mirror not reconstructing s_exec/seg, not a difference in what the
    gate computes -- folded to shared tokens.
  - production's rate substitution is written as a ternary assigned to a
    local (`float rate_c_per_hr = stretched_this_tick ? (stretch_rate_c_
    per_s * 3600.0f) : seg->ramp_c_per_hr;`); the mirror writes the
    identical ternary against its own parameter names. Folded to the same
    token stream once the parameter/field names above are unified.
Everything else -- the direction sign, the new_target advance, and the
reached/clamp comparison -- must be BYTE IDENTICAL (modulo whitespace/
comments) after that folding, or this FAILS naming the first divergent line.

Usage: python ramp_stepping_gate_mirror_drift_check.py [repo_root]
Exit 0: the two normalized fragments match.
Exit 1: they differ, or either fragment could not be located at all (fail
        closed -- an anchor that stops matching its target is a failure, not
        a vacuous pass, per this repo's standing rule for this class of
        check).
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

PROD_REL = "firmware/KilnFW/App/drivers/control/profile_executor.c"
MIRROR_REL = "firmware/KilnFW/App/test/test_ramp_lock_onesided.c"

# Anchored on the gate's own condition text, not a line number.
PROD_GATE_ANCHOR = "} else if (lock_ok || stretched_this_tick) {\n"
PROD_STOP_ANCHOR = "                s_exec.target_c = new_target;\n"


def find_prod_fragment(text: str):
    if text.count(PROD_GATE_ANCHOR) != 1:
        return None
    start = text.index(PROD_GATE_ANCHOR) + len(PROD_GATE_ANCHOR)
    if text.count(PROD_STOP_ANCHOR) != 1:
        return None
    stop = text.index(PROD_STOP_ANCHOR)
    if stop <= start:
        return None
    return text[start:stop]


MIRROR_SIG_RE = re.compile(
    r"static void step_schedule\([^)]*\)\n\{\n(.*?)\n\}\n",
    re.DOTALL,
)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


# The mirror passes its pointer parameters straight through where production
# takes the address of the s_exec fields; fold the mirror's call to
# production's spelling.
MIRROR_CALL_FOLD = (
    re.compile(r"exec_elapsed_accumulate\(segment_elapsed_s, segment_elapsed_rem_ms,"),
    "exec_elapsed_accumulate(&s_exec.segment_elapsed_s, &s_exec.segment_elapsed_rem_ms,",
)

FOLDS = [
    (re.compile(r"\bs_exec\.segment_elapsed_s\b"), "SEG_ELAPSED"),
    (re.compile(r"\*segment_elapsed_s\b"), "SEG_ELAPSED"),
    (re.compile(r"\bs_exec\.target_c\b"), "TARGET"),
    (re.compile(r"\*target_c\b"), "TARGET"),
    (re.compile(r"\bseg->target_c\b"), "SEG_TARGET"),
    (re.compile(r"\bseg_target_c\b"), "SEG_TARGET"),
    (re.compile(r"\bseg->ramp_c_per_hr\b"), "RAMP_RATE"),
    (re.compile(r"\bramp_c_per_hr\b"), "RAMP_RATE"),
    (re.compile(r"\bstretch_rate_c_per_s\b"), "STRETCH_RATE"),
    (re.compile(r"\bstretched_this_tick\b"), "STRETCHED"),
    (re.compile(r"\brate_c_per_hr\b"), "RATE"),
    (re.compile(r"\bdt_s\b"), "DT"),
]


# production's own if(reached){A}else{B;target_rate bookkeeping} -- folded
# to the same ternary shape the mirror already writes directly
# (`TARGET = reached ? SEG_TARGET : new_target;`). The target_rate_c_per_s
# bookkeeping in the `else` arm is production-only feedforward plumbing the
# mirror's own docstring already declares out of scope (same status as
# dwelling-entry bookkeeping), so it's dropped here rather than forced onto
# a mirror that was never asked to cover it.
PROD_REACHED_IF_RE = re.compile(
    r"if \(reached\) \{ new_target = seg->target_c; \} else \{ s_exec\.target_rate_c_per_s = [^;]*; \}",
)


def normalize_prod(fragment: str) -> list:
    fragment = strip_comments(fragment)
    flat = re.sub(r"\s+", " ", fragment).strip()
    # production's outer `if (!s_exec.dwelling) { float new_target; if (seg->
    # ramp_c_per_hr <= 0.0f) { new_target = seg->target_c; } else { ... } }`
    # wrapper has no mirror counterpart: the mirror is scoped to the ramp
    # sub-case only (see module docstring) and never models the step-segment
    # (ramp_c_per_hr <= 0) case at all -- that gap predates this check and is
    # unrelated to sec 7.2, so those wrapper tokens are dropped rather than
    # forced onto a mirror that was never asked to cover them.
    for literal in ("if (!s_exec.dwelling) {", "float new_target;",
                    "if (seg->ramp_c_per_hr <= 0.0f) { new_target = seg->target_c; } else {"):
        flat = flat.replace(literal, "")
    m = PROD_REACHED_IF_RE.search(flat)
    if not m:
        return []
    flat = flat[:m.start()] + "s_exec.target_c = reached ? seg->target_c : new_target;" + flat[m.end():]
    # Drop the trailing close-braces left over from the wrapper removal --
    # they no longer bracket anything on this side once the wrapper tokens
    # above are gone.
    lines = []
    for chunk in re.split(r"(?<=[;{}])\s+", flat):
        line = chunk.strip()
        if not line or line == "}":
            continue
        for pattern, repl in FOLDS:
            line = pattern.sub(repl, line)
        line = re.sub(r"\s+", " ", line)
        lines.append(line)
    return lines


def normalize_mirror(func_body: str) -> list:
    func_body = strip_comments(func_body)
    lines = []
    for raw_line in func_body.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        # The early-return gate is production's disjunct negated (De
        # Morgan's law) -- folded to the same GATE_OK token as production's
        # else-if so the comparison is over the disjunct, not the two
        # different control-flow shapes that carry it.
        if line == "if (!lock_ok && !stretched_this_tick) {":
            line = "GATE_NOT_OK {"
        elif line.startswith("return;"):
            continue
        elif line == "}" and lines and lines[-1] == "GATE_NOT_OK {":
            lines.pop()
            continue
        line = MIRROR_CALL_FOLD[0].sub(MIRROR_CALL_FOLD[1], line)
        for pattern, repl in FOLDS:
            line = pattern.sub(repl, line)
        line = re.sub(r"\s+", " ", line)
        lines.append(line)
    # production declares `float new_target;` once, outside this fragment's
    # extraction (dropped by normalize_prod's wrapper removal), because the
    # same local is also assigned in the step-segment arm this check doesn't
    # cover; the mirror declares it inline at first use instead (`float
    # new_target = ...;`). Storage-declaration-site difference forced by the
    # wrapper this check already excludes, not a difference in what the
    # assignment computes -- folded away by stripping the leading `float `
    # off the `new_target =` assignment.
    for i, line in enumerate(lines):
        if line.startswith("float new_target = "):
            lines[i] = line[len("float "):]
            break
    return lines


ACCUM_RE = re.compile(
    r"static void exec_elapsed_accumulate\([^)]*\)\s*\{(.*?)\n\}\n",
    re.DOTALL,
)


def check_accumulate_helper(prod_text: str, mirror_text: str):
    """exec_elapsed_accumulate() is static in production, so the mirror carries
    a copy; its body must match production's (comments/whitespace aside)."""
    bodies = []
    for label, text in (("production", prod_text), ("mirror", mirror_text)):
        matches = ACCUM_RE.findall(text)
        if len(matches) != 1:
            return f"{label}: exec_elapsed_accumulate() not found exactly once ({len(matches)})"
        bodies.append(re.sub(r"\s+", " ", strip_comments(matches[0])).strip())
    if bodies[0] != bodies[1]:
        return ("exec_elapsed_accumulate() body differs.\n    production: "
                f"{bodies[0]}\n    mirror:     {bodies[1]}")
    return None


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    try:
        prod_path = resolve_driver_file(repo_root, Path(PROD_REL).name)
    except DriverFileError as exc:
        print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED (setup)")
        print(f"  production file not found: {exc}")
        return 1
    mirror_path = repo_root / MIRROR_REL

    for label, path in (("mirror", mirror_path),):
        if not path.is_file():
            print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    prod_text = prod_path.read_text(encoding="utf-8")
    mirror_text = mirror_path.read_text(encoding="utf-8")

    prod_fragment = find_prod_fragment(prod_text)
    if prod_fragment is None:
        print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate the ramp-stepping gate in {PROD_REL} -- either")
        print(f"  '{PROD_GATE_ANCHOR.strip()}' or '{PROD_STOP_ANCHOR.strip()}'")
        print("  is no longer unique/present. Update this check rather than letting it")
        print("  pass vacuously.")
        return 1

    mirror_match = MIRROR_SIG_RE.search(mirror_text)
    if not mirror_match:
        print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate step_schedule() in {MIRROR_REL} -- update this check's")
        print("  MIRROR_SIG_RE rather than letting it pass vacuously.")
        return 1

    helper_err = check_accumulate_helper(prod_text, mirror_text)
    if helper_err:
        print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED (exec_elapsed_accumulate)")
        print(f"  {helper_err}")
        return 1

    prod_lines = normalize_prod(prod_fragment)
    mirror_lines = normalize_mirror(mirror_match.group(1))

    if not prod_lines or not mirror_lines:
        print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  Normalization left an empty fragment on one side -- fail closed rather")
        print("  than compare against nothing.")
        return 1

    if prod_lines != mirror_lines:
        print("RAMP-STEPPING GATE MIRROR DRIFT CHECK: FAILED")
        print(f"  {MIRROR_REL}'s step_schedule() no longer matches the ramp-stepping gate")
        print(f"  in {PROD_REL} -- the mirror is now testing a stepping gate production")
        print("  does not run. Update the mirror to match.")
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
        print("  --- normalized mirror ---")
        for line in mirror_lines:
            print(f"    {line}")
        return 1

    print(
        f"RAMP-STEPPING GATE MIRROR DRIFT CHECK: OK ({len(prod_lines)} normalized lines "
        "match between production's ramp-stepping gate and the host-test mirror "
        "step_schedule()). Dwelling-entry bookkeeping, the already-dwelling branch and "
        "the io_blocking/relay-segment branch are NOT covered -- see module docstring."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
