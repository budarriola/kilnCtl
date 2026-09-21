"""check_python_zero_caller_sweep.py -- dead-code / no-caller sweep for
tools/PcTools/src, run as a standing check instead of the one-off scripted
pass docs/RELEASE_HARDENING_PLAN.md's item 1 acceptance criteria previously
relied on (see that doc's "Python-side zero-production-caller sweep"
status entries, 2026-09-17).

WHY THIS EXISTS. The C-side sweep found the `web_auth_table_create_session`
shape once: a live, wired-in production entry point nothing actually calls.
The one-off Python sweep (2026-09-17, in a since-discarded worktree) covered
`tools/PcTools/src` by hand and found three benign zeros (ad hoc research
modules, never wired) plus 59 false zeros from its own exclude-defining-file
methodology -- the same false-positive shape the C sweep hit first. That
pass was never made a standing check, so a genuinely new zero-caller
function added after 2026-09-17 would not be caught until someone re-ran the
sweep by hand again. This script is that standing check.

METHODOLOGY (corrected from the one-off pass's own postmortem: the original
scripts excluded each function's *defining* file when searching for callers,
which misclassified a `static`/private helper called only elsewhere in its
own file as a false zero). This script instead greps the whole non-test
tree, INCLUDING the defining file, and only excludes the function's own
`def name(...)` line itself. A function is a zero-caller candidate only if
no other line anywhere in the scanned tree (any file, any directory, minus
tests/ dirs and the def line) mentions its name at all -- so an in-file
caller, a decorator, or a string-keyed dispatch-table entry all correctly
count as "used".

SCOPE. Every top-level, non-underscore `def name(...)` in every .py file
under tools/PcTools/src/kilnctrl and tools/PcTools/src/mcpkit (the two
production package roots named by this item; tools/PcTools/scripts and
tools/PcTools/ui_scripts are one-off/CLI entry points, not the "src" tree
this check is scoped to, and are intentionally left out -- see the 2026-09-17
pass's note that those two dirs contain research/CLI tooling design to be
invoked by a human, not by other code).

EXCLUDED FROM THE SCAN: tools/mykicadMcp (a separate git submodule -- never
edited from this repo), any tests/ or test/ directory, __pycache__, and
.venv.

ALLOWLIST. A short, justified allowlist of names already reviewed by hand
and confirmed non-production (research-only modules with no `__main__` and
no documented caller) lives in ZERO_CALLER_ALLOWLIST below. Anything found
zero-caller that is NOT on that allowlist fails the check by name, so a
newly introduced dead function (or a genuinely orphaned wired-in one) is
caught the next time this check runs, rather than waiting for the next
manual sweep.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Production package roots this item is scoped to.
DEFAULT_SCAN_ROOTS = [
    REPO_ROOT / "tools" / "PcTools" / "src" / "kilnctrl",
    REPO_ROOT / "tools" / "PcTools" / "src" / "mcpkit",
]

# Directories excluded everywhere: from function discovery AND from the
# caller search.
EXCLUDED_DIR_NAMES = {"__pycache__", ".venv", "mykicadMcp", ".git"}

FUNC_DEF_RE = re.compile(r"^def ([A-Za-z][A-Za-z0-9_]*)\s*\(")

# A decorator immediately above a def that matches one of these substrings is
# itself the production wiring -- the same shape as a firmware httpd_uri_t
# table entry (registered by reference, never called by textual name
# elsewhere). Mirrors the C sweep's "genuinely indirect-dispatch" carve-out
# for zones_http.c's sweep_status_get_handler. Decorator-registered MCP
# tools (mcp_server_*.py's `@_srv._tool()`) are the concrete case in this
# tree today.
DECORATOR_WIRING_MARKERS = ("_tool(", ".tool(", ".route(", "app.get(", "app.post(")

# Names already reviewed by hand (docs/RELEASE_HARDENING_PLAN.md item 1,
# "Python-side zero-production-caller sweep", 2026-09-17 status) and
# confirmed to be ad hoc, interactively-invoked research/analysis tooling
# with no production caller by design -- not the web_auth_table_create_session
# shape (a live, wired-in entry point nothing calls). Each entry names the
# defining file so a same-named function elsewhere is NOT silently covered.
ZERO_CALLER_ALLOWLIST = {
    ("fuzzy_load_sweep.py", "find_best_strength_per_load"),
    ("http_capture_log.py", "starting_temps_c"),
    ("load_mass_sweep.py", "run_profile7_loaded"),
    # Below: same one-off research/analysis modules as the three above (no
    # __main__, no CLI entry point, no documented caller other than a human
    # importing the module by hand -- see each module's own docstring),
    # found by this check's first full-coverage run (2026-09-20) once the
    # methodology switched from "top 8 most-referenced names per pairing"
    # to every top-level name. Same benign shape, not re-litigated per name.
    ("fuzzy_load_sweep.py", "run_grid"),
    ("fuzzy_load_sweep.py", "summarize_by_strength_load"),
    ("fuzzy_load_sweep.py", "format_grid_table"),
    ("load_mass_sweep.py", "run_cone_schedule_loaded"),
    ("load_mass_sweep.py", "format_metrics_table"),
    ("coupled_ident.py", "identify_zone_dead_time_tau_from_capture_path"),
    ("coupled_ident.py", "parse_manual_dwell_tsv"),
    ("load_estimator.py", "estimate_per_source_from_capture_path"),
    ("plant_sim.py", "format_per_zone_gain_holdout_report_text"),
    ("plant_sim.py", "render_sim_report"),
    ("plant_sim.py", "format_sim_report_text"),
    # http_auth.clear_sessions's own docstring: "Used by tests, and available
    # to any [caller]" -- a deliberate test-support utility, not a dead
    # production path.
    ("http_auth.py", "clear_sessions"),
    # capture_pool_provenance.assert_pool_gate_consistent is the "raises"
    # half of a documented pair (README.md's capture_pool_provenance.py
    # bullet): its sibling check_pool_gate_consistency (the "reports" half)
    # already has a live CLI entry point (this module's own __main__ block)
    # and is called BY assert_pool_gate_consistent itself, so the module as
    # a whole is wired -- assert_pool_gate_consistent is documented public
    # API for a future importer who wants raise-on-violation semantics
    # rather than a returned problem list, not a stray unwired path.
    ("capture_pool_provenance.py", "assert_pool_gate_consistent"),
}

# Found genuinely zero-caller by this check's first full-coverage run
# (2026-09-20) and reviewed by hand against the web_auth_table_create_session
# shape (a live, wired-in production entry point nothing calls) -- these ARE
# that shape, or close enough that only the owner should decide whether to
# wire them up or delete them. Listed separately from the benign allowlist
# above (rather than silently merged into it) so this distinction survives:
# a future reader of this file should not assume every entry below is fine.
# See docs/RELEASE_HARDENING_PLAN.md item 1 status, 2026-09-20, for the full
# writeup of each.
PENDING_OWNER_REVIEW = {
    # CONTROL-task binary wire-command encoders for the 2026-08-21 shared
    # unit-preference feature; the feature that actually shipped reads/writes
    # unit preference over HTTP (POST /api/unit_pref), so this CONTROL-task
    # pair (0x04 GET_UNIT_PREF / 0x05 SET_UNIT_PREF) was apparently built as
    # an alternate transport and never wired to any caller.
    ("devices_control.py", "control_get_unit_pref"),
    ("devices_control.py", "control_set_unit_pref"),
    # params_by_name's own docstring explains id-based lookup was deliberately
    # rejected in favor of name-based ("a renumbered id cannot silently
    # retarget a value at a different field") -- params_by_id looks like the
    # leftover of that decision, never removed.
    ("safety_cfg_http_client.py", "params_by_id"),
    # entry_from_dict (its deserializing counterpart, run_queue.py:1596) IS
    # called (run_queue.py:2600); entry_to_dict, the serializing half of the
    # same pair, has no caller anywhere -- an asymmetric pair, the
    # "reset-one-side" bug-class shape CLAUDE.md calls out, though here the
    # unused side is dead code rather than stale state.
    ("run_queue.py", "entry_to_dict"),
    # No caller anywhere in the repo, including tools/PcTools/scripts and
    # ui_scripts (outside this check's own scan roots, but inside its
    # caller-search universe). Reads a JSON preset file into a dict; nothing
    # calls it today.
    ("run_queue.py", "load_preset_json"),
    # Thin `path.read_bytes()` + find_one_identity(...) wrapper; find_one_
    # identity itself is used elsewhere (check_slot_bins_fresh), this
    # convenience wrapper around it is not.
    ("pico_image_freshness.py", "read_file_identity"),
    # cone_table's sibling functions (band_bottom_c, heat_work_weight) are
    # both actively used by ramp_assist.py; cone_for_temp_c -- a direct
    # mirror of firmware's cone_table_cone_for_temp_c -- has no caller
    # anywhere in tools/PcTools outside its own test.
    ("cone_table.py", "cone_for_temp_c"),
    # log_analysis.py is itself a CLI tool (has __main__/main()) built around
    # three input-source parsers (poll-capture JSONL, UART capture, and CSV);
    # parse_trace_csv (its /api/autotune/trace.csv sibling) is used by
    # render_autotune_report, but parse_history_csv (/api/history.csv) is
    # never called by any of this module's own report/main functions --
    # the CSV history-file input path looks implemented but never wired in.
    ("log_analysis.py", "parse_history_csv"),
}


def _is_excluded_dir(path: Path) -> bool:
    return any(part in EXCLUDED_DIR_NAMES for part in path.parts)


def _is_test_path(path: Path) -> bool:
    parts_lower = {p.lower() for p in path.parts}
    if "tests" in parts_lower or "test" in parts_lower:
        return True
    return path.name.startswith("test_")


def iter_py_files(root: Path):
    if not root.is_dir():
        return
    for path in root.rglob("*.py"):
        if _is_excluded_dir(path):
            continue
        yield path


def _is_decorator_wired(lines, def_line_idx: int) -> bool:
    """True if a contiguous run of decorator lines directly above the def
    (0-based def_line_idx into lines) contains a known registration-decorator
    marker."""
    j = def_line_idx - 1
    while j >= 0:
        stripped = lines[j].strip()
        if not stripped:
            j -= 1
            continue
        if not stripped.startswith("@"):
            break
        if any(marker in stripped for marker in DECORATOR_WIRING_MARKERS):
            return True
        j -= 1
    return False


def find_top_level_functions(path: Path):
    """Return [(name, line_no, decorator_wired)] for top-level (non-underscore,
    non-indented) def statements in path."""
    out = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return out
    for i, line in enumerate(lines, start=1):
        m = FUNC_DEF_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        if name.startswith("_"):
            continue
        wired = _is_decorator_wired(lines, i - 1)
        out.append((name, i, wired))
    return out


def load_corpus(repo_root: Path):
    """Read every non-test, non-excluded .py file under the repo ONCE into
    memory as (path, [lines]) so the O(functions x files) caller search
    below does not re-open/re-read files per candidate function."""
    corpus = []
    for path in repo_root.rglob("*.py"):
        if _is_excluded_dir(path):
            continue
        if _is_test_path(path.relative_to(repo_root)):
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        corpus.append((path, lines))
    return corpus


def has_any_production_caller(name: str, def_file: Path, def_line: int,
                               corpus) -> bool:
    pattern = re.compile(r"\b" + re.escape(name) + r"\b")
    for path, lines in corpus:
        same_file = path == def_file
        for i, line in enumerate(lines, start=1):
            if same_file and i == def_line:
                continue  # the def statement itself is not a call
            if pattern.search(line):
                return True
    return False


def run_sweep(scan_roots, repo_root=REPO_ROOT):
    corpus = load_corpus(repo_root)
    findings = []
    pending = []
    checked = 0
    for root in scan_roots:
        for path in iter_py_files(root):
            if _is_test_path(path.relative_to(repo_root)):
                continue
            for name, line_no, decorator_wired in find_top_level_functions(path):
                checked += 1
                if decorator_wired:
                    continue
                if has_any_production_caller(name, path, line_no, corpus):
                    continue
                key = (path.name, name)
                rel = path.relative_to(repo_root)
                if key in ZERO_CALLER_ALLOWLIST:
                    continue
                if key in PENDING_OWNER_REVIEW:
                    pending.append((rel, line_no, name))
                    continue
                findings.append((rel, line_no, name))
    return checked, findings, pending


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    scan_roots = DEFAULT_SCAN_ROOTS
    if argv:
        scan_roots = [Path(a) for a in argv]

    checked, findings, pending = run_sweep(scan_roots)

    if pending:
        print(
            f"check_python_zero_caller_sweep: NOTE -- {len(pending)} function(s) "
            f"are genuinely zero-caller and flagged PENDING_OWNER_REVIEW (not "
            f"failing the build, but not cleared as benign either -- see this "
            f"script's PENDING_OWNER_REVIEW comments for why each was left in "
            f"place rather than removed):"
        )
        for rel_path, line_no, name in pending:
            print(f"  {rel_path}:{line_no}: {name}")

    if not findings:
        print(
            f"check_python_zero_caller_sweep: {checked} top-level functions "
            f"scanned across {len(scan_roots)} root(s), 0 unreviewed "
            f"zero-caller functions (beyond the reviewed allowlist of "
            f"{len(ZERO_CALLER_ALLOWLIST)} and the {len(PENDING_OWNER_REVIEW)} "
            f"pending owner review)."
        )
        return 0

    print(
        f"check_python_zero_caller_sweep: FAIL -- {len(findings)} function(s) "
        f"have no caller anywhere outside test directories and are not on "
        f"the reviewed allowlist:"
    )
    for rel_path, line_no, name in findings:
        print(f"  {rel_path}:{line_no}: {name}")
    print(
        "\nEach must be reviewed by hand: either it is genuine dead code "
        "(remove it) or a wired-in entry point nothing calls (the "
        "web_auth_table_create_session shape -- wire it up or remove it), "
        "or it is a deliberate, ad hoc, interactively-invoked research "
        "function -- in which case add (defining-filename, name) to "
        "ZERO_CALLER_ALLOWLIST (benign) or PENDING_OWNER_REVIEW (genuinely "
        "dead/unwired, kept only pending an explicit owner decision) in "
        "this file with a one-line justification."
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
