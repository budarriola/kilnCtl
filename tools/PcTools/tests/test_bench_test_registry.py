#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.registry -- the case catalog itself.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_registry.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import registry as R  # noqa: E402


class VerdictTest(unittest.TestCase):
    def test_case_result_rejects_unknown_verdict(self):
        with self.assertRaises(ValueError):
            R.CaseResult(verdict="MAYBE")

    def test_non_pass_verdict_requires_a_reason(self):
        with self.assertRaises(ValueError):
            R.CaseResult(verdict=R.Verdict.FAIL, reason="")

    def test_pass_verdict_needs_no_reason(self):
        r = R.CaseResult(verdict=R.Verdict.PASS)
        self.assertEqual(r.reason, "")


class RegistryLookupTest(unittest.TestCase):
    def test_known_case_ids_resolve(self):
        for cid in ("ST-05", "FL-01", "SK-01", "SP-01"):
            self.assertEqual(R.get_case(cid).id, cid)

    def test_unknown_case_id_raises_key_error(self):
        with self.assertRaises(KeyError):
            R.get_case("NOT-A-REAL-CASE")

    def test_register_rejects_a_duplicate_id(self):
        with self.assertRaises(ValueError):
            R.register(R.CaseSpec(id="ST-05", area="ST", description="dup"))


class SuiteTest(unittest.TestCase):
    def test_smoke_suite_ids_all_exist(self):
        ids = R.suite_case_ids("smoke")
        self.assertIn("ST-05", ids)
        self.assertIn("SP-07", ids)

    def test_unknown_suite_raises_key_error(self):
        with self.assertRaises(KeyError):
            R.suite_case_ids("not-a-real-suite")

    def test_smoke_cases_all_have_judge_functions_wired(self):
        """cases_smoke.py's bottom-of-module loop wires judge functions into
        REGISTRY at import time (bench_test/__init__.py imports it) -- a
        smoke case with judge=None would silently report NOT_RUN forever."""
        for cid in R.SUITES["smoke"]:
            self.assertIsNotNone(R.get_case(cid).judge, f"{cid} has no judge function wired")

    def test_full_suite_covers_every_registered_id(self):
        self.assertEqual(set(R.SUITES["full"]), set(R.REGISTRY.keys()))

    def test_web_and_lcd_suites_match_their_prefix(self):
        self.assertTrue(all(c.startswith("WEB-") for c in R.SUITES["web"]))
        self.assertTrue(all(c.startswith("LCD-") for c in R.SUITES["lcd"]))

    def test_full_suite_is_not_alphabetical_heat_cases_run_last(self):
        """plan §5.2 fixed order: ST -> FL (read-only) -> SK-01/03/04 ->
        SP (read-only) -> everything else -> AT-*/HP-* (heat) last. A plain
        alphabetical sort would run AT-* before SK-*/SP-*/ST-*, which is
        exactly what this order must avoid."""
        ids = R.SUITES["full"]
        self.assertNotEqual(ids, sorted(ids), "full suite must not be plain-alphabetical")
        idx = {cid: i for i, cid in enumerate(ids)}
        self.assertLess(idx["ST-05"], idx["FL-01"])
        self.assertLess(idx["FL-01"], idx["SK-01"])
        self.assertLess(idx["SK-01"], idx["SP-01"])
        last_read_only = max(idx[c] for c in ids if not R.get_case(c).heat)
        first_heat = min(idx[c] for c in ids if R.get_case(c).heat)
        self.assertLess(last_read_only, first_heat, "every heat case (AT-*/HP-*) must run after every read-only case")

    def test_nightly_suite_also_uses_the_fixed_order(self):
        ids = R.SUITES["nightly"]
        idx = {cid: i for i, cid in enumerate(ids)}
        self.assertLess(idx["ST-05"], idx["FL-01"])
        heat_ids = [c for c in ids if R.get_case(c).heat]
        if heat_ids:
            read_only_ids = [c for c in ids if not R.get_case(c).heat]
            self.assertLess(max(idx[c] for c in read_only_ids), min(idx[c] for c in heat_ids))


class HeatFlagTest(unittest.TestCase):
    def test_all_hp_cases_are_flagged_heat(self):
        for cid in R.SUITES["heat"]:
            self.assertTrue(R.get_case(cid).heat, f"{cid} should be heat=True")

    def test_smoke_suite_has_no_heat_cases(self):
        """Wave 0 rule: no smoke case may heat, flash, write config, or
        touch Wi-Fi."""
        for cid in R.SUITES["smoke"]:
            self.assertFalse(R.get_case(cid).heat, f"{cid} must not be heat=True in the smoke suite")


if __name__ == "__main__":
    unittest.main()
