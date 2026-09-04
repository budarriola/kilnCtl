#!/usr/bin/env python3
"""Tests for tuning_campaign.py's synthetic-plant measurement quantization
(B10 -- B11 ROADMAP.md M15 item).

``run_step_test``/``run_relay_test`` used to feed the fitter/relay-cycle
detector raw continuous floats straight off the plant model. The real
MAX31856 quantizes every reading to plant_sim.MAX31856_QUANTUM_C (1/128 C);
unquantized synthetic data can hide whole branches a real, quantized
measurement would exercise, per the "Idealized test input bug class"
project history. quantize=True (the default) now rounds every measured
temperature to that quantum; quantize=False is an escape hatch back to the
old continuous behaviour.

Run with: python -m pytest tools/PcTools/tests/test_tuning_campaign_quantize.py
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import plant_sim, tuning_campaign as tcamp


class QuantizeHelperTests(unittest.TestCase):
    def test_quantize_snaps_to_multiple_of_quantum(self):
        q = plant_sim.MAX31856_QUANTUM_C
        for raw in (0.0, 1.0, 23.456, 999.999, -5.0):
            snapped = tcamp._quantize(raw)
            # exact multiple of the quantum, within float rounding.
            ratio = snapped / q
            self.assertAlmostEqual(ratio, round(ratio), places=6)

    def test_quantize_is_idempotent(self):
        q = plant_sim.MAX31856_QUANTUM_C
        once = tcamp._quantize(37.61)
        twice = tcamp._quantize(once)
        self.assertAlmostEqual(once, twice, places=9)
        self.assertAlmostEqual(once % q, 0.0, places=6)


class RunStepTestQuantizeTests(unittest.TestCase):
    """A short, cheap step test (not the full-duration default) just to
    inspect the emitted trace's quantization, not to get a valid fit."""

    def _short_trace(self, quantize: bool):
        plant = tcamp.make_plant(mass_mult=1.0, regime="physical")
        import numpy as np
        duty = np.full(tcamp.N_ZONES, 0.2)
        n = 50
        y = np.empty(n)
        q = plant_sim.MAX31856_QUANTUM_C
        for i in range(n):
            temps = plant.step(duty)
            y[i] = tcamp._quantize(temps[0]) if quantize else temps[0]
        return y, q

    def test_quantized_trace_values_are_exact_multiples(self):
        y, q = self._short_trace(quantize=True)
        for v in y:
            ratio = v / q
            self.assertAlmostEqual(ratio, round(ratio), places=6)

    def test_unquantized_trace_has_a_non_multiple_value(self):
        y, q = self._short_trace(quantize=False)
        # The plant is continuous; at least one sample should land off the
        # quantum grid when quantize=False actually bypasses rounding.
        off_grid = any(abs((v / q) - round(v / q)) > 1e-6 for v in y)
        self.assertTrue(off_grid, "expected at least one unquantized sample off the quantum grid")

    def test_run_step_test_quantize_default_true(self):
        import inspect
        sig = inspect.signature(tcamp.run_step_test)
        self.assertIn("quantize", sig.parameters)
        self.assertTrue(sig.parameters["quantize"].default)

    def test_run_relay_test_quantize_default_true(self):
        import inspect
        sig = inspect.signature(tcamp.run_relay_test)
        self.assertIn("quantize", sig.parameters)
        self.assertTrue(sig.parameters["quantize"].default)


if __name__ == "__main__":
    unittest.main()
