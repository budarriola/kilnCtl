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
