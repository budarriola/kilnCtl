#!/usr/bin/env python3
"""Worst-case `profile_executor`-task stack usage, measured out of the built ELF.

WHY THIS EXISTS
---------------
2026-09-09 10:38: a `profile_executor` panic (`exc_pc=0xfffffffd`, i.e. a saved
PC of exactly 0 -- a return through a smashed `a0`, per `crash_report.c`'s
`CRASH_REPORT_PC_OF_ZERO`) during a heating run.
`docs/audits/executor_panic_stack_overflow_2026-09-09.md` diagnosed it as a
stack overflow of this task's 4096 B stack (`profile_executor_start.c:85`) on
its run-end / guard-escalation path, NOT a numerical fault in the control law
-- and found this task WAS already registered for stack-margin reporting
(`stack_margin_register("profile_executor", ...)`, same file, line 100) but had
**no** `check_*_task_stack_budget.py` of its own, unlike `main`, `httpd_worker`,
`system_uart_bridge` and `uart_log_bridge`. That gap is why the regression
reached HEAD undetected: `firing_stats_persist()` still carried a 1364 B
`profile_firing_history_blob_t` on this task's stack a full day after the
2026-09-08 `firing_history` audit heap-allocated the READ path's four copies of
the same struct (`docs/audits/firing_history_stack_overflow_2026-09-08.md`) --
the WRITE path, reached only from this task's smaller stack, was left.

Measured before the 2026-09-09 fix, deepest static path from
`executor_task_entry`: 3504 B of 4096 B, via `adaptive_tune_run_end` ->
`adaptive_tune_refine_coupled_locked` -> `zones_config_set_coupling_cell` ->
`nvs_save` -> `zones_config_cfg_fs_save` -> `zones_config_json_compute_crc`
(a second ~900 B `zones_cfg_t` stack copy, made just to zero one field before
CRCing). Both of those were fixed the same pass (heap-allocating
`firing_stats_persist()`'s blob exactly as `firing_stats_load()` already does;
computing the CRC over the real struct in two passes -- real bytes, then four
zero bytes for the `crc32` field itself, which is provably the struct's last
field -- instead of copying it; and moving `zones_config_cfg_fs_save()`'s own
`zones_cfg_t stamped` local into its already-heap-allocated write buffer).
Post-fix, those two paths dropped to 1168 B and 1088 B respectively.

CEILING, not a headroom-fraction budget -- same reasoning as
check_httpd_task_stack_budget.py: this task is not comfortably under budget
in general (a 4096 B stack, not 8192 B), so this check enforces a ceiling on
the single root rather than a percentage, and exists to catch the deepest
reachable path getting WORSE, not to relitigate whatever it happens to
measure at right now.

After the 2026-09-09 fix, the overall deepest path from `executor_task_entry`
is 2784 B, NOT through either fixed chain: `escalate_guard_trip` ->
`release_profile_relay_claim` -> `heat_enable_release` ->
`safety_link_request_enable` -> ... -> `uart_protocol_send_broadcast` ->
`frame_and_send$constprop$0` -> ... -- the shared safety-link frame-send path
every UART bridge task in this codebase pays
(check_uart_log_bridge_stack_budget.py's own docstring independently measured
~1152 B for this same `frame_and_send$constprop$0` chain as "unrelated to this
task specifically"). CEILING_BYTES is set to that, not to some larger
theoretical number, on the same "catch regressions, don't relitigate" basis --
if a future change makes this task's own deepest path exceed it, that is
exactly the signal this check exists to raise.

UNMODELED_OVERHEAD_BYTES: this task's own live measurement is directly
available (DRAM_PSRAM_PLAN.md, cited by the 2026-09-09 audit) --
1388 B free of 4096 B during a real firing (2708 B used) against a
deepest-TICK static path of 1488 B, i.e. ~1220 B of real, measured,
unmodelled overhead (ISR window-spill, FreeRTOS scheduler cost on top of the
statically-visible call chain). That is used directly here, the same way
check_uart_log_bridge_stack_budget.py prefers a real live/static pair over
the httpd checker's 1800 B placeholder when one is available. Applying it to
the post-fix worst case (2784 B): 4096 - 2784 - 1220 = 92 B honest headroom
-- thin, and worth watching, but the deepest path is now the pre-existing,
already-reviewed shared UART send chain, not either of today's two fixed
regressions.

LIMITS, stated honestly (same as check_main_task_stack_budget.py /
check_httpd_task_stack_budget.py):
  * Indirect calls (`callx8`) are not followed.
  * Recursion is cut at the first repeat rather than unrolled.
  * ISR/window-overflow spill is not modelled (that is exactly what
    UNMODELED_OVERHEAD_BYTES accounts for, imperfectly, above).
Both make this an UNDER-estimate: anything this reports as too deep genuinely
is too deep.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402  (reuse ELF/objdump/deepest-path machinery)

REPO_ROOT = base.REPO_ROOT
DEFAULT_ELF = base.DEFAULT_ELF

ROOT = "executor_task_entry"
STACK_BYTES = 4096

# See "CEILING, not a headroom-fraction budget" above -- the post-2026-09-09-fix
# worst case, unrelated to either regression this check exists because of.
CEILING_BYTES = 2784

# See "UNMODELED_OVERHEAD_BYTES" above -- this task's own live-measured figure,
# not the httpd checker's 1800 B placeholder.
UNMODELED_OVERHEAD_BYTES = 1220


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--root", default=ROOT, help="override the walked root (used by the negative test)")
    ap.add_argument("--stack-bytes", type=int, default=STACK_BYTES,
                     help="override the configured task stack size (used by the negative test)")
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                     help="override CEILING_BYTES (used by the negative test)")
    ap.add_argument("--overhead-bytes", type=int, default=None,
                     help="override UNMODELED_OVERHEAD_BYTES (used by the negative test)")
    args = ap.parse_args()

    if not os.path.isfile(args.elf):
        print(f"check_executor_task_stack_budget: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build, or check_00_kilnfw_target_build.ps1) and "
              "re-run; this check cannot measure anything without one. This is a SKIP, not a pass -- "
              "the class of overflow this check exists to catch "
              "(docs/audits/executor_panic_stack_overflow_2026-09-09.md) goes UNMEASURED on this run, "
              "not 'checked and found fine.'")
        return 3

    objdump = base.find_objdump()
    if not objdump:
        print("check_executor_task_stack_budget: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3

    frames, calls = base.parse(objdump, args.elf)
    if args.root not in frames:
        print(f"check_executor_task_stack_budget: FAIL -- {args.root} not found in {args.elf}")
        return 1

    ceiling = args.ceiling_bytes if args.ceiling_bytes is not None else CEILING_BYTES
    overhead = args.overhead_bytes if args.overhead_bytes is not None else UNMODELED_OVERHEAD_BYTES
    stack_bytes = args.stack_bytes

    total, path = base.deepest(args.root, frames, calls)
    print(f"deepest static stack path from {args.root}: {total} B "
          f"(ceiling {ceiling} B of a {stack_bytes} B profile_executor stack)")
    running = 0
    for fn in [args.root] + path:
        running += frames.get(fn, 0)
        print(f"    {frames.get(fn, 0):>6} B  {running:>6} B cumulative  {fn}")

    naive_free = stack_bytes - total
    honest_free = naive_free - overhead
    honest_pct = 100.0 * honest_free / stack_bytes
    if honest_pct < 15.0:
        level = "CRITICAL"
    elif honest_pct < 30.0:
        level = "LOW"
    else:
        level = "OK"
    print()
    print(f"naive implied free: {naive_free} B (ignores ISR/scheduler overhead -- do not act on this)")
    print(f"honest free (naive - {overhead} B unmodelled overhead): {honest_free} B "
          f"({honest_pct:.1f}% of {stack_bytes} B) -- classified {level}")

    # Two separate failure conditions, deliberately not the same gate:
    #   1. The static path itself got WORSE than the known post-2026-09-09-fix
    #      ceiling -- catches a regression on TOP of what is already here,
    #      the same "don't relitigate the current baseline" contract
    #      check_httpd_task_stack_budget.py uses its own CEILING_BYTES for.
    #   2. Honest free actually goes NEGATIVE -- a real, not merely
    #      uncomfortable, predicted overflow. Unlike the httpd checker's
    #      arbitrary 15%-of-stack CRITICAL cutoff, this task's post-fix
    #      baseline (92 B honest free, 2.2%) is ALREADY inside any percentage
    #      cutoff worth choosing, via a pre-existing, already-reviewed shared
    #      UART frame-send chain (frame_and_send$constprop$0, ~1152 B,
    #      documented as "unrelated to this task specifically" in
    #      check_uart_log_bridge_stack_budget.py) that is out of this check's
    #      scope to fix. A percentage cutoff here would make this check FAIL
    #      on every run regardless of whether anything got worse, which is
    #      exactly the "shipped vacuous" failure mode
    #      feedback_negative_test_every_check.md warns about -- so the gate
    #      that actually predicts an overflow (honest_free < 0) is the one
    #      enforced, not an arbitrary band.
    if total > ceiling:
        print()
        print(f"check_executor_task_stack_budget: FAIL -- {args.root} reaches {total} B, "
              f"exceeding the {ceiling} B ceiling.")
        print("  profile_executor runs on a 4096 B stack and was overflowed on its run-end/guard-"
              "escalation path 2026-09-09 (docs/audits/executor_panic_stack_overflow_2026-09-09.md). "
              "Fix by moving the large locals named above onto the heap (malloc + free on every "
              "return path), never by enlarging the stack -- the frames are cumulative and will "
              "keep growing. See firing_stats_persist()/zones_config_json_compute_crc()/"
              "zones_config_cfg_fs_save() for the established pattern.")
        return 1

    if honest_free < 0:
        print()
        print(f"check_executor_task_stack_budget: FAIL -- honest free is {honest_free} B (negative) "
              "once the unmodelled ISR/scheduler overhead is counted, even though the static path "
              f"itself ({total} B) is still under the {ceiling} B ceiling.")
        print("  Fix by moving large locals off this stack, or lower CEILING_BYTES/raise "
              "UNMODELED_OVERHEAD_BYTES only with a documented reason for accepting the new margin.")
        return 1

    print()
    print("check_executor_task_stack_budget: OK"
          + (f" ({level} honest headroom -- worth a look, not yet failing)" if level != "OK" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
