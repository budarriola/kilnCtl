#!/usr/bin/env python3
"""Schema + honesty-gate validation for the generated PID tuning-method
recommendation artifact (tools/PcTools/config_presets/tuning_recommendations.json),
produced by kilnctrl.tuning_campaign from the empty-kiln x peak-temp x load
simulation campaign.

The artifact is consumed by another agent's web GUI page; this test is the
contract that keeps it small, flat, and honest about confidence -- in
particular that a row above EXTRAPOLATION_BOUNDARY_C can never claim
"measured" (see tuning_campaign.py's HONESTY GATE section).

Run with: python -m pytest tools/PcTools/tests/test_tuning_recommendations.py -q
"""
from __future__ import annotations

import json
import os
import unittest

_PATH = os.path.join(os.path.dirname(__file__), "..", "config_presets", "tuning_recommendations.json")

_VALID_CONFIDENCE = {"measured", "extrapolated", "indistinguishable"}
_VALID_METHODS = {"simc", "cohen_coon", "ziegler_nichols", "tyreus_luyben", "none"}


def _load():
    with open(_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


class ArtifactExistsTest(unittest.TestCase):
    def test_artifact_file_exists(self):
        self.assertTrue(os.path.isfile(_PATH), _PATH)


class SchemaTest(unittest.TestCase):
    def setUp(self):
        self.data = _load()

    def test_top_level_keys(self):
        for key in ("schema_version", "generated_from", "sim_confidence", "recommendations", "caveats"):
            self.assertIn(key, self.data)

    def test_schema_version_is_1(self):
        self.assertEqual(self.data["schema_version"], 1)

    def test_generated_from_is_nonempty_string(self):
        self.assertIsInstance(self.data["generated_from"], str)
        self.assertGreater(len(self.data["generated_from"]), 0)

    def test_sim_confidence_shape(self):
        sc = self.data["sim_confidence"]
        self.assertEqual(len(sc["held_out_rms_c"]), 3)
        self.assertEqual(len(sc["discrimination_threshold_c"]), 3)
        self.assertIsInstance(sc["extrapolation_boundary_c"], (int, float))

    def test_recommendations_is_nonempty_list(self):
        recs = self.data["recommendations"]
        self.assertIsInstance(recs, list)
        self.assertGreater(len(recs), 0)

    def test_recommendation_fields(self):
        required = {"peak_temp_c_max", "load", "method", "rule", "confidence", "why", "runner_up", "margin_c"}
        for rec in self.data["recommendations"]:
            self.assertTrue(required.issubset(rec.keys()), rec.keys())

    def test_recommendation_field_types(self):
        for rec in self.data["recommendations"]:
            self.assertIsInstance(rec["peak_temp_c_max"], (int, float))
            self.assertIsInstance(rec["load"], str)
            self.assertIn(rec["method"], _VALID_METHODS)
            self.assertIsInstance(rec["rule"], str)
            self.assertIn(rec["confidence"], _VALID_CONFIDENCE)
            self.assertIsInstance(rec["why"], str)
            self.assertGreater(len(rec["why"]), 0)
            self.assertTrue(rec["runner_up"] is None or isinstance(rec["runner_up"], str))
            self.assertTrue(rec["margin_c"] is None or isinstance(rec["margin_c"], (int, float)))

    def test_caveats_is_list_of_strings(self):
        caveats = self.data["caveats"]
        self.assertIsInstance(caveats, list)
        self.assertGreater(len(caveats), 0)
        for c in caveats:
            self.assertIsInstance(c, str)

    def test_artifact_is_small_and_flat(self):
        """Must be embeddable in firmware flash -- keep it well under any
        reasonable flash-embed budget and free of deep nesting."""
        raw = json.dumps(self.data)
        self.assertLess(len(raw), 20_000, "artifact grew unexpectedly large")
        for rec in self.data["recommendations"]:
            for v in rec.values():
                self.assertNotIsInstance(v, (dict, list))


class HonestyGateTest(unittest.TestCase):
    """The mandatory gate: a recommendation whose peak_temp_c_max is above
    the artifact's own extrapolation_boundary_c can never be confidence
    'measured' -- every parameter up there is ASSUMED, not measured (see
    tuning_campaign.py's HONESTY GATE docstring section and the owner's
    z0/z1/z2 held-out RMS figures)."""

    def setUp(self):
        self.data = _load()
        self.boundary = self.data["sim_confidence"]["extrapolation_boundary_c"]

    def test_no_measured_claim_above_extrapolation_boundary(self):
        for rec in self.data["recommendations"]:
            if rec["peak_temp_c_max"] > self.boundary:
                self.assertNotEqual(
                    rec["confidence"], "measured",
                    f"peak_temp_c_max={rec['peak_temp_c_max']} exceeds the "
                    f"{self.boundary} C extrapolation boundary but claims 'measured'")

    def test_at_least_one_row_covers_the_measured_regime(self):
        """Sanity check the campaign actually produced a bench-scale point
        within the calibrated range -- otherwise the honesty-gate test above
        would pass vacuously (nothing ever claims 'measured' because
        nothing COULD)."""
        self.assertTrue(any(rec["peak_temp_c_max"] <= self.boundary for rec in self.data["recommendations"]))

    def test_indistinguishable_rows_carry_a_margin_below_threshold(self):
        min_threshold = min(self.data["sim_confidence"]["discrimination_threshold_c"])
        for rec in self.data["recommendations"]:
            if rec["confidence"] == "indistinguishable" and rec["margin_c"] is not None:
                self.assertLess(rec["margin_c"], min_threshold + 1e-9)


if __name__ == "__main__":
    unittest.main()
