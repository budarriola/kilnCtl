#!/usr/bin/env python3
"""Mutation-tested sanity checks for ``kilnctrl.load_mass_sweep`` (owner
request 2026-09-02: does the current PID tuning / adopted coupling matrix
survive a kiln load the bench identification never tried).

These are cheap monotonicity/no-op checks, not a validation of the ASSUMED
load model's absolute numbers (see load_mass_sweep.py's module docstring for
what is measured vs assumed). What they DO prove: the mass/coupling scaling
is not a silent no-op, and it moves tracking error in the physically
expected direction (more mass -> slower response -> worse profile-7
tracking at these gains).
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import load_mass_sweep as lms  # noqa: E402


def _worst_abs_mean_error(mass_mult, coupling_mult):
    result = lms.run_profile7_loaded(mass_mult, coupling_mult)
    rows = lms.compute_metrics(result)
    return max(abs(r.mean_error_c) for r in rows)


class LoadMassScalingTests(unittest.TestCase):
    def test_loaded_K_tau_scales_tau_only_by_mass(self):
        import numpy as np
        from kilnctrl import plant_sim as ps
        K, tau = lms.loaded_K_tau(mass_mult=2.0, coupling_mult=1.0)
        self.assertTrue(np.allclose(tau, ps.tau * 2.0))
        self.assertTrue(np.allclose(K, ps.K_full))  # coupling_mult=1.0 -> unchanged

    def test_loaded_K_tau_scales_only_off_diagonal_by_coupling(self):
        import numpy as np
        from kilnctrl import plant_sim as ps
        K, tau = lms.loaded_K_tau(mass_mult=1.0, coupling_mult=0.5)
        self.assertTrue(np.allclose(np.diag(K), ps.K_diag))  # diagonal untouched
        off_expected = 0.5 * (ps.K_full - np.diag(ps.K_diag))
        off_actual = K - np.diag(np.diag(K))
        self.assertTrue(np.allclose(off_actual, off_expected))
        self.assertTrue(np.allclose(tau, ps.tau))  # mass_mult=1.0 -> unchanged

    def test_more_mass_degrades_profile7_tracking(self):
        """The core claim this module exists to check: a heavier load (more
        thermal mass, same gains, same feedforward matrix) makes profile-7
        tracking worse, not better or unchanged. If this ever goes green
        with an unmodified load_mass_sweep.py, the scaling has become a
        no-op -- see the mutation-testing note in
        firmware/KilnFW/docs/PID_EXPANSION_PLAN.md."""
        err_1x = _worst_abs_mean_error(1.0, 1.0)
        err_4x = _worst_abs_mean_error(4.0, 1.0)
        self.assertGreater(
            err_4x, err_1x + 1.0,
            f"expected 4x load to measurably worsen tracking vs 1x "
            f"(got {err_1x:.2f}C -> {err_4x:.2f}C) -- mass scaling may be a no-op",
        )

    def test_loaded_physical_plant_delegates_to_base_step(self):
        """Finding 6 (opus review round 4): LoadedPhysicalKilnPlant used to
        hand-copy PhysicalKilnPlant.step() verbatim, with nothing to catch
        the two drifting apart while plant_sim.py is under active edit.
        LoadedPhysicalKilnPlant now overrides only the scaled instance
        attributes and inherits step() unchanged -- so at
        mass_mult=coupling_mult=1.0 (the reference point, no scaling
        applied) it must reproduce PhysicalKilnPlant's own trajectory
        byte-for-byte over several ticks, for any starting temps/duty
        sequence, not just at t=0."""
        import numpy as np
        from kilnctrl import plant_sim as ps

        start = [40.0, 55.0, 62.0]
        base = ps.PhysicalKilnPlant(ps.DT, ambient=20.0, start_temp=list(start))
        loaded = lms.LoadedPhysicalKilnPlant(ps.DT, ambient=20.0, start_temp=list(start),
                                              mass_mult=1.0, coupling_mult=1.0)
        duties = [
            np.array([0.8, 0.5, 0.3]),
            np.array([0.6, 0.6, 0.6]),
            np.array([0.0, 1.0, 0.2]),
            np.array([0.4, 0.4, 0.9]),
        ]
        for duty in duties:
            t_base = base.step(duty.copy())
            t_loaded = loaded.step(duty.copy())
            self.assertTrue(np.array_equal(t_base, t_loaded),
                             f"LoadedPhysicalKilnPlant at 1.0x/1.0x diverged from "
                             f"PhysicalKilnPlant: {t_base} vs {t_loaded}")

    def test_identified_load_reproduces_unscaled_plant(self):
        """mass_mult=coupling_mult=1.0 must be byte-identical to calling
        FOPDTPlant with the module's own K_full/tau directly -- the 'load'
        axes must be true no-ops at their reference point, not merely close."""
        import numpy as np
        from kilnctrl import plant_sim as ps
        K, tau = lms.loaded_K_tau(1.0, 1.0)
        self.assertTrue(np.array_equal(K, ps.K_full))
        self.assertTrue(np.array_equal(tau, ps.tau))


if __name__ == "__main__":
    unittest.main()
