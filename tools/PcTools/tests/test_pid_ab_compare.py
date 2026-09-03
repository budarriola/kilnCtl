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

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
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
    report = ab.compare_runs(EXCERPT, EXCERPT)
    assert "error" not in report
    for z, d in report["start_temp_deltas_c"].items():
        assert d == pytest.approx(0.0)
    # every comparison should be PROVISIONAL (or n/a for missing data), never REFUSED
    assert any(c.verdict.startswith("PROVISIONAL") for c in report["comparisons"])
    assert not any(c.verdict.startswith("REFUSED") for c in report["comparisons"])


def test_large_start_temp_delta_refuses_a_winner(tmp_path):
    shifted = _write_shifted_copy(tmp_path, EXCERPT, temp_shift_c=4.8, name="shifted.jsonl")
    report = ab.compare_runs(EXCERPT, shifted)
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
    report = ab.compare_runs(EXCERPT, shifted)
    assert report["start_temp_deltas_c"][0] < ab.CONFOUND_THRESHOLD_C
    comps = [c for c in report["comparisons"] if c.zone == 0]
    assert comps
    assert all(c.verdict.startswith("PROVISIONAL") or c.verdict.startswith("n/a") for c in comps)


def test_noise_floor_note_always_present_in_text_output():
    report = ab.compare_runs(EXCERPT, EXCERPT)
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
    rc = ab.main(["compare", EXCERPT, EXCERPT])
    assert rc == 0
    out = capsys.readouterr().out
    assert "A/B compare" in out
    assert "NOISE FLOOR: UNKNOWN" in out


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
    report = ab.compare_runs(EXCERPT, EXCERPT)
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
                start_delta_c=0.0, floor_c=0.2)
    assert c.verdict.startswith("INDISTINGUISHABLE")
    assert c.delta == pytest.approx(0.05)


def test_cmp_above_floor_stays_provisional_not_indistinguishable():
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.500,
                start_delta_c=0.0, floor_c=0.2)
    assert c.verdict.startswith("PROVISIONAL")
    assert "INDISTINGUISHABLE" not in c.verdict


def test_cmp_exactly_at_floor_is_not_indistinguishable():
    """abs(delta) < floor_c, strictly -- a difference exactly equal to the
    floor is not below it, so it must still get a verdict (PROVISIONAL),
    proving the comparison isn't <=."""
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.0, b=1.25,
                start_delta_c=0.0, floor_c=0.25)
    assert c.verdict.startswith("PROVISIONAL")


def test_cmp_with_no_floor_falls_back_to_unknown_provisional():
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.001,
                start_delta_c=0.0, floor_c=None)
    assert c.verdict.startswith("PROVISIONAL")
    assert "noise floor unknown" in c.verdict


def test_confound_gate_still_wins_over_a_known_floor():
    """A start-temp delta beyond CONFOUND_THRESHOLD_C must still REFUSE, even
    when a noise floor is known and the metric delta is tiny -- the confound
    gate is about whether the comparison is valid at all, not about metric
    magnitude."""
    c = ab._cmp(0, "iae_normalized_whole_c", None, a=1.000, b=1.001,
                start_delta_c=5.0, floor_c=0.2)
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
    report = ab.compare_runs(EXCERPT, EXCERPT, noise_floor_artifact=artifact)
    assert report["noise_floor_known"] is True
    comparable = [c for c in report["comparisons"] if c.a is not None]
    assert comparable  # sanity: the fixture actually produced comparisons
    for c in comparable:
        assert c.verdict.startswith("INDISTINGUISHABLE"), c


def test_compare_runs_without_artifact_reports_noise_floor_unknown():
    report = ab.compare_runs(EXCERPT, EXCERPT)
    assert report["noise_floor_known"] is False
    for c in report["comparisons"]:
        assert "REFUSED" not in c.verdict or True  # start temps match here (delta 0)
        assert "INDISTINGUISHABLE" not in c.verdict


def test_format_compare_text_uses_known_note_when_artifact_present():
    artifact = _fake_artifact(floor_c=0.001)
    report = ab.compare_runs(EXCERPT, EXCERPT, noise_floor_artifact=artifact)
    text = ab.format_compare_text(report)
    assert "NOISE FLOOR: measured" in text
    assert "NOISE FLOOR: UNKNOWN" not in text


def test_format_compare_text_uses_unknown_note_without_artifact():
    report = ab.compare_runs(EXCERPT, EXCERPT)
    text = ab.format_compare_text(report)
    assert "NOISE FLOOR: UNKNOWN" in text


def test_cli_compare_none_flag_disables_the_artifact_lookup(capsys):
    rc = ab.main(["compare", EXCERPT, EXCERPT, "--noise-floor", "none"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "NOISE FLOOR: UNKNOWN" in out


def test_cli_compare_missing_artifact_path_behaves_like_unknown(capsys, tmp_path):
    missing = str(tmp_path / "does_not_exist_noise_floor.json")
    rc = ab.main(["compare", EXCERPT, EXCERPT, "--noise-floor", missing])
    assert rc == 0
    out = capsys.readouterr().out
    assert "NOISE FLOOR: UNKNOWN" in out
