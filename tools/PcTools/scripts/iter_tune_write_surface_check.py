#!/usr/bin/env python3
"""Guard against `iter_tune`'s decision core getting a write surface it is
not supposed to have, and against it getting silently wired into the
firing pipeline before the owner has signed off on that.

Background: docs/ITER_TUNE_REDESIGN_PLAN.md sec 5 ("Safety and
containment"). The module's ONLY sanctioned write path into board state is
the caller invoking `zones_config_set_pid(zone, kp, ki, kd)` with the gains
`iter_tune_active_gains()` returned -- `iter_tune.c` itself never calls any
setter, persistence function, or hardware/NVS API. It is pure decision
logic (gains in, verdict/gains out), and per sec 9.3 ("bench fixture only
for now") it must not be reachable from `profile_executor.c` or any other
production caller until the owner explicitly wires it in.

This script enforces both halves mechanically:

1. **No write surface inside iter_tune.c/.h.** Any call to a persistence/
   hardware setter (zones_config_set_*, nvs_set*, *_write(, any ESP-IDF or
   FreeRTOS symbol) inside the two iter_tune source files is forbidden.
   `zones_config_set_pid` is even named in a comment today (the caller's
   documented job) -- that is fine; only an actual call `zones_config_set_pid(`
   is forbidden, so the docstring lives on lines this check skips.
2. **Still unwired.** No production file (anything under
   firmware/KilnFW/App, excluding firmware/KilnFW/App/test and iter_tune.c/.h
   themselves) may call any iter_tune_* function. A prose/doc-comment mention
   of the name (e.g. "iter_tune_process_firing() is deliberately not wired in
   by this commit") is allowed; only an actual call -- the symbol immediately
   followed by `(` -- trips this.

Per this repo's standing rule (check_ct_cal_write_surface.ps1 et al.), a
missing file or missing python FAILS this check, not a silent skip.

Usage: python iter_tune_write_surface_check.py <repo_root>
Exit 0 = clean, 1 = a violation was found.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ITER_TUNE_FILES = [
    "firmware/KilnFW/App/drivers/control/iter_tune.c",
    "firmware/KilnFW/App/drivers/control/iter_tune.h",
]

# Any call matching one of these regexes inside iter_tune.c/.h is a write
# surface iter_tune must not have. Matched as a live call (name immediately
# followed by "("), not a bare substring, so comments/docstrings naming
# zones_config_set_pid as the CALLER's job are not flagged.
FORBIDDEN_CALL_PATTERNS = [
    re.compile(r"\bzones_config_set_\w*\s*\("),
    re.compile(r"\bnvs_set\w*\s*\("),
    re.compile(r"\bnvs_commit\s*\("),
    re.compile(r"\b\w*_write\s*\("),
    re.compile(r"\bxTaskCreate\w*\s*\("),
    re.compile(r"\bgpio_set_level\s*\("),
]

# Root to scan for wiring; test/ is excluded because host tests are
# SUPPOSED to call iter_tune_* directly.
PRODUCTION_ROOT = "firmware/KilnFW/App"
EXCLUDED_DIRS = {"test", "build"}
EXCLUDED_FILES = set(ITER_TUNE_FILES)

ITER_TUNE_CALL_RE = re.compile(r"\biter_tune_\w+\s*\(")


def _strip_c_comments(text: str) -> str:
    # Full-file // and /* */ stripper (not line-local): this repo's own
    # iter_tune-adjacent prep comments (profile_executor_internal.h) name
    # iter_tune_* functions prose-style inside /* ... */ blocks spanning
    # several lines, which a line-local "//"-only strip does not see.
    # Comment bodies are replaced with spaces, not removed, so line numbers
    # in later reporting stay aligned with the original file.
    out = []
    i = 0
    n = len(text)
    in_block = False
    in_line = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_block:
            if c == "*" and nxt == "/":
                out.append("  ")
                i += 2
                in_block = False
                continue
            out.append(c if c == "\n" else " ")
            i += 1
            continue
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            in_block = True
            continue
        if c == "/" and nxt == "/":
            out.append("  ")
            i += 2
            in_line = True
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_no_write_surface(repo_root: Path) -> list[str]:
    failures: list[str] = []
    for rel in ITER_TUNE_FILES:
        path = repo_root / rel
        if not path.is_file():
            failures.append(f"{rel}: file not found -- check is stale, update ITER_TUNE_FILES")
            continue
        stripped = _strip_c_comments(path.read_text(encoding="utf-8"))
        for lineno, line in enumerate(stripped.splitlines(), start=1):
            for pattern in FORBIDDEN_CALL_PATTERNS:
                if pattern.search(line):
                    failures.append(
                        f"{rel}:{lineno}: forbidden write-surface call {pattern.pattern!r} -- "
                        "iter_tune.c must never call a setter/persistence/hardware API itself "
                        "(ITER_TUNE_REDESIGN_PLAN.md sec 5)"
                    )
    return failures


def check_unwired(repo_root: Path) -> list[str]:
    failures: list[str] = []
    prod_root = repo_root / PRODUCTION_ROOT
    if not prod_root.is_dir():
        return [f"{PRODUCTION_ROOT}: directory not found -- check is stale"]
    for path in prod_root.rglob("*"):
        if not path.is_file() or path.suffix not in (".c", ".h"):
            continue
        if any(part in EXCLUDED_DIRS for part in path.relative_to(prod_root).parts):
            continue
        rel = path.relative_to(repo_root).as_posix()
        if rel in EXCLUDED_FILES:
            continue
        stripped = _strip_c_comments(path.read_text(encoding="utf-8"))
        for lineno, line in enumerate(stripped.splitlines(), start=1):
            if ITER_TUNE_CALL_RE.search(line):
                failures.append(
                    f"{rel}:{lineno}: calls an iter_tune_* function -- iter_tune is bench-fixture-"
                    "only and not yet owner-approved to be wired into production (sec 9.3); "
                    "if this is the deliberate wiring commit, update this check's scope alongside it"
                )
    return failures


def check(repo_root: Path) -> list[str]:
    return check_no_write_surface(repo_root) + check_unwired(repo_root)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: iter_tune_write_surface_check.py <repo_root>", file=sys.stderr)
        return 2
    repo_root = Path(argv[1]).resolve()
    failures = check(repo_root)
    if failures:
        print("ITER_TUNE WRITE SURFACE CHECK: FAILED")
        for f in failures:
            print(f"  {f}")
        return 1
    print("ITER_TUNE WRITE SURFACE CHECK: ok -- no setter calls inside iter_tune.c/.h, "
          "and no production caller wires it in")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
