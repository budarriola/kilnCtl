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

    def __init__(self, frames, calls, names, name_addrs):
        self.frames = frames          # {addr:int -> frame_bytes:int}
        self.calls = calls            # {addr:int -> set(addr:int)}
        self.names = names            # {addr:int -> name:str}  (display only)
        self.name_addrs = name_addrs  # {name:str -> [addr:int, ...]}


def parse(objdump, elf):
    out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
    frames, calls, names, name_addrs = {}, {}, {}, {}
    seen_entry = set()
    cur = None
    for line in out.splitlines():
        m = FN_RE.match(line)
        if m:
            cur = int(m.group(1), 16)
            name = m.group(2)
            frames.setdefault(cur, 0)
            calls.setdefault(cur, set())
            names[cur] = name
            name_addrs.setdefault(name, []).append(cur)
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
            calls[cur].add(int(c.group(1), 16))
    return ParsedElf(frames, calls, names, name_addrs)


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
    (total_bytes, [addr, ...] path after the root, NOT including root)."""
    memo = {}
    frames, calls = parsed.frames, parsed.calls

    def walk(addr, on_stack):
        if addr in on_stack:
            return 0, []
        if addr in memo:
            return memo[addr]
        best = (0, [])
        for callee in sorted(calls.get(addr, ())):
            if callee not in frames:
                continue  # unresolved indirect target / offset-qualified caption / section marker
            d, p = walk(callee, on_stack | {addr})
            if d > best[0]:
                best = (d, [callee] + p)
        result = (frames.get(addr, 0) + best[0], best[1])
        memo[addr] = result
        return result

    return walk(root_addr, set())


def render_path(parsed, root_addr, path_addrs):
    """Yield (frame_bytes, cumulative_bytes, display_name) for root + path,
    for the per-check printed breakdown."""
    running = 0
    for addr in [root_addr] + path_addrs:
        running += parsed.frames.get(addr, 0)
        yield parsed.frames.get(addr, 0), running, parsed.names.get(addr, f"0x{addr:x}")
