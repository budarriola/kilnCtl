#!/usr/bin/env python3
"""Negative-test for the 2026-09-10 (round 2) opus-review defect B:
stack_budget_lib_arm.py's parse() tracked "register rN was last loaded from
PC-relative literal at address A" (ldr_pc_regs) but never invalidated that
fact when rN was subsequently clobbered by anything other than another
`ldr rN, [pc, #imm]`. A later, unrelated `add sp, rN` in the same function
could then resolve against a STALE literal left over from an earlier load
into the same register, adding a WRONG byte count to that function's frame
and marking it `resolved = True` -- clearing regsp_adjust and dropping the
INDETERMINATE tag it should have kept.

This test feeds parse() a synthetic objdump -d transcript (via a
subprocess.run monkeypatch, so no real ELF/toolchain is needed) for one
function that:
  1. `ldr r4, [pc, #imm]` -- loads r4 from a literal holding -600 (a real
     large-frame growth, same shape as config_store_seqlock_read).
  2. `movs r4, #0` -- clobbers r4 with something UNRELATED to that literal.
  3. `add sp, r4` -- uses r4's (now-stale) value to adjust sp.

With the bug: r4 is still "tracked" from step 1, so step 3 resolves against
the -600 literal, adds 600 B to the frame, and marks the function resolved
(regsp_adjust stays False) -- WRONG on both counts (600 B never actually
happened via that adjust; it should not have added anything, and it must
be flagged INDETERMINATE because the real byte count post-clobber is
unknown).

With the fix: step 2 invalidates r4 out of ldr_pc_regs, so step 3 finds no
tracked literal for r4 and correctly falls back to regsp_adjust[cur]=True
(INDETERMINATE), adding 0 bytes.

Run directly: python test_regsp_stale_literal.py
"""
import os
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(__file__))
import stack_budget_lib_arm as lib  # noqa: E402

# Synthetic objdump -d transcript. Address of the .word literal (10000120)
# matches the `@ (10000120 ...)` annotation on the ldr line, same shape
# real arm-none-eabi-objdump emits.
OBJDUMP_TEXT = """
10000100 <victim_fn>:
   10000100:	b510      	push	{r4, lr}
   10000102:	4c07      	ldr	r4, [pc, #28]	@ (10000120 <victim_fn+0x20>)
   10000104:	2400      	movs	r4, #0
   10000106:	4460      	add	sp, r4
   10000108:	bd10      	pop	{r4, pc}
   1000010a:	46c0      	nop			@ (mov r8, r8)
   1000010c:	46c0      	nop			@ (mov r8, r8)
   1000010e:	46c0      	nop			@ (mov r8, r8)
   10000110:	46c0      	nop			@ (mov r8, r8)
   10000112:	46c0      	nop			@ (mov r8, r8)
   10000114:	46c0      	nop			@ (mov r8, r8)
   10000116:	46c0      	nop			@ (mov r8, r8)
   10000118:	46c0      	nop			@ (mov r8, r8)
   1000011a:	46c0      	nop			@ (mov r8, r8)
   1000011c:	46c0      	nop			@ (mov r8, r8)
   1000011e:	46c0      	nop			@ (mov r8, r8)
   10000120:	fffffda8 	.word	0xfffffda8
"""
# 0xfffffda8 signed32 == -600 -- i.e. `add sp, r4` with r4 == -600 would
# grow the frame by 600 B if r4 genuinely still held that literal.


class StaleLiteralRegspTest(unittest.TestCase):
    def test_clobbered_register_is_not_resolved_against_stale_literal(self):
        fake_result = mock.Mock(stdout=OBJDUMP_TEXT)
        with mock.patch.object(lib.subprocess, "run", return_value=fake_result):
            parsed = lib.parse("fake-objdump", "fake.elf")

        root_addr = 0x10000100
        self.assertIn(root_addr, parsed.frames, "victim_fn was not parsed at all")

        # The fix's actual contract: regsp_adjust must be True (INDETERMINATE)
        # for this function, because the real post-clobber adjustment is
        # unknown to this walk -- resolving it (silently, to nothing or to
        # the stale literal) is exactly the "confidently wrong" failure this
        # test guards against.
        self.assertTrue(
            parsed.regsp_adjust.get(root_addr, False),
            "victim_fn's `add sp, r4` was resolved (regsp_adjust=False) even "
            "though r4 was clobbered between the pc-relative load and the "
            "sp adjust -- this must fall back to INDETERMINATE, not a "
            "confident (and wrong) resolution")

        # And it must not have silently added the stale literal's magnitude
        # to the frame total either.
        self.assertEqual(
            parsed.frames[root_addr], 8,  # push {r4, lr} = 2 regs * 4 B; no regsp growth
            "unexpected frame total -- push {r4,lr} should contribute 8 B and "
            "the (unresolved) regsp adjust should contribute 0")


if __name__ == "__main__":
    unittest.main()
