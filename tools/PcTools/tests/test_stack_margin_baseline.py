"""Tests for kilnctrl.stack_margin_baseline -- the DRAM_PSRAM_PLAN.md
section 4.3/7 measurement-procedure module. Every test here works from
fabricated StackMarginEntry/FirmwareVersion objects; nothing touches a link,
a serial port, or a real board -- see capture_stack_margin_baseline.py's own
docstring for why that split exists.
"""
from __future__ import annotations

import json
from datetime import datetime, timezone

import pytest

from kilnctrl.devices_info import FirmwareVersion, StackMarginEntry
from kilnctrl.protocol import StackMarginLevel
from kilnctrl.stack_margin_baseline import (
    LOAD_CONDITIONS,
    LoadSnapshot,
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


def test_worst_case_across_conditions_collapses_entries_sharing_a_name():
    """Documents the sharp edge worst_case_across_conditions() has when two
    DIFFERENT tasks are registered on-target under the same name -- exactly
    what i2c_owner_init() did before its 2026-09-03 fix (SX1509's IO
    expander and NS2009's touch controller both registered as "i2c_owner",
    with different configured stack sizes). This function has no way to
    know two same-named entries in one record are actually two different
    tasks; it treats the second as a second (smaller-or-not) reading of the
    SAME task and keeps only one, per this module's `best` dict keyed by
    name. That is the real-world consequence the firmware-side fix (giving
    each i2c_owner call site a distinct name) avoids -- this test exists so
    that if some other shared-init call site reintroduces the same-name
    pattern, the silent-collapse behavior it triggers here is at least
    documented and pinned, not rediscovered from a live board."""
    # One capture, two entries claiming the same name -- exactly what a
    # single GET_STACK_MARGIN reply looked like before the fix: two owner
    # tasks, "i2c_owner_sx1509" (4096 B stack) and "i2c_owner_ns2009"
    # (3072 B), both reported under the literal name "i2c_owner".
    rec = build_record(
        "idle",
        [_entry("i2c_owner", 4096, 3000), _entry("i2c_owner", 3072, 1500)],
        _FW,
        now=_NOW,
    )
    worst = worst_case_across_conditions([rec])
    # Only ONE entry survives under the shared name -- the other is gone
    # with no error, no warning, nothing to say a second task's reading
    # was ever there. The 3072/1500 pair -- NS2009's real numbers -- wins
    # here only because it is smaller and both are alive; had NS2009's
    # reading come from a load condition where it read HIGHER than
    # SX1509's, SX1509's own genuinely-worse reading would have been the
    # one silently dropped instead.
    assert len(worst) == 1
    assert worst["i2c_owner"].hwm_bytes == 1500
    assert worst["i2c_owner"].configured_stack_bytes == 3072


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


def test_load_snapshot_is_idle_true_only_when_nothing_is_running():
    """The 2026-09-04 finding: an idle capture must be identifiable from
    structured data, not a caller-typed string. is_idle is false the moment
    ANY of the three signals says otherwise."""
    assert LoadSnapshot(firing_active=False, autotune_active=False, zones_heating=0, observations=0).is_idle
    assert not LoadSnapshot(firing_active=True, autotune_active=False, zones_heating=0, observations=0).is_idle
    assert not LoadSnapshot(firing_active=False, autotune_active=True, zones_heating=0, observations=12).is_idle
    assert not LoadSnapshot(firing_active=False, autotune_active=False, zones_heating=1, observations=0).is_idle


def test_load_snapshot_json_round_trips():
    snap = LoadSnapshot(firing_active=True, autotune_active=False, zones_heating=2, observations=340)
    back = LoadSnapshot.from_json_dict(snap.to_json_dict())
    assert back == snap


def test_build_record_persists_load_snapshot_through_write_and_load(tmp_path):
    """A record built with a real LoadSnapshot (as the board reported it, not
    a caller flag) must survive the JSON round trip -- this is the field
    check_stack_margin_baseline.py's check #4 depends on existing at all."""
    load = LoadSnapshot(firing_active=True, autotune_active=False, zones_heating=1, observations=90)
    rec = build_record(
        "mid_firing", [_entry("profile_executor", 4096, 3000)], _FW, load=load, now=_NOW,
    )
    assert rec.load == load

    path = write_record(rec, tmp_path)
    loaded = load_records(tmp_path)
    assert len(loaded) == 1
    assert loaded[0].load == load


def test_build_record_load_defaults_to_none_for_backward_compatibility():
    """A caller that has not been taught about LoadSnapshot yet (or a legacy
    baseline written before this field existed) must not be forced to
    fabricate one -- load stays None, and load_records() must not choke on
    the resulting JSON (no 'load' key at all, the exact shape of the
    checked-in idle_2bcdc2d baseline predating this feature)."""
    rec = build_record("idle", [_entry("t", 4096, 2000)], _FW, now=_NOW)
    assert rec.load is None
    d = rec.to_json_dict()
    assert d["load"] is None


def test_load_records_tolerates_a_file_with_no_load_key(tmp_path):
    """Simulates a pre-existing baseline file (like the real
    stack_margin_idle_2bcdc2d_... capture) written before the `load` field
    existed: no 'load' key in the JSON at all, not even null."""
    (tmp_path / "stack_margin_idle_legacy.json").write_text(
        json.dumps(
            {
                "condition": "idle",
                "captured_at_utc": "2026-09-01T00:00:00Z",
                "fw_commit": "legacy1",
                "fw_dirty": False,
                "fw_built": "2026-09-01 00:00:00Z",
                "notes": "",
                "entries": [
                    {"name": "t", "configured_stack_bytes": 4096, "hwm_bytes": 2000, "alive": True, "level": "OK"}
                ],
            }
        ),
        encoding="utf-8",
    )
    loaded = load_records(tmp_path)
    assert len(loaded) == 1
    assert loaded[0].load is None


def test_load_records_skips_unparseable_file_without_raising(tmp_path):
    """A partially-written or corrupted capture file must not take down the
    whole report -- one bad file is dropped, not fatal."""
    (tmp_path / "stack_margin_idle_broken.json").write_text("{not valid json", encoding="utf-8")
    good = build_record("idle", [_entry("t", 4096, 2000)], _FW, now=_NOW)
    write_record(good, tmp_path)

    loaded = load_records(tmp_path)
    assert len(loaded) == 1
    assert loaded[0].condition == "idle"
