"""Tests for kilnctrl.noise_floor -- the run-to-run noise-floor measurement
(PID_EXPANSION_PLAN.md SS3.3's iterative-tuning item) and the artifact it
produces for pid_ab_compare.py to consume.

Uses the same p7_fuzzy0_http_excerpt.jsonl fixture pid_ab_compare's own
tests use, duplicated with small per-run offsets to stand in for "N repeats
of one configuration" -- the offsets are deliberately small (well under the
4.8C confound example) so this is exercising spread measurement, not the
start-temp confound gate (that is pid_ab_compare's own test file's job).
"""
from __future__ import annotations

import json
import math
import os

import pytest

from kilnctrl import noise_floor as nf
from kilnctrl import pid_ab_compare as ab

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
EXCERPT = os.path.join(FIXTURES, "p7_fuzzy0_http_excerpt.jsonl")


def _write_offset_copy(tmp_path, src_path, error_offset_c: float, name: str) -> str:
    """Copy the fixture, nudging every zone's mean_error_c-derived signal by
    changing actual_c only (target_c held fixed) -- a synthetic 'repeat run'
    with a small, controlled per-run offset standing in for real run-to-run
    noise."""
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
                z["actual_c"] = z["actual_c"] + error_offset_c
            lines.append(json.dumps(obj))
    out.write_text("\n".join(lines) + "\n")
    return str(out)


@pytest.fixture
def repeat_paths(tmp_path):
    # Three "repeats" with small, distinct offsets so mean/std/range are all
    # non-degenerate (not every value identical).
    return [
        _write_offset_copy(tmp_path, EXCERPT, 0.0, "rep1.jsonl"),
        _write_offset_copy(tmp_path, EXCERPT, 0.2, "rep2.jsonl"),
        _write_offset_copy(tmp_path, EXCERPT, -0.3, "rep3.jsonl"),
    ]


# ---------------------------------------------------------------------------
# compute_repeat_spread
# ---------------------------------------------------------------------------

def test_requires_at_least_two_paths():
    with pytest.raises(ValueError):
        nf.compute_repeat_spread([EXCERPT])


def test_spread_covers_every_zone(repeat_paths):
    stats = nf.compute_repeat_spread(repeat_paths)
    zones = {s.zone for s in stats}
    assert zones == {0, 1, 2}


def test_spread_n_matches_repeat_count(repeat_paths):
    stats = nf.compute_repeat_spread(repeat_paths)
    whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
    assert whole.n == 3


def test_spread_range_is_nonzero_for_offset_repeats(repeat_paths):
    stats = nf.compute_repeat_spread(repeat_paths)
    whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
    # The three copies have different actual_c offsets -> different IAE ->
    # a genuinely non-zero range, not a degenerate 0.0.
    assert whole.range_c is not None
    assert whole.range_c > 0.0


def test_spread_range_equals_max_minus_min(repeat_paths):
    stats = nf.compute_repeat_spread(repeat_paths)
    whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
    assert whole.range_c == pytest.approx(max(whole.values) - min(whole.values))


def test_identical_repeats_give_zero_range():
    stats = nf.compute_repeat_spread([EXCERPT, EXCERPT, EXCERPT])
    whole = [s for s in stats if s.metric == "iae_normalized_whole_c" and s.zone == 0][0]
    assert whole.range_c == pytest.approx(0.0)
    assert whole.std_c == pytest.approx(0.0)


def test_single_value_key_has_no_floor(repeat_paths):
    # Every (zone, metric, segment) that appears in all repeat_paths gets
    # n=3; there is no way to construct a key with n=1 from these fixtures,
    # so this pins the *shape* of the "not enough data" contract directly
    # rather than depend on one fixture happening to omit a key.
    stats = nf.compute_repeat_spread(repeat_paths)
    for s in stats:
        if s.n < nf.MIN_REPEATS_FOR_FLOOR:
            assert s.range_c is None
            assert s.std_c is None


# ---------------------------------------------------------------------------
# build_artifact / load_artifact / floor_lookup
# ---------------------------------------------------------------------------

def test_build_artifact_schema(repeat_paths):
    artifact = nf.build_artifact(repeat_paths)
    assert artifact["schema_version"] == nf.SCHEMA_VERSION
    assert artifact["n_repeats"] == 3
    assert artifact["generated_from"] == repeat_paths
    assert isinstance(artifact["entries"], dict)
    assert len(artifact["entries"]) > 0


def test_build_artifact_entry_shape(repeat_paths):
    artifact = nf.build_artifact(repeat_paths)
    key = nf._key_str(0, "iae_normalized_whole_c", None)
    entry = artifact["entries"][key]
    assert entry["zone"] == 0
    assert entry["metric"] == "iae_normalized_whole_c"
    assert entry["segment"] is None
    assert entry["n"] == 3
    assert entry["noise_floor_c"] >= 0.0


def test_floor_lookup_returns_none_for_missing_artifact():
    assert nf.floor_lookup(None, 0, "iae_normalized_whole_c", None) is None


def test_floor_lookup_returns_none_for_unknown_key(repeat_paths):
    artifact = nf.build_artifact(repeat_paths)
    assert nf.floor_lookup(artifact, 99, "no_such_metric", None) is None


def test_floor_lookup_round_trips_through_json(tmp_path, repeat_paths):
    artifact = nf.build_artifact(repeat_paths)
    out_path = tmp_path / "noise_floor.json"
    out_path.write_text(json.dumps(artifact))
    loaded = nf.load_artifact(str(out_path))
    key0 = nf._key_str(0, "iae_normalized_whole_c", None)
    assert nf.floor_lookup(loaded, 0, "iae_normalized_whole_c", None) == pytest.approx(
        artifact["entries"][key0]["noise_floor_c"])


def test_load_artifact_missing_file_returns_none():
    assert nf.load_artifact("does/not/exist/noise_floor.json") is None


def test_load_artifact_malformed_json_returns_none(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text("{not json")
    assert nf.load_artifact(str(p)) is None


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def test_cli_build_writes_file(tmp_path, repeat_paths):
    out_path = tmp_path / "out.json"
    rc = nf.main(["build", *repeat_paths, "--out", str(out_path)])
    assert rc == 0
    assert out_path.exists()
    data = json.loads(out_path.read_text())
    assert data["n_repeats"] == 3


def test_cli_report_runs(repeat_paths, capsys):
    rc = nf.main(["report", *repeat_paths])
    assert rc == 0
    out = capsys.readouterr().out
    assert "z0" in out


def test_cli_report_rejects_single_path(capsys):
    rc = nf.main(["report", EXCERPT])
    assert rc == 1
    out = capsys.readouterr().out
    assert "error" in out
