#!/usr/bin/env python3
"""Mutation-tested sanity checks for ``kilnctrl.fuzzy_load_sweep`` (owner
request 2026-09-02: cross the fuzzy-strength sweep, sec 3.6, with the load
sweep, sec 3.8 -- neither alone answers whether the fuzzy layer helps at a
load it was never tested against).

These check the plumbing is not a no-op and the honesty-gate arithmetic
matches the rule the task specifies (refuse a winner whose margin over
strength=0 does not exceed BOTH the seed-to-seed std and the zone's
hardware discrimination threshold), not the absolute IAE numbers -- those
come from the ASSUMED load model already sanity-checked in
test_load_mass_sweep.py.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import fuzzy_load_sweep as fls  # noqa: E402
from kilnctrl import load_mass_sweep as lms  # noqa: E402


class FuzzyLoadLoopFidelityTests(unittest.TestCase):
    def test_strength_zero_noise_free_matches_load_mass_sweep_bit_for_bit(self):
        """fuzzy_strength_pct=0 with the measurement chain disabled must
        reproduce load_mass_sweep._run_loop's own trajectory exactly -- this
        module's per-tick loop is a third copy of the same coupled-ff/PID
        loop (module docstring), and this pins that copy against the
        original rather than trusting it by inspection."""
        import numpy as np

        a = fls.run_profile7_fuzzy_loaded(
            1.5, 0.7, fuzzy_strength_pct=0.0, measurement_seed=0,
            measurement_quantum_c=0.0, measurement_noise_std_c=0.0)
        b = lms.run_profile7_loaded(1.5, 0.7)
        self.assertTrue(np.array_equal(a["temps"], b["temps"]))
        self.assertTrue(np.array_equal(a["duty"], b["duty"]))

    def test_fuzzy_strength_is_not_a_noop_under_load(self):
        """strength=100 must move the trajectory relative to strength=0 at
        a loaded condition -- if this ever goes green with an unmodified
        module, the fuzzy wiring silently stopped doing anything under
        load."""
        import numpy as np

        a = fls.run_profile7_fuzzy_loaded(2.0, 0.7, fuzzy_strength_pct=0.0, measurement_seed=0)
        b = fls.run_profile7_fuzzy_loaded(2.0, 0.7, fuzzy_strength_pct=100.0, measurement_seed=0)
        self.assertFalse(np.array_equal(a["temps"], b["temps"]))

    def test_load_axis_is_not_a_noop_at_fixed_strength(self):
        """Same check as test_load_mass_sweep's core claim, run through
        THIS module's loop (which load_mass_sweep's own test does not
        exercise): heavier load must worsen whole-run IAE at a fixed fuzzy
        strength, matching the physically expected direction."""
        light = fls.run_profile7_fuzzy_loaded(1.0, 1.0, fuzzy_strength_pct=0.0, measurement_seed=0)
        heavy = fls.run_profile7_fuzzy_loaded(4.0, 1.0, fuzzy_strength_pct=0.0, measurement_seed=0)
        light_iae = max(m.iae_whole_c for m in fls.compute_metrics(light))
        heavy_iae = max(m.iae_whole_c for m in fls.compute_metrics(heavy))
        self.assertGreater(heavy_iae, light_iae + 1.0,
                            f"expected 4x load to worsen whole-run IAE vs 1x "
                            f"(got {light_iae:.2f}C -> {heavy_iae:.2f}C)")


class HonestyGateTests(unittest.TestCase):
    """Directly test find_best_strength_per_load's arithmetic against
    synthetic summaries -- isolates the gate logic from the (slow, and
    numerically noisy) simulator runs above."""

    def test_refuses_winner_inside_discrimination_threshold(self):
        # z0 threshold is 2.1C; a 0.5C improvement must NOT clear the gate.
        summary = {
            (1.0, 1.0, 0.0, 0): dict(mean_iae_c=1.0, seed_std_c=0.01, n=5),
            (1.0, 1.0, 50.0, 0): dict(mean_iae_c=0.5, seed_std_c=0.01, n=5),
        }
        best = fls.find_best_strength_per_load(summary)
        self.assertFalse(best[(1.0, 1.0, 0)]["clears_honesty_gate"])

    def test_refuses_winner_inside_seed_spread(self):
        # z0 threshold (2.1C) cleared by the 2.5C margin, but seed std (3.0)
        # is larger than the margin, so the spread bound must still refuse.
        summary = {
            (1.0, 1.0, 0.0, 0): dict(mean_iae_c=5.0, seed_std_c=3.0, n=5),
            (1.0, 1.0, 50.0, 0): dict(mean_iae_c=2.5, seed_std_c=3.0, n=5),
        }
        best = fls.find_best_strength_per_load(summary)
        self.assertFalse(best[(1.0, 1.0, 0)]["clears_honesty_gate"])

    def test_accepts_winner_clearing_both_bounds(self):
        # z0 threshold 2.1C: a 5C margin with tiny seed std must clear.
        summary = {
            (1.0, 1.0, 0.0, 0): dict(mean_iae_c=10.0, seed_std_c=0.01, n=5),
            (1.0, 1.0, 50.0, 0): dict(mean_iae_c=5.0, seed_std_c=0.01, n=5),
        }
        best = fls.find_best_strength_per_load(summary)
        self.assertTrue(best[(1.0, 1.0, 0)]["clears_honesty_gate"])
        self.assertEqual(best[(1.0, 1.0, 0)]["best_strength"], 50.0)

    def test_strength_zero_never_reported_as_its_own_winner(self):
        # strength=0 scoring best must not spuriously mark clears_honesty_gate True.
        summary = {
            (1.0, 1.0, 0.0, 0): dict(mean_iae_c=1.0, seed_std_c=0.001, n=5),
            (1.0, 1.0, 50.0, 0): dict(mean_iae_c=2.0, seed_std_c=0.001, n=5),
        }
        best = fls.find_best_strength_per_load(summary)
        self.assertFalse(best[(1.0, 1.0, 0)]["clears_honesty_gate"])
        self.assertEqual(best[(1.0, 1.0, 0)]["best_strength"], 0.0)


if __name__ == "__main__":
    unittest.main()
