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
