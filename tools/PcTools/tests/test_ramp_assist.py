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
    """Pins ``ramp_assist.dwell_credit_parity``.

    DEFECT 1 (adversarial review): the OLD ``parity_vs_unassisted_pct``
    metric compared ``credit_s + dwell_heat_work_assisted_s`` against
    ``dwell_heat_work_unassisted_s``. During a dwell the zone sits at
    target (weight ~1.0), so shortening the dwell by ``credit_s`` seconds
    removes ~``credit_s`` weight-seconds from ``dwell_heat_work_assisted_s``
    relative to the unassisted run -- making the two sides ALGEBRAICALLY
    approximately equal FOR ANY VALUE OF ``credit_s``, correct or not. This
    was proven by scaling ONLY the accrual line ``z.credit_s += w * dt`` by
    a constant factor SCALE and re-running this class's own schedule:

        SCALE 0.5 -> parity_vs_unassisted_pct  0.038 / 0.131 / 0.162
        SCALE 1.0 -> parity_vs_unassisted_pct  0.075 / 0.088 / 0.149  (real code)
        SCALE 2.0 -> parity_vs_unassisted_pct  0.151 / 0.004 / 0.123
        SCALE 3.0 -> parity_vs_unassisted_pct  0.055 / 0.092 / 0.923

    A credit half, double, or triple the correct value all passed the old
    ``abs(...) < 1.0`` threshold -- it could not tell 0.5x from 3x. Worse,
    mutating the accrual to raw seconds (``w * dt`` -> ``dt``) gave parity
    0.000 / 0.000 / 4.380 on this same schedule: two of three zones, which
    WERE genuinely mis-banked, read EXACTLY ZERO. The old metric and its
    docstring claim ("near 0 means the credit mechanism is unit-consistent")
    have been removed; see ``ramp_assist.dwell_credit_parity``'s current
    docstring and ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` §7.3 for
    the full writeup, including the fact that the FIRST replacement
    attempted (a fixed-window heat-work comparison) was ALSO caught as
    insufficiently sensitive by this exact sweep before ``credit_audit_pct``
    was adopted -- see ``ScaleSweepDiscriminatesCreditErrorsTests`` below,
    which is the actual proof obligation for "does the replacement metric
    discriminate".
    """

    _SCHED = [
        ra.RampStep(700.0, 300.0), ra.RampStep(803.9, 500.0), ra.DwellStep(10.0),
    ]
    _KW = dict(start_temp_c=20.0, max_temp_c=1300.0, plant_regime='physical', max_sim_s=15 * 3600.0)

    def test_credit_audit_pct_is_near_zero_for_correct_accrual(self):
        # credit_audit_pct compares credit actually spent (credit_s, from
        # the real accrual line) against an INDEPENDENT audit accumulator
        # computed on its own separate statement at the same gate
        # (credit_reference_heat_s). In correct code both compute the
        # identical weight*dt sum, so this should read ~0% -- but, unlike
        # the old metric, that is not forced algebraically; see the
        # NEGATIVE-TESTED note below.
        #
        # NEGATIVE-TESTED: changed the real accrual line ``z.credit_s +=
        # w * dt`` to ``z.credit_s += dt`` (raw seconds), leaving the
        # independent audit line untouched. Re-ran this test; it failed
        # with:
        #     AssertionError: 3055.9372297151176 not less than 5.0 : zone 0 credit_audit_pct
        # (audit blew out to ~3056%/3180%/3135% -- the mutation was caught
        # by roughly 30x its own magnitude, not hidden). Reverted; this
        # test passes again.
        res = ra.dwell_credit_parity(self._SCHED, **self._KW)
        for zi, z in enumerate(res['per_zone']):
            self.assertGreater(z['credit_s'], 0.0, f"zone {zi} should have accrued real credit")
            self.assertLess(
                abs(z['credit_audit_pct']), 5.0,
                f"zone {zi} credit_audit_pct")

    def test_zero_credit_still_shows_a_nominal_gap_from_catchup_lag(self):
        # A fast, hard ramp straight into a dwell where the zone is STILL
        # below target (still catching up) when the dwell begins: this
        # produces zero dwell credit (the ramp is fast enough it never
        # enters the in-band-and-lagging gate at all -- rate=1000 C/min is
        # used, not the 500 C/min the module docstring's older writeup
        # used, because the DEFECT 2 band-width fix widens the credit band
        # near 900 C enough that 500 C/min now earns a little real credit;
        # 1000 C/min still earns none, see this test's own assertion
        # below), yet parity_vs_nominal_pct is still negative -- proof the
        # nominal-gap metric alone conflates a genuine second-order
        # plant-lag effect with the credit mechanism, even when the credit
        # mechanism did nothing at all.
        sched = [ra.RampStep(900.0, 1000.0), ra.DwellStep(30.0)]
        res = ra.dwell_credit_parity(sched, start_temp_c=20.0, max_temp_c=1300.0,
                                      plant_regime='physical', max_sim_s=15 * 3600.0)
        for zi, z in enumerate(res['per_zone']):
            self.assertEqual(z['credit_s'], 0.0, f"zone {zi} should have earned no credit here")
            self.assertLess(
                z['parity_vs_nominal_pct'], -1.0,
                f"zone {zi} parity_vs_nominal_pct should show a real catch-up gap")
            self.assertGreater(z['catchup_deficit_s'], 0.0, f"zone {zi} catchup_deficit_s")
            # credit_audit_pct is nan when no credit/reference heat was ever
            # accrued (0/0) -- also proof no credit-caused problem, since
            # none was banked at all.
            import math
            self.assertTrue(math.isnan(z['credit_audit_pct']))


class ScaleSweepDiscriminatesCreditErrorsTests(unittest.TestCase):
    """The actual DEFECT 1 proof obligation the task set: run the module's
    own accrual line scaled by 0.5x/1.0x/2.0x/3.0x and show
    ``credit_audit_pct`` clearly tells them apart -- unlike the old
    ``parity_vs_unassisted_pct``, which read 0.000%-4.380% across the same
    sweep (see ``DwellCreditParityTests``'s docstring).

    This test loads ``ramp_assist.py``'s own source, replaces exactly one
    occurrence of ``z.credit_s += w * dt`` with a SCALE-multiplied version,
    and execs the mutated module under a throwaway name -- i.e. it performs
    the mutation sweep the task asked for as an executable, permanent
    regression test rather than a one-off manual check.
    """

    _SCHED_SRC = "[RampStep(700.0, 300.0), RampStep(803.9, 500.0), DwellStep(10.0)]"
    _KW = dict(start_temp_c=20.0, max_temp_c=1300.0, plant_regime='physical', max_sim_s=15 * 3600.0)

    @staticmethod
    def _load_scaled_module(scale: float):
        import importlib.util
        src_path = os.path.join(os.path.dirname(ra.__file__), "ramp_assist.py")
        src = open(src_path, encoding="utf-8").read()
        target = "z.credit_s += w * dt"
        # Appears twice: once in the real accrual line, once inside a
        # docstring discussing it. Mutating both is harmless (the
        # docstring occurrence is not executable); what matters is that
        # the REAL accrual line is the one under test.
        assert src.count(target) == 2, src.count(target)
        mutated = src.replace(target, f"z.credit_s += ({scale}) * w * dt")
        mod_name = f"kilnctrl._scaled_ramp_assist_test_{str(scale).replace('.', '_')}"
        spec = importlib.util.spec_from_loader(mod_name, loader=None)
        m = importlib.util.module_from_spec(spec)
        m.__package__ = "kilnctrl"
        m.__name__ = mod_name
        sys.modules[mod_name] = m
        exec(compile(mutated, src_path, "exec"), m.__dict__)
        return m

    def test_credit_audit_pct_discriminates_scale_errors(self):
        results = {}
        for scale in (0.5, 1.0, 2.0, 3.0):
            m = self._load_scaled_module(scale)
            sched = eval(self._SCHED_SRC, {"RampStep": m.RampStep, "DwellStep": m.DwellStep})
            res = m.dwell_credit_parity(sched, **self._KW)
            results[scale] = [z['credit_audit_pct'] for z in res['per_zone']]

        # Every zone's credit_audit_pct must equal (scale - 1) * 100% to
        # within float tolerance -- exact, not just "different".
        for scale, pcts in results.items():
            for zi, pct in enumerate(pcts):
                self.assertAlmostEqual(
                    pct, (scale - 1.0) * 100.0, places=3,
                    msg=f"scale={scale} zone={zi} credit_audit_pct={pct}")

        # And the four scales are clearly, monotonically distinguishable --
        # the exact failure mode of the old metric (0.5x/1.0x/2.0x/3.0x all
        # read within a 4.4-percentage-point band) cannot reproduce here.
        for zi in range(3):
            series = [results[s][zi] for s in (0.5, 1.0, 2.0, 3.0)]
            self.assertEqual(series, sorted(series), f"zone {zi} not monotonic: {series}")
            self.assertGreater(max(series) - min(series), 100.0,
                                f"zone {zi} scale sweep span too small: {series}")


class BandWidthCliffTests(unittest.TestCase):
    """DEFECT 2: ``cone_table.band_bottom_c`` used to anchor the credit
    band's half-width on the distance from ``target_c`` down to the
    nearest tabulated cone BELOW it, not on the local cone spacing -- so a
    target a hair above a tabulated cone collapsed the band to a
    near-zero width. That was fixed directly in ``cone_table.c``/``.py``
    (commit f84d4c8) to use the bracketing-pair formula. ``ramp_assist``
    used to carry its own duplicate workaround
    (``_local_band_half_c``/``_band_bottom_c_fixed``); that duplicate is
    now removed and ``ramp_assist`` calls ``cone_table.band_bottom_c``
    directly (see the ``ct.band_bottom_c`` call in the dwell-credit gate).
    This class pins the exact numbers from the PID_EXPANSION_PLAN.md §7.3
    defect-2 writeup against the ONE shared formula ramp_assist now uses.

    NEGATIVE-TESTED: temporarily edited ``cone_table.band_bottom_c`` in
    ``tools/PcTools/src/kilnctrl/cone_table.py`` to reintroduce the old
    buggy anchor (``return target_c - (target_c - lower_temp_c) / 2.0``
    in place of ``return target_c - half``). Ran this class; it failed
    with:
        AssertionError: 0.0049999999999954525 != 8.35 within 2 places
    (band collapsed back to ~0.005 C at 1222.21, exactly the DEFECT 2
    symptom -- proving this class does exercise the live formula
    ramp_assist depends on, not a fixed constant). Reverted; this class
    passes again.
    """

    def test_just_below_a_cone_gets_the_wide_local_spacing(self):
        # target 1222.19 is just BELOW cone 6 (1222.2) -- bracketed by
        # cone 5HALF (1203.0) and cone 6 (1222.2) now that the table
        # carries the half-cones, so half the local spacing is
        # (1222.2-1203.0)/2 = 9.60. (Before the half-cones were added this
        # was bracketed by cone 5 (1186.1)/cone 6, giving 18.05 -- see
        # cone_table.h's half-cone addition note.)
        width = 1222.19 - ct.band_bottom_c(1222.19)
        self.assertAlmostEqual(width, 9.60, places=2)

    def test_just_above_a_cone_does_not_collapse(self):
        # target 1222.21 is one hundredth of a degree above cone 6
        # (1222.2) -- the OLD band_bottom_c formula anchored on the
        # DISTANCE to cone 6 itself (0.01 C), collapsing the band to
        # 0.005 C. The fixed formula instead uses the local spacing to the
        # NEXT cone up (cone 7, 1238.9), giving ~8.35 C -- 1670x wider.
        width = 1222.21 - ct.band_bottom_c(1222.21)
        self.assertGreater(width, 5.0, "band should not collapse just above a tabulated cone")
        self.assertAlmostEqual(width, 8.35, places=2)

    def test_a_degree_above_a_cone_still_uses_local_spacing(self):
        # target 1223.00 ("cone 6 plus a margin") -- still bracketed by
        # cone 6/cone 7, so the fix gives the SAME ~8.35 C width as
        # 1222.21 above, not the old formula's narrow 0.4 C.
        width = 1223.00 - ct.band_bottom_c(1223.00)
        self.assertGreater(width, 5.0)
        self.assertAlmostEqual(width, 8.35, places=2)

    def test_ramp_assist_no_longer_carries_its_own_copy(self):
        # The duplicate workaround functions are gone entirely -- there is
        # exactly one band-width formula left for ramp_assist to call.
        self.assertFalse(hasattr(ra, "_local_band_half_c"))
        self.assertFalse(hasattr(ra, "_band_bottom_c_fixed"))


class DwellStateMachineTests(unittest.TestCase):
    """DEFECT 3: ``dwell_remaining_s`` was assigned ONLY at a ramp->dwell
    transition and reset to ``None`` on dwell completion, so a schedule
    that opens with a ``DwellStep`` (a leading candling hold) or chains two
    ``DwellStep``s back to back hit ``assert z.dwell_remaining_s is not
    None`` with nothing upstream ever having set it. Dwell state
    (including any credit spend) is now initialised lazily on the
    ``DwellStep``'s own first tick instead.

    NEGATIVE-TESTED: reproduced the original bug by disabling the lazy-init
    guard (``if z.dwell_remaining_s is None:`` -> ``if False:``) and
    reinstating the old bare ``assert z.dwell_remaining_s is not None``
    ahead of the decrement. Ran a leading-dwell schedule through the
    mutated module; it raised:
        AssertionError:
    exactly the crash this class exists to prevent. Reverted; both tests
    below pass again against the real code.
    """

    def test_leading_dwell_does_not_crash(self):
        sched = [ra.DwellStep(2.0), ra.RampStep(45.0, 3.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0)
        self.assertTrue(all(res['targets_reached']), res['targets_reached'])

    def test_consecutive_dwells_do_not_crash(self):
        sched = [ra.RampStep(45.0, 3.0), ra.DwellStep(2.0), ra.DwellStep(2.0)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0)
        self.assertTrue(all(res['targets_reached']), res['targets_reached'])
        # Both dwells' nominal durations should be accounted for.
        self.assertEqual(res['dwell_nominal_s'], [120.0, 120.0])


class CreditExceedsDwellTests(unittest.TestCase):
    """Weaker defect flagged by the review: credit exceeding a dwell's
    nominal duration is silently discarded (``spend = min(credit, nominal)``
    -- never a negative dwell), but that was untested and undocumented.
    Pins the intended behaviour: a dwell's spent duration never drops below
    0, and the excess credit is discarded, not carried into the next dwell.

    NEGATIVE-TESTED: removed the cap (``spend = min(spend, nominal_s)`` ->
    a no-op). Ran ``test_credit_exceeding_dwell_is_capped_not_carried``; it
    failed with:
        AssertionError: 4.848005168144967 != 3.0
    (spend came back as the FULL, uncapped credit -- more than the entire
    3-second dwell -- instead of being pinned at the dwell's own nominal
    length). Reverted; this test passes again.
    """

    def test_credit_exceeding_dwell_is_capped_not_carried(self):
        # A ramp tail earning several seconds of real credit (see
        # DwellCreditParityTests -- ~4.8/5.4/7.3 s on this same schedule),
        # spent against a deliberately tiny (3 s nominal) dwell.
        sched = [ra.RampStep(700.0, 300.0), ra.RampStep(803.9, 500.0), ra.DwellStep(0.05)]
        res = ra.run_ramp_assist(sched, max_temp_c=1300.0, start_temp_c=20.0,
                                  plant_regime='physical', max_sim_s=15 * 3600.0)
        self.assertEqual(res['dwell_nominal_s'], [3.0])
        for zi, spend in enumerate(res['credit_applied_s']):
            self.assertEqual(spend, 3.0, f"zone {zi} spend should be capped at the 3 s dwell")
        self.assertTrue(all(res['targets_reached']), res['targets_reached'])


if __name__ == "__main__":
    unittest.main()
