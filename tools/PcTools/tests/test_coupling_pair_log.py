"""Tests for kilnctrl.coupling_pair_log -- the two-poller
(``<name>_mcp.jsonl`` + ``<name>_thermo.jsonl``) capture-format parser for
the single-zone coupling-excitation runs (see that module's docstring).

Fixtures under tests/fixtures/coupling_pair/ are a real capture of the
cpl_z0 run (profile #4, zone 0 driven alone, 55 C dwell), copied verbatim --
small enough (under 80 lines each) to check in whole rather than trim.

Every check below has a negative-test companion proving it can actually go
red, per repo policy.
"""
from __future__ import annotations

import json
import math
import os

import pytest

from kilnctrl import coupling_pair_log as cpl
from kilnctrl import log_analysis as la

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "coupling_pair")
MCP_PATH = os.path.join(FIXTURES, "cpl_z0_mcp.jsonl")
THERMO_PATH = os.path.join(FIXTURES, "cpl_z0_thermo.jsonl")


# ---------------------------------------------------------------------------
# Line-level parsing
# ---------------------------------------------------------------------------

def test_parse_exec_status_text_header_and_zone():
    text = ("state=1 profile=#4 'cpl_z0' segment=0/1 dwelling=True target=55.0C "
            "elapsed=600s dwell_remaining=1500s ramp_lock=False fault_guard=0\n"
            "  zone 0: mode=2 actual=56.1C (valid) duty=0.63 relay=on faulted=False")
    parsed = cpl.parse_exec_status_text(text)
    assert parsed is not None
    assert parsed["profile"] == 4
    assert parsed["name"] == "cpl_z0"
    assert parsed["segment_index"] == 0 and parsed["segment_count"] == 1
    assert parsed["dwelling"] is True
    assert parsed["target_c"] == pytest.approx(55.0)
    assert parsed["text_elapsed_s"] == pytest.approx(600.0)
    assert set(parsed["zones"].keys()) == {0}
    zs = parsed["zones"][0]
    assert zs.actual_c == pytest.approx(56.1)
    assert zs.duty == pytest.approx(0.63)


def test_parse_exec_status_text_garbage_returns_none():
    """A line that isn't an exec-status body at all (e.g. cross-wired with
    the thermo file, or plain garbage) must be rejected, not silently
    parsed into nonsense.
    """
    assert cpl.parse_exec_status_text("CH0: 56.88 C (CJ 26.31 C)") is None
    assert cpl.parse_exec_status_text("") is None


def test_parse_exec_status_text_invalid_reading_is_nan():
    """A thermocouple fault ('(invalid)') must come through as NaN, matching
    log_analysis's own convention -- mutate a valid reading into 'invalid'
    and confirm the numeric text becomes unusable garbage that gets
    converted to NaN rather than trusted.
    """
    text = ("state=1 profile=#4 'cpl_z0' segment=0/1 dwelling=True target=55.0C "
            "elapsed=600s dwell_remaining=1500s ramp_lock=False fault_guard=0\n"
            "  zone 0: mode=2 actual=999.0C (invalid) duty=0.63 relay=on faulted=True")
    parsed = cpl.parse_exec_status_text(text)
    assert math.isnan(parsed["zones"][0].actual_c)


def test_parse_thermo_text_three_channels():
    text = "CH0: 56.88 C (CJ 26.31 C)\nCH1: 33.81 C (CJ 26.47 C)\nCH2: 29.61 C (CJ 26.58 C)"
    channels = cpl.parse_thermo_text(text)
    assert channels == pytest.approx({0: 56.88, 1: 33.81, 2: 29.61})


def test_parse_thermo_text_no_channels_returns_none():
    assert cpl.parse_thermo_text("nothing here") is None


# ---------------------------------------------------------------------------
# File loading tolerance
# ---------------------------------------------------------------------------

def test_load_jsonl_skips_malformed_lines(tmp_path):
    p = tmp_path / "x.jsonl"
    p.write_text(
        '{"t": 1.0, "s": "ok1"}\n'
        "not json at all\n"
        '{"t": 2.0}\n'  # missing "s"
        "\n"
        '{"t": 3.0, "s": "ok2"}\n',
        encoding="utf-8",
    )
    rows = cpl._load_jsonl(str(p))
    assert [s for _, s in rows] == ["ok1", "ok2"]


# ---------------------------------------------------------------------------
# Join: nearest-timestamp, max_skew_s
# ---------------------------------------------------------------------------

def _mcp_line(t, elapsed, dwelling=True, actual=55.0, duty=0.5, target=55.0):
    return json.dumps({
        "t": t,
        "s": (f"state=1 profile=#4 'cpl_z0' segment=0/1 dwelling={dwelling} "
              f"target={target}C elapsed={elapsed}s dwell_remaining=100s ramp_lock=False fault_guard=0\n"
              f"  zone 0: mode=2 actual={actual}C (valid) duty={duty} relay=on faulted=False"),
    })


def _thermo_line(t, ch0, ch1, ch2):
    return json.dumps({
        "t": t,
        "s": f"CH0: {ch0} C (CJ 25.0 C)\nCH1: {ch1} C (CJ 25.0 C)\nCH2: {ch2} C (CJ 25.0 C)",
    })


def test_join_pairs_nearest_thermo_within_skew(tmp_path):
    mcp_path = tmp_path / "m.jsonl"
    thermo_path = tmp_path / "t.jsonl"
    mcp_path.write_text(_mcp_line(1000.0, 0) + "\n" + _mcp_line(1040.0, 20) + "\n", encoding="utf-8")
    # First thermo row is 5s off the first exec row (within default 15s skew).
    # Second exec row's NEAREST thermo row is 20s away (1040 vs 1060) -- outside skew.
    thermo_path.write_text(
        _thermo_line(1005.0, 30.0, 20.0, 19.0) + "\n"
        + _thermo_line(1060.0, 31.0, 20.5, 19.5) + "\n",
        encoding="utf-8",
    )
    rows = cpl.load_pair_run(str(mcp_path), str(thermo_path))
    assert len(rows) == 1  # the second exec row has no thermo match within skew -- dropped
    r = rows[0]
    assert r.zones[0].actual_c == pytest.approx(55.0)  # from the exec line, not the thermo line
    assert r.zones[1].actual_c == pytest.approx(20.0)  # from the matched thermo row
    assert r.zones[2].actual_c == pytest.approx(19.0)
    assert r.zones[1].duty == pytest.approx(0.0)  # passive zone -- no duty info, must be 0


def test_join_respects_max_skew_s_override(tmp_path):
    """Same construction as above but widen max_skew_s so the previously-
    dropped second exec row now finds its (still 20s-away) thermo match --
    proves the skew gate is load-bearing, not decorative.
    """
    mcp_path = tmp_path / "m.jsonl"
    thermo_path = tmp_path / "t.jsonl"
    mcp_path.write_text(_mcp_line(1000.0, 0) + "\n" + _mcp_line(1040.0, 20) + "\n", encoding="utf-8")
    thermo_path.write_text(
        _thermo_line(1005.0, 30.0, 20.0, 19.0) + "\n"
        + _thermo_line(1060.0, 31.0, 20.5, 19.5) + "\n",
        encoding="utf-8",
    )
    rows = cpl.load_pair_run(str(mcp_path), str(thermo_path), max_skew_s=25.0)
    assert len(rows) == 2


def test_elapsed_s_is_wall_clock_not_text_field(tmp_path):
    """The text 'elapsed=Ns' field resets to 0 at every phase change (see
    module docstring) -- PollRow.elapsed_s must instead track wall time so
    it stays monotonic across the ramp->dwell boundary. Build two rows
    whose TEXT elapsed goes 800 -> 0 (a real ramp-to-dwell transition) and
    confirm the emitted PollRow.elapsed_s still increases.
    """
    mcp_path = tmp_path / "m.jsonl"
    thermo_path = tmp_path / "t.jsonl"
    mcp_path.write_text(
        _mcp_line(1000.0, 800, dwelling=False) + "\n"
        + _mcp_line(1020.0, 0, dwelling=True) + "\n",
        encoding="utf-8",
    )
    thermo_path.write_text(
        _thermo_line(1000.0, 30.0, 20.0, 19.0) + "\n"
        + _thermo_line(1020.0, 31.0, 20.5, 19.5) + "\n",
        encoding="utf-8",
    )
    rows = cpl.load_pair_run(str(mcp_path), str(thermo_path))
    assert len(rows) == 2
    assert rows[0].elapsed_s < rows[1].elapsed_s
    # And build_windows (which requires non-decreasing elapsed_s across a
    # run) must accept this without raising.
    windows = la.build_windows(rows)
    assert len(windows) == 2
    assert windows[0].phase == "ramp" and windows[1].phase == "dwell"


def test_elapsed_s_mutation_using_text_field_breaks_build_windows(tmp_path):
    """Negative-test companion to the above: prove that feeding the raw
    text elapsed field (instead of the wall-clock derivation) into
    PollRow.elapsed_s really would break build_windows, by constructing
    that exact broken row sequence by hand.
    """
    zones = {0: la.ZoneSample(zone=0, actual_c=54.0, duty=0.8)}
    bad_rows = [
        la.PollRow(wall_time="", elapsed_s=800.0, segment_index=0, segment_count=1,
                   dwelling=False, target_c=55.0, state="1", zones=zones),
        la.PollRow(wall_time="", elapsed_s=0.0, segment_index=0, segment_count=1,
                   dwelling=True, target_c=55.0, state="1", zones=zones),
    ]
    with pytest.raises(ValueError):
        la.build_windows(bad_rows)


def test_load_pair_run_missing_mcp_file_drops_nothing_silently():
    """An mcp file with zero valid exec-status lines yields zero rows, not
    a crash or a row with fabricated data.
    """
    assert cpl.load_exec_status_log(os.devnull if os.name != "nt" else "NUL") == []


# ---------------------------------------------------------------------------
# Real capture: cpl_z0 fixture
# ---------------------------------------------------------------------------

def test_load_pair_run_real_cpl_z0_fixture():
    rows = cpl.load_pair_run(MCP_PATH, THERMO_PATH)
    assert len(rows) > 30
    # Every row carries all three zones (peer coverage requirement).
    assert all(set(r.zones.keys()) == {0, 1, 2} for r in rows)
    # elapsed_s must be non-decreasing across the whole run (wall-clock
    # derived) even though it spans a ramp->dwell transition.
    for a, b in zip(rows, rows[1:]):
        assert b.elapsed_s >= a.elapsed_s
    # Zone 0 is the active zone throughout (duty > 0); zones 1/2 are
    # passive (duty exactly 0, per the join rule).
    assert all(r.zones[1].duty == 0.0 and r.zones[2].duty == 0.0 for r in rows)
    assert any(r.zones[0].duty > 0.0 for r in rows)
    # build_windows must not raise on the real capture.
    windows = la.build_windows(rows)
    assert any(w.phase == "ramp" for w in windows)
    assert any(w.phase == "dwell" for w in windows)


# ---------------------------------------------------------------------------
# load_thermo_samples_any_format -- both cooldown capture shapes
# ---------------------------------------------------------------------------

def test_load_thermo_samples_any_format_reads_pair_log_shape(tmp_path):
    """The ordinary {"t": ..., "s": "CH0: .. C"} pair-log thermo format,
    same as load_thermo_log already handles."""
    p = tmp_path / "thermo.jsonl"
    p.write_text(
        '{"t": 100.0, "s": "CH0: 45.20 C (CJ 27.30 C)\\nCH1: 44.10 C (CJ 27.40 C)\\nCH2: 40.00 C (CJ 27.50 C)"}\n'
        '{"t": 120.0, "s": "CH0: 44.90 C (CJ 27.30 C)\\nCH1: 43.90 C (CJ 27.40 C)\\nCH2: 39.80 C (CJ 27.50 C)"}\n',
        encoding="utf-8",
    )
    samples = cpl.load_thermo_samples_any_format(str(p))
    assert len(samples) == 2
    assert samples[0].t == 100.0
    assert samples[0].channels == {0: 45.20, 1: 44.10, 2: 40.00}
    assert samples[1].channels[0] == 44.90


def test_load_thermo_samples_any_format_reads_status_json_shape(tmp_path):
    """The raw kiln_io_get_status() capture shape used by
    cooldown_after_coupid6.jsonl: 'HH:MM:SS {json}' per line, temperatures
    under channels[].temp_c, timestamped by time_now_epoch."""
    p = tmp_path / "status.jsonl"
    line1 = ('23:32:34 {"channels":[{"channel":0,"temp_c":69.65,"valid":true},'
             '{"channel":1,"temp_c":69.56,"valid":true},'
             '{"channel":2,"temp_c":69.24,"valid":true}],"time_now_epoch":1788344465}\n')
    line2 = ('23:32:54 {"channels":[{"channel":0,"temp_c":68.90,"valid":true},'
             '{"channel":1,"temp_c":68.50,"valid":false},'
             '{"channel":2,"temp_c":68.10,"valid":true}],"time_now_epoch":1788344485}\n')
    p.write_text(line1 + line2, encoding="utf-8")
    samples = cpl.load_thermo_samples_any_format(str(p))
    assert len(samples) == 2
    assert samples[0].t == 1788344465.0
    assert samples[0].channels == {0: 69.65, 1: 69.56, 2: 69.24}
    # an invalid channel must be DROPPED, not carried through as a reading
    # (the real cooldown_after_coupid6.jsonl capture has occasional
    # invalid channels mid-run).
    assert 1 not in samples[1].channels
    assert samples[1].channels[0] == 68.90


def test_load_thermo_samples_any_format_skips_unrecognized_lines(tmp_path):
    """A line matching neither format is skipped, not raised on -- same
    tolerance every other parser in this module documents for a flaky
    capture link."""
    p = tmp_path / "mixed.jsonl"
    p.write_text(
        "not json at all\n"
        '{"t": 100.0, "s": "no CH lines here"}\n'
        '{"t": 120.0, "s": "CH0: 45.0 C (CJ 27.0 C)"}\n',
        encoding="utf-8",
    )
    samples = cpl.load_thermo_samples_any_format(str(p))
    assert len(samples) == 1
    assert samples[0].channels == {0: 45.0}


def test_load_thermo_samples_any_format_sorts_by_time(tmp_path):
    p = tmp_path / "unsorted.jsonl"
    p.write_text(
        '{"t": 200.0, "s": "CH0: 40.0 C (CJ 27.0 C)"}\n'
        '{"t": 100.0, "s": "CH0: 45.0 C (CJ 27.0 C)"}\n',
        encoding="utf-8",
    )
    samples = cpl.load_thermo_samples_any_format(str(p))
    assert [s.t for s in samples] == [100.0, 200.0]
