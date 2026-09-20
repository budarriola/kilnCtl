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
        # ST/FL/SK-01,03,04/SP read-only block still precedes every WEB/LCD/
        # HP/OT case, exactly as in `full` -- OT-B01/OT-E* are the one
        # deliberate exception (plan §5.2: "OTA last"), so this checks the
        # ST/FL/SK/SP prefix specifically rather than "all read-only before
        # all heat" (no longer true in nightly: OT-B01/OT-E* are read-only
        # yet legitimately run after the heat-originating HP-* block).
        prefix_end = max(idx[c] for c in ids if R.get_case(c).area in ("ST", "FL", "SK", "SP") and c not in ("SK-02", "SP-03", "SP-06"))
        heat_ids = [c for c in ids if R.get_case(c).heat]
        first_non_prefix = min(i for cid, i in idx.items() if cid not in (
            "SK-02", "SP-03", "SP-06"
        ) and R.get_case(cid).area not in ("ST", "FL", "SK", "SP"))
        self.assertLess(prefix_end, first_non_prefix)
        if heat_ids:
            self.assertGreater(min(idx[c] for c in heat_ids), prefix_end)

    def test_nightly_hp_before_sk02_and_ota_last(self):
        """Plan §5.3 interdependence rules, the two named in the task:
        HP runs before SK-02 (SK-02 depends_on='HP-01' and needs real httpd
        traffic from a firing already in flight), and the OTA block
        (OT-B01/OT-E*) is the very last thing nightly runs."""
        ids = R.SUITES["nightly"]
        idx = {cid: i for i, cid in enumerate(ids)}
        self.assertLess(idx["HP-01"], idx["SK-02"])
        ot_ids = [c for c in ids if c.startswith("OT-")]
        self.assertTrue(ot_ids)
        non_ot_ids = [c for c in ids if not c.startswith("OT-")]
        self.assertLess(max(idx[c] for c in non_ot_ids), min(idx[c] for c in ot_ids))
        # SK-02's own dependency wiring, and each SP observer sits right
        # after the HP case it depends on (§5.2's "HP-01 (with ... SP-*
        # observers)" pattern).
        self.assertEqual(R.get_case("SK-02").depends_on, "HP-01")
        self.assertEqual(R.get_case("SP-06").depends_on, "HP-01")
        self.assertEqual(R.get_case("SP-03").depends_on, "HP-02")
        self.assertEqual(idx["SP-06"], idx["HP-01"] + 1)
        self.assertEqual(idx["SP-03"], idx["HP-02"] + 1)

    def test_nightly_matches_plan_5_1_membership(self):
        """Plan §5.1's nightly membership, spelled out explicitly so a future
        edit to either the plan or this list is forced to reconcile the two
        (check_bench_test_registry.ps1 checks ids-exist, not this specific
        set membership)."""
        ids = set(R.SUITES["nightly"])
        expected = set(R.SUITES["smoke"]) | {
            "ST-01", "ST-02", "ST-03", "ST-04",
            "HP-01", "HP-02", "HP-04", "HP-05", "HP-06", "HP-08",
            "SK-02", "SP-03", "SP-06",
            "WEB-DASH-03", "WEB-DASH-06", "WEB-DASH-07", "WEB-DASH-09",
            "WEB-PROF-02", "WEB-PROF-03", "WEB-PROF-04", "WEB-PROF-05",
            "WEB-PROF-06", "WEB-PROF-07", "WEB-PROF-08", "WEB-PROF-09",
            "WEB-ZONE-02", "WEB-ZONE-03", "WEB-ZONE-05", "WEB-ZONE-09", "WEB-ZONE-12",
            "WEB-BAK-02", "WEB-BAK-03",
            "WEB-KCFG-02", "WEB-KCFG-03",
            "WEB-DIAG-07", "WEB-DIAG-08",
            "WEB-OTA-01", "WEB-OTA-02",
            "WEB-SEC-03",
            "WEB-X-01", "WEB-X-02",
            "LCD-02", "LCD-03", "LCD-04", "LCD-09", "LCD-14", "LCD-16",
            "OT-B01", "OT-E01", "OT-E02", "OT-E03", "OT-E12",
        }
        self.assertEqual(ids, expected)
        self.assertEqual(len(R.SUITES["nightly"]), len(set(R.SUITES["nightly"])), "no duplicate ids in nightly")


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
