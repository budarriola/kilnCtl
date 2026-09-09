#!/usr/bin/env python3
"""Worst-case `system_uart_bridge`-task stack usage, measured out of the built ELF.

WHY THIS EXISTS
---------------
2026-09-08 stack audit (docs/audits/2026-09-08-stack-margin-audit.md): only
`main` and `httpd_worker` had ELF-based regression checks, even though
`get_stack_margin()` on hardware reported `system_uart_bridge` LOW at 892 B
free of a 3072 B internal-DRAM stack (29.0% raw headroom) -- one of the six
"TODO.md section 13" candidate stacks never resized off a guess, per this
task's own comment in uart_bridge_start_system_task(). Unlike
`info_uart_bridge`/`telemetry_log`, this task's stack is plain internal DRAM
(`xTaskCreatePinnedToCore`, no MALLOC_CAP_SPIRAM), the scarce resource this
codebase has already run out of once (see the "ESP internal DRAM exhaustion"
class). `system_bridge_task` (uart_bridge_system.c) is a link-frame-command
dispatcher -- IO_CMD_SET_RELAY[_MASK], CT cal, danger-mode, factory-reset,
watchdog-cfg commands all land here -- so new command handling is exactly
the kind of change plausible to deepen this path, and a real overflow here
means link command handling corrupts the heap the same way `boot_guard.h`'s
two prior stack-overflow incidents did.

WHAT IT DOES
------------
Reuses check_main_task_stack_budget.py's ELF/objdump/deepest-path machinery,
rooted at `system_bridge_task` instead of `app_main`.

CEILING, not a headroom-fraction budget -- same reasoning as
check_httpd_task_stack_budget.py: this task is not comfortably under budget
today, so this check exists to catch the deepest reachable path getting
WORSE, not to relitigate whatever the current worst case measures at.

UNMODELED_OVERHEAD_BYTES: unlike httpd_worker, this task has no ESP-IDF
httpd dispatch layer underneath it -- it is a single FreeRTOS task blocked
on `uart_protocol_register_task()`'s inbox queue, so there is no analogous
per-request dispatch cost to add. The ISR-window-spill component httpd's
1800 B allowance also covers still applies here (any task can be
interrupted, and Xtensa's windowed-register ABI spills onto whichever
stack is current) -- 300 B is used as a conservative, order-of-magnitude
placeholder for that alone, NOT re-derived from a live hardware
measurement the way httpd's 1800 B was. Treat it as an assumption, not a
measured constant: it has not been validated the way httpd's gap was
(deliberately probing this task's deepest frame and comparing against the
live high-water mark), because this task does not have handler roots to
probe individually the way httpd does. If a future measurement disagrees,
update this constant and document the new source the way
check_httpd_task_stack_budget.py's UNMODELED_OVERHEAD_BYTES comment does.

LIMITS: same as check_main_task_stack_budget.py (indirect calls not
followed, recursion cut at first repeat, ISR spill not modelled beyond the
placeholder above) -- an UNDER-estimate of true worst-case depth.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402

ROOT = "system_bridge_task"
CONFIGURED_STACK_BYTES = 3072  # must match uart_bridge_start_system_task()'s xTaskCreatePinnedToCore() literal

# Measured 2026-09-08 against KilnCtrl.elf as built that day -- see this
# module's docstring for the "TODO.md section 13" unresized-stack context.
# Retighten to the new worst case if it legitimately moves; do not raise it
# to paper over a regression without checking the code that deepened it.
#
# 2026-09-08 follow-up (same day, same audit): system_bridge_task's own
# uart_proto_message_t `msg` (~256 B on this build) moved from a plain stack
# local to a single heap_caps_malloc(..., MALLOC_CAP_INTERNAL) allocation
# made once before the task's `while (true)` loop (never freed -- the task
# never returns) -- see uart_bridge_system.c's comment at that allocation.
# That shrank system_bridge_task's OWN frame from 336 B to 80 B, dropping
# the deepest reachable path (still through the cfg_fs_confirm_format_device
# chain -- see this module's docstring; that chain is off-limits cfg_fs*
# code and was not touched) from 2192 B to 1936 B. Retightened to match.
CEILING_BYTES = 1936

UNMODELED_OVERHEAD_BYTES = 300


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=base.DEFAULT_ELF)
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                     help="override CEILING_BYTES (used by the negative test)")
    args = ap.parse_args()
    ceiling = args.ceiling_bytes if args.ceiling_bytes is not None else CEILING_BYTES

    if not os.path.isfile(args.elf):
        print(f"check_system_uart_bridge_stack_budget: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build) and re-run; unmeasured, not passing.")
        return 3

    objdump = base.find_objdump()
    if not objdump:
        print("check_system_uart_bridge_stack_budget: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3

    frames, calls = base.parse(objdump, args.elf)
    if ROOT not in frames:
        print(f"check_system_uart_bridge_stack_budget: FAIL -- {ROOT} not found in {args.elf} "
              "(has system_bridge_task been renamed?)")
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
        print(f"check_system_uart_bridge_stack_budget: FAIL -- {total} B exceeds the {ceiling} B ceiling.")
        print("  system_uart_bridge is a plain internal-DRAM stack (no MALLOC_CAP_SPIRAM) already "
              "measured LOW on hardware (892 B free of 3072 B, 2026-09-08). Fix by moving new large "
              "locals off this task's stack (heap-allocate, same convention as "
              "backup_import_apply()/setup_progress_http.c), not by enlarging the stack.")
        return 1

    print("check_system_uart_bridge_stack_budget: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
