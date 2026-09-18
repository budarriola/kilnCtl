#!/usr/bin/env python3
"""setup_wizard_step_count_mirror_drift_check.py -- pins the setup wizard
page's step list (setup_wizard_page.html's WIZARD_STEPS array) against the
firmware's persisted-progress step count (setup_wizard_progress.h's
SETUP_WIZARD_STEP_COUNT).

2026-09-18 confirmed defect: WIZARD_STEPS grew to 14 entries (ids 0..13,
step 13 being the web-auth work's "Authentication (optional)" step) while
SETUP_WIZARD_STEP_COUNT stayed at 13 -- GET /api/setup/progress therefore
never surfaced step 13 and POST rejected it outright with "step out of
range", so step 13 could never be recorded done or skipped by anyone. Both
sides independently passed their own tests (the page's own JS host test
only asserts WIZARD_STEPS.length/shape; setup_wizard_progress.c's host test
only exercises SETUP_WIZARD_STEP_COUNT against itself) -- nothing compared
the two counts against each other. This check is that comparison, so the
same drift cannot ship silently again.

This is a static extraction+diff, not a test exercising real behavior --
same class as the other *_mirror_drift_check.py scripts in this directory
(see power_diag_flag_mirror_drift_check.py's header for the rationale). If
either regex stops matching, this FAILS CLOSED (treated as drift, not
skipped): a check that silently matches nothing is worse than none.

Usage: python setup_wizard_step_count_mirror_drift_check.py [repo_root]
Exit 0: the two counts agree. Exit 1: drift found (or extraction failure).
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _drivers_layout import DriverFileError, resolve_driver_file  # noqa: E402

STEP_COUNT_RE = re.compile(r"#define\s+SETUP_WIZARD_STEP_COUNT\s+(\d+)")

# Matches each `{ id: <N>, ... }` entry inside the WIZARD_STEPS array literal.
# Deliberately anchored on `id:` rather than counting commas/braces generally,
# so it keeps working if entries gain/lose other fields.
WIZARD_STEPS_ARRAY_RE = re.compile(r"var\s+WIZARD_STEPS\s*=\s*\[(.*?)\];", re.DOTALL)
STEP_ENTRY_ID_RE = re.compile(r"\{\s*id:\s*(\d+)")


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

    try:
        progress_h_path = resolve_driver_file(repo_root, "setup_wizard_progress.h")
        page_html_path = resolve_driver_file(repo_root, "setup_wizard_page.html")
    except DriverFileError as exc:
        print("SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: FAILED (setup)")
        print(f"  {exc}")
        return 1

    progress_h_text = progress_h_path.read_text(encoding="utf-8")
    page_html_text = page_html_path.read_text(encoding="utf-8")

    count_match = STEP_COUNT_RE.search(progress_h_text)
    if not count_match:
        print("SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not find '#define SETUP_WIZARD_STEP_COUNT <N>' in {progress_h_path}")
        return 1
    firmware_count = int(count_match.group(1))

    array_match = WIZARD_STEPS_ARRAY_RE.search(page_html_text)
    if not array_match:
        print("SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not find 'var WIZARD_STEPS = [...]' in {page_html_path}")
        return 1

    ids = [int(m.group(1)) for m in STEP_ENTRY_ID_RE.finditer(array_match.group(1))]
    if not ids:
        print("SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  WIZARD_STEPS array in {page_html_path} matched zero '{{ id: N, ... }}' entries")
        return 1

    page_count = len(ids)
    expected_ids = list(range(page_count))
    if ids != expected_ids:
        print("SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: FAILED (shape)")
        print(f"  WIZARD_STEPS ids are {ids!r}, expected a contiguous 0..{page_count - 1} run with no gaps")
        print("  -- SETUP_WIZARD_STEP_COUNT is only a valid upper bound if ids are dense from 0.")
        return 1

    if firmware_count != page_count:
        print("SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: FAILED")
        print(f"  {progress_h_path}: SETUP_WIZARD_STEP_COUNT = {firmware_count}")
        print(f"  {page_html_path}: WIZARD_STEPS has {page_count} entries (ids 0..{page_count - 1})")
        print("  These must agree -- a mismatch means the page offers a step the firmware's")
        print("  persisted-progress store cannot record (GET/POST /api/setup/progress reject it),")
        print("  or the firmware reserves a slot the page never populates.")
        return 1

    print(f"SETUP WIZARD STEP COUNT MIRROR DRIFT CHECK: OK (both sides agree: {firmware_count} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
