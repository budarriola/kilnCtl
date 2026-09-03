#!/usr/bin/env python3
"""Cone-SCALE (real ceramic temperature) exercise of the ramp-assist dwell
credit -- everything in ``test_ramp_assist.py`` runs at bench scale (profile
7 tops out ~60 C, far below the lowest cone, 586 C), so none of it has ever
actually driven a zone INSIDE a real cone band. This module runs the same
mechanism (unmodified -- nothing in ``ramp_assist.py`` or ``cone_table.py``
is touched to make these pass) against ``plant_sim.PhysicalKilnPlant`` at a
bisque (~cone 04, 1062.8 C), a mid-fire glaze (cone 6, 1222.2 C) and a high
fire (cone 10, 1285.0 C) target, matching the scenarios reported in
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` section 7.6.

HONESTY NOTE, inherited from ``ramp_assist.py``'s own module docstring:
``PhysicalKilnPlant``'s parameters (element wattage, wall insulation,
thermal mass, and the cross-zone coupling growth above the bench rig's
80 C ceiling) are ASSUMED, not measured. These tests pin the MECHANISM's
behaviour at cone scale (does the accrual gate fire, does the credit stay
bounded, does it grow or shrink with load) against THIS model -- they are
not, and cannot be, evidence about a real kiln's numbers.

RUNTIME NOTE: a cone-scale run simulates several real hours of firing at
``plant_sim.DT`` == 1 s per tick, so each ``run_ramp_assist``/
``dwell_credit_parity`` call here takes tens of seconds of wall-clock CPU
time -- this module is deliberately smaller than ``test_ramp_assist.py``
(only the scenarios that need cone-scale temperatures) to keep total suite
time reasonable.

CREDIT-GATE FIX, RE-RUN 2026-09-03 (supersedes every number this module
previously reported): the firmware and this simulator both used to gate
dwell-credit accrual on ``lagging`` -- the ramp-lock's own 25 C
``lag_band_c``/``EXEC_RAMP_LOCK_BAND_C`` signal -- AND ``in_band`` (inside
the half-cone-step credit band below the segment target). Those two
conditions are structurally near-mutually-exclusive at real cone-scale
targets: during a ramp the commanded setpoint never exceeds the segment
target, so a zone lagging by more than 25 C is necessarily more than 25 C
below the segment target too, while ``in_band`` requires being within half
a cone step -- under 25 C almost everywhere in the Orton table. Measured
result before the fix: EXACTLY ZERO credit at bisque, cone 6 and cone 10, at
every load level tested (1x-4x mass). See
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sections 7.3/7.6 for the full
writeup.

**The fix (owner decision):** credit now accrues whenever a zone is BEHIND
SCHEDULE AT ALL (``actual_c < the moving commanded_c``, no 25 C threshold)
AND in_band -- the ramp-lock's 25 C band still governs the lock itself
(freezing the commanded setpoint) and is untouched. Because ordinary PID
tracking practically never puts ``actual_c`` bit-exactly at or above the
still-advancing commanded setpoint while inside a several-degree-wide band,
this gate now fires under completely ordinary, on-schedule tracking, not
only during a genuine >25 C lag. The classes below replace the old
(now-void) ``ShippedDefaultBandNeverFiresTests``/``ConeScaleMechanismRunsTests``
split -- there is no longer a "mechanism only fires with a synthetic
narrower band" case to carve out, because the shipped band was never what
gated credit in the first place.

Re-run headline numbers, zone 0, REAL shipped code (no ``lag_band_c``
override -- it no longer has any bearing on credit):

| target | mass_mult | credit_s | % of nominal dwell |
|---|---|---:|---:|
| bisque (1062.8 C, 150 C/hr, 30 min dwell) | 1x | 180.6 s | 10.0% |
| bisque | 2x | 0.0 s | 0.0% |
| bisque | 4x | 0.0 s | 0.0% |
| cone 6 (1222.2 C, 150 C/hr, 15 min dwell) | 1x | 112.8 s | 12.5% |
| cone 6 | 2x | 0.0 s | 0.0% |
| cone 6 | 4x | 0.0 s | 0.0% |
| cone 10 (1285.0 C, 100 C/hr, 15 min dwell) | 1x | 218.7 s | 24.3% |
| cone 10 | 2x | 218.5 s | 24.3% |
| cone 10 | 4x | 0.0 s | 0.0% |

At light load (1x, and cone 10's slower rate also at 2x) credit is now real
and material (10-24% of the nominal dwell) -- a sharp reversal from the
"exactly zero, structurally" verdict the mutually-exclusive gate produced.
At heavier load the figure collapses back to exactly zero: heavier mass
makes the zone fall further behind DURING the ramp, so by the time the
segment's commanded setpoint reaches ``step.target_c`` (ending the ramp
step and starting the dwell) the zone's actual temperature has often not
yet entered the half-cone-step in-band window at all -- it enters that
window only after the dwell has already begun, where this ramp-phase
accrual gate no longer runs (``ramping_now`` is false; see
``ramp_assist_dwell_credit_tick()``'s own gate). This is a genuine
model-dependent finding about WHEN a zone crosses into the credit band
relative to the ramp/dwell boundary, not a re-introduction of the old
mutual-exclusion defect -- the gate itself no longer references the 25 C
band at all, and directly-observed accrual at 1x proves that.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import ramp_assist as ra  # noqa: E402

# (target_c, rate_c_per_hr, dwell_min, {mass_mult: max_sim_s}) -- same three
# targets used in the PID_EXPANSION_PLAN.md section 7.6 scenario scan.
BISQUE = (1062.8, 150.0, 30.0, {1.0: 40000.0, 2.0: 45000.0, 4.0: 100000.0})
CONE6 = (1222.2, 150.0, 15.0, {1.0: 45000.0, 2.0: 48000.0, 4.0: 81000.0})
CONE10 = (1285.0, 100.0, 15.0, {1.0: 70000.0, 2.0: 75000.0, 4.0: 110000.0})


def _parity(target_c, rate_c_per_hr, dwell_min, mass_mult, max_sim_s):
    sched = [ra.RampStep(target_c, rate_c_per_hr / 60.0), ra.DwellStep(dwell_min)]
    return ra.dwell_credit_parity(
        sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
        mass_mult=mass_mult, max_sim_s=max_sim_s)


class ShippedBandNowFiresAtLightLoadTests(unittest.TestCase):
    """What the mechanism actually does at cone scale with the REAL shipped
    code, no ``lag_band_c`` override -- credit no longer needs one, because
    it is no longer gated on ``lagging`` at all (see module docstring). At
    1x (ordinary, on-schedule) load, all three targets earn real, nonzero
    credit -- a direct reversal of the pre-fix "exactly zero, structurally"
    finding.

    NEGATIVE-TESTED: temporarily re-added the old 25 C-lock gate inside
    ``ramp_assist_dwell_credit_tick`` (mirrored here by reverting the
    ``behind_schedule`` line in ``run_ramp_assist`` to
    ``z.lagging and in_band``). Re-ran ``test_bisque_earns_real_credit_at_1x_load``;
    it failed with:
        AssertionError: 0.0 not greater than 0.0
    (credit collapsed back to exactly zero with the old gate, confirming
    this test is actually sensitive to the fix). Reverted; passes again.
    """

    def test_bisque_earns_real_credit_at_1x_load(self):
        target_c, rate, dwell_min, masses = BISQUE
        res = _parity(target_c, rate, dwell_min, 1.0, masses[1.0])
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0,
                            "bisque at 1x load must earn real credit under the corrected gate")
        self.assertLess(pz['credit_s'], pz['dwell_nominal_s'],
                         "credit must still clamp below the nominal dwell")

    def test_cone6_earns_real_credit_at_1x_load(self):
        target_c, rate, dwell_min, masses = CONE6
        res = _parity(target_c, rate, dwell_min, 1.0, masses[1.0])
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0,
                            "cone 6 at 1x load must earn real credit under the corrected gate")

    def test_cone10_earns_real_credit_at_1x_and_2x_load(self):
        target_c, rate, dwell_min, masses = CONE10
        for mass_mult in (1.0, 2.0):
            res = _parity(target_c, rate, dwell_min, mass_mult, masses[mass_mult])
            self.assertTrue(res['assisted']['targets_reached'][0], f"mass_mult={mass_mult}")
            pz = res['per_zone'][0]
            self.assertGreater(pz['credit_s'], 0.0,
                                f"cone 10 at {mass_mult}x load must earn real credit")


class CreditCollapsesAtHeavierLoadTests(unittest.TestCase):
    """At heavier simulated load the measured credit falls back to exactly
    zero for bisque/cone 6 at 2x-4x and cone 10 at 4x -- NOT a return of the
    old mutual-exclusion defect (the gate no longer references the 25 C
    band at all; ``ShippedBandNowFiresAtLightLoadTests`` proves this same
    code path fires at 1x). The mechanism finding here is that heavier mass
    makes a zone fall further behind schedule during the ramp itself, so
    the zone's actual temperature often does not enter the half-cone-step
    in-band window until AFTER the ramp step has already ended and the
    dwell has begun -- past this accrual gate's own ``ramping_now`` window.
    See the module docstring's table.

    NEGATIVE-TESTED: temporarily forced ``ramping_now`` to stay ``True``
    for one extra tick past the ramp->dwell transition (a boundary-off-by-
    one). Re-ran ``test_bisque_zero_credit_at_heavier_load``; it did NOT
    change the pinned zero values here (this scenario's zone crosses into
    band well after the boundary at heavier load, not by one tick), so
    that mutation is not evidence for or against this specific test --
    documented for completeness; the meaningful negative test for the gate
    ITSELF is in ``ShippedBandNowFiresAtLightLoadTests``.
    """

    def test_bisque_zero_credit_at_heavier_load(self):
        target_c, rate, dwell_min, masses = BISQUE
        for mass_mult in (2.0, 4.0):
            res = _parity(target_c, rate, dwell_min, mass_mult, masses[mass_mult])
            self.assertTrue(res['assisted']['targets_reached'][0], f"mass_mult={mass_mult}")
            pz = res['per_zone'][0]
            self.assertEqual(pz['credit_s'], 0.0, f"mass_mult={mass_mult} credit_s should be 0")

    def test_cone6_zero_credit_at_heavier_load(self):
        target_c, rate, dwell_min, masses = CONE6
        for mass_mult in (2.0, 4.0):
            res = _parity(target_c, rate, dwell_min, mass_mult, masses[mass_mult])
            self.assertTrue(res['assisted']['targets_reached'][0], f"mass_mult={mass_mult}")
            pz = res['per_zone'][0]
            self.assertEqual(pz['credit_s'], 0.0, f"mass_mult={mass_mult} credit_s should be 0")

    def test_cone10_zero_credit_at_4x_load_only(self):
        target_c, rate, dwell_min, masses = CONE10
        res = _parity(target_c, rate, dwell_min, 4.0, masses[4.0])
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertEqual(pz['credit_s'], 0.0, "cone10 4x credit_s should be 0")


class CreditAuditHoldsAtConeScaleTests(unittest.TestCase):
    """``credit_audit_pct`` (the two-writer accrual/spend consistency
    check, see ``ramp_assist.dwell_credit_parity``'s docstring) at cone
    scale, now firing under the real shipped code (no synthetic band
    override needed any more).

    NEGATIVE-TESTED: temporarily changed the credit accrual line from
    ``z.credit_s += w * dt`` to ``z.credit_s += w * dt * 1.3`` (a 30% scale
    error confined to the real accrual statement). Re-ran
    ``test_credit_audit_pct_is_zero_at_cone_scale``; it failed with:
        AssertionError: 30.000000000000068 not less than 0.5
    (``credit_audit_pct`` read +30.0%, exactly the injected scale error).
    Reverted the mutation; passes again with the real 1.0x line.
    """

    def test_credit_audit_pct_is_zero_at_cone_scale(self):
        target_c, rate, dwell_min, masses = BISQUE
        res = _parity(target_c, rate, dwell_min, 1.0, masses[1.0])
        pz = res['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0, "need a nonzero credit for this check to mean anything")
        self.assertLess(abs(pz['credit_audit_pct']), 0.5,
                         f"credit_audit_pct should be ~0 at cone scale too, got {pz['credit_audit_pct']:.4f}%")


if __name__ == '__main__':
    unittest.main()
