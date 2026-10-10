#!/usr/bin/env python3
"""Regression test: neither stack-budget parser may attribute a disassembly
line past a function's real (`nm -S`/`objdump -t`) end to that function.

Context (2026-09-20): 924eeea2 fixed exactly this class of phantom call
edge in stack_budget_lib.py (the new address-keyed walker used by
check_all_task_stack_budgets.py) but explicitly left check_main_task_stack_
budget.py's own, older, name-keyed parse()/deepest() untouched -- and that
older pair is the one check_executor_task_stack_budget.py,
check_httpd_task_stack_budget.py, check_system_uart_bridge_stack_budget.py
and check_uart_log_bridge_stack_budget.py all actually import and call as
`base.parse`/`base.deepest`. A real instance reached HEAD:
`profile_executor_guard_zone_ramp_rate` (nm -S size 0x42, a 3-line leaf
float function) has ~4 KB of literal-pool/padding bytes between it and the
next real symbol; objdump captions those bytes under its name and decoded a
fabricated `call8 lfs_rename`, grafting the whole LittleFS rename/compact
chain onto profile_executor's call graph and failing
check_executor_task_stack_budget.py on a path the firmware never executes.

This test builds a tiny synthetic objdump transcript with exactly that
shape -- a 5-byte function followed by inter-function bytes objdump
capitons with the same name and decodes a call8 out of -- for BOTH parsers,
and asserts neither parser produces the phantom edge, while both still
correctly read the function's own (real, in-bounds) frame size.

Run standalone: `python test_stack_budget_symbol_bounds.py`. No ELF, no
Xtensa toolchain, and no network access are needed -- `subprocess.run` is
monkeypatched to return the canned transcript below instead of actually
invoking objdump.
"""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_main_task_stack_budget as legacy  # noqa: E402
import stack_budget_lib as addr_keyed  # noqa: E402

# -- Synthetic transcript -----------------------------------------------
# tiny_fn: real body is 0x10..0x15 (size 0x05, entry frame 16 B). Bytes
# 0x15..0x20 belong to no function (a literal pool objdump nonetheless
# captions "tiny_fn" and decodes into a call8 targeting next_fn -- exactly
# the calc_content_width/profile_executor_guard_zone_ramp_rate shape).
# next_fn is a real, separate function at 0x00000200 that the phantom call
# happens to target, so a parser that fails to bound tiny_fn would report a
# call edge tiny_fn -> next_fn that must not exist.

OBJDUMP_T_STDOUT = (
    "00000010 g     F .flash.text\t00000005 tiny_fn\n"
    "00000200 g     F .flash.text\t00000004 next_fn\n"
)

OBJDUMP_D_STDOUT = """
Disassembly of section .flash.text:

00000010 <tiny_fn>:
10:\t006136        \tentry\ta1, 16
13:\tf01d          \tretw.n
15:\t00            \t.byte\t0x00
16:\t00            \t.byte\t0x00
17:\t00            \t.byte\t0x00
18:\t00            \t.byte\t0x00
19:\t00            \t.byte\t0x00
1a:\t002ce5        \tcall8\t00000200 <next_fn>
1d:\t00            \t.byte\t0x00
1e:\t00            \t.byte\t0x00
1f:\t00            \t.byte\t0x00

00000200 <next_fn>:
200:\t006236        \tentry\ta1, 32
203:\tf01d          \tretw.n
"""


def _fake_run(cmd, capture_output=True, text=True, check=False):
    assert capture_output and text
    if "-t" in cmd:
        stdout = OBJDUMP_T_STDOUT
    elif "-d" in cmd:
        stdout = OBJDUMP_D_STDOUT
    else:
        raise AssertionError(f"unexpected objdump invocation: {cmd}")
    return mock.Mock(returncode=0, stdout=stdout, stderr="")


class SymbolSizesTest(unittest.TestCase):
    def test_legacy_symbol_sizes(self):
        with mock.patch.object(legacy.subprocess, "run", side_effect=_fake_run):
            sizes = legacy.symbol_sizes("objdump", "fake.elf")
        self.assertEqual(sizes, {0x10: 0x05, 0x200: 0x04})

    def test_addr_keyed_symbol_sizes(self):
        with mock.patch.object(addr_keyed.subprocess, "run", side_effect=_fake_run):
            sizes = addr_keyed.symbol_sizes("objdump", "fake.elf")
        self.assertEqual(sizes, {0x10: 0x05, 0x200: 0x04})


class NoPhantomEdgePastSymbolEndTest(unittest.TestCase):
    """The actual regression: neither parser may credit tiny_fn with a call
    to next_fn, since that call8 line is decoded from bytes past tiny_fn's
    real (0x05-byte) extent."""

    def test_legacy_parser_stops_at_symbol_end(self):
        with mock.patch("os.path.getsize", return_value=len(OBJDUMP_D_STDOUT)), \
                mock.patch.object(legacy.subprocess, "run", side_effect=_fake_run):
            frames, calls = legacy.parse("objdump", "fake.elf")
        self.assertEqual(frames.get("tiny_fn"), 16, "tiny_fn's own in-bounds frame must still be read")
        self.assertEqual(
            calls.get("tiny_fn", set()), set(),
            "tiny_fn must not be credited with a call decoded from bytes past its real end "
            "(nm -S size 0x05) -- this is the profile_executor_guard_zone_ramp_rate/lfs_rename "
            "phantom edge class fixed by 924eeea2 for stack_budget_lib.py but reproduced here "
            "in this module's own separate parser.",
        )

    def test_addr_keyed_parser_stops_at_symbol_end(self):
        with mock.patch.object(addr_keyed.subprocess, "run", side_effect=_fake_run), \
                mock.patch("os.path.getsize", return_value=len(OBJDUMP_D_STDOUT)):
            parsed = addr_keyed.parse("objdump", "fake.elf")
        self.assertEqual(parsed.frames.get(0x10), 16)
        self.assertEqual(parsed.calls.get(0x10, set()), set())


# -- Long calls (l32r literal + callx) and DECLARED_EDGES ---------------
# 2026-10-09: a call8 reaches +-512 KB only; every farther flash call, and
# every call into IRAM/ROM, is `l32r aN,<literal> (<addr> <sym>)` + `callx8 aN`.
# Both parsers used to drop those edges. Synthetic snippets, no ELF needed.

LONGCALL_T = (
    "00000010 g     F .flash.text\t00000040 caller_fn\n"
    "00000200 g     F .flash.text\t00000004 far_fn\n"
    "00000300 g     F .flash.text\t00000004 other_fn\n"
)

LONGCALL_D = """
Disassembly of section .flash.text:

00000010 <caller_fn>:
10:\t006136        \tentry\ta1, 32
13:\tc90c81        \tl32r\ta8, 00000004 <caller_fn-0xc> (00000200 <far_fn>)
16:\t0008e0        \tcallx8\ta8
19:\tc90c81        \tl32r\ta9, 00000008 <caller_fn-0x8> (00000300 <other_fn>)
1c:\t001982        \tmovi\ta9, 1
1f:\t0009e0        \tcallx8\ta9
22:\tc90c81        \tl32r\ta10, 0000000c <caller_fn-0x4> (3fc9a8a0 <some_data>)
25:\t000ae0        \tcallx8\ta11
28:\tf01d          \tretw.n

00000200 <far_fn>:
200:\t006236        \tentry\ta1, 48
203:\tf01d          \tretw.n

00000300 <other_fn>:
300:\t006236        \tentry\ta1, 64
303:\tf01d          \tretw.n
"""


def _longcall_run(cmd, capture_output=True, text=True, check=False):
    return mock.Mock(returncode=0, stderr="",
                     stdout=LONGCALL_T if "-t" in cmd else LONGCALL_D)


class LongCallTrackerTest(unittest.TestCase):
    def test_pairs_l32r_with_callx(self):
        t = addr_keyed.LongCallTracker()
        self.assertIsNone(t.feed("13:\tc90c81        \tl32r\ta8, 00000004 <x> (00000200 <far_fn>)"))
        self.assertEqual(t.feed("16:\t0008e0        \tcallx8\ta8"), (0x200, "far_fn"))

    def test_redefinition_drops_the_literal(self):
        t = addr_keyed.LongCallTracker()
        t.feed("13:\tc90c81        \tl32r\ta9, 00000004 <x> (00000300 <other_fn>)")
        t.feed("1c:\t001982        \tmovi\ta9, 1")
        self.assertIsNone(t.feed("1f:\t0009e0        \tcallx8\ta9"))

    def test_store_does_not_drop_the_literal(self):
        t = addr_keyed.LongCallTracker()
        t.feed("13:\tc90c81        \tl32r\ta8, 00000004 <x> (00000200 <far_fn>)")
        t.feed("14:\t0008e0        \ts32i\ta8, a1, 4")
        self.assertEqual(t.feed("16:\t0008e0        \tcallx8\ta8"), (0x200, "far_fn"))

    def test_reset_clears_state(self):
        t = addr_keyed.LongCallTracker()
        t.feed("13:\tc90c81        \tl32r\ta8, 00000004 <x> (00000200 <far_fn>)")
        t.reset()
        self.assertIsNone(t.feed("16:\t0008e0        \tcallx8\ta8"))


class LongCallParseTest(unittest.TestCase):
    def test_addr_keyed_parser_resolves_long_call(self):
        with mock.patch.object(addr_keyed.subprocess, "run", side_effect=_longcall_run), \
                mock.patch("os.path.getsize", return_value=len(LONGCALL_D)):
            parsed = addr_keyed.parse("objdump", "fake.elf")
        self.assertEqual(parsed.calls[0x10], {0x200}, "l32r+callx8 edge to far_fn must exist; the "
                         "movi-clobbered a9 and the unpaired callx8 a11 must not add edges")
        total, _ = addr_keyed.deepest(0x10, parsed)
        self.assertEqual(total, 32 + 48)
        self.assertTrue(parsed.indirect[0x10], "the unresolved callx8 a11 is still indirect")

    def test_legacy_parser_resolves_long_call(self):
        with mock.patch.object(legacy.subprocess, "run", side_effect=_longcall_run), \
                mock.patch("os.path.getsize", return_value=len(LONGCALL_D)):
            frames, calls = legacy.parse("objdump", "fake.elf")
        self.assertEqual(calls["caller_fn"], {"far_fn"})
        self.assertEqual(legacy.deepest("caller_fn", frames, calls)[0], 32 + 48)


class DeclaredEdgesTest(unittest.TestCase):
    SITE = ("        if (uart_bridge_ext_is_on_flash_worker()) {\n"
            "            void (*volatile job)(void *arg) = zones_autosave_job;\n"
            "            job((void *)xTaskGetCurrentTaskHandle());\n")

    def test_present_site_passes(self):
        import check_all_task_stack_budgets as c
        self.assertEqual(c.declared_edge_violations(read=lambda rel: self.SITE), [])

    def test_missing_site_fails(self):
        import check_all_task_stack_budgets as c
        errs = c.declared_edge_violations(read=lambda rel: "zones_autosave_job(arg);\n")
        self.assertEqual(len(errs), 1)
        self.assertIn("nvs_save->zones_autosave_job", errs[0])

    def test_declared_edge_is_added_to_the_view_only(self):
        import check_all_task_stack_budgets as c
        parsed = addr_keyed.ParsedElf({1: 8, 2: 16}, {1: set(), 2: set()}, {1: "nvs_save", 2: "zones_autosave_job"},
                                      {"nvs_save": [1], "zones_autosave_job": [2]}, {1: True, 2: False})
        view = c.apply_declared_edges(parsed, lambda n: parsed.name_addrs[n][0])
        self.assertEqual(addr_keyed.deepest(1, view)[0], 24)
        self.assertEqual(addr_keyed.deepest(1, parsed)[0], 8, "the shared parse must stay untouched")


if __name__ == "__main__":
    unittest.main()
