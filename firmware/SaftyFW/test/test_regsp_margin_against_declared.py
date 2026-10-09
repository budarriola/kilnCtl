#!/usr/bin/env python3
"""Negative-test for the 2026-09-10 (round 2) opus-review defect A:
check_saftyfw_task_stack_budgets.py's regsp_margin_fail graded
`measured_total * REGSP_MARGIN_FACTOR` against `ceiling` -- a hand-maintained
table value that the same commit set to exactly 2x its own measurement for
every task carrying an unresolved regsp adjust (e.g. link_task: ceiling=9472,
"measured 4736"). That makes the condition `2*m > 2*m`, which is false by
construction: the margin check could never fire, for any measurement, ever.

The margin must instead be graded against DECLARED stack (the quantity that
actually overflows on real hardware), so a task whose unresolved-regsp lower
bound is already close to what FreeRTOS actually allocated it is caught even
though a hand-copied ceiling says otherwise.

This test uses link_task's REAL declared stack (read back from its own
#define via the production declared_words() helper, same as the checker
itself does -- no hand-typed byte count) to prove:
  1. Old (bug) formula, graded against CEILING_BYTES["link_task"]: a
     measurement set to exactly ceiling/REGSP_MARGIN_FACTOR never fires
     the margin check, by construction. This documents the failure being
     fixed; it does not exercise production code.
  2. Fixed formula, graded against the LIVE declared_bytes (read from
     link_task.c, not a byte count copied into this file): a measurement
     just over declared_bytes/REGSP_MARGIN_FACTOR correctly fires.

2026-09-10: this test previously hardcoded both the expected declared_bytes
(6144) and the "historical measurement" (4736 B) it fed to part 2. c27484a2
raised LINK_TASK_STACK_WORDS from configMINIMAL_STACK_SIZE*6 to *10 (6144 ->
10240 B) without anyone touching this file, and the hardcoded 4736 no longer
satisfies 4736*2 > 10240 -- exactly the kind of drift this test exists to
catch in the CHECKER, not fall victim to itself. Both constants are now
derived from the checker's own declared_bytes at test time so the test's
behavior tracks whatever link_task.c currently declares, the same way
production code does.

Part 2 imports check_saftyfw_task_stack_budgets's own REGSP_MARGIN_FACTOR
and re-derives regsp_margin_fail exactly as main()'s per-task loop does, so a
regression back to grading against `ceiling` is caught mechanically rather
than by inspection.

Run directly: python test_regsp_margin_against_declared.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import check_saftyfw_task_stack_budgets as chk  # noqa: E402


def _link_task_declared_bytes():
    """Live declared stack for link_task, read the same way the checker
    itself computes it -- never a byte count hand-typed into this file."""
    return chk.declared_words(
        "link_task.c", "LINK_TASK_STACK_WORDS",
        r'xTaskCreate\(link_task_fn,\s*"link_task",\s*LINK_TASK_STACK_WORDS') * 4


class RegspMarginGradedAgainstDeclaredTest(unittest.TestCase):
    def test_declared_bytes_is_read_from_real_source_not_hand_typed(self):
        declared_bytes = _link_task_declared_bytes()
        # Must be a positive, word-aligned byte count derived from a real
        # #define -- not asserted against a specific number, since that
        # number is exactly what changes (legitimately) when link_task's
        # stack is retuned, and hardcoding it here is the bug this test
        # documents (see module docstring, 2026-09-10).
        self.assertGreater(declared_bytes, 0)
        self.assertEqual(declared_bytes % 4, 0)

    def test_old_ceiling_graded_formula_could_never_fire(self):
        # Documents the failure being fixed -- computed independently here,
        # not by calling production code (the whole point is that grading
        # against `ceiling` is no longer what production code does). The
        # measurement is derived from the ceiling itself (ceiling /
        # REGSP_MARGIN_FACTOR, matching how every CEILING_BYTES row was
        # hand-set to exactly 2x its own measurement), not a hardcoded
        # historical byte count, so this stays true regardless of future
        # ceiling-table edits.
        old_ceiling = chk.CEILING_BYTES["link_task"]
        measured = old_ceiling // chk.REGSP_MARGIN_FACTOR
        old_formula_result = measured * chk.REGSP_MARGIN_FACTOR > old_ceiling
        self.assertFalse(
            old_formula_result,
            "the OLD (ceiling-graded) formula was expected to be false by "
            "construction for link_task -- if this is now True, the ceiling "
            "table has changed and this test's premise needs re-checking")

    def test_fixed_formula_grades_against_declared_and_fires(self):
        # Calls PRODUCTION code directly (chk.regsp_margin_fail) so a
        # regression back to grading against `ceiling` is caught here, not
        # just documented. The measurement is derived from the LIVE
        # declared_bytes (declared_bytes / REGSP_MARGIN_FACTOR + 1, i.e.
        # just over half), not a hardcoded byte count -- so it stays a
        # genuine over-margin measurement no matter what link_task.c
        # currently declares.
        declared_bytes = _link_task_declared_bytes()
        measured = declared_bytes // chk.REGSP_MARGIN_FACTOR + 1
        result = chk.regsp_margin_fail(measured, declared_bytes, unresolved_regsp=True)
        self.assertTrue(
            result,
            "regsp_margin_fail() graded against declared_bytes should fire "
            "for a measurement just over declared_bytes/REGSP_MARGIN_FACTOR "
            "-- an unresolved regsp frame anywhere near that much again "
            "would not fit in what FreeRTOS actually allocated this task")

    def test_no_fail_when_regsp_fully_resolved(self):
        # unresolved_regsp=False must short-circuit regardless of the
        # arithmetic -- a fully-resolved task's exact measurement is not
        # subject to this speculative margin at all.
        declared_bytes = _link_task_declared_bytes()
        measured = declared_bytes // chk.REGSP_MARGIN_FACTOR + 1
        result = chk.regsp_margin_fail(measured, declared_bytes, unresolved_regsp=False)
        self.assertFalse(result)


if __name__ == "__main__":
    unittest.main()
