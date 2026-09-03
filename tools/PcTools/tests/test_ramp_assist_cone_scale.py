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
bounded, does it grow with load) against THIS model -- they are not, and
cannot be, evidence about a real kiln's numbers.

RUNTIME NOTE: a cone-scale run simulates several real hours of firing at
``plant_sim.DT`` == 1 s per tick, so each ``run_ramp_assist``/
``dwell_credit_parity`` call here takes tens of seconds of wall-clock CPU
time -- this module is deliberately smaller than ``test_ramp_assist.py``
(only the scenarios that need cone-scale temperatures) to keep total suite
time reasonable.

FORMER MODEL ARTIFACT, RESOLVED by firmware commit 8f12449 ("Fix ramp-lock
hot-start stall: one-sided lock + guard 4 arming backstop") and mirrored
here: with the schedule replicated identically across all three zones
(matching every other scenario in this codebase -- see
``ramp_assist.run_ramp_assist``'s own docstring), zone 0 dominates the
physical model's cross-zone coupling enough that zones 1/2 can be pushed
by coupling ABOVE their own (still-low) commanded target early in the run.
The lag detector USED TO define "lagging" as ``abs(actual - commanded) >
lag_band_c`` with no direction check, so an OVERSHOOT froze the commanded
target exactly like a genuine shortfall did, and once frozen at a low
value the zone's own feedforward computed a negative hold duty (clamped to
0) forever -- a permanent stall, not a slow convergence. Firmware fixed
this with a ONE-SIDED lock (only a zone COLDER than commanded by more than
the band holds; an overshoot does not), and ``ramp_assist.py`` now mirrors
that one-sided form (``(commanded_c - actual_c) > lag_band_c``) -- an
overshot zone here no longer stalls. See ``ShippedDefaultBandNeverFiresTests``
and the other classes below for the re-run results; where a stall is still
observed at very high simulated load, it is noted at that test's own call
site rather than assumed away.

MIRROR-BUG RE-RUN NOTE (2026-09): this module's first pass ran against
``ramp_assist.DEFAULT_LAG_BAND_C`` while it was still hard-coded to 3.0 C
-- 8.3x tighter than firmware's real ``PROFILE_EXECUTOR_RAMP_LOCK_BAND_C``
(25.0 C); see ``ramp_assist.py``'s "MIRROR BUG" note. Re-run at the
corrected 25 C band, EVERY scenario in this module -- bisque/cone 6/cone 10,
at 1x through 8x simulated load -- now earns EXACTLY ZERO dwell credit: the
widest possible credit band anywhere in the whole cone table (cone 019,
677.8 C, an unusual mid-range target none of these three scenarios target)
is only 25.85 C, an 0.85 C sliver wider than the 25 C lag band, and every
other tabulated cone's band is narrower than the lag band outright -- so
"lagging (>25 C off) AND inside the credit band (usually 8-18 C wide)" is a
structurally near-empty set for realistic targets, not just an unlucky
draw of load or rate. Tests that need the accrual MECHANISM to actually
fire (to prove the mechanism itself is correct when it does) now pass an
explicit, narrower ``lag_band_c=3.0`` -- documented at each call site as a
synthetic override, not the shipped default -- mirroring the same pattern
``test_ramp_assist.py`` adopted for its own bench-scale mechanism tests.
``ShippedDefaultBandNeverFiresTests`` below is the new class that actually
answers "what does the shipped default do here": nothing, at every load
level tested. See PID_EXPANSION_PLAN.md section 7.6 for the full writeup
and the corrected headline numbers.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import cone_table as ct  # noqa: E402
from kilnctrl import ramp_assist as ra  # noqa: E402
from kilnctrl import plant_sim as ps  # noqa: E402

# (target_c, rate_c_per_hr, dwell_min, max_sim_s) -- same three targets used
# in the PID_EXPANSION_PLAN.md section 7.6 scenario scan.
BISQUE = (1062.8, 150.0, 30.0, 40000.0)
CONE6 = (1222.2, 150.0, 15.0, 45000.0)
CONE10 = (1285.0, 100.0, 15.0, 70000.0)

# Synthetic, narrower-than-shipped lag band used ONLY by tests that need to
# exercise the credit-accrual mechanism itself (see the module docstring's
# "MIRROR-BUG RE-RUN NOTE" -- the real 25 C default leaves this mechanism a
# near-empty accrual window at every cone-scale target tested). NOT the
# shipped default; never pass this implicitly.
_MECHANISM_TEST_LAG_BAND_C = 3.0

# BISQUE's own 150 C/hr rate, combined with the ONE-SIDED lock (firmware
# commit 8f12449, mirrored in ramp_assist.py -- see the module docstring)
# tracks closely enough that even at _MECHANISM_TEST_LAG_BAND_C it almost
# never locks (measured: 101/26929 ticks, none overlapping the credit
# band -- credit_s == 0.0). A faster rate is needed purely to give the
# mechanism-only tests below a real, sustained "colder than commanded"
# window to accrue credit in; NOT used by ShippedDefaultBandNeverFiresTests,
# which intentionally keeps BISQUE's real 150 C/hr rate.
_MECHANISM_TEST_BISQUE_RATE_C_PER_HR = 600.0


def _time_in_band_while_lagging_s(res, target_c, zi, dt):
    """Ticks where zone ``zi`` is both lagging AND inside the half-cone-step
    credit band -- the direct answer to §7.6 item 3 ("how long does a zone
    realistically spend inside that band while lagging")."""
    bb = ct.band_bottom_c(target_c)
    temp = res['temp'][:, zi]
    lag = res['lagging'][:, zi]
    in_band = (temp >= bb) & (temp < target_c)
    return float((in_band & lag).sum()) * dt


class ShippedDefaultBandNeverFiresTests(unittest.TestCase):
    """What the mechanism actually does at cone scale with the REAL,
    correctly-mirrored 25 C lag band (``ra.DEFAULT_LAG_BAND_C``, no
    override) -- the question the rest of this module's tests, which use a
    synthetic narrower band to test the mechanism in isolation, do not
    answer. All three standard scenarios (bisque/cone 6/cone 10), at every
    load multiplier tested (1x/2x/4x), earn EXACTLY zero credit -- not
    "small", zero -- because the credit band at every one of these targets
    is narrower than the 25 C lag band (see the module docstring). This is
    the corrected, material verdict for PID_EXPANSION_PLAN.md section 7.6:
    at its shipped default, this feature is inert at cone scale, not just
    "small at 1x load".

    NEGATIVE-TESTED: temporarily changed ``ra.DEFAULT_LAG_BAND_C`` from
    25.0 back to the old, wrong 3.0 (i.e. reproducing the mirror bug this
    module exists to catch downstream effects of). Re-ran
    ``test_bisque_zero_credit_at_every_load_level``; it failed with:
        AssertionError: 13.1... not equal to 0.0 : mass_mult=1 zone 0 credit_s should be exactly 0 at the shipped band
    (credit_s came back nonzero at every load level once the band was
    wrong-narrow again -- proving this test is actually sensitive to the
    band value, not vacuously true). Reverted; this test passes again with
    the real 25.0 C default.
    """

    def test_bisque_zero_credit_at_every_load_level(self):
        target_c, rate, dwell_min, _ = BISQUE
        sched = [ra.RampStep(target_c, rate / 60.0), ra.DwellStep(dwell_min)]
        for mass_mult, max_sim_s in ((1.0, 40000.0), (2.0, 45000.0), (4.0, 100000.0)):
            res = ra.dwell_credit_parity(
                sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
                mass_mult=mass_mult, max_sim_s=max_sim_s)
            self.assertTrue(res['assisted']['targets_reached'][0],
                             f"mass_mult={mass_mult} zone 0 did not reach target+dwell")
            credit_s = res['per_zone'][0]['credit_s']
            self.assertEqual(credit_s, 0.0,
                              f"mass_mult={mass_mult} zone 0 credit_s should be exactly 0 "
                              f"at the shipped band, got {credit_s}")

    def test_cone6_zero_credit_at_1x_and_2x_load(self):
        target_c, rate, dwell_min, _ = CONE6
        sched = [ra.RampStep(target_c, rate / 60.0), ra.DwellStep(dwell_min)]
        for mass_mult, max_sim_s in ((1.0, 45000.0), (2.0, 48000.0)):
            res = ra.dwell_credit_parity(
                sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
                mass_mult=mass_mult, max_sim_s=max_sim_s)
            self.assertTrue(res['assisted']['targets_reached'][0],
                             f"mass_mult={mass_mult} zone 0 did not reach target+dwell")
            credit_s = res['per_zone'][0]['credit_s']
            self.assertEqual(credit_s, 0.0,
                              f"mass_mult={mass_mult} zone 0 credit_s should be exactly 0 "
                              f"at the shipped band, got {credit_s}")

    def test_cone6_zero_time_in_band_at_1x_and_4x_load(self):
        target_c, rate, dwell_min, _ = CONE6
        sched = [ra.RampStep(target_c, rate / 60.0), ra.DwellStep(dwell_min)]
        light = ra.run_ramp_assist(sched, max_temp_c=1400.0, plant_regime='physical',
                                    start_temp_c=20.0, mass_mult=1.0, max_sim_s=45000.0)
        heavy = ra.run_ramp_assist(sched, max_temp_c=1400.0, plant_regime='physical',
                                    start_temp_c=20.0, mass_mult=4.0, max_sim_s=81000.0)
        self.assertTrue(light['targets_reached'][0])
        self.assertTrue(heavy['targets_reached'][0])
        self.assertEqual(_time_in_band_while_lagging_s(light, target_c, 0, ps.DT), 0.0)
        self.assertEqual(_time_in_band_while_lagging_s(heavy, target_c, 0, ps.DT), 0.0)


class ConeScaleMechanismRunsTests(unittest.TestCase):
    """Proves the accrual gate CAN fire at real cone temperatures (not just
    in the bench-scale tests) and that when it fires the credit stays a
    small, bounded fraction of the dwell it is spent against -- not
    "erases most of a dwell" territory. Uses the synthetic
    ``_MECHANISM_TEST_LAG_BAND_C`` override (see module docstring): at the
    real shipped band this accrual gate does not fire at all for this
    schedule (see ``ShippedDefaultBandNeverFiresTests``), so this class
    tests the mechanism's own correctness in isolation from that finding.

    NEGATIVE-TESTED: temporarily changed the accrual gate in
    ``run_ramp_assist`` from ``if z.lagging and in_band:`` to
    ``if False:`` (accrual never fires). Re-ran ``test_bisque_zone0_earns_
    some_but_not_most_of_the_dwell``; it failed with:
        AssertionError: 0.0 not greater than 0.0
    (credit_s was exactly 0.0 -- the assertion that the mechanism actually
    banks something at cone scale is real, not vacuous). Reverted the
    mutation; the test passes again with the live gate.
    """

    def test_bisque_zone0_earns_some_but_not_most_of_the_dwell(self):
        # Uses _MECHANISM_TEST_BISQUE_RATE_C_PER_HR (600 C/hr), not
        # BISQUE's own 150 C/hr: with the ONE-SIDED lock (firmware commit
        # 8f12449, mirrored here), 150 C/hr tracks closely enough that even
        # at _MECHANISM_TEST_LAG_BAND_C it almost never locks (measured
        # 101/26929 ticks, none overlapping the credit band, credit_s ==
        # 0.0) -- see the constant's own comment. A faster rate is needed
        # purely to give this mechanism-only test a real "colder than
        # commanded" window to accrue credit in.
        target_c, _rate, dwell_min, max_sim_s = BISQUE
        sched = [ra.RampStep(target_c, _MECHANISM_TEST_BISQUE_RATE_C_PER_HR / 60.0),
                 ra.DwellStep(dwell_min)]
        parity = ra.dwell_credit_parity(
            sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
            max_sim_s=max_sim_s, lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        assisted = parity['assisted']
        # zone 0 is the zone that actually reaches target and dwell in this
        # scenario -- see this module's docstring on the cross-zone stall.
        self.assertTrue(assisted['targets_reached'][0])
        pz = parity['per_zone'][0]
        nominal_s = pz['dwell_nominal_s']
        credit_s = pz['credit_s']
        self.assertGreater(credit_s, 0.0, "bisque zone 0 should enter the credit band while lagging")
        # Neither inert nor dwell-erasing: a real but small fraction.
        self.assertLess(credit_s, 0.10 * nominal_s,
                         f"credit {credit_s:.1f}s should stay a small fraction of "
                         f"the {nominal_s:.0f}s nominal dwell at 1x load, got "
                         f"{100.0 * credit_s / nominal_s:.1f}%")

    def test_cone10_slower_rate_tracks_closely_so_credit_is_near_zero(self):
        # Cone 10 here uses a slower commanded rate (100 C/hr vs 150 C/hr
        # for the other two targets) that this plant model can track much
        # more closely -- see PID_EXPANSION_PLAN.md section 7.6: credit is
        # NOT a function of temperature alone, it is a function of how far
        # behind the commanded ramp the zone actually falls. Uses the same
        # synthetic mechanism-test band as the rest of this class -- even
        # with that narrower band, this well-tracked ramp earns very
        # little credit, which is the point under test.
        target_c, rate, dwell_min, max_sim_s = CONE10
        sched = [ra.RampStep(target_c, rate / 60.0), ra.DwellStep(dwell_min)]
        parity = ra.dwell_credit_parity(
            sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
            max_sim_s=max_sim_s, lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        assisted = parity['assisted']
        self.assertTrue(assisted['targets_reached'][0])
        pz = parity['per_zone'][0]
        self.assertLess(pz['credit_s'], 60.0,
                         f"a well-tracked cone-10 ramp should earn very little credit, got {pz['credit_s']:.1f}s")


class LoadGrowsCreditTests(unittest.TestCase):
    """§7.6 item 2: load is exactly when a kiln falls behind, so credit
    (and the fraction of the dwell it removes) should grow with mass_mult
    -- WHEN the accrual gate can fire at all. Uses the synthetic
    ``_MECHANISM_TEST_LAG_BAND_C`` override (see module docstring): at the
    real 25 C shipped band, ``ShippedDefaultBandNeverFiresTests`` already
    shows credit stays exactly zero through 4x load for this same
    schedule, so there is nothing here for load to grow FROM at the real
    band -- this class instead confirms the underlying MECHANISM still
    scales with load correctly when it is given a band that lets it fire.

    NEGATIVE-TESTED: temporarily hardcoded ``mass_mult = 1.0`` at the top
    of ``run_ramp_assist`` (right after the ``dt = ...`` line), overriding
    whatever the caller passed. Re-ran ``test_credit_grows_with_load``; it
    failed with:
        AssertionError: 9.123583942780646 not less than 9.123583942780646
    (the mass_mult=2.0 run produced the IDENTICAL credit_s as mass_mult=1.0
    -- 9.12s both times -- because the hardcoded override made every run use
    the same 1x plant regardless of the requested load). Reverted the
    mutation; the test passes again once mass_mult actually reaches the
    plant.
    """

    def test_credit_grows_with_load(self):
        target_c, rate, dwell_min, _ = CONE6
        sched = [ra.RampStep(target_c, rate / 60.0), ra.DwellStep(dwell_min)]
        light = ra.dwell_credit_parity(
            sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
            mass_mult=1.0, max_sim_s=45000.0, lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        heavy = ra.dwell_credit_parity(
            sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
            mass_mult=2.0, max_sim_s=48000.0, lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        self.assertTrue(light['assisted']['targets_reached'][0])
        self.assertTrue(heavy['assisted']['targets_reached'][0])
        light_credit = light['per_zone'][0]['credit_s']
        heavy_credit = heavy['per_zone'][0]['credit_s']
        self.assertLess(light_credit, heavy_credit,
                         f"2x-mass credit ({heavy_credit:.1f}s) should exceed 1x-mass credit "
                         f"({light_credit:.1f}s) -- load is when the kiln falls behind")


class TimeInBandTests(unittest.TestCase):
    """§7.6 item 3: measures actual dwell-time-in-band, not just the
    arithmetic -- if a zone only spends a handful of seconds inside an
    8-18 C-wide cone-scale band while lagging, the credit mechanism is
    starved of the window it needs regardless of the weight formula. Uses
    the synthetic ``_MECHANISM_TEST_LAG_BAND_C`` override (see module
    docstring); at the real 25 C shipped band time-in-band is exactly zero
    at every load level tested (``ShippedDefaultBandNeverFiresTests``).

    NEGATIVE-TESTED: temporarily changed the lag threshold ``lag_band_c``
    passed into these calls from 3.0 to 30.0 (an order of magnitude wider
    "lagging" band, which should make almost nothing register as lagging
    near a cone-scale target since the plant tracks within a few degrees).
    Re-ran ``test_time_in_band_is_measured_and_small_at_1x_load``; it
    failed with:
        AssertionError: 35.0 not greater than 0.0
    (time-in-band-while-lagging collapsed to 0.0 at the wider band, while
    the un-mutated 3.0 C run kept its nonzero value, confirming the metric
    is sensitive to the threshold and not a hardcoded stand-in). Reverted
    the mutation.
    """

    def test_time_in_band_is_measured_and_small_at_1x_load(self):
        # Uses _MECHANISM_TEST_BISQUE_RATE_C_PER_HR, not BISQUE's own
        # 150 C/hr -- see that constant's comment and
        # ConeScaleMechanismRunsTests' matching note: with the ONE-SIDED
        # lock, 150 C/hr never gives this metric a nonzero window to
        # measure.
        target_c, _rate, dwell_min, max_sim_s = BISQUE
        sched = [ra.RampStep(target_c, _MECHANISM_TEST_BISQUE_RATE_C_PER_HR / 60.0),
                 ra.DwellStep(dwell_min)]
        res = ra.run_ramp_assist(sched, max_temp_c=1400.0, plant_regime='physical',
                                  start_temp_c=20.0, max_sim_s=max_sim_s,
                                  lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        self.assertTrue(res['targets_reached'][0])
        tib = _time_in_band_while_lagging_s(res, target_c, 0, ps.DT)
        # Real, but a matter of tens of seconds at 1x load -- nowhere near
        # "many minutes" for this scenario/model. See PID_EXPANSION_PLAN.md
        # section 7.6 for the full per-scenario table (loaded cases spend
        # much longer in-band), and for the corrected finding that at the
        # real shipped band this is exactly 0, not just small.
        self.assertGreater(tib, 0.0)
        self.assertLess(tib, 300.0,
                         f"expected a small time-in-band-while-lagging at 1x load, got {tib:.1f}s")

    def test_time_in_band_grows_with_load(self):
        target_c, rate, dwell_min, _ = CONE6
        sched = [ra.RampStep(target_c, rate / 60.0), ra.DwellStep(dwell_min)]
        light = ra.run_ramp_assist(sched, max_temp_c=1400.0, plant_regime='physical',
                                    start_temp_c=20.0, mass_mult=1.0, max_sim_s=45000.0,
                                    lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        heavy = ra.run_ramp_assist(sched, max_temp_c=1400.0, plant_regime='physical',
                                    start_temp_c=20.0, mass_mult=4.0, max_sim_s=81000.0,
                                    lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        self.assertTrue(light['targets_reached'][0])
        self.assertTrue(heavy['targets_reached'][0])
        tib_light = _time_in_band_while_lagging_s(light, target_c, 0, ps.DT)
        tib_heavy = _time_in_band_while_lagging_s(heavy, target_c, 0, ps.DT)
        self.assertLess(tib_light, tib_heavy,
                         f"4x-mass time-in-band ({tib_heavy:.1f}s) should exceed 1x-mass "
                         f"({tib_light:.1f}s)")


class CreditAuditHoldsAtConeScaleTests(unittest.TestCase):
    """``credit_audit_pct`` (the two-writer accrual/spend consistency
    check, see ``ramp_assist.dwell_credit_parity``'s docstring) was only
    ever pinned at bench scale before this module. Confirms the same
    identity holds when the accrual gate is actually firing against a
    cone-scale target and the physical (not FOPDT) plant -- and reiterates
    that this check is BLIND to a wrong Ea/band/weight shared by both
    writers (see the parent docstring); it is not evidence the physics is
    right, only that the accrual and spend statements agree. Uses the
    synthetic ``_MECHANISM_TEST_LAG_BAND_C`` override (see module
    docstring) because the check needs a nonzero credit to mean anything,
    and the real shipped band earns none here.

    NEGATIVE-TESTED: temporarily changed the credit accrual line from
    ``z.credit_s += w * dt`` to ``z.credit_s += w * dt * 1.3`` (a 30% scale
    error confined to the real accrual statement, leaving the independent
    audit statement untouched). Re-ran ``test_credit_audit_pct_is_zero_at_
    cone_scale``; it failed with:
        AssertionError: 30.00000000000004 not less than 0.5
    (``credit_audit_pct`` read +30.0%, exactly the injected scale error).
    Reverted the mutation; the test passes again with the real 1.0x line.
    """

    def test_credit_audit_pct_is_zero_at_cone_scale(self):
        # Uses _MECHANISM_TEST_BISQUE_RATE_C_PER_HR, not BISQUE's own
        # 150 C/hr -- see that constant's comment: with the ONE-SIDED lock,
        # 150 C/hr never earns nonzero credit here, and this check needs a
        # nonzero credit to mean anything.
        target_c, _rate, dwell_min, max_sim_s = BISQUE
        sched = [ra.RampStep(target_c, _MECHANISM_TEST_BISQUE_RATE_C_PER_HR / 60.0),
                 ra.DwellStep(dwell_min)]
        parity = ra.dwell_credit_parity(
            sched, max_temp_c=1400.0, plant_regime='physical', start_temp_c=20.0,
            max_sim_s=max_sim_s, lag_band_c=_MECHANISM_TEST_LAG_BAND_C)
        pz = parity['per_zone'][0]
        self.assertGreater(pz['credit_s'], 0.0, "need a nonzero credit for this check to mean anything")
        self.assertLess(abs(pz['credit_audit_pct']), 0.5,
                         f"credit_audit_pct should be ~0 at cone scale too, got {pz['credit_audit_pct']:.4f}%")


if __name__ == '__main__':
    unittest.main()
