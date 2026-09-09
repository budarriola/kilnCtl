#!/usr/bin/env python3
"""Worst-case stack usage for every FreeRTOS task SaftyFW creates, measured
out of the built SaftyFW.elf. The RP2040/Cortex-M0+ analogue of KilnFW's
check_all_task_stack_budgets.py -- see stack_budget_lib_arm.py's module
docstring for why the ELF-walk itself had to be a genuinely different
implementation (Thumb push/sub-sp frames, not Xtensa's single `entry a1,N`).

WHY THIS EXISTS
---------------
2026-09-09: a dual reflash deadlocked the Pico. current_task.c and
discrete_task.c were running on bare configMINIMAL_STACK_SIZE and
overflowed on core 1 -- confirmed via SWD register reads (core 1 parked in
vApplicationStackOverflowHook, core 0 permanently deadlocked in a spinlock
wait), reproduced twice on independent reflashes. Fixed per-task in
3afc5ea6 by matching the *6 pattern link_task/safety_core already used.
Before this file, SaftyFW had NO stack-budget check at all -- every
check_*.ps1 and stack_budget_lib.py in this repo was KilnFW-only. This is
the fourth stack overflow this project has suffered (see CLAUDE.md's
firmware-gotchas section); this check exists to make the fifth one loud
before hardware rather than after.

NO HAND-COPIED STACK SIZES
---------------------------
Every TASKS[*]['stack_words'] entry reads the declared stack depth back out
of the real #define at its task's own .c file (STACK_WORDS_RE below), after
confirming the real xTaskCreate() call site actually references that
macro -- never a bare integer typed into this file. FreeRTOS's RP2040 SMP
port's xTaskCreate() takes stack DEPTH IN WORDS (StackType_t == uint32_t on
this target, confirmed via FreeRTOSConfig.h -- NOT bytes, unlike the
ESP-IDF variants KilnFW's own checker deals with); every declared_bytes()
call below multiplies by 4 explicitly so this distinction is stated, not
assumed silently.

WHAT THIS CAN AND CANNOT MEASURE (see stack_budget_lib_arm.py for the full
reasoning; summarized here per the honesty requirement)
---------------------------------------------------------------------------
A task whose resolvable call graph contains a `blx <reg>` (register-
indirect call -- a function pointer, e.g. a pico-sdk IRQ callback
registration) is reported INDETERMINATE, never a plain pass: deepest()'s
total for that task is a LOWER BOUND only, and the real worst case may be
larger by an amount this walk cannot see at all. Recursion is cut at first
repeat (memoized only when the subtree computed with no cut in it, so a cut
branch is recomputed on reuse rather than poisoning a later call path --
same fix stack_budget_lib.py's deepest() carries for KilnFW, 2026-09-09).
ISR exception-entry stacking (Cortex-M0+ pushes r0-r3/r12/lr/pc/xPSR = 32 B
onto whatever stack is active at interrupt time) is added as a flat,
documented allowance below, not modelled instruction-by-instruction.
Thumb-2 `sub.w sp, sp, #N` (needed once a function's locals exceed 508 B)
is not matched by this walk's SUB_SP_RE; stack_budget_lib_arm.parse() sets
has_subw_anywhere=True if the built ELF now contains one ANYWHERE, and this
script refuses to report success at all if so, rather than silently
under-measuring every task in that build (see main()).

EXIT CODES: 0 OK (every ceiling-graded task within budget; INDETERMINATE
tasks are noted, not failed, as long as their own known lower bound plus
ISR allowance is within budget), 1 FAIL (a task over its ceiling, a task
missing a CEILING_BYTES entry, or the ELF now contains a sub.w sp pattern
this walk cannot measure), 3 SKIP (no ELF / no arm-none-eabi-objdump --
never claims success without measuring).
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import stack_budget_lib_arm as lib  # noqa: E402

REPO_ROOT = lib.REPO_ROOT
DEFAULT_ELF = lib.DEFAULT_ELF
SRC_TASKS_DIR = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "src", "tasks")
FREERTOS_CONFIG = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "FreeRTOSConfig.h")

# Cortex-M0+ exception entry (NVIC hardware stacking): r0-r3, r12, lr, pc,
# xPSR = 8 words = 32 bytes, pushed onto whichever stack was active at the
# moment of interrupt -- every FreeRTOS task here can be interrupted, so
# this is added as a flat allowance on top of every measured/lower-bound
# total below rather than modelled instruction-by-instruction.
ISR_STACKING_BYTES = 32


def _read(name):
    path = os.path.join(SRC_TASKS_DIR, name)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


_MINIMAL_STACK_WORDS = None


def minimal_stack_words():
    global _MINIMAL_STACK_WORDS
    if _MINIMAL_STACK_WORDS is None:
        text = open(FREERTOS_CONFIG, encoding="utf-8", errors="replace").read()
        m = re.search(r"#define\s+configMINIMAL_STACK_SIZE\s+(\d+)", text)
        if not m:
            raise ValueError(f"configMINIMAL_STACK_SIZE not found in {FREERTOS_CONFIG}")
        _MINIMAL_STACK_WORDS = int(m.group(1))
    return _MINIMAL_STACK_WORDS


def declared_words(file_name, macro_name, create_pattern):
    """Read a task's declared stack DEPTH IN WORDS from its own #define,
    after confirming the real xTaskCreate() call site references that exact
    macro (never hand-copied -- see module docstring)."""
    text = _read(file_name)
    if not re.search(create_pattern, text, re.DOTALL):
        raise ValueError(f"xTaskCreate call site does not reference {macro_name!r} in {file_name}")
    # Bare "configMINIMAL_STACK_SIZE" or "(configMINIMAL_STACK_SIZE * N)".
    m = re.search(
        r"#define\s+" + re.escape(macro_name) + r"\s+\(?\s*configMINIMAL_STACK_SIZE\s*(?:\*\s*(\d+))?\s*\)?",
        text)
    if not m:
        raise ValueError(f"#define {macro_name} not found (or not configMINIMAL_STACK_SIZE-derived) in {file_name}")
    multiplier = int(m.group(1)) if m.group(1) else 1
    return minimal_stack_words() * multiplier


# ---------------------------------------------------------------------------
# TASKS: one row per xTaskCreate() call site in firmware/SaftyFW/src/tasks/.
# `root` is the task's own C entry function (xTaskCreate's first argument).
# Enumerated 2026-09-09 by grepping every xTaskCreate( call under
# src/tasks/ -- see this file's companion audit in the commit message for
# the exact grep. If a new task is added, this table must grow with it or
# check_saftyfw_task_count.py (added alongside this file) fails loud.
# ---------------------------------------------------------------------------
TASKS = [
    dict(name="current_task", root="current_task_fn",
         words=lambda: declared_words("current_task.c", "CURRENT_TASK_STACK_WORDS",
             r'xTaskCreate\(current_task_fn,\s*"current_task",\s*CURRENT_TASK_STACK_WORDS')),
    dict(name="discrete_task", root="discrete_task_fn",
         words=lambda: declared_words("discrete_task.c", "DISCRETE_TASK_STACK_WORDS",
             r'xTaskCreate\(discrete_task_fn,\s*"discrete_task",\s*DISCRETE_TASK_STACK_WORDS')),
    dict(name="link_task", root="link_task_fn",
         words=lambda: declared_words("link_task.c", "LINK_TASK_STACK_WORDS",
             r'xTaskCreate\(link_task_fn,\s*"link_task",\s*LINK_TASK_STACK_WORDS')),
    dict(name="log_task", root="log_task_fn",
         words=lambda: declared_words("log_task.c", "LOG_TASK_STACK_WORDS",
             r'xTaskCreate\(log_task_fn,\s*"log_task",\s*LOG_TASK_STACK_WORDS')),
    dict(name="relay_owner", root="relay_owner_task",
         words=lambda: declared_words("relay_owner.c", "RELAY_OWNER_STACK_WORDS",
             r'xTaskCreate\(relay_owner_task,\s*"relay_owner",\s*RELAY_OWNER_STACK_WORDS')),
    dict(name="safety_core", root="safety_core_task",
         words=lambda: declared_words("safety_core.c", "SAFETY_CORE_STACK_WORDS",
             r'xTaskCreate\(safety_core_task,\s*"safety_core",\s*SAFETY_CORE_STACK_WORDS')),
    dict(name="thermo_task", root="thermo_task_fn",
         historical_note="2026-09-09: bumped from bare configMINIMAL_STACK_SIZE to *4 after this "
                          "checker measured an 800 B resolved-lower-bound floor (INDETERMINATE, "
                          "runs through a pico-sdk blx) against a 1024 B stack -- same overflow "
                          "shape as current_task/discrete_task's real 2026-09-09 hardware overflow.",
         words=lambda: declared_words("thermo_task.c", "THERMO_TASK_STACK_WORDS",
             r'xTaskCreate\(thermo_task_fn,\s*"thermo_task",\s*THERMO_TASK_STACK_WORDS')),
    dict(name="update_task", root="update_task_fn",
         words=lambda: declared_words("update_task.c", "UPDATE_TASK_STACK_WORDS",
             r'xTaskCreate\(update_task_fn,\s*"update_task",\s*UPDATE_TASK_STACK_WORDS')),
    dict(name="watchdog_task", root="watchdog_task_fn",
         words=lambda: declared_words("watchdog_task.c", "WATCHDOG_TASK_STACK_WORDS",
             r'xTaskCreate\(watchdog_task_fn,\s*"watchdog_task",\s*WATCHDOG_TASK_STACK_WORDS')),
]

# Measured 2026-09-09 against SaftyFW.elf as built that day (the run that
# added this check) -- see module docstring "EXIT CODES". A ceiling here is
# the deepest resolved-lower-bound path plus ISR_STACKING_BYTES measured at
# that time, not a theoretical maximum; retighten down if a fix legitimately
# shrinks it, never raise one to paper over a regression without documenting
# why in this file.
CEILING_BYTES = {
    "current_task": 348,
    "discrete_task": 184,
    "link_task": 784,
    "log_task": 472,
    "relay_owner": 224,
    "safety_core": 780,
    "thermo_task": 832,
    "update_task": 796,
    "watchdog_task": 288,
}


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--dump-ceilings", action="store_true")
    ap.add_argument("--force-ceiling", metavar="TASK=BYTES", action="append", default=[])
    args = ap.parse_args()

    forced_ceilings = {}
    for item in args.force_ceiling:
        k, _, v = item.partition("=")
        forced_ceilings[k] = int(v)

    if not os.path.isfile(args.elf):
        print("check_saftyfw_task_stack_budgets: SKIP: no ELF at " + args.elf)
        print("  Build SaftyFW and re-run; unmeasured, not passing -- this is a SKIP, not a pass.")
        return 3

    objdump = lib.find_objdump()
    if not objdump:
        print("check_saftyfw_task_stack_budgets: SKIP: arm-none-eabi-objdump not found "
              "(set ARM_OBJDUMP). Unmeasured, not passing.")
        return 3
    addr2line = lib.find_addr2line()

    parsed = lib.parse(objdump, args.elf)

    if parsed.has_subw_anywhere:
        print("check_saftyfw_task_stack_budgets: FAIL: this ELF now contains a `sub.w sp, sp, #N` "
              "instruction (Thumb-2 32-bit stack-adjust, needed once a function's locals exceed "
              "508 B) -- stack_budget_lib_arm.py's frame extraction does not match this form (see "
              "its module docstring), so every measurement below would silently under-count that "
              "function's frame. Refusing to report success rather than measure dishonestly; "
              "extend SUBW_SP_RE in stack_budget_lib_arm.py before trusting this checker again.")
        return 1

    # Register-operand add/sub sp,rN (see stack_budget_lib_arm.py's "REGISTER-COMPUTED
    # stack adjust") is NOT a global refusal here -- it occurs legitimately elsewhere
    # in this ELF (e.g. config_store_unpack_ex.part.0's large local buffers) with no
    # bearing on any task's stack. lib.has_unresolved_dispatch() below checks it
    # PER TASK, against that task's own reachable call graph, and folds it into the
    # same INDETERMINATE handling as an unresolved blx -- see the per-task loop.

    results = []
    errors = []
    for task in TASKS:
        tname = task["name"]
        try:
            root_addr = lib.resolve_root(parsed, task["root"], args.elf, addr2line,
                                          task.get("expect_path"))
        except ValueError as e:
            errors.append(f"{tname}: could not resolve root symbol {task['root']!r}: {e}")
            continue
        try:
            words = task["words"]()
        except ValueError as e:
            errors.append(f"{tname}: could not derive declared stack size from source: {e}")
            continue
        declared_bytes = words * 4  # RP2040 FreeRTOS SMP port: xTaskCreate takes DEPTH IN WORDS

        own_total, path_addrs = lib.deepest(root_addr, parsed)
        indeterminate = lib.has_unresolved_dispatch(root_addr, parsed)
        measured_total = own_total + ISR_STACKING_BYTES

        ceiling = forced_ceilings.get(tname, CEILING_BYTES.get(tname))
        if ceiling is None:
            errors.append(f"{tname}: no CEILING_BYTES entry -- every table row must be graded")
            continue

        over_ceiling = measured_total > ceiling
        over_declared = measured_total > declared_bytes
        results.append(dict(name=tname, declared=declared_bytes, measured=measured_total,
                             raw=own_total, ceiling=ceiling, indeterminate=indeterminate,
                             over_ceiling=over_ceiling, over_declared=over_declared,
                             historical_note=task.get("historical_note")))

    if args.dump_ceilings:
        print("CEILING_BYTES = {")
        for r in results:
            print(f'    "{r["name"]}": {r["measured"]},')
        print("}")
        return 0

    fail = bool(errors)
    print("check_saftyfw_task_stack_budgets: SaftyFW (RP2040/Cortex-M0+) per-task stack budgets")
    print(f"  ELF: {args.elf}")
    print(f"  ISR stacking allowance added to every total: {ISR_STACKING_BYTES} B")
    print()
    for r in sorted(results, key=lambda x: -x["measured"]):
        tag = "INDETERMINATE (lower bound)" if r["indeterminate"] else "measured"
        margin = r["declared"] - r["measured"]
        status = "FAIL(ceiling)" if r["over_ceiling"] else ("FAIL(>declared)" if r["over_declared"] else "ok")
        if r["over_ceiling"] or r["over_declared"]:
            fail = True
        print(f"  {r['name']:16s} {tag:28s} total={r['measured']:5d} B  "
              f"declared={r['declared']:5d} B  margin={margin:5d} B  "
              f"ceiling={r['ceiling']:5d} B  [{status}]")
        if r["historical_note"]:
            print(f"      note: {r['historical_note']}")
    if errors:
        print()
        for e in errors:
            print(f"  ERROR: {e}")

    print()
    n_indet = sum(1 for r in results if r["indeterminate"])
    print(f"  {len(results)} tasks graded, {n_indet} INDETERMINATE (real depth may exceed the "
          "reported lower bound -- see stack_budget_lib_arm.py)")

    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
