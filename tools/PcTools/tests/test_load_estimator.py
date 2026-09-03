"""Mutation-tested checks for ``kilnctrl.load_estimator`` -- see its module
docstring and ``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` sec 3.8 for
what this module is for. Validates the duty-vs-rate mass-multiplier fit
against ``load_mass_sweep.py``'s known-truth simulated loads (where the
estimator recovers the injected multiplier to within 1%), and exercises it
against the real profile-7 captures without asserting a specific number
(the doc records what those returned and why they should not be trusted)."""
import glob
import os

import numpy as np
import pytest

from kilnctrl import load_estimator as le
from kilnctrl import load_mass_sweep as lms
from kilnctrl import log_analysis as la
from kilnctrl import plant_sim as ps

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "plant_sim")


@pytest.mark.parametrize("mass_mult", [1.0, 1.5, 2.0, 3.0, 4.0])
def test_recovers_known_mass_multiplier_in_sim(mass_mult):
    """load_mass_sweep.run_profile7_loaded injects a KNOWN mass_mult into
    the plant; the estimator, given only duty/temperature, must recover it
    to within 1% -- this is the noise-free "is the math right" check."""
    res = lms.run_profile7_loaded(mass_mult, 1.0)
    ests = le.estimate_all_zones(res['t'], res['temps'], res['duty'], min_drive_c=5.0)
    assert len(ests) == 3
    for e in ests:
        assert abs(e.mass_mult_est - mass_mult) / mass_mult < 0.01, (
            f"zone {e.zone}: estimated {e.mass_mult_est:.3f} vs true {mass_mult}")
        assert e.r2 > 0.9


def test_recovers_with_realistic_sensor_noise():
    """0.1 C quantization + 0.05 C gaussian noise (the honesty gate's own
    measurement chain) barely moves the estimate -- confirms sensor noise is
    NOT what limits this estimator on real hardware (model mismatch is;
    see the real-capture test below and the doc)."""
    rng = np.random.default_rng(7)
    res = lms.run_profile7_loaded(2.0, 1.0)
    temps = res['temps'] + rng.normal(0, 0.05, res['temps'].shape)
    temps = np.round(temps / 0.1) * 0.1
    ests = le.estimate_all_zones(res['t'], temps, res['duty'], min_drive_c=5.0)
    for e in ests:
        assert abs(e.mass_mult_est - 2.0) < 0.05


def test_dead_time_warmup_exclusion_is_load_bearing():
    """Without excluding the first L[zone] seconds of the window (the
    duty-history-before-the-window-starts problem -- see the long comment
    in estimate_zone_mass_mult), a 4.0x sim run reads back at ~4.5x, a >10%
    error that would have been reported as a real finding. This test
    fails if that exclusion is ever removed or weakened."""
    res = lms.run_profile7_loaded(4.0, 1.0)
    t, temps, duty = res['t'], res['temps'], res['duty']
    est = le.estimate_zone_mass_mult(t, temps, duty, zone=0, min_drive_c=5.0)
    assert est is not None
    # With the exclusion in place this is ~3.997; bound well clear of the
    # ~4.53 the unguarded fit produces (measured directly during
    # development -- see module docstring).
    assert abs(est.mass_mult_est - 4.0) < 0.1


def test_refuses_non_physical_negative_slope():
    """Synthetic data where dT/dt anti-correlates with drive (e.g. cooling
    while nominally driven) must not be reported as a mass multiplier --
    the estimator should refuse (return None) rather than emit a negative
    or infinite tau silently."""
    n = 50
    t = np.arange(n, dtype=float)
    duty = np.zeros((n, 3))
    duty[:, 0] = 0.8
    temps = np.zeros((n, 3))
    temps[:, 0] = 50.0 - 0.1 * t  # cooling despite full duty on zone 0
    temps[:, 1] = 20.0
    temps[:, 2] = 20.0
    est = le.estimate_zone_mass_mult(t, temps, duty, zone=0, min_drive_c=1.0)
    assert est is None


def test_too_few_samples_returns_none():
    t = np.arange(2, dtype=float)
    temps = np.full((2, 3), 20.0)
    duty = np.zeros((2, 3))
    assert le.estimate_zone_mass_mult(t, temps, duty, zone=0) is None


def test_real_capture_loader_handles_both_envelopes():
    """logs/coupling/*_http.jsonl uses the {"t","exec","status"} wrapper;
    tests/fixtures/plant_sim/*.jsonl uses the bare "HH:MM:SS {...}" poll
    form. load_capture_rows must auto-detect either."""
    wrapped = os.path.join(REPO_ROOT, "logs", "coupling", "p7_fuzzy0_http.jsonl")
    bare = os.path.join(FIXTURES, "final.jsonl")
    assert os.path.exists(wrapped) and os.path.exists(bare)
    rows_wrapped = le.load_capture_rows(wrapped)
    rows_bare = le.load_capture_rows(bare)
    assert len(rows_wrapped) > 100
    assert len(rows_bare) > 100
    assert rows_wrapped[0].zones and rows_bare[0].zones


# ---------------------------------------------------------------------------
# Per-path dead time reconstruction (added 2026-09-03) -- sec 3.8's leading
# suspect for the negative real-capture R^2 was that ``_delay_duty`` applied
# one dead time per RECEIVING zone to every column, when a neighbour's true
# dead time (135-158 s) runs 3-4x the diagonal's (34-53 s). Drives
# ``FOPDTPlantPerPath`` (the per-path plant, ``plant_sim.py``) with duty on
# zone 1 ONLY -- zone 0's whole drive term comes through the off-diagonal
# path, so getting that path's dead time right is the entire test.
# ---------------------------------------------------------------------------

def test_per_path_l_pair_recovers_off_diagonal_drive_better_than_uniform_delay():
    """Simulate ``FOPDTPlantPerPath`` (the true per-path model) with zone 1
    duty stepped and zones 0/2 held at zero -- zone 0's whole temperature
    rise is cross-zone (off-diagonal) heat. Estimating zone 0's mass
    multiplier with the CORRECT per-path delay (``ps.L_PAIR``, ~146.5 s off
    diagonal) must fit far better (higher R^2) than reconstructing the same
    data with the OLD single-delay-per-zone assumption (every column
    delayed by zone 0's own ~52.8 s dead time) -- exactly the mismatch sec
    3.8 named as its leading suspect for the real captures' negative R^2.

    Proof this can fail: passed ``L_pair=OLD_UNIFORM`` to BOTH calls (as if
    ``estimate_zone_mass_mult``'s new ``L_pair`` argument were silently
    ignored / not threaded through). Captured red output:

        FAILED tests/test_load_estimator.py::test_per_path_l_pair_recovers_off_diagonal_drive_better_than_uniform_delay
        AssertionError: correct per-path R2 (0.450) not clearly better than
          uniform-delay R2 (0.450) -- L_pair argument looks inert
        assert 0.44974222422830656 > (0.44974222422830656 + 0.2)

    Reverted, suite green again before this test was kept.
    """
    plant = ps.FOPDTPlantPerPath(ps.K_full, ps.L_PAIR, ps.TAU_PAIR, ps.DT, ambient=20.0)
    n_steps = 2400
    duty = np.zeros(3)
    duty[1] = 0.7
    temps = np.zeros((n_steps, 3))
    dutys = np.zeros((n_steps, 3))
    for k in range(n_steps):
        temps[k] = plant.step(duty)
        dutys[k] = duty
    t = np.arange(n_steps, dtype=float) * ps.DT

    OLD_UNIFORM = np.tile(ps.L.reshape(-1, 1), (1, 3))  # old: one delay per receiving zone

    est_correct = le.estimate_zone_mass_mult(t, temps, dutys, zone=0, min_drive_c=1.0, L_pair=ps.L_PAIR)
    est_wrong = le.estimate_zone_mass_mult(t, temps, dutys, zone=0, min_drive_c=1.0, L_pair=OLD_UNIFORM)
    assert est_correct is not None and est_wrong is not None
    assert est_correct.r2 > est_wrong.r2 + 0.2, (
        f"correct per-path R2 ({est_correct.r2:.3f}) not clearly better than "
        f"uniform-delay R2 ({est_wrong.r2:.3f}) -- L_pair argument looks inert"
    )
    assert est_correct.r2 > 0.9


def test_real_captures_produce_finite_estimates_no_crash():
    """Runs the estimator against every real profile-7 capture this repo
    has (logs/coupling/p7_*_http.jsonl + the rested-start fixtures) --
    smoke-level only: every zone's estimate must be a finite, positive
    number (the estimator's own non-physical-slope guard already rules out
    the alternative). Does NOT assert consistency or a tolerance band --
    the doc records that real-capture per-segment estimates for zone 0
    cluster loosely (~0.4-0.6x) while zones 1/2 do not cluster at all and
    R2 is frequently negative, i.e. this capture set does not support a
    trustworthy load number. This test only guards against the loader or
    estimator crashing/returning nonsense on real data, not against that
    (already-documented) inconsistency regressing further."""
    paths = sorted(glob.glob(os.path.join(REPO_ROOT, "logs", "coupling", "p7_*_http.jsonl")))
    assert paths, "expected at least one real profile-7 capture"
    seen_any = False
    for p in paths:
        rows_all = le.load_capture_rows(p)
        for run in la.split_runs(rows_all):
            if len(run) < 60:
                continue
            t, temps, duty = le.arrays_from_capture(run)
            ests = le.estimate_all_zones(t, temps, duty, min_drive_c=5.0)
            for e in ests:
                seen_any = True
                assert np.isfinite(e.mass_mult_est)
                assert e.mass_mult_est > 0
    assert seen_any
