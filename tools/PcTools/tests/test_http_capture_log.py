"""Tests for kilnctrl.http_capture_log -- the {"t","exec","status"} HTTP
poll-capture parser used by the fuzzy-layer A/B runs.

Fixture is a 7-line excerpt of the real logs/coupling/p7_fuzzy0_http.jsonl
capture (rows spanning the profile-7 segment-0 ramp -> dwell boundary),
plus two trailing garbage lines (a non-JSON line and a status-only line
with no ``exec`` body) to exercise the skip path.
"""
from __future__ import annotations

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
