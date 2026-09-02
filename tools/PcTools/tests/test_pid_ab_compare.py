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

from kilnctrl import pid_ab_compare as ab

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
EXCERPT = os.path.join(FIXTURES, "p7_fuzzy0_http_excerpt.jsonl")


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
    rows = ab.load_last_run(EXCERPT)
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
