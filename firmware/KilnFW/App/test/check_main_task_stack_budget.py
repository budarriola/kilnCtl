#!/usr/bin/env python3
"""Worst-case `main`-task stack usage, measured out of the built ELF.

WHY THIS EXISTS
---------------
2026-09-08: flashing `218f65f7` panicked the board at boot -- `exc_task='main'`,
`exc_cause_str='IllegalInstruction'`, `exc_pc=0xfffffffd`, backtrace corrupted.
Nothing in the crash record named a line, because there was no intact stack left
to walk: the `main` task had overflowed its CONFIG_ESP_MAIN_TASK_STACK_SIZE
(8192 B) stack and executed a smashed return address.

The overflow was not one big allocation, it was ordinary-looking locals in three
different modules that only became reachable together once the `cfg` LittleFS
partition mounted for the first time:

    app_main                        32 B
    main_network_http_bringup       80 B
    kiln_cfg_store_init            144 B
    nvs_load_store_with_cfg_fs    7552 B   <- kiln_cfg_store_blob_t on the stack
    kiln_cfg_store_cfg_fs_resolve   96 B
    kiln_cfg_store_cfg_fs_load_raw7536 B   <- a second one
    cfg_fs_read                    640 B
                                 ------
                                 16112 B on an 8192 B stack

No compiler warning, no host test and no review caught it: every one of those
frames is legal C, each module looked reasonable on its own, and the host test
build runs on a PC stack of megabytes. The only thing that can see it is the
whole call tree at once, which is exactly what this script computes.

WHAT IT DOES
------------
Disassembles the ELF, reads each function's frame size from its `entry a1, N`
prologue, builds the static call graph (direct `call4/call8/call12` edges only),
and reports the deepest byte-cost path reachable from `app_main`. Fails if that
exceeds the budget.

LIMITS, stated honestly:
  * Indirect calls (`callx8` through a function pointer) are NOT followed --
    the callee is not knowable statically. A path that goes through a callback
    can therefore be under-counted.
  * Recursion is cut at the first repeat rather than unrolled.
  * ISR/window-overflow spill is not modelled; that is what the headroom
    fraction below is for.
Both limits make this an UNDER-estimate, never an over-estimate: anything it
reports as too deep genuinely is too deep.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
DEFAULT_ELF = os.path.join(REPO_ROOT, "firmware", "KilnFW", "build", "KilnCtrl.elf")
DEFAULT_SDKCONFIG = os.path.join(REPO_ROOT, "firmware", "KilnFW", "sdkconfig")

# The measured path is an under-estimate (see LIMITS above) and says nothing
# about interrupt frames pushed onto whatever task happens to be running, so
# the budget is a fraction of the configured stack, not the whole thing.
HEADROOM_FRACTION = 0.75

FN_RE = re.compile(r"^[0-9a-f]{8} <(.+)>:")
ENTRY_RE = re.compile(r"\bentry\ta1, (0x[0-9a-f]+|\d+)")
# Deliberately requires the target to land EXACTLY on a symbol (no
# "+0xNNNN") -- 2026-09-08 fix, see SECTION_MARKER_NAMES' comment below for
# the full story. Xtensa's windowed-register ABI means a `call4/8/12` can
# only legally target a function's own `entry` prologue; it can never jump
# into the middle of another function's body the way `j`/`callx` might. So
# whenever objdump captions a call target as "<name+0xNNNN>", that is proof
# the REAL callee has no symbol of its own and objdump fell back to the
# nearest earlier one in the whole symbol table, however far away -- not
# that this call actually reaches `name`. The previous version of this
# regex matched that case anyway (silently discarding the offset), which is
# what produced fabricated call-graph edges like `backup_export_get_handler`
# -> `_stext` -> `profiles_handle_message` -> ... 6176 B, a path
# backup_export.c never calls (grep confirms no reference). Dropping
# offset-qualified matches entirely is a strict subset of before -- it can
# only make this checker's reported paths MORE accurate (an under-estimate,
# same direction as this file's other documented LIMITS), never hide a call
# that previously wasn't there.
CALL_RE = re.compile(r"\bcall(?:4|8|12)\t[0-9a-f]+ <([^>+]+)>")


def find_objdump():
    env = os.environ.get("XTENSA_OBJDUMP")
    if env and os.path.isfile(env):
        return env
    on_path = shutil.which("xtensa-esp32s3-elf-objdump")
    if on_path:
        return on_path
    pattern = os.path.join(os.path.expanduser("~"), ".espressif", "tools", "xtensa-esp-elf", "*",
                           "xtensa-esp-elf", "bin", "xtensa-esp32s3-elf-objdump*")
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else None


def stack_size_from_sdkconfig(path):
    if not os.path.isfile(path):
        return None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("CONFIG_ESP_MAIN_TASK_STACK_SIZE="):
            return int(line.split("=", 1)[1].strip())
    return None


def parse(objdump, elf):
    out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
    frames, calls, seen_entry, cur = {}, {}, set(), None
    for line in out.splitlines():
        m = FN_RE.match(line)
        if m:
            cur = m.group(1)
            frames.setdefault(cur, 0)
            calls.setdefault(cur, set())
            continue
        if cur is None:
            continue
        if cur not in seen_entry:
            e = ENTRY_RE.search(line)
            if e:
                frames[cur] = int(e.group(1), 0)
                seen_entry.add(cur)
        c = CALL_RE.search(line)
        if c:
            calls[cur].add(c.group(1))
    return frames, calls


# Linker section-boundary symbols (_stext and friends). objdump has no
# closer symbol to caption a call target that lands in an unlabeled stretch
# of the image -- a literal pool, a jump/veneer table, or genuinely
# code whose own local symbol didn't make it into this ELF's symbol table
# -- so it captions the target against whichever real symbol precedes it,
# however far away that is. `_stext` sits at the very start of .text, so
# EVERY such orphaned target in the low addresses gets captioned
# "<_stext+0xNNNN>" regardless of which of the many unrelated functions
# actually live there. Treating "_stext" as one callable node then unions
# together the call sets of all of them, producing entirely fabricated
# transitive reachability: confirmed 2026-09-08 on
# `backup_export_get_handler` -> `_stext` -> `profiles_handle_message` ->
# `profile_executor_halt` -> ... -> 6176 B, a path backup_export.c does not
# call into (grep confirms no reference), inflating
# check_httpd_task_stack_budget.py's reported worst case past its real one
# (revert_post_handler, 4832 B). Skip these names as callees, exactly like
# an unresolvable indirect call (see LIMITS above) -- the path stops here
# rather than continuing through a fabricated edge.
SECTION_MARKER_NAMES = frozenset({
    "_stext", "_etext", "_sinittext", "_einittext", "_srodata", "_erodata",
})


def deepest(root, frames, calls):
    memo = {}

    def walk(fn, on_stack):
        if fn in on_stack:
            return 0, []
        if fn in memo:
            return memo[fn]
        best = (0, [])
        for callee in sorted(calls.get(fn, ())):
            if callee not in frames or callee in SECTION_MARKER_NAMES:
                continue
            d, p = walk(callee, on_stack | {fn})
            if d > best[0]:
                best = (d, [callee] + p)
        result = (frames.get(fn, 0) + best[0], best[1])
        memo[fn] = result
        return result

    return walk(root, set())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--root", default="app_main")
    ap.add_argument("--stack-bytes", type=int, default=None,
                    help="override CONFIG_ESP_MAIN_TASK_STACK_SIZE (used by the negative test)")
    args = ap.parse_args()

    if not os.path.isfile(args.elf):
        print(f"check_main_task_stack_budget: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build) and re-run; this check cannot "
              "measure anything without one. This is a SKIP, not a pass -- it means the "
              "8704 B/6144 B-class overflow this check exists to catch (see the module "
              "docstring) goes UNMEASURED on this run, not that it was checked and found fine.")
        return 3

    objdump = find_objdump()
    if not objdump:
        print("check_main_task_stack_budget: SKIP: xtensa-esp32s3-elf-objdump not found "
              "(set XTENSA_OBJDUMP). Unmeasured, not passing.")
        return 3

    stack = args.stack_bytes or stack_size_from_sdkconfig(DEFAULT_SDKCONFIG)
    if not stack:
        print("check_main_task_stack_budget: FAIL -- could not read "
              "CONFIG_ESP_MAIN_TASK_STACK_SIZE from sdkconfig")
        return 1
    budget = int(stack * HEADROOM_FRACTION)

    frames, calls = parse(objdump, args.elf)
    if args.root not in frames:
        print(f"check_main_task_stack_budget: FAIL -- {args.root} not found in {args.elf}")
        return 1

    total, path = deepest(args.root, frames, calls)
    print(f"deepest static stack path from {args.root}: {total} B "
          f"(budget {budget} B = {int(HEADROOM_FRACTION * 100)}% of a {stack} B stack)")
    running = 0
    for fn in [args.root] + path:
        running += frames.get(fn, 0)
        print(f"    {frames.get(fn, 0):>6} B  {running:>6} B cumulative  {fn}")

    if total > budget:
        print()
        print(f"check_main_task_stack_budget: FAIL -- {total} B exceeds the {budget} B budget.")
        print("  The `main` task can overflow its stack on this path. That is not a crash with a "
              "line number: it smashes the return address and the board takes an "
              "IllegalInstruction panic at boot with a corrupted backtrace "
              "(docs/audits/boot_hang_2026-09-08.md).")
        print("  Fix by moving the large locals named above onto the heap, not by enlarging the "
              "stack -- the frames listed are cumulative and will keep growing.")
        return 1

    print("check_main_task_stack_budget: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
