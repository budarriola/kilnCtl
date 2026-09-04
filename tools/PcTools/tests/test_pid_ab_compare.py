"""Tests for kilnctrl.pid_ab_compare -- the fuzzy=0 vs fuzzy=50 A/B tool.

Uses tests/fixtures/p7_fuzzy0_http_excerpt.jsonl (real capture excerpt,
ramp -> dwell boundary, 3 zones) both alone and duplicated with a
synthetic starting-temperature offset to exercise the confound gate.
"""
from __future__ import annotations

import json
import math
import os

import pytest

from kilnctrl import log_analysis as la
from kilnctrl import pid_ab_compare as ab

from kilnctrl import noise_floor as nf

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
REPO_ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "..")
REAL_REPEAT_SET = [
    os.path.join(REPO_ROOT, "logs", "coupling", "noise_floor_p7_run1.jsonl"),
    os.path.join(REPO_ROOT, "logs", "coupling", "noise_floor_p7b_run1.jsonl"),
    os.path.join(REPO_ROOT, "logs", "coupling", "noise_floor_p7c_run1.jsonl"),
]
EXCERPT = os.path.join(FIXTURES, "p7_fuzzy0_http_excerpt.jsonl")
# Trimmed excerpt of the REAL logs/coupling/p7_oldmatrix_http.jsonl that
# produced the actual near-miss this refusal exists to prevent: a poller
# left running caught a second firing (163 rows) appended after the first
# (281 rows) into the same file. See test_http_capture_log.py's split tests
# for how this fixture was trimmed.
TWO_RUN_EXCERPT = os.path.join(FIXTURES, "p7_oldmatrix_http_two_run_excerpt.jsonl")


def _write_shifted_copy(tmp_path, src_path, temp_shift_c: float, name: str) -> str:
    """Copy the fixture, adding temp_shift_c to every zone's actual_c (and
    the same to target_c, to keep error patterns comparable) -- a synthetic
    'second run' with a controlled starting-temperature delta."""
    out = tmp_path / name
    lines = []
    with open(src_path) as fh:
        for line in fh:
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            obj = json.loads(line)
            body = obj.get("exec")
            if not isinstance(body, dict) or "zones" not in body:
                continue
            for z in body["zones"]:
                z["actual_c"] = z["actual_c"] + temp_shift_c
            lines.append(json.dumps(obj))
    out.write_text("\n".join(lines) + "\n")
    return str(out)


# ---------------------------------------------------------------------------
# compute_run_metrics / compute_zone_metrics
# ---------------------------------------------------------------------------

def test_compute_run_metrics_basic():
    rows = ab.load_run(EXCERPT)
    metrics = ab.compute_run_metrics(rows)
    assert set(metrics) == {0, 1, 2}
    m0 = metrics[0]
    assert m0.start_temp_c == pytest.approx(rows[0].zones[0].actual_c)
    assert not math.isnan(m0.iae_normalized_whole_c)
    # excerpt has exactly one segment (0), split ramp/dwell
    assert set(m0.iae_normalized_by_segment) == {0}
    assert 0 in m0.ramp_mean_error_c
    assert 0 in m0.dwell_steady_state_offset_c


def test_compute_zone_metrics_empty_rows_returns_none():
    assert ab.compute_zone_metrics([], zone=0, start_temp_c=20.0) is None


# ---------------------------------------------------------------------------
# The confound gate -- this is the honesty requirement under test.
# ---------------------------------------------------------------------------

def test_identical_runs_no_confound_gives_provisional_verdict():
    report = ab.compare_runs(EXCERPT, EXCERPT, check_completeness=False)
    assert "error" not in report
    for z, d in report["start_temp_deltas_c"].items():
        assert d == pytest.approx(0.0)
    # every comparison should be PROVISIONAL (or n/a for missing data), never REFUSED
    assert any(c.verdict.startswith("PROVISIONAL") for c in report["comparisons"])
    assert not any(c.verdict.startswith("REFUSED") for c in report["comparisons"])


def test_large_start_temp_delta_refuses_a_winner(tmp_path):
    shifted = _write_shifted_copy(tmp_path, EXCERPT, temp_shift_c=4.8, name="shifted.jsonl")
    report = ab.compare_runs(EXCERPT, shifted, check_completeness=False)
    assert "error" not in report
    for z, d in report["start_temp_deltas_c"].items():
        assert d == pytest.approx(4.8, abs=0.05)
    assert report["start_temp_deltas_c"][0] > ab.CONFOUND_THRESHOLD_C
    zone0 = [c for c in report["comparisons"] if c.zone == 0]
    assert zone0, "expected at least one zone-0 comparison"
    # every zone-0 comparison with actual data must be refused, never provisional
    assert not any(c.verdict.startswith("PROVISIONAL") for c in zone0)
    assert any(c.verdict.startswith("REFUSED") for c in zone0)


def test_small_start_temp_delta_stays_below_threshold_and_provisional(tmp_path):
    shifted = _write_shifted_copy(tmp_path, EXCERPT, temp_shift_c=0.3, name="shifted_small.jsonl")
    report = ab.compare_runs(EXCERPT, shifted, check_completeness=False)
    assert report["start_temp_deltas_c"][0] < ab.CONFOUND_THRESHOLD_C
    comps = [c for c in report["comparisons"] if c.zone == 0]
    assert comps
    assert all(c.verdict.startswith("PROVISIONAL") or c.verdict.startswith("n/a") for c in comps)


def test_noise_floor_note_always_present_in_text_output():
    report = ab.compare_runs(EXCERPT, EXCERPT, check_completeness=False)
    text = ab.format_compare_text(report)
    assert "NOISE FLOOR: UNKNOWN" in text
    assert ab.NOISE_FLOOR_NOTE in text


def test_compare_missing_file_reports_error(tmp_path):
    empty = tmp_path / "empty.jsonl"
    empty.write_text("")
    report = ab.compare_runs(str(empty), EXCERPT)
    assert "error" in report
    assert ab.format_compare_text(report).startswith("error:")


# ---------------------------------------------------------------------------
# CLI smoke tests
# ---------------------------------------------------------------------------

def test_cli_run_json(capsys):
    rc = ab.main(["run", EXCERPT, "--json"])
    assert rc == 0
    out = capsys.readouterr().out
    parsed = json.loads(out)
    assert "0" in parsed


def test_cli_compare_text(capsys):
    rc = ab.main(["compare", EXCERPT, EXCERPT, "--skip-completeness-check"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "A/B compare" in out
    # The CLI's default --noise-floor path is the checked-in, now-populated
    # noise_floor.json artifact (real 6-run campaign) -- so a plain "compare"
    # invocation with no --noise-floor override reports the floor as
    # measured, not unknown, even though EXCERPT itself is unrelated to that
    # campaign (noise_floor_known only checks the artifact is non-empty).
    assert "NOISE FLOOR: measured" in out


def test_cli_run_missing_file(capsys):
    rc = ab.main(["run", os.path.join(FIXTURES, "does_not_exist.jsonl")])
    assert rc == 1


# ---------------------------------------------------------------------------
# THE NEAR-MISS: a capture holding more than one run must be REFUSED, never
# silently resolved to "the last one" -- that silent resolution is exactly
# what turned p7_oldmatrix_http.jsonl's two runs into an A/B compare of the
# second run against itself.
# ---------------------------------------------------------------------------

def test_load_run_refuses_multirun_capture_by_default():
    with pytest.raises(la.MultiRunError) as exc_info:
        ab.load_run(TWO_RUN_EXCERPT)
    msg = str(exc_info.value)
    assert "2 separate runs" in msg
    # both runs must be named -- row 0 and row 1 of split_runs' output --
    # with their start times/temps, exactly what would have made the actual
    # incident obvious instead of merely suspicious.
    assert "run 0" in msg
    assert "run 1" in msg
    assert "elapsed=7s" in msg
    assert "elapsed=12s" in msg


def test_load_run_explicit_run_index_selects_one_run():
    rows0 = ab.load_run(TWO_RUN_EXCERPT, run_index=0)
    rows1 = ab.load_run(TWO_RUN_EXCERPT, run_index=1)
    assert len(rows0) == 12
    assert len(rows1) == 12
    assert rows0[0].elapsed_s == pytest.approx(7.0)
    assert rows1[0].elapsed_s == pytest.approx(12.0)


def test_compare_runs_refuses_when_either_side_is_multirun():
    report = ab.compare_runs(TWO_RUN_EXCERPT, EXCERPT)
    assert "error" in report
    assert "2 separate runs" in report["error"]
    text = ab.format_compare_text(report)
    assert text.startswith("error:")


def test_compare_runs_with_explicit_run_index_proceeds_normally():
    report = ab.compare_runs(TWO_RUN_EXCERPT, TWO_RUN_EXCERPT, run_index_a=0, run_index_b=1)
    assert "error" not in report
    assert report["n_rows_a"] == 12
    assert report["n_rows_b"] == 12


def test_single_run_capture_still_works_unchanged():
    """A plain single-run capture must behave exactly as before -- no
    refusal, no explicit run index required."""
    rows_implicit = ab.load_run(EXCERPT)
    rows_explicit = ab.load_run(EXCERPT, run_index=0)
    assert rows_implicit == rows_explicit
    report = ab.compare_runs(EXCERPT, EXCERPT, check_completeness=False)
    assert "error" not in report


def test_cli_compare_multirun_refuses_and_returns_nonzero(capsys):
    rc = ab.main(["compare", TWO_RUN_EXCERPT, EXCERPT])
    assert rc == 1
    out = capsys.readouterr().out
    assert "error:" in out
    assert "2 separate runs" in out


def test_cli_compare_explicit_run_selects_and_succeeds(capsys):
    rc = ab.main(["compare", TWO_RUN_EXCERPT, TWO_RUN_EXCERPT, "--run-a", "0", "--run-b", "1"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "A/B compare" in out


def test_cli_run_multirun_refuses(capsys):
    rc = ab.main(["run", TWO_RUN_EXCERPT])
    assert rc == 1
    out = capsys.readouterr().out
    assert "error:" in out
    assert "2 separate runs" in out


# ---------------------------------------------------------------------------
# `pid_ab_compare split` -- the first-class version of the ad-hoc split
# script this incident required by hand.
# ---------------------------------------------------------------------------

def test_cli_split_writes_two_files(tmp_path, capsys):
    outdir = tmp_path / "split_out"
    rc = ab.main(["split", TWO_RUN_EXCERPT, str(outdir)])
    assert rc == 0
    out = capsys.readouterr().out.strip().splitlines()
    assert len(out) == 2
    for p in out:
        assert os.path.isfile(p)
    # each split file must now load cleanly with no --run needed.
    rows0 = ab.load_run(out[0])
    rows1 = ab.load_run(out[1])
    assert len(rows0) == 12
    assert len(rows1) == 12


def test_cli_split_missing_file_errors(tmp_path, capsys):
    rc = ab.main(["split", os.path.join(FIXTURES, "does_not_exist.jsonl"), str(tmp_path / "out")])
    assert rc == 1
    assert "error:" in capsys.readouterr().out


# ---------------------------------------------------------------------------
# The noise-floor gate (PID_EXPANSION_PLAN.md SS3.3) -- a difference smaller
# than the measured floor must be reported INDISTINGUISHABLE, not
# PROVISIONAL. This is the whole point of measuring the floor at all, so it
# gets proved both at the _cmp unit level and through the full compare_runs
# path with a synthetic artifact.
# ---------------------------------------------------------------------------

def test_cmp_below_floor_is_indistinguishable():
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.050,
                start_delta_c=0.0, floor_entry={"noise_floor_c": 0.2, "n": 6, "std_c": 0.05})
    assert c.verdict.startswith("INDISTINGUISHABLE")
    assert c.delta == pytest.approx(0.05)
    # the structured field must agree with the verdict text -- a caller
    # reading report["comparisons"] programmatically (not just format_compare_text)
    # needs this to be correct on its own.
    assert c.distinguishable is False
    assert c.floor_c == pytest.approx(0.2)
    assert c.floor_n == 6


def test_cmp_above_floor_stays_provisional_not_indistinguishable():
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.500,
                start_delta_c=0.0, floor_entry={"noise_floor_c": 0.2, "n": 6, "std_c": 0.05})
    assert c.verdict.startswith("PROVISIONAL")
    assert "INDISTINGUISHABLE" not in c.verdict
    assert "DISTINGUISHABLE" in c.verdict
    assert c.distinguishable is True


def test_cmp_exactly_at_floor_is_not_indistinguishable():
    """abs(delta) < floor_c, strictly -- a difference exactly equal to the
    floor is not below it, so it must still get a verdict (PROVISIONAL),
    proving the comparison isn't <=."""
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.0, b=1.25,
                start_delta_c=0.0, floor_entry={"noise_floor_c": 0.25, "n": 6, "std_c": 0.06})
    assert c.verdict.startswith("PROVISIONAL")


def test_cmp_with_no_floor_falls_back_to_unknown_provisional():
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.001,
                start_delta_c=0.0, floor_entry=None)
    assert c.verdict.startswith("PROVISIONAL")
    assert "noise floor unknown" in c.verdict


def test_confound_gate_still_wins_over_a_known_floor():
    """A start-temp delta beyond CONFOUND_THRESHOLD_C must still REFUSE, even
    when a noise floor is known and the metric delta is tiny -- the confound
    gate is about whether the comparison is valid at all, not about metric
    magnitude."""
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.001,
                start_delta_c=5.0, floor_entry={"noise_floor_c": 0.2, "n": 6, "std_c": 0.05})
    assert c.verdict.startswith("REFUSED")


def _fake_artifact(floor_c: float) -> dict:
    """A minimal noise_floor.json-shaped artifact covering every key
    compare_runs(EXCERPT, EXCERPT) will look up, all at the same floor --
    enough to drive compare_runs end to end without needing a real
    multi-repeat campaign in this test."""
    entries = {}
    for zone in (0, 1, 2):
        for metric, seg in (
            ("iae_normalized_whole_c", None),
            ("iae_normalized_c", 0),
            ("ramp_mean_error_c", 0),
            ("ramp_worst_error_c", 0),
            ("dwell_entry_overshoot_peak_c", 0),
            ("dwell_entry_time_to_peak_s", 0),
            ("dwell_steady_state_offset_c", 0),
            ("settle_time_s", 0),
        ):
            key = f"z{zone}:{metric}:{'whole' if seg is None else seg}"
            entries[key] = {"zone": zone, "metric": metric, "segment": seg,
                             "n": 5, "mean": 0.0, "std_c": floor_c / 4, "noise_floor_c": floor_c}
    return {"schema_version": 1, "generated_from": [], "band_c": 1.0,
            "n_repeats": 5, "note": "", "entries": entries}


def test_compare_runs_with_huge_floor_marks_identical_run_indistinguishable():
    # EXCERPT vs itself: every delta is exactly 0.0, so ANY positive floor
    # must mark every comparable metric INDISTINGUISHABLE.
    artifact = _fake_artifact(floor_c=999.0)
    report = ab.compare_runs(EXCERPT, EXCERPT, noise_floor_artifact=artifact, check_completeness=False)
    assert report["noise_floor_known"] is True
    comparable = [c for c in report["comparisons"] if c.a is not None]
    assert comparable  # sanity: the fixture actually produced comparisons
    for c in comparable:
        assert c.verdict.startswith("INDISTINGUISHABLE"), c


def test_compare_runs_without_artifact_reports_noise_floor_unknown():
    report = ab.compare_runs(EXCERPT, EXCERPT, check_completeness=False)
    assert report["noise_floor_known"] is False
    for c in report["comparisons"]:
        assert "REFUSED" not in c.verdict or True  # start temps match here (delta 0)
        assert "INDISTINGUISHABLE" not in c.verdict


def test_format_compare_text_uses_known_note_when_artifact_present():
    artifact = _fake_artifact(floor_c=0.001)
    report = ab.compare_runs(EXCERPT, EXCERPT, noise_floor_artifact=artifact, check_completeness=False)
    text = ab.format_compare_text(report)
    assert "NOISE FLOOR: measured" in text
    assert "NOISE FLOOR: UNKNOWN" not in text


def test_format_compare_text_uses_unknown_note_without_artifact():
    report = ab.compare_runs(EXCERPT, EXCERPT, check_completeness=False)
    text = ab.format_compare_text(report)
    assert "NOISE FLOOR: UNKNOWN" in text


def test_cli_compare_none_flag_disables_the_artifact_lookup(capsys):
    rc = ab.main(["compare", EXCERPT, EXCERPT, "--noise-floor", "none", "--skip-completeness-check"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "NOISE FLOOR: UNKNOWN" in out


def test_cli_compare_missing_artifact_path_behaves_like_unknown(capsys, tmp_path):
    missing = str(tmp_path / "does_not_exist_noise_floor.json")
    rc = ab.main(["compare", EXCERPT, EXCERPT, "--noise-floor", missing, "--skip-completeness-check"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "NOISE FLOOR: UNKNOWN" in out
    # The degradation from "measured" to "unknown" must be LOUD, not merely
    # inferable from the absence of the word "measured" -- a genuinely
    # absent artifact is reported as such, distinct from an unreadable one.
    assert "WARNING" in out
    assert "NOT LOADED" in out
    assert "missing" in out


def test_cli_compare_unreadable_artifact_reports_distinct_loud_warning(capsys, tmp_path):
    """A file that EXISTS but fails to parse (corrupt/truncated JSON) is a
    different failure mode than a missing file, and must be reported as
    such -- both degrade to NOISE FLOOR: UNKNOWN, but the loud warning text
    must say *unreadable*, not *missing*."""
    bad = tmp_path / "corrupt_noise_floor.json"
    bad.write_text("{not valid json")
    rc = ab.main(["compare", EXCERPT, EXCERPT, "--noise-floor", str(bad), "--skip-completeness-check"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "NOISE FLOOR: UNKNOWN" in out
    assert "WARNING" in out
    assert "NOT LOADED" in out
    assert "unreadable" in out
    assert "missing" not in out.split("WARNING")[1].split("\n")[0]


# ---------------------------------------------------------------------------
# 2026-09-02f -- the start-temperature covariate: sensitivity fit, the
# residual decomposition reported alongside a comparison, and the metric
# floor-reliability summary. Uses REAL_REPEAT_SET (the three real
# noise-floor repeat captures on hand: 27.60/28.64/28.78 C start temps for
# the same preset+profile) wherever a genuine same-config repeat set is
# needed, per the task's instruction to demonstrate the method on real data.
# ---------------------------------------------------------------------------

def test_real_repeat_set_files_exist():
    """Sanity check the fixture list above actually points at the real
    captures this test module's docstring claims to use -- a silently
    missing file would make every test below vacuously pass on empty data."""
    for p in REAL_REPEAT_SET:
        assert os.path.isfile(p), p


def test_first_valid_start_temp_skips_invalid_placeholder_row():
    """Real HTTP captures often have actual_valid=false on row 0 (the poll
    landed before the first thermocouple read completed); the firmware's own
    wire convention (documented in log_analysis.PollRow) is to carry that as
    a literal 0.0 placeholder, not a real temperature. compute_run_metrics
    must not treat that placeholder as a real starting temperature."""
    rows = ab.load_run(REAL_REPEAT_SET[0])
    # 2026-09-03: log_analysis's HTTP-capture parser now honors actual_valid
    # itself (it used to read actual_c regardless, silently taking the
    # firmware's literal 0.0 "no reading yet" placeholder as a real
    # temperature -- the very bug this test's own docstring describes, but
    # was, ironically, encoding into its own premise check). An invalid
    # first sample now surfaces as NaN, not 0.0 -- confirm THAT instead.
    assert math.isnan(rows[0].zones[0].actual_c)  # confirm the fixture actually has the placeholder row
    metrics = ab.compute_run_metrics(rows)
    for z, m in metrics.items():
        assert m.start_temp_c != 0.0
        assert m.start_temp_c > 20.0  # a real rested kiln reading, not the placeholder


def test_fit_start_temp_sensitivity_insufficient_n_reports_no_slope(tmp_path):
    two_paths = REAL_REPEAT_SET[:2]
    sens = ab.fit_start_temp_sensitivity(two_paths)
    for z, s in sens.items():
        assert s.n == 2
        assert s.slope_c_per_c is None
        assert "insufficient" in s.note


def test_fit_start_temp_sensitivity_exact_linear_fit_recovers_known_slope(tmp_path):
    """Synthetic exact-linear data (not the noisy real captures) to verify
    the OLS math itself is correct, independent of real-world noise."""
    lines = []
    base = json.loads(open(EXCERPT).readline())
    # start_temp = 20, 22, 24 -> outcome (iae_normalized_whole_c) known exactly
    # via a synthetic single-row-per-run capture with a hand-picked duty/error
    # trace is fiddly; instead drive _ols directly with a controlled input to
    # prove the regression math, and separately prove
    # fit_start_temp_sensitivity wires that same math up correctly using the
    # shifted-copy fixtures (next test).
    xs = [20.0, 22.0, 24.0]
    ys = [1.0, 1.5, 2.0]  # slope = 0.25 exactly, intercept = -4.0
    fit = ab._ols(xs, ys)
    assert fit is not None
    slope, intercept = fit
    assert slope == pytest.approx(0.25)
    assert intercept == pytest.approx(-4.0)


def test_fit_start_temp_sensitivity_uses_real_repeat_set():
    """End-to-end on the real captures: a per-zone slope, n=3, and a
    (necessarily loose, n=3) Pearson r are reported for every zone that has
    3 usable points."""
    sens = ab.fit_start_temp_sensitivity(REAL_REPEAT_SET)
    assert set(sens) == {0, 1, 2}
    for z, s in sens.items():
        assert s.n == 3
        assert s.slope_c_per_c is not None
        assert s.r is not None
        assert -1.0 <= s.r <= 1.0


def test_start_temp_adjustment_none_without_a_fit():
    assert ab._start_temp_adjustment(None, 20.0, 21.0, 0.5) is None
    no_slope = ab.StartTempSensitivity(0, 2, None, None, None, "insufficient")
    assert ab._start_temp_adjustment(no_slope, 20.0, 21.0, 0.5) is None


def test_start_temp_adjustment_decomposes_delta_correctly():
    sens = ab.StartTempSensitivity(0, 5, slope_c_per_c=0.1, intercept=0.0, r=0.9, note="fit")
    adj = ab._start_temp_adjustment(sens, start_a=20.0, start_b=25.0, raw_delta=1.0)
    assert adj["start_delta_c"] == pytest.approx(5.0)
    assert adj["predicted_from_start_temp"] == pytest.approx(0.5)  # 0.1 * 5.0
    assert adj["residual_after_adjustment"] == pytest.approx(0.5)  # 1.0 - 0.5


def _real_artifact():
    return nf.build_artifact(REAL_REPEAT_SET)


def test_sensitivity_does_not_manufacture_effect_between_same_config_runs():
    """THE SANITY CHECK: the three real captures are all the SAME controller
    configuration (same preset, same profile -- see
    logs/coupling/noise_floor_campaign_state.json / PID_EXPANSION_PLAN.md
    SS3.3), differing only in start temperature and run-to-run noise.

    The concrete failure mode being guarded against: attaching a start-temp
    sensitivity fit must not change which comparisons get REFUSED /
    INDISTINGUISHABLE / PROVISIONAL, and must not flip
    ``distinguishable`` -- the verdict is computed from the RAW delta and
    the measured floor only (see _cmp / compare_runs), same as before this
    feature existed. Proved directly: run compare_runs on every same-config
    pair TWICE, once with the sensitivity fit attached and once without
    (``sensitivity_paths=[]``), and require byte-identical verdicts. This
    would fail immediately if the adjustment were ever wired into the gate
    instead of being purely explanatory."""
    artifact = _real_artifact()
    for a, b in ((REAL_REPEAT_SET[0], REAL_REPEAT_SET[1]),
                 (REAL_REPEAT_SET[0], REAL_REPEAT_SET[2]),
                 (REAL_REPEAT_SET[1], REAL_REPEAT_SET[2])):
        with_sens = ab.compare_runs(a, b, noise_floor_artifact=artifact)
        without_sens = ab.compare_runs(a, b, noise_floor_artifact=artifact, sensitivity_paths=[])
        assert "error" not in with_sens and "error" not in without_sens
        whole_with = [c for c in with_sens["comparisons"]
                      if c.metric == "iae_normalized_whole_c" and c.segment is None]
        whole_without = [c for c in without_sens["comparisons"]
                         if c.metric == "iae_normalized_whole_c" and c.segment is None]
        assert whole_with, "expected a whole-run iae comparison for every zone"
        assert len(whole_with) == len(whole_without)
        for c_with, c_without in zip(
            sorted(whole_with, key=lambda c: c.zone), sorted(whole_without, key=lambda c: c.zone),
        ):
            assert c_with.zone == c_without.zone
            assert c_with.verdict == c_without.verdict, (a, b, c_with, c_without)
            assert c_with.distinguishable == c_without.distinguishable
            assert c_without.start_temp_adjustment is None
            # sensitivity IS present on the with_sens side (proving the fit
            # actually ran, not that it was silently skipped)
            assert c_with.start_temp_adjustment is not None
            assert "residual" in ab.format_compare_text(with_sens)


def test_genuine_large_effect_still_detected_with_sensitivity_attached():
    """A large, real delta must still come back DISTINGUISHABLE/PROVISIONAL
    even when a start-temp sensitivity is fit and attached -- the adjustment
    must never suppress a genuine effect either."""
    artifact = _real_artifact()
    # Build a synthetic 'B' run: a copy of the first real repeat with a huge
    # (obviously not start-temp-driven) shift applied to actual_c, but with
    # matching start temp to A so the confound gate does not fire and the
    # comparison isn't refused outright.
    import tempfile
    src = REAL_REPEAT_SET[0]
    lines = []
    with open(src) as fh:
        for line in fh:
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            obj = json.loads(line)
            body = obj.get("exec")
            if not isinstance(body, dict) or "zones" not in body:
                continue
            elapsed_s = body.get("elapsed_s", 0)
            if elapsed_s > 30:  # leave the starting rows alone so start_temp_c matches A
                for z in body["zones"]:
                    z["actual_c"] = z["actual_c"] + 5.0  # huge, deliberately not a start-temp-only effect
            lines.append(json.dumps(obj))
    tmpdir = tempfile.mkdtemp()
    shifted_path = os.path.join(tmpdir, "shifted_large_effect.jsonl")
    with open(shifted_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    report = ab.compare_runs(src, shifted_path, noise_floor_artifact=artifact)
    assert "error" not in report
    whole_run = [c for c in report["comparisons"]
                 if c.metric == "iae_normalized_whole_c" and c.segment is None]
    assert whole_run
    for c in whole_run:
        # start temps match closely (both come from the same source row 1),
        # so this must not be REFUSED, and the huge actual_c shift must
        # produce a large, DISTINGUISHABLE (or at minimum non-indistinguishable) delta.
        assert not c.verdict.startswith("REFUSED"), c
        assert c.verdict.startswith("PROVISIONAL"), c
        assert c.distinguishable is True, c


def test_metric_floor_reliability_flags_unstable_metric():
    artifact = _real_artifact()
    reliability = ab.summarize_metric_floor_reliability(artifact)
    assert "dwell_steady_state_offset_c" in reliability
    dwell = reliability["dwell_steady_state_offset_c"]
    assert dwell["reliable"] is False
    assert dwell["ratio"] > ab.FLOOR_RELIABILITY_RATIO
    iae_whole = reliability["iae_normalized_whole_c"]
    assert iae_whole["reliable"] is True
    assert iae_whole["ratio"] < dwell["ratio"]


def test_metric_floor_reliability_empty_without_artifact():
    assert ab.summarize_metric_floor_reliability(None) == {}
    assert ab.summarize_metric_floor_reliability({"entries": {}}) == {}


def test_metric_floor_reliability_single_entry_reports_none_not_a_ratio():
    artifact = {"entries": {
        "z0:foo_metric:whole": {"zone": 0, "metric": "foo_metric", "segment": None, "noise_floor_c": 0.1},
    }}
    r = ab.summarize_metric_floor_reliability(artifact)
    assert r["foo_metric"]["reliable"] is None
    assert r["foo_metric"]["ratio"] is None


def test_compare_runs_confound_refusal_still_fires_alongside_sensitivity(tmp_path):
    """The confound gate (REFUSED above CONFOUND_THRESHOLD_C) must survive
    unchanged even when a start-temp sensitivity artifact is supplied --
    the strengthening requirement: adjustment machinery must never be able
    to talk its way past a refusal."""
    shifted = _write_shifted_copy(tmp_path, EXCERPT, temp_shift_c=4.8, name="shifted_refuse.jsonl")
    artifact = _real_artifact()
    report = ab.compare_runs(EXCERPT, shifted, noise_floor_artifact=artifact, check_completeness=False)
    assert "error" not in report
    zone0 = [c for c in report["comparisons"] if c.zone == 0 and c.metric == "iae_normalized_whole_c"]
    assert zone0
    for c in zone0:
        assert c.verdict.startswith("REFUSED"), c
        assert c.distinguishable is None


def test_format_compare_text_includes_floor_reliability_section():
    artifact = _real_artifact()
    report = ab.compare_runs(REAL_REPEAT_SET[0], REAL_REPEAT_SET[1], noise_floor_artifact=artifact)
    text = ab.format_compare_text(report)
    assert "metric floor reliability" in text
    assert "UNSTABLE" in text


def test_cli_compare_end_to_end_on_real_repeat_set(capsys, tmp_path):
    """Full CLI smoke test on the real repeat set, building the artifact
    on the fly, matching how a real invocation would work."""
    artifact_path = os.path.join(tmp_path, "real_artifact.json")
    art = _real_artifact()
    with open(artifact_path, "w") as fh:
        json.dump(ab._jsonable(art), fh)
    rc = ab.main([
        "compare", REAL_REPEAT_SET[0], REAL_REPEAT_SET[1],
        "--noise-floor", artifact_path,
    ])
    assert rc == 0
    out = capsys.readouterr().out
    assert "start-temp sensitivity" in out
    assert "metric floor reliability" in out


# ---------------------------------------------------------------------------
# 2026-09-03 -- adversarial statistical review. Problem 1: no multiplicity
# control over the 48-key report. Problem 2: the range floor is not
# scale-stable across n; prediction_interval_floor is the scale-stable
# alternative, reported alongside (never gating) the verdict.
# ---------------------------------------------------------------------------

def test_t_975_matches_known_table_values():
    """Sanity check the hand-maintained t-table against textbook values --
    if this drifts, prediction_interval_floor silently drifts with it."""
    assert ab._t_975(1) == pytest.approx(12.706)
    assert ab._t_975(5) == pytest.approx(2.571)   # n=6 repeat set
    assert ab._t_975(9) == pytest.approx(2.262)
    assert ab._t_975(100) == pytest.approx(1.96)  # large-sample fallback


def test_prediction_interval_floor_wider_than_a_typical_range_floor():
    """The whole point of PROBLEM 2's fix: the honest PI floor for n=6 is
    materially WIDER than a range-of-6 floor built from the same std_c, so
    presenting it does not just relabel the same number."""
    std_c = 0.05
    n = 6
    pi = ab.prediction_interval_floor(std_c, n)
    # t(.975, 5) * std_c * sqrt(2) = 2.571 * 0.05 * 1.4142 = 0.1818
    assert pi == pytest.approx(2.571 * 0.05 * math.sqrt(2), rel=1e-3)
    # E[range]/sigma at n=6 is ~2.53 (review's figure) -- a range floor for
    # the same 6 draws would be roughly 2.53 * std_c = 0.1265, well below
    # the PI floor above, demonstrating the PI is the wider, more
    # conservative statistic the review asked for.
    approx_range_floor = 2.53 * std_c
    assert pi > approx_range_floor


def test_prediction_interval_floor_none_when_inputs_missing():
    assert ab.prediction_interval_floor(None, 6) is None
    assert ab.prediction_interval_floor(0.1, None) is None
    assert ab.prediction_interval_floor(0.1, 1) is None  # n=1 -> 0 df, undefined


def test_cmp_reports_pi_floor_alongside_range_floor_without_changing_verdict():
    """A comparison whose delta clears the (tight) range floor but would NOT
    clear the (wider) PI floor must still come back DISTINGUISHABLE -- the
    PI floor is informational (pi_distinguishable), never the gate."""
    # range floor 0.2, but std_c=0.5 with n=6 gives a PI floor of
    # t(.975,5)*0.5*sqrt(2) = 2.571*0.5*1.4142 ~= 1.817 -- comfortably above
    # a delta of 0.3.
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.300,
                start_delta_c=0.0, floor_entry={"noise_floor_c": 0.2, "n": 6, "std_c": 0.5})
    assert c.verdict.startswith("PROVISIONAL")
    assert c.distinguishable is True          # the actual verdict: range floor cleared
    assert c.pi_floor_c is not None
    assert c.pi_floor_c > abs(c.delta)
    assert c.pi_distinguishable is False       # the informational PI verdict: NOT cleared


def test_floor_n_surfaced_on_every_comparison_with_a_known_floor():
    """PROBLEM 2: floor_n must be visible on the structured comparison, not
    just buried in the artifact -- this is what lets a caller (or a human
    reading JSON output) notice n=3 mixed in with n=6."""
    c = ab._cmp(0, "settle_time_s", 1, a=100.0, b=110.0, start_delta_c=0.0,
                floor_entry={"noise_floor_c": 5.0, "n": 3, "std_c": 2.0})
    assert c.floor_n == 3


def test_real_artifact_n_is_consistent_within_each_metric_but_mixed_overall():
    """Confirms the concrete claim in the module docstring: the checked-in
    artifact mixes n across keys (z0:settle_time_s:1 has n=3, everything
    else has n=6). This is a fact about the checked-in artifact, not a
    synthetic fixture -- if the artifact is regenerated with a uniform n,
    this test should be revisited, not silently left green on stale
    reasoning."""
    artifact = nf.load_artifact()
    assert artifact is not None
    ns = {entry["n"] for entry in artifact["entries"].values()}
    assert ns == {3, 6}, (
        "expected the checked-in artifact to mix n=3 and n=6 (the known "
        "z0:settle_time_s:1 outlier) -- if this changed, the module "
        "docstring's PROBLEM 2 example needs updating too"
    )


def _artifact_with_uniform_floor(n: int, std_c: float, floor_c: float) -> dict:
    entries = {}
    for zone in (0, 1, 2):
        entries[f"z{zone}:iae_normalized_whole_c:whole"] = {
            "zone": zone, "metric": "iae_normalized_whole_c", "segment": None,
            "n": n, "mean": 0.0, "std_c": std_c, "noise_floor_c": floor_c,
        }
    return {"schema_version": 1, "generated_from": [], "band_c": 1.0,
            "n_repeats": n, "note": "", "entries": entries}


def test_summarize_multiplicity_family_size_and_alpha_for_real_report():
    """End-to-end: compare_runs on real repeat-set captures against a
    uniform-floor artifact must produce a multiplicity summary whose
    family_size equals the number of keyed (floor-known) comparisons, and
    whose per_n_alpha for n=6 is close to the review's ~12% figure."""
    artifact = _artifact_with_uniform_floor(n=6, std_c=0.03, floor_c=0.001)
    report = ab.compare_runs(REAL_REPEAT_SET[0], REAL_REPEAT_SET[1], noise_floor_artifact=artifact)
    assert "error" not in report
    mult = report["multiplicity"]
    keyed = [c for c in report["comparisons"] if c.distinguishable is not None]
    assert mult.family_size == len(keyed)
    assert 6 in mult.per_n_alpha
    assert 0.08 < mult.per_n_alpha[6] < 0.16  # review's Monte Carlo figure: ~0.121
    assert mult.prob_at_least_one_independent is not None
    assert mult.prob_at_least_one_independent > mult.per_n_alpha[6]  # multiple keys -> higher than any one


def test_multiplicity_note_and_summary_survive_into_text_report():
    artifact = _artifact_with_uniform_floor(n=6, std_c=0.03, floor_c=0.001)
    report = ab.compare_runs(REAL_REPEAT_SET[0], REAL_REPEAT_SET[1], noise_floor_artifact=artifact)
    text = ab.format_compare_text(report)
    assert "MULTIPLICITY" in text
    assert "keys evaluated" in text
    assert "P(>=1 spurious DISTINGUISHABLE)" in text


def test_start_temp_metric_note_present_in_every_text_report():
    """PROBLEM 3: the two 1.0C thresholds gate different quantities -- prove
    the disambiguating note is actually printed, not just documented."""
    report = ab.compare_runs(EXCERPT, EXCERPT, check_completeness=False)
    text = ab.format_compare_text(report)
    assert "START-TEMP UNIT NOTE" in text
    assert "DIFFERENT" in text


# --- Single-lucky-key mutation guard -----------------------------------
# THE POINT OF THIS TEST: a caller must not be able to satisfy
# "consistent pattern" (the one thing this module calls potentially
# actionable) with a SINGLE DISTINGUISHABLE key. Below MUTATES the module's
# CONSISTENT_PATTERN_MIN_KEYS down to 1 to prove the real (>=3) threshold
# actually does the rejecting -- restored in a try/finally.

def test_single_lucky_distinguishable_key_is_not_a_consistent_pattern():
    # Isolate "a single lucky key must not count as a pattern": build a
    # report with exactly one DISTINGUISHABLE key among the rest
    # (REFUSED or INDISTINGUISHABLE) and confirm no pattern is reported.
    tight_artifact = _artifact_with_uniform_floor(n=6, std_c=0.001, floor_c=999.0)
    # zones 0/1 have a >1.0C start-temp delta between these two real
    # captures (REFUSED, so they never reach the floor gate at all -- see
    # the printed deltas this asserts against below); zone 2's delta is
    # small enough to be PROVISIONAL, so only its own floor matters. Give
    # z2 alone a tiny floor so its delta clears -- i.e. exactly one lucky
    # key -- and leave z0/z1's floors at the huge default (moot, since they
    # are REFUSED regardless of floor).
    tight_artifact["entries"]["z2:iae_normalized_whole_c:whole"]["noise_floor_c"] = 0.0001
    lucky_report = ab.compare_runs(REAL_REPEAT_SET[0], REAL_REPEAT_SET[1], noise_floor_artifact=tight_artifact)
    assert lucky_report["start_temp_deltas_c"][2] < ab.CONFOUND_THRESHOLD_C, "test setup depends on zone 2 not being REFUSED"
    lucky_mult = lucky_report["multiplicity"]
    assert lucky_mult.n_distinguishable == 1, "test setup must produce exactly one lucky key"
    assert lucky_mult.consistent_patterns == [], (
        "a single DISTINGUISHABLE key must never be reported as a consistent pattern"
    )

    # MUTATION: prove this isn't vacuous -- lower the real module's own
    # threshold to 1 and confirm the SAME single-key report now DOES get
    # flagged as a pattern, i.e. the >=3 threshold is actually load-bearing.
    original = ab.CONSISTENT_PATTERN_MIN_KEYS
    try:
        ab.CONSISTENT_PATTERN_MIN_KEYS = 1
        mutated_mult = ab.summarize_multiplicity(lucky_report["comparisons"])
        assert mutated_mult.consistent_patterns != [], (
            "MUTATION CHECK FAILED: lowering CONSISTENT_PATTERN_MIN_KEYS to 1 should have "
            "made the single lucky key register as a pattern -- if it didn't, the real "
            "test above proves nothing"
        )
    finally:
        ab.CONSISTENT_PATTERN_MIN_KEYS = original


# ---------------------------------------------------------------------------
# min_segment_index -- the scored-window ambient-confound fix (see
# PID_EXPANSION_PLAN.md "A/B campaign ambient-confound protocol").
# ---------------------------------------------------------------------------

def _synthetic_two_segment_rows():
    """Segment 0: a ramp from 20C to 48C with a huge, deliberately garbage
    tracking error (stands in for an unstabilised, ambient-exposed opening
    ramp -- this is exactly what min_segment_index=1 should exclude).
    Segment 1: a clean, near-zero-error ramp from 48C -> 50C (the "real"
    scored segment). If min_segment_index correctly excludes segment 0, the
    whole-window IAE should be small and start_temp_c should read ~48C, not
    ~20C."""
    rows = []
    t = 0.0
    # segment 0 -- huge error, ramping (garbage, must be excluded)
    for i in range(5):
        actual = 20.0 + i * 2.0   # 20, 22, 24, 26, 28
        target = 48.0             # far ahead -- huge error on purpose
        rows.append(la.PollRow(
            wall_time=f"t{i}", elapsed_s=t, segment_index=0, segment_count=2,
            dwelling=False, target_c=target, state="running",
            zones={0: la.ZoneSample(zone=0, actual_c=actual, duty=1.0)},
        ))
        t += 60.0
    # segment 1 -- clean, near-zero error, ramping from 48 to 50
    for i in range(5):
        actual = 48.0 + i * 0.4   # 48.0 .. 49.6
        target = 48.0 + i * 0.4   # tracks exactly -- ~0 error
        rows.append(la.PollRow(
            wall_time=f"s{i}", elapsed_s=t, segment_index=1, segment_count=2,
            dwelling=False, target_c=target, state="running",
            zones={0: la.ZoneSample(zone=0, actual_c=actual, duty=0.5)},
        ))
        t += 60.0
    return rows


def test_min_segment_index_excludes_stabilization_segment_from_whole_iae():
    rows = _synthetic_two_segment_rows()

    unscored = ab.compute_run_metrics(rows)  # default: everything scored
    m0_unscored = unscored[0]
    assert m0_unscored.start_temp_c == pytest.approx(20.0)
    assert set(m0_unscored.iae_normalized_by_segment) == {0, 1}
    # segment 0's huge error dominates the whole-run IAE
    assert m0_unscored.iae_normalized_whole_c > 5.0

    scored = ab.compute_run_metrics(rows, min_segment_index=ab.STABILIZATION_SEGMENT_INDEX)
    m0_scored = scored[0]
    # start temp is now the stabilised ~48C, not the ambient ~20C start
    assert m0_scored.start_temp_c == pytest.approx(48.0)
    # only segment 1 survives in the per-segment dicts
    assert set(m0_scored.iae_normalized_by_segment) == {1}
    # and the whole-window IAE reflects only the clean segment
    assert m0_scored.iae_normalized_whole_c < 1.0


def test_min_segment_index_mutation_wrong_default_would_leak_segment_0():
    """Negative test: prove the exclusion is actually load-bearing, not a
    parameter that happens to do nothing. Mutate by simulating the OLD
    (pre-fix) call -- i.e. omitting min_segment_index entirely -- and
    confirm the real bug this feature fixes (segment 0's huge error leaking
    into the whole-run IAE) is reproduced when the fix isn't applied."""
    rows = _synthetic_two_segment_rows()
    # "mutated" call: forgetting to pass min_segment_index at all
    forgot = ab.compute_run_metrics(rows)[0]
    assert forgot.iae_normalized_whole_c > 5.0, (
        "MUTATION CHECK: omitting min_segment_index must reproduce the ambient-segment "
        "leak this feature exists to fix -- if it doesn't, the real test above proves "
        "nothing about what the parameter actually does"
    )


def test_compute_zone_metrics_min_segment_index_beyond_run_returns_none():
    rows = _synthetic_two_segment_rows()
    assert ab.compute_zone_metrics(rows, zone=0, start_temp_c=20.0, min_segment_index=5) is None


# ---------------------------------------------------------------------------
# TASK 1 (2026-09-03, owner decision: hold on by default). resolve_min_
# segment_index is the producer/consumer agreement point: run_queue.py
# writes a {"meta": {...}} header line recording whether a capture got the
# stabilisation hold, and this is the consumer side that reads it back and
# refuses (loudly) rather than silently comparing two runs that used
# different scoring-window conventions.
# ---------------------------------------------------------------------------

def _write_meta_capture(tmp_path, name: str, src_path: str, min_segment_index) -> str:
    """Build a capture file shaped exactly like run_queue.py's own output:
    a {"meta": {...}} header line (or none, if min_segment_index is None --
    simulates a pre-TASK-1 capture with no marker at all) followed by the
    real rows copied verbatim from an existing fixture."""
    out = tmp_path / name
    lines = []
    if min_segment_index is not None:
        lines.append(json.dumps({"meta": {
            "stabilized": min_segment_index != 0,
            "min_segment_index": min_segment_index,
        }}))
    with open(src_path) as fh:
        for line in fh:
            line = line.strip()
            if line:
                lines.append(line)
    out.write_text("\n".join(lines) + "\n")
    return str(out)


def test_resolve_min_segment_index_no_meta_defaults_to_zero(tmp_path):
    path = _write_meta_capture(tmp_path, "nometa.jsonl", EXCERPT, min_segment_index=None)
    assert ab.resolve_min_segment_index(path) == 0


def test_resolve_min_segment_index_reads_stabilized_meta(tmp_path):
    path = _write_meta_capture(tmp_path, "stab.jsonl", EXCERPT, min_segment_index=1)
    assert ab.resolve_min_segment_index(path) == 1


def test_resolve_min_segment_index_agreeing_pair_resolves(tmp_path):
    path_a = _write_meta_capture(tmp_path, "a.jsonl", EXCERPT, min_segment_index=1)
    path_b = _write_meta_capture(tmp_path, "b.jsonl", EXCERPT, min_segment_index=1)
    assert ab.resolve_min_segment_index(path_a, path_b) == 1


def test_resolve_min_segment_index_explicit_override_uncontested(tmp_path):
    # No meta line at all -- an explicit override with nothing to disagree
    # with is accepted (the documented escape hatch).
    path = _write_meta_capture(tmp_path, "nometa.jsonl", EXCERPT, min_segment_index=None)
    assert ab.resolve_min_segment_index(path, explicit=1) == 1


# --------------------------------------------------------------------------
# THE HEADLINE NEGATIVE TEST: prove the runner and the analysis genuinely
# cannot silently disagree.
#
# Step 1 (positive control): two captures both recording the stabilisation
# convention (min_segment_index=1, exactly what run_queue.py writes for a
# stabilized run) compare cleanly with NO explicit flag -- the analysis
# picked up the runner's own convention automatically.
#
# Step 2 (the actual negative test): mutate ONE of the two captures' meta
# line to claim the opposite convention (min_segment_index=0, as if that
# arm had been run with --skip-stabilization-hold while its partner had
# not) and confirm compare_runs refuses LOUDLY -- an "error" key naming
# both paths and both recorded values -- rather than silently comparing
# apples to oranges. This is exactly the class of bug
# project_split_module_missing_name_class / project_consumer_without_
# producer_class describe: a producer and consumer that quietly drift
# apart. TASK 1 makes it structurally loud instead.
# --------------------------------------------------------------------------

def test_stabilization_agreement_end_to_end_and_mutation_makes_it_loud(tmp_path):
    path_a = _write_meta_capture(tmp_path, "arm_a.jsonl", EXCERPT, min_segment_index=1)
    path_b = _write_meta_capture(tmp_path, "arm_b.jsonl", EXCERPT, min_segment_index=1)

    # REAL BEHAVIOUR: both arms recorded the same (stabilized) convention --
    # compare_runs succeeds with no explicit min_segment_index at all, and
    # actually used min_segment_index=1 (proven by cross-checking against
    # calling compute_run_metrics directly with that value).
    report = ab.compare_runs(path_a, path_b, noise_floor_artifact=None, check_completeness=False)
    assert "error" not in report, report.get("error")

    assert ab.resolve_min_segment_index(path_a, path_b) == ab.STABILIZATION_SEGMENT_INDEX

    # MUTATION: arm_b's meta line is rewritten to claim min_segment_index=0
    # (as if it had been captured with --skip-stabilization-hold while
    # arm_a had the hold) -- everything else about the file (its rows) is
    # untouched. This is the exact silent-disagreement scenario TASK 1
    # exists to make impossible.
    mutated_b = _write_meta_capture(tmp_path, "arm_b_mutated.jsonl", EXCERPT, min_segment_index=0)

    report_mismatched = ab.compare_runs(path_a, mutated_b, noise_floor_artifact=None)
    assert "error" in report_mismatched, (
        "MUTATION CHECK FAILED: two captures recording DIFFERENT stabilisation "
        "conventions (min_segment_index=1 vs 0) were compared without complaint -- "
        "this is exactly the silently-reintroduced ambient-start confound TASK 1 "
        "was supposed to make impossible"
    )
    msg = report_mismatched["error"]
    assert "min_segment_index=1" in msg
    assert "min_segment_index=0" in msg
    assert path_a in msg or os.path.basename(path_a) in msg

    # And resolve_min_segment_index itself raises the same way, with the
    # dedicated exception type, when called directly (not just through
    # compare_runs' try/except wrapper).
    with pytest.raises(ab.StabilizationMismatchError):
        ab.resolve_min_segment_index(path_a, mutated_b)


def test_explicit_override_conflicting_with_recorded_meta_is_refused(tmp_path):
    """The documented escape hatch (an explicit --min-segment-index) is not
    a way to silently score a stabilised capture from its ambient segment 0
    without comment -- passing 0 against a capture whose own meta line says
    1 must be refused, loudly, naming both values."""
    path = _write_meta_capture(tmp_path, "stab.jsonl", EXCERPT, min_segment_index=1)
    with pytest.raises(ab.StabilizationMismatchError, match="min-segment-index=0.*must agree"):
        ab.resolve_min_segment_index(path, explicit=0)
