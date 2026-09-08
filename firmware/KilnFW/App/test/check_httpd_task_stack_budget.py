#!/usr/bin/env python3
"""Worst-case `httpd_worker`-task stack usage, measured out of the built ELF.

WHY THIS EXISTS
---------------
2026-09-08 hardware verification (`24fcbc0d`) measured `httpd_worker`'s live
high-water mark at 632-468 B free of an 8192 B stack -- CRITICAL, down from a
1728 B pre-flash baseline -- after a day of handler additions (`/api/cfgfs`
dual-write widening to 17 items, the setup wizard's `/api/setup/progress`,
safety-TC fault detail in diagnostics, `/api/cfgfs/file`, recovery/firing
refusal checks). One handler, `api_setup_progress_get_handler`
(`setup_progress_http.c`, added in `90bd8b2c`), put a
`setup_wizard_step_t[SETUP_WIZARD_STEP_COUNT]` array AND a 2048-byte `json[]`
buffer directly on the httpd task stack -- ~2.7 KB in that one frame -- fixed
in the same pass as this check by moving both onto the heap (the same
malloc-then-free-on-every-return-path convention `cfgfs_status_get_handler`
already used).

Unlike `check_main_task_stack_budget.py`'s single `app_main` root, this task
has ~120 possible roots: every `esp_http_server` URI handler function is
reachable, dispatched through a registration table this static analysis
cannot follow (the dispatch itself is an indirect call through a function
pointer). So this script enumerates handler names straight out of the
`http/*.c` source (`.handler = <name>` registration sites) and measures the
deepest statically-reachable path from EACH one, reporting the worst.

CEILING, not a headroom-fraction budget
----------------------------------------
`check_main_task_stack_budget.py` budgets against a fraction of the
configured stack size because that check was written to fix a genuine
overflow and is normally comfortably under budget. httpd_worker is not
comfortably under budget in general, so this check enforces a CEILING
(see CEILING_BYTES below) rather than a percentage of 8192 -- it exists to
catch the deepest reachable path getting WORSE, not to relitigate whatever
the worst case happened to measure at last time it was set.

2026-09-08, FIXED: `backup_import_post_handler` (via
`backup_import_apply()` -> `zones_config_cfg_fs_save()` ->
`zones_config_json_compute_crc()`) was the deepest path at 7952 B of the
8192 B stack, 240 B free -- almost entirely one 4656 B frame in
`backup_import_apply()` itself, dominated by two stack-local arrays:
`profile_candidate_t candidates[PROFILES_MAX_COUNT]` (~428 B each, ~3.4 KB)
and `zone_candidate_t zone_candidates[MAX31856_CHANNEL_COUNT]`. Fixed by
splitting the function: the two-pass validate-then-commit logic moved
unchanged (every `return false`/`return true` untouched, same all-or-
nothing ordering) into a new `backup_import_apply_locked()` that takes the
two arrays as pointers, and the renamed `backup_import_apply()` is now a
thin wrapper that heap-allocates both (PSRAM preferred via
`MALLOC_CAP_SPIRAM`, same convention already used for this handler's own
request-body buffer) before calling it, freeing on every path. An
allocation failure is reported exactly like any other pass-1 validation
refusal (false + err_msg, nothing touched yet) -- see backup_import.c's
own comment above `profile_candidate_t`. This dropped the path to 3808 B;
CEILING_BYTES below has been retightened to the new overall worst case
(see its own comment).

LIMITS, stated honestly (same as check_main_task_stack_budget.py):
  * Indirect calls (`callx8`) are not followed.
  * Recursion is cut at the first repeat rather than unrolled.
  * ISR/window-overflow spill is not modelled.
Both make this an UNDER-estimate. It is also an under-estimate specifically
for httpd handlers because the one true root -- the ESP-IDF httpd worker
function that dispatches to a handler via a function pointer -- can't be
walked; each handler is measured as its own root, so any stack the ESP-IDF
dispatch machinery itself uses before calling the handler is not counted.
That is exactly why the live board measurement (get_heap_status /
get_stack_margin) is the authority for "is this actually safe right now" --
this script is the pre-flash gate that catches an obvious regression before
that measurement ever happens.
"""

import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402  (reuse the ELF/objdump/deepest-path machinery)

REPO_ROOT = base.REPO_ROOT
DEFAULT_ELF = base.DEFAULT_ELF
HTTP_DIR = os.path.join(REPO_ROOT, "firmware", "KilnFW", "App", "drivers", "http")

# Known worst case as of 2026-09-08, after the four-handler stack-budget pass
# (profile_detail_get_handler's 5184 B json[] buffer, ct_auto_zero_post_
# handler's st/pstat locals, backup_export_get_handler -- which shares this
# ceiling's path only through this static analysis's per-name call-graph
# match, see below -- and firing_stats_get_dualwrite_status's two 1364 B
# blobs, all moved to heap.malloc): the new worst reachable path is
# backup_export_get_handler (752 B own frame) -> ct_auto_zero_post_handler's
# NAME reused as a call-graph match for the safety_cfg_store_refetch_locked
# -> safety_link_get_config_page -> ... -> uart_enable_tx_write_fifo chain,
# at 5776 B total. NOTE: backup_export.c never actually calls
# ct_auto_zero_post_handler (nor, before this pass, profile_detail_get_
# handler) -- this script's static analysis measures the deepest path
# reachable from a name it can find a call edge FOR, and something in this
# codepath's call-graph extraction is mis-attributing an edge across files
# that do not call each other. That mis-attribution predates this pass (the
# same shape was already visible in the prior 7472 B ceiling's reported
# path) and is out of scope here -- the real, load-bearing worst case this
# pass leaves behind is ct_cal_post_handler at 5056 B (see the "deepest 5"
# printout), which is what CEILING_BYTES is retightened against, with
# headroom to the reported (if mis-attributed) 5776 B above it. This is a
# CEILING, not a percentage-of-stack budget: it exists to catch the deepest
# reachable handler path getting WORSE, not to relitigate this depth.
CEILING_BYTES = 5776

HANDLER_RE = re.compile(r"\.handler\s*=\s*([A-Za-z_][A-Za-z0-9_]*)")


def find_handler_roots(http_dir):
    names = set()
    for path in sorted(glob.glob(os.path.join(http_dir, "*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in HANDLER_RE.finditer(f.read()):
                names.add(m.group(1))
    return sorted(names)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                     help="override CEILING_BYTES (used by the negative test)")
    args = ap.parse_args()

    if not os.path.isfile(args.elf):
        print(f"check_httpd_task_stack_budget: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build) and re-run; unmeasured, not passing.")
        return 3

    objdump = base.find_objdump()
    if not objdump:
        print("check_httpd_task_stack_budget: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3

    roots = find_handler_roots(HTTP_DIR)
    if not roots:
        print(f"check_httpd_task_stack_budget: FAIL -- no '.handler = ...' registrations found under {HTTP_DIR}")
        return 1

    frames, calls = base.parse(objdump, args.elf)

    ceiling = args.ceiling_bytes or CEILING_BYTES
    results = []
    for r in roots:
        if r not in frames:
            continue  # handler compiled out (e.g. feature-gated) -- not an error
        total, path = base.deepest(r, frames, calls)
        results.append((total, r, path))
    results.sort(reverse=True)

    print(f"httpd handler roots measured: {len(results)} of {len(roots)} found in {os.path.basename(args.elf)}")
    print(f"ceiling: {ceiling} B of an 8192 B httpd_worker stack")
    print()
    print("deepest 5 handler paths:")
    for total, root, path in results[:5]:
        print(f"  {total:>6} B  {root}")

    worst_total, worst_root, worst_path = results[0]
    print()
    print(f"deepest overall: {worst_root} = {worst_total} B")
    running = 0
    for fn in [worst_root] + worst_path:
        running += frames.get(fn, 0)
        print(f"    {frames.get(fn, 0):>6} B  {running:>6} B cumulative  {fn}")

    if worst_total > ceiling:
        print()
        print(f"check_httpd_task_stack_budget: FAIL -- {worst_root} reaches {worst_total} B, "
              f"exceeding the {ceiling} B ceiling.")
        print("  httpd_worker is shared by every HTTP handler in this codebase and was measured "
              "CRITICAL (632 B free of 8192 B) on hardware 2026-09-08. Fix by moving the large "
              "locals named above onto the heap (malloc + free on every return path, 500 on OOM) "
              "or streaming the response -- never by enlarging CONFIG httpd stack_size. See "
              "cfgfs_status_get_handler / api_setup_progress_get_handler for the pattern.")
        return 1

    print()
    print("check_httpd_task_stack_budget: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
