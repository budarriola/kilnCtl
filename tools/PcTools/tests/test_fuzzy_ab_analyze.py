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

from kilnctrl import pid_ab_compare as ab  # noqa: E402

_SCRIPT_PATH = os.path.join(_REPO_ROOT, "tools", "PcTools", "scripts", "fuzzy_ab_analyze.py")
_spec = importlib.util.spec_from_file_location("fuzzy_ab_analyze", _SCRIPT_PATH)
faa = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(faa)  # type: ignore[union-attr]


def _control_body(bd_by_zone: dict) -> dict:
    return {"zones": [dict(bd, zone=z) for z, bd in bd_by_zone.items()]}


def _exec_body(target_c: float, actual_by_zone: dict, dwelling: bool = True, state: str = "running") -> dict:
    return {
        "state": state, "profile_id": 7, "segment_index": 0, "segment_count": 1,
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


def _write_capture_bd_by_zone(path: str, n: int, kp_by_zone: dict, actual_seed: float,
                                final_state: str = "done") -> None:
    """Like :func:`_write_capture` but with an explicit per-zone
    ``bd_kp_effective``, so a PARTIALLY INERT pair can be built: some zones'
    fuzzy term differing between arms, others bit-identical."""
    with open(path, "w", encoding="utf-8") as fh:
        for i in range(n):
            bd = {z: {"bd_kp_effective": kp_by_zone[z],
                      "bd_ki_effective": 0.0001, "bd_kd_effective": 0.5}
                  for z in kp_by_zone}
            actual = {z: actual_seed + z * 0.1 + i * 0.01 for z in kp_by_zone}
            state = final_state if i == n - 1 else "running"
            row = {"t": 1000.0 + i, "exec": _exec_body(40.0, actual, state=state),
                   "control": _control_body(bd)}
            fh.write(json.dumps(row) + "\n")


def _write_capture(path: str, n: int, bd_seed: float, actual_seed: float,
                    final_state: str = "done") -> None:
    """Writes ``n`` rows, all ``state="running"`` except the LAST, which is
    ``final_state`` (default ``"done"`` -- a genuinely complete arm, so this
    helper's callers pass the completeness gate (pid_ab_compare.
    capture_completeness) without having to think about it). Pass
    ``final_state="running"`` to build a deliberately SHORT/TRUNCATED
    capture instead -- see the completeness negative test below."""
    with open(path, "w", encoding="utf-8") as fh:
        for i in range(n):
            bd = {z: {"bd_kp_effective": bd_seed + z * 0.01 + i * 0.0001,
                      "bd_ki_effective": 0.0001, "bd_kd_effective": 0.5}
                  for z in range(3)}
            actual = {z: actual_seed + z * 0.1 + i * 0.01 for z in range(3)}
            state = final_state if i == n - 1 else "running"
            row = {"t": 1000.0 + i, "exec": _exec_body(40.0, actual, state=state),
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


class TruncatedArmNegativeTest(unittest.TestCase):
    """MANDATORY NEGATIVE TEST for the short/truncated-arm gate
    (pid_ab_compare.capture_completeness). A campaign killed mid-arm leaves
    behind a capture file that EXISTS and PARSES -- the file-existence check
    this script already had sees nothing wrong -- but never reached a
    terminal exec.state. Comparing it against a complete sibling arm must be
    reported and REFUSED, exactly like an INERT pair, not silently averaged
    into a verdict."""

    def test_truncated_arm_reported_void_and_excluded_not_analyzed(self):
        with tempfile.TemporaryDirectory() as d:
            a_path, b_path = faa.arm_paths(d, "campaign", 1)
            # A: genuinely complete (last row state="done").
            _write_capture(a_path, 5, bd_seed=0.02, actual_seed=39.0, final_state="done")
            # B: SHORT/TRUNCATED -- last row still "running", as if the
            # runner/board died mid-arm. THE DANGEROUS ASYMMETRIC CASE: one
            # side is complete, so this pair looks superficially valid.
            _write_capture(b_path, 3, bd_seed=0.05, actual_seed=39.0, final_state="running")

            report = faa.analyze(d, "campaign", 1)

            self.assertFalse(report["any_pair_inert"])
            self.assertTrue(report["any_pair_truncated"])
            pair0 = report["pairs"][0]
            self.assertIn("VOID", pair0["status"])
            self.assertIn("short/truncated", pair0["status"])
            self.assertIn("EXCLUDED", pair0["status"])
            # Must NOT have proceeded to a tracking-error comparison for
            # this pair -- it was never averaged in.
            self.assertNotIn("compare_text", pair0)
            self.assertIn("incomplete_arms", pair0)
            # The asymmetric danger (one complete, one truncated) must be
            # named explicitly, not left implicit.
            self.assertIn("DANGEROUS ASYMMETRIC", pair0["compare_error"])
            self.assertIn("SHORT/TRUNCATED", pair0["compare_error"])
            # The overall campaign verdict must call out the exclusion too.
            self.assertIn("EXCLUDED", report["verdict"])

    def test_both_arms_complete_are_not_falsely_flagged_truncated(self):
        """Sanity companion: two genuinely complete arms must be analyzed
        normally -- proves the gate isn't just always failing closed."""
        with tempfile.TemporaryDirectory() as d:
            a_path, b_path = faa.arm_paths(d, "campaign", 1)
            _write_capture(a_path, 5, bd_seed=0.02, actual_seed=39.0, final_state="done")
            _write_capture(b_path, 5, bd_seed=0.05, actual_seed=39.0, final_state="done")

            report = faa.analyze(d, "campaign", 1)

            self.assertFalse(report["any_pair_truncated"])
            self.assertEqual(report["pairs"][0]["status"], "analyzed")


class PartiallyInertZonesTest(unittest.TestCase):
    """MANDATORY NEGATIVE TEST for the per-zone reachability gate.

    THE DEFECT THIS PINS (2026-09-04): reachability used to be a single
    pair-wide boolean, ``any(not bit_identical)`` over the flat verdict
    list. A pair in which the fuzzy term demonstrably acted in ONE zone but
    was EXACTLY bit-identical -- provably inert -- in the other two was
    therefore reported REACHABLE, analyzed normally, and all THREE zones
    fed the project's ">= 3 zones, same metric, same direction" decision
    rule. Two of the three votes would have come from zones where the
    treatment provably never ran: a confident campaign verdict from data
    that cannot support it. Inert zones must cast no vote, and the
    exclusion must be stated in the verdict, not left implicit."""

    def _partially_inert_report(self):
        d = self._d
        a_path, b_path = faa.arm_paths(d, "campaign", 1)
        # zone 0's fuzzy term differs; zones 1 and 2 are bit-identical.
        _write_capture_bd_by_zone(a_path, 6, {0: 0.02, 1: 0.03, 2: 0.04}, 39.0)
        _write_capture_bd_by_zone(b_path, 6, {0: 0.05, 1: 0.03, 2: 0.04}, 41.0)
        return faa.analyze(d, "campaign", 1)

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._d = self._tmp.name
        self.addCleanup(self._tmp.cleanup)

    def test_inert_zones_cast_no_vote_in_the_decision_rule(self):
        report = self._partially_inert_report()

        # The pair is not VOID -- zone 0 IS reachable, so its data is usable.
        self.assertEqual(report["pairs"][0]["status"], "analyzed")
        reach = report["pairs"][0]["reachability"]
        self.assertEqual(reach["reachable_zones"], [0])
        self.assertEqual(reach["inert_zones"], [1, 2])

        # THE POINT: only the reachable zone appears in the >=3-zone rule's
        # own vote table. Zones 1 and 2 must be absent entirely -- with the
        # old any()-based gate all three were present, and three same-
        # direction zones would have satisfied the rule outright.
        self.assertEqual(sorted(report["whole_run_iae_directions_by_zone"]), ["0"])
        self.assertEqual(report["pair_inert_zones"], {"1": [1, 2]})
        self.assertTrue(report["any_pair_partially_inert"])

        # And the exclusion is stated in the verdict, not left implicit.
        self.assertIn("PARTIALLY INERT", report["verdict"])
        self.assertIn("EXCLUDED", report["verdict"])

    def test_partial_inertness_is_named_in_the_text_report(self):
        text = faa.format_text(self._partially_inert_report())
        self.assertIn("PARTIALLY INERT", text)
        self.assertIn("[1, 2]", text)

    def test_fully_reachable_pair_keeps_every_zone(self):
        """Sanity companion: when every zone's fuzzy term differs, no zone is
        excluded -- proves the gate is not just always failing closed."""
        d = self._d
        a_path, b_path = faa.arm_paths(d, "campaign", 1)
        _write_capture_bd_by_zone(a_path, 6, {0: 0.02, 1: 0.03, 2: 0.04}, 39.0)
        _write_capture_bd_by_zone(b_path, 6, {0: 0.05, 1: 0.06, 2: 0.07}, 41.0)
        report = faa.analyze(d, "campaign", 1)
        self.assertEqual(report["pairs"][0]["reachability"]["inert_zones"], [])
        self.assertFalse(report["any_pair_partially_inert"])
        self.assertEqual(sorted(report["whole_run_iae_directions_by_zone"]), ["0", "1", "2"])
        self.assertNotIn("PARTIALLY INERT", report["verdict"])


class UnreplicatedDecisionRuleTest(unittest.TestCase):
    """The >=3-zone rule counts ZONES, and the three zones of ONE firing are
    not three independent experiments. A rule satisfied by a single analyzed
    pair is an unreplicated finding against an uncorrected family of keys
    (pid_ab_compare.summarize_multiplicity: ~30.5% per-key false-positive at
    n=3, ~12% at n=6). The campaign verdict must say so out loud instead of
    reading as a settled campaign-wide result."""

    def _run(self, n_pairs, stub_direction="B"):
        def fake_compare_runs(path_a, path_b, **kwargs):
            return {"comparisons": [
                ab.MetricComparison(zone=z, metric="iae_normalized_whole_c", segment=None,
                                     a=1.0, b=0.5, delta=-0.5, verdict="PROVISIONAL",
                                     distinguishable=True, better=stub_direction)
                for z in range(3)],
                "start_temp_deltas_c": {}}
        real_compare, real_fmt = ab.compare_runs, ab.format_compare_text
        ab.compare_runs = fake_compare_runs
        ab.format_compare_text = lambda r: "(stubbed)"
        try:
            with tempfile.TemporaryDirectory() as d:
                for i in range(1, n_pairs + 1):
                    a_path, b_path = faa.arm_paths(d, "campaign", i)
                    _write_capture(a_path, 5, bd_seed=0.02, actual_seed=39.0)
                    _write_capture(b_path, 5, bd_seed=0.05, actual_seed=39.0)
                return faa.analyze(d, "campaign", n_pairs)
        finally:
            ab.compare_runs, ab.format_compare_text = real_compare, real_fmt

    def test_single_pair_win_is_flagged_unreplicated(self):
        report = self._run(1)
        self.assertTrue(report["verdict"].startswith("DISTINGUISHABLE"))
        self.assertIn("UNREPLICATED", report["verdict"])
        self.assertIn("30.5%", report["verdict"])

    def test_two_pairs_agreeing_is_not_flagged_unreplicated(self):
        """Sanity companion -- the caveat is not unconditional boilerplate."""
        report = self._run(2)
        self.assertTrue(report["verdict"].startswith("DISTINGUISHABLE"))
        self.assertNotIn("UNREPLICATED", report["verdict"])


if __name__ == "__main__":
    unittest.main()
