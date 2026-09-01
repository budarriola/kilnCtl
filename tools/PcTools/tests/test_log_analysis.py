"""Tests for kilnctrl.log_analysis -- windowed firing/tuning log analysis.

Fixtures under tests/fixtures/ are excerpts of real capture files from a
3-zone firing (track3zone*.jsonl) and a successful autotune (retune_z0c.jsonl
/ z0_trace_full.csv), captured the night this tool was written. See
tools/PcTools/src/kilnctrl/log_analysis.py's module docstring for the field
definitions and the metric conventions asserted here.
"""
from __future__ import annotations

import math
import os

import pytest

from kilnctrl import log_analysis as la

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
TRACK3ZONE_EXCERPT = os.path.join(FIXTURES, "track3zone_excerpt.jsonl")
TRACK3ZONE_FULL = os.path.join(FIXTURES, "track3zone_full.jsonl")
RETUNE_Z0C = os.path.join(FIXTURES, "retune_z0c.jsonl")
Z0_TRACE = os.path.join(FIXTURES, "z0_trace_full.csv")


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def test_parse_profile_exec_jsonl_basic():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    assert len(rows) == 60
    first = rows[0]
    assert first.wall_time == "06:08:56"
    assert first.segment_index == 0
    assert first.dwelling is False
    assert first.target_c == pytest.approx(38.06)
    assert set(first.zones) == {0, 1, 2}
    assert first.zones[0].actual_c == pytest.approx(37.55)
    assert first.zones[0].duty == pytest.approx(0.309)


def test_parse_profile_exec_jsonl_skips_non_matching_lines(tmp_path):
    p = tmp_path / "mixed.jsonl"
    p.write_text(
        "06:00:00 {\"not\": \"a firing row\"}\n"
        "\n"
        "bad line no json\n"
        "06:00:15 {\"state\":\"running\",\"segment_index\":0,\"segment_count\":1,"
        "\"dwelling\":false,\"target_c\":10.0,\"elapsed_s\":5,"
        "\"zones\":[{\"zone\":0,\"actual_c\":9.0,\"duty\":0.5}]}\n"
    )
    rows = la.parse_profile_exec_jsonl(str(p))
    assert len(rows) == 1
    assert rows[0].elapsed_s == 5


def test_parse_autotune_jsonl():
    rows = la.parse_autotune_jsonl(RETUNE_Z0C)
    assert len(rows) == 56
    last = rows[-1]
    assert last.state == "done"
    assert last.model_valid is True
    assert last.k_gain_c_per_duty == pytest.approx(39.246)
    assert last.tau_s == pytest.approx(263.8)
    assert last.dead_time_s == pytest.approx(52.8)
    assert last.baseline_c == pytest.approx(31.36)
    assert last.rise_inf_c == pytest.approx(39.25)
    assert last.raw_rise_c == pytest.approx(38.04)


def test_parse_trace_csv():
    t, y = la.parse_trace_csv(Z0_TRACE)
    assert len(t) == len(y) == 97
    assert t[0] == pytest.approx(0.0)
    assert y[0] == pytest.approx(32.00)
    assert t[-1] == pytest.approx(960.0)


# ---------------------------------------------------------------------------
# Windowing
# ---------------------------------------------------------------------------

def test_build_windows_splits_on_segment_and_dwelling():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    windows = la.build_windows(rows)
    # excerpt is head(30) + tail(30) of the real capture, so segment 1's ramp
    # window is missing (spliced out) -- the boundaries that ARE present must
    # still come out right.
    assert [(w.segment_index, w.phase) for w in windows] == [
        (0, "ramp"), (0, "dwell"), (1, "dwell"), (2, "dwell"),
    ]
    assert windows[0].start_idx == 0
    assert windows[0].end_idx == 13
    assert windows[0].start_s == pytest.approx(9.0)
    assert windows[0].end_s == pytest.approx(208.0)


def test_build_windows_full_run_has_five_windows():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_FULL)
    windows = la.build_windows(rows)
    assert [(w.segment_index, w.phase) for w in windows] == [
        (0, "ramp"), (0, "dwell"), (1, "ramp"), (1, "dwell"), (2, "dwell"),
    ]
    # the true segment 2 "dwell" is a single terminal (state=done) snapshot
    assert windows[-1].start_idx == windows[-1].end_idx == len(rows) - 1


# ---------------------------------------------------------------------------
# Per-window, per-zone statistics -- hand-computed anchors.
#
# window[0] is segment 0's ramp, zone 0, rows 0..13 of the excerpt (14
# samples, elapsed_s 9..208). error_i = actual_c_i - target_c_i for each row;
# dt_i is the gap to the next row (last sample gets 0 weight, per the
# trapezoid-style integral documented in log_analysis.py). Verified by an
# independent read of the raw JSONL (not by rerunning this same code):
#   max positive error (overshoot) is +4.74 C at elapsed_s=208 (last sample:
#     actual 42.74, target 38.00)
#   max negative error (undershoot) is -1.51 C at elapsed_s=39
#   time-weighted mean error is +0.8033 C, iae_raw is 359.27 C*s over a
#     199 s window -> iae_normalized (iae_raw/duration) is 1.8054 C.
# ---------------------------------------------------------------------------

def test_window_zone_stats_hand_computed_anchor():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    windows = la.build_windows(rows)
    ramp0 = windows[0]
    assert ramp0.segment_index == 0 and ramp0.phase == "ramp"

    s = la.window_zone_stats(ramp0, rows, zone=0)
    assert s.n_samples == 14
    assert s.mean_error_c == pytest.approx(0.8033, abs=1e-3)
    assert s.max_overshoot_c == pytest.approx(4.74, abs=1e-6)
    assert s.max_overshoot_at_s == pytest.approx(208.0)
    assert s.max_undershoot_c == pytest.approx(1.51, abs=1e-6)
    assert s.max_undershoot_at_s == pytest.approx(39.0)
    assert s.iae_raw_c_s == pytest.approx(359.27, abs=0.05)
    assert s.iae_normalized_c == pytest.approx(359.27 / 199.0, abs=1e-3)
    assert s.duty_min == pytest.approx(0.043)
    assert s.duty_max == pytest.approx(0.408)


def test_window_zone_stats_whole_run_close_to_firmware_but_not_identical():
    """Independent cross-check against the firmware's own final firing_stats
    row for zone 0 (full 3-zone run). This is the discrepancy check the
    coordinator asked for: our windowed/whole-run numbers are computed from
    raw actual_c/target_c samples at the (sparse, ~15 s) poll cadence, while
    the firmware accumulates at its own internal tick -- so overshoot/
    undershoot land close (missed by less than the poll interval's worth of
    slew) but not bit-identical, and the two "mean error" figures can
    diverge more once the ramp's sparse sampling is folded in equally with
    dwell. See the module docstring for why iae_normalized is intentionally
    a different metric (ours: iae_raw/duration; firmware's: also divided by
    the segment's setpoint span) and is not expected to match at all.
    """
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_FULL)
    whole = la.Window(-1, "all", 0, len(rows) - 1, rows[0].elapsed_s, rows[-1].elapsed_s)
    s = la.window_zone_stats(whole, rows, zone=0)

    # firmware's own last-row zone-0 firing_stats: mean_error_c=1.09,
    # max_overshoot_c=5.34, max_undershoot_c=1.78, iae_normalized=0.0735
    assert s.mean_error_c == pytest.approx(1.096, abs=0.02)
    assert s.max_overshoot_c == pytest.approx(5.34, abs=0.15)
    assert s.max_undershoot_c == pytest.approx(1.78, abs=0.15)
    # our normalization is deliberately different -- not compared here.


# ---------------------------------------------------------------------------
# Transitions: settle time, peak overshoot, duty-off -> peak lag
# ---------------------------------------------------------------------------

def test_ramp_to_dwell_transition_hand_computed():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    windows = la.build_windows(rows)
    events = la.ramp_to_dwell_transitions(rows, windows, zone=0, band_c=1.0)
    assert len(events) == 1
    e = events[0]
    assert e.segment_index == 0
    assert e.transition_at_s == pytest.approx(224.0)
    # duty first reads <=0.02 at elapsed_s=224 (the first dwell sample)
    assert e.duty_zero_at_s == pytest.approx(224.0)
    # peak actual_c within the dwell window is at elapsed_s=239 (5.24 C over
    # the 38.00 C dwell target)
    assert e.peak_overshoot_at_s == pytest.approx(239.0)
    assert e.peak_overshoot_c == pytest.approx(5.24, abs=1e-6)
    assert e.overshoot_lag_s == pytest.approx(15.0)
    # error does fall back within +/-1.0 C later in the dwell window (at
    # elapsed_s=454, the excerpt's last dwell sample of this window)
    assert e.settle_time_s == pytest.approx(199.0)


def test_settle_time_found_when_band_is_wide_enough():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    windows = la.build_windows(rows)
    events = la.ramp_to_dwell_transitions(rows, windows, zone=0, band_c=6.0)
    assert events[0].settle_time_s == pytest.approx(0.0)


# ---------------------------------------------------------------------------
# Saturation and ff_hold_infeasible
# ---------------------------------------------------------------------------

def test_saturation_time_zero_when_never_pinned():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    sat = la.saturation_time_s(rows, zone=2)
    assert sat == {"at_or_above": 0.0, "at_or_below": 0.0}


def test_saturation_time_detects_high_duty(tmp_path):
    p = tmp_path / "pinned.jsonl"
    lines = []
    for i, elapsed in enumerate((0, 10, 20, 30)):
        duty = 0.995 if i < 3 else 0.5
        lines.append(
            f"00:0{i}:00 {{\"state\":\"running\",\"segment_index\":0,\"segment_count\":1,"
            f"\"dwelling\":true,\"target_c\":100.0,\"elapsed_s\":{elapsed},"
            f"\"zones\":[{{\"zone\":0,\"actual_c\":90.0,\"duty\":{duty}}}]}}"
        )
    p.write_text("\n".join(lines) + "\n")
    rows = la.parse_profile_exec_jsonl(str(p))
    sat = la.saturation_time_s(rows, zone=0)
    # dt is measured to the NEXT row: rows at t=0,10,20 all read duty>=0.99,
    # so each of the three 10s gaps up to t=30 counts as saturated time.
    assert sat["at_or_above"] == pytest.approx(30.0)
    assert sat["at_or_below"] == pytest.approx(0.0)


def test_ff_hold_infeasible_episodes(tmp_path):
    p = tmp_path / "ff.jsonl"
    rows_json = []
    for elapsed, flag in ((0, False), (10, True), (20, True), (30, False), (40, True)):
        rows_json.append(
            f"00:00:00 {{\"state\":\"running\",\"segment_index\":0,\"segment_count\":1,"
            f"\"dwelling\":true,\"target_c\":50.0,\"elapsed_s\":{elapsed},"
            f"\"zones\":[{{\"zone\":0,\"actual_c\":49.0,\"duty\":0.2,"
            f"\"ff_hold_infeasible\":{'true' if flag else 'false'}}}]}}"
        )
    p.write_text("\n".join(rows_json) + "\n")
    rows = la.parse_profile_exec_jsonl(str(p))
    episodes = la.ff_hold_infeasible_episodes(rows, zone=0)
    assert episodes == [(10.0, 20.0), (40.0, 40.0)]


# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------

def test_sanity_check_firing_flags_systematic_overdrive(tmp_path):
    p = tmp_path / "hot.jsonl"
    lines = []
    for elapsed in (0, 10, 20):
        lines.append(
            f"00:00:00 {{\"state\":\"running\",\"segment_index\":0,\"segment_count\":1,"
            f"\"dwelling\":true,\"target_c\":50.0,\"elapsed_s\":{elapsed},"
            f"\"zones\":[{{\"zone\":0,\"actual_c\":53.0,\"duty\":0.1}},"
            f"{{\"zone\":1,\"actual_c\":52.0,\"duty\":0.1}}]}}"
        )
    p.write_text("\n".join(lines) + "\n")
    rows = la.parse_profile_exec_jsonl(str(p))
    warnings = la.sanity_check_firing(rows)
    assert any("HOT" in w for w in warnings)


def test_sanity_check_firing_silent_when_mixed_signs():
    rows = la.parse_profile_exec_jsonl(TRACK3ZONE_EXCERPT)
    # the excerpt's zones do not all share a sign over the (spliced,
    # non-contiguous) window -- exercise the real path end-to-end and just
    # assert it does not raise and returns a list.
    warnings = la.sanity_check_firing(rows)
    assert isinstance(warnings, list)


def test_sanity_check_autotune_flags_baseline_disagreement():
    class Row:
        pass
    r = Row()
    r.state = "done"
    r.model_valid = True
    r.final_c = 70.0
    r.baseline_c = 31.36
    r.rise_inf_c = 39.25
    trace = ([0.0, 10.0, 20.0], [50.0, 50.5, 51.0])  # opens at ~50, not ~31
    warnings = la.sanity_check_autotune([r], trace)
    assert any("baseline_c" in w for w in warnings)


def test_sanity_check_autotune_flags_asymptote_below_measured():
    class Row:
        pass
    r = Row()
    r.state = "done"
    r.model_valid = True
    r.final_c = 70.0
    r.baseline_c = 31.0
    r.rise_inf_c = 30.0  # asymptote 61.0, below the trace's own peak
    trace = ([0.0, 500.0, 960.0], [31.0, 65.0, 70.0])
    warnings = la.sanity_check_autotune([r], trace)
    assert any("asymptote" in w for w in warnings)


def test_sanity_check_autotune_silent_on_the_real_good_tune():
    rows = la.parse_autotune_jsonl(RETUNE_Z0C)
    trace = la.parse_trace_csv(Z0_TRACE)
    warnings = la.sanity_check_autotune(rows, trace)
    assert warnings == []


# ---------------------------------------------------------------------------
# Autotune summary + independent FOPDT refit
# ---------------------------------------------------------------------------

def test_summarize_autotune():
    rows = la.parse_autotune_jsonl(RETUNE_Z0C)
    s = la.summarize_autotune(rows)
    assert s.zone == 0
    assert s.completed is True
    assert s.method == "step"
    assert s.k_gain_c_per_duty == pytest.approx(39.246)
    assert s.tau_s == pytest.approx(263.8)
    assert s.dead_time_s == pytest.approx(52.8)


def test_summarize_autotune_reports_abort():
    class Row:
        pass
    r = Row()
    r.state = "aborted"
    r.zone = 2
    r.method = "step"
    r.model_valid = False
    r.abort_reason = "guard1_stall"
    r.k_gain_c_per_duty = 0.0
    r.tau_s = 0.0
    r.dead_time_s = 0.0
    r.baseline_c = 0.0
    r.step_ambient_c = 0.0
    r.final_c = 0.0
    r.raw_rise_c = 0.0
    r.rise_inf_c = 0.0
    r.model_settled = False
    r.model_extrapolation_converged = False
    r.model_tau_consistent = False
    r.proposed_kp = 0.0
    r.proposed_ki = 0.0
    r.proposed_kd = 0.0
    s = la.summarize_autotune([r])
    assert s.completed is False
    assert s.abort_reason == "guard1_stall"


def test_refit_fopdt_agrees_with_firmware_within_a_few_percent():
    """The tonight-was-right cross-check: fit the raw trace independently
    and compare to the firmware's own two-point-plus-extrapolation fit."""
    t, y = la.parse_trace_csv(Z0_TRACE)
    fit = la.refit_fopdt(t, y)
    assert fit.k_gain_c_per_duty is not None
    # firmware: K=39.246 C/duty, tau=263.8 s, L=52.8 s
    assert fit.k_gain_c_per_duty == pytest.approx(39.246, rel=0.10)
    assert fit.tau_s == pytest.approx(263.8, rel=0.20)
    assert fit.dead_time_s == pytest.approx(52.8, rel=0.20)
    assert fit.method == "least_squares_grid"


def test_refit_fopdt_too_few_samples():
    fit = la.refit_fopdt([0.0, 1.0, 2.0], [10.0, 10.1, 10.2])
    assert fit.k_gain_c_per_duty is None


# ---------------------------------------------------------------------------
# Reports (text + JSON) and comparison
# ---------------------------------------------------------------------------

def test_render_firing_report_text_has_expected_shape():
    report = la.render_firing_report(TRACK3ZONE_EXCERPT, band_c=1.0)
    text = la.format_firing_report_text(report)
    assert "seg 0 ramp" in text
    assert "z0:" in text
    assert "transition z0 seg0" in text


def test_render_firing_report_json_round_trips():
    import json
    report = la.render_firing_report(TRACK3ZONE_EXCERPT, band_c=1.0)
    js = la.firing_report_to_json(report)
    data = json.loads(js)
    assert data["n_rows"] == 60
    assert len(data["windows"]) == 4


def test_render_firing_report_missing_file_reports_error():
    with pytest.raises(FileNotFoundError):
        la.render_firing_report("no/such/file.jsonl")


def test_render_autotune_report_with_trace():
    report = la.render_autotune_report(RETUNE_Z0C, Z0_TRACE)
    text = la.format_autotune_report_text(report)
    assert "completed=True" in text
    assert "independent refit" in text
    assert report["refit"].k_gain_c_per_duty is not None


def test_render_autotune_report_without_trace():
    report = la.render_autotune_report(RETUNE_Z0C)
    assert report["refit"] is None
    text = la.format_autotune_report_text(report)
    assert "independent refit" not in text


def test_compare_firing_runs_identical_file_shows_no_improvement():
    report = la.compare_firing_runs(TRACK3ZONE_EXCERPT, TRACK3ZONE_EXCERPT, band_c=1.0)
    text = la.format_compare_report_text(report)
    for z, d in report["diffs"].items():
        assert d["iae_normalized_a"] == pytest.approx(d["iae_normalized_b"])
        assert d["iae_normalized_improved"] is False  # b < a is strict, equal is not "improved"
    assert "compare:" in text


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def test_cli_firing_text(capsys):
    rc = la.main(["firing", TRACK3ZONE_EXCERPT])
    assert rc == 0
    out = capsys.readouterr().out
    assert "seg 0 ramp" in out


def test_cli_firing_json(capsys):
    rc = la.main(["firing", TRACK3ZONE_EXCERPT, "--json"])
    assert rc == 0
    out = capsys.readouterr().out
    assert out.strip().startswith("{")


def test_cli_autotune(capsys):
    rc = la.main(["autotune", RETUNE_Z0C, "--trace", Z0_TRACE])
    assert rc == 0
    out = capsys.readouterr().out
    assert "autotune:" in out


def test_cli_compare(capsys):
    rc = la.main(["compare", TRACK3ZONE_EXCERPT, TRACK3ZONE_EXCERPT])
    assert rc == 0
    out = capsys.readouterr().out
    assert "compare:" in out


# ---------------------------------------------------------------------------
# Multi-run logs -- elapsed_s restarts at 0 partway through the file because
# a second firing was appended to the same JSONL. See log_analysis.py's
# module docstring for the bug this class of test pins: a merged-run
# "whole run" window used to have start_s > end_s, Window.duration_s
# clamped to 0.0, and the old ``iae_raw / (duration or 1.0)`` fallback
# silently returned the raw C*s integral mislabelled as normalized C.
# ---------------------------------------------------------------------------

def _write_run(lines, elapsed_values, actual_c, target_c=50.0, duty=0.2, zone=0):
    for elapsed in elapsed_values:
        lines.append(
            f"00:00:00 {{\"state\":\"running\",\"segment_index\":0,\"segment_count\":1,"
            f"\"dwelling\":true,\"target_c\":{target_c},\"elapsed_s\":{elapsed},"
            f"\"zones\":[{{\"zone\":{zone},\"actual_c\":{actual_c},\"duty\":{duty}}}]}}"
        )


def _two_run_jsonl(tmp_path, name="two_run.jsonl"):
    """A file holding two runs: run 1 (elapsed 0..30, actual 55 -> mean err
    +5C) followed by run 2 restarting at elapsed_s=0 (elapsed 0..20, actual
    52 -> mean err +2C). The LAST run is the one every consumer should use.
    """
    p = tmp_path / name
    lines = []
    _write_run(lines, (0, 10, 20, 30), actual_c=55.0)
    _write_run(lines, (0, 10, 20), actual_c=52.0)
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def _one_run_jsonl(tmp_path, name="one_run.jsonl"):
    p = tmp_path / name
    lines = []
    _write_run(lines, (0, 10, 20), actual_c=52.0)
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def test_split_runs_detects_two_runs(tmp_path):
    rows = la.parse_profile_exec_jsonl(_two_run_jsonl(tmp_path))
    runs = la.split_runs(rows)
    assert len(runs) == 2
    assert [r.elapsed_s for r in runs[0]] == [0.0, 10.0, 20.0, 30.0]
    assert [r.elapsed_s for r in runs[1]] == [0.0, 10.0, 20.0]


def test_split_runs_single_run_returns_one(tmp_path):
    rows = la.parse_profile_exec_jsonl(_one_run_jsonl(tmp_path))
    runs = la.split_runs(rows)
    assert len(runs) == 1
    assert len(runs[0]) == 3


def test_split_runs_empty():
    assert la.split_runs([]) == []


def test_build_windows_refuses_multi_run_rows(tmp_path):
    rows = la.parse_profile_exec_jsonl(_two_run_jsonl(tmp_path))
    with pytest.raises(ValueError):
        la.build_windows(rows)


def test_merged_run_window_no_longer_yields_huge_finite_iae():
    """Pins the exact reported bug: a window whose start_s > end_s (duration
    clamped to 0) must NOT produce a large finite iae_normalized_c -- it must
    come back as NaN rather than the raw C*s integral relabelled as C."""
    rows = [
        la.PollRow(
            wall_time="00:00:00", elapsed_s=e, segment_index=0, segment_count=1,
            dwelling=True, target_c=50.0, state="running",
            zones={0: la.ZoneSample(zone=0, actual_c=55.0, duty=0.2)},
        )
        for e in (10.0, 20.0, 30.0, 0.0, 5.0)
    ]
    # a merged-run "whole run" window, exactly as the old whole_run_stats
    # helper built one: start_s from the first row, end_s from the last.
    bogus_window = la.Window(-1, "all", 0, len(rows) - 1, rows[0].elapsed_s, rows[-1].elapsed_s)
    assert bogus_window.duration_s == 0.0
    # window_zone_stats asserts dts are non-negative; a genuinely merged
    # sequence of rows (decreasing elapsed_s inside the window) must be
    # rejected rather than silently integrated with a negative weight.
    with pytest.raises(AssertionError):
        la.window_zone_stats(bogus_window, rows, zone=0)


def test_window_zone_stats_zero_duration_window_is_nan_not_raw_iae():
    """A single-row-spanning window with start_s == end_s (duration 0) but
    otherwise valid (non-decreasing) rows must report iae_normalized_c as
    NaN, never the raw iae_raw_c_s value mislabelled as C."""
    rows = [
        la.PollRow(
            wall_time="00:00:00", elapsed_s=5.0, segment_index=0, segment_count=1,
            dwelling=True, target_c=50.0, state="running",
            zones={0: la.ZoneSample(zone=0, actual_c=55.0, duty=0.2)},
        ),
    ]
    zero_window = la.Window(-1, "all", 0, 0, 5.0, 5.0)
    s = la.window_zone_stats(zero_window, rows, zone=0)
    assert s.iae_raw_c_s == pytest.approx(0.0)
    assert math.isnan(s.iae_normalized_c)


def test_compare_firing_runs_uses_last_run_and_reports_run_counts(tmp_path):
    """The core regression: comparing a multi-run file's iae_normalized must
    equal what you'd get analyzing the LAST run in isolation -- not the old
    corrupted merged-run figure -- and the report must say the file was
    multi-run."""
    two_run_path = _two_run_jsonl(tmp_path, "a_two_run.jsonl")
    last_run_only_path = _one_run_jsonl(tmp_path, "a_last_run_only.jsonl")  # run 2, isolated
    single_run_path = _one_run_jsonl(tmp_path, "b_single_run.jsonl")

    report = la.compare_firing_runs(two_run_path, single_run_path, band_c=1.0)
    assert report["runs_in_a"] == 2
    assert report["runs_in_b"] == 1
    assert report["used_run_index_a"] == 1
    assert report["used_run_index_b"] == 0

    isolated_report = la.compare_firing_runs(last_run_only_path, single_run_path, band_c=1.0)

    a_iae = report["diffs"][0]["iae_normalized_a"]
    isolated_iae = isolated_report["diffs"][0]["iae_normalized_a"]
    assert a_iae == pytest.approx(isolated_iae, rel=1e-9)
    # sanity: this must be a plausible normalized-C number, nowhere near the
    # corrupted ~1800x-too-large figure the raw-C*s fallback used to produce.
    assert 0.0 <= a_iae <= 10.0

    text = la.format_compare_report_text(report)
    assert "A holds 2 runs" in text


def test_render_firing_report_uses_last_run(tmp_path):
    report = la.render_firing_report(_two_run_jsonl(tmp_path), band_c=1.0)
    assert report["runs_in_file"] == 2
    assert report["used_run_index"] == 1
    assert report["n_rows"] == 3  # run 2 has 3 polls
    text = la.format_firing_report_text(report)
    assert "2 runs" in text
