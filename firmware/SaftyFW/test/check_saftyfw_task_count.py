#!/usr/bin/env python3
"""Cross-checks that TASKS in check_saftyfw_task_stack_budgets.py enumerates
EVERY xTaskCreate() call site under firmware/SaftyFW/src/ -- no more, no
fewer.

WHY THIS EXISTS
----------------
check_saftyfw_task_stack_budgets.py's TASKS table comment claims: "If a new
task is added, this table must grow with it or check_saftyfw_task_count.py
(added alongside this file) fails loud." That file did not exist -- this is
it. Without it, a tenth SaftyFW task could be added, would never appear in
TASKS, and would silently receive NO stack-budget coverage at all while the
existing comment asserted the opposite. The per-task checker itself cannot
catch this: it only ever iterates the rows it already has.

This is a pure source-text grep, deliberately NOT ELF-based (unlike its
sibling) -- a newly-added task that has not been wired into a stack-depth
macro yet, or that fails to compile, would not exist in any ELF for the
ELF-side walk to find in the first place, and "a task was added but is
currently broken" must fail this check just as loudly as "a task was added
and builds fine but nobody remembered the table."

WHAT COUNTS AS A TASK CALL SITE: a line under firmware/SaftyFW/src/ (any
depth) matching `xTaskCreate(<root_fn>, "<name>", ...)` in a *.c file. This
excludes commented-out call sites (CALL_RE requires the actual paren-comma
shape immediately after `xTaskCreate(`, which a `// ... xTaskCreate() ...`
prose comment such as main.c's does not produce) and vendored FreeRTOS/
pico-sdk kernel sources (not present at all under src/).

EXIT CODES: 0 OK (call sites found == TASKS entries, one-to-one by (root,
name)), 1 FAIL (any set difference, in either direction), naming the
task(s) responsible.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_saftyfw_task_stack_budgets as budgets  # noqa: E402

REPO_ROOT = budgets.REPO_ROOT
SRC_DIR = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "src")

# Matches the real call shape, e.g.:
#   BaseType_t ok = xTaskCreate(current_task_fn, "current_task", CURRENT_TASK_STACK_WORDS, NULL,
# Deliberately anchored on `xTaskCreate(<ident>, "<ident>",` -- a prose
# comment mentioning "xTaskCreate()" (no open-paren-comma-quote shape
# immediately following) will not match.
CALL_RE = re.compile(r'\bxTaskCreate\(\s*([A-Za-z_]\w*)\s*,\s*"([^"]+)"\s*,')


def find_call_sites():
    """{(root_fn, name): [(path, lineno), ...]} for every real xTaskCreate()
    call site under src/*.c."""
    sites = {}
    for dirpath, _dirnames, filenames in os.walk(SRC_DIR):
        for fname in filenames:
            if not fname.endswith(".c"):
                continue
            path = os.path.join(dirpath, fname)
            with open(path, encoding="utf-8", errors="replace") as f:
                for lineno, line in enumerate(f, start=1):
                    m = CALL_RE.search(line)
                    if m:
                        key = (m.group(1), m.group(2))
                        sites.setdefault(key, []).append((path, lineno))
    return sites


def main():
    call_sites = find_call_sites()
    table_keys = {(task["root"], task["name"]) for task in budgets.TASKS}
    call_keys = set(call_sites.keys())

    missing_from_table = call_keys - table_keys   # real call site, no TASKS row
    missing_from_source = table_keys - call_keys  # TASKS row, no matching call site found

    fail = bool(missing_from_table or missing_from_source)

    print("check_saftyfw_task_count: xTaskCreate() call sites under src/ vs. "
          "check_saftyfw_task_stack_budgets.py's TASKS table")
    print(f"  {len(call_keys)} call site(s) found, {len(table_keys)} TASKS row(s)")

    for root, name in sorted(missing_from_table):
        locs = ", ".join(f"{p}:{ln}" for p, ln in call_sites[(root, name)])
        print(f"  FAIL: task {name!r} (root={root!r}) has a real xTaskCreate() call site "
              f"({locs}) but NO row in TASKS -- add one to "
              "check_saftyfw_task_stack_budgets.py's TASKS table (and give it a CEILING_BYTES "
              "entry) before this task ships with zero stack-budget coverage.")

    for root, name in sorted(missing_from_source):
        print(f"  FAIL: TASKS row {name!r} (root={root!r}) has no matching xTaskCreate() call "
              "site found under firmware/SaftyFW/src/ -- the task was renamed/removed and the "
              "table is now stale, or this checker's CALL_RE no longer matches its call site "
              "shape (check for a reflow of the real call before assuming the task is gone).")

    if not fail:
        print("  OK: one-to-one match, every task graded.")

    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
