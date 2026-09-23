#!/usr/bin/env python3
"""check_no_exec_status_stack_locals.py -- guard against a `profile_exec_status_t`
automatic (stack) variable in firmware/KilnFW/App's httpd handlers or persist
layer.

WHY THIS EXISTS. `profile_exec_status_t` is 1384 bytes
(firmware/KilnFW/App/drivers/control/profile_executor.h). Its own doc comment
warns against materializing a copy on the stack of a tight-stack task, and
safety_cfg_http.c already heap-allocates it for exactly that reason
(`heap_caps_malloc(sizeof(*p), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`, freed
on every return path). A 2026-09-22 audit found a NEW stack local of this
type in a persist-layer function reachable from the httpd task
(telemetry_log.c's telemetry_log_task -- not httpd-reachable itself, but in
scope below since it lives under drivers/persist). The full audit this
check came out of scanned every `profile_exec_status_t` stack local under
firmware/KilnFW/App and found thirteen sites total (nine in the first scan,
broadened to the whole drivers/ tree after that scan proved the pattern
recurs across module boundaries, which then surfaced four more), spread
across drivers/http (dashboard_exec_http.c x3, ota_http.c,
profiles_edit_http.c), drivers/persist (telemetry_log.c), drivers/bridge
(uart_bridge_ext_control.c, boot_button.c -- the latter list-only, see
below), drivers/safety (danger_mode.c, safety_link_frames.c -- the latter
on safety_poll_task's own PSRAM-backed 8192 B stack) and drivers/ui
(ui_page_home_actions.c x2, ui_page_home_refresh.c, ui_page_profile_detail.c,
screen_idle.c) -- every one on a tight-stack task (httpd 8 KB,
bx_flash_worker, safety_poll_task, screen_idle_task, the lvgl task -- which
turned out to be one of the tightest tasks in the codebase, not a generous
one, see ui_page_home_refresh.c's own header comment on its measured stack
ceiling -- or a small dedicated task) was converted, either to the same
heap-alloc pattern or to the narrower `profile_executor_get_active_id()`
accessor (profile_executor.h) when only "is a firing active" is needed.
Nothing mechanical stopped a FOURTEENTH one (or a reintroduction of one of
the thirteen just fixed) from landing later, which is what this check is
for. Scope is the whole drivers/ tree, not just the directories the first
sites happened to be found in, since the audit showed the pattern recurs
across module boundaries.

What counts as a violation: a `profile_exec_status_t` (or
`autotune_engine_status_t`-style array-of-it) declared as a plain automatic
variable -- `profile_exec_status_t foo;` or `profile_exec_status_t foo[N];`
-- anywhere under firmware/KilnFW/App/drivers/**/*.c. A pointer declaration
(`profile_exec_status_t *foo = heap_caps_malloc(...)`) is NOT flagged --
that is the fix, not the bug. A function PARAMETER of this type
(`void f(profile_exec_status_t *st)` or, more rarely, by value) is also not
flagged by the declaration regex below since parameters do not match the
"name;" / "name[N];" statement shape this scans for.

This is a source-text scan (like the repo's other check_*.py/ps1 guards),
not a compiler AST -- see the negative-test evidence in
docs/audits/ (or this check's own commit message) for what it does and does
not catch. It deliberately does not try to catch every possible C
declaration syntax (e.g. multiple declarators on one line,
`profile_exec_status_t a, b;`) since no such call site exists in this
codebase today and a regex broad enough to also catch that shape reliably
would risk false positives against unrelated multi-declarator lines.

Allowlist: EXPLICIT_ALLOWLIST below is a set of (relative-posix-path,
line-number) pairs for a stack local that has been reviewed and judged
acceptable to keep (e.g. a file confirmed host-test-only despite living
under one of the scanned directories, or a documented exception). It is
empty as of the audit that added this check -- every profile_exec_status_t
stack local found under drivers/http and drivers/persist was converted.

Usage: python tools/check_no_exec_status_stack_locals.py [--root REPO_ROOT]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SCAN_DIRS = ("firmware/KilnFW/App/drivers",)

#: (posix-relative-path-from-repo-root, line-number) pairs allowed to keep a
#: profile_exec_status_t stack local. Empty: every site found by the
#: 2026-09-22 audit was converted to heap-alloc or the narrow accessor.
EXPLICIT_ALLOWLIST: set[tuple[str, int]] = set()

#: Matches a plain automatic declaration of profile_exec_status_t, with or
#: without an array suffix, but NOT a pointer declaration (no leading `*`
#: after the type name) and not a cast/sizeof usage. Deliberately anchored
#: on `profile_exec_status_t` as a whole word so it does not also match
#: `profile_exec_status_t *` (heap-alloc, fine) or a comment mentioning the
#: type name in prose.
STACK_LOCAL_RE = re.compile(
    r"^\s*profile_exec_status_t\s+(?!\*)[A-Za-z_][A-Za-z0-9_]*(\[[^\]]*\])?\s*;"
)


def _strip_c_comments(text: str) -> str:
    # Block comments first (non-greedy, DOTALL), then line comments. Each
    # block comment is replaced by the same number of newlines it spanned so
    # line numbers reported below stay aligned with the original file.
    def _blank_block(m: re.Match) -> str:
        return "\n" * m.group(0).count("\n")

    text = re.sub(r"/\*.*?\*/", _blank_block, text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return text


def find_c_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for rel in SCAN_DIRS:
        base = root / rel
        if not base.exists():
            continue
        files.extend(sorted(base.rglob("*.c")))
    return files


def check_file(path: Path, root: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    stripped = _strip_c_comments(raw)
    lines = stripped.splitlines()
    rel = path.relative_to(root).as_posix()
    violations: list[str] = []
    for i, line in enumerate(lines):
        if not STACK_LOCAL_RE.match(line):
            continue
        line_no = i + 1
        if (rel, line_no) in EXPLICIT_ALLOWLIST:
            continue
        source_line = raw.splitlines()[i].strip() if i < len(raw.splitlines()) else line.strip()
        violations.append(
            f"{path}:{line_no}: profile_exec_status_t stack local (1384 bytes) "
            f"under firmware/KilnFW/App/drivers -- heap-allocate it "
            f"(heap_caps_malloc(sizeof(*p), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), "
            f"freed on every return path, matching safety_cfg_http.c) or, if only "
            f"the active-firing bool/id is needed, use "
            f"profile_executor_get_active_id() instead: {source_line}"
        )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=None, help="repo root (default: one level up from this file)"
    )
    args = parser.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[1]

    violations: list[str] = []
    for path in find_c_files(root):
        violations.extend(check_file(path, root))

    if violations:
        print("check_no_exec_status_stack_locals: profile_exec_status_t stack local(s) found:")
        for v in violations:
            print(f"  {v}")
        return 1

    print(
        "check_no_exec_status_stack_locals: OK -- no profile_exec_status_t "
        "automatic variable under firmware/KilnFW/App/drivers."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
