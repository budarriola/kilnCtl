#!/usr/bin/env python3
"""Shared ELF-call-graph-walk machinery for the check_*_task_stack_budget.py family.

This is the address-keyed successor to the name-keyed graph in
check_main_task_stack_budget.py (kept as-is so the five pre-existing
callers -- check_main_task_stack_budget.py itself,
check_httpd_task_stack_budget.py, check_executor_task_stack_budget.py,
check_system_uart_bridge_stack_budget.py, check_uart_log_bridge_stack_budget.py
-- do not have to be touched or re-verified as part of this pass). New
tasks are driven exclusively through THIS module and
check_all_task_stack_budgets.py, added 2026-09-09 to close the coverage gap
documented in docs/audits/ (25 stack_margin_register()-registered tasks with
no budget check at all, two of them -- safety_poll and lvgl -- directly
implicated in the 2026-09-04 panic: an 8192 B `s_lvgl_task_stack` in
lvgl_port.c sitting close behind thermo_owner.c's `s_slots[]` in .bss).

WHY ADDRESS-KEYED, NOT NAME-KEYED
----------------------------------
The pre-existing five checkers each walk from a single, uniquely-named root
(app_main, httpd handler entries, executor_task_entry, system_bridge_task,
uart_log_bridge_task) and key their frame/call-graph dicts by function NAME
straight out of objdump's disassembly captions. That is unsound in general:
C `static` functions are only unique within their own translation unit, and
this codebase has at least one real collision among the newly-covered
tasks -- `owner_task` is the (deliberately identical, copy-once-adapted)
task entry point in BOTH kiln_io_owner.c and thermo_owner.c:

    $ xtensa-esp32s3-elf-objdump -t KilnCtrl.elf | grep -w owner_task
    42048ff8 l F .flash.text  owner_task   (wifi_prov.c -- unrelated, unregistered)
    4201b804 l F .flash.text  owner_task   (kiln_io_owner.c)
    4201bfc4 l F .flash.text  owner_task   (thermo_owner.c)

A name-keyed frames dict (`frames["owner_task"] = size`) can only hold ONE
of these three -- the last one parsed silently overwrites the others, and
BOTH kiln_io_owner's and thermo_owner's checks would silently measure
whichever one happened to load last out of objdump's output, an entirely
address-independent accident of link order. That is exactly the "two
pieces of state joined by a semantic contract, expressed nowhere as a
single owning type" bug class this repo has been bitten by four times
already (see CLAUDE.md's "reset one side of a pair" section) -- here the
contract is "this task's measured frame size must be THIS function's own
frame size", silently broken by name reuse instead of a reset.

Fix: key everything by ADDRESS, the one thing objdump's disassembly is
never ambiguous about. `call4/8/12 <target>` lines carry the callee's
address directly (right before the `<name>` caption) -- using that address
as the graph key means the call graph is correct even when two functions
share a name, AND it makes the old SECTION_MARKER_NAMES workaround
(check_main_task_stack_budget.py's fabricated-`_stext`-edge fix) fall out
for free: an offset-qualified objdump caption like `<_stext+0x1234>` names
an address that is NOT any real function's entry point, so it simply never
appears as a key in `frames` and the walk stops there, same effect as the
name-based exclusion list, with no exclusion list required.

Root-symbol resolution for an AMBIGUOUS name (i.e. `owner_task`) uses
`addr2line -f` against each candidate address and matches the reported
source path against a caller-supplied substring (e.g. "kiln_io_owner.c").
See resolve_root().
"""

import glob
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
DEFAULT_ELF = os.path.join(REPO_ROOT, "firmware", "KilnFW", "build", "KilnCtrl.elf")
DEFAULT_SDKCONFIG = os.path.join(REPO_ROOT, "firmware", "KilnFW", "sdkconfig")

FN_RE = re.compile(r"^([0-9a-f]{8}) <(.+)>:")
ENTRY_RE = re.compile(r"\bentry\ta1, (0x[0-9a-f]+|\d+)")
# Capture the callee's ADDRESS (the hex just before "<name>"), not just its
# name -- see the module docstring's "WHY ADDRESS-KEYED" section. Still
# requires the objdump caption to land exactly on a symbol (no "+0xNNNN"),
# same reasoning check_main_task_stack_budget.py's CALL_RE comment gives:
# Xtensa's windowed-register call4/8/12 can only legally target a real
# `entry` prologue, so an offset-qualified caption proves the true callee
# has no symbol of its own -- but with address keys this filtering happens
# automatically (the offset address is simply absent from `frames`), so this
# regex only needs to capture what objdump actually printed.
CALL_RE = re.compile(r"\bcall(?:4|8|12)\t([0-9a-f]+)(?: <([^>]+)>)?")
# Xtensa's INDIRECT call form -- callx4/8/12 <reg> -- targets whatever address
# is currently in the named register (a function pointer: an LVGL flush/timer
# callback, a queued-worker dispatch, a registered I2C owner callback, ...).
# There is no static target here at all, so deepest()'s walk cannot follow it
# no matter how the frames/calls graph is built. A function that contains one
# of these is a genuine dead end for this analysis, not a gap that better
# parsing could close -- see has_unresolved_dispatch() below, added
# 2026-09-09 after an opus review found check_all_task_stack_budgets.py
# reporting confident "measured and within budget" passes for tasks (lvgl:
# 752 B against an 8192 B stack; bx_flash_worker, recovery_exit,
# backlight_pwm, i2c_owner_*) whose real depth lives almost entirely behind
# exactly this kind of call.
CALLX_RE = re.compile(r"\bcallx(?:0|4|8|12)\b")


# ---------------------------------------------------------------------------
# LONG CALLS (2026-10-09). A `call8 <sym>` reaches only +-512 KB, so every
# flash call across that distance, and every call into IRAM/ROM, is emitted as
#     l32r   a8, <literal>  (<target addr> <symbol>)
#     callx8 a8
# The walk used to follow only call4/8/12, silently dropping all of these
# (control_handle_message measured 944 B while really reaching
# zones_config_set_pid and everything below it). LongCallTracker pairs the two
# instructions: it keeps the UNION of literals each register was loaded with since the
# last callx and flags the site tainted if any other instruction wrote the
# register (2026-10-09 review: a last-literal-wins tracker dropped edges). A register left stale
# across a branch target can only ADD an edge, never hide one (the safe
# direction for this checker).
# ---------------------------------------------------------------------------
L32R_RE = re.compile(r"\tl32r\t(a\d+),\s*[0-9a-f]+(?: <[^>]*>)? \(([0-9a-f]+) <([^>]+)>\)")
CALLX_REG_RE = re.compile(r"\tcallx(?:0|4|8|12)\t(a\d+)")
_INSN_RE = re.compile(r"^[0-9a-f]+:\t[0-9a-f ]+\t(\S+)\s+(a\d+)\b")
_NON_WRITING = ("s32", "s16", "s8", "ssi", "ssx", "b", "j", "call", "ret", "nop", "memw",
                "isync", "dsync", "esync", "rsync", "wsr", "wur", "ill")
# Opcodes that start with a _NON_WRITING prefix yet write their AR operand
# (review F6): xsr* exchanges, s32c1i stores the old memory value back.
_WRITING_EXCEPTIONS = ("xsr", "s32c1i")


class LongCallTracker:
    """Feed every disassembly line of one function in order via feed(); it
    returns None, or (targets, tainted) when a callx4/8/12 uses a register
    that an l32r loaded since the last callx. `targets` is the UNION of every
    literal l32r put in the register (a branch can pick either), `tainted` is
    True when a non-l32r instruction also wrote the register, so some path
    to the call may hold an unknown value: the caller must add the edges AND
    still mark the function indirect. Call reset() at each function
    boundary."""

    def __init__(self):
        self.regs = {}   # reg -> [set of (addr, name), tainted]

    def reset(self):
        self.regs = {}

    def feed(self, line):
        m = L32R_RE.search(line)
        if m:
            ent = self.regs.setdefault(m.group(1), [set(), False])
            ent[0].add((int(m.group(2), 16), m.group(3)))
            return None
        m = CALLX_REG_RE.search(line)
        if m:
            ent = self.regs.pop(m.group(1), None)
            if ent is None:
                return None
            return sorted(ent[0]), ent[1]
        m = _INSN_RE.match(line)
        if m and self.regs and (m.group(1).startswith(_WRITING_EXCEPTIONS)
                                or not m.group(1).startswith(_NON_WRITING)):
            ent = self.regs.get(m.group(2))
            if ent is not None:
                ent[1] = True
        return None


# Address of the instruction a disassembly line describes, e.g.
# "42084dfe:\tb08765        \tcall8 ...". Needed to tell a line that is
# really part of the current function from a line objdump merely PRINTED
# under that function's caption -- see symbol_sizes() below.
LINE_ADDR_RE = re.compile(r"^([0-9a-f]+):\t")


def symbol_sizes(objdump, elf):
    """{addr:int -> size_bytes:int} for every sized function symbol, read
    from the ELF symbol table (`objdump -t`).

    WHY THIS EXISTS (2026-09-19). `objdump -d` is a LINEAR SWEEP: it prints
    every byte of an executable section and captions those bytes with the
    nearest preceding symbol. Bytes that belong to no function at all --
    literal pools, jump tables, alignment padding, and the constant blobs the
    toolchain parks between functions in `.flash.text` -- are therefore
    printed under the previous function's name, and objdump decodes them as
    if they were instructions. Xtensa instructions are unaligned and
    variable-length, so arbitrary data routinely decodes into a plausible
    `call8 <some real symbol>` line.

    The analyser's linear parse credited those fabricated lines to the
    captioned function, inventing call-graph edges out of nothing. A real
    instance: `calc_content_width` (LVGL layout; ELF symbol size 0x1e1, so
    it genuinely ends at 0x42084625) picked up a `call8
    cfg_fs_confirm_format_job_run` decoded from data at 0x42084dfe, ~2 KB
    past its own end and visibly surrounded by objdump `.byte` lines. That
    one phantom edge grafted the whole LittleFS mount path (`cfg_fs_init`
    -> `sweep_tmp$constprop$0`, 1232 B of frame by itself) onto the lvgl
    task and reported lvgl at 5888 B against a 4880 B ceiling. No firmware
    behaviour had changed; a code-layout shift had merely moved which data
    landed behind which function.

    The symbol table is the authority on where a function ends, so bound
    each function's disassembly to [addr, addr + size) and ignore anything
    outside it. A size-0 symbol (hand-written assembly with no `.size`
    directive) stays unbounded exactly as before -- missing information is
    not a reason to start dropping real edges."""
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
        # "42084444 l     F .flash.text\t000001e1 calc_content_width"
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
        # Aliases can share an address; the larger span is the safe one --
        # it can only ever admit MORE of the real function, never less.
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


def find_addr2line():
    env = os.environ.get("XTENSA_ADDR2LINE")
    if env and os.path.isfile(env):
        return env
    on_path = shutil.which("xtensa-esp32s3-elf-addr2line")
    if on_path:
        return on_path
    objdump = find_objdump()
    if objdump:
        cand = objdump.replace("objdump", "addr2line")
        if os.path.isfile(cand):
            return cand
    pattern = os.path.join(os.path.expanduser("~"), ".espressif", "tools", "xtensa-esp-elf", "*",
                            "xtensa-esp-elf", "bin", "xtensa-esp32s3-elf-addr2line*")
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else None


class ParsedElf:
    """Address-keyed frame sizes + call graph, plus a name->[addresses] index
    for root resolution."""

    def __init__(self, frames, calls, names, name_addrs, indirect, bodyless=None):
        self.bodyless = bodyless or {}  # {addr -> {name}}: resolved ROM long-call targets with no body (F3)
        self.frames = frames          # {addr:int -> frame_bytes:int}
        self.calls = calls            # {addr:int -> set(addr:int)}
        self.names = names            # {addr:int -> name:str}  (display only)
        self.name_addrs = name_addrs  # {name:str -> [addr:int, ...]}
        self.indirect = indirect      # {addr:int -> bool}  True iff this function's own
                                       # disassembly contains a callx4/8/12 (indirect call
                                       # through a register -- a function pointer this walk
                                       # cannot resolve a target address for at all)


# Review F2 (2026-10-09): edges that exist in the object code but run ONLY
# under the bx_flash_worker root. check_all_task_stack_budgets.py's
# DECLARED_EDGES is the inverse (adds them back for that one row).
WORKER_ONLY_EDGES = {"nvs_save": ["zones_autosave_job"]}

# ESP32-S3 mask ROM. A resolved long call here has a symbol but no body in
# the disassembly, so it contributes 0 B (review F3).
ROM_ADDR_RANGE = (0x40000000, 0x40060000)


def drop_worker_only_edges(parsed, resolve):
    """A copy of `parsed` without WORKER_ONLY_EDGES. `resolve(name)` -> addr;
    a name that does not resolve is skipped (the edge cannot exist then)."""
    import copy
    view = copy.copy(parsed)
    view.calls = {k: set(v) for k, v in parsed.calls.items()}
    for caller, callees in WORKER_ONLY_EDGES.items():
        try:
            ca = resolve(caller)
        except ValueError:
            continue
        for callee in callees:
            try:
                view.calls.get(ca, set()).discard(resolve(callee))
            except ValueError:
                pass
    return view


def bodyless_calls(root_addr, parsed):
    """Sorted names of resolved ROM long-call targets reachable from root_addr
    whose frame is unknown (counted as 0 B by deepest()). A warning list, not
    a verdict (review F3)."""
    out = set()
    for a in reachable_addrs(root_addr, parsed):
        out.update(parsed.bodyless.get(a, ()))
    return sorted(out)


class ElfParseError(RuntimeError):
    """Raised when objdump cannot disassemble the given ELF at all -- an
    empty (0-byte, e.g. mid-write from a concurrent build) or otherwise
    corrupt/truncated file. Callers must catch this and report a clear
    SKIP/FAIL (see each check's main()), never let it propagate as a raw
    CalledProcessError traceback -- that reads as a crash, not a verdict,
    and this repo has been burned before by a check that looked broken
    instead of naming what was actually wrong (docs/audits/
    check_independence_2026-09-07.md).

    `skip` is True only for the 0-byte placeholder case (a build plausibly
    still in progress) -- callers branch on this attribute, NOT on a
    substring of the message (a message that legitimately mentions a size
    like "(200 bytes)" contains the literal substring "0 bytes" too, which
    is exactly the false-SKIP bug a substring check produced here on
    2026-09-15). Any other parse failure (objdump itself errors on a
    non-empty file) is a genuine FAIL, never a SKIP."""

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
    frames, calls, names, name_addrs, indirect = {}, {}, {}, {}, {}
    pending_lc = {}   # cur -> {(addr, name)}: long-call literals, filtered after the pass
    seen_entry = set()
    lct = LongCallTracker()
    cur = None
    cur_end = None   # first address PAST the current function per the ELF
                     # symbol table; None means "size unknown, unbounded".
    for line in out.splitlines():
        m = FN_RE.match(line)
        if m:
            cur = int(m.group(1), 16)
            name = m.group(2)
            size = sizes.get(cur)
            cur_end = (cur + size) if size else None
            frames.setdefault(cur, 0)
            calls.setdefault(cur, set())
            indirect.setdefault(cur, False)
            lct.reset()
            names[cur] = name
            name_addrs.setdefault(name, []).append(cur)
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
            calls[cur].add(int(c.group(1), 16))
        lc = lct.feed(line)
        if lc is not None:
            pending_lc.setdefault(cur, set()).update(lc[0])
            if lc[1]:
                indirect[cur] = True   # a non-l32r write also reached the call
        elif CALLX_RE.search(line):
            indirect[cur] = True
    # Review F8: a long-call literal is an edge only when it is a function
    # entry (a data/MMIO literal or a callback address merely loaded into the
    # register is not). F3: a ROM target has no body -> recorded, not silent.
    bodyless = {}
    for fn, lits in pending_lc.items():
        for addr, name in lits:
            if addr in frames:
                calls[fn].add(addr)
            elif ROM_ADDR_RANGE[0] <= addr < ROM_ADDR_RANGE[1]:
                bodyless.setdefault(fn, set()).add(name)
    return ParsedElf(frames, calls, names, name_addrs, indirect, bodyless)


def reachable_addrs(root_addr, parsed):
    """Every address deepest() can actually walk to from root_addr (i.e. only
    following resolved call4/8/12 edges whose target has a frame). Used to
    look for indirect dispatch anywhere in the graph this walk covers, not
    just on whichever single path happens to be deepest."""
    seen = set()
    stack = [root_addr]
    while stack:
        a = stack.pop()
        if a in seen:
            continue
        seen.add(a)
        for c in parsed.calls.get(a, ()):
            if c in parsed.frames and c not in seen:
                stack.append(c)
    return seen


def has_unresolved_dispatch(root_addr, parsed):
    """True if root_addr or anything in its resolvable call graph contains an
    indirect call (callx4/8/12). When true, deepest(root_addr, parsed) is a
    LOWER BOUND, not a measurement: the real worst-case path may continue
    through whatever function pointer that callx targets at runtime (an LVGL
    flush/timer callback, a queued-worker dispatch, a registered I2C
    callback, ...), and this walk has no way to know what that is or how deep
    it goes. Callers must not report a confident pass off of a total this
    flags -- see check_all_task_stack_budgets.py's INDETERMINATE handling."""
    return any(parsed.indirect.get(a, False) for a in reachable_addrs(root_addr, parsed))


def resolve_root(parsed, name, elf, addr2line=None, expect_path_substr=None):
    """Return the entry address for `name`, disambiguating collisions with
    addr2line + a caller-supplied source-path substring.

    Raises ValueError (never silently guesses) if:
      * `name` is not a symbol in the ELF at all,
      * `name` is ambiguous and no `expect_path_substr` was given,
      * `expect_path_substr` matches zero or more-than-one candidate.
    """
    addrs = parsed.name_addrs.get(name)
    if not addrs:
        raise ValueError(f"symbol {name!r} not found in {elf}")
    if len(addrs) == 1:
        return addrs[0]
    if not expect_path_substr:
        raise ValueError(
            f"symbol {name!r} is ambiguous ({len(addrs)} definitions in {elf}) and no "
            "expect_path_substr was given to disambiguate -- refusing to silently pick one "
            "(this is the owner_task-class collision; see stack_budget_lib.py's module docstring)")
    if not addr2line:
        raise ValueError(f"symbol {name!r} is ambiguous and no addr2line is available to disambiguate")
    matches = []
    for addr in addrs:
        out = subprocess.run([addr2line, "-e", elf, "-f", f"0x{addr:x}"],
                              capture_output=True, text=True, check=True).stdout.splitlines()
        # addr2line -f prints two lines: function name, then "file:line".
        src = out[1] if len(out) > 1 else ""
        if expect_path_substr.replace("\\", "/") in src.replace("\\", "/"):
            matches.append(addr)
    if len(matches) != 1:
        raise ValueError(
            f"symbol {name!r} is ambiguous ({len(addrs)} definitions); expect_path_substr "
            f"{expect_path_substr!r} matched {len(matches)} of them via addr2line, need exactly 1")
    return matches[0]


def deepest(root_addr, parsed):
    """Deepest cumulative-byte static call path from root_addr. Returns
    (total_bytes, [addr, ...] path after the root, NOT including root).

    MEMO CORRECTNESS (fixed 2026-09-09, opus review of 316967b7): a node hit
    while walking INSIDE a recursion cut (addr already in on_stack, so that
    branch returns (0, []) early) must NOT be cached as if it were that
    node's true deepest value -- the same node reached later via a DIFFERENT
    path, with a different on_stack, may not hit that cycle at all and could
    legitimately measure deeper. Caching the cut-short value under the first
    on_stack it happened to be visited with would then leak into every later
    reuse of that memo entry, silently under-reporting -- always in the
    unsafe direction (this checker's one stated invariant is that it may
    under- but never over-estimate). Fixed by tracking, per call, whether the
    subtree the call just computed contains a cut anywhere in it, and only
    writing an address's memo entry when its own computation was cut-free --
    a cut result is recomputed (safely, since real Xtensa call graphs here
    are small and shallow) every time it is reached, instead of being cached
    as if final."""
    memo = {}
    frames, calls = parsed.frames, parsed.calls

    def walk(addr, on_stack):
        if addr in on_stack:
            return 0, [], True  # cycle cut here; caller must not cache this branch's result
        if addr in memo:
            return memo[addr][0], memo[addr][1], False
        best = (0, [])
        any_cut = False
        for callee in sorted(calls.get(addr, ())):
            if callee not in frames:
                continue  # unresolved indirect target / offset-qualified caption / section marker
            d, p, cut = walk(callee, on_stack | {addr})
            any_cut = any_cut or cut
            if d > best[0]:
                best = (d, [callee] + p)
        result = (frames.get(addr, 0) + best[0], best[1])
        if not any_cut:
            memo[addr] = result
        return result[0], result[1], any_cut

    total, path, _cut = walk(root_addr, set())
    return total, path


def render_path(parsed, root_addr, path_addrs):
    """Yield (frame_bytes, cumulative_bytes, display_name) for root + path,
    for the per-check printed breakdown."""
    running = 0
    for addr in [root_addr] + path_addrs:
        running += parsed.frames.get(addr, 0)
        yield parsed.frames.get(addr, 0), running, parsed.names.get(addr, f"0x{addr:x}")
