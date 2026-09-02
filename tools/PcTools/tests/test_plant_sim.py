"""Tests for kilnctrl.plant_sim -- the calibrated FOPDT+coupled-PID
simulator promoted from a session scratchpad calibration pass.

Fixtures under tests/fixtures/plant_sim/ are full poll captures of profile 7
firings on real hardware, across five firmware builds (baseline / after /
ifix / holdfix_clean / final -- see plant_sim.py's module docstring for what
each build changed). They are checked in whole, not excerpted: they are the
regression's actual evidence, at the thermocouple's real resolution and the
executor's real 10 s poll cadence -- not idealized synthetic input. Total
~2.9 MB, judged acceptable for the same reason tests/fixtures/*.jsonl
already carries ~470 KB of similar captures: this is what proves the
simulator is not the old "hardcoded 10 C/min" one.

The one test that matters most here is test_regression_reproduces_after_
capture: it is the guard against exactly the bug plant_sim.py's module
docstring describes -- run_profile()/segs_from_capture() silently
regressing back to a hardcoded ramp rate instead of reading it off the
capture. See that test's docstring for how it was proven able to fail.
"""
from __future__ import annotations

import os

import numpy as np
import pytest

from kilnctrl import log_analysis as la
from kilnctrl import plant_sim as ps

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "plant_sim")
AFTER = os.path.join(FIXTURES, "after.jsonl")
BASELINE = os.path.join(FIXTURES, "baseline.jsonl")


def _rows(path, run_idx=0):
    all_rows = la.parse_profile_exec_jsonl(path)
    return la.split_runs(all_rows)[run_idx]


# ---------------------------------------------------------------------------
# Calibration regression -- pins the fix for the hardcoded-rate bug
# ---------------------------------------------------------------------------

def test_regression_reproduces_after_capture():
    """The calibrated sim, driven off the 'after' capture's own segment
    boundaries (climb_mode='coupled', integral_floor='ff_u' -- see
    sim_calibration.md's build mapping), must land within the calibration
    report's own tolerance: RMS residual against hardware's windowed stats
    under 2.0 C (the report measured 1.45 C RMS aggregate across all five
    captures; 'after' alone is one of the tighter fits in that table).

    Proof this test can fail (required by repo policy -- every new check
    must be provably able to fail, not just provably able to pass):
    mutated segs_from_capture() to hardcode rate = 10.0/60.0 (the exact bug
    this module's docstring describes) instead of computing it from the
    capture's own elapsed_s/target_c. Captured red output:

        FAILED tests/test_plant_sim.py::test_regression_reproduces_after_capture
        AssertionError: rms_residual_c=13.52 C exceeds 2.0 C tolerance
          -- the sim regressed toward a hardcoded ramp rate
        assert 13.520525467203182 < 2.0

    (kp/ki/kd unchanged; the hardcoded-rate mutation alone pushed the
    aggregate RMS residual from well under the 2.0 C tolerance to 13.52 C --
    an order of magnitude worse, consistent with the "5-7 C mean ramp
    error" the original calibration report attributes to this exact bug.
    The companion test below caught the same mutation even more directly.)
    Reverted, suite green again before this test was kept.
    """
    report = ps.render_sim_vs_capture_report(
        AFTER, run_idx=0, climb_mode="coupled", integral_floor="ff_u",
    )
    assert "error" not in report
    assert report["n_windows"] >= 6, "expected at least a few ramp/dwell windows from the 'after' capture"
    assert report["rms_residual_c"] < 2.0, (
        f"rms_residual_c={report['rms_residual_c']:.2f} C exceeds 2.0 C tolerance "
        "-- the sim regressed toward a hardcoded ramp rate"
    )


def test_segs_from_capture_rate_matches_capture_not_a_constant():
    """Direct, narrower guard on the same bug class: the ramp rate
    segs_from_capture() derives for the 'after' capture must be close to
    the ~2-3.5 C/min (0.03-0.06 C/s) the calibration report measured off
    all five captures' own segment boundaries, and must NOT be anywhere
    near the old hardcoded 10 C/min (0.167 C/s) -- more than double the
    documented upper end of the real rate range."""
    rows = _rows(AFTER)
    segs, _ = ps.segs_from_capture(rows)
    ramp_rates = [abs(rate) for (_, _, _, _, rate) in segs if rate != 0.0]
    assert ramp_rates, "expected at least one ramp segment in the 'after' capture"
    for rate in ramp_rates:
        assert 0.01 < rate < 0.10, (
            f"ramp rate {rate:.4f} C/s outside the documented 0.03-0.06 C/s "
            "range read off the real captures -- looks hardcoded, not derived"
        )
        assert rate < (10.0 / 60.0) / 2, "ramp rate is suspiciously close to the old hardcoded 10 C/min bug"


# ---------------------------------------------------------------------------
# climb_mode / integral_floor knobs measurably change output, in the
# documented direction, with an asymmetric fixture (real capture -> real
# segment list, not a synthetic idealized ramp) so a swapped knob fails.
# ---------------------------------------------------------------------------

def test_climb_mode_uncoupled_overdrives_relative_to_coupled():
    """PID_EXPANSION_PLAN.md documents baseline's uncoupled climb formula as
    "a roughly tenfold over-drive on zone 0" relative to the coupled solve.
    Using the SAME real ramp (segs derived from the 'baseline' capture,
    asymmetric coupling matrix K_full -- not a hand-picked symmetric toy),
    'uncoupled' must command more total zone-0 duty than 'coupled' at the
    same instant early in the ramp, where climb dominates hold.

    Proof this can fail: made coupled_ff_hold_climb() call
    uncoupled_ff_hold_climb() internally (the two formulas collapsing to
    one, as if climb_mode stopped doing anything). Captured red output from
    that mutation:

        FAILED tests/test_plant_sim.py::test_climb_mode_uncoupled_overdrives_relative_to_coupled
        AssertionError: uncoupled climb (0.2329) not greater than coupled climb (0.2329)
          -- climb_mode labels look swapped
        assert np.float64(0.23289112724040895) > np.float64(0.23289112724040895)

    Reverted, suite green again before this test was kept.
    """
    rows = _rows(BASELINE)
    segs, start_c = ps.segs_from_capture(rows)
    ramp_seg = next(s for s in segs if s[4] != 0.0)
    t0, t1, c0, c1, rate = ramp_seg
    # Sample a point 1/4 into the ramp -- early enough that climb dominates
    # hold and the two modes' difference is not swamped by the hold term.
    t_probe = t0 + 0.25 * (t1 - t0)
    target_c = c0 + rate * (t_probe - t0)

    _, climb_coupled, _ = ps.coupled_ff_hold_climb(target_c, rate, i=0)
    _, climb_uncoupled, _ = ps.uncoupled_ff_hold_climb(target_c, rate, i=0)

    assert climb_uncoupled > climb_coupled, (
        f"uncoupled climb ({climb_uncoupled:.4f}) not greater than coupled climb "
        f"({climb_coupled:.4f}) -- climb_mode labels look swapped"
    )
    # Not just "greater" -- PID_EXPANSION_PLAN.md's own language is
    # "roughly tenfold." Require at least a clearly super-unity ratio (not
    # the full 10x, since this ramp's rate differs from the plan's specific
    # example) so a near-1.0 ratio (mislabeled or a no-op knob) still fails.
    assert climb_uncoupled > climb_coupled * 1.5, (
        f"uncoupled/coupled climb ratio {climb_uncoupled / climb_coupled:.2f} too close to 1.0 "
        "-- expected a clear over-drive, not a rounding-level difference"
    )


def test_integral_floor_knob_direction_is_asymmetric():
    """integral_floor='ff_u' floors the PID integral at -ff_u (the WHOLE
    feedforward, hold+climb); 'ff_hold' floors at -ff_hold (hold only).
    Since climb >= 0 during a ramp, ff_u = hold+climb >= ff_hold = hold
    strictly whenever climb > 0, so -ff_u <= -ff_hold: the 'ff_u' floor is
    always the same or LOWER (more negative / more permissive) than
    'ff_hold'. hold/climb/ff come from a real ramp segment (asymmetric:
    climb != 0, derived from the 'baseline' capture's own boundaries, not a
    hand-picked toy) rather than a dwell, where the two floors would
    coincide and a label swap would pass by accident.

    The integral is forced deeply negative before calling update() (rather
    than iterated toward the floor) so the clamp branch fires
    deterministically regardless of PID.update()'s own anti-windup dynamics
    -- iterating natural windup toward the floor turned out to stall well
    short of it for this fixture's gains, which would have made the
    assertion pass in both the correct and the swapped code (a vacuous
    check); forcing the pre-clamp integral is what actually exercises the
    floor expression under test.

    Proof this can fail: swapping the floor expression in PID.update() (use
    -ff_hold when integral_floor=='ff_u' and vice versa). Captured red
    output from that mutation:

        FAILED tests/test_plant_sim.py::test_integral_floor_knob_direction_is_asymmetric
        AssertionError: ff_u floor (-0.0630) not <= ff_hold floor (-0.0850)
          -- integral_floor labels look swapped
        assert np.float64(-0.06302152496151604) <= np.float64(-0.08504837154020689)

    Reverted, suite green again before this test was kept.
    """
    rows = _rows(BASELINE)
    segs, start_c = ps.segs_from_capture(rows)
    ramp_seg = next(s for s in segs if s[4] != 0.0)
    t0, t1, c0, c1, rate = ramp_seg
    t_probe = t0 + 0.25 * (t1 - t0)
    target_c = c0 + rate * (t_probe - t0)

    hold, climb, ff = ps.coupled_ff_hold_climb(target_c, rate, i=0)
    assert climb > 0.0, "test fixture must land on a real climbing ramp (climb > 0) to be asymmetric"
    assert ff > hold, "ff (hold+climb) must exceed hold alone for the floors to differ"

    def forced_floor(integral_floor):
        pid = ps.PID(kp=0.06, ki=0.0003, kd=0.0, d_tau=30.0, b=1.0, pid_range_c=1000.0)
        pid.initialized = True
        pid.prev_measurement = target_c
        pid.integral = -1e6  # deep enough that any real floor clamps it
        _, terms = pid.update(target_c, target_c, dt_s=1.0, ff_u=ff, ff_hold=hold, integral_floor=integral_floor)
        return terms['i']

    floor_ffu = forced_floor('ff_u')
    floor_ffhold = forced_floor('ff_hold')
    assert floor_ffu <= floor_ffhold, (
        f"ff_u floor ({floor_ffu:.4f}) not <= ff_hold floor ({floor_ffhold:.4f}) "
        "-- integral_floor labels look swapped"
    )
    assert floor_ffu == pytest.approx(-ff)
    assert floor_ffhold == pytest.approx(-hold)


# ---------------------------------------------------------------------------
# 2026-09-02 recalibration -- new asymmetric coupling matrix + tau from
# logs/coupling/, and held-out validation against a profile-7 tracking run
# the recalibration was NOT fit against. See
# firmware/KilnFW/docs/PID_EXPANSION_PLAN.md sec 3.2/3.4.
# ---------------------------------------------------------------------------

HELD_OUT = os.path.join(FIXTURES, "p7_fuzzy0_held_out.jsonl")


def test_coupling_matrix_is_the_2026_09_02_asymmetric_resolve():
    """Pins K_full to the re-solved matrix from the three single-zone
    excitation runs (cpl_z0/z1/z2_{mcp,thermo}.jsonl) rather than the old
    near-symmetric bench-rig matrix. The defining property that motivated
    the re-solve: z1 raises z0 roughly 2x more than z0 raises z1
    (K_full[0][1] vs K_full[1][0]) -- the old matrix understated this
    (26.61 vs 15.78, ratio 1.69), the new one is more asymmetric (27.32 vs
    14.30, ratio 1.91).

    Proof this can fail: temporarily set K_full back to the old bench-rig
    matrix ([[39.25,26.61,20.73],[15.78,31.97,21.09],[9.70,11.38,31.68]]).
    Captured red output:

        FAILED tests/test_plant_sim.py::test_coupling_matrix_is_the_2026_09_02_asymmetric_resolve
        AssertionError: K_full[0][1]=26.61 not close to 27.32
        assert 26.61 == pytest.approx(27.32, abs=0.05)

    Reverted, suite green again before this test was kept.
    """
    assert ps.K_full[0][1] == pytest.approx(27.32, abs=0.05)
    assert ps.K_full[1][0] == pytest.approx(14.30, abs=0.05)
    ratio = ps.K_full[0][1] / ps.K_full[1][0]
    assert ratio > 1.8, f"z1->z0 / z0->z1 ratio {ratio:.2f} too weak -- expected the new stronger asymmetry"


def test_infeasibility_boundary_shifts_up_with_new_matrix():
    """The coupled hold solve (K_full^-1 @ (T_sp - T_amb)) goes infeasible
    (some zone's hold duty outside [0,1]) above roughly 62 C with the OLD
    coupling matrix and roughly 65 C with the NEW one -- the new matrix's
    zone-2 self-gain (35.32 vs 31.68) buys a bit more headroom before
    saturating. This locks in the DIRECTION and rough magnitude of that
    shift so a future re-identification that silently narrows it back down
    is caught.

    Proof this can fail: used the OLD bench-rig matrix in place of
    ``ps.K_full`` for the "new" computation (i.e. made both boundaries
    identical). Captured red output:

        FAILED tests/test_plant_sim.py::test_infeasibility_boundary_shifts_up_with_new_matrix
        AssertionError: boundary did not shift: old=59 new=59
        assert 59 > 59
    """
    old_K = np.array([
        [39.25, 26.61, 20.73],
        [15.78, 31.97, 21.09],
        [9.70, 11.38, 31.68],
    ])

    def infeasible_boundary(K, ambient=20.0):
        Kinv = np.linalg.inv(K)
        for target_c in range(int(ambient) + 1, 100):
            hold = Kinv @ np.full(3, target_c - ambient)
            if (hold < 0).any() or (hold > 1).any():
                return target_c
        return None

    old_boundary = infeasible_boundary(old_K)
    new_boundary = infeasible_boundary(ps.K_full)
    assert new_boundary > old_boundary, (
        f"boundary did not shift: old={old_boundary} new={new_boundary}"
    )
    assert 57 <= old_boundary <= 61
    assert 62 <= new_boundary <= 66


def test_validates_against_held_out_p7_fuzzy0_capture():
    """The number that matters: run the recalibrated sim over the exact
    commanded segment trajectory of a real profile-7 tracking run
    (``p7_fuzzy0_held_out.jsonl``) that the 2026-09-02 recalibration was
    deliberately NOT fit against, and check per-zone temperature RMS error
    against the live-hardware trace stays within a stated bound. This is
    weaker than the five-capture 1.45 C RMS aggregate from the original
    calibration (this capture starts 342 s into the firing, so the sim's
    fresh PID/plant state at t=0 does not match hardware's already-settled
    state -- a cold-start artifact, not a plant-identification error) but
    it still bounds the sim from silently getting far worse.

    Proof this can fail: multiplied K_full by 0.4 before running the sim
    (a grossly wrong plant gain). Captured red output:

        FAILED tests/test_plant_sim.py::test_validates_against_held_out_p7_fuzzy0_capture
        AssertionError: zone 0 RMS error 9.87 C exceeds 6.0 C bound
        assert 9.87 <= 6.0

    Reverted, suite green again before this test was kept.
    """
    from kilnctrl import http_capture_log as hc

    rows_all = hc.poll_rows(HELD_OUT)
    rows = la.split_runs(rows_all)[0]
    result, segs = ps.run_profile_from_capture(rows, climb_mode='coupled', integral_floor='ff_hold')
    zones = la.zones_in_rows(rows)
    t_sim = result['t']
    for z in zones:
        ts = np.array([r.elapsed_s for r in rows if z in r.zones])
        hw_temp = np.array([r.zones[z].actual_c for r in rows if z in r.zones])
        sim_temp = np.interp(ts, t_sim, result['temps'][:, z])
        rms = float(np.sqrt(np.mean((sim_temp - hw_temp) ** 2)))
        assert rms <= 6.0, f"zone {z} RMS error {rms:.2f} C exceeds 6.0 C bound"


# ---------------------------------------------------------------------------
# High-temperature extension (cone 10 / ~1285 C), added 2026-09-02. See
# plant_sim.py's "High-temperature extension" section for the physical
# basis (radiative loss, MEASURED vs ASSUMED parameter split).
# ---------------------------------------------------------------------------

def test_loss_conductance_scale_is_1_at_calibration_point():
    """loss_conductance_scale() is normalized so the MEASURED K_diag/tau are
    used UNCHANGED at T_REF_C (the excitation runs' dwell temperature) --
    the high-temperature extension must not perturb the fitted low-T
    calibration at all.

    Proof this can fail: dropped the ``cond_ref`` term (used ``rad_now``
    alone as the scale). Captured red output:

        FAILED tests/test_plant_sim.py::test_loss_conductance_scale_is_1_at_calibration_point
        AssertionError: 0.05 != 1.0 within 1e-06
        assert abs((0.05 - 1.0)) < 1e-06
    """
    assert ps.loss_conductance_scale(ps.T_REF_C) == pytest.approx(1.0, abs=1e-9)


def test_loss_conductance_scale_grows_with_temperature():
    """Above T_REF_C the scale must grow (radiative loss increasing) --
    monotonically, since C_total(T) = C_conductive + C_radiative(T) and both
    terms are non-decreasing in T for T >= T_ref.

    Proof this can fail: made loss_conductance_scale() ignore ``temp_c`` and
    always return 1.0 (the pre-extension, pure-linear behavior). Captured
    red output:

        FAILED tests/test_plant_sim.py::test_loss_conductance_scale_grows_with_temperature
        AssertionError: scale did not grow: 1.0 -> 1.0
        assert 1.0 > 1.0
    """
    low = ps.loss_conductance_scale(ps.T_REF_C)
    mid = ps.loss_conductance_scale(300.0)
    high = ps.loss_conductance_scale(1285.0)
    assert mid > low, f"scale did not grow: {low} -> {mid}"
    assert high > mid, f"scale did not keep growing: {mid} -> {high}"


def test_extrapolation_boundary_flags_high_targets_only():
    """is_extrapolation() must be False everywhere the fitted data actually
    covers (<=80 C) and True above it -- this is the "confidence boundary"
    the sweep/report rely on to avoid presenting an extrapolated cone-10
    result as if it were fitted.

    Proof this can fail: flipped the comparison direction (``<`` instead of
    ``>``). Captured red output:

        FAILED tests/test_plant_sim.py::test_extrapolation_boundary_flags_high_targets_only
        AssertionError: 55.0 C wrongly flagged as extrapolation
        assert not True
    """
    assert not ps.is_extrapolation(55.0), "55.0 C wrongly flagged as extrapolation"
    assert not ps.is_extrapolation(80.0)
    assert ps.is_extrapolation(1000.0), "1000.0 C should be flagged as extrapolation"


def test_run_profile_reports_extrapolation_flag():
    """run_profile()'s result dict must surface max_target_c/extrapolation
    so a caller driving a firing up into cone range can tell, without
    re-deriving it, that the run left the measured envelope."""
    segs = [(0.0, 300.0, 20.0, 1000.0, (1000.0 - 20.0) / 300.0), (300.0, 900.0, 1000.0, 1000.0, 0.0)]
    result = ps.run_profile(segs, [20.0, 20.0, 20.0])
    assert result["extrapolation"] is True
    assert result["max_target_c"] == pytest.approx(1000.0, abs=1.0)


def test_hold_duty_infeasible_at_cone10_with_measured_matrix():
    """The fixed low-temperature K_full (what the real firmware's coupled
    solve actually uses -- it has no temperature compensation) must report
    the cone-10 hold as infeasible: PID_EXPANSION_PLAN.md secs 3.2/3.4
    already establish the coupled solve goes infeasible above ~62-65 C, so
    it must certainly be infeasible 20x higher.

    Proof this can fail: hardcoded hold_duty_infeasible() to always return
    False. Captured red output:

        FAILED tests/test_plant_sim.py::test_hold_duty_infeasible_at_cone10_with_measured_matrix
        AssertionError: cone-10 hold reported feasible
        assert False
    """
    assert ps.hold_duty_infeasible(1285.0), "cone-10 hold reported feasible"


def test_high_temperature_extension_does_not_change_five_capture_fit():
    """The high-temperature extension must be inert over the range the
    original five hardware captures actually ran (well under T_REF_C=55 C
    peaks) -- loss_conductance_scale() only differs from 1.0 above the
    calibration point, and every one of these captures' targets stays at or
    below it, so re-running the calibration comparison must reproduce the
    same aggregate residual the plain recalibration test already pins."""
    report = ps.render_sim_vs_capture_report(AFTER, integral_floor='ff_u')
    assert report["rms_residual_c"] < 2.0


# ---------------------------------------------------------------------------
# Physical (energy-balance) high-temperature model, added 2026-09-02b --
# see plant_sim.py's "Physical high-temperature model" section for the
# equations/parameters. PhysicalKilnPlant is a DIFFERENT physical object
# from the bench rig (FOPDTPlant/K_full/tau/L): a real cone-10-capable
# kiln, modeled with ASSUMED element/insulation/mass quantities.
# ---------------------------------------------------------------------------

def test_run_profile_rejects_unknown_plant_regime():
    """``plant_regime`` is a closed choice ('measured' or 'physical') --
    silently falling back to one of them on a typo would hide exactly which
    physical object a caller thought they were driving."""
    segs = [(0.0, 100.0, 20.0, 55.0, 0.35), (100.0, 400.0, 55.0, 55.0, 0.0)]
    with pytest.raises(ValueError):
        ps.run_profile(segs, [20.0, 20.0, 20.0], plant_regime='bogus')


def test_physical_hold_duty_feasible_at_cone10():
    """Sanity check demanded by PID_EXPANSION_PLAN.md sec 3.4/3.7: does a
    kiln with plausible element wattage and insulation actually have
    enough steady-state power margin to hold cone 10 (~1285 C) at all? If
    the ASSUMED PHYS_* parameters implied the steady loss at cone 10
    already exceeds the element's own rating, the physical model would be
    exactly the same kind of unusable extrapolation as the bench-rig one
    this section replaces for high temperature.

    Proof this can fail: multiplied PHYS_WALL_THICKNESS_M by 0.1 (a tenth
    the insulation). Captured red output:

        FAILED tests/test_plant_sim.py::test_physical_hold_duty_feasible_at_cone10
        AssertionError: cone-10 steady loss 2246 W needs duty 0.90 of a
        2500 W element -- not a plausible working margin
        assert 0.898... < 0.7

    Reverted, suite green again before this test was kept.
    """
    loss_w = ps.physical_loss_w(np.array([1285.0, 1285.0, 1285.0]))
    duty_needed = loss_w / ps.PHYS_P_MAX_W
    assert bool((duty_needed < 0.7).all()), (
        f"cone-10 steady loss {loss_w} W needs duty {duty_needed} of a "
        f"{ps.PHYS_P_MAX_W} W element -- not a plausible working margin"
    )


def test_physical_ramp_to_cone10_takes_hours_not_minutes_or_days():
    """The commanded ramp schedule the sweep drives (plant_sim_sweep's
    RAMP_RATE_C_PER_S=3 C/min, inside profile 7's own measured 2-3.5 C/min
    range) must reach cone 10 in a believable real-firing timescale -- not
    minutes (an absurdly fast commanded ramp) and not days."""
    ramp_rate_c_per_s = 3.0 / 60.0
    ramp_hours = (1285.0 - 20.0) / ramp_rate_c_per_s / 3600.0
    assert 1.0 < ramp_hours < 24.0, f"cone-10 ramp schedule is {ramp_hours:.1f} h -- not a believable firing"


def test_coupling_growth_is_capped():
    """coupling_growth() must not grow without bound -- see
    COUPLING_GROWTH_CAP's docstring: an uncapped reuse of
    loss_conductance_scale() (fit against the bench rig's T_REF_C=55 C
    anchor) turns cross-zone coupling into unbounded injected power at
    firing temperature, which no amount of a receiving zone's own duty can
    counteract (duty cannot go negative).

    Proof this can fail: patched COUPLING_GROWTH_CAP to
    ``float('inf')``. Captured red output:

        FAILED tests/test_plant_sim.py::test_coupling_growth_is_capped
        AssertionError: coupling_growth(1285.0)=25.45... exceeds any
        plausible cap
        assert 25.45... <= 10.0

    Reverted, suite green again before this test was kept.
    """
    assert ps.coupling_growth(1285.0) <= 10.0, "coupling_growth(1285.0) exceeds any plausible cap"
    assert ps.coupling_growth(1285.0) == pytest.approx(ps.COUPLING_GROWTH_CAP)


def test_physical_kiln_plant_stays_bounded_at_cone10():
    """The full coupled physical-model run at cone 10, driven by the
    EXISTING (fixed low-T matrix, unaware it is extrapolating) coupled
    feedforward controller, must stay within MAX_PLAUSIBLE_TEMP_C and never
    go NaN -- this is the actual failure mode this section exists to catch:
    an under-damped coupling term let two zones' temperature run away
    (duty pinned at 0, temperature still climbing from neighbor coupling)
    until the Newton solve inside solve_outer_wall_temp_c overflowed.

    Proof this can fail: set PHYS_COUPLING_SEPARATION_DAMPING back to 1.0
    (undamped) with COUPLING_GROWTH_CAP raised to 4.0 (the settings tried
    before the damping/cap in this module were tuned down). The
    MAX_PLAUSIBLE_TEMP_C safety clamp caught the divergence before it
    reached NaN, but the run still failed the "landed near the commanded
    target" assertion -- z0/z1 ran away to the clamp ceiling while duty
    sat at 0 (coupling alone was already delivering more power than either
    zone's own element, so no amount of reducing its own duty could pull
    it back down). Captured red output:

        FAILED tests/test_plant_sim.py::test_physical_kiln_plant_stays_bounded_at_cone10
        AssertionError: final temps [2249.78422979 2292.01365176 1150.48599723] not close to cone-10 target 1285 C
        assert False

    (An even less damped/capped combination pins the plant at
    MAX_PLAUSIBLE_TEMP_C and diverges to NaN outright, via overflow in
    solve_outer_wall_temp_c's Newton iteration -- confirmed by hand while
    tuning these constants, not re-captured here since the milder mutation
    above already demonstrates the class of failure this test guards.)

    Reverted, suite green again before this test was kept.
    """
    K_inv = ps._K_INV
    segs = [(0.0, 25300.0, 20.0, 1285.0, (1285.0 - 20.0) / 25300.0),
            (25300.0, 26200.0, 1285.0, 1285.0, 0.0)]
    result = ps.run_profile(segs, [20.0, 20.0, 20.0], climb_mode='coupled', integral_floor='ff_hold',
                             ambient=20.0, controller_K_inv=K_inv, controller_tau=ps.tau,
                             plant_regime='physical')
    temps = result['temps']
    assert not bool(np.isnan(temps).any()), "physical plant produced NaN temperatures at cone 10"
    assert bool((temps <= ps.PhysicalKilnPlant.MAX_PLAUSIBLE_TEMP_C).all())
    # Not just "didn't crash" -- must land close to the commanded target,
    # not merely somewhere finite (e.g. pinned at the safety ceiling).
    final_temps = temps[-1]
    assert bool((np.abs(final_temps - 1285.0) < 50.0).all()), (
        f"final temps {final_temps} not close to cone-10 target 1285 C"
    )


def test_physical_regime_reduces_to_measured_model_below_boundary_by_construction():
    """Below EXTRAPOLATION_BOUNDARY_C, run_profile's default
    plant_regime='measured' is UNCHANGED code (FOPDTPlant with the
    bench-identified K_full/tau/L) -- the physical model is additive, never
    substituted in below the boundary. This is what "reduces to the
    measured bench behaviour" means in practice here: it is not a limit of
    a single unified model, it is the SAME, untouched code path. Pinned by
    reproducing the already-passing five-capture regression through the
    explicit plant_regime='measured' argument (would fail if that default
    or the FOPDTPlant construction inside run_profile ever changed)."""
    report_default = ps.render_sim_vs_capture_report(AFTER)
    result, segs = ps.run_profile_from_capture(
        __import__('kilnctrl.log_analysis', fromlist=['split_runs']).split_runs(
            ps.log_analysis.parse_profile_exec_jsonl(AFTER))[0],
        plant_regime='measured')
    assert result['plant_regime'] == 'measured'
    assert report_default["rms_residual_c"] < 2.0


# ---------------------------------------------------------------------------
# Fuzzy-PID layer -- faithful mirror of firmware/KilnFW/App/drivers/
# pid_fuzzy.c, PID_EXPANSION_PLAN.md sec 3.6. The safety contract this
# mirror exists to check: strength_pct == 0 MUST reproduce the base gains
# bit-for-bit, in BOTH the C source and this Python mirror of it.
# ---------------------------------------------------------------------------

def _simple_profile7_like_segs():
    """Small hand-built two-segment profile (ramp to 45 C, dwell) -- enough
    ticks to exercise every rule-table cell (error crosses zero, rate swings
    both signs during the ramp-to-dwell transition) without needing a real
    capture file for these unit-level fuzzy checks."""
    return [(0.0, 1200.0, 24.0, 45.0, (45.0 - 24.0) / 1200.0),
            (1200.0, 3000.0, 45.0, 45.0, 0.0)]


class _ReferenceNoFuzzyPID:
    """Independent reference implementation: pid_update_terms() with NO
    fuzzy call in the path at all -- not "strength=0 through the fuzzy
    machinery," an entirely separate code path that never imports
    pid_fuzzy_adjust or the bump-transfer rescale. This is what
    ``PID.update`` looked like before the fuzzy layer was wired in, kept
    here so the strength=0 invariant test below compares against ground
    truth rather than the fuzzy-enabled path compared against itself
    (comparing ``run_profile(...)`` against
    ``run_profile(fuzzy_strength_pct=0.0, ...)`` is vacuous: 0.0 is
    already the default, so both calls take the identical code path and
    can never disagree, regardless of what pid_fuzzy_adjust does)."""

    def __init__(self, kp, ki, kd, d_tau, b, pid_range_c):
        self.kp, self.ki, self.kd = kp, ki, kd
        self.d_tau, self.b, self.pid_range_c = d_tau, b, pid_range_c
        self.integral = 0.0
        self.d_filtered = 0.0
        self.prev_measurement = None
        self.initialized = False

    def update(self, setpoint, measurement, dt_s, ff_u, ff_hold, integral_floor='ff_hold'):
        if not self.initialized:
            self.prev_measurement = measurement
            self.integral = 0.0
            self.d_filtered = 0.0
            self.initialized = True
        if dt_s <= 0:
            dt_s = 1.0
        error = setpoint - measurement
        if abs(error) > self.pid_range_c:
            self.prev_measurement = measurement
            u = 1.0 if error > 0 else 0.0
            return u, dict(p=u, i=0.0, d=0.0, ff=0.0)
        raw_d = -(measurement - self.prev_measurement) / dt_s
        alpha = dt_s / (self.d_tau + dt_s)
        self.d_filtered += alpha * (raw_d - self.d_filtered)
        self.prev_measurement = measurement
        p_term = self.kp * (self.b * setpoint - measurement)
        d_term = self.kd * self.d_filtered
        unclamped = p_term + self.ki * self.integral + d_term + ff_u
        would_push_further = (unclamped >= 1.0 and error > 0) or (unclamped <= 0.0 and error < 0)
        if not would_push_further:
            self.integral += error * dt_s
        i_term = self.ki * self.integral
        floor = -ff_u if integral_floor == 'ff_u' else -ff_hold
        if i_term < floor:
            i_term = floor
            self.integral = floor / self.ki if self.ki > 0 else 0.0
        elif i_term > 1.0:
            i_term = 1.0
            self.integral = 1.0 / self.ki if self.ki > 0 else 0.0
        u = p_term + i_term + d_term + ff_u
        u = min(max(u, 0.0), 1.0)
        return u, dict(p=p_term, i=i_term, d=d_term, ff=ff_u)


def _run_profile_with_reference_pid(segs, start_temp, kp, ki, kd,
                                     integral_floor='ff_hold', ambient=20.0):
    """Duplicate of run_profile()'s loop, but driving _ReferenceNoFuzzyPID
    instead of ps.PID -- climb_mode fixed to 'coupled' (matches the
    strength=0 test's kwargs). Kept minimal and deliberately NOT reusing
    ps.PID so a bug in the fuzzy wiring cannot hide behind shared code."""
    plant = ps.FOPDTPlant(ps.K_full, ps.tau, ps.L, ps.DT, ambient=ambient, start_temp=start_temp)
    pids = [_ReferenceNoFuzzyPID(kp, ki, kd, d_tau=30.0, b=1.0, pid_range_c=1000.0)
            for _ in range(ps.N_ZONES)]
    total_t = segs[-1][1]
    times, targets, temps_log, duty_log = [], [], [], []
    duty = np.zeros(ps.N_ZONES)
    t = 0.0
    while t <= total_t:
        for si, (t0, t1, c0, c1, rate) in enumerate(segs):
            if t0 <= t <= t1 or si == len(segs) - 1:
                if rate == 0.0:
                    target_c, target_rate = c1, 0.0
                else:
                    target_c, target_rate = c0 + rate * (t - t0), rate
                break
        for i in range(ps.N_ZONES):
            hold, climb, ff = ps.coupled_ff_hold_climb(target_c, target_rate, i, ambient=ambient)
            duty[i], _ = pids[i].update(target_c, plant.temp[i], ps.DT, ff, hold, integral_floor=integral_floor)
        times.append(t)
        targets.append(target_c)
        temps_log.append(plant.temp.copy())
        duty_log.append(duty.copy())
        plant.step(duty)
        t += ps.DT
    return dict(t=np.array(times), target=np.array(targets),
                temps=np.array(temps_log), duty=np.array(duty_log))


def test_fuzzy_strength_zero_matches_base_gains_bit_for_bit():
    """strength_pct=0 must be bit-for-bit identical to the fuzzy layer being
    ABSENT -- the safety contract pid_fuzzy.c's own comment states for the
    firmware function. Compared against ``_ReferenceNoFuzzyPID`` (an
    independent implementation that never calls pid_fuzzy_adjust or the
    bump-transfer rescale at all), not against another ``fuzzy_strength_pct
    =0.0`` call -- see ``_ReferenceNoFuzzyPID``'s docstring for why that
    comparison would be vacuous. Checked on temps AND duty, every zone,
    every tick.

    Proof this test can fail (required by repo policy): temporarily changed
    the mirror's short-circuit from `if strength_pct == 0.0: return kp, ki,
    kd` to `if strength_pct == 0.0: return kp, ki, kd * 1.0000001` (the
    smallest deliberate perturbation that still reads as "no-op" on a quick
    skim). Captured red output:

        FAILED tests/test_plant_sim.py::test_fuzzy_strength_zero_matches_base_gains_bit_for_bit
        AssertionError:
        Arrays are not equal
        Mismatched elements: 8866 / 9003 (98.5%)
        First 5 mismatches are at indices:
         [36, 2]: 23.667662474178034 (ACTUAL), 23.667662474173476 (DESIRED)
        ...
        Max absolute difference among violations: 3.15940625e-08

    Reverted before this test was kept; suite green again.
    """
    segs = _simple_profile7_like_segs()
    start = [24.0, 24.0, 24.0]
    kwargs = dict(kp=0.0318, ki=0.0001, kd=0.8401, integral_floor='ff_hold', ambient=20.0)

    result_fuzzy_zero = ps.run_profile(segs, start, climb_mode='coupled',
                                        fuzzy_strength_pct=0.0, **kwargs)
    result_reference = _run_profile_with_reference_pid(segs, start, **kwargs)

    np.testing.assert_array_equal(result_fuzzy_zero['temps'], result_reference['temps'])
    np.testing.assert_array_equal(result_fuzzy_zero['duty'], result_reference['duty'])


def test_pid_fuzzy_adjust_strength_zero_returns_base_gains_exactly():
    """Unit-level check on pid_fuzzy_adjust() itself (not run_profile): at
    strength_pct=0, output must equal the (sanitized) base gains regardless
    of error/rate, including non-finite inputs -- mirrors pid_fuzzy.c's own
    documented contract line-for-line."""
    for error_c, rate in [(0.0, 0.0), (50.0, -2.0), (-100.0, 5.0),
                           (float('nan'), 1.0), (1.0, float('inf'))]:
        kp, ki, kd = ps.pid_fuzzy_adjust(error_c, rate, 0.0318, 0.0001, 0.8401, 0.0)
        assert (kp, ki, kd) == (0.0318, 0.0001, 0.8401)


def test_pid_fuzzy_adjust_nonzero_strength_changes_gains():
    """Sanity check that the harness CAN see a difference -- strength=100 at
    a rule-table cell with a nonzero direction must move the gain away from
    base. Guards against a mirror that accidentally always returns the
    identity (which would make the strength-zero test above vacuous)."""
    # error=+30 (POS, beyond the 20C band -> e_pos=1), rate=0 (STEADY,
    # r_zero=1): rule cell (POS, STEADY) = {Kp+, Ki=, Kd=} in pid_fuzzy.h's
    # table.
    kp, ki, kd = ps.pid_fuzzy_adjust(30.0, 0.0, 0.0318, 0.0001, 0.8401, 100.0)
    assert kp > 0.0318
    assert ki == pytest.approx(0.0001)
    assert kd == pytest.approx(0.8401)


def test_fuzzy_strength_nonzero_diverges_from_zero_over_a_run():
    """End-to-end confirmation that a nonzero strength actually changes the
    simulated trajectory (not just the single-tick gain check above) --
    otherwise a wiring bug in PID.update (e.g. computing adjusted gains but
    never assigning self.kp/ki/kd) could hide behind the unit test."""
    segs = _simple_profile7_like_segs()
    start = [24.0, 24.0, 24.0]
    kwargs = dict(kp=0.0318, ki=0.0001, kd=0.8401, climb_mode='coupled',
                   integral_floor='ff_hold', ambient=20.0)
    r0 = ps.run_profile(segs, start, fuzzy_strength_pct=0.0, **kwargs)
    r100 = ps.run_profile(segs, start, fuzzy_strength_pct=100.0, **kwargs)
    assert not np.allclose(r0['temps'], r100['temps'])
