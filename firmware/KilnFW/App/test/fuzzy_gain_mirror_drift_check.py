#!/usr/bin/env python3
"""fuzzy_gain_mirror_drift_check.py -- test_closed_loop.c's own header comment
admits fuzzy_tick() is a "hand-written MIRROR" of pid_fuzzy_prepare_gains()
(profile_executor_pid_tick.c) -- not a call into it, since a host test cannot
reach zone_runtime_t/s_exec to call the real function -- and that "a future
change to pid_fuzzy_prepare_gains() ... would silently diverge from this
mirror and go undetected by every test below". Nothing was checking that.
Same extract-normalize-diff technique as approach_rate_cap_mirror_drift_
check.py, frame_a_offset_drift_check.py and power_diag_flag_mirror_drift_
check.py (read those first).

SCOPE, DELIBERATELY NARROW: this compares fuzzy_tick() only up through its
bump-transfer/gain-assembly lines -- i.e. exactly the body of pid_fuzzy_
prepare_gains(), which is the function test_closed_loop.c's own comment names
as what it mirrors. fuzzy_tick()'s final line, `return pid_update(state,
&adjusted, setpoint, measurement, dt_s, ff_u, ff_u);`, is NOT compared against
anything: the real per-tick caller is pid_family_zone_tick()
(profile_executor_pid_tick.c), which calls pid_update_terms() (not
pid_update()) with a per-zone capped setpoint (zone_commanded_setpoint_c()),
tapered feedforward (zone_taper_climb_rate()/zone_feedforward()), and a
membership-change bumpless reseed -- none of which the mirror attempts, and
its own header comment does not claim to. Extending this check to also cover
that call would require normalizing away all of that real behaviour, which
would stop this check from detecting real drift in that machinery rather than
proving equivalence -- so it is left out honestly instead of faked.

WHAT IS COMPARED: both sides reduce to the same core once three structurally-
required differences are normalized away:
  - production takes z/zi (a zone_runtime_t + index) and writes into an
    out_cfg parameter; the mirror takes discrete pid_state_t/base_cfg/
    prev_ki/setpoint/measurement parameters and returns a duty its caller
    doesn't have here. The struct-field reads/writes this implies
    (z->actual_c, z->pid_state.d_filtered, z->pid_cfg.*, z->
    fuzzy_prev_effective_ki, out_cfg->*) are folded to the same tokens as
    the mirror's plain locals/parameters (measurement, state->d_filtered,
    base_cfg->*, *prev_ki, adjusted.*) below.
  - production resolves strength_pct from zones_config_get_fuzzy_strength_
    pct(zi) with a clamp/NaN guard (defence-in-depth on a value zones_http.c
    already validates at load time); the mirror takes strength_pct as a
    plain parameter, i.e. the already-resolved value. Both are folded to one
    token; the clamp itself is production-only config-loader plumbing, not
    part of the gain arithmetic this check exists to protect.
  - production resolves error_band_c/rate_band_c_per_s exclusively from
    resolve_fuzzy_bands() (this zone's own identified plant model) and, per
    docs/audits/fuzzy_no_model_no_fuzzy_2026-09-14.md, forces strength_pct to
    0 (dropping the call to pid_fuzzy_adjust() into its bit-exact-base-gains
    short-circuit) rather than inventing a band when no model is identified;
    the mirror has no model concept at all and passes 20.0f/0.5f as literals
    directly, standing in for "some already-resolved band value", never
    exercising the no-model path. Both are folded to one token each -- this
    check does NOT verify the literals equal 20.0f/0.5f (a pid_fuzzy.c
    internal-default concern, not this call site's arithmetic), only that the
    same four values reach pid_fuzzy_adjust() in the same argument positions,
    followed by the same rescale/prev_ki-update/field-assignment sequence.
Everything else -- the error_c/error_rate_c_per_s computation, the
pid_fuzzy_adjust() call's argument order, the pid_rescale_integral_for_new_ki()
call and its argument order, the prev-Ki bookkeeping, and the final kp/ki/kd
assignment -- must be BYTE IDENTICAL (modulo whitespace/comments) after that
folding, or this FAILS naming the first differing normalized line on each
side.

Usage: python fuzzy_gain_mirror_drift_check.py [repo_root]
Exit 0: the two normalized fragments match.
Exit 1: they differ, or either fragment could not be located at all (fail
        closed -- a regex/anchor that stops matching its target is a
        failure, not a vacuous pass, per this repo's standing rule for this
        class of check).
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

PROD_REL = "firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c"
MIRROR_REL = "firmware/KilnFW/App/test/test_closed_loop.c"

# Anchored on the function signature (unique in the file -- this is the only
# definition of pid_fuzzy_prepare_gains()) rather than a line number, per this
# repo's standing rule that a line-number anchor IS the drift this class of
# check exists to catch.
PROD_SIG = "void pid_fuzzy_prepare_gains(zone_runtime_t *z, uint8_t zi, pid_cfg_t *out_cfg)\n{\n"

MIRROR_SIG_RE = re.compile(
    r"static float fuzzy_tick\([^)]*\)\n\{\n(.*?)\n\}\n",
    re.DOTALL,
)
# The one line in fuzzy_tick() that belongs to pid_family_zone_tick()'s
# caller wiring, not pid_fuzzy_prepare_gains() -- see module docstring for
# why it is out of scope rather than normalized.
MIRROR_TAIL_RE = re.compile(r"^\s*return pid_update\(.*\);\s*$", re.MULTILINE)


def find_prod_body(text: str):
    """Returns pid_fuzzy_prepare_gains()'s body text, or None if the
    signature is not found exactly once or its closing brace can't be
    matched by simple brace counting."""
    if text.count(PROD_SIG) != 1:
        return None
    start = text.index(PROD_SIG) + len(PROD_SIG)
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        return None
    return text[start:i - 1]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


# (regex, replacement) pairs applied, in order, to each normalized line.
# Every one of these folds a structurally-required difference documented in
# the module docstring -- nothing else.
FOLDS = [
    (re.compile(r"\bz->actual_c\b"), "MEASUREMENT"),
    (re.compile(r"\bmeasurement\b"), "MEASUREMENT"),
    (re.compile(r"\bz->pid_state\.d_filtered\b"), "D_FILTERED"),
    (re.compile(r"\bstate->d_filtered\b"), "D_FILTERED"),
    (re.compile(r"\bz->pid_state\b"), "PID_STATE"),
    (re.compile(r"\bstate\b"), "PID_STATE"),
    (re.compile(r"\bz->pid_cfg\b"), "BASE_CFG"),
    (re.compile(r"\bbase_cfg\b"), "BASE_CFG"),
    (re.compile(r"\bz->fuzzy_prev_effective_ki\b"), "PREV_KI"),
    (re.compile(r"\*prev_ki\b"), "PREV_KI"),
    (re.compile(r"\bout_cfg->"), "ADJUSTED."),
    (re.compile(r"\badjusted\."), "ADJUSTED."),
    (re.compile(r"\*out_cfg\b"), "ADJUSTED"),
    (re.compile(r"\badjusted\b"), "ADJUSTED"),
    # docs/FUZZY_CONTROLLER_PLAN.md finding (D), fixed 2026-09-11: production
    # now resolves this zone's own commanded setpoint via zone_commanded_
    # setpoint_c(z, zi) (the same helper pid_family_zone_tick() already uses
    # for its own error/feedforward terms -- see that function's own doc
    # comment) instead of reading the shared s_exec.target_c directly, so a
    # capped zone's gain scheduling tracks the same setpoint its control loop
    # actually chases. Semantically this is still exactly "the setpoint this
    # tick", the same role the mirror's plain `setpoint` parameter plays (see
    # module docstring's first bullet) -- just resolved through a per-zone
    # helper call instead of a raw shared-field read -- so it folds to the
    # same SETPOINT token rather than being left to show up as drift.
    #
    # BLIND SPOT (verified 2026-09-11, deliberate, not a bug in this check):
    # folding zone_commanded_setpoint_c(z, zi) and s_exec.target_c to the same
    # SETPOINT token means this check CANNOT detect a regression of production
    # back from the former to the latter -- it structurally cannot express
    # setpoint provenance, because the mirror it compares against
    # (test_closed_loop.c's fuzzy_tick()) has no per-zone/cap concept at all,
    # only a bare `setpoint` parameter. Confirmed directly: reverting this
    # call site's error_c back to `s_exec.target_c - z->actual_c` still makes
    # this check print OK. Do NOT "fix" this by un-folding the two tokens --
    # that would just make the check fail permanently, not restore coverage
    # the mirror is incapable of providing. The actual guard against that
    # regression is test_fuzzy_prepare_gains_uses_zone_commanded_setpoint_
    # when_capped() in test_profile_executor_prestart.c, which calls the real
    # pid_fuzzy_prepare_gains() (not a mirror) and fails on the reverted code.
    (re.compile(r"\bzone_commanded_setpoint_c\(z,\s*zi\)"), "SETPOINT"),
    (re.compile(r"\bs_exec\.target_c\b"), "SETPOINT"),
    (re.compile(r"\bsetpoint\b"), "SETPOINT"),
    # strength_pct: production re-derives + clamps it from a config getter
    # (several statements); the mirror receives it as one parameter. Folding
    # the parameter name is not enough by itself -- production's derivation
    # statements are dropped entirely below (PROD_STRIP_LINE_RES) and only
    # the final resolved-value token has to line up, so this fold exists for
    # symmetry with the other tokens rather than because strength_pct's
    # spelling differs.
    (re.compile(r"\bstrength_pct_f\b"), "STRENGTH_PCT"),
    (re.compile(r"\bstrength_pct\b"), "STRENGTH_PCT"),
    (re.compile(r"\berror_band_c\b"), "ERROR_BAND"),
    (re.compile(r"\brate_band_c_per_s\b"), "RATE_BAND"),
    (re.compile(r"\b20\.0f\b"), "ERROR_BAND"),
    (re.compile(r"\b0\.5f\b"), "RATE_BAND"),
    (re.compile(r"\badj_kp\b"), "GAIN_KP"),
    (re.compile(r"\bkp\b"), "GAIN_KP"),
    (re.compile(r"\badj_ki\b"), "GAIN_KI"),
    (re.compile(r"\bki\b"), "GAIN_KI"),
    (re.compile(r"\badj_kd\b"), "GAIN_KD"),
    (re.compile(r"\bkd\b"), "GAIN_KD"),
]

# Statements (not lines -- see normalize()'s docstring for why) that exist on
# production's side only because it re-derives strength_pct/error_band_c/
# rate_band_c_per_s from config getters instead of receiving them as
# already-resolved parameters (see module docstring). The call site that
# USES the resolved values is not stripped -- only the derivation statements
# and their own declarations are.
PROD_ONLY_STMT_RES = [
    re.compile(r"^float strength_pct_f = 0\.0f$"),
    re.compile(r"^\(void\)zones_config_get_fuzzy_strength_pct\(zi, &strength_pct_f\)$"),
    re.compile(r"^uint8_t strength_pct = \(!isfinite\(strength_pct_f\)\) \? 0 : \(strength_pct_f < 0\.0f\) \? 0 "
               r": \(strength_pct_f > 100\.0f\) \? 100 : \(uint8_t\)\(strength_pct_f \+ 0\.5f\)$"),
    re.compile(r"^float error_band_c = 0\.0f, rate_band_c_per_s = 0\.0f$"),
    # docs/audits/fuzzy_no_model_no_fuzzy_2026-09-14.md (supersedes the
    # fuzzy_dimensionless_bands_2026-09-13.md-era config-getter fallback this
    # check used to fold above): production now resolves error_band_c/
    # rate_band_c_per_s exclusively through resolve_fuzzy_bands(), a
    # zone-model-aware resolver, and forces strength_pct to 0 (plain PID,
    # reusing the existing strength_pct==0 short-circuit) rather than
    # inventing a band when this zone has no identified plant model. The
    # mirror has no zone_runtime_t/model concept at all -- same "structurally
    # required difference" as strength_pct's own resolution above -- so this
    # bool-returning call and the two model-gated one-line `if` statements
    # that consume it are dropped entirely, matching how the whole
    # strength_pct derivation block is dropped.
    re.compile(r"^bool bands_from_model = resolve_fuzzy_bands\(z, zi, &error_band_c, &rate_band_c_per_s\)$"),
    re.compile(r"^if \(!bands_from_model\) strength_pct = 0$"),
    re.compile(r"^if \(!bands_from_model\) log_fuzzy_disabled_no_model_once\(zi\)$"),
    re.compile(r"^\*out_cfg = z->pid_cfg$"),  # mirror has no equivalent whole-struct copy statement
    # production initializes adj_kp/ki/kd from the base gains inline (belt
    # and braces against pid_fuzzy_adjust() not writing them); the mirror
    # declares kp/ki/kd bare since pid_fuzzy_adjust() always writes all
    # three (its documented contract). Same reason as the *out_cfg copy
    # above -- a declaration-style difference, not an arithmetic one.
    re.compile(r"^float adj_kp = z->pid_cfg\.kp, adj_ki = z->pid_cfg\.ki, adj_kd = z->pid_cfg\.kd$"),
]
MIRROR_ONLY_STMT_RES = [
    re.compile(r"^float kp, ki, kd$"),
    re.compile(r"^pid_cfg_t adjusted = \*base_cfg$"),  # matches production's *out_cfg = z->pid_cfg, dropped above
]


def split_statements(body: str) -> list:
    """Splits a function body into ';'-terminated statements, tolerant of
    the body's own line wrapping -- production and the mirror wrap the same
    logical pid_fuzzy_adjust() call across a different number of physical
    lines, so comparing physical lines directly would flag a cosmetic rewrap
    as drift. All whitespace (including newlines) is collapsed to single
    spaces first, then the (already comment-stripped) text is split on
    top-level ';' -- safe here because this fragment has no nested compound
    statements (no braces, no semicolons inside a for-header) between the
    function's opening and closing braces."""
    flat = re.sub(r"\s+", " ", body).strip()
    stmts = [s.strip() for s in flat.split(";")]
    return [s for s in stmts if s]


def normalize(body: str, only_stmt_res: list) -> list:
    body = strip_comments(body)
    stmts = []
    for stmt in split_statements(body):
        if any(r.match(stmt) for r in only_stmt_res):
            continue
        for pattern, repl in FOLDS:
            stmt = pattern.sub(repl, stmt)
        # z->pid_cfg is a by-value struct field on production's side
        # (accessed with `.`); base_cfg is a pointer parameter on the
        # mirror's side (accessed with `->`) -- same structurally-required
        # value-vs-pointer difference PID_STATE gets below. Both spellings
        # are folded to `.` so the field access itself, not the access
        # operator, is what's compared.
        stmt = stmt.replace("BASE_CFG->", "BASE_CFG.")
        # pid_rescale_integral_for_new_ki() takes a pid_state_t*: production
        # passes &z->pid_state (a by-value struct field, address-of); the
        # mirror passes `state`, already a pointer parameter. Folding
        # z->pid_state/state to the same PID_STATE token above leaves an
        # `&` on production's side only -- stripped here for the same
        # value-vs-pointer reason as BASE_CFG above.
        stmt = stmt.replace("&PID_STATE", "PID_STATE")
        stmts.append(stmt)
    return stmts


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    try:
        prod_path = resolve_driver_file(repo_root, Path(PROD_REL).name)
    except DriverFileError as exc:
        print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED (setup)")
        print(f"  production file not found: {exc}")
        return 1
    mirror_path = repo_root / MIRROR_REL

    for label, path in (("mirror", mirror_path),):
        if not path.is_file():
            print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    prod_text = prod_path.read_text(encoding="utf-8")
    mirror_text = mirror_path.read_text(encoding="utf-8")

    prod_body = find_prod_body(prod_text)
    if not prod_body:
        print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate pid_fuzzy_prepare_gains() in {PROD_REL} -- either its")
        print("  signature changed or it moved/was renamed. Update this check rather than")
        print("  letting it pass vacuously.")
        return 1

    mirror_match = MIRROR_SIG_RE.search(mirror_text)
    if not mirror_match:
        print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate fuzzy_tick() in {MIRROR_REL} -- update this check's")
        print("  MIRROR_SIG_RE rather than letting it pass vacuously.")
        return 1

    mirror_body = mirror_match.group(1)
    if not MIRROR_TAIL_RE.search(mirror_body):
        print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  Expected fuzzy_tick() to end with a `return pid_update(...)` line (the")
        print("  out-of-scope caller-wiring line this check deliberately excludes -- see")
        print("  module docstring) -- it is no longer there, so fuzzy_tick()'s shape has")
        print("  changed more than this check accounts for. Update it rather than letting")
        print("  it silently compare the wrong span.")
        return 1
    mirror_body = MIRROR_TAIL_RE.sub("", mirror_body)

    prod_lines = normalize(prod_body, PROD_ONLY_STMT_RES)
    mirror_lines = normalize(mirror_body, MIRROR_ONLY_STMT_RES)

    if not prod_lines or not mirror_lines:
        print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  Normalization left an empty fragment on one side -- fail closed rather")
        print("  than compare against nothing.")
        return 1

    if prod_lines != mirror_lines:
        print("FUZZY-GAIN MIRROR DRIFT CHECK: FAILED")
        print(f"  {MIRROR_REL}'s fuzzy_tick() no longer matches {PROD_REL}'s")
        print("  pid_fuzzy_prepare_gains() -- the mirror is now testing gain arithmetic")
        print("  that production does not run. Update the mirror to match.")
        print()
        n = max(len(prod_lines), len(mirror_lines))
        for i in range(n):
            p = prod_lines[i] if i < len(prod_lines) else "<missing>"
            m = mirror_lines[i] if i < len(mirror_lines) else "<missing>"
            if p != m:
                print(f"  first divergent normalized line ({i}):")
                print(f"    production: {p}")
                print(f"    mirror:     {m}")
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
        f"FUZZY-GAIN MIRROR DRIFT CHECK: OK ({len(prod_lines)} normalized lines match "
        "between pid_fuzzy_prepare_gains() and the host-test mirror fuzzy_tick())"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
