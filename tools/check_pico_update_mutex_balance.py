#!/usr/bin/env python3
"""check_pico_update_mutex_balance.py -- mechanical guard over
firmware/KilnFW/App/drivers/net/pico_auto_update_boot.c's cross-processor
update mutex handling (Opus review, 2026-09-20, alongside the
record_failure() misuse fix in the same file).

WHY THIS EXISTS. attempt_update_acquire() claims the single cross-processor
update mutex (ota_http_update_try_begin()); from that point on, EVERY early
`return false;` on the way to handing the mutex to the relay task
(attempt_update_staged_locked()'s ota_pico_relay_start() call) must first
call ota_http_update_end() to release it -- pico_img_stage.h's own contract
says staging into pico_img is "not reentrant and not locked internally", so
a leaked mutex here would let a concurrent HTTP Pico-OTA upload interleave
its own erase/write into the same flash region (review finding F1). This is
exactly the kind of easy-to-get-right-once, easy-to-break-later invariant a
one-off code review cannot keep re-verifying by hand.

WHAT THIS CHECKS, over pico_auto_update_boot.c:
  1. Ordering: attempt_update_acquire( appears in attempt_update_embedded's
     source before any pico_img_stage_ call in the same function -- staging
     must never begin before the mutex is claimed (F1's actual defect).
  2. Every `return false;` between the acquire() call and the tail call into
     attempt_update_staged_locked( within attempt_update_embedded is
     preceded, within 3 lines, by `ota_http_update_end();` -- i.e. no early
     exit leaks the mutex.
  3. Same as (2), scoped to attempt_update_staged_locked's own body between
     its `pico_update_attempts_record_attempt(` entry and its
     `ota_pico_relay_start(` call (the mutex hand-off point) -- every early
     `return false;` in that span must also release first.

This is a narrow, function-scoped text scan, not a C parser -- it is
deliberately pinned to these two functions by name (per this repo's
"narrow checks over a concrete, stable pair" convention -- see CLAUDE.md's
reset-one-side-of-a-pair section) rather than attempting a general
mutex-balance analysis that would either miss this shape or false-positive
on the large majority of ordinary, unrelated `return false;` statements
elsewhere in the file.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TARGET = REPO_ROOT / "firmware" / "KilnFW" / "App" / "drivers" / "net" / "pico_auto_update_boot.c"

FUNC_START_RE = {
    "attempt_update_embedded": re.compile(r"^static bool attempt_update_embedded\("),
    "attempt_update_staged_locked": re.compile(r"^static bool attempt_update_staged_locked\("),
}


def extract_function(lines, start_idx):
    """Returns (start_idx, end_idx) inclusive, spanning from the function's
    signature line to the closing brace at column 0, found by counting
    braces from the opening `{` (which may be on a later line than the
    signature for a multi-line parameter list)."""
    i = start_idx
    # Find the opening brace.
    depth = 0
    opened = False
    while i < len(lines):
        depth += lines[i].count("{")
        depth -= lines[i].count("}")
        if "{" in lines[i]:
            opened = True
        if opened and depth == 0:
            return start_idx, i
        i += 1
    return start_idx, len(lines) - 1


def check_ordering(func_lines, func_name, errors):
    acquire_idx = None
    stage_idx = None
    for i, line in enumerate(func_lines):
        if acquire_idx is None and "attempt_update_acquire(" in line:
            acquire_idx = i
        if stage_idx is None and re.search(r"\bpico_img_stage_\w*\s*\(", line):
            stage_idx = i
    if acquire_idx is None:
        errors.append(f"{func_name}: no attempt_update_acquire( call found")
        return
    if stage_idx is not None and stage_idx < acquire_idx:
        errors.append(
            f"{func_name}: pico_img_stage_*( at line offset {stage_idx} appears BEFORE "
            f"attempt_update_acquire( at line offset {acquire_idx} -- staging must never begin "
            f"before the update mutex is claimed (review finding F1)"
        )


def check_releases(func_lines, func_name, span_start, span_end, errors):
    """Within func_lines[span_start:span_end] (exclusive of span_end), every
    `return false;` must be preceded within 3 lines by `ota_http_update_end();`."""
    RETURN_FALSE_RE = re.compile(r"^\s*return false;\s*$")
    RELEASE_RE = re.compile(r"ota_http_update_end\s*\(\s*\)\s*;")
    for i in range(span_start, span_end):
        if not RETURN_FALSE_RE.match(func_lines[i]):
            continue
        window_start = max(span_start, i - 3)
        window = func_lines[window_start:i]
        if not any(RELEASE_RE.search(w) for w in window):
            errors.append(
                f"{func_name}: `return false;` at line offset {i} is not preceded within 3 "
                f"lines by ota_http_update_end() -- this leaks the cross-processor update mutex"
            )


def main():
    if not TARGET.exists():
        print(f"check_pico_update_mutex_balance: FAIL -- {TARGET} not found")
        return 1

    text = TARGET.read_text(encoding="utf-8")
    lines = text.splitlines()

    errors = []

    # attempt_update_embedded: ordering + release balance across the WHOLE
    # function (acquire is the first real statement; the tail call into
    # attempt_update_staged_locked is the last).
    embedded_start = None
    for i, line in enumerate(lines):
        if FUNC_START_RE["attempt_update_embedded"].match(line):
            embedded_start = i
            break
    if embedded_start is None:
        errors.append("attempt_update_embedded( definition not found")
    else:
        f_start, f_end = extract_function(lines, embedded_start)
        func_lines = lines[f_start:f_end + 1]
        check_ordering(func_lines, "attempt_update_embedded", errors)
        # Scope the release check to the acquire..tail-call span so the
        # early "if (!attempt_update_acquire())" branch's own bare `return
        # false;` (nothing to release yet -- acquire already failed) is not
        # flagged.
        acquire_offset = None
        tail_call_offset = None
        for i, line in enumerate(func_lines):
            if acquire_offset is None and "attempt_update_acquire()" in line and "if" in line:
                acquire_offset = i
            if "attempt_update_staged_locked(" in line:
                tail_call_offset = i
        if acquire_offset is None or tail_call_offset is None:
            errors.append(
                "attempt_update_embedded: could not locate the acquire-check / tail-call span "
                "markers -- has the function been restructured?"
            )
        else:
            # Skip past the acquire-check's own `if (!attempt_update_acquire())
            # { return false; }` block -- that bare return has nothing to
            # release yet, since acquire() itself is what failed. Its
            # closing brace is the next line after the if's own body.
            release_span_start = acquire_offset + 1
            while release_span_start < tail_call_offset and \
                    func_lines[release_span_start].strip() != "}":
                release_span_start += 1
            release_span_start += 1  # past the closing brace
            check_releases(func_lines, "attempt_update_embedded", release_span_start,
                            tail_call_offset, errors)

    # attempt_update_staged_locked: release balance between its entry
    # (pico_update_attempts_record_attempt() call) and the mutex hand-off
    # (ota_pico_relay_start() call) -- everything before the hand-off must
    # release on any early exit; nothing after it may (the relay owns the
    # mutex from there).
    staged_start = None
    for i, line in enumerate(lines):
        if FUNC_START_RE["attempt_update_staged_locked"].match(line):
            staged_start = i
            break
    if staged_start is None:
        errors.append("attempt_update_staged_locked( definition not found")
    else:
        f_start, f_end = extract_function(lines, staged_start)
        func_lines = lines[f_start:f_end + 1]
        entry_offset = None
        handoff_offset = None
        for i, line in enumerate(func_lines):
            if entry_offset is None and "pico_update_attempts_record_attempt(" in line:
                entry_offset = i
            if "ota_pico_relay_start(" in line:
                handoff_offset = i
        if entry_offset is None or handoff_offset is None:
            errors.append(
                "attempt_update_staged_locked: could not locate the record_attempt / "
                "ota_pico_relay_start span markers -- has the function been restructured?"
            )
        else:
            check_releases(func_lines, "attempt_update_staged_locked", entry_offset, handoff_offset,
                            errors)

    if errors:
        print("check_pico_update_mutex_balance: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1

    print("check_pico_update_mutex_balance: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
