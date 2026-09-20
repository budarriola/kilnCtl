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
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import stack_budget_lib_arm as lib  # noqa: E402

REPO_ROOT = lib.REPO_ROOT
DEFAULT_ELF = lib.DEFAULT_ELF
SRC_TASKS_DIR = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "src", "tasks")
SRC_DIR = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "src")
FREERTOS_CONFIG = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "FreeRTOSConfig.h")

# FRESHNESS GATE -------------------------------------------------------------
# 2026-09-09: this checker was run once against a SaftyFW.elf built the
# PREVIOUS day (Sep 8 21:40), 16-20 hours stale against thermo_task.c,
# current_task.c, discrete_task.c and config_store_flash.c (all edited Sep 9
# for the seqlock work) -- with no gate at all, it happily printed confident
# "measured"/INDETERMINATE numbers off a binary that did not contain the
# seqlock's large per-getter scratch-copy frames at all. This is the same
# failure class af0bb774 fixed for KilnFW's check_00 the same day: compare
# artifact mtime against the newest mtime among the sources that feed the
# measurement, and refuse rather than measure when the artifact is older.
# Originally every *.c/*.h directly under src/tasks/ and src/ (non-
# recursive globs) plus FreeRTOSConfig.h and this checker's own two files.
# 2026-09-10 (opus review): that non-recursive src/*.{c,h} glob silently
# excluded roughly half of what actually feeds the measurement --
# firmware/SaftyFW/src/update/*.c (update_receiver.c, image_header.c,
# confirm.c, received_ranges.c -- all in update_task's reachable call
# graph) and firmware/SaftyFW/src/board/ are both one level too deep for a
# non-recursive glob to see; firmware/hwAbstraction/ (hal_uart_pico_
# internal.h, hal_flash_pico.c, hal_gpio* -- reached by every task, e.g.
# link_task.c's own #include "hal_uart_pico_internal.h") and
# firmware/CommonFW/ (link_frame.h and friends, link_task's kilnlink frame
# codecs) were excluded ENTIRELY. Edit any of those and this gate reported
# "fresh" on an ELF that predated the edit -- exactly the failure mode this
# gate exists to catch (the third stale-artifact incident in two days).
# check_saftyfw_task_count.py already gets this right with os.walk(); this
# checker did not, despite living right next to it. Now recursive, and
# rooted at every directory whose contents can end up on a task's call
# graph or feed this checker's own measurement logic, not just the ones
# named in TASKS below (a task's graph routinely calls into a sibling
# task's helper, and every task here calls into config_store).
FRESHNESS_ROOTS = (
    SRC_DIR,   # recursive: src/tasks/, src/update/, src/board/, src/*.c|h
    os.path.join(REPO_ROOT, "firmware", "hwAbstraction"),
    os.path.join(REPO_ROOT, "firmware", "CommonFW"),
)
FRESHNESS_EXTRA_FILES = (
    FREERTOS_CONFIG,
    os.path.join(os.path.dirname(__file__), "check_saftyfw_task_stack_budgets.py"),
    os.path.join(os.path.dirname(__file__), "stack_budget_lib_arm.py"),
)
# Build/vendored/test-only content under those roots has no bearing on what
# actually gets linked into SaftyFW.elf's task call graphs and would only
# make this gate spuriously "stale" on every unrelated edit -- excluded the
# same way run_all_checks.ps1 excludes build/vendored trees from its own
# discovery glob.
FRESHNESS_EXCLUDE_DIR_NAMES = {"build", "test", ".git", "docs"}


def newest_source_mtime():
    newest_path, newest_t = None, -1.0

    def consider(path):
        nonlocal newest_path, newest_t
        if not path.endswith((".c", ".h", ".cpp", ".hpp")):
            return
        if not os.path.isfile(path):
            return
        t = os.path.getmtime(path)
        if t > newest_t:
            newest_path, newest_t = path, t

    for root in FRESHNESS_ROOTS:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in FRESHNESS_EXCLUDE_DIR_NAMES]
            for fname in filenames:
                consider(os.path.join(dirpath, fname))
    for path in FRESHNESS_EXTRA_FILES:
        consider(path)

    if newest_t < 0:
        raise ValueError("newest_source_mtime: no source files matched any FRESHNESS_ROOTS/"
                          "FRESHNESS_EXTRA_FILES entry -- refusing to grade artifact freshness "
                          "with no signal to grade it against")
    return newest_path, newest_t

# Cortex-M0+ exception entry (NVIC hardware stacking): r0-r3, r12, lr, pc,
# xPSR = 8 words = 32 bytes, pushed onto whichever stack was active at the
# moment of interrupt -- every FreeRTOS task here can be interrupted, so
# this is added as a flat allowance on top of every measured/lower-bound
# total below rather than modelled instruction-by-instruction.
ISR_STACKING_BYTES = 32

# See the "REGSP_MARGIN_FACTOR safety margin" comment in main() below --
# applies only to a task whose graph still contains a register-operand
# add/sub sp,rN this walk could not resolve to an exact byte count.
REGSP_MARGIN_FACTOR = 2


def regsp_margin_fail(measured_total, declared_bytes, unresolved_regsp):
    """See the "REGSP_MARGIN_FACTOR safety margin" comment in main()'s
    per-task loop. Graded against DECLARED stack, not a hand-maintained
    ceiling -- a ceiling in CEILING_BYTES below was set to exactly
    2x its own measurement for every task this applies to, which made
    grading against `ceiling` false by construction (2026-09-10, opus
    review round 2). Pulled out to its own function so this specific
    comparison can be unit-tested directly rather than only through a full
    ELF walk."""
    return bool(unresolved_regsp) and (measured_total * REGSP_MARGIN_FACTOR > declared_bytes)


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

# ---------------------------------------------------------------------------
# ELF-SIDE RECONCILIATION -- "stop mixing sources" (opus review, 2026-09-09).
# `declared_words()`/`declared_bytes` above are parsed from the CURRENT C
# SOURCE (the macro's #define text). `measured`, everywhere else in this
# file, comes from walking the ELF's disassembly. Before the freshness gate
# above existed, those two could silently describe two different builds --
# and even with the gate (which only proves the ELF is not OLDER than the
# source, not that this ELF was actually compiled FROM this exact source),
# a renamed macro, a changed call-site pattern, or a wrong multiplier could
# still make declared_bytes print a number that is not what xTaskCreate()
# was actually handed in the binary being measured. FreeRTOS's RP2040
# xTaskCreate() takes usStackDepth as its 3rd argument (r2); every task here
# compiles that argument as `movs r2, #N` immediately followed by
# `lsls r2, r2, #k` (N << k words) right before the `bl xTaskCreate` --
# confirmed by inspecting all nine call sites in SaftyFW.elf as built
# 2026-09-09, see the commit that added this reconciliation. This walks the
# ELF a second, independent way and cross-checks the two: mismatch is a
# hard FAIL naming both numbers, never a silent pick-one.
# ---------------------------------------------------------------------------
WORD_RE = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+\.word\s+0x([0-9a-f]+)\s*$")
FN_HDR_RE = re.compile(r"^([0-9a-f]{7,8}) <(.+)>:")
MOVS_R2_RE = re.compile(r"\bmovs\tr2, #(\d+)\b")
LSLS_R2_RE = re.compile(r"\blsls\tr2, r2, #(\d+)\b")
LDR_R0_PC_RE = re.compile(r"\bldr\tr0, \[pc, #\d+\]\s*@ \(([0-9a-f]+)")
BL_XTASKCREATE_RE = re.compile(r"\bbl\t[0-9a-f]+ <xTaskCreate>")


def elf_side_stack_words_by_root(objdump, elf):
    """Independent second walk of the ELF: {root_func_addr: stack_words_actually_
    passed_to_xTaskCreate}, derived entirely from the binary, no source file
    involved. See module-level comment above TASKS' CEILING_BYTES block."""
    out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
    words_by_addr = {}
    for line in out.splitlines():
        m = WORD_RE.match(line)
        if m:
            words_by_addr[int(m.group(1), 16)] = int(m.group(2), 16)

    result = {}
    cur_movs_r2 = None
    cur_ldr_r0_target = None
    for line in out.splitlines():
        if FN_HDR_RE.match(line):
            cur_movs_r2 = None
            cur_ldr_r0_target = None
            continue
        m = MOVS_R2_RE.search(line)
        if m:
            cur_movs_r2 = int(m.group(1))
        m = LSLS_R2_RE.search(line)
        if m and cur_movs_r2 is not None:
            cur_movs_r2 = cur_movs_r2 << int(m.group(1))
        m = LDR_R0_PC_RE.search(line)
        if m:
            cur_ldr_r0_target = int(m.group(1), 16)
        if BL_XTASKCREATE_RE.search(line):
            if cur_movs_r2 is not None and cur_ldr_r0_target is not None:
                fn_ptr = words_by_addr.get(cur_ldr_r0_target)
                if fn_ptr is not None:
                    root_addr = fn_ptr & ~1  # strip Thumb bit
                    result[root_addr] = cur_movs_r2
            cur_movs_r2 = None
            cur_ldr_r0_target = None
    return result


# Measured 2026-09-09 against SaftyFW.elf as built that day (the run that
# added this check) -- see module docstring "EXIT CODES". A ceiling here is
# the deepest resolved-lower-bound path plus ISR_STACKING_BYTES measured at
# that time, not a theoretical maximum; retighten down if a fix legitimately
# shrinks it, never raise one to paper over a regression without documenting
# why in this file.
# Retightened 2026-09-10 (opus review of this checker, see stack_budget_lib_
# arm.py's REGSP_RE/LDR_PC_RE comment): the 2026-09-09 numbers below for
# current_task (348) and discrete_task (184) were themselves the bug this
# review found -- both were pinned at the exact under-measured lower bound
# from BEFORE parse() could resolve config_store_seqlock_read's 532/608 B
# register-computed frames (`ldr r4, [pc,#N]` / `add sp, r4`, invisible to
# the old SUB_SP_RE-only walk), so this checker reported "ok" for two tasks
# it was silently under-measuring by 4-6x -- and those are the same two
# that actually overflowed on 2026-09-09 hardware. Hand-disassembly at the
# time (opus review) put current_task ~1.2 KB and discrete_task ~1.1 KB
# against 6144/4096 B stacks -- genuinely fine margins, this was a checker-
# honesty bug, not a live overflow. Values below are measured against
# SaftyFW.elf as rebuilt 2026-09-10 with the regsp-literal resolution in
# place; current_task and link_task still reach one additional register-
# operand sp adjust this walk cannot resolve to an exact byte count (an
# epilogue restore computed via movs+lsls rather than a PC-relative
# literal -- see stack_budget_lib_arm.has_unresolved_regsp()), so their
# ceilings carry REGSP_MARGIN_FACTOR headroom on top of the measured total
# instead of being pinned at it; main() also independently refuses to pass
# either if a future measurement closes to within that margin (regsp-margin
# FAIL), so this table cannot mask a repeat the way the old numbers did.
CEILING_BYTES = {
    "current_task": 2752,   # measured 1376 B, unresolved regsp -- 2x margin
    "discrete_task": 1160,
    "link_task": 9472,      # measured 4736 B, unresolved regsp -- 2x margin
    "log_task": 472,
    "relay_owner": 224,
    # KILN_PROFILES_PLAN.md item 16: safety_core_task's tick gained one bool
    # local (abs_max_temp_c_unconfigured) and a short branch for the
    # unconfigured-ARMED backstop, moving the measured total 2160 -> 2168 B.
    # Re-pinned to the new measured value, same "pinned at measured, not a
    # padded guess" convention this table's own header comment describes.
    #
    # 2026-09-16, Gap 1 telemetry fix (TODO.md Phase 8): the newly_tripped
    # block now also logs the original trip event via log_task_log() (see
    # safety_core.c's own comment at that call site), adding a `char
    # trip_msg[64]` local and its snprintf() call -- moving the measured
    # total 2168 -> 2176 B. Re-pinned to the new measured value, same
    # convention.
    "safety_core": 2176,
    # Live tc_type reapply (thermo_task_request_tc_type_reapply(), 2026-09-15):
    # thermo_task_fn()'s loop gained two locals (verified_before_retry,
    # forced_reconfigure) around the reconfig-retry gate, moving the measured
    # total 1216 -> 1224 B. Re-pinned to the new measured value, same
    # "pinned at measured, not a padded guess" convention this table's own
    # header comment describes.
    "thermo_task": 1224,
    # 2026-09-18, the mid-erase watchdog reset fix
    # (docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md):
    # update_task_erase_slot() went from an inline offset/remaining walk over
    # 13 64K blocks to a chunk-index walk over 208 4K sectors through
    # update_erase_plan_chunk(), which keeps a chunk_count plus two out-params
    # (offset, len) live across the hal_flash_safe_execute() call -- moving the
    # measured total 2536 -> 2544 B. Re-pinned to the new measured value, same
    # "pinned at measured, not a padded guess" convention this table's own
    # header comment describes. The declared stack (6144 B) is untouched and
    # still leaves 3600 B of margin; this table is an anti-drift ratchet, not
    # the overflow guard.
    #
    # 2026-09-20: update_task_process_end() now calls
    # update_task_slot_linkage_plausible() (src/tasks/update_task.c) before
    # the metadata flip -- see docs/BOOTLOADER.md's "Slot-linkage
    # plausibility checked before the metadata flip" entry. That call's own
    # small local frame (two uint32_t words plus a pointer) moved the
    # measured total 2544 -> 2560 B. Re-pinned again to the new measured
    # value, same ratchet as above; declared stack (6144 B) unchanged, still
    # 3584 B of margin.
    "update_task": 2560,
    "watchdog_task": 304,
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

    elf_mtime = os.path.getmtime(args.elf)
    newest_path, newest_mtime = newest_source_mtime()
    if elf_mtime < newest_mtime:
        import datetime
        print("check_saftyfw_task_stack_budgets: FAIL: STALE ELF -- " + args.elf)
        print(f"  ELF mtime:    {datetime.datetime.fromtimestamp(elf_mtime)}")
        print(f"  newest source mtime: {datetime.datetime.fromtimestamp(newest_mtime)}  ({newest_path})")
        print("  This ELF predates a source file its own measurement depends on -- rebuild SaftyFW "
              "and re-run. A stale ELF has produced confident, wrong numbers before (2026-09-09: "
              "a Sep-8 ELF measured discrete_task at 184 B 'measured, fully resolved' with no "
              "seqlock code in it at all). Refusing to report a measurement rather than repeat that.")
        return 1

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

    elf_words_by_root = elf_side_stack_words_by_root(objdump, args.elf)

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

        # "Stop mixing sources": declared_bytes above came from the .c file's #define
        # text; cross-check it against what the ELF's own xTaskCreate() call site
        # actually passed as usStackDepth, derived independently from the binary
        # (see elf_side_stack_words_by_root's docstring). Refuse rather than print a
        # declared value that does not exist in the binary being measured.
        elf_words = elf_words_by_root.get(root_addr)
        if elf_words is None:
            errors.append(f"{tname}: could not locate/decode this task's xTaskCreate() call site in "
                           "the ELF disassembly to cross-check declared_bytes -- refusing to report "
                           "a declared value with nothing on the binary side to reconcile it against")
            continue
        if elf_words != words:
            errors.append(f"{tname}: declared stack size MISMATCH -- source macro says {words} words "
                           f"({words * 4} B) but the ELF's own xTaskCreate() call site passes "
                           f"{elf_words} words ({elf_words * 4} B). declared_bytes would be reporting "
                           "a number that does not exist in the binary being measured; refusing.")
            continue

        declared_bytes = words * 4  # RP2040 FreeRTOS SMP port: xTaskCreate takes DEPTH IN WORDS

        own_total, path_addrs = lib.deepest(root_addr, parsed)
        indeterminate = lib.has_unresolved_dispatch(root_addr, parsed)
        unresolved_regsp = lib.has_unresolved_regsp(root_addr, parsed)
        measured_total = own_total + ISR_STACKING_BYTES

        ceiling = forced_ceilings.get(tname, CEILING_BYTES.get(tname))
        if ceiling is None:
            errors.append(f"{tname}: no CEILING_BYTES entry -- every table row must be graded")
            continue

        # 2026-09-10 (opus review, round 2): grading REGSP_MARGIN_FACTOR against
        # `ceiling` was false by construction -- every ceiling in CEILING_BYTES
        # was itself set to exactly 2x its own measurement (e.g. link_task:
        # 9472, measured 4736), so `measured_total * 2 > ceiling` reduces to
        # `2*m > 2*m`, which never fires. It graded the table against itself,
        # not against anything the hardware cares about. What overflows the
        # processor is DECLARED stack, not a hand-maintained ceiling -- so this
        # margin must be checked against declared_bytes: if an unresolved regsp
        # frame could plausibly be as large, relative to what this walk already
        # measured, as the resolved ones turned out to be (4-6x, see
        # config_store_seqlock_read above), does that inflated total still fit
        # in the stack FreeRTOS actually allocated? A task whose own_total is a
        # large fraction of declared_bytes with an unresolved regsp adjust on
        # its path is not safely "ok" merely because a ceiling was copied from
        # the same measurement.
        regsp_margin_fail_val = regsp_margin_fail(measured_total, declared_bytes, unresolved_regsp)

        over_ceiling = measured_total > ceiling
        over_declared = measured_total > declared_bytes
        results.append(dict(name=tname, declared=declared_bytes, measured=measured_total,
                             raw=own_total, ceiling=ceiling, indeterminate=indeterminate,
                             unresolved_regsp=unresolved_regsp, regsp_margin_fail=regsp_margin_fail_val,
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
        if r["unresolved_regsp"]:
            tag = "INDETERMINATE (unresolved regsp -- may be WRONG, not just incomplete)"
        elif r["indeterminate"]:
            tag = "INDETERMINATE (lower bound)"
        else:
            tag = "measured"
        margin = r["declared"] - r["measured"]
        status = ("FAIL(ceiling)" if r["over_ceiling"]
                  else "FAIL(>declared)" if r["over_declared"]
                  else f"FAIL(regsp-margin<{REGSP_MARGIN_FACTOR}x)" if r["regsp_margin_fail"]
                  else "ok")
        if r["over_ceiling"] or r["over_declared"] or r["regsp_margin_fail"]:
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
