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
available (DRAM_PSRAM_STATUS.md, cited by the 2026-09-09 audit) --
1388 B free of 4096 B during a real firing (2708 B used) against a
deepest-TICK static path of 1488 B, i.e. ~1220 B of real, measured,
unmodelled overhead (ISR window-spill, FreeRTOS scheduler cost on top of the
statically-visible call chain). That is used directly here, the same way
check_uart_log_bridge_stack_budget.py prefers a real live/static pair over
the httpd checker's 1800 B placeholder when one is available.

2026-09-10 UPDATE -- the 92 B margin below was a CONFIRMED recurring crash,
not a theoretical CRITICAL rating. `docs/audits/
profile_executor_panic_2026-09-10_root_cause.md` traced a second
`profile_executor` panic (byte-for-byte identical `exc_a0`/`exc_a1_sp` to
2026-09-09's) to exactly this 2784 B path, reached from `profiles_stop()` ->
`profile_executor_halt()` -> `heat_enable_release()` ->
`safety_link_request_enable()`. The deepest branch of that path went through
`safety_exchange()`'s own unconditional opportunistic pre-drain
(`safety_drain_inbox(link, 0)`), which happened to dispatch a queued
FW_VERSION or DIAG frame into `safety_apply_fw_version()` /
`safety_apply_diag()` -- each of which, on specific low-frequency conditions
(a Pico boot_id change; a stale S6a latched from before this boot), sent a
FURTHER frame synchronously (`safety_link_send_announce_version_burst()` /
`safety_link_send_clear_trip()`) reaching the same deep
`uart_protocol_send_broadcast()` -> `frame_and_send$constprop$0()` chain a
SECOND time, on top of the base exchange. Both of those side-effect sends ran
on WHATEVER task happened to be draining the inbox -- not necessarily one
with headroom for them -- so they were moved off this call path entirely:
`safety_apply_fw_version()`/`safety_apply_diag()` now only set a pending flag
(`reannounce_pending`/`boot_clear_pending`, `safety_link.h`) under lock, and
`safety_poll_task` (`safety_link_poll.c`, its own dedicated 8192 B stack)
performs the actual sends once per iteration. This is a structural fix, not a
byte-shave: it removes two entire reachable branches from every caller of
`safety_drain_inbox()`/`safety_exchange()`, `profile_executor` included, not
just this one call site.

Post-fix, the deepest static path from `executor_task_entry` is 1936 B (down
from 2784 B), and it is now the irreducible one every caller of
`safety_exchange()` already pays for the REQUEST itself: `safety_exchange`
-> `uart_protocol_send_broadcast` -> `frame_and_send$constprop$0` -> ... --
no side-effect branch, just sending the one frame this call always intended
to send. CEILING_BYTES is set to that. Applying UNMODELED_OVERHEAD_BYTES:
4096 - 1936 - 1220 = 940 B honest headroom (22.9%), classified LOW, not
CRITICAL -- a real, structural improvement, not a relitigated number.

LIMITS, stated honestly (same as check_main_task_stack_budget.py /
check_httpd_task_stack_budget.py):
  * Indirect calls (`callx8`) are not followed.
  * Recursion is cut at the first repeat rather than unrolled.
  * ISR/window-overflow spill is not modelled (that is exactly what
    UNMODELED_OVERHEAD_BYTES accounts for, imperfectly, above).
Both make this an UNDER-estimate: anything this reports as too deep genuinely
is too deep.

CRITICAL NOW FAILS THE CHECK (2026-09-10). Before this fix, a CRITICAL
classification (honest_pct < 15%) printed a warning and still returned 0 --
this is exactly the class flagged by feedback_negative_test_every_check.md:
a check whose worst rating cannot fail is not a check, it is a comment. The
92 B/2.2% CRITICAL result sat "passing" for a full day between the
2026-09-09 fix and the 2026-09-10 recurrence, and the checker never once
went red. This was defensible ONLY as long as 92 B was believed to be an
already-reviewed, irreducible floor (the commit that set it said so
explicitly); it is not defensible now that it is a documented, confirmed
recurring crash signature. CRITICAL now fails, same as an over-ceiling total
or a negative honest_free.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402  (reuse ELF/objdump/deepest-path machinery)

REPO_ROOT = base.REPO_ROOT
DEFAULT_ELF = base.DEFAULT_ELF

ROOT = "executor_task_entry"
# 4096 -> 6144, 2026-09-24: profile_executor's declared stack was raised
# (profile_executor_start.c) after two live baselines
# (docs/stack_margin_baseline/stack_margin_mid_firing_111b1b6f_*.json and
# stack_margin_web_ui_open_111b1b6f_*.json) both recorded a worst-since-boot
# CRITICAL reading (468 B free of 4096 B, 11.4%) during a real firing --
# owner-authorized per CLAUDE.md's 2026-09-21 decision. CEILING_BYTES/
# UNMODELED_OVERHEAD_BYTES below are unchanged: the static deepest path and
# its unmodelled-overhead figure did not change, only the declared stack the
# same path now has more room within.
STACK_BYTES = 6144

# See "CEILING, not a headroom-fraction budget" above and the 2026-09-10
# UPDATE in the module docstring -- the post-2026-09-10-fix worst case
# (1936 B), now the irreducible safety_exchange() request-send path, not the
# side-effect announce-burst/boot-clear branches that used to make this
# 2784 B. Lowered deliberately, with the cause stated, per "do not raise a
# ceiling quietly" -- the same rule applies to lowering one.
# 2026-10-09: long calls (l32r+callx) now followed: measured 3360 B (was 1936 B). With the nvs_save -> zones_autosave_job
# edge (a volatile function pointer the static walk cannot follow; see check_all_task_stack_budgets.py DECLARED_EDGES -- that
# declared edge applies only to the bx_flash_worker task, NOT to this executor measurement) the
# bx_flash_worker prototype resolver gives 3936 B; 3936 + 1220 overhead = 5156 B of 6144 B, so no stack bump is needed.
# 2026-10-09 (LongCallTracker union-merge fix): a register loaded with several literals now yields every edge,
# not just the last, so the walk reaches the __assert_func/panic_abort tail it used to miss: measured 3920 B (was 3360 B).
# Honest free still above 10% of the declared stack; re-baselined for this stated cause only.
CEILING_BYTES = 3920

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

    try:
        frames, calls = base.parse(objdump, args.elf)
    except base.ElfParseError as exc:
        if exc.skip:
            print(f"check_executor_task_stack_budget: SKIP: {exc}")
            return 3
        print(f"check_executor_task_stack_budget: FAIL -- {exc}")
        return 1
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

    # Three separate failure conditions, deliberately not the same gate:
    #   1. The static path itself got WORSE than the known post-2026-09-10-fix
    #      ceiling -- catches a regression on TOP of what is already here,
    #      the same "don't relitigate the current baseline" contract
    #      check_httpd_task_stack_budget.py uses its own CEILING_BYTES for.
    #   2. Honest free actually goes NEGATIVE -- a real, not merely
    #      uncomfortable, predicted overflow.
    #   3. CRITICAL (honest_pct < 15%). Before 2026-09-10 this was
    #      deliberately NOT a failure, on the reasoning that this task's
    #      post-fix baseline (92 B, 2.2%) was ALREADY inside any percentage
    #      cutoff worth choosing, via a pre-existing shared UART frame-send
    #      chain judged "unrelated to this task specifically" and out of
    #      scope to fix -- so a percentage gate would fail on every run
    #      regardless of whether anything got worse, exactly the "shipped
    #      vacuous" failure mode feedback_negative_test_every_check.md warns
    #      about. That reasoning is WITHDRAWN: docs/audits/
    #      profile_executor_panic_2026-09-10_root_cause.md confirmed the 92 B
    #      CRITICAL rating was a real, recurring crash (two panics, byte-for-
    #      byte identical exception frames), not a benign, irreducible floor
    #      -- it sat "passing" for a full day between the two panics. The
    #      2026-09-10 fix also removed the two side-effect branches that were
    #      inflating this number (see the module docstring), so CRITICAL is
    #      no longer an unfixable structural fact of this task's only path --
    #      it is now, correctly, a hard failure like the other two gates,
    #      and the 940 B/22.9% LOW baseline this check ships with today has
    #      comfortable room before it would ever trip.
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

    if level == "CRITICAL":
        print()
        print(f"check_executor_task_stack_budget: FAIL -- honest free is {honest_free} B "
              f"({honest_pct:.1f}% of {stack_bytes} B), classified CRITICAL (< 15%). "
              "CRITICAL fails this check as of 2026-09-10 -- see the module docstring's "
              "\"CRITICAL NOW FAILS\" section: this exact rating was a confirmed, recurring "
              "profile_executor panic, not a benign floor.")
        print("  Fix by moving large locals or side-effect sends off this stack (as the "
              "2026-09-10 fix did for the announce-version/boot-clear branches), not by "
              "raising UNMODELED_OVERHEAD_BYTES to make the number look better.")
        return 1

    print()
    print("check_executor_task_stack_budget: OK"
          + (f" ({level} honest headroom -- worth a look, not yet failing)" if level != "OK" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
