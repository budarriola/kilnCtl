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
nothing ordering) into a new `backup_import_apply_two_pass()` that takes the
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
# Every directory whose .c files register `.handler = ...` on the shared httpd worker. The update
# handlers (WP4 stage upload, WP8 fetch routes, WP9 settings) live in drivers/update, not http;
# scanning only http left them unmeasured. Each entry must contribute at least one root (a
# vacuity guard: an empty or renamed directory fails loudly instead of silently measuring less).
UPDATE_DIR = os.path.join(REPO_ROOT, "firmware", "KilnFW", "App", "drivers", "update")
HANDLER_DIRS = [HTTP_DIR, UPDATE_DIR]

# Known worst case as of 2026-09-08, after the shared-safety-link-chain pass:
# ct_cal_post_handler (5056 B) and revert_post_handler (4832 B) shared
# ~4 KB in apply_pairs() -> safety_cfg_store_refetch_locked() ->
# safety_link_get_config_page() -> ... -> uart_enable_tx_write_fifo. Of that
# 4 KB, 992 B was safety_cfg_store_refetch_locked()'s OWN frame: a
# safety_cfg_store_blob_t (67-entry cache scratch copy) and a
# kilnlink_config_page_t (32-entry wire-page decode buffer) both declared as
# plain stack locals. Both are used only by that one function, on whichever
# task happens to call in -- safety_poll_task (PSRAM stack, fine) or the
# shared httpd worker (internal DRAM, the tight one) via
# safety_cfg_http.c's confirm_commit_landed()/ct_cal_post_handler/
# revert_post_handler's apply_pairs() chain. Fixed once, at the shared
# root, rather than in each caller: both locals now live in one
# malloc()-ed bundle (safety_cfg_store_refetch_scratch_t), freed on every
# return path, same "malloc + free on every return, 500-equivalent
# `return false` on OOM" convention as setup_progress_http.c's scratch --
# plain internal-DRAM heap, not MALLOC_CAP_SPIRAM: this function never
# writes flash directly (it marks s_dirty and defers to the flash-safe
# worker), so there is no flash-write-from-PSRAM-stack hazard to avoid, and
# no reason to prefer PSRAM for a transient decode buffer either. This
# dropped safety_cfg_store_refetch_locked()'s own frame to 64 B and took
# ct_cal_post_handler/ct_auto_zero_post_handler out of the "deepest 5"
# entirely (4128 B / 4096 B now, well under the new ceiling).
#
# Measuring the new worst case exposed a SEPARATE, larger instance of the
# call-graph mis-attribution artifact this file's own history already
# flagged once (see git blame): the raw report showed
# backup_export_get_handler reaching a fabricated 6176 B via
# `_stext` -> `profiles_handle_message` -> `profile_executor_halt` -> ... --
# backup_export.c calls neither (grep confirms). Root cause, this time
# actually run down rather than merely worked around: objdump captions an
# indirect-looking call target against the NEAREST PRECEDING symbol in the
# whole symbol table when the real callee has no symbol of its own (a
# literal pool / veneer table / a local whose symbol didn't make this ELF's
# symtab) -- and for everything low in the image, that nearest symbol is
# `_stext`, however far away. check_main_task_stack_budget.py's CALL_RE
# used to match `<name+0xNNNN>` and silently discard the offset, treating
# that guess as a real edge; folding dozens of unrelated functions'
# unrelated call sets under one `_stext` node fabricates whatever
# transitive reachability happens to exist anywhere in that region. FIXED
# there (CALL_RE now requires the target to land exactly on a symbol, no
# offset) rather than patched around here, since Xtensa's windowed-register
# ABI means a real `call4/8/12` can never legally land mid-function anyway
# -- this can only make every check built on that module's parse()/deepest()
# more accurate, never hide a real edge. See that file's own comment on
# CALL_RE and SECTION_MARKER_NAMES for the full story; the latter is kept
# as cheap defence in depth even though CALL_RE now excludes the case that
# motivated it.
#
# The real, verified (grep-confirmed) worst case after both fixes is
# revert_post_handler at 4832 B, entirely unrelated to the safety-link
# chain (adaptive_tune_revert -> zones_config_set_model -> nvs_save ->
# zones_config_cfg_fs_save -> zones_config_json_compute_crc) and unchanged
# by this pass -- CEILING_BYTES is retightened to it. This is a CEILING,
# not a percentage-of-stack budget: it exists to catch the deepest
# reachable handler path getting WORSE, not to relitigate this depth.
# 2026-10-09: long calls (l32r+callx) now followed: profile_exec_start_post_handler 4848 B (was 4832 B), 8192 - 4848 - 1800 = 1544 B honest free.
# 2026-10-09 (LongCallTracker union-merge fix): a register loaded with several literals now yields every edge,
# not just the last, so the walk reaches the __assert_func/panic_abort tail it used to miss: measured 5248 B (was 4848 B).
# Honest free still above 10% of the declared stack; re-baselined for this stated cause only.
# 2026-10-09: profile_exec_start_post_handler ran 5248 B (14.0% honest headroom, under the 15% CRITICAL line) once the
# literal-merge fix exposed it. Fixed by moving the handlers' JSON refusal buffers into noinline helpers (start + autotune
# handlers) and kiln_cfg_store.c populate_pico_half_and_hash's two 896 B canonical[] buffers to persist_scratch_alloc: now 4496 B.
# 2026-10-09 (stack analyser review F2/F7/F8): worker-only autosave edge dropped, long-call edges only to function entries:
# profile_exec_start_post_handler measures 4448 B (was 4496 B). Lowered to it.
CEILING_BYTES = 4448

# 2026-09-08 honesty fix (docs/audits/2026-09-08-httpd-stack-gap.md, `022bde0a`):
# the static walk's "N B free" framing was misleading. It measures only each
# handler's own reachable frames -- it cannot see ESP-IDF's httpd
# dispatch/session-parsing machinery (runs BELOW every handler root) or
# Xtensa ISR window-spill onto whatever stack is current, both of which run
# on the real task before/around the handler and are structurally invisible
# to a per-handler frame walk. That audit measured live, on hardware
# (build `4bbfcfbe`): 1528-1656 B of free stack at the worst observed mark,
# against this script's naive "8192 - 4832 = 3360 B free" implication -- a
# ~1700-1900 B gap that did not move when four different deep handlers were
# deliberately exercised (probing did not deepen the mark further), so it is
# attributed to a roughly CONSTANT per-request/dispatch overhead rather than
# to any one handler being mismeasured.
#
# UNMODELED_OVERHEAD_BYTES below is that gap's documented, doc-traceable
# estimate -- 1800 B, the midpoint of the audit's 1700-1900 B range. Sanity
# check: applying it to the CURRENT worst case (revert_post_handler, 4832 B)
# gives 8192 - 4832 - 1800 = 1560 B honest headroom (19.0%), matching the
# audit's live-measured 1528 B / 18.7% to within 32 B / 0.3 pp -- close
# enough to trust the constant without pretending it is exact. This is a
# subtracted allowance, not a re-measurement: it does not explain the gap
# mechanically (that remains open per the audit's Verdict section), it just
# stops the printed number from being read as real margin when it isn't.
UNMODELED_OVERHEAD_BYTES = 1800

# Explicit call edges the objdump walk cannot see because they are indirect (a function pointer
# stored in a table). The zone-to-aux conversion (docs/SPARE_RELAY_ONOFF_PLAN.md section 10) is
# dispatched from zones_post_handler through a hook pointer and then reaches the profile store,
# the aux store and the journal through zone_aux_ops_t, so without these edges the static walk
# stops at zones_post_handler and misses the deepest chain on the httpd task. An edge whose
# caller or callee is absent from the ELF is a FAIL, not a skip: a renamed function would
# otherwise silently turn the edge (and the measurement) vacuous.
_AUX_CORE_FNS = ("zone_aux_convert_run", "run_locked", "resume_run", "rollback", "read_back_ok",
                 "fail_rolled_back", "journal_stage")
_AUX_OPS_TARGETS = (
    "aux_outputs_cfg_get", "aux_outputs_cfg_get_raw", "aux_outputs_cfg_set", "aux_outputs_cfg_quarantined",
    "aux_convert_journal_read", "aux_convert_journal_write", "aux_convert_journal_clear",
    "profiles_retarget_zone_to_aux_plan", "profiles_retarget_zone_to_aux_commit",
    "profiles_retarget_zone_to_aux_revert", "profiles_retarget_zone_to_aux_resume",
    "op_mode_blocked", "op_zone_get", "op_zone_free", "op_zone_restore", "op_zone_done", "op_zones_union",
    "op_live_uses_zone", "op_verify_persisted", "op_busy", "op_scratch_alloc",
)
EXTRA_EDGES = (
    [("zones_post_handler", "move_handler"), ("move_handler", "zone_aux_convert_run"),
     # update_stage_upload_write -> flush_head calls the policy gate through st->gate (a function pointer the
     # call-graph walk cannot see); flush_head is usually inlined into the writer.
     ("flush_head", "policy_gate"), ("update_stage_upload_write", "policy_gate")]
    + [(c, t) for c in _AUX_CORE_FNS for t in _AUX_OPS_TARGETS]
)
# Names the compiler may legitimately fold away (static helpers): only these may be absent from
# the ELF, as caller or callee, without failing.
_EDGE_MAY_BE_INLINED = frozenset(
    ("flush_head", "policy_gate") + _AUX_CORE_FNS[1:] + ("op_busy", "op_scratch_alloc", "op_verify_persisted", "op_zone_done",
                         "op_zones_union", "op_mode_blocked", "op_zone_get", "op_zone_free",
                         "op_zone_restore", "op_live_uses_zone", "aux_outputs_cfg_quarantined",
                         "aux_outputs_cfg_get", "aux_outputs_cfg_get_raw"))


def apply_extra_edges(frames, calls):
    """Adds EXTRA_EDGES to `calls`. Returns a list of problems (empty = fine)."""
    problems = []
    for caller, callee in EXTRA_EDGES:
        if caller not in frames:
            if caller in _EDGE_MAY_BE_INLINED:
                continue
            problems.append(f"edge caller {caller} is not in the ELF")
            continue
        if callee not in frames:
            if callee in _EDGE_MAY_BE_INLINED:
                continue
            problems.append(f"edge callee {callee} is not in the ELF")
            continue
        calls.setdefault(caller, set()).add(callee)
    return problems


HANDLER_RE = re.compile(r"\.handler\s*=\s*([A-Za-z_][A-Za-z0-9_]*)")


def find_handler_roots(http_dir):
    names = set()
    for path in sorted(glob.glob(os.path.join(http_dir, "*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in HANDLER_RE.finditer(f.read()):
                names.add(m.group(1))
    return sorted(names)


def find_all_handler_roots(dirs):
    """(sorted roots across every dir, {dir: roots found there})."""
    per_dir = {d: find_handler_roots(d) for d in dirs}
    allr = sorted({r for roots in per_dir.values() for r in roots})
    return allr, per_dir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                     help="override CEILING_BYTES (used by the negative test)")
    ap.add_argument("--overhead-bytes", type=int, default=None,
                     help="override UNMODELED_OVERHEAD_BYTES (used by the negative test)")
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

    roots, per_dir = find_all_handler_roots(HANDLER_DIRS)
    for d in HANDLER_DIRS:
        if not per_dir[d]:
            print(f"check_httpd_task_stack_budget: FAIL -- no '.handler = ...' registrations found under {d}")
            return 1

    try:
        frames, calls = base.parse(objdump, args.elf)
    except base.ElfParseError as exc:
        if exc.skip:
            print(f"check_httpd_task_stack_budget: SKIP: {exc}")
            return 3
        print(f"check_httpd_task_stack_budget: FAIL -- {exc}")
        return 1

    edge_problems = apply_extra_edges(frames, calls)
    if edge_problems:
        for pr in sorted(set(edge_problems)):
            print(f"check_httpd_task_stack_budget: FAIL -- {pr} (EXTRA_EDGES went stale)")
        return 1

    ceiling = args.ceiling_bytes or CEILING_BYTES
    overhead = args.overhead_bytes if args.overhead_bytes is not None else UNMODELED_OVERHEAD_BYTES
    stack_bytes = 8192
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

    upd_roots = set(per_dir[UPDATE_DIR])
    upd = [(t, r) for t, r, _p in results if r in upd_roots]
    print()
    print(f"update handlers (drivers/update) measured: {len(upd)} of {len(upd_roots)}")
    for total, root in upd:
        print(f"  {total:>6} B  {root}")

    worst_total, worst_root, worst_path = results[0]
    print()
    print(f"deepest overall: {worst_root} = {worst_total} B")
    running = 0
    for fn in [worst_root] + worst_path:
        running += frames.get(fn, 0)
        print(f"    {frames.get(fn, 0):>6} B  {running:>6} B cumulative  {fn}")

    # Honest headroom: subtract the unmodelled dispatch/ISR overhead (see
    # UNMODELED_OVERHEAD_BYTES above) from the naive "stack - worst path"
    # figure before calling it free. This is the number a human should
    # actually act on -- docs/audits/2026-09-08-httpd-stack-gap.md's whole
    # point was that the naive figure reads as comfortable when live
    # measurement was not.
    naive_free = stack_bytes - worst_total
    honest_free = naive_free - overhead
    honest_pct = 100.0 * honest_free / stack_bytes
    if honest_pct < 15.0:
        level = "CRITICAL"
    elif honest_pct < 30.0:
        level = "LOW"
    else:
        level = "OK"
    print()
    print(f"naive implied free: {naive_free} B (ignores dispatch/ISR overhead -- do not act on this)")
    print(f"honest free (naive - {overhead} B unmodelled overhead): {honest_free} B "
          f"({honest_pct:.1f}% of {stack_bytes} B) -- classified {level}")

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

    if honest_pct < 15.0:
        print()
        print(f"check_httpd_task_stack_budget: FAIL -- honest headroom is {honest_pct:.1f}% "
              "(CRITICAL, <15%) once the unmodelled dispatch/ISR overhead is counted, even though "
              f"the static path itself ({worst_total} B) is still under the {ceiling} B ceiling.")
        print("  This is the gap documented in docs/audits/2026-09-08-httpd-stack-gap.md: the "
              "ceiling alone does not see it. Fix the same way -- move large locals off this "
              "stack -- or lower CEILING_BYTES until honest headroom clears CRITICAL.")
        return 1

    print()
    print("check_httpd_task_stack_budget: OK"
          + (" (honest headroom is LOW -- worth a look, not yet failing)" if level == "LOW" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
