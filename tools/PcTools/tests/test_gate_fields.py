"""Unit tests for kilnctrl.gate_fields -- the authoring-time check that a
preset tuning a gated feature (e.g. fuzzy_strength_pct) also pins that
feature's gate (control_mode) to a reachable value in the same zone.

THE INCIDENT THIS EXISTS FOR: two campaigns ran for hours at
control_mode: 2 while the operator believed the fuzzy layer was active,
because fuzzy_strength_pct (a tuning field) was checked instead of
control_mode (the gate). check_preset_gate_consistency is the authoring-time
guard that would have refused such a preset before any kiln hour was spent.

MANDATORY NEGATIVE TEST: feeds the exact historical shape (fuzzy_strength_pct
set, control_mode pinned to 2, i.e. NOT the reachable value) and confirms it
is flagged; then breaks the check itself (reachable_when made permissive) and
confirms it goes quiet, proving the assertion is load-bearing and not
vacuous; then reverts and confirms a clean preset (control_mode: 3) passes.

Run with: python -m pytest tools/PcTools/tests/test_gate_fields.py -q
"""
from __future__ import annotations

import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import gate_fields as gf  # noqa: E402


def _zone(idx, **overrides):
    z = {"index": idx, "control_mode": 3, "fuzzy_strength_pct": 50.0}
    z.update(overrides)
    return z


class TestGateFieldsRegistry(unittest.TestCase):
    def test_find_gate_known(self):
        self.assertIsNotNone(gf.find_gate("control_mode"))
        self.assertIsNone(gf.find_gate("no_such_field"))

    def test_control_mode_reachable_predicate(self):
        gate = gf.find_gate("control_mode")
        self.assertTrue(gate.reachable_when(3))
        self.assertFalse(gate.reachable_when(2))
        self.assertFalse(gate.reachable_when(None))


class TestCheckPresetGateConsistency(unittest.TestCase):
    def test_clean_preset_mode3_passes(self):
        """The historical GOOD case: fuzzy_ab_strength50-shaped preset with
        control_mode: 3 pinned in every zone that sets fuzzy_strength_pct."""
        preset = {"zones": [_zone(0), _zone(1), _zone(2)]}
        self.assertEqual(gf.check_preset_gate_consistency(preset), [])

    def test_inert_preset_mode2_is_flagged(self):
        """THE ACTUAL HISTORICAL FAILURE: fuzzy_strength_pct set, but
        control_mode pinned to 2 -- the fuzzy layer is UNREACHABLE. This
        preset is exactly the shape that ran for hours while the operator
        believed the fuzzy layer was active."""
        preset = {"zones": [_zone(0, control_mode=2), _zone(1), _zone(2)]}
        problems = gf.check_preset_gate_consistency(preset)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("zone 0", problems[0])
        self.assertIn("control_mode", problems[0])
        self.assertIn("fuzzy_strength_pct", problems[0])

    def test_missing_gate_field_entirely_is_flagged(self):
        """fuzzy_strength_pct set, control_mode never mentioned at all --
        must also be flagged, not silently treated as "fine" because there
        is no explicit wrong value to compare against."""
        zone = _zone(0)
        del zone["control_mode"]
        preset = {"zones": [zone]}
        problems = gf.check_preset_gate_consistency(preset)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("never pins", problems[0])

    def test_preset_that_touches_neither_field_is_not_flagged(self):
        """A preset that sets neither control_mode nor fuzzy_strength_pct
        for a zone doesn't touch this feature at all -- not this incident's
        shape, must not be flagged."""
        preset = {"zones": [{"index": 0, "pid_kp": 0.05}]}
        self.assertEqual(gf.check_preset_gate_consistency(preset), [])

    def test_negative_control_check_can_be_made_to_miss(self):
        """MANDATORY NEGATIVE TEST. Prove the check is not vacuous: monkeypatch
        the control_mode gate's reachable_when to accept everything (as a
        broken "== 3 or == 2, both fine" predicate might), confirm the known-bad
        preset (mode 2 + fuzzy_strength_pct) then goes UNDETECTED, and revert."""
        preset = {"zones": [_zone(0, control_mode=2)]}
        # Sanity: detected under the real registry.
        self.assertEqual(len(gf.check_preset_gate_consistency(preset)), 1)

        original = gf.GATE_FIELDS
        try:
            broken = tuple(
                gf.dataclasses.replace(g, reachable_when=lambda v: True)
                if g.name == "control_mode" else g
                for g in original
            )
            gf.GATE_FIELDS = broken
            problems = gf.check_preset_gate_consistency(preset)
            self.assertEqual(
                problems, [],
                "broken permissive reachable_when should have gone silent, but the check "
                "still flagged something -- the negative test itself is not exercising the "
                "code path it claims to")
        finally:
            gf.GATE_FIELDS = original

        # Revert confirmed: back to detecting it.
        self.assertEqual(len(gf.check_preset_gate_consistency(preset)), 1)


class TestSummarizePresetGates(unittest.TestCase):
    def test_records_only_gates_the_preset_sets(self):
        preset = {"zones": [_zone(0)], "ramp_assist_enabled": False}
        summary = gf.summarize_preset_gates(preset)
        self.assertIn("control_mode", summary)
        self.assertIn("fuzzy_strength_pct", summary)
        self.assertIn("ramp_assist_enabled", summary)
        self.assertNotIn("ct_installed", summary)  # preset never mentions it

    def test_reachable_flag_matches_predicate(self):
        preset = {"zones": [_zone(0, control_mode=2)]}
        summary = gf.summarize_preset_gates(preset)
        self.assertFalse(summary["control_mode"]["values"][0]["reachable"])

    def test_top_level_gate_recorded_under_top_key(self):
        preset = {"zones": [], "ramp_assist_enabled": True}
        summary = gf.summarize_preset_gates(preset)
        self.assertEqual(summary["ramp_assist_enabled"]["values"]["top"]["value"], True)
        self.assertTrue(summary["ramp_assist_enabled"]["values"]["top"]["reachable"])


if __name__ == "__main__":
    unittest.main()
