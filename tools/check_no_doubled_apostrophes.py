#!/usr/bin/env python3
"""check_no_doubled_apostrophes.py -- standing guard against the class of
artifact fixed in commit ee0b8754 (firmware/KilnFW/TODO.md, 39 instances).

WHY THIS EXISTS. An agent writing prose through a PowerShell single-quoted
string escapes a literal apostrophe by doubling it (`''`), which is correct
PowerShell syntax but wrong once that string lands verbatim in Markdown
prose: `doesn''t` instead of `doesn't`, `` `file.h`''s `` instead of
`` `file.h`'s ``. Nothing caught this mechanically before ee0b8754 hand-fixed
all 39 instances; this check makes a reintroduction a build-time failure
instead of a silent doc defect.

What counts as an offense: a `''` (two adjacent single-quote/apostrophe
characters) that is either immediately adjacent to a word character (so it
reads as a contraction/possessive artifact: `doesn''t`, `word''s`) or
immediately follows a backtick (`` `''s ``, the closing-backtick-then-
possessive shape ee0b8754 fixed repeatedly) -- found OUTSIDE fenced code
blocks (``` ... ```) and OUTSIDE inline code spans (`` `...` ``), since a
literal `''` (an empty string literal, or two adjacent code spans) is
legitimate there.

Scan scope: every `*.md` file under the repo root passed in (or discovered
from this file's own location), excluding `.git`, `.venv`, and build/output
directories. This check is run against whatever tree it is invoked from --
usually a `C:/wt/<label>_<hash>` worktree, not the shared main tree -- so
there is no separate exclusion for worktree paths: `rglob` never yields
anything outside the scanned root regardless of where that root itself
lives on disk.

Usage: python tools/check_no_doubled_apostrophes.py [--root REPO_ROOT]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

EXCLUDE_DIR_NAMES = {
    ".git", ".venv", "venv", "node_modules", "build", "build_agent",
    "__pycache__", ".pytest_cache",
}

# A run of two or more single quotes / apostrophes. We only care about the
# first two of a run (three or more is the same artifact, just worse).
DOUBLED_RE = re.compile(r"''")

# Offense shapes, checked against the ORIGINAL (unmasked) line text at the
# match position:
#   1. '' adjacent to a word character on either side: doesn''t, word''s,
#      ''tis, can''t
#   2. '' immediately preceded by a backtick: `''s (closing code span then
#      doubled apostrophe -- the `file.h`''s shape)
# A legitimate empty-string-literal '' (e.g. inside prose describing code,
# but OUTSIDE any code span/fence -- rare, but possible as `''`  without
# backticks) would still match #1 only if adjacent to a word char, which an
# empty string literal in plain English prose essentially never is (it would
# read as ''  with punctuation/space around it, not letters).


def _is_offense(line: str, start: int, end: int) -> bool:
    before = line[start - 1] if start > 0 else ""
    after = line[end] if end < len(line) else ""
    if before == "`":
        return True
    if before.isalnum() or after.isalnum():
        return True
    return False


def _mask_fenced_blocks(lines: list[str]) -> list[bool]:
    """Return a per-line bool: True if the line is inside a fenced code block
    (``` or ~~~ delimited), including the fence lines themselves."""
    in_fence = False
    fence_marker = None
    masked = [False] * len(lines)
    fence_re = re.compile(r"^\s*(```+|~~~+)")
    for i, line in enumerate(lines):
        m = fence_re.match(line)
        if m:
            marker = m.group(1)[0] * 3  # normalize to the leading char x3
            if not in_fence:
                in_fence = True
                fence_marker = marker
                masked[i] = True
            elif marker[0] == fence_marker[0] if fence_marker else False:
                masked[i] = True
                in_fence = False
                fence_marker = None
            else:
                masked[i] = True
            continue
        masked[i] = in_fence
    return masked


def _mask_inline_code_spans(line: str) -> str:
    """Blank out the contents of inline code spans (`...`) so a '' inside one
    is never flagged, while preserving string length/positions so column
    offsets on the surrounding text stay correct."""

    def _blank(m: re.Match) -> str:
        s = m.group(0)
        return s[0] + " " * (len(s) - 2) + s[-1]

    # Inline code spans: `...` (single backtick, no internal backtick) --
    # sufficient for this repo's Markdown style (double-backtick spans for
    # literal-backtick content are rare/absent in current docs).
    return re.sub(r"`[^`\n]*`", _blank, line)


def find_files(root: Path) -> list[Path]:
    # NOTE: `root.rglob(...)` only ever yields paths *under* `root` -- so a
    # blanket "exclude anything under C:\wt" check here would blind this
    # scanner every time it is run against a worktree (which is the normal
    # way checks run in this repo: every implementer works in a
    # `C:\wt\<label>_<hash>` worktree, never the shared main tree). There is
    # nothing to additionally exclude for "C:\wt" beyond what EXCLUDE_DIR_NAMES
    # already covers, since rglob can't escape `root` regardless of where
    # `root` itself lives.
    out: list[Path] = []
    for path in root.rglob("*.md"):
        try:
            rel_parts = path.relative_to(root).parts
        except ValueError:
            continue
        if any(part in EXCLUDE_DIR_NAMES for part in rel_parts):
            continue
        out.append(path)
    return sorted(out)


def check_file(path: Path) -> list[str]:
    try:
        raw = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return []
    lines = raw.splitlines()
    fenced = _mask_fenced_blocks(lines)
    violations: list[str] = []
    for i, line in enumerate(lines):
        if fenced[i]:
            continue
        masked = _mask_inline_code_spans(line)
        for m in DOUBLED_RE.finditer(masked):
            if _is_offense(line, m.start(), m.end()):
                snippet = line.strip()
                if len(snippet) > 120:
                    snippet = snippet[:117] + "..."
                violations.append(f"{path}:{i + 1}: {snippet}")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=None, help="repo root (default: two levels up from this file)"
    )
    args = parser.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[1]

    violations: list[str] = []
    for path in find_files(root):
        violations.extend(check_file(path))

    if violations:
        print("check_no_doubled_apostrophes: doubled-apostrophe artifact(s) found:")
        for v in violations:
            print(f"  {v}")
        print(
            "  This is the PowerShell single-quote-escaping artifact fixed in ee0b8754 -- "
            "a literal apostrophe written through a PowerShell single-quoted string became "
            "'' instead of '. Fix the source text to a single apostrophe."
        )
        return 1

    print(
        "check_no_doubled_apostrophes: OK -- no doubled-apostrophe artifacts found in any "
        "*.md file outside fenced code blocks or inline code spans."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
