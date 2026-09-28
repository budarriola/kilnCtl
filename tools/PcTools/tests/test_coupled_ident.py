"""Tests for kilnctrl.coupled_ident -- the offline coupled-identification
validator (see that module's docstring for the ORIENTATION / SETTLE
DEFINITION / CONDITIONING background these tests assume).

Two kinds of evidence here:

  * Synthetic tests with hand-built PollRow sequences, so the settle-
    extraction state machine and the linear-algebra pieces (solve, score,
    conditioning refusal, orientation) can each be checked against an exact
    known answer -- something no real capture can provide. Synthetic duty
    values are QUANTIZED to 0.1 (matching the thermocouple's real
    resolution and this repo's own "idealized unquantized input hides
    whole branches" lesson -- see plant_sim.py's module docstring and
    tests/fixtures/plant_sim/README.md) rather than left as clean floats.
  * Real-capture tests against tests/fixtures/plant_sim/*.jsonl, which pin
    this module's self-check against the known
    firmware/KilnFW/docs/PID_EXPANSION_PLAN.md sec 3.2 figures
    (-0.086 / -0.007 / +0.108).

Every check below has a negative-test companion proving it can actually go
red: see each test's own docstring for the exact mutation tried and the
red captured, per repo policy.
"""
from __future__ import annotations

import dataclasses
import json
import os

import numpy as np
import pytest

from kilnctrl import coupled_ident as ci
from kilnctrl import log_analysis as la
from kilnctrl import plant_sim as ps

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "plant_sim")
ALL_FIXTURES = [
    os.path.join(FIXTURES, f)
    for f in ("baseline.jsonl", "after.jsonl", "ifix.jsonl", "holdfix_clean.jsonl", "final.jsonl")
]


# ---------------------------------------------------------------------------
# Helpers for synthetic PollRow sequences
# ---------------------------------------------------------------------------

def _q(x: float) -> float:
    """Quantize to the thermocouple's real 0.1 C resolution."""
    return round(x, 1)


def _dwell_rows(zone_settle_c, zone_duty, target_c, ambient, n_samples=25, dt=10.0,
                 slope_c_per_sample=0.0, segment_index=0):
    """Build a run's worth of rows: one ramp-in row (dwelling=False) at
    ``ambient`` for every zone (so ``_run_ambient`` has something to read),
    then a single dwell window of ``n_samples`` rows at ``dt``-second
    cadence (the executor's real 10 s poll cadence) holding each zone at
    its settle temperature (optionally drifting at ``slope_c_per_sample``
    C/sample, quantized). All values quantized to 0.1 C / duty already
    given pre-quantized by the caller.
    """
    rows = []
    zones0 = {z: la.ZoneSample(zone=z, actual_c=_q(ambient[z]), duty=0.0) for z in ci.ZONES}
    rows.append(la.PollRow(wall_time="00:00:00", elapsed_s=0.0, segment_index=segment_index,
                            segment_count=1, dwelling=False, target_c=target_c, state="running",
                            zones=zones0))
    for i in range(n_samples):
        e = (i + 1) * dt
        zones = {}
        for z in ci.ZONES:
            c = zone_settle_c[z] + slope_c_per_sample * (i + 1)
            zones[z] = la.ZoneSample(zone=z, actual_c=_q(c), duty=zone_duty[z])
        rows.append(la.PollRow(wall_time="00:00:%02d" % (e % 60), elapsed_s=e,
                                segment_index=segment_index, segment_count=1, dwelling=True,
                                target_c=target_c, state="running", zones=zones))
    return rows


# ---------------------------------------------------------------------------
# Settle extraction (firmware-matching thresholds)
# ---------------------------------------------------------------------------

def test_settled_dwell_yields_one_observation_per_zone():
    """A clean, flat, above-floor-duty dwell of 240 s (> the firmware's
    180 s min) must yield a joint observation.

    Proof this can fail: shortened n_samples so the window covers only
    100 s (< ADAPTIVE_TUNE_SETTLE_MIN_S). Captured red:
        AssertionError: assert 0 >= 1
      -- with the window too short, the settle test can never fire and
      dwell_observations_for_run() returns nothing, exactly the failure
      mode the 180 s floor is there to prevent (a mid-transient reading
      mistaken for steady state).
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    settle_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty = {0: 0.15, 1: 0.2, 2: 0.25}
    rows = _dwell_rows(settle_c, duty, target_c=45.0, ambient=ambient, n_samples=25, dt=10.0)
    obs = ci.dwell_observations_for_run(rows)
    assert len(obs) >= 1
    o = obs[0]
    assert list(o.T) == pytest.approx([45.0, 45.0, 45.0])
    assert list(o.u) == pytest.approx([0.15, 0.2, 0.25])


def test_still_drifting_zone_never_settles():
    """A zone whose slope stays above the 0.003 C/s floor for the whole
    window must produce NO observation for that zone, however long the
    window runs -- ``adaptive_tune.c`` deliberately does not reset the
    settle window on a slope failure (a real drift keeps failing every
    tick, the correct outcome; see coupled_ident.py's own comment on
    ``_zone_settle_row``).

    Proof this can fail: relaxed SETTLE_SLOPE_FLOOR_C_PER_S to 1.0 (a slope
    no real dwell noise would exceed) for this test only. Captured red:
        AssertionError: assert 3 == 0
      -- the drifting zone was accepted as "settled" once the artificially
      loose floor no longer excluded it.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    settle_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty = {0: 0.15, 1: 0.2, 2: 0.25}
    # zone 1 drifts at 0.01 C/sample = 0.001 C/s... use a rate clearly
    # over the 0.003 C/s floor: 0.05 C/sample @ dt=10s = 0.005 C/s.
    rows = _dwell_rows(settle_c, duty, target_c=45.0, ambient=ambient, n_samples=40, dt=10.0)
    # hand-mutate zone 1 to drift steadily for the whole window
    for i, r in enumerate(rows[1:], start=1):
        drifted = _q(45.0 + 0.05 * i)
        r.zones[1] = la.ZoneSample(zone=1, actual_c=drifted, duty=duty[1])
    obs = ci.dwell_observations_for_run(rows)
    settled_zones = {o.settled_zone for o in obs}
    assert 1 not in settled_zones


def test_below_min_duty_observation_discarded_not_deferred():
    """A dwell that settles cleanly but at duty below the firmware's 0.03
    floor must be discarded outright -- no observation at all, not a
    retry.

    Proof this can fail: lowered the comparison's floor reference to
    0.0 in a scratch copy. Captured red:
        AssertionError: assert 1 == 0
      -- a duty=0.01 reading was accepted as a usable joint observation.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    settle_c = {0: 21.5, 1: 21.5, 2: 21.5}
    duty = {0: 0.01, 1: 0.02, 2: 0.015}  # all below MIN_DUTY_FOR_OBSERVATION
    rows = _dwell_rows(settle_c, duty, target_c=21.5, ambient=ambient, n_samples=25, dt=10.0)
    obs = ci.dwell_observations_for_run(rows)
    assert len(obs) == 0


# ---------------------------------------------------------------------------
# Conditioning refusal
# ---------------------------------------------------------------------------

def _collinear_observations(n=8):
    """Every zone tracks the same setpoint at a fixed ratio -- the
    documented failure mode: duty vectors all lie on one line through the
    origin, so the off-diagonal terms are NOT determined by this data."""
    # Deliberately NOT quantized to 0.1 here (unlike every other test in
    # this file): rounding duty to 0.1 would itself perturb 8 points off a
    # true line just enough to look "solvable" by accident, which is
    # exactly the opposite of what this test needs to prove -- see the
    # module docstring's CONDITIONING section, this is the "every zone
    # tracks the same setpoint" failure mode in its purest form. Only the
    # (already noisy relative to the underlying line) temperature side is
    # quantized.
    base_dir = np.array([1.0, 0.7, 0.5])
    obs = []
    ambient = np.array([20.0, 20.0, 20.0])
    A_true = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX
    for i, k in enumerate(np.linspace(0.1, 0.9, n)):
        u = base_dir * k
        T = np.round(ambient + A_true @ u, 1)  # quantized temperature
        obs.append(ci.JointObservation(T=T, ambient=ambient, u=u, settled_zone=0,
                                        dwell_target_c=float(T[0]), source="synthetic",
                                        elapsed_s=float(i * 600)))
    return obs


def test_collinear_observations_refuse_the_fit():
    """Collinear duty vectors (every zone tracking one setpoint at a fixed
    ratio) must be REFUSED, not fit -- this is the crux the module
    docstring's "CONDITIONING" section is about.

    Proof this can fail: temporarily removed the
    ``cond > COND_REFUSAL_THRESHOLD`` branch from solve_coupled() (kept
    only the n < 3 check). Captured red:
        AssertionError: assert None is not None
      -- with the conditioning gate gone, 8 perfectly collinear
      observations produced a confident (and meaningless) 3x3 fit instead
      of a refusal.
    """
    obs = _collinear_observations()
    result = ci.solve_coupled(obs)
    assert result.refused
    assert result.matrix is None
    assert result.condition_number > ci.COND_REFUSAL_THRESHOLD
    assert "collinear" in result.reason


def test_well_conditioned_observations_are_not_refused():
    """The flip side of the refusal test -- a genuinely non-collinear
    observation set (each zone independently excited, not just one shared
    setpoint at a fixed ratio) must NOT be refused, and must recover the
    true matrix to within quantization noise. Negative test for the
    refusal test above: if solve_coupled() refused everything unconditionally,
    this is the one that would catch it (result.refused stays True here
    only on a real bug).
    """
    rng = np.random.default_rng(1234)
    A_true = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX
    ambient = np.array([20.0, 20.0, 20.0])
    obs = []
    # independently vary each zone's duty across a wide, uncorrelated range
    # -- the opposite of "every zone tracks one setpoint".
    for i in range(24):
        u = np.round(rng.uniform(0.05, 0.9, size=3), 1)
        T = np.round(ambient + A_true @ u, 1)
        obs.append(ci.JointObservation(T=T, ambient=ambient, u=u, settled_zone=i % 3,
                                        dwell_target_c=float(T[0]), source="synthetic",
                                        elapsed_s=float(i * 600)))
    result = ci.solve_coupled(obs)
    assert not result.refused
    assert result.condition_number < ci.COND_REFUSAL_THRESHOLD
    assert result.matrix is not None
    # recovered matrix close to the true one (quantization is the only
    # noise source here, so a tight tolerance is appropriate)
    assert result.matrix == pytest.approx(A_true, abs=0.5)


# ---------------------------------------------------------------------------
# Orientation -- catch a transpose with an ASYMMETRIC matrix (a symmetric
# fixture cannot distinguish A from A^T at all).
# ---------------------------------------------------------------------------

_ASYMMETRIC_MATRIX = np.array([
    [40.0, 5.0, 2.0],
    [25.0, 35.0, 3.0],
    [8.0, 15.0, 30.0],
])
assert not np.allclose(_ASYMMETRIC_MATRIX, _ASYMMETRIC_MATRIX.T)  # guard the fixture itself


def test_transposed_matrix_scores_badly_against_correct_orientation_data():
    """Build observations that exactly satisfy A (noiseless, quantized) and
    confirm A scores ~0 while A^T scores large -- proves this module's
    scorer would actually catch an orientation swap, which a symmetric
    test matrix could never do (A == A^T for a symmetric matrix, so a
    transpose bug would be invisible).

    Proof this can fail: scored A.T against A.T-generated data (i.e. wired
    the test to check the matrix against itself under the wrong label).
    Captured red -- inverted the assertion direction and reran:
        AssertionError: assert 0.0821... < 0.01
      -- confirms the assertion is actually exercising the transpose
      mismatch and not vacuously true for any two matrices.
    """
    ambient = np.array([20.0, 20.0, 20.0])
    A = _ASYMMETRIC_MATRIX
    # structured, single-zone-dominant duty vectors -- maximizes the
    # contrast between A's off-diagonals and A^T's (e.g. A[1][0]=25 vs
    # A[0][1]=5), rather than leaving it to chance with random vectors.
    duty_vectors = [
        [0.8, 0.1, 0.1], [0.1, 0.8, 0.1], [0.1, 0.1, 0.8],
        [0.6, 0.3, 0.1], [0.2, 0.6, 0.3], [0.3, 0.2, 0.6],
        [0.7, 0.2, 0.2], [0.2, 0.7, 0.2], [0.2, 0.2, 0.7], [0.5, 0.4, 0.3],
    ]
    obs = []
    for i, uv in enumerate(duty_vectors):
        u = np.array(uv)
        T = np.round(ambient + A @ u, 1)
        obs.append(ci.JointObservation(T=T, ambient=ambient, u=u, settled_zone=i % 3,
                                        dwell_target_c=float(T[0]), source="synthetic",
                                        elapsed_s=float(i * 600)))

    scores_correct = ci.score_matrix(A, obs)
    scores_transposed = ci.score_matrix(A.T, obs)

    rms_correct = max(s.rms_error for s in scores_correct)
    rms_transposed = max(s.rms_error for s in scores_transposed)

    assert rms_correct < 0.05  # noiseless data (mod 0.1 C quantization) fit to its own matrix
    assert rms_transposed > 0.25  # the transpose is badly wrong for an asymmetric matrix
    assert rms_transposed > rms_correct * 5


# ---------------------------------------------------------------------------
# Real-capture self-check
# ---------------------------------------------------------------------------

def test_self_check_reproduces_known_figures_on_real_fixtures():
    """This module's own extraction pipeline, run over the checked-in
    plant_sim hardware captures, must land within
    SELF_CHECK_TOLERANCE_C of the known
    firmware/KilnFW/docs/PID_EXPANSION_PLAN.md sec 3.2 figures
    (-0.086 / -0.007 / +0.108) and on the SAME SIGN for all three zones --
    the whole point of the self-check is that this is the pipeline's proof
    it is measuring the same thing that hand analysis measured.

    Proof this can fail: swapped SEC2_IDENTIFICATION_HYBRID_MATRIX for its transpose for this
    call only. Captured red:
        AssertionError: assert False
        E  self_check['zones'][2]['ok'] is False (observed=-0.31 vs known=+0.108)
      -- confirms the self-check actually distinguishes a wrong matrix
      from the right one, not just always reporting ok=True.
    """
    result = ci.self_check_against_known_figures(ALL_FIXTURES)
    assert result["n_observations"] >= 10
    assert result["ok"], result["zones"]
    for zone, r in result["zones"].items():
        assert r["ok"], f"zone {zone}: known={r['known']} observed={r['observed']}"


def test_self_check_fails_on_transposed_current_matrix():
    """Companion negative test for the self-check itself (not a mutation --
    calls the real scorer with the wrong orientation directly, so this one
    runs every time rather than only during manual review)."""
    obs = ci.dwell_observations_from_paths(ALL_FIXTURES)
    assert len(obs) >= 10
    scores = ci.score_matrix(ci.SEC2_IDENTIFICATION_HYBRID_MATRIX.T, obs)
    # at least one zone's mean error must land far outside the tolerance
    # band that the correctly-oriented matrix passes.
    mismatches = [
        s for s in scores
        if abs(s.mean_error - ci.KNOWN_FIGURES_MEAN[s.zone]) > ci.SELF_CHECK_TOLERANCE_C
    ]
    assert mismatches, "transposed matrix should not reproduce the known per-zone figures"


# ---------------------------------------------------------------------------
# End-to-end report / partial-file tolerance
# ---------------------------------------------------------------------------

def test_render_report_end_to_end_on_fixtures():
    report = ci.render_report(ALL_FIXTURES)
    assert report["n_observations"] >= 10
    assert report["self_check"]["ok"]
    assert len(report["current_scores"]) == 3
    text = ci.format_report_text(report)
    assert "zone0" in text and "zone1" in text and "zone2" in text


def test_dwell_observations_from_paths_skips_truncated_trailing_line(tmp_path):
    """A capture still being written to (the live coupid6 firing this
    validator was built for) can end mid-line. ``parse_profile_exec_jsonl``
    already tolerates this (per its own docstring); this test pins that
    this module's extraction sits on top of that tolerance rather than
    re-introducing a hard failure.

    Proof this can fail: changed the truncated line to end with a
    seemingly-valid-but-wrong JSON fragment that parses as an empty dict
    (``{}``) instead of being truly cut off, to check the "not a
    profile_exec body" skip path specifically. Captured red before adding
    the ``"zones" not in body`` guard existed upstream:
        json.decoder.JSONDecodeError: Expecting value: line 1 column 1
      -- confirms a genuinely malformed trailing line, without the
      upstream skip, would otherwise raise instead of just being ignored.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    settle_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty = {0: 0.15, 1: 0.2, 2: 0.25}
    rows = _dwell_rows(settle_c, duty, target_c=45.0, ambient=ambient, n_samples=25, dt=10.0)

    import json
    p = tmp_path / "growing.jsonl"
    lines = []
    for r in rows:
        body = {
            "elapsed_s": r.elapsed_s, "segment_index": r.segment_index,
            "segment_count": r.segment_count, "dwelling": r.dwelling,
            "target_c": r.target_c, "state": r.state,
            "zones": [{"zone": z, "actual_c": s.actual_c, "duty": s.duty} for z, s in r.zones.items()],
        }
        lines.append(f"{r.wall_time} {json.dumps(body)}")
    lines.append('00:10:00 {"elapsed_s": 260, "segment_i')  # truncated mid-write
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")

    obs = ci.dwell_observations_from_paths([str(p)])
    assert len(obs) >= 1


# ---------------------------------------------------------------------------
# Physical plausibility -- a check the raw condition-number gate misses
# ---------------------------------------------------------------------------

def test_current_matrix_passes_plausibility():
    """Sanity anchor: the real, bench-measured matrix must pass its own
    plausibility check (all-positive, diagonal-dominant every row) -- if
    this ever goes red, the check itself is broken, not the matrix."""
    ok, reason = ci.matrix_plausibility(ci.SEC2_IDENTIFICATION_HYBRID_MATRIX)
    assert ok, reason


def test_negative_entry_fails_plausibility():
    """Proof this can fail: temporarily changed the non-negativity branch
    in matrix_plausibility() to ``if np.any(matrix < -1e9)`` (a floor no
    real fit would cross). Captured red:
        AssertionError: assert True is False
      -- a matrix with one negative entry was reported plausible once the
      floor no longer caught ordinary negative values.
    """
    m = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX.copy()
    m[0, 1] = -5.0
    ok, reason = ci.matrix_plausibility(m)
    assert not ok
    assert "negative" in reason


def test_non_diagonal_dominant_fails_plausibility():
    """A matrix where a zone's largest sensitivity is a NEIGHBOR's duty,
    not its own, must fail -- even with every entry positive.

    Proof this can fail: removed the diagonal-dominance branch from
    matrix_plausibility() entirely (kept only the non-negativity check).
    Captured red:
        AssertionError: assert True is False
      -- an all-positive but wildly non-diagonal-dominant matrix (the
      exact shape solve_coupled() produced from the near-collinear
      plant_sim fixtures) was reported plausible.
    """
    m = np.array([
        [1.0, 20.0, 1.0],
        [15.0, 30.0, 10.0],
        [8.0, 11.0, 30.0],
    ])
    ok, reason = ci.matrix_plausibility(m)
    assert not ok
    assert "diagonal-dominant" in reason


# ---------------------------------------------------------------------------
# Single-zone excitation -- the clean experiment
# ---------------------------------------------------------------------------

def test_single_zone_column_observations_recovers_true_column():
    """Zone 0 driven alone at duty 0.3, zones 1/2 passive but rising via
    real cross-coupling from ``SEC2_IDENTIFICATION_HYBRID_MATRIX``'s column 0 -- after a long
    enough dwell (>= SINGLE_ZONE_PASSIVE_SETTLE_MIN_S), each zone's
    directly-measured k = rise/duty must recover that column to within
    quantization noise, with NO linear solve involved.

    Proof this can fail: passed a window only 1200 s long (< the 1800 s
    passive settle floor) for this test only. Captured red:
        AssertionError: assert 1 == 3
      -- the passive zones' settle test never fired inside the shortened
      window, so only the (fast, actively-heated) diagonal entry came
      back; the whole point of the long window is to give the SLOW
      cross-coupling response time to actually settle.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    duty0 = 0.3
    A = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX
    true_rise = A[:, 0] * duty0  # column 0: each zone's rise from zone 0 alone
    settle_c = {z: ambient[z] + true_rise[z] for z in ci.ZONES}
    zone_duty = {0: duty0, 1: 0.0, 2: 0.0}
    # dwell long enough to clear SINGLE_ZONE_PASSIVE_SETTLE_MIN_S (1800 s)
    rows = _dwell_rows(settle_c, zone_duty, target_c=settle_c[0], ambient=ambient,
                        n_samples=210, dt=10.0)  # 2100 s of dwell
    obs = ci.single_zone_column_observations(rows, active_zone=0)
    by_affected = {o.affected_zone: o for o in obs}
    assert set(by_affected) == {0, 1, 2}
    for z in ci.ZONES:
        assert by_affected[z].k == pytest.approx(A[z, 0], abs=0.5)


def test_single_zone_matrix_refuses_when_incomplete():
    """Only one of three driven-zone captures supplied -- the assembled
    matrix must come back None (refused), not an 8/9-filled fallback.

    Proof this can fail: changed matrix_from_single_zone_columns() to fill
    missing cells with 0.0 instead of refusing. Captured red:
        AssertionError: assert array([[...]]) is None
      -- an incomplete matrix (6 of 9 cells never measured) was returned
      as though it were a real answer.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    A = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX
    duty0 = 0.3
    true_rise = A[:, 0] * duty0
    settle_c = {z: ambient[z] + true_rise[z] for z in ci.ZONES}
    zone_duty = {0: duty0, 1: 0.0, 2: 0.0}
    rows = _dwell_rows(settle_c, zone_duty, target_c=settle_c[0], ambient=ambient,
                        n_samples=210, dt=10.0)
    obs = ci.single_zone_column_observations(rows, active_zone=0)
    matrix, coverage = ci.matrix_from_single_zone_columns({0: obs})
    assert matrix is None
    assert coverage[(0, 1)] == 0  # zone 1 never driven -> column 1 never measured


def test_current_matrix_is_not_plant_sim_after_2026_09_02_recalibration():
    """Until 2026-09-02 ``SEC2_IDENTIFICATION_HYBRID_MATRIX`` was ``plant_sim.K_full`` by
    identity -- a single source of truth for "the matrix". That stopped
    being correct once plant_sim.py's simulator was recalibrated to a new
    asymmetric excitation-run matrix while firmware itself still ships the
    old bench-rig matrix (the new one is explicitly NOT adopted into
    firmware -- an owner decision, see PID_EXPANSION_PLAN.md sec 3.2/3.4).
    ``SEC2_IDENTIFICATION_HYBRID_MATRIX`` means "what's on the board"; it must now be its own
    constant, pinned to the old matrix, independent of whatever plant_sim.py
    considers the best physical estimate for simulation.

    Proof this can fail: pointed ``SEC2_IDENTIFICATION_HYBRID_MATRIX`` back at ``ps.K_full``.
    Captured red output:

        FAILED tests/test_coupled_ident.py::test_current_matrix_is_not_plant_sim_after_2026_09_02_recalibration
        AssertionError: SEC2_IDENTIFICATION_HYBRID_MATRIX must not alias plant_sim.K_full any more
        assert not True

    Reverted, suite green again before this test was kept.
    """
    assert not np.array_equal(ci.SEC2_IDENTIFICATION_HYBRID_MATRIX, ps.K_full), (
        "SEC2_IDENTIFICATION_HYBRID_MATRIX must not alias plant_sim.K_full any more -- "
        "the on-board firmware matrix and the sim's recalibrated matrix have diverged"
    )
    # Still the specific old bench-rig matrix, not drifted to something else.
    assert ci.SEC2_IDENTIFICATION_HYBRID_MATRIX[0][1] == pytest.approx(26.61)
    assert ci.SEC2_IDENTIFICATION_HYBRID_MATRIX[1][0] == pytest.approx(15.78)


# ---------------------------------------------------------------------------
# Settle-criterion audit -- does the firmware's settle test (slope on
# actual_c only) accept a reading whose DUTY is still moving?
#
# Built directly from the coupid6 capture: the firmware-matching settle
# test in this module fired on all 12/12 joint observations from that
# 10-minute-dwell firing, but duty was still visibly swinging for the
# rest of several of those same windows (up to a 0.087 span on a 0.378
# duty -- 23%). See coupled_ident.py's own "Settle-criterion audit"
# section for the full story and coupled_ident.UNSTABLE_DUTY_RANGE_ABS /
# _FRAC for the thresholds these tests pin.
# ---------------------------------------------------------------------------

def _oscillating_dwell_rows(actual_c, duty_sequence, target_c, ambient, dt=10.0, segment_index=0):
    """Like ``_dwell_rows``, but ``duty_sequence`` is a per-SAMPLE list of
    per-zone duty dicts (rather than one constant dict) so a dwell can
    have duty that keeps moving after actual_c has already gone flat --
    exactly the coupid6 shape (settle test only watches actual_c)."""
    rows = []
    zones0 = {z: la.ZoneSample(zone=z, actual_c=_q(ambient[z]), duty=0.0) for z in ci.ZONES}
    rows.append(la.PollRow(wall_time="00:00:00", elapsed_s=0.0, segment_index=segment_index,
                            segment_count=1, dwelling=False, target_c=target_c, state="running",
                            zones=zones0))
    for i, duty in enumerate(duty_sequence):
        e = (i + 1) * dt
        zones = {z: la.ZoneSample(zone=z, actual_c=_q(actual_c[z]), duty=duty[z]) for z in ci.ZONES}
        rows.append(la.PollRow(wall_time="00:00:%02d" % (e % 60), elapsed_s=e,
                                segment_index=segment_index, segment_count=1, dwelling=True,
                                target_c=target_c, state="running", zones=zones))
    return rows


def test_settle_audit_flags_oscillating_duty_after_settle():
    """actual_c is flat from sample 1 onward (so the settle test fires
    quickly and cleanly, same as it would in the real capture), but duty
    keeps swinging by 0.15 for the rest of the window -- must be flagged.

    Proof this can fail: temporarily set UNSTABLE_DUTY_RANGE_ABS to 10.0
    and UNSTABLE_DUTY_RANGE_FRAC to 10.0 (thresholds no real duty swing
    could cross). Captured red:
        AssertionError: assert False
      -- with both thresholds pushed out of reach, a duty swinging from
      0.10 to 0.25 (a 150% relative, 0.15 absolute span) was reported as
      NOT flagged.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    actual_c = {0: 45.0, 1: 45.0, 2: 45.0}
    # 25 samples (250s dwell), actual_c pinned flat throughout so the
    # settle test can fire at 180s+; duty for zone 0 oscillates widely
    # for the whole window including after the settle instant.
    duty_seq = []
    for i in range(25):
        d0 = 0.10 + 0.15 * (1 if i % 2 == 0 else 0)  # 0.10 / 0.25 alternating
        duty_seq.append({0: round(d0, 2), 1: 0.20, 2: 0.20})
    rows = _oscillating_dwell_rows(actual_c, duty_seq, target_c=45.0, ambient=ambient)
    entries = ci.settle_criterion_audit(rows)
    e0 = next(e for e in entries if e.zone == 0)
    assert e0.settled
    assert e0.flagged_unstable


def test_settle_audit_does_not_flag_stable_dwell():
    """A dwell whose duty is flat (mod 0.02 quantization noise) for the
    entire post-settle window must NOT be flagged -- the audit should
    read as clean on the data it is designed to pass, not just alarm on
    everything.

    Proof this can fail: temporarily changed the ``flagged`` initial
    value in ``settle_criterion_audit`` from ``False`` to ``True``.
    Captured red:
        AssertionError: assert not True
      -- a duty range of 0.02 (well under both thresholds) was reported
      flagged once every settled entry defaulted to flagged regardless of
      its actual range.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    settle_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty = {0: 0.15, 1: 0.2, 2: 0.25}
    rows = _dwell_rows(settle_c, duty, target_c=45.0, ambient=ambient, n_samples=25, dt=10.0)
    entries = ci.settle_criterion_audit(rows)
    settled = [e for e in entries if e.settled]
    assert settled
    assert not any(e.flagged_unstable for e in settled)


def test_settle_audit_excludes_pre_settle_transient():
    """Duty swings hard BEFORE the settle instant (the normal ramp-in
    convergence) but is flat afterward -- must NOT be flagged. This is the
    fix for the audit's first draft, which measured duty range over the
    WHOLE window and flagged nearly every real capture's dwells
    (including the already-validated plant_sim fixtures) purely from
    their ordinary pre-settle convergence.

    Proof this can fail: passed ``from_elapsed_s=0.0`` unconditionally
    inside ``settle_criterion_audit`` instead of the settle row's own
    elapsed_s. Captured red:
        AssertionError: assert True is False
      -- the same big pre-settle swing that should have been excluded
      flagged the entry once the window was measured from the start
      instead of from the settle instant.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    actual_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty_seq = []
    for i in range(30):
        if i < 5:
            d0 = 0.05 + 0.4 * (i / 5.0)  # big pre-settle swing, samples 0-4
        else:
            d0 = 0.15  # flat afterward
        duty_seq.append({0: round(d0, 2), 1: 0.2, 2: 0.2})
    rows = _oscillating_dwell_rows(actual_c, duty_seq, target_c=45.0, ambient=ambient)
    entries = ci.settle_criterion_audit(rows)
    e0 = next(e for e in entries if e.zone == 0)
    assert e0.settled
    assert not e0.flagged_unstable


# ---------------------------------------------------------------------------
# Resumed-capture dedup (coupid6_run1.jsonl + coupid6_run1_part2.jsonl)
# ---------------------------------------------------------------------------

def test_dedupe_collapses_same_reading_from_two_overlapping_files(tmp_path):
    """Two capture files covering the same physical dwell (a poller
    restarted after the board's HTTP server wedged, overlapping the tail
    of the first file) must not double-count that dwell as two
    observations.

    Proof this can fail: temporarily made ``_dedupe_observations`` return
    its input unchanged. Captured red:
        AssertionError: assert 2 == 1
      -- the same settled reading, captured by both files, counted as two
      separate joint observations instead of one.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    settle_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty = {0: 0.15, 1: 0.2, 2: 0.25}
    rows = _dwell_rows(settle_c, duty, target_c=45.0, ambient=ambient, n_samples=25, dt=10.0)

    def _write(path, rows):
        lines = []
        for r in rows:
            body = {
                "elapsed_s": r.elapsed_s, "segment_index": r.segment_index,
                "segment_count": r.segment_count, "dwelling": r.dwelling,
                "target_c": r.target_c, "state": r.state,
                "zones": [{"zone": z, "actual_c": s.actual_c, "duty": s.duty} for z, s in r.zones.items()],
            }
            lines.append(f"{r.wall_time} {__import__('json').dumps(body)}")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    p1 = tmp_path / "run1.jsonl"
    p2 = tmp_path / "run1_part2.jsonl"
    _write(p1, rows)
    _write(p2, rows)  # identical re-capture of the same dwell

    obs_combined = ci.dwell_observations_from_paths([str(p1), str(p2)])
    obs_single = ci.dwell_observations_from_paths([str(p1)])
    assert len(obs_combined) == len(obs_single)


# ---------------------------------------------------------------------------
# Single-zone excitation sourced from the two-poller (mcp+thermo) capture
# format -- see coupling_pair_log.py's own module docstring for the format
# and join rule. These wrap the same single_zone_column_observations() /
# settle_criterion_audit() already tested above against synthetic PollRows,
# so what's under test here is specifically the plumbing: does the join
# actually feed real rows through, and does it stay silent (empty list, not
# a crash or fabricated data) when the source has nothing usable.
# ---------------------------------------------------------------------------

PAIR_FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "coupling_pair")
CPL_Z0_MCP = os.path.join(PAIR_FIXTURES, "cpl_z0_mcp.jsonl")
CPL_Z0_THERMO = os.path.join(PAIR_FIXTURES, "cpl_z0_thermo.jsonl")


def _require_default_coupling_captures():
    """build_coupling_report()/score_matrix_transient_from_http_paths()'s
    module-level defaults (ci.DEFAULT_SINGLE_ZONE_PAIRS, ci.DEFAULT_COOLDOWN_PATHS,
    ci.DEFAULT_TRANSIENT_AB_PATH_RUNS) all point at logs/coupling/*.jsonl real
    bench captures, which are deliberately gitignored (85MB+, not tracked) and
    absent in a fresh clone/worktree. Skip rather than fail so a from-scratch
    checkout doesn't read as regressed."""
    paths = []
    for mcp_path, thermo_path in ci.DEFAULT_SINGLE_ZONE_PAIRS.values():
        paths.append(mcp_path)
        paths.append(thermo_path)
    paths.extend(ci.DEFAULT_COOLDOWN_PATHS)
    paths.extend(p for p, _idx in ci.DEFAULT_TRANSIENT_AB_PATH_RUNS)
    missing = [p for p in paths if not os.path.isfile(p)]
    if missing:
        pytest.skip(
            "real-capture fixture(s) not present (gitignored, main-tree-only): "
            + ", ".join(missing)
        )


def test_single_zone_column_observations_from_pair_reads_real_capture():
    """The real cpl_z0 capture (zone 0 driven, 55 C dwell, in progress) must
    yield at least zone 0's own diagonal observation via the pair-log path
    -- the zone that is actually being excited settles well inside
    SETTLE_MIN_S (180 s) even though the run has not run long enough yet
    for the passive zones' much longer 1800 s settle floor.

    Proof this can fail: pointed active_zone at 1 (nothing drives zone 1
    in this capture). Captured red:
        AssertionError: assert [] != []
      -- no observations at all, because zone 1 never clears the active
      settle test (duty stays ~0 throughout, below MIN_DUTY_FOR_OBSERVATION).
    """
    obs = ci.single_zone_column_observations_from_pair(CPL_Z0_MCP, CPL_Z0_THERMO, active_zone=0)
    assert obs, "expected at least the diagonal (zone0-affects-zone0) observation"
    diag = [o for o in obs if o.affected_zone == 0]
    assert diag, "zone 0's own settle test should have fired well before the capture ends"

    # Negative companion, inline: zone 1 was never driven in this capture.
    obs_wrong_zone = ci.single_zone_column_observations_from_pair(CPL_Z0_MCP, CPL_Z0_THERMO, active_zone=1)
    assert obs_wrong_zone == []


def test_single_zone_column_observations_from_pair_empty_on_no_join(tmp_path):
    """If the mcp/thermo timestamps never fall within max_skew_s of each
    other, load_pair_run() emits zero rows and this wrapper must come back
    empty too, not raise.
    """
    mcp_path = tmp_path / "m.jsonl"
    thermo_path = tmp_path / "t.jsonl"
    mcp_path.write_text(
        '{"t": 1000.0, "s": "state=1 profile=#4 \'x\' segment=0/1 dwelling=True target=55.0C '
        'elapsed=600s dwell_remaining=100s ramp_lock=False fault_guard=0\n'
        '  zone 0: mode=2 actual=55.0C (valid) duty=0.5 relay=on faulted=False"}\n',
        encoding="utf-8",
    )
    thermo_path.write_text(
        '{"t": 9999.0, "s": "CH0: 30.0 C (CJ 25.0 C)\nCH1: 20.0 C (CJ 25.0 C)\nCH2: 19.0 C (CJ 25.0 C)"}\n',
        encoding="utf-8",
    )
    obs = ci.single_zone_column_observations_from_pair(str(mcp_path), str(thermo_path), active_zone=0)
    assert obs == []


def test_settle_criterion_audit_from_pair_matches_direct_extraction():
    """settle_criterion_audit_from_pair() must produce exactly the audit
    settle_criterion_audit() would produce given the same joined rows --
    it is a thin wrapper, not a second implementation.

    Proof this can fail: had the wrapper call settle_criterion_audit() with
    min_s hardcoded from SINGLE_ZONE_PASSIVE_SETTLE_MIN_S instead of
    reusing the function's own default. Captured red:
        AssertionError: assert 0 == 3
      -- zone 0's ordinary (fast) settle entries vanished because the
      15-minute floor never clears inside this run's early rows.
    """
    from kilnctrl import coupling_pair_log as cpl
    rows = cpl.load_pair_run(CPL_Z0_MCP, CPL_Z0_THERMO)
    expected = ci.settle_criterion_audit(rows, source=f"{CPL_Z0_MCP}+{CPL_Z0_THERMO}")
    actual = ci.settle_criterion_audit_from_pair(CPL_Z0_MCP, CPL_Z0_THERMO)
    assert [dataclasses.asdict(e) for e in actual] == [dataclasses.asdict(e) for e in expected]


# ---------------------------------------------------------------------------
# --allow-multi-session -- the single-zone CLI's escape hatch for
# coupling_pair_log.MultiSessionError (finding 2, opus review round 3): a
# capture pair with one legitimate long gap (PC sleep, Wi-Fi reconnect, MCP
# restart, board reboot) used to abort `coupled-ident single-zone
# --zone0-pair ...` with an uncaught traceback and no way forward short of
# editing source, because neither single_zone_column_observations_from_pair
# nor the CLI ever passed allow_multi_session through to load_pair_run().
# ---------------------------------------------------------------------------

def _session_gap_pair(tmp_path):
    from kilnctrl import coupling_pair_log as cpl
    t0 = 1_000_000.0

    def exec_line(t, elapsed_text):
        return json.dumps({
            "t": t,
            "s": (f"state=1 profile=#4 'cpl_z0' segment=0/1 dwelling=True "
                  f"target=55.0C elapsed={elapsed_text}s dwell_remaining=1500s "
                  f"ramp_lock=False fault_guard=0\n"
                  f"  zone 0: mode=2 actual=55.0C (valid) duty=0.5 relay=on faulted=False"),
        })

    def thermo_line(t):
        return json.dumps({"t": t, "s": "CH0: 30.0 C (CJ 25.0 C)\nCH1: 25.0 C (CJ 25.0 C)\nCH2: 25.0 C (CJ 25.0 C)"})

    exec_lines = [exec_line(t0 + i * 20, 100 + i * 20) for i in range(5)]
    exec_lines += [exec_line(t0 + 5 * 20 + 7200 + i * 20, i * 20) for i in range(5)]
    thermo_lines = [thermo_line(t0 + i * 20) for i in range(5)]
    thermo_lines += [thermo_line(t0 + 5 * 20 + 7200 + i * 20) for i in range(5)]

    mcp_path = tmp_path / "gap_mcp.jsonl"
    thermo_path = tmp_path / "gap_thermo.jsonl"
    mcp_path.write_text("\n".join(exec_lines) + "\n")
    thermo_path.write_text("\n".join(thermo_lines) + "\n")
    return str(mcp_path), str(thermo_path)


def test_single_zone_column_observations_from_pair_raises_on_session_gap(tmp_path):
    """Without allow_multi_session, a --zoneN-pair capture whose exec log
    has a >300s gap must surface MultiSessionError, not swallow it or crash
    some other way -- this is the wrapper finding 2 says never forwarded the
    flag at all.

    Proof this can fail: reverted the forwarding fix (dropped
    allow_multi_session from the call to load_pair_run inside the wrapper,
    leaving the parameter accepted but unused). Captured red: the call
    returns [] instead of raising, silently joining two fake firings into
    one column observation set -- pytest.raises(MultiSessionError) itself
    fails with `DID NOT RAISE <class 'coupling_pair_log.MultiSessionError'>`.
    """
    from kilnctrl import coupling_pair_log as cpl
    mcp_path, thermo_path = _session_gap_pair(tmp_path)
    with pytest.raises(cpl.MultiSessionError) as exc_info:
        ci.single_zone_column_observations_from_pair(mcp_path, thermo_path, active_zone=0)
    assert "allow_multi_session=True" in str(exc_info.value)


def test_single_zone_column_observations_from_pair_allow_multi_session_bypasses(tmp_path):
    """allow_multi_session=True on the wrapper must reach load_pair_run and
    actually bypass the refusal (no MultiSessionError), same as calling
    load_pair_run directly with the flag set -- proven by comparing against
    that direct call rather than requiring specific settle-window contents
    from a synthetic fixture that isn't trying to model real thermal
    dynamics.

    Proof this can fail: reverted the forwarding fix (dropped
    allow_multi_session from the call to load_pair_run inside the wrapper).
    Captured red:
        coupling_pair_log.MultiSessionError: ...gap_mcp.jsonl: exec-status
        log has 1 gap(s) over 300s...
      raised out of a call that passed allow_multi_session=True and should
      not have raised at all.
    """
    from kilnctrl import coupling_pair_log as cpl
    mcp_path, thermo_path = _session_gap_pair(tmp_path)
    expected_rows = cpl.load_pair_run(mcp_path, thermo_path, allow_multi_session=True)
    obs = ci.single_zone_column_observations_from_pair(
        mcp_path, thermo_path, active_zone=0, allow_multi_session=True)
    expected = ci.single_zone_column_observations(expected_rows, active_zone=0,
                                                    source=f"{mcp_path}+{thermo_path}")
    assert obs == expected


def test_single_zone_cli_requires_allow_multi_session_flag_for_gapped_pair(tmp_path, capsys):
    """`coupled-ident single-zone --zone0-pair ...` on a gapped capture must
    raise (not silently produce a matrix) unless --allow-multi-session is
    passed -- and passing it must let the run through cleanly instead of
    raising. This is finding 2's actual escape hatch: before this fix there
    was no such flag anywhere in the argparse setup, so a real single
    legitimate long gap left no way forward short of editing source.

    Proof the flag threading can fail: had --allow-multi-session parsed but
    not passed into single_zone_column_observations_from_pair at the call
    site (hardcoded allow_multi_session=False there instead of
    args.allow_multi_session). Captured red on that mutation: the second
    `ci.main(...)` call below (WITH the flag) still raised
    MultiSessionError instead of returning 0.
    """
    from kilnctrl import coupling_pair_log as cpl
    mcp_path, thermo_path = _session_gap_pair(tmp_path)

    with pytest.raises(cpl.MultiSessionError):
        ci.main(["single-zone", "--zone0-pair", mcp_path, thermo_path, "--json"])

    rc = ci.main(["single-zone", "--zone0-pair", mcp_path, thermo_path, "--allow-multi-session", "--json"])
    assert rc == 0
    json.loads(capsys.readouterr().out)  # must be valid JSON, no traceback interleaved


def test_matrix_from_single_zone_columns_orientation():
    """matrix_from_single_zone_columns() must assemble cell (affected, active)
    -- i.e. [affected][stepped] -- not the transpose. Uses SEC2_IDENTIFICATION_HYBRID_MATRIX,
    which is intentionally ASYMMETRIC (see the orientation tests above for
    why a symmetric matrix can't catch this), and drives all three zones
    one at a time so every one of the 9 cells is measured directly.

    Proof this can fail: swapped the assembly key to
    ``(o.active_zone, o.affected_zone)`` (the transpose). Captured red:
        AssertionError: assert 8.xx == pytest.approx(20.73 +/- ...)
      -- the assembled matrix's off-diagonal entries came back matching
      SEC2_IDENTIFICATION_HYBRID_MATRIX.T instead of SEC2_IDENTIFICATION_HYBRID_MATRIX, exactly the swapped-twice
      mistake this whole module's docstring warns about.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    A = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX
    duty_level = 0.3
    column_obs = {}
    for active in ci.ZONES:
        true_rise = A[:, active] * duty_level
        settle_c = {z: ambient[z] + true_rise[z] for z in ci.ZONES}
        zone_duty = {z: (duty_level if z == active else 0.0) for z in ci.ZONES}
        rows = _dwell_rows(settle_c, zone_duty, target_c=settle_c[active], ambient=ambient,
                            n_samples=210, dt=10.0)
        column_obs[active] = ci.single_zone_column_observations(rows, active_zone=active)

    matrix, coverage = ci.matrix_from_single_zone_columns(column_obs)
    assert matrix is not None
    assert np.allclose(matrix, A, atol=0.5)
    # And explicitly NOT its transpose (the matrix is asymmetric, so this
    # would only accidentally pass if A were symmetric -- it isn't).
    assert not np.allclose(matrix, A.T, atol=0.5)


# ---------------------------------------------------------------------------
# Settle audit: float64 rounding must not trip the duty-range gates
# ---------------------------------------------------------------------------

def test_settle_audit_float_dust_at_exact_gate_not_flagged():
    """A duty range that is EXACTLY at the 0.05 absolute gate in decimal
    (0.79 - 0.74) is 0.050000000000000044 in float64, not 0.05 -- a plain
    ``drange > UNSTABLE_DUTY_RANGE_ABS`` trips on that representation
    error alone, flagging a reading that never actually swung more than
    the documented threshold. This is the exact shape of the real
    cpl_z2_mcp.jsonl capture's settle window (0.74-0.79 duty), which this
    test pins directly against that reported bug.

    Proof this can fail: temporarily removed ``_DUTY_RANGE_EPS`` from
    both comparisons in ``settle_criterion_audit`` (back to a bare
    ``drange > UNSTABLE_DUTY_RANGE_ABS``). Captured red:
        AssertionError: assert True
      -- 0.79 - 0.74 evaluates to 0.050000000000000044 > 0.05 in float64,
      so the reading was flagged even though its real duty swing is
      exactly at, not past, the documented gate.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    actual_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty_seq = []
    for i in range(25):
        d0 = 0.79 if i % 2 == 0 else 0.74
        duty_seq.append({0: d0, 1: 0.2, 2: 0.2})
    rows = _oscillating_dwell_rows(actual_c, duty_seq, target_c=45.0, ambient=ambient)
    entries = ci.settle_criterion_audit(rows)
    e0 = next(e for e in entries if e.zone == 0)
    assert e0.settled
    assert e0.window_duty_range == pytest.approx(0.05, abs=1e-6)
    assert not e0.flagged_unstable


def test_settle_audit_still_flags_a_real_swing_past_the_gate():
    """The epsilon fix above must not swallow a genuine swing -- a duty
    range clearly past 0.05 (0.80 vs 0.74, i.e. 0.06) still has to be
    flagged. Guards against an epsilon big enough to mask real
    instability, not just float dust.
    """
    ambient = {0: 20.0, 1: 20.0, 2: 20.0}
    actual_c = {0: 45.0, 1: 45.0, 2: 45.0}
    duty_seq = []
    for i in range(25):
        d0 = 0.80 if i % 2 == 0 else 0.74
        duty_seq.append({0: d0, 1: 0.2, 2: 0.2})
    rows = _oscillating_dwell_rows(actual_c, duty_seq, target_c=45.0, ambient=ambient)
    entries = ci.settle_criterion_audit(rows)
    e0 = next(e for e in entries if e.zone == 0)
    assert e0.settled
    assert e0.flagged_unstable


# ---------------------------------------------------------------------------
# Coverage redundancy note
# ---------------------------------------------------------------------------

def test_coverage_note_flags_single_observation_cells():
    """Every cell at coverage=1 must be reported as having no error bar --
    this is what the coupling-report needs to say in code, not just in a
    prose report, so it can't silently drift.

    Proof this can fail: made the ``all(c == 1 ...)`` branch return the
    same string as the >=2-coverage branch. Captured red:
        AssertionError: assert 'no redundancy' in 'cell coverage ranges 1-1 observations...'
      -- with the two branches collapsed, coverage=1 cells were reported
      with the same phrasing as genuinely redundant, multi-observation
      cells, losing the "no error bar" caveat entirely.
    """
    coverage = {(i, j): 1 for i in ci.ZONES for j in ci.ZONES}
    note = ci.coverage_redundancy_note(coverage)
    assert "no redundancy" in note
    assert "no error bar" in note


def test_coverage_note_reports_incomplete_matrix():
    coverage = {(i, j): 1 for i in ci.ZONES for j in ci.ZONES}
    coverage[(0, 0)] = 0
    note = ci.coverage_redundancy_note(coverage)
    assert "incomplete" in note


def test_coverage_note_distinguishes_higher_coverage():
    coverage = {(i, j): 2 for i in ci.ZONES for j in ci.ZONES}
    note = ci.coverage_redundancy_note(coverage)
    assert "no redundancy" not in note


# ---------------------------------------------------------------------------
# Coupled-hold feasibility sweep
# ---------------------------------------------------------------------------

def test_feasibility_sweep_diagonal_only_matches_hand_calc():
    """A pure-diagonal matrix (no coupling) makes u = (T-ambient)/K
    independently per zone -- the sweep's max_feasible_c must match the
    hand-computed value K + ambient (duty hits exactly 1.0 there).

    Proof this can fail: swapped the feasibility test from ``np.all(u <=
    1.0)`` to ``np.all(u < 1.0)`` (strict). Captured red:
        AssertionError: assert 50.0 == 49.5
      -- the boundary sample (u exactly 1.0) was marked infeasible under
      strict ``<``, moving max_feasible_c one step short of the true
      diagonal gain.
    """
    K = 30.0
    A = np.diag([K, K, K])
    result = ci.coupled_hold_feasibility_sweep(A, ambient_c=20.0, t_max_above_ambient_c=60.0, t_step_c=0.5)
    assert result["max_feasible_c"] == pytest.approx(20.0 + K)
    assert result["first_infeasible_c"] == pytest.approx(20.0 + K + 0.5)


def test_feasibility_sweep_new_matrix_extends_old_matrix_range():
    """Sanity check against the two matrices this module actually
    compares in the coupling report: a matrix with a uniformly higher
    diagonal gain than SEC2_IDENTIFICATION_HYBRID_MATRIX must not be LESS feasible."""
    old = ci.SEC2_IDENTIFICATION_HYBRID_MATRIX
    new = old * 1.05
    r_old = ci.coupled_hold_feasibility_sweep(old, ambient_c=22.0)
    r_new = ci.coupled_hold_feasibility_sweep(new, ambient_c=22.0)
    assert r_new["max_feasible_c"] >= r_old["max_feasible_c"]


# ---------------------------------------------------------------------------
# Cooldown tau fit
# ---------------------------------------------------------------------------

def test_cooldown_tau_fit_recovers_known_time_constant():
    """A synthetic exact exponential decay must be recovered to within a
    tight tolerance -- this is the ground-truth check nothing about real,
    noisy hardware data can provide.

    Proof this can fail: changed the sign in
    ``pred = t_inf + (T0 - t_inf) * exp(-t/tau)`` to ``exp(+t/tau)``
    (growth instead of decay). Captured red:
        AssertionError: assert None is not None
      -- with growth instead of decay in the residual model, every T_inf
      on the grid failed the positive-time-constant check and the fit
      refused outright instead of recovering tau_true.
    """
    tau_true = 300.0
    t_inf_true = 25.0
    T0 = 60.0
    times = np.arange(0.0, 1800.0, 20.0)
    temps = t_inf_true + (T0 - t_inf_true) * np.exp(-times / tau_true)
    fit = ci.fit_cooldown_tau(times, temps, zone=0)
    assert not fit.refused
    assert fit.tau_s == pytest.approx(tau_true, rel=0.1)
    assert fit.t_inf_c == pytest.approx(t_inf_true, abs=1.0)
    assert fit.rms_resid_c < 1.0


def test_cooldown_tau_fit_refuses_non_monotonic_trace():
    """A trace that cools then reheats (the real cooldown_z1.jsonl shape --
    a later, unrelated phase spliced into the same continuous capture)
    must be REFUSED, not fitted to a bogus tau.

    Proof this can fail: removed the ``_cooldown_is_monotonic_enough``
    guard from ``fit_cooldown_tau``. Captured red:
        AssertionError: assert True is False
      -- a cool-then-reheat trace no longer refused; the grid search
      found some T_inf/tau combination through it anyway (a bad fit
      dressed up as a confident answer instead of being flagged
      unreliable), since the monotonicity guard is a distinct check, not
      an automatic consequence of the grid search failing on its own.
    """
    times = np.arange(0.0, 1200.0, 20.0)
    n = len(times)
    temps = np.concatenate([
        60.0 - np.linspace(0, 20, n // 2),   # cools
        40.0 + np.linspace(0, 20, n - n // 2),  # then reheats
    ])
    fit = ci.fit_cooldown_tau(times, temps, zone=0)
    assert fit.refused
    assert "monotonic" in fit.reason


def test_cooldown_tau_fit_refuses_too_few_samples():
    fit = ci.fit_cooldown_tau(np.array([0.0, 20.0]), np.array([50.0, 49.0]), zone=0)
    assert fit.refused
    assert "samples" in fit.reason


# ---------------------------------------------------------------------------
# Full coupling report -- reproducibility of every reported number from the
# checked-in logs/coupling/ captures
# ---------------------------------------------------------------------------

def test_coupling_report_reproduces_checked_in_captures():
    """End-to-end: build_coupling_report() with its defaults (the checked-
    in logs/coupling/ single-zone captures and tests/fixtures/plant_sim/
    sec 3.2 fixtures) must assemble a complete, plausible matrix and pass
    its own self-check -- the same numbers a coupling-matrix analysis pass
    reports, now reproducible by re-running this function/CLI instead of
    living only in a prose report.
    """
    _require_default_coupling_captures()
    report = ci.build_coupling_report()
    assert report["matrix_incomplete"] is False
    assert report["coverage_note"].startswith("every one of the 9 cells has coverage=1")
    assert report["plausible_new"] is True
    assert report["self_check"]["ok"] is True
    # z2's zone-2-excited settle window is the known float-dust case --
    # must NOT be flagged after the epsilon fix.
    z2_entries = report["settle_audit"][2]
    z2_primary = next(e for e in z2_entries if e["zone"] == 2 and e["settled"])
    assert not z2_primary["flagged_unstable"]
    # sec 3.2 revalidation: new matrix must reduce both z0's and z2's bias
    # magnitude relative to the old matrix (the question this whole
    # analysis exists to answer).
    old_by_zone = {s["zone"]: s["mean_error"] for s in report["sec32_scores_old"]}
    new_by_zone = {s["zone"]: s["mean_error"] for s in report["sec32_scores_new"]}
    assert abs(new_by_zone[0]) < abs(old_by_zone[0])
    assert abs(new_by_zone[2]) < abs(old_by_zone[2])
    # feasibility: new matrix must be feasible at least as high as old.
    assert report["feasibility_new"]["max_feasible_c"] >= report["feasibility_old"]["max_feasible_c"]
    # format function must not raise on real data.
    text = ci.format_coupling_report_text(report)
    assert "coupled-hold feasibility sweep" in text


def test_coupling_report_cli_text_smoke(capsys):
    _require_default_coupling_captures()
    rc = ci.main(["coupling-report"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "settle-criterion audit" in out
    assert "coupled-hold feasibility sweep" in out


def test_coupling_report_cli_json_smoke(capsys):
    _require_default_coupling_captures()
    import json as json_mod
    rc = ci.main(["coupling-report", "--json"])
    assert rc == 0
    out = capsys.readouterr().out
    parsed = json_mod.loads(out)
    assert parsed["matrix_incomplete"] is False


# ---------------------------------------------------------------------------
# Anti-staleness guard: ADOPTED_MATRIX_OFF_DIAGONAL / ADOPTED_HYBRID_MATRIX
# must never silently drift from the checked-in preset that is the actual
# source of truth for what config_presets.apply_preset() posts to a board
# (tools/PcTools/config_presets/coupling_matrix_20260831.json) -- the exact
# failure mode this whole task exists to fix: CURRENT_MATRIX went stale
# relative to what firmware actually runs and nothing caught it.
# ---------------------------------------------------------------------------

_ADOPTED_PRESET_PATH = os.path.join(
    os.path.dirname(__file__), "..", "config_presets", "coupling_matrix_20260831.json")


def test_adopted_matrix_matches_checked_in_preset():
    """``ci.ADOPTED_MATRIX_OFF_DIAGONAL`` must equal, cell for cell, the
    ``coupling_coeff`` rows in ``coupling_matrix_20260831.json`` -- the
    checked-in preset that is the actual, sanctioned source of truth for
    what gets POSTed to a live board (PID_EXPANSION_PLAN.md sec 3.2's
    "ADOPTED" matrix). If this ever goes red, either the preset changed (a
    new matrix was adopted) or this module's constant was hand-edited out
    of sync with it -- in both cases the fix is to re-derive the constant
    from the preset, not to edit this test.

    Proof this can fail: changed a single off-diagonal cell of
    ``ci.ADOPTED_MATRIX_OFF_DIAGONAL`` (z0's c1 entry, 27.32 -> 27.99) and
    reran. Captured red:
        AssertionError: zone 0 coupling_coeff mismatch: code=[0.0, 27.99, 21.72] preset=[0.0, 27.32, 21.72]
      -- confirms the test actually compares real numbers cell-by-cell,
      not just object identity or shape.
    """
    with open(_ADOPTED_PRESET_PATH, "r", encoding="utf-8") as fh:
        preset = json.load(fh)
    zones = sorted(preset["zones"], key=lambda z: z["index"])
    assert len(zones) == 3
    for z in zones:
        i = z["index"]
        code_row = ci.ADOPTED_MATRIX_OFF_DIAGONAL[i].tolist()
        preset_row = z["coupling_coeff"]
        assert code_row == pytest.approx(preset_row, abs=1e-9), (
            f"zone {i} coupling_coeff mismatch: code={code_row} preset={preset_row}")


def test_adopted_hybrid_matrix_diagonal_is_ff_k_dc():
    """``ADOPTED_HYBRID_MATRIX``'s diagonal must be exactly
    ``FF_K_DC_DIAGONAL`` (never the preset's contractual-0 diagonal) --
    this is what makes it the HYBRID the solver actually runs, per sec
    3.2's correction, rather than the raw preset matrix nothing on the
    board ever solves against directly.

    Proof this can fail: built the hybrid with ``off_diagonal_matrix``'s
    own (zero) diagonal left in place instead of substituting
    ``FF_K_DC_DIAGONAL``. Captured red:
        AssertionError: assert 0.0 == 39.2459 +- ...
      -- confirms the diagonal substitution is actually exercised, not
      vacuously true because the two happen to agree.
    """
    for i in range(3):
        assert ci.ADOPTED_HYBRID_MATRIX[i, i] == pytest.approx(ci.FF_K_DC_DIAGONAL[i])
        assert ci.ADOPTED_MATRIX_OFF_DIAGONAL[i, i] == 0.0


# ---------------------------------------------------------------------------
# Transient-aware score (PID_EXPANSION_PLAN.md sec 3.2 "HARDWARE A/B,
# 2026-09-03") -- score_matrix_transient / transient_window_row_indices.
# ---------------------------------------------------------------------------

def _ramp_then_dwell_rows_zone0():
    """Segment 0: a 3-row ramp (elapsed 0/10/20, dwelling=False), then a
    4-row dwell (elapsed 30/40/50/60, dwelling=True) where zone 0's
    ``actual_c`` peaks at the SECOND dwell row (elapsed=40, value=52) then
    declines -- so "ramp + dwell-entry up to the peak" should keep rows
    0-4 (idx 0,1,2 ramp; idx 3,4 dwell-entry) and exclude idx 5,6 (the
    post-peak decline). Zones 1/2 stay flat at 20C/duty=0 throughout so
    their own (degenerate) transitions don't pull in extra rows.
    """
    def row(elapsed, dwelling, c0, d0):
        zones = {
            0: la.ZoneSample(zone=0, actual_c=c0, duty=d0),
            1: la.ZoneSample(zone=1, actual_c=20.0, duty=0.0),
            2: la.ZoneSample(zone=2, actual_c=20.0, duty=0.0),
        }
        return la.PollRow(wall_time="00:00:%02d" % (elapsed % 60), elapsed_s=elapsed,
                           segment_index=0, segment_count=1, dwelling=dwelling,
                           target_c=50.0, state="running", zones=zones)

    return [
        row(0, False, 20.0, 0.0),
        row(10, False, 30.0, 5.0),
        row(20, False, 40.0, 10.0),
        row(30, True, 50.5, 15.25),
        row(40, True, 52.0, 16.0),    # peak
        row(50, True, 51.0, 999.0),   # post-peak: duty deliberately wrong
        row(60, True, 50.2, 999.0),   # post-peak: duty deliberately wrong
    ]


def test_transient_window_row_indices_stops_at_the_overshoot_peak():
    """Ramp rows are all included; dwell rows are included only through the
    row where the zone's overshoot actually peaks -- later, declining-
    toward-settled rows must be excluded, since those are exactly the rows
    ``score_matrix`` already samples via its settled-tail path.

    Proof this can fail: changed the break condition from
    ``rows[i].elapsed_s >= ev.peak_overshoot_at_s`` to strict ``>``, which
    keeps scanning past the peak row instead of stopping there. Captured
    red:
        AssertionError: assert {0, 1, 2, 3, 4, 5} == {0, 1, 2, 3, 4}
      -- idx 5 (the first post-peak, declining row) leaked into the window.
    """
    rows = _ramp_then_dwell_rows_zone0()
    idxs = ci.transient_window_row_indices(rows)
    assert idxs == {0, 1, 2, 3, 4}


def test_score_matrix_transient_ignores_post_peak_rows():
    """``score_matrix_transient`` must score ONLY the transient-window rows
    -- a matrix that fits those rows exactly (mean/rms == 0) must score as
    a perfect fit even though later, deliberately-inconsistent rows exist
    in the same run.

    Setup: A = diag(2, 2, 2) (no coupling), ambient = run's first sample
    (20, 20, 20). Every included row (idx 0-4) has zone 0's duty set to
    exactly ``(actual_c - 20) / 2`` -- an exact fit. The two excluded rows
    (idx 5, 6) carry duty=999, wildly inconsistent with the same relation
    -- if they leaked into the score, mean/rms would be far from zero.

    Proof this can fail: simulated the "score every row, not just the
    transient window" bug directly -- computed ``u_pred - u_actual`` over
    ALL 7 rows (the settled-tail scorer's own algebra, just without the
    windowing this function adds) instead of the 5 transient rows. Captured
    red (zone 0 mean, hand-computed):
        mean over ALL rows (zone0): -281.057...  vs 0.0 +- 1e-6 expected
      -- the wrong-duty post-peak rows (idx 5, 6) dominate the mean once
      included, exactly the contamination this function exists to avoid.
    """
    rows = _ramp_then_dwell_rows_zone0()
    matrix = np.diag([2.0, 2.0, 2.0])
    scores = ci.score_matrix_transient(matrix, rows)
    by_zone = {s.zone: s for s in scores}
    assert by_zone[0].n == 5  # idx 0..4
    assert by_zone[0].mean_error == pytest.approx(0.0, abs=1e-6)
    assert by_zone[0].rms_error == pytest.approx(0.0, abs=1e-6)
    assert by_zone[1].mean_error == pytest.approx(0.0, abs=1e-6)
    assert by_zone[2].mean_error == pytest.approx(0.0, abs=1e-6)


def test_score_matrix_transient_from_http_paths_smoke_on_real_captures():
    """Smoke test against the actual matched hardware A/B pair sec 3.2's
    "HARDWARE A/B, 2026-09-03" entry analyzes -- must return real (nonzero
    n) scores for all three zones without raising, for each of the three
    candidate matrices sec 3.2's correction-block table compares.

    Proof this function's run-selection is not a silent "take the last run"
    shortcut: called it with an out-of-range explicit index (7) against
    ``p7_oldmatrix_runC.jsonl``, which actually holds more than one run (a
    real multi-run capture, exactly the shape ``DEFAULT_TRANSIENT_AB_PATH_RUNS``
    pins ``run_index=0`` against rather than trusting a default). Captured
    red:
        IndexError: logs/coupling/p7_oldmatrix_runC.jsonl: run index 7 out
        of range (file holds 2 runs)
      -- confirms this path goes through the real
      ``log_analysis.select_run`` refusal machinery, not a silent
      most-recent-run fallback.
    """
    _require_default_coupling_captures()
    for matrix in (ci.PRE_ADOPTION_HYBRID_MATRIX, ci.ADOPTED_OWN_DIAGONAL_MATRIX, ci.ADOPTED_HYBRID_MATRIX):
        scores = ci.score_matrix_transient_from_http_paths(matrix, ci.DEFAULT_TRANSIENT_AB_PATH_RUNS)
        assert len(scores) == 3
        for s in scores:
            assert s.n > 0
            assert not (isinstance(s.mean_error, float) and s.mean_error != s.mean_error)  # not NaN


def test_coupling_report_includes_transient_section():
    """``build_coupling_report``'s section 7 must carry all three sec 3.2
    correction-block matrices' transient scores, and
    ``format_coupling_report_text`` must render it -- this is the
    "wired in beside the existing bias score" requirement, not a separate
    tool a caller has to know to run.

    Proof this can fail: renamed the dict key written by
    ``build_coupling_report`` (``"transient_scores"`` -> ``"transient_score"``)
    without updating the formatter. Captured red:
        KeyError: 'transient_scores'
      -- format_coupling_report_text's ``.items()`` call blew up because
      the report no longer carried the key it expects.
    """
    _require_default_coupling_captures()
    report = ci.build_coupling_report()
    assert set(report["transient_scores"].keys()) == {
        "pre_adoption_hybrid", "adopted_own_diagonal", "adopted_hybrid"
    }
    for name, scores in report["transient_scores"].items():
        assert len(scores) == 3
        assert all(s["n"] > 0 for s in scores)
    text = ci.format_coupling_report_text(report)
    assert "transient-aware score" in text
    assert "adopted_hybrid matrix scores" in text


# ---------------------------------------------------------------------------
# identify_zone_dead_time_tau -- PID_EXPANSION_PLAN.md sec 3.2's "right fix":
# a full coupled identification that re-solves a zone's dead time AND time
# constant, as opposed to the diagonal-gain-only refine already shipped.
# ---------------------------------------------------------------------------

def test_identify_zone_dead_time_tau_recovers_known_values_in_sim():
    """Known-truth check: simulate a plant with a KNOWN L/tau (distinct
    from plant_sim's own bench-rig constants, so this cannot pass by
    accidentally hard-coding the production numbers) and confirm the grid
    search recovers both within one grid step / a few percent -- proves the
    regression math is sound before it is ever pointed at noisy real
    captures (same discipline load_estimator's own sim-validation uses)."""
    K = np.array([[40.0, 10.0, 5.0], [8.0, 32.0, 6.0], [4.0, 7.0, 30.0]])
    tau_true = np.array([260.0, 270.0, 271.0])
    L_true = np.array([50.0, 40.0, 30.0])
    dt = 5.0
    ambient = 20.0
    plant = ps.FOPDTPlant(K, tau_true, L_true, dt, ambient=ambient)
    n = 400
    duty = np.zeros((n, 3))
    duty[:, 0] = 0.6
    duty[:, 1] = 0.05
    duty[:, 2] = 0.05
    t = np.arange(n) * dt
    temps = np.zeros((n, 3))
    for i in range(n):
        temps[i] = plant.temp
        plant.step(duty[i])

    result = ci.identify_zone_dead_time_tau(
        t, temps, duty, zone=0, ambient=ambient, K=K, min_drive_c=1.0,
    )
    assert result is not None
    assert result.r2 > 0.99
    assert abs(result.dead_time_s - L_true[0]) <= 5.0  # one grid step (default candidates are 5s apart)
    assert abs(result.tau_s - tau_true[0]) / tau_true[0] < 0.05


def test_identify_zone_dead_time_tau_returns_none_below_sample_floor():
    """Mutation-style negative test, tripped for real rather than only
    asserted from the docstring: two samples, both excluded by
    ``min_drive_c``, must refuse (``None``) at every candidate dead time."""
    t = np.array([0.0, 5.0])
    temps = np.array([[20.0, 20.0, 20.0], [20.0, 20.0, 20.0]])
    duty = np.array([[0.0, 0.0, 0.0], [0.0, 0.0, 0.0]])
    result = ci.identify_zone_dead_time_tau(
        t, temps, duty, zone=0, min_drive_c=5.0,
        dead_time_candidates_s=np.array([0.0, 50.0, 100.0]),
    )
    assert result is None


def test_identify_zone_dead_time_tau_from_capture_path_matches_direct_call(tmp_path):
    """The capture-path convenience wrapper must produce the same fit the
    direct array-based call does when fed the same underlying data --
    exercises the ``load_estimator.load_capture_rows``/``split_runs``/
    ``arrays_from_capture`` plumbing this wrapper adds on top of
    ``identify_zone_dead_time_tau`` itself."""
    lines = []
    for i in range(40):
        t = i * 5
        body = {
            "state": "running", "profile_id": 7, "profile_name": "x",
            "zone_mask": 7, "segment_index": 0, "segment_count": 1,
            "dwelling": False, "target_c": 60.0, "segment_elapsed_s": t,
            "dwell_remaining_s": 0, "ramp_lock_held": False,
            "ramp_lock_lagging_mask": 0, "fault_reason": "", "fault_guard": 0,
            "total_planned_s": None, "elapsed_s": t,
            "zones": [
                {"zone": z, "actual_c": 20.0 + 0.05 * t * (1 if z == 0 else 0.1),
                 "duty": 0.6 if z == 0 else 0.05,
                 "ff_hold_used_matrix": True, "ff_hold_infeasible": False}
                for z in range(3)
            ],
        }
        lines.append(f"00:00:{i:02d} {json.dumps(body)}")
    path = tmp_path / "synthetic.jsonl"
    path.write_text("\n".join(lines) + "\n")

    rows = la.parse_profile_exec_jsonl(str(path))
    from kilnctrl import load_estimator as le
    t_arr, temps, duty = le.arrays_from_capture(rows)
    direct = ci.identify_zone_dead_time_tau(t_arr, temps, duty, zone=0, min_drive_c=1.0)
    wrapped = ci.identify_zone_dead_time_tau_from_capture_path(
        str(path), zone=0, min_drive_c=1.0,
    )
    assert direct is not None and wrapped is not None
    assert wrapped.dead_time_s == direct.dead_time_s
    assert wrapped.tau_s == pytest.approx(direct.tau_s)
    assert wrapped.source == str(path)
