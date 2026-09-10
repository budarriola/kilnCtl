#!/usr/bin/env python3
"""ELF-call-graph-walk machinery for SaftyFW (RP2040, Cortex-M0+, Thumb-1),
the ARM analogue of firmware/KilnFW/App/test/stack_budget_lib.py.

WHY A SEPARATE MODULE, NOT A PORT OF THE XTENSA ONE
----------------------------------------------------
The Xtensa library keys a function's frame size off ONE instruction:
`entry a1, N` puts the whole frame size (locals + windowed-register spill
area) in a single immediate, emitted exactly once at the top of every
non-leaf function by that ABI. Cortex-M0+ (Thumb-1, no `entry`-equivalent)
has no such single source of truth: a GCC -mthumb -mcpu=cortex-m0plus
prologue instead:

  * `push {r4, r5, r6, r7, lr}`  -- callee-saved low registers + lr, and
  * (for callee-saved r8-r11, which Thumb-1's push/pop cannot name
    directly)  `mov lr, r9` / `mov r7, r8` then a SECOND `push {r7, lr}`,
    confirmed present in relay_owner_task's own disassembly, and
  * `sub sp, #N` for the actual locals (Thumb-1 encoding, 7-bit immediate
    *4, so this alone cannot express more than 508 bytes -- a function
    needing more would need a second `sub sp, #N` or a `sub.w sp, sp, #N`
    Thumb-2 32-bit encoding; SaftyFW.elf as built 2026-09-09 uses neither,
    confirmed by grep, but a future large local could introduce one).

So a function's frame here is the SUM of every push register-list's byte
count plus every `sub sp, #N` immediate found in its disassembly -- multiple
of each, not one authoritative instruction. This is a materially different
extraction than the Xtensa one and cannot honestly be built by reusing that
module's regexes; ENTRY_RE has no ARM equivalent at all.

The call graph and indirect-dispatch detection carry over conceptually
unchanged: `bl <addr>` is call4/8/12's analogue (direct, address known from
the disassembly), and `blx rN` (register-indirect -- confirmed present in
this ELF: blx r3/r4/r5/r6/r7/r8/r9/sl/fp, at least 82 sites) is callx4/8/12's
analogue (target unknowable statically). Recursion-cut and address-keyed
frame/call dicts follow the same reasoning as the Xtensa library's own
docstring (owner_task-class name collisions are possible here too -- e.g.
this codebase reuses "owner_task"-shaped patterns is not confirmed for
SaftyFW, but keying by address costs nothing and is not worth re-litigating).

WHAT THIS DOES NOT MODEL (stated once here, all callers inherit it)
--------------------------------------------------------------------
  * Register-spill pairs realized as mov+push (the r8-r11 pattern above) ARE
    counted, because they still show up as an ordinary `push` mnemonic --
    this library does not need to special-case the mov half, only sum every
    push it finds.
  * A `sub.w sp, sp, #N` (Thumb-2 32-bit immediate form, needed once a
    frame's locals exceed 508 B) is NOT matched by SUB_SP_RE below. Grepped
    absent from SaftyFW.elf as built 2026-09-09 (see this module's own
    negative check in the test file), so no function in this build is
    silently under-measured by this gap TODAY -- but it is a real blind
    spot if a future function's locals grow past that threshold, and the
    checker must not claim a full measurement when the ELF's own disassembly
    proves this pattern is now present (see has_subw_anywhere below).
  * REGISTER-COMPUTED stack adjust (`add sp, rN` / `sub sp, rN`, the
    immediate loaded into rN from a PC-relative literal-pool constant a few
    instructions earlier): confirmed present via this checker's own
    negative test (2026-09-09) -- adding a 600 B local to relay_owner_task
    pushed its frame's `sub sp` past the Thumb-1 7-bit*4 (508 B) immediate
    ceiling, and GCC's actual codegen for THAT case was neither a bare
    `sub sp, #N` nor a `sub.w sp, sp, #N`, but `ldr r4, [pc, #N]` /
    `add sp, r4` -- a THIRD form this library did not originally handle at
    all. Worse than a simple miss: because `add`/`sub` with a register
    operand does not match SUB_SP_RE, the frame silently measured SMALLER
    (100 B) than the unmodified baseline (112 B) despite a real 600 B
    growth -- exactly the "confident, wrong number" failure this whole
    family of checks exists to prevent.

    This pattern turns out to be common in this ELF already -- e.g.
    config_store_unpack_ex.part.0 (JSON/config unpacking, large local
    buffers) uses it legitimately, nowhere near any task's overflow. A
    GLOBAL refusal (fail the whole run if this pattern appears ANYWHERE in
    the ELF) was tried first and rejected: it made the checker permanently
    red regardless of whether the affected function is even reachable from
    any task, which is exactly the kind of check that gets ignored/disabled
    rather than trusted. Instead, `regsp_adjust` is tracked PER-FUNCTION,
    the same shape as `indirect`, and has_unresolved_dispatch() (kept its
    name; it now means "this task's graph contains something this walk
    cannot measure", blx OR regsp-adjust) checks it per task's own
    reachable call graph -- a task is INDETERMINATE only if the pattern is
    actually on ITS path, not merely present somewhere in the binary. This
    library still does not attempt to resolve the literal-pool constant and
    recover the true adjustment for an affected function -- that remains a
    real gap, surfaced honestly via INDETERMINATE, not silently absorbed
    into a wrong total.
  * Stack realignment (`bic sp, sp, #N` / `and sp, sp, #N`, used when a
    function needs >4-byte alignment for a local) is not modelled. Not
    observed in this ELF; would silently under-measure if introduced.
  * Tail calls / `b.n <fn>` used as a tail-call substitute for `bl` are not
    followed as calls (this compiler/optimization level was not observed to
    do this across SaftyFW.elf's task functions, but is not exhaustively
    ruled out for every function in the ELF).
  * Indirect calls (`blx rN`) are dead ends for the walk, exactly like
    Xtensa's callx -- has_unresolved_dispatch() flags this so a caller can
    report INDETERMINATE rather than a confident pass, same contract as the
    Xtensa library.
  * ISR stacking overhead (Cortex-M0+ exception entry pushes r0-r3, r12,
    lr, pc, xPSR = 32 bytes onto whatever stack was active) is not modelled
    here; callers should add it explicitly if a task's measured path can be
    interrupted (all FreeRTOS tasks can).

All of the above bias this walk toward UNDER-estimating true worst-case
depth, never over-estimating -- same invariant the Xtensa library states.
"""

import glob
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEFAULT_ELF = os.path.join(REPO_ROOT, "firmware", "SaftyFW", "build", "SaftyFW.elf")

FN_RE = re.compile(r"^([0-9a-f]{7,8}) <(.+)>:")
# One push/one sub-sp/one bl/one blx per matched instruction LINE. objdump's
# Thumb disassembly puts the mnemonic and operands after two tab-separated
# columns (address+opcode bytes, then mnemonic\toperands) -- match loosely on
# whitespace so this is not sensitive to opcode-byte-count formatting.
PUSH_RE = re.compile(r"\bpush\t\{([^}]*)\}")
SUB_SP_RE = re.compile(r"\bsub\tsp, #(\d+)\b")
# Thumb-2 32-bit encoding, needed once locals exceed the T1 7-bit*4 (508 B)
# ceiling. Not matched by SUB_SP_RE above -- see has_subw().
SUBW_SP_RE = re.compile(r"\bsub\.w\tsp, sp, #(\d+)\b")
# Register-operand sp adjust -- see module docstring "REGISTER-COMPUTED stack
# adjust". Deliberately excludes the immediate forms (SUB_SP_RE/SUBW_SP_RE
# above) by requiring the operand to start with a register name, not '#'.
REGSP_RE = re.compile(r"\b(add|sub)\tsp, (r\d+|sl|fp|lr|ip)\b")
BL_RE = re.compile(r"\bbl\t([0-9a-f]+) <([^>]+)>")
BLX_REG_RE = re.compile(r"\bblx\t(r\d+|sl|fp|lr|ip)\b")

# config_store_seqlock_read (2026-09-10, opus review of this checker) proved
# REGSP_RE's target is resolvable in the common case: GCC materializes the
# adjustment via a PC-relative literal-pool load immediately before the
# add/sub, e.g. `ldr r4, [pc, #568] @ (10003860 <...>)` then `add sp, r4`,
# with the literal itself a `.word` a few instructions later holding the
# signed delta (0xfffffdec == -532, i.e. sp -= 532: a 532-byte frame this
# walk previously could not see AT ALL -- `current_task`/`discrete_task`
# both reach this function and were measured 4-6x under real depth as a
# result). Reusing the same "resolve the PC-relative literal, cross-check
# against the ELF's own .word table" technique
# elf_side_stack_words_by_root() already uses for xTaskCreate's stack-depth
# argument: when the register add/sub sp,rN targets a register whose value
# was JUST loaded this way, recover the real byte count instead of falling
# back to the INDETERMINATE lower-bound path. Unresolvable cases (register
# not freshly loaded from a PC-relative literal, e.g. computed some other
# way) still fall back to regsp_adjust=True, i.e. INDETERMINATE -- this is
# strictly an improvement, never a new way to be wrong.
WORD_RE = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+\.word\s+0x([0-9a-f]+)\s*$")
LDR_PC_RE = re.compile(r"\bldr\t(r\d+|sl|fp|lr|ip), \[pc, #\d+\]\s*@ \(([0-9a-f]+)")

# 2026-09-10 (opus review, round 2): ldr_pc_regs tracked "this register was
# last loaded from this PC-relative literal address" but was NEVER
# invalidated when that register was subsequently clobbered by anything
# other than another `ldr rN, [pc, #imm]` -- not a `movs rN, #imm`, not a
# `mov rN, rM`, not a `pop {..., rN, ...}`, not a `bl` (which trashes
# r0-r3/r12/lr per AAPCS). A later, unrelated `add sp, rN` could then resolve
# against a STALE literal address left over from an earlier, unconnected
# load into the same register -- adding a WRONG byte count to frames[cur]
# and marking the function `resolved = True`, which clears regsp_adjust and
# silently drops the INDETERMINATE tag. That is exactly the "confidently
# wrong, not just incomplete" failure this whole literal-resolution feature
# was added to fix, reappearing one level down. Fix: track every register
# write on every disassembly line (generic data-processing/load mnemonics,
# `pop {..}`, and `bl`'s AAPCS-clobbered set) and drop that register out of
# ldr_pc_regs the moment anything but a fresh `ldr rN,[pc,#imm]` writes it.
# When in doubt (a write we don't recognize the shape of), we still prefer
# to invalidate -- a spurious INDETERMINATE costs nothing but honesty; a
# missed invalidation reintroduces the bug this note describes.
_REGSP_TRACKED_REG_RE = r"(r\d+|sl|fp|lr|ip)"
GENERIC_WRITE_RE = re.compile(
    r"\b(movs?|mvns?|adds?|subs?|lsls?|lsrs?|asrs?|rors?|ands?|orrs?|orns?|eors?|"
    r"bics?|muls?|mlas?|sdiv|udiv|rsbs?|adcs?|sbcs?|ldr|ldrb|ldrh|ldrsb|ldrsh|"
    r"rev|rev16|revsh|sxtb|sxth|uxtb|uxth)\t" + _REGSP_TRACKED_REG_RE + r"\b")
POP_RE = re.compile(r"\bpop\t\{([^}]*)\}")
# 2026-09-10 (opus review, round 2, defect E): `pop` was handled above, but
# `ldm`/`ldmia` -- the remaining multi-register writer on Cortex-M0+ Thumb-1
# outside `pop` -- was not, so `ldmia r4!, {r0, r1}` left r0/r1 (and, on the
# writeback form, r4 itself) still tracked in ldr_pc_regs after this line,
# exactly the stale-literal shape this feature exists to catch. Matches both
# `ldm` and `ldmia` (Thumb-1 only has the incrementing-after form, but IA is
# sometimes spelled out and sometimes not depending on objdump version).
LDM_RE = re.compile(r"\bldm(?:ia)?\t(r\d+|sl|fp|lr|ip)(!)?,\s*\{([^}]*)\}")
# AAPCS: a `bl`/`blx` call may clobber r0-r3, r12 (ip) and lr (the link
# register itself is overwritten with the return address).
BL_CLOBBERS = ("r0", "r1", "r2", "r3", "ip", "lr")


def _invalidate_clobbered_regs(line, ldr_pc_regs):
    """Drop any register from ldr_pc_regs that this disassembly line
    redefines by any means OTHER than a fresh `ldr rN, [pc, #imm]` (that
    case is (re-)established by the caller right after this runs)."""
    p = POP_RE.search(line)
    if p:
        for r in (x.strip() for x in p.group(1).split(",")):
            ldr_pc_regs.pop(r, None)
        return
    lm = LDM_RE.search(line)
    if lm:
        base_reg, writeback, reglist = lm.group(1), lm.group(2), lm.group(3)
        for r in (x.strip() for x in reglist.split(",")):
            ldr_pc_regs.pop(r, None)
        if writeback:
            # `ldmia rN!, {...}` also rewrites the base register itself
            # (post-increment writeback) -- a stale PC-relative literal
            # previously tracked in rN is no longer valid either.
            ldr_pc_regs.pop(base_reg, None)
        return
    if BL_RE.search(line) or BLX_REG_RE.search(line):
        for r in BL_CLOBBERS:
            ldr_pc_regs.pop(r, None)
        return
    m = GENERIC_WRITE_RE.search(line)
    if m:
        ldr_pc_regs.pop(m.group(2), None)


def _s32(word):
    """Reinterpret an unsigned 32-bit word as signed."""
    return word - 0x100000000 if word & 0x80000000 else word


def find_objdump():
    env = os.environ.get("ARM_OBJDUMP")
    if env and os.path.isfile(env):
        return env
    on_path = shutil.which("arm-none-eabi-objdump")
    if on_path:
        return on_path
    for pattern in (
        r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-objdump.exe",
        r"C:\Program Files\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-objdump.exe",
    ):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]
    return None


def find_addr2line():
    env = os.environ.get("ARM_ADDR2LINE")
    if env and os.path.isfile(env):
        return env
    on_path = shutil.which("arm-none-eabi-addr2line")
    if on_path:
        return on_path
    objdump = find_objdump()
    if objdump:
        cand = objdump.replace("objdump", "addr2line")
        if os.path.isfile(cand):
            return cand
    return None


class ParsedElf:
    def __init__(self, frames, calls, names, name_addrs, indirect, regsp_adjust,
                 has_subw_anywhere):
        self.frames = frames          # {addr:int -> frame_bytes:int}
        self.calls = calls            # {addr:int -> set(addr:int)}
        self.names = names            # {addr:int -> name:str}
        self.name_addrs = name_addrs  # {name:str -> [addr:int, ...]}
        self.indirect = indirect      # {addr:int -> bool}  contains a blx <reg>
        self.regsp_adjust = regsp_adjust  # {addr:int -> bool}  contains a register-
            # operand add/sub sp,rN -- see module docstring "REGISTER-COMPUTED stack
            # adjust" (found by this checker's own 2026-09-09 negative test). Per-
            # function, like `indirect`: a task is only affected if this is on ITS
            # reachable path, not merely present somewhere in the ELF.
        self.has_subw_anywhere = has_subw_anywhere  # True if ANY function in the whole
            # ELF uses the Thumb-2 sub.w sp,sp,#N form this walk cannot measure --
            # a global honesty flag (this form was not observed to occur outside of
            # a genuinely large frame in this codebase, unlike regsp_adjust, so it
            # stays a global refusal rather than a per-function INDETERMINATE).


def parse(objdump, elf):
    out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()

    # First pass: every `.word` literal in the disassembly, keyed by its own
    # address, so a PC-relative `ldr rN, [pc, #imm] @ (ADDR ...)` a few lines
    # earlier can be resolved to the actual constant GCC put there. Same
    # technique check_saftyfw_task_stack_budgets.py's elf_side_stack_words_
    # by_root() already uses for xTaskCreate's stack-depth argument.
    words_by_addr = {}
    for line in lines:
        m = WORD_RE.match(line)
        if m:
            words_by_addr[int(m.group(1), 16)] = int(m.group(2), 16)

    frames, calls, names, name_addrs, indirect, regsp_adjust = {}, {}, {}, {}, {}, {}
    has_subw_anywhere = False
    cur = None
    ldr_pc_regs = {}  # register name -> literal address, reset per function
    for line in lines:
        m = FN_RE.match(line)
        if m:
            cur = int(m.group(1), 16)
            name = m.group(2)
            frames.setdefault(cur, 0)
            calls.setdefault(cur, set())
            indirect.setdefault(cur, False)
            regsp_adjust.setdefault(cur, False)
            names[cur] = name
            name_addrs.setdefault(name, []).append(cur)
            ldr_pc_regs = {}
            continue
        if cur is None:
            continue
        p = PUSH_RE.search(line)
        if p:
            regs = [r.strip() for r in p.group(1).split(",") if r.strip()]
            frames[cur] = frames.get(cur, 0) + 4 * len(regs)
        s = SUB_SP_RE.search(line)
        if s:
            frames[cur] = frames.get(cur, 0) + int(s.group(1))
        if SUBW_SP_RE.search(line):
            has_subw_anywhere = True
        ldr = LDR_PC_RE.search(line)
        rsp = REGSP_RE.search(line)
        if rsp:
            op, reg = rsp.group(1), rsp.group(2)
            literal_addr = ldr_pc_regs.get(reg)
            word = words_by_addr.get(literal_addr) if literal_addr is not None else None
            resolved = False
            if word is not None:
                delta = _s32(word)
                # `sub sp, rN`: sp -= rN -- a positive rN grows the frame.
                # `add sp, rN`: sp += rN -- a NEGATIVE rN (i.e. GCC's way of
                # encoding a large `sub sp,#N` that a Thumb-1 immediate can't
                # hold) grows the frame; a positive rN is an epilogue
                # shrinking a growth already counted elsewhere, contributes 0.
                growth = delta if op == "sub" and delta > 0 else (-delta if op == "add" and delta < 0 else 0)
                if growth > 0:
                    frames[cur] = frames.get(cur, 0) + growth
                    resolved = True
                elif growth == 0:
                    # A resolved literal that nets to zero/negative growth
                    # (an epilogue restore) is not a mismeasurement -- still
                    # resolved, not indeterminate.
                    resolved = True
            if not resolved:
                # Register not freshly loaded via a PC-relative literal, or
                # the literal could not be found -- genuinely can't measure
                # this one, so it stays INDETERMINATE (see module docstring).
                regsp_adjust[cur] = True
        # Invalidate any register this line redefines by a means other than
        # the `ldr rN, [pc, #imm]` handled just below -- see
        # _invalidate_clobbered_regs' docstring ("stale literal" bug, 2026-09-10).
        _invalidate_clobbered_regs(line, ldr_pc_regs)
        if ldr:
            ldr_pc_regs[ldr.group(1)] = int(ldr.group(2), 16)
        c = BL_RE.search(line)
        if c:
            calls[cur].add(int(c.group(1), 16))
        if BLX_REG_RE.search(line):
            indirect[cur] = True
    return ParsedElf(frames, calls, names, name_addrs, indirect, regsp_adjust,
                      has_subw_anywhere)


def reachable_addrs(root_addr, parsed):
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
    """True if root_addr or anything in its resolvable call graph contains
    EITHER a blx <reg> (register-indirect call, target unknowable) OR a
    register-operand add/sub sp,rN (large-frame stack adjust this library
    cannot measure -- see module docstring "REGISTER-COMPUTED stack
    adjust"). Either one means deepest()'s total for this root is not a
    trustworthy number: a genuine LOWER BOUND in the blx case (real depth
    may continue past the call this walk can't follow), but potentially
    already WRONG (too small) in the regsp case for whichever function on
    the path contains it. Callers must treat both the same way --
    INDETERMINATE, never a confident pass -- see
    check_saftyfw_task_stack_budgets.py's main()."""
    reachable = reachable_addrs(root_addr, parsed)
    return (any(parsed.indirect.get(a, False) for a in reachable)
            or any(parsed.regsp_adjust.get(a, False) for a in reachable))


def has_unresolved_regsp(root_addr, parsed):
    """True if root_addr's reachable call graph still contains a
    register-operand add/sub sp,rN this walk could NOT resolve via the
    PC-relative-literal technique in parse() (2026-09-10) -- i.e. the
    genuinely-unmeasured subset of has_unresolved_dispatch(), excluding
    plain `blx <reg>` (unresolved calls, a documented, accepted lower-bound
    contract on their own). Narrower than has_unresolved_dispatch() so a
    caller can single out the case that was silently under-measuring real
    depth (config_store_seqlock_read's 532/608 B frames, 2026-09-10) from
    the merely-incomplete-call-graph case, and hold only the former to a
    stricter bar."""
    reachable = reachable_addrs(root_addr, parsed)
    return any(parsed.regsp_adjust.get(a, False) for a in reachable)


def resolve_root(parsed, name, elf, addr2line=None, expect_path_substr=None):
    addrs = parsed.name_addrs.get(name)
    if not addrs:
        raise ValueError(f"symbol {name!r} not found in {elf}")
    if len(addrs) == 1:
        return addrs[0]
    if not expect_path_substr:
        raise ValueError(
            f"symbol {name!r} is ambiguous ({len(addrs)} definitions in {elf}) and no "
            "expect_path_substr was given to disambiguate")
    if not addr2line:
        raise ValueError(f"symbol {name!r} is ambiguous and no addr2line is available to disambiguate")
    matches = []
    for addr in addrs:
        out = subprocess.run([addr2line, "-e", elf, "-f", f"0x{addr:x}"],
                              capture_output=True, text=True, check=True).stdout.splitlines()
        src = out[1] if len(out) > 1 else ""
        if expect_path_substr.replace("\\", "/") in src.replace("\\", "/"):
            matches.append(addr)
    if len(matches) != 1:
        raise ValueError(
            f"symbol {name!r} is ambiguous ({len(addrs)} definitions); expect_path_substr "
            f"{expect_path_substr!r} matched {len(matches)} of them, need exactly 1")
    return matches[0]


def deepest(root_addr, parsed):
    """Deepest cumulative-byte static call path from root_addr. Returns
    (total_bytes, [addr, ...] path after the root). Same cut-vs-memo
    correctness rule as the Xtensa library's deepest() -- a branch cut by
    recursion must not be cached as final; see that module's docstring for
    the full reasoning (fixed there 2026-09-09, carried over here verbatim
    since the bug is in the walk shape, not the ISA)."""
    memo = {}
    frames, calls = parsed.frames, parsed.calls

    def walk(addr, on_stack):
        if addr in on_stack:
            return 0, [], True
        if addr in memo:
            return memo[addr][0], memo[addr][1], False
        best = (0, [])
        any_cut = False
        for callee in sorted(calls.get(addr, ())):
            if callee not in frames:
                continue
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
    running = 0
    for addr in [root_addr] + path_addrs:
        running += parsed.frames.get(addr, 0)
        yield parsed.frames.get(addr, 0), running, parsed.names.get(addr, f"0x{addr:x}")
