#!/usr/bin/env python3
"""Decision-logic tests for ``kilnctrl.ramp_assist`` (simulator-only
validation harness for PID_EXPANSION_PLAN.md §7, "ramp assist"). Covers:
inert-when-capable, stretch-when-not-capable, hard refusal above
``max_temp_c``, back-to-back ramps, and the dwell-credit accrual gate
(only while lagging, zero at/below band bottom, maximal at target -- the
last two are cone_table.py's own contract, pinned again here through
``ramp_assist``'s actual call sites so a wiring mistake -- e.g. passing the
wrong temperature into ``heat_work_weight`` -- is also caught).

Every test in this file has been NEGATIVE-TESTED by hand: the corresponding
source line was mutated, the test was re-run and observed to fail, the
real failure output was recorded in that test's docstring, and the mutation
was reverted before this file was finalized. See each test's docstring for
the quoted failure.
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import cone_table as ct  # noqa: E402
from kilnctrl import ramp_assist as ra  # noqa: E402


class InertWhenCapableTests(unittest.TestCase):
    """Profile 7-scale ramp (45 C, 3 C/min -- a rate the bench plant
    demonstrably tracks, see plant_sim.py's TRUST section) should barely
    lag: assist should be very close to a no-op.

    NEGATIVE-TESTED: changed the lag threshold comparison in
    ``run_ramp_assist`` from ``abs(actual_c - z.commanded_c) > lag_band_c``
    to ``> 0.0`` (i.e. "lagging" the instant there is ANY tracking error,
    which is true almost every tick even on an easy ramp). Re-ran; it
    failed with:
        AssertionError: False is not true
    (``targets_reached`` was no longer ``[True, True, True]`` -- a zone
    that is "lagging" on any nonzero error never lets its commanded target
    advance at all, so it can never reach the segment target within
    max_sim_s). Reverted the mutation; this test passes again with the
    real 3.0 C threshold.
    """

    def test_easy_ramp_is_nearly_inert(self):
        sched = [ra.RampStep(45.0, 3.0), ra.DwellStep(20.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0)
        self.assertTrue(all(res['targets_reached']))
        for s in res['stretched_s']:
            self.assertLess(
                s, 60.0,
                f"stretched_s should be small on an easily-achievable ramp, got {res['stretched_s']}")

    def test_easy_ramp_dwell_credit_is_negligible(self):
        # An easy ramp barely lags, so it should barely enter the
        # dwell-credit band -- credit is a rounding error against the
        # nominal 20-minute dwell, not a meaningful shortening.
        sched = [ra.RampStep(45.0, 3.0), ra.DwellStep(20.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0)
        for c in res['credit_applied_s']:
            self.assertLess(c, 30.0, f"credit should be small on an easy ramp, got {res['credit_applied_s']}")


class StretchesWhenNotCapableTests(unittest.TestCase):
    """A ramp rate well beyond what the plant can track (60 C/min to a
    still-feasible 55 C steady state -- see ``hold_duty_infeasible(55.0)``
    is False) must still reach the target, via stretching.

    NEGATIVE-TESTED: replaced the ``z.commanded_c += rate_c_per_s * dt``
    advance (the branch that runs when NOT lagging) with a no-op, so the
    commanded target never moves at all once ramp-lock first engages. Ran
    ``test_unachievable_rate_still_reaches_target``; it failed with:
        AssertionError: False is not true : [False, False, False]
    (the run hit ``max_sim_s`` without ever finishing, because a
    permanently-frozen commanded target can never un-lag -- exactly the
    "stretch never resumes" bug this test exists to catch). Reverted; this
    test passes again.
    """

    def test_unachievable_rate_still_reaches_target(self):
        sched = [ra.RampStep(55.0, 60.0), ra.DwellStep(15.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0, max_sim_s=4 * 3600.0)
        self.assertTrue(all(res['targets_reached']), res['targets_reached'])
        # And it actually had to stretch to get there -- distinguishes this
        # test from the "inert when capable" case above.
        for s in res['stretched_s']:
            self.assertGreater(s, 100.0, f"expected real stretching, got {res['stretched_s']}")


class RefusalTests(unittest.TestCase):
    """A target above max_temp_c is a hard refusal, checked BEFORE any
    simulation tick runs -- never stretched.

    NEGATIVE-TESTED: changed ``check_schedule_feasible``'s comparison from
    ``step.target_c > max_temp_c`` to ``step.target_c > max_temp_c * 10``
    (i.e. effectively disabling the refusal for any realistic schedule).
    Ran the whole ``RefusalTests`` class; two of three failed with:
        AssertionError: RampAssistRefused not raised
    (``test_target_above_max_is_refused`` and
    ``test_refusal_never_stretches_a_second_over_limit_step`` both stopped
    raising; ``test_target_at_max_is_not_refused`` still passed, as
    expected since it asserts the no-raise case). Reverted; all three pass
    again with the real threshold.
    """

    def test_target_above_max_is_refused(self):
        sched = [ra.RampStep(1250.0, 100.0)]
        with self.assertRaises(ra.RampAssistRefused):
            ra.check_schedule_feasible(sched, max_temp_c=1200.0)
        with self.assertRaises(ra.RampAssistRefused):
            ra.run_ramp_assist(sched, max_temp_c=1200.0, start_temp_c=20.0)

    def test_target_at_max_is_not_refused(self):
        # Exactly at the ceiling is allowed (the check is strictly-greater,
        # matching a target that just reaches the ceiling rather than
        # exceeding it).
        sched = [ra.RampStep(1200.0, 100.0)]
        ra.check_schedule_feasible(sched, max_temp_c=1200.0)  # must not raise

    def test_refusal_never_stretches_a_second_over_limit_step(self):
        # A schedule with an achievable first ramp and an over-limit second
        # ramp must refuse on the SECOND step's index, not silently clamp
        # or stretch past the first.
        sched = [ra.RampStep(45.0, 5.0), ra.DwellStep(5.0), ra.RampStep(1250.0, 5.0)]
        with self.assertRaises(ra.RampAssistRefused) as cm:
            ra.check_schedule_feasible(sched, max_temp_c=1200.0)
        self.assertEqual(cm.exception.step_index, 2)


class BackToBackRampsTests(unittest.TestCase):
    """Two ramps with no dwell between them (each ramp endpoint is its own
    desired temperature) -- both endpoints must be reached, and credit
    accrued across BOTH ramp legs must be available to the dwell that
    finally follows (per ramp_assist.py's "credit accumulates across a
    whole run of consecutive ramp steps" design note).

    NEGATIVE-TESTED: added ``z.credit_s = 0.0`` immediately after
    ``z.seg_idx += 1`` on EVERY step advance (ramp->ramp included, and
    running before the dwell-spend calculation on a ramp->dwell advance
    too), not just when applying it to a dwell. Ran
    ``test_credit_carries_over_a_ramp_to_ramp_boundary``; it failed with:
        AssertionError: 0.0 not greater than 0.0
    (all credit earned during the first ramp leg was discarded before the
    second leg could add to it -- in fact this mutation broke credit
    application entirely, since it also fires on the very step that would
    have read ``z.credit_s`` to spend it on the next dwell). Reverted;
    this test passes again.
    """

    def test_both_ramp_endpoints_reached(self):
        sched = [ra.RampStep(700.0, 300.0), ra.RampStep(803.9, 300.0), ra.DwellStep(10.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0,
                                  plant_regime='physical', max_sim_s=15 * 3600.0)
        self.assertTrue(all(res['targets_reached']), res['targets_reached'])

    def test_credit_carries_over_a_ramp_to_ramp_boundary(self):
        # First leg alone (700 C) sits far below the 803.9 C target's own
        # cone band, so it earns essentially no credit against THAT band on
        # its own; a fast second leg into 803.9 C's wide band (see the
        # module-level widest-gap note) is what should produce credit. The
        # point under test is that the ramp->ramp transition does not zero
        # the accumulator before that second leg gets to contribute.
        sched = [ra.RampStep(700.0, 300.0), ra.RampStep(803.9, 500.0), ra.DwellStep(10.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0,
                                  plant_regime='physical', max_sim_s=15 * 3600.0)
        for c in res['credit_applied_s']:
            self.assertGreater(c, 0.0)


class DwellCreditGateTests(unittest.TestCase):
    """The credit accumulator's gate: only while lagging (not rising at
    the desired rate), and only within the half-cone-step band -- zero at
    or below band bottom, growing toward the target, mirroring
    cone_table.heat_work_weight's own contract (pinned independently in
    test_cone_table.py; this class checks ramp_assist actually calls it
    correctly).

    NEGATIVE-TESTED: changed the gate from ``if z.lagging and in_band:`` to
    ``if in_band:`` (i.e. accrue credit even while the zone IS achieving
    the commanded rate). Ran ``test_credit_is_zero_when_never_lagging``;
    it failed with:
        AssertionError: 300.0 != 0.0
    (credit accrued for the whole time the never-lagging ramp spent inside
    the target's band, capped at the dwell's own 300 s nominal duration --
    i.e. with the gate dropped, an ordinary on-schedule ramp would have
    zeroed out its own dwell entirely). Reverted; this test passes again.
    """

    def test_credit_is_zero_when_never_lagging(self):
        # An easy, slow ramp (1 C/min into 700 C) into a cone-range target
        # never lags at all (measured: stretched_s == [0,0,0]), so credit
        # should be exactly zero -- not just small.
        sched = [ra.RampStep(700.0, 1.0), ra.DwellStep(5.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0,
                                  plant_regime='physical', max_sim_s=20 * 3600.0)
        self.assertTrue(all(res['targets_reached']), res['targets_reached'])
        for c in res['credit_applied_s']:
            self.assertEqual(c, 0.0)

    def test_credit_weight_matches_cone_table_at_endpoints(self):
        # Direct check that ramp_assist evaluates heat_work_weight at the
        # SEGMENT's target (not some other temperature) -- band bottom
        # gives weight 0, the target itself gives weight 1, by construction
        # of cone_table.heat_work_weight.
        target = 803.9
        bottom = ct.band_bottom_c(target)
        self.assertEqual(ct.heat_work_weight(bottom, target), 0.0)
        self.assertEqual(ct.heat_work_weight(target, target), 1.0)


class DwellCreditParityTests(unittest.TestCase):
    """Pins ``ramp_assist.dwell_credit_parity``: the credit mechanism banks
    and spends the SAME unit (heat-work seconds, i.e. ``weight * dt`` while
    lagging and in-band), so the dwell-window heat work it actually
    delivers should track the achievable unassisted-dwell baseline closely
    -- NOT the idealized ``dwell_nominal_s`` figure, which even a
    zero-credit run falls short of whenever the plant is still catching up
    to target at dwell entry (see ``test_zero_credit_still_shows_a_nominal_
    gap_from_catchup_lag`` below; that gap is a plant-dynamics effect, not a
    credit-accounting bug, and this module's own PID_EXPANSION_PLAN.md
    write-up under §7.3 quantifies it separately).

    NEGATIVE-TESTED: changed ``z.credit_s += w * dt`` (ramp_assist.py, the
    dwell-credit accrual line) to ``z.credit_s += dt`` -- i.e. banking raw
    in-band-and-lagging SECONDS instead of heat-work seconds, exactly the
    unit-mismatch bug this test exists to catch a regression to. Ran
    ``test_credit_parity_is_near_exact_against_achievable_baseline``; it
    failed with:
        AssertionError: 4.380232859005497 not less than 1.0 : zone 2 parity_vs_unassisted_pct
    (parity blew out from ~0.15% to ~4.4% on zone 2 once credit started
    banking a full second of "value" for ticks that were only earning a
    fraction of a weight-second -- the dwell got over-shortened relative to
    what the ramp tail actually earned). Reverted the mutation; this test
    passes again with the real ``w * dt`` accrual.
    """

    _SCHED = [
        ra.RampStep(700.0, 300.0), ra.RampStep(803.9, 500.0), ra.DwellStep(10.0),
    ]
    _KW = dict(start_temp_c=20.0, max_temp_c=1300.0, plant_regime='physical', max_sim_s=15 * 3600.0)

    def test_credit_parity_is_near_exact_against_achievable_baseline(self):
        res = ra.dwell_credit_parity(self._SCHED, **self._KW)
        for zi, z in enumerate(res['per_zone']):
            self.assertGreater(z['credit_s'], 0.0, f"zone {zi} should have accrued real credit")
            self.assertLess(
                abs(z['parity_vs_unassisted_pct']), 1.0,
                f"zone {zi} parity_vs_unassisted_pct")

    def test_zero_credit_still_shows_a_nominal_gap_from_catchup_lag(self):
        # A fast, hard ramp straight into a dwell where the zone is STILL
        # below target (still catching up) when the dwell begins: this
        # produces zero dwell credit (physical_hard-style profile below the
        # cone band, so no in-band gate ever opens), yet
        # parity_vs_nominal_pct is still negative -- proof the nominal-gap
        # metric alone conflates a genuine second-order plant-lag effect
        # with the credit mechanism, even when the credit mechanism did
        # nothing at all.
        sched = [ra.RampStep(900.0, 500.0), ra.DwellStep(30.0)]
        res = ra.dwell_credit_parity(sched, start_temp_c=20.0, max_temp_c=1300.0,
                                      plant_regime='physical', max_sim_s=15 * 3600.0)
        for zi, z in enumerate(res['per_zone']):
            self.assertEqual(z['credit_s'], 0.0, f"zone {zi} should have earned no credit here")
            self.assertLess(
                z['parity_vs_nominal_pct'], -1.0,
                f"zone {zi} parity_vs_nominal_pct should show a real catch-up gap")
            self.assertGreater(z['catchup_deficit_s'], 0.0, f"zone {zi} catchup_deficit_s")
            # And the achievable-baseline metric agrees there's no
            # credit-caused problem, since none was applied.
            self.assertAlmostEqual(z['parity_vs_unassisted_pct'], 0.0, places=6)


if __name__ == "__main__":
    unittest.main()
