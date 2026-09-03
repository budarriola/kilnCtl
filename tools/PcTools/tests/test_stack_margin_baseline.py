"""Tests for kilnctrl.stack_margin_baseline -- the DRAM_PSRAM_PLAN.md
section 4.3/7 measurement-procedure module. Every test here works from
fabricated StackMarginEntry/FirmwareVersion objects; nothing touches a link,
a serial port, or a real board -- see capture_stack_margin_baseline.py's own
docstring for why that split exists.
"""
from __future__ import annotations

from datetime import datetime, timezone

import pytest

from kilnctrl.devices_info import FirmwareVersion, StackMarginEntry
from kilnctrl.protocol import StackMarginLevel
from kilnctrl.stack_margin_baseline import (
    LOAD_CONDITIONS,
    build_record,
    load_records,
    render_markdown_table,
    worst_case_across_conditions,
    write_record,
)

_FW = FirmwareVersion(protocol_version=3, dirty=False, commit="abc1234", built="2026-09-02 00:00:00Z")
_NOW = datetime(2026, 9, 2, 12, 0, 0, tzinfo=timezone.utc)


def _entry(name, configured, hwm, alive=True, level=StackMarginLevel.OK):
    return StackMarginEntry(name=name, configured_stack_bytes=configured, hwm_bytes=hwm, alive=alive, level=level)


def test_build_record_rejects_unknown_condition():
    """The whole point of LOAD_CONDITIONS is that a capture is tagged with
    one of the three the plan actually asks for -- a typo'd condition name
    must not silently create a fourth, uncomparable bucket."""
    with pytest.raises(ValueError):
        build_record("mostly_idle", [_entry("t", 4096, 2000)], _FW, now=_NOW)


def test_build_record_accepts_every_declared_condition():
    for cond in LOAD_CONDITIONS:
        rec = build_record(cond, [_entry("t", 4096, 2000)], _FW, now=_NOW)
        assert rec.condition == cond
        assert rec.fw_commit == "abc1234"
        assert rec.captured_at_utc == "2026-09-02T12:00:00Z"


def test_write_and_load_round_trips_entries_exactly(tmp_path):
    rec = build_record(
        "idle",
        [_entry("kiln_io_owner", 4096, 3500), _entry("profile_exec_wdt", 4096, 368, level=StackMarginLevel.CRITICAL)],
        _FW,
        notes="fresh boot, nothing running",
        now=_NOW,
    )
    path = write_record(rec, tmp_path)
    assert path.exists()

    loaded = load_records(tmp_path)
    assert len(loaded) == 1
    got = loaded[0]
    assert got.condition == "idle"
    assert got.fw_commit == "abc1234"
    assert got.notes == "fresh boot, nothing running"
    assert {e.name for e in got.entries} == {"kiln_io_owner", "profile_exec_wdt"}
    wdt = next(e for e in got.entries if e.name == "profile_exec_wdt")
    assert wdt.hwm_bytes == 368
    assert wdt.level == StackMarginLevel.CRITICAL


def test_write_record_filename_encodes_condition_and_commit(tmp_path):
    rec = build_record("mid_firing", [_entry("t", 4096, 1000)], _FW, now=_NOW)
    path = write_record(rec, tmp_path)
    assert "mid_firing" in path.name
    assert "abc1234" in path.name


def test_worst_case_across_conditions_takes_the_smallest_hwm():
    """The whole reason to capture three conditions instead of one: a task's
    real worst case is the minimum HWM seen across all of them, not
    whichever capture happened to run last."""
    idle = build_record("idle", [_entry("httpd_worker", 8192, 6000)], _FW, now=_NOW)
    firing = build_record("mid_firing", [_entry("httpd_worker", 8192, 3000)], _FW, now=_NOW)
    web = build_record("web_ui_open", [_entry("httpd_worker", 8192, 2772)], _FW, now=_NOW)
    worst = worst_case_across_conditions([idle, firing, web])
    assert worst["httpd_worker"].hwm_bytes == 2772


def test_worst_case_prefers_a_live_reading_over_a_dead_one():
    """A task that failed to register/create under one load condition (e.g.
    a boot race) must not have that show up as its 'worst case' just because
    a dead entry sorts lower -- a genuinely dead task should only win if
    EVERY capture shows it dead."""
    alive_reading = build_record("idle", [_entry("thermo_owner", 4096, 3000)], _FW, now=_NOW)
    dead_reading = build_record(
        "mid_firing", [_entry("thermo_owner", 4096, 0, alive=False)], _FW, now=_NOW
    )
    worst = worst_case_across_conditions([alive_reading, dead_reading])
    assert worst["thermo_owner"].alive is True
    assert worst["thermo_owner"].hwm_bytes == 3000


def test_worst_case_reports_dead_if_every_capture_saw_it_dead():
    r1 = build_record("idle", [_entry("gone_task", 4096, 0, alive=False)], _FW, now=_NOW)
    r2 = build_record("mid_firing", [_entry("gone_task", 4096, 0, alive=False)], _FW, now=_NOW)
    worst = worst_case_across_conditions([r1, r2])
    assert worst["gone_task"].alive is False


def test_render_markdown_table_matches_dram_psram_plan_column_shape():
    """DRAM_PSRAM_PLAN.md section 3.1's table columns, in order: task, free
    at worst, allocated, headroom. This proves the renderer's output can be
    pasted straight in without reformatting."""
    rec = build_record(
        "idle",
        [_entry("profile_exec_wdt", 4096, 368, level=StackMarginLevel.CRITICAL)],
        _FW,
        now=_NOW,
    )
    table = render_markdown_table([rec])
    assert "| task | free at worst | allocated | headroom | condition(s) captured |" in table
    assert "`profile_exec_wdt`" in table
    assert "368 B" in table
    assert "4096 B" in table
    assert "9.0%" in table  # 368/4096 = 8.98...%


def test_render_markdown_table_marks_not_running_tasks():
    rec = build_record("idle", [_entry("dead_task", 3072, 0, alive=False)], _FW, now=_NOW)
    table = render_markdown_table([rec])
    assert "not running" in table


def test_load_records_skips_unparseable_file_without_raising(tmp_path):
    """A partially-written or corrupted capture file must not take down the
    whole report -- one bad file is dropped, not fatal."""
    (tmp_path / "stack_margin_idle_broken.json").write_text("{not valid json", encoding="utf-8")
    good = build_record("idle", [_entry("t", 4096, 2000)], _FW, now=_NOW)
    write_record(good, tmp_path)

    loaded = load_records(tmp_path)
    assert len(loaded) == 1
    assert loaded[0].condition == "idle"
