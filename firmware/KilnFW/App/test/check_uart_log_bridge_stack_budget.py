#!/usr/bin/env python3
"""Worst-case `uart_log_bridge`-task stack usage, measured out of the built ELF.

WHY THIS EXISTS
----------------
`uart_log_bridge_task` was one of six long-lived UART bridge tasks that ran
every boot with no `stack_margin_register()` call site at all until
`36394fbd` (2026-09-08) wired all six in. Once measurable, live
`get_stack_margin()` reported it the ONLY task in the whole registry
classified LOW: 1080 B free of a 4096 B PSRAM stack (26.4% headroom) --
worse than five other newly-registered peers measured the same day. It
deserves its own regression gate specifically because it is the LOG path:
a log task running out of stack while reporting a fault would take out the
diagnostics exactly when they matter, and this codebase's log traffic is
bursty and growing (see this file's own history -- `b12faf41`/`cb6f3cd5`
widened the eviction policy to retain WARN lines, not just drop them).

WHAT WAS FOUND
--------------
`uart_log_bridge_task`'s OWN frame was 832 B of the measured 1984 B total
static path -- by far the largest single contributor, ahead of the shared
`uart_protocol_send_limited` -> `frame_and_send$constprop$0` send-path
frames every UART bridge task pays (1152 B, unrelated to this task
specifically). That 832 B was three ~253-256 B buffers that used to be
plain stack locals: `entry` (the dequeued `uart_log_entry_t`), `payload`
(entry + level byte, built and sent) and `drop_payload` (the separate
"N line(s) dropped" notice) -- none of them formatted with `vsnprintf`
directly in this function (that happens in `uart_log_vprintf`, which runs
on whichever task is logging, not on this task).

FIX APPLIED
-----------
All three moved to static storage inside `uart_log_bridge.c` (see
`s_log_task_entry`/`s_log_task_send_buf` there for the full reasoning):
single-instance task, sole reader of its queue, nothing reentrant or
ISR-called, so static storage carries no concurrent-access hazard. `payload`
and `drop_payload` are never live at the same time (payload is fully sent
before drop_payload is even built), so they share ONE static buffer instead
of two. Plain `static` (ordinary internal-DRAM `.bss`), not
`MALLOC_CAP_SPIRAM`/heap: this task never writes flash or NVS, so there is
no flash-write-from-PSRAM-stack hazard to dodge the way `bx_flash_worker`'s
callers must -- a compile-time static is simplest here. This dropped
`uart_log_bridge_task`'s own frame to 48 B and the deepest static path from
1984 B to 1200 B.

CEILING, not a headroom-fraction budget -- same reasoning as
check_httpd_task_stack_budget.py / check_system_uart_bridge_stack_budget.py:
this task was measured LOW on hardware, so this check exists to catch the
deepest reachable path getting WORSE, not to relitigate the current depth.

UNMODELED_OVERHEAD_BYTES: this task, like system_uart_bridge, is a single
FreeRTOS task with no ESP-IDF dispatch layer underneath it -- it blocks on
its own queue (`xQueueReceive`), not on `uart_protocol_register_task()`'s
inbox. Live measurement gives a direct cross-check instead of a placeholder
guess: 4096 B configured - 1080 B live free (pre-fix) = 3016 B actually
used at the worst observed mark, against this script's pre-fix static
result of 1984 B -- a 1032 B gap attributed to ISR window-spill plus
whatever the FreeRTOS scheduler/queue-wait path itself costs on top of the
statically-visible call chain. 1032 B is used directly (not rounded to a
placeholder) since a real live/static pair is available here, unlike
system_uart_bridge's 300 B guess.

LIMITS: same as check_main_task_stack_budget.py (indirect calls not
followed, recursion cut at first repeat, ISR spill not modelled beyond the
allowance above) -- an UNDER-estimate of true worst-case depth.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402

ROOT = "uart_log_bridge_task"
CONFIGURED_STACK_BYTES = 4096  # must match uart_log_bridge_start()'s xTaskCreatePinnedToCoreWithCaps() literal

# Measured 2026-09-08 against KilnCtrl.elf as built that day, AFTER moving
# `entry`/`payload`/`drop_payload` off uart_log_bridge_task's stack (see
# this module's docstring). Pre-fix this was 1984 B (832 B of it this
# task's own frame). Retighten to the new worst case if it legitimately
# moves; do not raise it to paper over a regression without checking the
# code that deepened it.
CEILING_BYTES = 1200

# See "UNMODELED_OVERHEAD_BYTES" in this module's docstring: derived from a
# real live-vs-static pair (1080 B free / 4096 B live, pre-fix 1984 B
# static), not a placeholder guess.
UNMODELED_OVERHEAD_BYTES = 1032


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=base.DEFAULT_ELF)
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                     help="override CEILING_BYTES (used by the negative test)")
    args = ap.parse_args()
    ceiling = args.ceiling_bytes if args.ceiling_bytes is not None else CEILING_BYTES

    if not os.path.isfile(args.elf):
        print(f"check_uart_log_bridge_stack_budget: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build) and re-run; unmeasured, not passing.")
        return 3

    objdump = base.find_objdump()
    if not objdump:
        print("check_uart_log_bridge_stack_budget: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3

    frames, calls = base.parse(objdump, args.elf)
    if ROOT not in frames:
        print(f"check_uart_log_bridge_stack_budget: FAIL -- {ROOT} not found in {args.elf} "
              "(has uart_log_bridge_task been renamed?)")
        return 1

    total, path = base.deepest(ROOT, frames, calls)
    honest_headroom = CONFIGURED_STACK_BYTES - total - UNMODELED_OVERHEAD_BYTES
    print(f"deepest static stack path from {ROOT}: {total} B "
          f"(ceiling {ceiling} B; configured stack {CONFIGURED_STACK_BYTES} B; "
          f"honest headroom after {UNMODELED_OVERHEAD_BYTES} B unmodeled-overhead allowance: "
          f"{honest_headroom} B)")
    running = 0
    for fn in [ROOT] + path:
        running += frames.get(fn, 0)
        print(f"    {frames.get(fn, 0):>6} B  {running:>6} B cumulative  {fn}")

    if total > ceiling:
        print()
        print(f"check_uart_log_bridge_stack_budget: FAIL -- {total} B exceeds the {ceiling} B ceiling.")
        print("  uart_log_bridge is the LOG delivery path and was measured LOW on hardware "
              "(1080 B free of 4096 B, 26.4%, 2026-09-08) even before this ceiling was set. Fix by "
              "moving new large locals off this task's own stack (static/heap, same convention as "
              "s_log_task_entry/s_log_task_send_buf in uart_log_bridge.c), not by enlarging the stack.")
        return 1

    print("check_uart_log_bridge_stack_budget: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
