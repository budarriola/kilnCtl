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

# sdkconfig RESOLUTION IS NOT OURS -- it is check_all_task_stack_budgets.py's,
# imported rather than reimplemented (2026-09-16).
#
# This file used to hardcode `<repo root>/firmware/KilnFW/sdkconfig`
# unconditionally. `sdkconfig` is gitignored, so that path exists only in a
# tree where somebody has run a build: in EVERY clean worktree this check
# failed with "could not read CONFIG_ESP_MAIN_TASK_STACK_SIZE from sdkconfig"
# -- an environment gap wearing a budget-violation's clothes, repeatedly
# misread as a regression in whatever commit was under review. Worse, in the
# shared main tree it read whichever config that tree happens to hold right
# now, which is not necessarily the config that produced --elf.
#
# check_all_task_stack_budgets.py solved exactly this: ordered, terminal
# resolution relative to --elf (--sdkconfig, then a config published in the
# ELF's own directory, then the ELF's build-directory parent), no silent
# repo-root fallback, archived ELFs refused rather than graded against the
# live config, and a usability gate (key count + CONFIG_IDF_TARGET) so an
# empty or truncated file FAILs instead of reading as "every option off".
# Two checkers resolving the same file by two different rules is how this bug
# class returns, so the logic is SHARED, not copied: the sibling is a plain
# module in this same directory with no import-time side effects, and the two
# other stack-budget checkers already import this one the same way.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_all_task_stack_budgets as stack_budget_common  # noqa: E402

# The measured path is an under-estimate (see LIMITS above) and says nothing
# about interrupt frames pushed onto whatever task happens to be running, so
# the budget is a fraction of the configured stack, not the whole thing.
HEADROOM_FRACTION = 0.75

FN_RE = re.compile(r"^([0-9a-f]{8}) <(.+)>:")
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

# Address of the instruction a disassembly line describes, e.g.
# "42084dfe:\tb08765        \tcall8 ...". Needed to tell a line that is really
# part of the current function from a line objdump merely PRINTED under that
# function's caption -- see symbol_sizes() below. Same fix as
# stack_budget_lib.py's (2026-09-19, 924eeea2); that pass fixed the NEW
# address-keyed walker (stack_budget_lib.py, used by
# check_all_task_stack_budgets.py) but left THIS module's own parse() --
# which is the one check_executor_task_stack_budget.py,
# check_httpd_task_stack_budget.py, check_system_uart_bridge_stack_budget.py
# and check_uart_log_bridge_stack_budget.py all actually import and call as
# `base.parse` -- untouched, on the stated basis that those four pre-existing
# callers "do not have to be touched or re-verified". That left this exact
# class of phantom edge live in the code four other checks actually run.
# Confirmed 2026-09-20: `profile_executor_guard_zone_ramp_rate` (nm -S size
# 0x42, a 3-line leaf float function) has no `.size`-covered call of its own,
# but objdump keeps captioning the ~4 KB of literal-pool/padding bytes after
# it (up to the next real symbol, `profile_executor_on_off_log_transition`)
# under its name and decodes a `call8` into `lfs_rename` out of that data,
# grafting the whole LittleFS rename/compact chain onto profile_executor's
# graph and failing check_executor_task_stack_budget.py on a path the
# firmware never executes.
LINE_ADDR_RE = re.compile(r"^([0-9a-f]+):\t")


def symbol_sizes(objdump, elf):
    """{addr:int -> size_bytes:int} for every sized function symbol, read from
    the ELF symbol table (`objdump -t`). See stack_budget_lib.py's
    symbol_sizes() for the full rationale -- this is the same fix, applied
    here because this module's parse()/deepest() are a separate, older
    implementation that fix did not reach."""
    try:
        result = subprocess.run([objdump, "-t", elf], capture_output=True, text=True)
    except OSError as exc:
        raise ElfParseError(f"could not run {objdump} -t on {elf}: {exc}") from exc
    if result.returncode != 0:
        raise ElfParseError(
            f"{objdump} -t {elf} exited {result.returncode} -- could not read the symbol table "
            f"(needed to bound each function to its real extent). objdump stderr:\n"
            f"{result.stderr.strip()}"
        )
    sizes = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 5 or "F" not in parts[1:-3]:
            continue
        try:
            addr = int(parts[0], 16)
            size = int(parts[-2], 16)
        except ValueError:
            continue
        if size <= 0:
            continue
        if size > sizes.get(addr, 0):
            sizes[addr] = size
    return sizes


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


def main_stack_bytes_for_elf(elf, explicit_sdkconfig=None):
    """CONFIG_ESP_MAIN_TASK_STACK_SIZE out of the sdkconfig belonging to `elf`.

    Returns (stack_bytes, path, origin). Raises ValueError -- naming the file
    it tried and why -- if nothing resolves, if the resolved file is not a
    usable sdkconfig (empty/truncated/not an sdkconfig), or if the key is
    absent. There is deliberately NO assumed default: silently assuming a
    stack size is how a gitignored config produces a green check that proved
    nothing, and this budget is a fraction of that number, so a wrong value
    silently moves the bar this check exists to hold.
    """
    path, origin = stack_budget_common.use_sdkconfig_for_elf(elf, explicit_sdkconfig)
    # _sdkconfig_value() applies the sibling's USABLE gate and raises with
    # SDKCONFIG_ORIGIN's explanation when resolution itself failed.
    return stack_budget_common._sdkconfig_value("CONFIG_ESP_MAIN_TASK_STACK_SIZE"), path, origin


class ElfParseError(RuntimeError):
    """Raised when objdump cannot disassemble the given ELF at all -- an
    empty (0-byte, e.g. mid-write from a concurrent build) or otherwise
    corrupt/truncated file. Callers must catch this and report a clear
    SKIP/FAIL, never let it propagate as a raw CalledProcessError
    traceback -- that reads as a crash, not a verdict.

    `skip` is True only for the 0-byte placeholder case (a build plausibly
    still in progress) -- callers branch on this attribute, NOT on a
    substring of the message (a message that legitimately mentions a size
    like "(200 bytes)" contains the literal substring "0 bytes" too, which
    is exactly the false-SKIP bug a substring check produced here on
    2026-09-15). Any other parse failure is a genuine FAIL, never a SKIP."""

    def __init__(self, message, skip=False):
        super().__init__(message)
        self.skip = skip


def parse(objdump, elf):
    try:
        size = os.path.getsize(elf)
    except OSError as exc:
        raise ElfParseError(f"could not stat {elf}: {exc}") from exc
    if size == 0:
        raise ElfParseError(
            f"{elf} is 0 bytes -- looks like a build in progress wrote a placeholder/truncated "
            "file (e.g. a concurrent `idf.py build`/flash_firmware() still writing it), not a "
            "genuinely broken build. Re-run once the build that owns this ELF has finished.",
            skip=True,
        )
    try:
        result = subprocess.run([objdump, "-d", elf], capture_output=True, text=True)
    except OSError as exc:
        raise ElfParseError(f"could not run {objdump} on {elf}: {exc}") from exc
    if result.returncode != 0:
        raise ElfParseError(
            f"{objdump} -d {elf} exited {result.returncode} -- the ELF is present ({size} bytes) "
            f"but objdump could not disassemble it (corrupt/truncated/wrong format). objdump "
            f"stderr:\n{result.stderr.strip()}"
        )
    out = result.stdout
    sizes = symbol_sizes(objdump, elf)
    frames, calls, seen_entry, cur = {}, {}, set(), None
    lct = stack_budget_common.lib.LongCallTracker()
    cur_end = None  # first address PAST the current function per the ELF
                    # symbol table; None means "size unknown, unbounded".
    for line in out.splitlines():
        m = FN_RE.match(line)
        if m:
            addr = int(m.group(1), 16)
            cur = m.group(2)
            size = sizes.get(addr)
            cur_end = (addr + size) if size else None
            frames.setdefault(cur, 0)
            calls.setdefault(cur, set())
            lct.reset()
            continue
        if cur is None:
            continue
        if cur_end is not None:
            la = LINE_ADDR_RE.match(line)
            if la and int(la.group(1), 16) >= cur_end:
                # Past this function's real end: objdump is captioning
                # inter-function data (literal pool / jump table / padding /
                # blob) with this function's name and decoding it as
                # instructions. Those bytes belong to no function, so credit
                # them to none -- see symbol_sizes()'s docstring.
                cur = None
                cur_end = None
                continue
        if cur not in seen_entry:
            e = ENTRY_RE.search(line)
            if e:
                frames[cur] = int(e.group(1), 0)
                seen_entry.add(cur)
        c = CALL_RE.search(line)
        if c:
            calls[cur].add(c.group(1))
        lc = lct.feed(line)
        if lc is not None:
            # Resolved l32r+callx long call(s): union of every literal. A
            # "sym+0xNN" caption means the target lies INSIDE sym, so credit
            # sym itself (never skip it: that added 0 B silently).
            for _addr, tname in lc[0]:
                calls[cur].add(tname.split("+", 1)[0])
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
    ap.add_argument("--sdkconfig", default=None,
                    help="the sdkconfig that produced --elf. Normally inferred from --elf "
                         "(see main_stack_bytes_for_elf, which shares check_all_task_stack_"
                         "budgets.py's ordered resolution); REQUIRED for an ELF in elf_archive/, "
                         "which is deliberately never resolved against the live config.")
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

    if args.stack_bytes:
        stack = args.stack_bytes
        print(f"check_main_task_stack_budget: stack size: {stack} B supplied with "
              "--stack-bytes (sdkconfig not consulted)")
    else:
        try:
            stack, sdk_path, sdk_origin = main_stack_bytes_for_elf(args.elf, args.sdkconfig)
        except ValueError as exc:
            print(f"check_main_task_stack_budget: FAIL -- {exc}")
            return 1
        print(f"check_main_task_stack_budget: sdkconfig: {sdk_path} ({sdk_origin})")
    if not stack:
        print("check_main_task_stack_budget: FAIL -- the resolved sdkconfig yielded no usable "
              "CONFIG_ESP_MAIN_TASK_STACK_SIZE")
        return 1
    budget = int(stack * HEADROOM_FRACTION)

    try:
        frames, calls = parse(objdump, args.elf)
    except ElfParseError as exc:
        if exc.skip:
            print(f"check_main_task_stack_budget: SKIP: {exc}")
            return 3
        print(f"check_main_task_stack_budget: FAIL -- {exc}")
        return 1
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
