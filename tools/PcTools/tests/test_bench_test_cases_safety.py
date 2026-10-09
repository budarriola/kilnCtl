#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_safety -- SP-03/SP-06, which
read another case's stashed-in-ctx before/after data rather than driving a
firing of their own.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_safety.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_safety as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402


class Sp03Test(unittest.TestCase):
    def test_not_run_when_hp02_absent(self):
        result = C._case_sp03({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_zero_deltas_pass(self):
        ctx = {"_hp02": {
            "link_stats_before": {"crc_errors": 1, "timeouts": 2, "broadcast_dropped": 0},
            "link_stats_after": {"crc_errors": 1, "timeouts": 2, "broadcast_dropped": 0},
        }}
        result = C._case_sp03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_nonzero_delta_fails(self):
        ctx = {"_hp02": {
            "link_stats_before": {"crc_errors": 1, "timeouts": 2, "broadcast_dropped": 0},
            "link_stats_after": {"crc_errors": 3, "timeouts": 2, "broadcast_dropped": 0},
        }}
        result = C._case_sp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Sp06Test(unittest.TestCase):
    def test_not_run_when_hp01_absent(self):
        result = C._case_sp06({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_energized_only_while_running_passes(self):
        ctx = {"_hp01": {"energized_samples": [("running", True), ("done", False)]}}
        result = C._case_sp06(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_energized_after_done_fails(self):
        ctx = {"_hp01": {"energized_samples": [("running", True), ("done", True)]}}
        result = C._case_sp06(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class RegistryWiringTest(unittest.TestCase):
    def test_sp03_depends_on_hp02(self):
        self.assertEqual(get_case("SP-03").depends_on, "HP-02")

    def test_sp06_depends_on_hp01(self):
        self.assertEqual(get_case("SP-06").depends_on, "HP-01")

    def test_judges_wired(self):
        self.assertIs(get_case("SP-03").judge, C._case_sp03)
        self.assertIs(get_case("SP-06").judge, C._case_sp06)


if __name__ == "__main__":
    unittest.main()
