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
(was `xTaskCreatePinnedToCore`; PSRAM-capable since 2026-10-14), the scarce resource this
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
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402

ROOT = "system_bridge_task"
BRIDGE_SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "drivers", "bridge", "uart_bridge_system.c")


_LITERAL_OR_COMMENT = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)
_ZERO = re.compile(r"^\(?\s*(0[uUlL]*|!\s*1[uUlL]*)\s*\)?$")
_ONE = re.compile(r"^\(?\s*1[uUlL]*\s*\)?$")


def strip_dead_c(src):
    """Comments (string/char-literal aware, so a "/*" in a string is not a comment opener) and preprocessor-dead
    regions (`#if 0`, `#if 0L`, `#if !1`, `#elif 0`, the else of `#if 1`) removed (REVIEW_DEVBREAK5 LOW-5).
    Unknown conditions stay live in every branch, so a decoy inside one trips the exactly-one counts."""
    src = re.sub(r"\\\r?\n", "", src)
    src = _LITERAL_OR_COMMENT.sub(lambda m: " " if m.group(0).startswith("/") else m.group(0), src)
    out, stack, live = [], [], True
    for ln in src.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if)\b(.*)$", ln)
        if m:
            c = m.group(2).strip()
            kind = "unknown"
            if m.group(1) == "if" and _ZERO.match(c):
                kind = "false"
            elif m.group(1) == "if" and _ONE.match(c):
                kind = "true"
            stack.append([live, kind])
            live = live and kind != "false"
            continue
        m = re.match(r"\s*#\s*elif\b(.*)$", ln)
        if m and stack:
            kind = "false" if _ZERO.match(m.group(1).strip()) else "unknown"
            stack[-1][1] = kind
            live = stack[-1][0] and kind != "false"
            continue
        if re.match(r"\s*#\s*else\b", ln) and stack:
            live = stack[-1][0] and stack[-1][1] != "true"
            continue
        if re.match(r"\s*#\s*endif\b", ln) and stack:
            live = stack.pop()[0]
            continue
        if live:
            out.append(ln)
    return "\n".join(out)


def read_configured_stack(path=BRIDGE_SOURCE):
    """Derive the task stack from the C source (REVIEW_DEVBREAK4 LOW-4): the literal passed to
    xTaskCreatePinnedToCore(system_bridge_task, "system_uart_bridge", N, ...) and the one passed to
    stack_margin_register("system_uart_bridge", ..., N) must both exist, be unique and agree.
    Returns (bytes, error_message)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            src = fh.read()
    except OSError as exc:
        return None, f"cannot read {path}: {exc}"
    src = strip_dead_c(src)
    # REVIEW_DEVBREAKFX6 LOW-3: count EVERY create/register site that names the task, parseable or not, and require
    # exactly one of each; a site whose size is not a plain decimal/hex literal is a failure, never "absent" (a hex
    # real create beside a decimal decoy used to leave the decoy as the only parseable match).
    any_create = re.findall(r'xTaskCreate\w*\s*\([^;]*?"system_uart_bridge"', src)
    any_reg = re.findall(r'stack_margin_register\s*\(\s*"system_uart_bridge"', src)
    created = re.findall(r'xTaskCreatePinnedToCoreWithCaps\s*\(\s*system_bridge_task\s*,\s*"system_uart_bridge"\s*,\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\s*,', src)
    regd = re.findall(r'stack_margin_register\s*\(\s*"system_uart_bridge"\s*,[^;]*?,\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\s*\)\s*;', src)
    if len(any_create) != 1 or len(any_reg) != 1 or len(created) != 1 or len(regd) != 1:
        return None, (f"expected exactly one xTaskCreate*(... \"system_uart_bridge\" ...) and one "
                      f"stack_margin_register(\"system_uart_bridge\", ...), each with a plain decimal/hex literal size, in {path}; "
                      f"found {len(any_create)} create site(s) ({len(created)} parseable) and {len(any_reg)} register site(s) ({len(regd)} parseable)")
    c_val, r_val = int(created[0], 0), int(regd[0], 0)
    if c_val != r_val:
        return None, (f"{path}: task created with stack {c_val} B but stack_margin_register() reports against "
                      f"{r_val} B -- the margin is graded against the wrong denominator")
    return c_val, None


CONFIGURED_STACK_BYTES, _STACK_ERR = read_configured_stack()

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
# 2026-10-09: long calls (l32r+callx) now followed: measured 3136 B (was 1936 B); stack raised 3072 -> 4096.
# 2026-10-10: 3136 -> 3152 B (+16 B in a frame on the factory_reset_execute/execute_scope chain after the
# persist K10 fixes touched factory_reset.c); honest headroom 644 B of 4096 B, above the floor (the earlier 256 B figure here was stale; the enforced floor is MIN_HONEST_HEADROOM_BYTES = 512 B, 2026-10-11), so no stack bump.
# 2026-10-10: 3152 -> 3216 B re-measured; honest headroom still 580 B of 4096 B after the 300 B allowance.
CEILING_BYTES = 3216

UNMODELED_OVERHEAD_BYTES = 300

# Honest headroom floor, enforced (2026-10-11, REVIEW_DEVBREAK3 LOW-4): retightening CEILING_BYTES upward must not walk
# the stack into the margin with the check green; a breach forces a stack bump. Same value as the all-task checker's absolute floor.
MIN_HONEST_HEADROOM_BYTES = 512


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=base.DEFAULT_ELF)
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                     help="override CEILING_BYTES (used by the negative test)")
    args = ap.parse_args()
    ceiling = args.ceiling_bytes if args.ceiling_bytes is not None else CEILING_BYTES

    if CONFIGURED_STACK_BYTES is None:
        print(f"check_system_uart_bridge_stack_budget: FAIL -- {_STACK_ERR}")
        return 1

    if not os.path.isfile(args.elf):
        print(f"check_system_uart_bridge_stack_budget: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build) and re-run; unmeasured, not passing.")
        return 3

    objdump = base.find_objdump()
    if not objdump:
        print("check_system_uart_bridge_stack_budget: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3

    try:
        frames, calls = base.parse(objdump, args.elf)
    except base.ElfParseError as exc:
        if exc.skip:
            print(f"check_system_uart_bridge_stack_budget: SKIP: {exc}")
            return 3
        print(f"check_system_uart_bridge_stack_budget: FAIL -- {exc}")
        return 1
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

    if honest_headroom < MIN_HONEST_HEADROOM_BYTES:
        print()
        print(f"check_system_uart_bridge_stack_budget: FAIL -- honest headroom {honest_headroom} B is below the "
              f"{MIN_HONEST_HEADROOM_BYTES} B floor; raise the system_uart_bridge stack instead of raising the ceiling.")
        return 1

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
