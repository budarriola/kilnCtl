"""Unit tests for tools/PcTools/scripts/fuzzy_ab_analyze.py -- the single
entry point for the fuzzy-PID A/B campaign's conclusion (bd_reachability_check
FIRST, then tracking-error comparison, then the >=3-zone decision rule).

Covers: (1) an incomplete campaign (missing arm files) is reported gracefully,
never crashes, and yields exit code 3 with no fabricated verdict; (2) a
bit-identical (INERT) pair is reported VOID and the script refuses to
proceed to a tracking-error conclusion -- the MANDATORY NEGATIVE TEST induces
exactly this failure mode (two arms built from the same source data, so their
bd_* fields are bit-identical by construction) and confirms the refusal names
specifics.

Run with: python -m pytest tools/PcTools/tests/test_fuzzy_ab_analyze.py -q
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_SRC = os.path.join(_REPO_ROOT, "tools", "PcTools", "src")
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

_SCRIPT_PATH = os.path.join(_REPO_ROOT, "tools", "PcTools", "scripts", "fuzzy_ab_analyze.py")
_spec = importlib.util.spec_from_file_location("fuzzy_ab_analyze", _SCRIPT_PATH)
faa = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(faa)  # type: ignore[union-attr]


def _control_body(bd_by_zone: dict) -> dict:
    return {"zones": [dict(bd, zone=z) for z, bd in bd_by_zone.items()]}


def _exec_body(target_c: float, actual_by_zone: dict, dwelling: bool = True) -> dict:
    return {
        "state": "running", "profile_id": 7, "segment_index": 0, "segment_count": 1,
        "dwelling": dwelling, "target_c": target_c,
        "zones": [
            {"zone": z, "actual_c": a, "actual_valid": True,
             "firing_stats": {"mean_error_c": a - target_c, "iae_raw_c_s": 0.0,
                               "iae_normalized": 0.1, "sample_count": 1,
                               "excluded_sample_count": 0, "duration_s": 1,
                               "ramp_err_mean_c": 0.0, "ramp_err_max_c": 0.0,
                               "dwell_err_mean_c": 0.0, "dwell_err_max_c": 0.0,
                               "max_overshoot_c": 0.0, "max_overshoot_elapsed_s": 0,
                               "max_overshoot_segment": 0, "max_undershoot_c": 0.0,
                               "max_undershoot_elapsed_s": 0, "max_undershoot_segment": 0}}
            for z, a in actual_by_zone.items()
        ],
    }


def _write_capture(path: str, n: int, bd_seed: float, actual_seed: float) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        for i in range(n):
            bd = {z: {"bd_kp_effective": bd_seed + z * 0.01 + i * 0.0001,
                      "bd_ki_effective": 0.0001, "bd_kd_effective": 0.5}
                  for z in range(3)}
            actual = {z: actual_seed + z * 0.1 + i * 0.01 for z in range(3)}
            row = {"t": 1000.0 + i, "exec": _exec_body(40.0, actual),
                   "control": _control_body(bd)}
            fh.write(json.dumps(row) + "\n")


class IncompleteCampaignTest(unittest.TestCase):
    def test_missing_arms_reported_gracefully_not_fabricated(self):
        with tempfile.TemporaryDirectory() as d:
            # No files written at all -- the campaign hasn't started yet.
            report = faa.analyze(d, "campaign", 3)
            self.assertEqual(report["verdict"], "INCOMPLETE -- no complete pairs yet")
            self.assertEqual(len(report["pairs"]), 3)
            for p in report["pairs"]:
                self.assertEqual(p["status"], "not yet available")
                self.assertTrue(p["missing"])

    def test_partial_pair_one_file_present(self):
        with tempfile.TemporaryDirectory() as d:
            a_path, b_path = faa.arm_paths(d, "campaign", 1)
            _write_capture(b_path, 5, bd_seed=0.02, actual_seed=39.0)
            report = faa.analyze(d, "campaign", 1)
            self.assertEqual(report["pairs"][0]["status"], "not yet available")
            self.assertIn(a_path, report["pairs"][0]["missing"])
            self.assertEqual(report["verdict"], "INCOMPLETE -- no complete pairs yet")


class InertPairNegativeTest(unittest.TestCase):
    """MANDATORY NEGATIVE TEST: a pair whose bd_* fields are bit-identical
    between arms (built from literally the same generator/seed) must be
    reported VOID/INERT, must refuse to proceed to a tracking-error verdict,
    and the refusal must name the specific inert fields -- not a generic
    failure."""

    def test_bit_identical_arms_reported_void_not_analyzed(self):
        with tempfile.TemporaryDirectory() as d:
            a_path, b_path = faa.arm_paths(d, "campaign", 1)
            # Same seed for both -> bit-identical bd_* by construction.
            _write_capture(a_path, 5, bd_seed=0.02, actual_seed=39.0)
            _write_capture(b_path, 5, bd_seed=0.02, actual_seed=39.0)

            report = faa.analyze(d, "campaign", 1)

            self.assertTrue(report["any_pair_inert"])
            pair0 = report["pairs"][0]
            self.assertIn("VOID", pair0["status"])
            self.assertIn("INERT", pair0["status"])
            # The reachability report must name specific fields/zones as
            # bit-identical, not just assert "inert" generically.
            self.assertIn("bd_kp_effective", pair0["reachability"]["report"])
            self.assertIn("bit-identical", pair0["reachability"]["report"])
            # Must NOT have proceeded to a tracking-error comparison for
            # this pair.
            self.assertNotIn("compare_text", pair0)
            self.assertTrue(report["verdict"].startswith("VOID"))

    def test_genuinely_differing_arms_are_not_falsely_flagged_inert(self):
        """Sanity companion to the negative test: arms with DIFFERENT bd_*
        seeds must NOT be reported inert (proves the check can also say
        REACHABLE, i.e. it isn't just always failing closed)."""
        with tempfile.TemporaryDirectory() as d:
            a_path, b_path = faa.arm_paths(d, "campaign", 1)
            _write_capture(a_path, 5, bd_seed=0.02, actual_seed=39.0)
            _write_capture(b_path, 5, bd_seed=0.05, actual_seed=39.0)

            report = faa.analyze(d, "campaign", 1)

            self.assertFalse(report["any_pair_inert"])
            self.assertEqual(report["pairs"][0]["status"], "analyzed")


if __name__ == "__main__":
    unittest.main()
