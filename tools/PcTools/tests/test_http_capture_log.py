"""Tests for kilnctrl.http_capture_log -- the {"t","exec","status"} HTTP
poll-capture parser used by the fuzzy-layer A/B runs.

Fixture is a 7-line excerpt of the real logs/coupling/p7_fuzzy0_http.jsonl
capture (rows spanning the profile-7 segment-0 ramp -> dwell boundary),
plus two trailing garbage lines (a non-JSON line and a status-only line
with no ``exec`` body) to exercise the skip path.
"""
from __future__ import annotations

import json
import math
import os

import pytest

from kilnctrl import http_capture_log as hc
from kilnctrl import log_analysis as la

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
EXCERPT = os.path.join(FIXTURES, "p7_fuzzy0_http_excerpt.jsonl")


def test_parse_http_capture_basic():
    rows = hc.parse_http_capture_jsonl(EXCERPT)
    # 7 valid exec rows; the trailing "not json" and status-only lines are skipped.
    assert len(rows) == 7
    first = rows[0]
    assert first.t == pytest.approx(1788362353.688112)
    assert isinstance(first.poll, la.PollRow)
    assert first.poll.dwelling is False
    assert first.poll.segment_index == 0
    assert first.poll.elapsed_s == pytest.approx(582)
    assert set(first.poll.zones) == {0, 1, 2}
    assert first.status is not None
    assert "io_ready" in first.status


def test_elapsed_s_is_monotonic_across_ramp_dwell_boundary():
    # This is the whole point of using exec.elapsed_s (run-total) rather
    # than a text elapsed=Ns field: it must NOT reset at the dwell
    # transition the way coupling_pair_log.py's text capture does.
    rows = hc.poll_rows(EXCERPT)
    assert [r.dwelling for r in rows] == [False, False, False, True, True, True, True]
    elapsed = [r.elapsed_s for r in rows]
    assert elapsed == sorted(elapsed)
    assert elapsed[2] < elapsed[3]  # ramp's last sample precedes dwell's first


def test_poll_rows_feed_log_analysis_windowing_unmodified():
    # Prove the reuse contract: the parser's output plugs straight into
    # log_analysis's build_windows/window_zone_stats with no adaptation.
    rows = hc.poll_rows(EXCERPT)
    windows = la.build_windows(rows)
    assert [w.phase for w in windows] == ["ramp", "dwell"]
    stats = la.window_zone_stats(windows[0], rows, zone=0)
    assert stats is not None
    assert stats.n_samples == 3


def test_skips_non_exec_and_malformed_lines(tmp_path):
    p = tmp_path / "mixed.jsonl"
    p.write_text(
        '{"t": 100.0, "exec": {"dwelling": false, "segment_index": 0, '
        '"segment_count": 1, "target_c": 10.0, "elapsed_s": 5, '
        '"zones": [{"zone": 0, "actual_c": 9.0, "duty": 0.5}]}, "status": {}}\n'
        "\n"
        "not json\n"
        '{"t": 200.0, "status": {"io_ready": true}}\n'
        '{"t": "not-a-float", "exec": {"dwelling": false, "zones": []}}\n'
    )
    rows = hc.parse_http_capture_jsonl(str(p))
    assert len(rows) == 1
    assert rows[0].t == pytest.approx(100.0)


def test_starting_temps_c():
    rows = hc.poll_rows(EXCERPT)
    start = hc.starting_temps_c(rows)
    assert set(start) == {0, 1, 2}
    for z in (0, 1, 2):
        assert not math.isnan(start[z])


def test_starting_temps_c_empty():
    assert hc.starting_temps_c([]) == {}


# ---------------------------------------------------------------------------
# split_http_capture_lines / write_split_runs -- the real near-miss fixture.
#
# tests/fixtures/p7_oldmatrix_http_two_run_excerpt.jsonl is a trimmed excerpt
# (first 6 + last 6 lines of each side) of the ACTUAL
# logs/coupling/p7_oldmatrix_http.jsonl that produced the near-miss this
# module's split support exists to prevent: a telemetry poller left running
# across a kiln cooldown, so the file holds two complete runs (281 + 163
# rows in the real file) instead of one.
# ---------------------------------------------------------------------------

TWO_RUN_EXCERPT = os.path.join(FIXTURES, "p7_oldmatrix_http_two_run_excerpt.jsonl")
RUN_A_EXCERPT = os.path.join(FIXTURES, "p7_oldmatrix_runA_excerpt.jsonl")


def test_split_http_capture_lines_finds_two_runs_in_real_multirun_fixture():
    runs = hc.split_http_capture_lines(TWO_RUN_EXCERPT)
    assert len(runs) == 2
    assert len(runs[0]) == 12
    assert len(runs[1]) == 12
    # each split-out run must itself parse as a single, monotonic run.
    for lines in runs:
        text = "\n".join(lines) + "\n"
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False, encoding="utf-8") as fh:
            fh.write(text)
            tmp_path = fh.name
        try:
            rows = hc.poll_rows(tmp_path)
            assert len(la.split_runs(rows)) == 1
        finally:
            os.remove(tmp_path)


def test_split_http_capture_lines_single_run_file_returns_one_run():
    runs = hc.split_http_capture_lines(EXCERPT)
    assert len(runs) == 1


def test_write_split_runs_writes_one_file_per_run_and_each_is_a_clean_single_run(tmp_path):
    outdir = tmp_path / "split_out"
    paths = hc.write_split_runs(TWO_RUN_EXCERPT, str(outdir))
    assert len(paths) == 2
    assert paths[0].endswith("p7_oldmatrix_http_two_run_excerpt_run1.jsonl")
    assert paths[1].endswith("p7_oldmatrix_http_two_run_excerpt_run2.jsonl")
    for p in paths:
        assert os.path.isfile(p)
        rows = hc.poll_rows(p)
        assert len(rows) == 12
        runs = la.split_runs(rows)
        assert len(runs) == 1  # each output file is a clean single run

    # run1's rows must match the FIRST run's data exactly (start-temp check
    # -- the whole point of splitting is that A/B tooling downstream can
    # trust each file is one real firing).
    rows1 = hc.poll_rows(paths[0])
    rows2 = hc.poll_rows(paths[1])
    assert rows1[0].elapsed_s == pytest.approx(7.0)
    assert rows2[0].elapsed_s == pytest.approx(12.0)
    assert rows1[-1].elapsed_s == pytest.approx(1900.0)
    assert rows2[-1].elapsed_s == pytest.approx(1856.0)


def test_write_split_runs_run1_matches_hand_split_runA_fixture(tmp_path):
    """RUN_A_EXCERPT is the same first/last 6 lines, taken independently
    from the hand-split logs/coupling/p7_oldmatrix_runA.jsonl (the file
    extracted by an ad-hoc script during the actual incident). run1 out of
    write_split_runs must match it row for row -- proof the first-class
    split does the same job the ad-hoc script did."""
    paths = hc.write_split_runs(TWO_RUN_EXCERPT, str(tmp_path / "out"))
    rows_split = hc.poll_rows(paths[0])
    rows_handsplit = hc.poll_rows(RUN_A_EXCERPT)
    assert len(rows_split) == len(rows_handsplit)
    for a, b in zip(rows_split, rows_handsplit):
        assert a.elapsed_s == pytest.approx(b.elapsed_s)
        assert a.zones[0].actual_c == pytest.approx(b.zones[0].actual_c)


def test_write_split_runs_single_run_file_writes_one_file(tmp_path):
    outdir = tmp_path / "split_out"
    paths = hc.write_split_runs(EXCERPT, str(outdir))
    assert len(paths) == 1
    rows_in = hc.poll_rows(EXCERPT)
    rows_out = hc.poll_rows(paths[0])
    assert len(rows_in) == len(rows_out)


def test_write_split_runs_empty_file_writes_nothing(tmp_path):
    empty = tmp_path / "empty.jsonl"
    empty.write_text("")
    paths = hc.write_split_runs(str(empty), str(tmp_path / "out"))
    assert paths == []


def test_write_split_runs_missing_file_raises(tmp_path):
    with pytest.raises((FileNotFoundError, OSError)):
        hc.write_split_runs(str(tmp_path / "does_not_exist.jsonl"), str(tmp_path / "out"))


# ---------------------------------------------------------------------------
# ct_from_line / HttpPollRow.ct --
# docs/audits/cplval75_aborted_executor_panic_2026-09-09.md's fix: a capture
# that logs setpoint/temperature/duty but not raw CT current cannot tell
# "no mains" apart from an ordinary control stall. See run_queue.py's
# _ct_field() for the writer side this reads back.
# ---------------------------------------------------------------------------

def _exec_body(zone_actuals):
    return {
        "dwelling": False, "segment_index": 0, "segment_count": 1,
        "target_c": 10.0, "elapsed_s": 5,
        "zones": [{"zone": z, "actual_c": c, "duty": 1.0} for z, c in enumerate(zone_actuals)],
    }


def test_ct_from_line_reads_new_top_level_ct_key():
    obj = {"t": 1.0, "exec": _exec_body([31.0]), "status": {},
           "ct": {"counts": [16, 17, 80], "topology": "summed",
                  "fitted": [False, False, True], "current_a": [None, None, None]}}
    ct = hc.ct_from_line(obj)
    assert ct == {"counts": [16, 17, 80], "topology": "summed",
                  "fitted": [False, False, True], "current_a": [None, None, None]}


def test_ct_from_line_derives_from_status_for_older_captures_with_no_ct_key():
    # A capture made before run_queue.py grew the top-level "ct" key still
    # carries the same data inside "status" (firmware has emitted ct_counts
    # there since 2026-09-06) -- must not read back as "no data".
    obj = {"t": 1.0, "exec": _exec_body([31.0]),
           "status": {"ct_counts": [16, 17, 80], "ct_topology": "summed",
                      "ct_fitted": [False, False, True]}}
    ct = hc.ct_from_line(obj)
    assert ct["counts"] == [16, 17, 80]
    assert ct["topology"] == "summed"
    assert ct["fitted"] == [False, False, True]


def test_ct_from_line_none_when_neither_source_has_ct_fields():
    obj = {"t": 1.0, "exec": _exec_body([31.0]), "status": {"io_ready": True}}
    assert hc.ct_from_line(obj) is None
    assert hc.ct_from_line({"t": 1.0, "exec": _exec_body([31.0])}) is None


def test_parse_http_capture_jsonl_populates_row_ct(tmp_path):
    p = tmp_path / "with_ct.jsonl"
    p.write_text(json.dumps({
        "t": 1.0, "exec": _exec_body([31.0]),
        "status": {}, "ct": {"counts": [16, 17, 80], "topology": "summed",
                              "fitted": [False, False, True], "current_a": None},
    }) + "\n")
    rows = hc.parse_http_capture_jsonl(str(p))
    assert len(rows) == 1
    assert rows[0].ct["counts"] == [16, 17, 80]


def test_parse_http_capture_jsonl_ct_none_for_old_format_line(tmp_path):
    p = tmp_path / "no_ct.jsonl"
    p.write_text(json.dumps({"t": 1.0, "exec": _exec_body([31.0]), "status": {}}) + "\n")
    rows = hc.parse_http_capture_jsonl(str(p))
    assert len(rows) == 1
    assert rows[0].ct is None

