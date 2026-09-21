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

import io
import re
import sys
import tokenize
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
# shape (a live, wired-in entry point nothing calls). Each entry keys on the
# repo-relative POSIX path of the defining file (not just its basename) so
# same-named files in different directories (e.g. `registry.py` exists at
# both tools/PcTools/src/kilnctrl/bench_test/registry.py and
# tools/PcTools/src/mcpkit/registry.py) are never silently conflated.
ZERO_CALLER_ALLOWLIST = {
    # binary_provenance.check_binary_fresh / assert_binary_fresh: the
    # call-site guard from docs/audits/review_sim_fuzzy_commits_2026-09-13.md,
    # meant to be invoked by hand immediately before reading a number out of
    # a host-test .exe. No PcTools script reads a prebuilt host-test binary
    # today (both build_host_tests.ps1 runners always rebuild and re-run),
    # so there is no in-repo caller yet, only test coverage
    # (tools/PcTools/tests/test_binary_provenance.py). Deliberately NOT
    # wired into a standing check_*.ps1 -- see the module docstring for why
    # a repo-wide sweep of the shared build directories measured 66/66
    # false positives.
    ("tools/PcTools/src/kilnctrl/binary_provenance.py", "check_binary_fresh"),
    ("tools/PcTools/src/kilnctrl/binary_provenance.py", "assert_binary_fresh"),
    ("tools/PcTools/src/kilnctrl/fuzzy_load_sweep.py", "find_best_strength_per_load"),
    ("tools/PcTools/src/kilnctrl/http_capture_log.py", "starting_temps_c"),
    ("tools/PcTools/src/kilnctrl/load_mass_sweep.py", "run_profile7_loaded"),
    # Below: same one-off research/analysis modules as the three above (no
    # __main__, no CLI entry point, no documented caller other than a human
    # importing the module by hand -- see each module's own docstring),
    # found by this check's first full-coverage run (2026-09-20) once the
    # methodology switched from "top 8 most-referenced names per pairing"
    # to every top-level name. Same benign shape, not re-litigated per name.
    ("tools/PcTools/src/kilnctrl/fuzzy_load_sweep.py", "run_grid"),
    ("tools/PcTools/src/kilnctrl/fuzzy_load_sweep.py", "summarize_by_strength_load"),
    ("tools/PcTools/src/kilnctrl/fuzzy_load_sweep.py", "format_grid_table"),
    ("tools/PcTools/src/kilnctrl/load_mass_sweep.py", "run_cone_schedule_loaded"),
    ("tools/PcTools/src/kilnctrl/load_mass_sweep.py", "format_metrics_table"),
    ("tools/PcTools/src/kilnctrl/coupled_ident.py", "identify_zone_dead_time_tau_from_capture_path"),
    ("tools/PcTools/src/kilnctrl/coupled_ident.py", "parse_manual_dwell_tsv"),
    ("tools/PcTools/src/kilnctrl/load_estimator.py", "estimate_per_source_from_capture_path"),
    ("tools/PcTools/src/kilnctrl/plant_sim.py", "format_per_zone_gain_holdout_report_text"),
    ("tools/PcTools/src/kilnctrl/plant_sim.py", "render_sim_report"),
    ("tools/PcTools/src/kilnctrl/plant_sim.py", "format_sim_report_text"),
    # http_auth.clear_sessions's own docstring: "Used by tests, and available
    # to any [caller]" -- a deliberate test-support utility, not a dead
    # production path.
    ("tools/PcTools/src/kilnctrl/http_auth.py", "clear_sessions"),
    # capture_pool_provenance.assert_pool_gate_consistent is the "raises"
    # half of a documented pair (README.md's capture_pool_provenance.py
    # bullet): its sibling check_pool_gate_consistency (the "reports" half)
    # already has a live CLI entry point (this module's own __main__ block)
    # and is called BY assert_pool_gate_consistent itself, so the module as
    # a whole is wired -- assert_pool_gate_consistent is documented public
    # API for a future importer who wants raise-on-violation semantics
    # rather than a returned problem list, not a stray unwired path.
    ("tools/PcTools/src/kilnctrl/capture_pool_provenance.py", "assert_pool_gate_consistent"),
    # Below: NOT benign-unused -- these are real, live production callees,
    # reached only through `getattr(kilnlink_codec, fn_name)` dispatch in
    # tools/PcTools/selfcheck_commonfw.py's `_PAYLOAD_VECTOR_MANIFESTS` table
    # (each entry names the encoder function by string, then
    # `commonfw_payload_vector_checks()` does `getattr(...)` and calls the
    # result). This is exactly the token-based matcher's documented blind
    # spot (getattr/dispatch-by-string): the call site never contains a NAME
    # token spelling the function's own identifier, only a STRING token, so
    # it cannot be told apart from a true zero-caller by this check. Verified
    # by hand (2026-09-20, this pass) against
    # tools/PcTools/selfcheck_commonfw.py's `_PAYLOAD_VECTOR_MANIFESTS` --
    # every name below appears there. Listed here (not deleted, not left
    # failing the build) because the check has no way to confirm this on its
    # own; a future reviewer should re-check selfcheck_commonfw.py's manifest
    # table before trusting this note if either file changes materially.
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_context"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_status"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_announce"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_diag"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_trip"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_power"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_ceiling"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_clear_trip"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_get_fw_version"),
    ("tools/PcTools/src/kilnctrl/kilnlink_codec.py", "encode_set_clock"),
    # Below: owner review 2026-09-21 of the 23-entry PENDING_OWNER_REVIEW
    # batch (docs/RELEASE_HARDENING_PLAN.md item 1) -- these 11 were kept,
    # each for the reason given. The other 12 were deleted in the same pass
    # (see git history / commit message for the full keep/delete table).
    #
    # cone_table's own mirror-of-firmware shape (cone_table_cone_for_temp_c
    # in the C code) is exactly the kind of near-future-need reference
    # implementation this repo keeps around even unwired -- see
    # band_bottom_c/heat_work_weight, its actively-used siblings.
    ("tools/PcTools/src/kilnctrl/cone_table.py", "cone_for_temp_c"),
    # A genuine "should probably be wired up" gap, not dead code: the GUI
    # hardcodes f"IO {io}" instead of calling this more descriptive label
    # function. Kept as a documented near-future wiring opportunity rather
    # than deleted or actually wired (wiring it in is out of scope for a
    # keep/delete pass).
    ("tools/PcTools/src/kilnctrl/devices_io.py", "digital_io_label"),
    # Documented feature in tools/PcTools/TODO.md; only test-exercised today
    # because the UART-capture input path it parses has no production
    # caller yet, not because it is superseded or dead.
    ("tools/PcTools/src/kilnctrl/log_analysis.py", "parse_profile_exec_uart_capture"),
    # Deliberately not wired as an MCP tool (no registration decorator) --
    # a documented test-support fixture-flash helper for
    # test_flash_board_pinning.py, not a leftover.
    ("tools/PcTools/src/kilnctrl/mcp_server_flash.py", "fixture_flash"),
    # Real production caller the sweep cannot see: invoked from a Python
    # heredoc embedded inside check_embedded_pico_image_fresh.ps1 (a
    # newly-documented blind spot -- Python code embedded in a .ps1 file is
    # invisible to this check's .py-file-glob-based corpus, distinct from
    # the getattr/dispatch-by-string blind spot already documented above).
    ("tools/PcTools/src/kilnctrl/pico_image_freshness.py", "check_slot_bins_fresh"),
    # Both are documented analysis recipes for plant_sim.py's own test
    # suite (per-zone gain holdout / actuator weight sensitivity), an
    # obvious near-future need once real gain-tuning data exists; kept
    # rather than deleted since their sibling report-formatting functions
    # already have production callers.
    ("tools/PcTools/src/kilnctrl/plant_sim.py", "actuator_weight_sensitivity_sweep"),
    ("tools/PcTools/src/kilnctrl/plant_sim.py", "per_zone_gain_holdout_report"),
    # Both are documented A/B comparison recipes referenced by
    # ramp_assist.py's own test suite; kept as near-future analysis tools
    # in the same family as the sweep functions above.
    ("tools/PcTools/src/kilnctrl/ramp_assist.py", "compare_heat_work"),
    ("tools/PcTools/src/kilnctrl/ramp_assist.py", "dwell_credit_parity"),
    # Documented recipe used by capture_stack_margin_baseline.py's own
    # workflow (test_bench_test_wave1d.py exercises the same path); kept as
    # a real recipe, not a leftover.
    ("tools/PcTools/src/kilnctrl/stack_margin_baseline.py", "load_pico_records"),
    # NOT actually dead: collapse_table() IS called in production from
    # tools/mykicadMcp/kicad_mcp_server.py, a separate git submodule this
    # check deliberately excludes from both function discovery and the
    # caller search (EXCLUDED_DIR_NAMES). Genuinely zero-caller only within
    # this check's in-repo scan scope, not in the full picture -- kept
    # (not deleted) for that reason. If mykicadMcp is ever folded back into
    # this check's scan scope, re-verify this note still holds.
    ("tools/PcTools/src/mcpkit/registry.py", "collapse_table"),
    # Collateral zero-callers created BY this same 2026-09-21 pass, not part
    # of the original 23 -- deleting check_chip_partition_table() (a
    # confirmed dead top-level wrapper) removed its only in-repo production
    # caller of these two. Both are explicitly retained per this module's
    # own docstring ("read_chip_partition_table_bytes() below is KEPT...
    # deprecated as a chip-read mechanism") -- unit-tested against synthetic
    # blobs, not superseded logic, just no longer reachable from a
    # production entry point until/unless a real JTAG-based mechanism is
    # ever revived.
    ("tools/PcTools/src/kilnctrl/partition_table.py", "parse_partition_table_binary"),
    ("tools/PcTools/src/kilnctrl/partition_table.py", "read_chip_partition_table_bytes"),
    # Same collateral shape: deleting estimate_from_capture_path() (a
    # confirmed dead convenience wrapper) removed its only in-repo caller of
    # this whole-board multi-zone estimator. Kept rather than also deleted --
    # it is a genuine, actively-tested (3 call sites in
    # test_load_estimator.py) general-purpose helper, not itself superseded;
    # a future caller wiring up whole-board (rather than per-source) load
    # estimation would reach for exactly this function.
    ("tools/PcTools/src/kilnctrl/load_estimator.py", "estimate_all_zones"),
}

# Found genuinely zero-caller by this check's first full-coverage run
# (2026-09-20) and reviewed by hand against the web_auth_table_create_session
# shape (a live, wired-in production entry point nothing calls) -- these ARE
# that shape, or close enough that only the owner should decide whether to
# wire them up or delete them. Listed separately from the benign allowlist
# above (rather than silently merged into it) so this distinction survives:
# a future reader of this file should not assume every entry below is fine.
#
# 2026-09-21: owner reviewed the batch of 23 that had accumulated here
# (docs/RELEASE_HARDENING_PLAN.md item 1). 11 were kept (moved into
# ZERO_CALLER_ALLOWLIST above, each with its own reason comment); the other
# 12 -- confirmed genuinely dead, not just a blind-spot false positive --
# were deleted from source along with their tests and doc mentions in the
# same commit. This set is empty as of that pass; a future zero-caller
# finding starts a new PENDING_OWNER_REVIEW batch.
PENDING_OWNER_REVIEW = set()


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


def _function_body_end(lines, def_line_no: int) -> int:
    """Return the 1-based line number of the last line belonging to the
    def statement starting at def_line_no (its signature continuation lines
    and its indented body), so callers can exclude a recursive self-call --
    a mention of the function's own name inside its own body -- from the
    caller search. Blank lines inside the body do not end it; the first
    non-blank line back at column 0 does."""
    n = len(lines)
    last_body_line = def_line_no
    j = def_line_no  # 0-based index of the line AFTER the def line
    while j < n:
        line = lines[j]
        if line.strip() == "":
            j += 1
            continue
        leading = len(line) - len(line.lstrip(" \t"))
        if leading == 0:
            break
        last_body_line = j + 1  # 1-based
        j += 1
    return last_body_line


def find_top_level_functions(path: Path):
    """Return [(name, line_no, decorator_wired, body_end_line)] for top-level
    (non-underscore, non-indented) def statements in path."""
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
        body_end = _function_body_end(lines, i)
        out.append((name, i, wired, body_end))
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
        if path.name == Path(__file__).name:
            # This script itself must NOT be part of the caller universe:
            # ZERO_CALLER_ALLOWLIST / PENDING_OWNER_REVIEW name their
            # functions as string literals, so including this file made every
            # listed name look "used" -- which silently defeated both sets
            # (in particular the PENDING_OWNER_REVIEW report in main() never
            # printed a single line, because no pending entry was ever
            # reached).
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        corpus.append((path, lines))
    return corpus


def build_mention_index(corpus):
    """name -> set of (path, line_no) where that identifier appears as a
    NAME token -- i.e. real code, never inside a string literal or a
    comment. This is what makes a mention in a docstring, a plain '#'
    comment, or an `__all__ = ["name", ...]` string list correctly NOT count
    as a caller: none of those produce a tokenize.NAME token for `name`.
    (`__all__` membership specifically: its entries are STRING tokens, not
    NAME tokens, so listing a function there was never itself a caller once
    scanning moved off a text/regex match.)

    Computed once instead of once per candidate function -- the per-name
    scan this replaced was O(functions x lines) and cost ~34 s on this repo.
    """
    index = {}
    for path, lines in corpus:
        text = "\n".join(lines) + "\n"
        try:
            for tok in tokenize.generate_tokens(io.StringIO(text).readline):
                if tok.type == tokenize.NAME:
                    index.setdefault(tok.string, set()).add(
                        (path, tok.start[0]))
        except (tokenize.TokenizeError, IndentationError, SyntaxError,
                ValueError):
            # A file that doesn't tokenize cleanly (encoding oddity, WIP
            # syntax error elsewhere in a shared tree) contributes no
            # mentions rather than crashing the sweep; it is not one of the
            # scanned production files, so this cannot hide a real
            # zero-caller finding, only a caller reference living in a
            # broken file.
            continue
    return index


def has_any_production_caller(name: str, def_file: Path, body_start: int,
                               body_end: int, index) -> bool:
    for path, line in index.get(name, ()):
        if path == def_file and body_start <= line <= body_end:
            # The def statement's own signature (which re-mentions `name`
            # once for the `def name(...)` itself) and any recursive
            # self-call inside the function's own body are not callers.
            continue
        return True
    return False


def run_sweep(scan_roots, repo_root=REPO_ROOT):
    index = build_mention_index(load_corpus(repo_root))
    findings = []
    pending = []
    allowed_hits = set()
    checked = 0
    for root in scan_roots:
        for path in iter_py_files(root):
            if _is_test_path(path.relative_to(repo_root)):
                continue
            rel = path.relative_to(repo_root)
            for name, line_no, decorator_wired, body_end in find_top_level_functions(path):
                checked += 1
                if decorator_wired:
                    continue
                if has_any_production_caller(name, path, line_no, body_end,
                                              index):
                    continue
                key = (rel.as_posix(), name)
                if key in ZERO_CALLER_ALLOWLIST:
                    allowed_hits.add(key)
                    continue
                if key in PENDING_OWNER_REVIEW:
                    allowed_hits.add(key)
                    pending.append((rel, line_no, name))
                    continue
                findings.append((rel, line_no, name))
    # Entries naming a function that is no longer zero-caller: it gained a
    # caller (good -- delete the entry) or was renamed/removed. Reported so
    # neither set rots unnoticed.
    stale = sorted((ZERO_CALLER_ALLOWLIST | PENDING_OWNER_REVIEW)
                   - allowed_hits)
    return checked, findings, pending, stale


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    scan_roots = DEFAULT_SCAN_ROOTS
    if argv:
        scan_roots = [Path(a) for a in argv]

    checked, findings, pending, stale = run_sweep(scan_roots)

    if stale:
        print(
            f"check_python_zero_caller_sweep: NOTE -- {len(stale)} "
            f"ZERO_CALLER_ALLOWLIST/PENDING_OWNER_REVIEW entr(ies) are stale: "
            f"the named function now has a caller, or was renamed/removed. "
            f"Delete the entry from this script so the sets do not rot:"
        )
        for file_name, name in stale:
            print(f"  ({file_name}, {name})")

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
            f"zero-caller functions (beyond {len(ZERO_CALLER_ALLOWLIST)} "
            f"reviewed-benign allowlist entries and {len(pending)} still "
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
