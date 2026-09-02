"""Tests for kilnctrl.plant_sim_sweep -- the parallel temperature-band
sweep over the calibrated plant simulator, added 2026-09-02 so a firing up
to cone 10 (~1285 C) can be explored in simulation only, never on the
physical test-fixture kiln (see plant_sim_sweep.py's module docstring)."""
from __future__ import annotations

from kilnctrl import plant_sim as ps
from kilnctrl import plant_sim_sweep as sweep


def test_run_sweep_is_deterministic_regardless_of_workers():
    """The same bands/matrices must produce the same table whether run
    single-process (workers=1) or pool-parallel (workers=3), and that table
    must be ordered by target_c regardless of the caller's dict insertion
    order -- the sweep's whole value is a reproducible table, not one that
    depends on worker scheduling or on the order bands happened to be typed
    in. ``bands`` is given here with its higher target_c FIRST specifically
    so an insertion-order bug would be caught.

    Proof this can fail: replaced ``sorted(bands.items(), key=lambda kv:
    kv[1])`` with plain ``list(bands.items())`` (drop the sort). Captured
    red output:

        FAILED tools/PcTools/tests/test_plant_sim_sweep.py::test_run_sweep_is_deterministic_regardless_of_workers
        AssertionError: assert [('b', 'old'), ('b', 'new'), ('a', 'old'), ('a', 'new')] == [('a', 'old'), ('a', 'new'), ('b', 'old'), ('b', 'new')]

    Reverted, suite green again before this test was kept.
    """
    bands = {"b": 55.0, "a": 30.0}  # deliberately out of target_c order
    matrices = ["old", "new"]
    seq = sweep.run_sweep(bands, matrices, workers=1)
    par = sweep.run_sweep(bands, matrices, workers=3)
    seq_keys = [(r["band"], r["matrix_variant"]) for r in seq]
    par_keys = [(r["band"], r["matrix_variant"]) for r in par]
    assert seq_keys == par_keys, "sequential and parallel runs produced different band/matrix order"
    assert seq_keys == [("a", "old"), ("a", "new"), ("b", "old"), ("b", "new")]


def test_low_band_matches_measured_envelope_not_extrapolation():
    """A band inside the fitted 0-80 C range must not be flagged as
    extrapolation, and should not be reported as duty-saturated the way the
    cone bands are -- this is the sanity anchor that the sweep's own
    mechanics (not just the underlying plant model) are behaving on data
    the calibration already covers."""
    results = sweep.run_sweep({"low": 55.0}, ["new"], workers=1)
    r = results[0]
    assert r["extrapolation"] is False
    assert r["target_c"] == 55.0


def test_cone_band_flagged_as_extrapolation_and_infeasible():
    """cone-10 (1285 C) must be flagged both as extrapolation (past
    EXTRAPOLATION_BOUNDARY_C) and as an infeasible coupled hold with the
    fixed low-temperature matrix -- the qualitative finding this sweep
    exists to surface plainly rather than silently produce a fabricated
    "sim tracked cone 10 fine" table.

    Proof this can fail: called ``ps.hold_duty_infeasible`` with the target
    divided by 100 before the check (i.e. checking feasibility near 12.85 C
    instead of 1285 C). Captured red output:

        FAILED tools/PcTools/tests/test_plant_sim_sweep.py::test_cone_band_flagged_as_extrapolation_and_infeasible
        AssertionError: cone-10 band not reported infeasible
        assert False
    """
    results = sweep.run_sweep({"cone10": 1285.0}, ["new"], workers=1)
    r = results[0]
    assert r["extrapolation"] is True
    assert r["hold_infeasible"] is True, "cone-10 band not reported infeasible"


def test_default_bands_cover_low_and_high_ranges():
    vals = sorted(sweep.DEFAULT_BANDS.values())
    assert vals[0] <= 55.0, "expected a low band inside the measured envelope"
    assert vals[-1] >= 1285.0, "expected cone 10 (~1285 C) covered"
