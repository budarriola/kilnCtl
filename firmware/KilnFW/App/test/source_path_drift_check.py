#!/usr/bin/env python3
"""source_path_drift_check.py -- 2026-09-04's twelve large-file splits were
clean, but NINE separate checks/tests broke SILENTLY because they key on a
source-file PATH that a split moved or renamed: flash_worker_lint.py's
allowlist, check_link_impl_isolation.ps1's allowlist (zones_config_json.c
after compute_crc() moved to zones_config_migrate.c), check_heat_enable_
wiring.ps1's singular glob (autotune_engine.c after the function moved to
autotune_engine_guard.c), test_autotune_wire_layout.py (moved file AND
pre-rename identifiers), test_autotune_rules_drift_guard.py (dashboard_
http.c moved), and -- the worst case -- test_selfcheck_zones_fields.py,
which guarded its hardcoded path with `if not path.is_file(): skipTest(...)`
so the split turned it into a SILENTLY SKIPPING green test with zero real
coverage. Build and host tests stayed green through all nine; each was found
one at a time, by accident.

THIS CHECK statically scans the places these hardcoded references live --
tools/*.ps1, firmware/*/tools/*.ps1, firmware/KilnFW/App/test/*.py, and
tools/PcTools/tests/*.py -- for source-file path references and FAILS,
naming the referencing file:line and the missing path, whenever the
referenced file does not exist anywhere it plausibly should.

WHAT COUNTS AS A "PATH REFERENCE" (the false-positive rule, per this check's
own mandate -- comments and docstrings must not fail the build):
  1. Comments and docstrings are stripped BEFORE any extraction regex runs
     (PowerShell `<# #>` blocks and `#` line comments; Python triple-quoted
     strings and `#` line comments). A source filename mentioned only in
     prose ("uart_bridge_ext.c's own header comment...") never reaches the
     extraction step.
  2. Of what remains, only two shapes are treated as a real reference:
       a. PATH CONSTRUCTION: a PowerShell `Join-Path <root> "<literal>"`
          call, or a Python chain of `/`-joined string literals
          (`_REPO_ROOT / "firmware" / "..." / "foo.c"`) -- i.e. code that
          is actually building a filesystem path, not just naming a file
          in an error message.
       b. BARE ALLOWLIST ENTRY: a line whose entire trimmed content is one
          quoted `name.c`/`name.h` literal (optionally trailing comma) --
          exactly flash_worker_lint.py's ALLOWLIST-of-filenames style. A
          filename mentioned mid-sentence in an error string does NOT match
          this shape and is not flagged.
     A wildcard component (`*`, `?`) marks a glob pattern, not a concrete
     path, and is never checked for existence.
  3. A PATH CONSTRUCTION literal is resolved against a fixed list of
     plausible base directories (repo root, firmware/, firmware/KilnFW/App/,
     firmware/KilnFW/App/drivers/, firmware/SaftyFW/, firmware/SaftyFW/src/,
     firmware/CommonFW/, tools/, tools/PcTools/, tools/PcTools/tests/) since
     this script does not evaluate the PowerShell/Python variable the
     literal was actually joined onto -- if the literal resolves under ANY
     of those bases it is accepted, which trades a little precision for
     zero false positives from an unresolved root variable.
  4. A BARE ALLOWLIST ENTRY is resolved by searching for that exact
     filename anywhere under firmware/ or tools/ (excluding build output,
     node_modules, and dotted directories).

SKIP-GUARD VARIANT (the test_selfcheck_zones_fields.py case): a Python file
that both (a) has at least one PATH CONSTRUCTION reference this check
resolved as MISSING, and (b) contains an `if not <expr>.is_file(): skip...`
or `skipTest(`/`pytest.skip(` guard anywhere, gets an escalated message
calling out the pattern by name. Narrowed to "guarded path does not
currently exist" rather than "any skip guard at all" -- flagging every
skipTest/pytest.skip call in the test suite would be mostly noise (skips
guard plenty of legitimately-optional things, e.g. "board not connected");
the dangerous combination is specifically a hardcoded path that is both
missing AND silently downgraded to a skip instead of a failure.

FAIL CLOSED: if the extraction step finds fewer than MIN_REFERENCES total
path references across every scanned file, that means the regexes stopped
matching reality (a directory moved, a coding style changed) -- this is
itself a failure, not zero findings to report cleanly.

Usage: python source_path_drift_check.py [repo_root]
Exit 0: every extracted reference resolves to a real file, extraction found
        a healthy number of references, and no skip-guard-over-missing-path
        pattern was found.
Exit 1: otherwise (missing path, skip-guard-over-missing-path, or the
        fail-closed extraction floor was not met).
"""
import re
import subprocess
import sys
from pathlib import Path

SOURCE_EXTS = (".c", ".h", ".py", ".ps1")

# Fail-closed floor: today's four glob targets contain well over 100 Join-
# Path / "/"-chain / bare-allowlist references combined. If the extraction
# regexes below ever find fewer than this, something about the scanned
# files' coding style changed out from under this check -- report that as
# a failure, not a clean pass over nothing.
MIN_REFERENCES = 40

BASES = [
    "",
    "firmware",
    "firmware/KilnFW/App",
    "firmware/KilnFW/App/drivers",
    "firmware/KilnFW/App/test",
    "firmware/SaftyFW",
    "firmware/SaftyFW/src",
    "firmware/SaftyFW/tools",
    "firmware/CommonFW",
    "firmware/CommonFW/include",
    "firmware/CommonFW/include/kilnlink",
    "tools",
    "tools/PcTools",
    "tools/PcTools/tests",
    "tools/PcTools/src",
    "tools/PcTools/src/kilnctrl",
]

BARE_FILENAME_LINE_RE = re.compile(r'^\s*"([\w][\w.\-]*\.(?:c|h))"\s*,?\s*$')
PY_TRIPLE_STRING_RE = re.compile(r'("""[\s\S]*?"""|\'\'\'[\s\S]*?\'\'\')')
PS_BLOCK_COMMENT_RE = re.compile(r'<#[\s\S]*?#>')
PY_CHAIN_RE = re.compile(
    r'(?:[A-Za-z_][\w.]*|\)|\])\s*(?:/\s*"([^"/\\]+)"\s*)+'
)
PY_CHAIN_LITERALS_RE = re.compile(r'"([^"/\\]+)"')
PS_JOIN_PATH_RE = re.compile(r'Join-Path\s+\S+\s+"([^"]+)"')
SKIP_GUARD_RE = re.compile(
    r'if\s+not\s+[\w.\(\)_]+\.is_file\(\)\s*:\s*[\r\n]+\s*(?:self\.)?(?:skipTest|skip)\('
    r'|(?:self\.)?skipTest\('
    r'|pytest\.skip\('
)


def strip_comments_py(text: str) -> str:
    text = PY_TRIPLE_STRING_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out_lines = []
    for line in text.splitlines():
        # Naive: cut at the first '#'. Path literals in this codebase never
        # contain '#', so this does not truncate a real reference.
        idx = line.find("#")
        out_lines.append(line if idx < 0 else line[:idx])
    return "\n".join(out_lines)


def strip_comments_ps1(text: str) -> str:
    text = PS_BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out_lines = []
    for line in text.splitlines():
        idx = line.find("#")
        out_lines.append(line if idx < 0 else line[:idx])
    return "\n".join(out_lines)


def is_source_literal(lit: str) -> bool:
    if "*" in lit or "?" in lit:
        return False
    return lit.endswith(SOURCE_EXTS)


def resolve_under_bases(repo_root: Path, rel_literal: str) -> bool:
    norm = rel_literal.replace("\\", "/")
    for base in BASES:
        candidate = (repo_root / base / norm) if base else (repo_root / norm)
        if candidate.is_file():
            return True
    # firmware/KilnFW/App/drivers/ is being split into layer subdirectories
    # (drivers/<layer>/<name>) -- a literal like "pid.c" or a chain that
    # used to resolve directly under the flat "firmware/KilnFW/App/drivers"
    # BASES entry would otherwise start reporting as missing the moment its
    # file moves one level deeper, even though nothing about the reference
    # is actually stale. Try every real subdirectory of drivers/ too, found
    # dynamically rather than a hardcoded layer-name list so this keeps
    # working regardless of which/how many layers end up existing.
    drivers_dir = repo_root / "firmware" / "KilnFW" / "App" / "drivers"
    if drivers_dir.is_dir():
        for sub in drivers_dir.iterdir():
            if sub.is_dir() and (sub / norm).is_file():
                return True
    return False


def get_tracked_files(repo_root: Path) -> set:
    """Concurrent sessions are the norm in this repo: another agent's
    in-flight negative-test/mutant run can drop a stray file (e.g. a
    fake_bypass_script.py fixture) anywhere under firmware/ or tools/,
    including directly inside one of this check's own scanned globs
    (firmware/KilnFW/App/test/*.py). That file is real on disk for only as
    long as the other agent's own run needs it -- it is not a source-path
    reference this project actually ships, so it must never be treated as
    a scan TARGET nor count as evidence a referenced filename exists.
    Restricting both to `git ls-files` makes this check see the same
    checked-in tree every session sees, regardless of what else is
    mid-flight on disk right now."""
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "ls-files", "--", "firmware", "tools"],
            capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(
            f"source_path_drift_check.py: 'git ls-files' failed ({exc}) -- "
            f"cannot determine which files are tracked, refusing to fall "
            f"back to scanning untracked/stray files on disk"
        ) from exc
    return {repo_root / line for line in out.stdout.splitlines() if line}


def build_filename_index(repo_root: Path, tracked_files: set) -> dict:
    index = {}
    for top in ("firmware", "tools"):
        top_dir = repo_root / top
        if not top_dir.is_dir():
            continue
        for p in top_dir.rglob("*"):
            if not p.is_file():
                continue
            parts = p.relative_to(repo_root).parts
            if any(part in ("build", "node_modules") or part.startswith(".") for part in parts):
                continue
            if p not in tracked_files:
                continue  # untracked -- another agent's in-flight file?
            index.setdefault(p.name, []).append(p)
    return index


def scan_python_file(repo_root: Path, path: Path, filename_index: dict, refs_counter: list, problems: list):
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_comments_py(raw)
    rel = path.relative_to(repo_root).as_posix()

    any_missing = False

    for m in PY_CHAIN_RE.finditer(text):
        literals = PY_CHAIN_LITERALS_RE.findall(m.group(0))
        if not literals:
            continue
        last = literals[-1]
        if not is_source_literal(last):
            continue
        refs_counter[0] += 1
        joined = "/".join(literals)
        line_no = text.count("\n", 0, m.start()) + 1
        if not resolve_under_bases(repo_root, joined) and not resolve_under_bases(repo_root, last):
            any_missing = True
            problems.append(
                f"{rel}:{line_no}: references '{joined}' (source file '{last}') "
                f"which does not exist under repo root or any known base directory"
            )

    for line_no, line in enumerate(text.splitlines(), start=1):
        bm = BARE_FILENAME_LINE_RE.match(line)
        if not bm:
            continue
        name = bm.group(1)
        refs_counter[0] += 1
        if name not in filename_index:
            any_missing = True
            problems.append(
                f"{rel}:{line_no}: bare allowlist entry '{name}' does not match any "
                f"file under firmware/ or tools/ -- renamed or deleted out from under "
                f"this allowlist"
            )

    if any_missing and SKIP_GUARD_RE.search(text):
        problems.append(
            f"{rel}: ALSO contains a skipTest()/pytest.skip()/'if not ...is_file()' "
            f"skip guard in a file with a missing hardcoded path above -- this is the "
            f"test_selfcheck_zones_fields.py pattern: a broken hardcoded path silently "
            f"downgrades to a green SKIP instead of a red FAIL, so a split can remove "
            f"all real coverage without the suite ever going red"
        )


def scan_ps1_file(repo_root: Path, path: Path, refs_counter: list, problems: list):
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_comments_ps1(raw)
    rel = path.relative_to(repo_root).as_posix()

    for m in PS_JOIN_PATH_RE.finditer(text):
        lit = m.group(1)
        if not is_source_literal(lit):
            continue
        refs_counter[0] += 1
        line_no = text.count("\n", 0, m.start()) + 1
        if not resolve_under_bases(repo_root, lit):
            problems.append(
                f"{rel}:{line_no}: Join-Path references '{lit}' which does not exist "
                f"under repo root or any known base directory"
            )


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    targets = []
    targets += sorted((repo_root / "tools").glob("*.ps1"))
    targets += sorted(repo_root.glob("firmware/*/tools/*.ps1"))
    targets += sorted((repo_root / "firmware" / "KilnFW" / "App" / "test").glob("*.py"))
    pctools_tests = repo_root / "tools" / "PcTools" / "tests"
    if pctools_tests.is_dir():
        targets += sorted(pctools_tests.glob("*.py"))

    if not targets:
        print("SOURCE PATH DRIFT CHECK: FAILED")
        print("  no target files discovered at all under tools/*.ps1, "
              "firmware/*/tools/*.ps1, firmware/KilnFW/App/test/*.py, "
              "tools/PcTools/tests/*.py -- fail closed, this almost certainly "
              "means a directory moved or the globs above are wrong")
        return 1

    try:
        tracked_files = get_tracked_files(repo_root)
    except RuntimeError as exc:
        print("SOURCE PATH DRIFT CHECK: FAILED")
        print(f"  {exc}")
        return 1

    untracked_targets = [t for t in targets if t not in tracked_files]
    targets = [t for t in targets if t in tracked_files]
    for t in untracked_targets:
        print(
            f"SOURCE PATH DRIFT CHECK: skipping untracked target "
            f"{t.relative_to(repo_root).as_posix()} -- another agent's "
            f"in-flight file?"
        )

    if not targets:
        print("SOURCE PATH DRIFT CHECK: FAILED")
        print("  every discovered target file is untracked -- fail closed, "
              "this almost certainly means nothing real is checked in under "
              "the scanned globs")
        return 1

    filename_index = build_filename_index(repo_root, tracked_files)
    refs_counter = [0]
    problems: list[str] = []

    for path in targets:
        if path.suffix == ".py":
            scan_python_file(repo_root, path, filename_index, refs_counter, problems)
        elif path.suffix == ".ps1":
            scan_ps1_file(repo_root, path, refs_counter, problems)
            # Bare allowlist entries for .ps1 files: re-scan with the index.
            raw = path.read_text(encoding="utf-8", errors="replace")
            text = strip_comments_ps1(raw)
            rel = path.relative_to(repo_root).as_posix()
            for line_no, line in enumerate(text.splitlines(), start=1):
                bm = BARE_FILENAME_LINE_RE.match(line)
                if not bm:
                    continue
                name = bm.group(1)
                refs_counter[0] += 1
                if name not in filename_index:
                    problems.append(
                        f"{rel}:{line_no}: bare allowlist entry '{name}' does not "
                        f"match any file under firmware/ or tools/ -- renamed or "
                        f"deleted out from under this allowlist"
                    )

    if refs_counter[0] < MIN_REFERENCES:
        print("SOURCE PATH DRIFT CHECK: FAILED")
        print(
            f"  only {refs_counter[0]} path reference(s) extracted across "
            f"{len(targets)} scanned file(s), below the fail-closed floor of "
            f"{MIN_REFERENCES}. This means the extraction regexes stopped "
            f"matching reality (a coding style changed, a directory moved) -- "
            f"update source_path_drift_check.py rather than trusting a quiet "
            f"pass over near-zero findings."
        )
        return 1

    if problems:
        print("SOURCE PATH DRIFT CHECK: FAILED")
        for p in problems:
            print(f"  {p}")
        return 1

    print(
        f"SOURCE PATH DRIFT CHECK: {refs_counter[0]} source-file path "
        f"reference(s) across {len(targets)} scanned file(s) all resolve to "
        f"real files; no skip-guard-over-missing-path pattern found."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
