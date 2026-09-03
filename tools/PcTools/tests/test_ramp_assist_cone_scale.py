#!/usr/bin/env python3
"""Cone-SCALE (real ceramic temperature) exercise of the ramp-assist dwell
credit -- everything in ``test_ramp_assist.py`` runs at bench scale (profile
7 tops out ~60 C, far below the lowest cone, 586 C), so none of it has ever
actually driven a zone INSIDE a real cone band. This module runs the same
mechanism against ``plant_sim.PhysicalKilnPlant`` at a bisque (~cone 04,
1062.8 C), a mid-fire glaze (cone 6, 1222.2 C) and a high fire (cone 10,
1285.0 C) target, matching the scenarios reported in
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

At heavier load the figure collapses back to exactly zero. THE PRECISE
MECHANISM (not just "heavier mass makes the zone fall further behind" --
that is true but not the actual gate the zero traces to): a ``RampStep``'s
own freeze-and-resume releases -- ending the ramp step, advancing
``z.seg_idx`` to the following ``DwellStep`` -- once the zone is back
within the WIDE ``lag_band_c`` (25 C, mirroring firmware's
``EXEC_RAMP_LOCK_BAND_C``). Credit's own gate is a MUCH NARROWER band --
half a cone step below the segment target, well under 25 C for every
target in this module. At heavy load the zone is still outside that
narrower band at the exact tick the wide band releases the ramp step, so
the zone enters its dwell before it has ever entered the credit band at
all -- these are two genuinely different thresholds, not one blocking the
other by construction.

EXTENDED PAST THE NOMINAL RAMP END (2026-09-03, see PID_EXPANSION_PLAN.md
section 7.3): credit accrual no longer stops the instant ``z.seg_idx``
moves into the ``DwellStep`` -- it now keeps running through the dwell too,
against the same held target, until the zone actually reaches it (see the
``DwellStep`` branch in ``ramp_assist.py``'s ``run_ramp_assist``). This
closes the gap above IN GENERAL (a multi-segment schedule can spend credit
banked during one dwell against a LATER dwell's own entry -- confirmed
directly: a synthetic two-``DwellStep`` cone-6-at-4x-mass schedule banks
~0.2 s of credit during the first dwell and spends it against the second).
It does NOT change any figure in the table above, because every schedule
this module drives is a single ``RampStep`` + a single ``DwellStep`` --
once that one dwell begins, there is no LATER dwell occurrence left in the
schedule for in-dwell accrual to ever be spent against (the schedule ends,
and any credit banked during that final dwell is simply discarded, the
same safe "unspent credit is lost, never negative" direction
``ramp_assist_dwell_credit_spend()`` already documents). The zeros in the
table above are therefore still the correct, current, re-verified reading
of the real shipped code as of 2026-09-03 -- re-run to confirm, not
carried forward stale.

RE-RUN AGAINST REALISTIC MULTI-SEGMENT SCHEDULES (2026-09-03, see
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` section 7.6.2): the single
RampStep+DwellStep shape above is not what a real firing looks like -- a
real bisque/glaze schedule candles, ramps, and only then dwells, often more
than once. ``BISQUE_MULTI``/``CONE6_MULTI``/``CONE10_MULTI`` below are built
from this repo's own shipped profile catalogue
(``firmware/KilnFW/App/drivers/profiles_builtin_table.inc``, sourced from
digitalfire.com/schedule): bisque from ``BQ1000`` ("Plainsman Electric
Bisque"), cone 6 from ``C6DHSC`` ("Plainsman Cone 6 Drop-and-hold, Slow
Cool", cool-down leg dropped -- this simulator/plant has no forced-cooling
model and the credit mechanism is about heating dwells only). Cone 10 has
no shipped profile pairing a candle with a single soak, so ``CONE10_MULTI``
adds a standard 150 C/30 min candle (ordinary pottery practice) ahead of
``C10RPL`` ("Plainsman Cone 10R Firing")'s own ramp structure. Each
schedule's FINAL target is snapped to this module's existing bisque/cone6/
cone10 figures (1062.8/1222.2/1285.0 C) so the multi-segment and
single-segment rows stay directly comparable; every other segment is the
shipped profile's own values, unmodified.

Re-run headline numbers, zone 0, 1 run per cell, REAL shipped code:

| schedule | mass_mult | total banked (spent + discarded) | spent (applied to final dwell) | discarded (never spent) | spent as % of the dwell it was applied to |
|---|---|---:|---:|---:|---:|
| bisque (candle 121C/60min, 945C, final 1062.8C/30min) | 1x | 1160.8 s | 619.4 s | 541.5 s | 34.4% |
| bisque | 2x | 153.3 s | 153.3 s | 0.0 s | 8.5% |
| bisque | 4x | 425.9 s | 1.6 s | 424.3 s | 0.1% |
| cone 6 (candle 121C/60min, 1148C, final 1222.2C/15min) | 1x | 248.2 s | 105.4 s | 142.8 s | 11.7% |
| cone 6 | 2x | 0.0 s | 0.0 s | 0.0 s | 0.0% |
| cone 6 | 4x | 53.1 s | 0.0 s | 53.1 s | 0.0% |
| cone 10 (candle 150C/30min, 980C, final 1285.0C/15min) | 1x | 406.5 s | 406.5 s | 0.0 s | 45.2% |
| cone 10 | 2x | 406.2 s | 406.2 s | 0.0 s | 45.1% |
| cone 10 | 4x | 19.6 s | 0.0 s | 19.6 s | 0.0% |

"Spent" and "discarded" are deliberately reported separately, not netted:
``credit_s`` (``dwell_credit_parity``'s per-zone field) is credit that was
actually applied to shrink a dwell; ``credit_reference_heat_s`` read
straight off the raw ``run_ramp_assist`` result (not the "applied" audit
variant ``dwell_credit_parity`` itself computes) is whatever was left
banked and unspent when the run ended -- conflating the two would overstate
the feature by counting heat-work that never shortened anything.

WHAT ACTUALLY CHANGES FROM THE SINGLE-SEGMENT TABLE, AND WHAT DOESN'T: the
multi-segment structure does NOT create a second real spend opportunity
the way the module docstring's earlier synthetic two-``DwellStep`` example
did. ``cone_table.CONE_TABLE``'s floor is 586.1 C (cone 022) -- every
candle segment here (121 C, 150 C) sits below it, so ``in_band`` raises
``ConeTableError`` and no credit accrues or is owed during the candle, and
the candle dwell itself always receives a zero spend (nothing has
accrued yet when it is entered). The intermediate ramp segments (945/1148/
980 C) ARE inside the cone table's range and DO contribute banked credit
that carries forward -- that is why the multi-segment "spent" figures above
differ from the single-segment table's -- but there is still only ONE real
dwell inside cone-table range in any of these three shipped-catalogue
schedules, so there is still only one place for credit to be spent. This is
an honest property of real bisque/cone/glaze recipes (they candle cold,
below the range the credit band and its cone-table lookup can even
evaluate), not a limitation of this test harness.

**Heavy-load verdict, the owner's motivating case (a kiln tuned empty then
fired full): UNCHANGED from the single-segment table.** At 4x mass, spent
credit is 1.6 s / 0.0 s / 0.0 s (bisque/cone 6/cone 10) against 1800/900/
900 s final dwells -- 0.1%, 0.0%, 0.0% of the dwell it was applied to. The
"discarded" column shows real credit DOES accrue under heavy load (424.3 s
at bisque 4x, 53.1 s at cone 6 4x, 19.6 s at cone 10 4x) -- it is not that
nothing happens -- but under heavy load that credit accrues mostly during
the terminal dwell itself (extended-past-ramp-end accrual, see the module
docstring above), after the one dwell it could have shortened has already
been sized, so it is banked and then simply discarded when the schedule
ends. Realistic multi-segment structure does not give that credit anywhere
else to go, because (as above) only one dwell in these schedules sits
inside the cone table's covered range. **The feature does not materially
help a loaded kiln** in either the single-segment or the multi-segment
scenarios measured here -- a clear negative, not a magnitude quibble.

STATISTICAL DISCIPLINE: every cell above is exactly ONE simulator run (no
repeats/seeds -- ``run_ramp_assist``/``PhysicalKilnPlant`` are
deterministic given ``mass_mult``, there is no stochastic element to
average over). All nine runs stay at or below ~1285 C simulated zone
temperature; none of this exercises the coupled hold/climb solve above its
known ~62 C feasibility ceiling in any way that differs from the
single-segment table already in this module -- that ceiling is a bench-rig
(``PhysicalKilnPlant`` operates in a different, unmeasured-above-80C regime
entirely per this module's own HONESTY NOTE above) constraint, not
something these multi-segment runs newly cross.
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


# ---------------------------------------------------------------------------
# REALISTIC MULTI-SEGMENT schedules -- see the module docstring's "RE-RUN
# AGAINST REALISTIC MULTI-SEGMENT SCHEDULES" section for the sourcing and
# headline table. Built from firmware/KilnFW/App/drivers/
# profiles_builtin_table.inc (this repo's own shipped profile catalogue,
# itself sourced from digitalfire.com/schedule), with only the FINAL
# target snapped to this module's existing bisque/cone6/cone10 figures so
# the two tables stay directly comparable.
# ---------------------------------------------------------------------------

def _multi_sched(steps):
    out = []
    for kind, *args in steps:
        if kind == 'r':
            out.append(ra.RampStep(args[0], args[1] / 60.0))  # args[1] is C/hr
        else:
            out.append(ra.DwellStep(args[0]))  # args[0] is minutes
    return out


# Bisque: "BQ1000" / Plainsman Electric Bisque (shipped). Final target
# snapped 1000 -> 1062.8 C (cone 04, this module's own bisque figure).
BISQUE_MULTI = _multi_sched([
    ('r', 121.0, 170.0), ('d', 60),
    ('r', 945.0, 170.0),
    ('r', 1062.8, 60.0), ('d', 30),
])

# Cone 6: "C6DHSC" / Plainsman Cone 6 Drop-and-hold, Slow Cool (shipped).
# Cool-down leg (target below the previous segment's) dropped -- this
# simulator/plant has no forced-cooling model and dwell credit is a
# heating-dwell mechanism. Final target snapped 1204 -> 1222.2 C.
CONE6_MULTI = _multi_sched([
    ('r', 121.0, 60.0), ('d', 60),
    ('r', 1148.0, 194.0),
    ('r', 1222.2, 60.0), ('d', 15),
])

# Cone 10: "C10RPL" / Plainsman Cone 10R Firing (shipped) has no candle
# dwell, and no shipped cone-10 profile in this catalogue pairs a candle
# with a single soak -- a standard 150 C/30 min candle (ordinary pottery
# practice) was added ahead of it. Final target snapped 1300 -> 1285.0 C.
CONE10_MULTI = _multi_sched([
    ('r', 150.0, 60.0), ('d', 30),
    ('r', 980.0, 100.0),
    ('r', 1285.0, 100.0), ('d', 15),
])

MULTI_SCHEDULES = {'bisque': BISQUE_MULTI, 'cone6': CONE6_MULTI, 'cone10': CONE10_MULTI}
MULTI_MAX_SIM_S = {
    'bisque': {1.0: 60000.0, 2.0: 65000.0, 4.0: 130000.0},
    'cone6': {1.0: 60000.0, 2.0: 65000.0, 4.0: 110000.0},
    'cone10': {1.0: 90000.0, 2.0: 95000.0, 4.0: 150000.0},
}
# The final dwell's own nominal duration -- the denominator "spent as % of
# the dwell it was applied to" in the module docstring's table uses (NOT
# the sum of every dwell in the schedule -- the candle dwell is always
# entered with zero banked credit, see the docstring's "WHAT ACTUALLY
# CHANGES" paragraph, so it is never the one credit shortens).
MULTI_FINAL_DWELL_S = {'bisque': 30 * 60.0, 'cone6': 15 * 60.0, 'cone10': 15 * 60.0}


def _multi_parity(which, mass_mult):
    return ra.dwell_credit_parity(
        MULTI_SCHEDULES[which], max_temp_c=1400.0, plant_regime='physical',
        start_temp_c=20.0, mass_mult=mass_mult, max_sim_s=MULTI_MAX_SIM_S[which][mass_mult])


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
    code path fires at 1x). See the module docstring's "THE PRECISE
    MECHANISM" paragraph for exactly which two thresholds this gap is
    between -- the wide ramp-lock release band (25 C) vs. the much narrower
    credit in-band test (half a cone step).

    RE-VERIFIED 2026-09-03 after extending credit accrual past the nominal
    ramp end (accrual now also runs on ``DwellStep`` ticks, not just
    ``RampStep`` ticks -- see ``ramp_assist.py``'s ``run_ramp_assist``,
    ``DwellStep`` branch, and PID_EXPANSION_PLAN.md section 7.3): these
    values are UNCHANGED. Every schedule this module drives is exactly one
    ``RampStep`` followed by exactly one ``DwellStep`` -- once that single
    dwell begins, there is no LATER dwell occurrence in the schedule for
    credit banked during it to ever be spent against (the schedule simply
    ends), so the extension has nothing to attach to here even though the
    accrual itself now runs. See the module docstring's "EXTENDED PAST THE
    NOMINAL RAMP END" paragraph for the synthetic two-dwell schedule that
    DOES show the extension banking and spending real credit.

    NEGATIVE-TESTED: temporarily forced the old ramp-phase-only accrual
    gate to stay ``True`` for one extra tick past the ramp->dwell
    transition (a boundary-off-by-one). Re-ran
    ``test_bisque_zero_credit_at_heavier_load``; it did NOT change the
    pinned zero values here (this scenario's zone crosses into band well
    after the boundary at heavier load, not by one tick), so that mutation
    is not evidence for or against this specific test -- documented for
    completeness; the meaningful negative test for the gate ITSELF is in
    ``ShippedBandNowFiresAtLightLoadTests``.
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


class RealisticMultiSegmentLightLoadTests(unittest.TestCase):
    """Light-load (1x, and cone 10's slower rate also at 2x) credit on the
    REALISTIC multi-segment ``BISQUE_MULTI``/``CONE6_MULTI``/``CONE10_MULTI``
    schedules (see module docstring for sourcing) -- mirrors
    ``ShippedBandNowFiresAtLightLoadTests`` above, which stays as the
    single-segment comparison baseline and is NOT superseded by this class.

    NEGATIVE-TESTED: temporarily reverted ``run_ramp_assist``'s RampStep
    branch from ``behind_schedule = actual_c < z.commanded_c`` to
    ``behind_schedule = z.lagging and in_band`` (the pre-fix mutually-
    exclusive gate this whole module's fix addresses -- see the module
    docstring). Re-ran ``test_bisque_multi_earns_real_credit_at_1x_load``;
    it failed with:
        AssertionError: 0.0 not greater than 0.0 : bisque multi-segment at
        1x load must earn real credit under the corrected gate
    (credit collapsed to exactly zero with the old gate, same as the
    single-segment finding). Reverted; passes again.
    """

    def test_bisque_multi_earns_real_credit_at_1x_load(self):
        res = _multi_parity('bisque', 1.0)
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0,
                            "bisque multi-segment at 1x load must earn real credit under the corrected gate")
        self.assertLessEqual(pz['credit_s'], MULTI_FINAL_DWELL_S['bisque'],
                              "spent credit must still clamp below the final dwell's own nominal duration")

    def test_cone6_multi_earns_real_credit_at_1x_load(self):
        res = _multi_parity('cone6', 1.0)
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0,
                            "cone 6 multi-segment at 1x load must earn real credit under the corrected gate")

    def test_cone10_multi_earns_real_credit_at_1x_and_2x_load(self):
        for mass_mult in (1.0, 2.0):
            res = _multi_parity('cone10', mass_mult)
            self.assertTrue(res['assisted']['targets_reached'][0], f"mass_mult={mass_mult}")
            pz = res['per_zone'][0]
            self.assertGreater(pz['credit_s'], 0.0,
                                f"cone 10 multi-segment at {mass_mult}x load must earn real credit")


class RealisticMultiSegmentHeavyLoadDoesNotHelpTests(unittest.TestCase):
    """The loaded-kiln verdict on the REALISTIC multi-segment schedules:
    at 4x mass, spent credit collapses to (near-)zero exactly as it does on
    the single-segment schedules in ``CreditCollapsesAtHeavierLoadTests``
    above -- multi-segment structure does not rescue the heavy-load case,
    because (per the module docstring's "WHAT ACTUALLY CHANGES" paragraph)
    every candle segment sits below ``cone_table.CONE_TABLE``'s 586.1 C
    floor, so there is still only ONE dwell in any of these three shipped-
    catalogue schedules inside the cone table's covered range for credit to
    ever be spent against. This is the section of this module that answers
    the owner's motivating question (a kiln tuned empty then fired full)
    directly: the answer is NO, this does not materially help, on either
    the single-segment or the multi-segment schedules measured.

    Bisque at 4x is asserted ``<=`` a small tolerance rather than
    ``assertEqual(..., 0.0)`` -- unlike the single-segment case this
    schedule's intermediate 945 C ramp segment is itself inside the cone
    table's range and contributes a small amount of carried-forward credit
    (measured 1.64 s here, against a 1800 s final dwell -- 0.09%), which is
    real but immaterial; the exact-zero pin stays on cone 6/cone 10, which
    measured exactly 0.0 s.
    """

    def test_bisque_multi_negligible_credit_at_4x_load(self):
        res = _multi_parity('bisque', 4.0)
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertLess(pz['credit_s'], 5.0,
                         "bisque multi-segment 4x credit_s should be immaterial (<5s of a 1800s dwell)")

    def test_cone6_multi_zero_credit_at_2x_and_4x_load(self):
        for mass_mult in (2.0, 4.0):
            res = _multi_parity('cone6', mass_mult)
            self.assertTrue(res['assisted']['targets_reached'][0], f"mass_mult={mass_mult}")
            pz = res['per_zone'][0]
            self.assertEqual(pz['credit_s'], 0.0, f"mass_mult={mass_mult} credit_s should be 0")

    def test_cone10_multi_zero_credit_at_4x_load(self):
        res = _multi_parity('cone10', 4.0)
        self.assertTrue(res['assisted']['targets_reached'][0])
        pz = res['per_zone'][0]
        self.assertEqual(pz['credit_s'], 0.0, "cone10 multi-segment 4x credit_s should be 0")


class RealisticMultiSegmentCreditAuditHoldsTests(unittest.TestCase):
    """``credit_audit_pct`` consistency check (see
    ``CreditAuditHoldsAtConeScaleTests`` above) re-run on a REALISTIC
    multi-segment schedule -- confirms the audit accumulator's lockstep
    reset (see ``ramp_assist.py``'s ``credit_reference_applied_s``) holds
    when there is a real intervening ramp segment (945 C) and a real
    candle dwell between the run's start and the dwell credit is spent
    against, not just the single-ramp shape every other cone-scale test in
    this module drives.
    """

    def test_credit_audit_pct_is_zero_on_multi_segment_schedule(self):
        res = _multi_parity('bisque', 1.0)
        pz = res['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0, "need a nonzero credit for this check to mean anything")
        self.assertLess(abs(pz['credit_audit_pct']), 0.5,
                         f"credit_audit_pct should be ~0 on a multi-segment schedule too, got {pz['credit_audit_pct']:.4f}%")


if __name__ == '__main__':
    unittest.main()
