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
itself does -- no hand-typed byte count) and the REAL historical measurement
recorded in this file's own CEILING_BYTES comment (4736 B, unresolved regsp)
to prove:
  1. Old (bug) formula, graded against CEILING_BYTES["link_task"] (9472):
     4736*2 > 9472 is False -- the margin check never fires. This documents
     the failure being fixed, it does not exercise production code.
  2. Fixed formula, graded against declared_bytes (6144): 4736*2 > 6144 is
     True -- the margin check correctly fires, since a real unresolved frame
     anywhere near 2x what was measured would not fit in the 6144 B FreeRTOS
     actually gave this task.

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

# Historical measurement recorded in CEILING_BYTES's own comment for
# link_task (4736 B measured, unresolved regsp) -- not re-derived from a
# fresh ELF walk here (that needs a live build; check_saftyfw_task_stack_
# budgets.ps1 / this repo's host-test run cover the live-ELF path), but a
# fixed, documented real number rather than an invented one.
LINK_TASK_MEASURED_HISTORICAL = 4736


class RegspMarginGradedAgainstDeclaredTest(unittest.TestCase):
    def test_declared_bytes_is_read_from_real_source_not_hand_typed(self):
        declared_bytes = chk.declared_words(
            "link_task.c", "LINK_TASK_STACK_WORDS",
            r'xTaskCreate\(link_task_fn,\s*"link_task",\s*LINK_TASK_STACK_WORDS') * 4
        # configMINIMAL_STACK_SIZE(256) * 6 words * 4 B/word = 6144 B.
        self.assertEqual(declared_bytes, 6144)

    def test_old_ceiling_graded_formula_could_never_fire(self):
        # Documents the failure being fixed -- computed independently here,
        # not by calling production code (the whole point is that grading
        # against `ceiling` is no longer what production code does).
        old_ceiling = chk.CEILING_BYTES["link_task"]
        old_formula_result = LINK_TASK_MEASURED_HISTORICAL * chk.REGSP_MARGIN_FACTOR > old_ceiling
        self.assertFalse(
            old_formula_result,
            "the OLD (ceiling-graded) formula was expected to be false by "
            "construction for link_task -- if this is now True, the ceiling "
            "table has changed and this test's premise needs re-checking")

    def test_fixed_formula_grades_against_declared_and_fires(self):
        # Calls PRODUCTION code directly (chk.regsp_margin_fail) so a
        # regression back to grading against `ceiling` is caught here, not
        # just documented.
        declared_bytes = chk.declared_words(
            "link_task.c", "LINK_TASK_STACK_WORDS",
            r'xTaskCreate\(link_task_fn,\s*"link_task",\s*LINK_TASK_STACK_WORDS') * 4
        result = chk.regsp_margin_fail(LINK_TASK_MEASURED_HISTORICAL, declared_bytes,
                                        unresolved_regsp=True)
        self.assertTrue(
            result,
            "regsp_margin_fail() graded against declared_bytes should fire "
            "for link_task's historical measurement (4736 B * 2 = 9472 > "
            "6144 declared) -- an unresolved regsp frame anywhere near "
            "double the measured lower bound would not fit in what FreeRTOS "
            "actually allocated this task")

    def test_no_fail_when_regsp_fully_resolved(self):
        # unresolved_regsp=False must short-circuit regardless of the
        # arithmetic -- a fully-resolved task's exact measurement is not
        # subject to this speculative margin at all.
        declared_bytes = chk.declared_words(
            "link_task.c", "LINK_TASK_STACK_WORDS",
            r'xTaskCreate\(link_task_fn,\s*"link_task",\s*LINK_TASK_STACK_WORDS') * 4
        result = chk.regsp_margin_fail(LINK_TASK_MEASURED_HISTORICAL, declared_bytes,
                                        unresolved_regsp=False)
        self.assertFalse(result)


if __name__ == "__main__":
    unittest.main()
