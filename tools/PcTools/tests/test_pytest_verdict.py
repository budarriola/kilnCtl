#!/usr/bin/env python3
"""Tests for mcpkit.pytest_verdict and its wiring into run_pctools_tests.

A lost xdist worker ("node down: Not properly terminated") used to leave a
green "0 failed" summary and exit 0. These tests feed fake pytest transcripts
to the pure parsing functions and to workbench._summarize.

Run with: python -m pytest tools/PcTools/tests/test_pytest_verdict.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcpkit import pytest_verdict as pv  # noqa: E402
from mcpkit import workbench  # noqa: E402

CLEAN = """\
============================= test session starts =============================
plugins: timeout-2.4.0, xdist-3.8.0
collected 1200 items
............................................................ [100%]
=========== 1190 passed, 8 skipped, 2 xfailed in 91.20s (0:01:31) ============
"""

CLEAN_XDIST = """\
created: 4/4 workers
4 workers [1200 items]
............................................................ [100%]
====== 1190 passed, 8 skipped, 1 xfailed, 1 xpassed in 40.00s ======
"""

NODE_DOWN = """\
4 workers [1200 items]
......................................
node down: Not properly terminated
replacing crashed worker gw2
....................................... [100%]
=========== 1100 passed, 0 failed in 80.00s ============
"""

NODE_DOWN_EXACT = """\
4 workers [10 items]
node down: Not properly terminated
== 8 passed, 2 failed in 1.00s ==
"""

SHORTFALL = """\
collected 1200 items
............................................................ [100%]
=========== 1100 passed, 8 skipped in 80.00s ============
"""


class ParsingTests(unittest.TestCase):
    def test_collected_forms(self):
        self.assertEqual(pv.collected_count("collected 12 items\n"), 12)
        self.assertEqual(pv.collected_count("4 workers [14 items]\n"), 14)
        self.assertEqual(pv.collected_count("14 tests collected in 0.1s\n"), 14)
        self.assertEqual(
            pv.collected_count("collected 80 items / 66 deselected / 14 selected\n"), 14)
        self.assertIsNone(pv.collected_count("nothing here\n"))

    def test_summary_total_counts_every_outcome(self):
        line = "== 1 failed, 3 passed, 2 skipped, 1 xfailed, 1 xpassed, 2 errors, 5 warnings in 1.00s ==\n"
        self.assertEqual(pv.summary_outcome_total(line), 10)
        self.assertEqual(pv.summary_outcome_total("== no tests ran in 0.01s ==\n"), 0)
        self.assertIsNone(pv.summary_outcome_total("still running\n"))

    def test_worker_loss_markers(self):
        for text in ("node down: Not properly terminated",
                     "replacing crashed worker gw3",
                     "worker 'gw1' crashed while running 'tests/x.py::t'"):
            self.assertTrue(pv.worker_loss_lines(text), text)
        self.assertFalse(pv.worker_loss_lines("1 passed in 0.1s"))


class VerdictTests(unittest.TestCase):
    def test_clean_transcripts_pass(self):
        self.assertEqual(pv.pytest_output_problems(CLEAN), [])
        self.assertEqual(pv.pytest_output_problems(CLEAN_XDIST), [])

    def test_node_down_with_zero_failed_fails(self):
        problems = pv.pytest_output_problems(NODE_DOWN)
        self.assertTrue(any("worker lost" in p for p in problems), problems)

    def test_node_down_with_exact_counts_still_fails(self):
        problems = pv.pytest_output_problems(NODE_DOWN_EXACT)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("worker lost", problems[0])

    def test_shortfall_names_both_numbers(self):
        problems = pv.pytest_output_problems(SHORTFALL)
        self.assertEqual(len(problems), 1)
        self.assertIn("1200", problems[0])
        self.assertIn("1108", problems[0])

    def test_missing_summary_or_count_fails_closed(self):
        self.assertTrue(pv.pytest_output_problems("collected 3 items\n...\n"))
        self.assertTrue(pv.pytest_output_problems("== 3 passed in 0.1s ==\n"))

    def test_extra_outcomes_beyond_collected_are_fine(self):
        # setup+teardown errors can make outcomes exceed collected
        out = "collected 2 items\n== 2 passed, 2 errors in 0.1s ==\n"
        self.assertEqual(pv.pytest_output_problems(out), [])


class SummarizeWiringTests(unittest.TestCase):
    def _summ(self, rc, output):
        with mock.patch.object(workbench, "_log_path", return_value=os.devnull):
            return workbench._summarize("t", ["pytest"], rc, output, 1.0,
                                        output_check=pv.pytest_output_problems)

    def test_exit_zero_node_down_reported_failed(self):
        text = self._summ(0, NODE_DOWN)
        self.assertIn("FAILED", text.splitlines()[0])
        self.assertIn("OUTPUT CHECK FAILED", text)

    def test_exit_zero_clean_reported_ok(self):
        self.assertIn(": OK in", self._summ(0, CLEAN))

    def test_run_pctools_tests_uses_timeout_and_check(self):
        with mock.patch.object(workbench, "_run_locked", return_value="x") as m:
            workbench.run_pctools_tests("foo")
        argv = m.call_args.args[2]
        self.assertIn(f"--timeout={pv.PER_TEST_TIMEOUT_S}", argv)
        self.assertGreaterEqual(pv.PER_TEST_TIMEOUT_S, 120)
        self.assertLess(pv.PER_TEST_TIMEOUT_S, 600)
        self.assertNotIn("-q", argv)
        self.assertIs(m.call_args.kwargs["output_check"], pv.pytest_output_problems)


if __name__ == "__main__":
    unittest.main()
