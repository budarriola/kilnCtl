#!/usr/bin/env python3
"""Unit tests for wave 1d (docs/BENCH_TEST_SYSTEM_PLAN.md §8) -- FL/SK
baselines: the new Pico record type in stack_margin_baseline.py, the
SK-01/02 baseline-comparison judge, and the FL-09
project_description.json-absent-is-INCONCLUSIVE override.

Per feedback_negative_test_every_check, every judgment function gets at
least one test that feeds it a bad/regressed input and confirms it reports
something other than PASS.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_wave1d.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import stack_margin_baseline as smb  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
from kilnctrl.devices_info import FirmwareVersion, StackMarginEntry  # noqa: E402
from kilnctrl.protocol import StackMarginLevel  # noqa: E402


def _entry(name, hwm, configured=4096, alive=True, level=StackMarginLevel.OK):
    return StackMarginEntry(
        name=name, configured_stack_bytes=configured, hwm_bytes=hwm, alive=alive, level=level
    )


def _fw():
    return FirmwareVersion(protocol_version=12, dirty=False, commit="abcdef1", built="2026-09-19T00:00:00Z")


class PicoRecordRoundTripTest(unittest.TestCase):
    """Round-trip for the new Pico baseline record type, and confirmation
    that it does not disturb the pre-existing ESP loader/old-format
    records (backward compatibility, plan requirement)."""

    def test_pico_record_round_trips(self):
        e = smb.PicoStackMarginEntry(
            task_id=2, name="safety_core", measured=True, high_water_words=300, stack_total_words=1024
        )
        record = smb.build_pico_record(
            [e], saftyfw_commit="deadbee", saftyfw_build_date="2026-09-19",
            saftyfw_build_time="12:00:00", rounds_completed=10, last_tick_ms=5000,
            all_measured=True, notes="unit test",
        )
        with tempfile.TemporaryDirectory() as d:
            path = smb.write_pico_record(record, Path(d))
            self.assertTrue(path.is_file())
            back = smb.load_pico_records(Path(d))
        self.assertEqual(len(back), 1)
        self.assertEqual(back[0].saftyfw_commit, "deadbee")
        self.assertEqual(back[0].entries[0].name, "safety_core")
        self.assertAlmostEqual(back[0].entries[0].free_fraction, 300 / 1024)

    def test_unmeasured_entry_has_no_free_fraction(self):
        e = smb.PicoStackMarginEntry(task_id=3, name="log_task", measured=False)
        self.assertIsNone(e.free_fraction)

    def test_pico_record_invisible_to_esp_load_records(self):
        """A Pico file matches load_records()'s glob (stack_margin_*.json)
        but must not be misread as an ESP record -- the KeyError it raises
        parsing Pico-shaped entries as ESP-shaped ones is caught by
        load_records()'s existing except-continue, so it is silently
        skipped, never crashes, and never contaminates an ESP comparison."""
        e = smb.PicoStackMarginEntry(task_id=1, name="watchdog_task", measured=True,
                                      high_water_words=100, stack_total_words=512)
        record = smb.build_pico_record(
            [e], saftyfw_commit="c0ffee1", saftyfw_build_date="2026-09-19",
            saftyfw_build_time="00:00:00", rounds_completed=1, last_tick_ms=1, all_measured=True,
        )
        with tempfile.TemporaryDirectory() as d:
            smb.write_pico_record(record, Path(d))
            esp_records = smb.load_records(Path(d))
        self.assertEqual(esp_records, [])

    def test_old_format_esp_record_still_loads(self):
        """A record written before the `load` field existed (no "load" key
        at all in the JSON) must still load_records() cleanly -- this is
        the backward-compatibility guarantee for pre-existing baseline
        files, unrelated to but exercised alongside the Pico addition."""
        entry = _entry("kiln_io_owner", hwm=1000)
        record = smb.build_record("idle", [entry], _fw(), notes="pre-load-field")
        with tempfile.TemporaryDirectory() as d:
            path = smb.write_record(record, Path(d))
            import json
            raw = json.loads(path.read_text(encoding="utf-8"))
            del raw["load"]  # simulate a file captured before this field existed
            path.write_text(json.dumps(raw), encoding="utf-8")
            back = smb.load_records(Path(d))
        self.assertEqual(len(back), 1)
        self.assertIsNone(back[0].load)
        self.assertEqual(back[0].entries[0].name, "kiln_io_owner")


class StackMarginAgainstBaselineTest(unittest.TestCase):
    def test_no_baseline_is_inconclusive_not_fail(self):
        entries = [_entry("new_task", hwm=500)]
        r = J.judge_stack_margin_against_baseline(entries, {}, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("new_task", r.reason)

    def test_matches_or_exceeds_baseline_passes(self):
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=900)]
        r = J.judge_stack_margin_against_baseline(entries, baseline, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_regression_below_baseline_fails(self):
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=200)]
        r = J.judge_stack_margin_against_baseline(entries, baseline, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("thermo_task", r.reason)

    def test_dead_task_fails_outright(self):
        baseline = {"log_task": _entry("log_task", hwm=800)}
        entries = [_entry("log_task", hwm=0, alive=False)]
        r = J.judge_stack_margin_against_baseline(entries, baseline, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("log_task", r.reason)

    def test_sk02_absolute_floor_fails_even_with_good_baseline_margin(self):
        """The httpd stack blob class: a task can be comfortably above its
        own committed baseline yet still be dangerously close to zero in
        absolute terms -- SK-02's floor must catch that independently."""
        baseline = {"httpd_worker": _entry("httpd_worker", hwm=100)}
        entries = [_entry("httpd_worker", hwm=200)]  # above baseline, below 512 B floor
        r = J.judge_stack_margin_against_baseline(entries, baseline, min_free_bytes=512)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("floor", r.reason)

    def test_no_entries_fails(self):
        r = J.judge_stack_margin_against_baseline([], {}, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.FAIL)


class Fl09ProjectDescriptionTest(unittest.TestCase):
    def test_no_match_still_fails(self):
        r = J.judge_pico_archive_with_description("error: no archived ELF found", lambda p: True)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_found_with_description_passes(self):
        r = J.judge_pico_archive_with_description(
            "found /archive/SaftyFW-abc123.elf (exact match, archived 2026-09-19)",
            lambda p: True,
        )
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_found_without_description_is_inconclusive_never_fail(self):
        r = J.judge_pico_archive_with_description(
            "found /archive/SaftyFW-abc123.elf (exact match, archived 2026-09-19)",
            lambda p: False,
        )
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("project_description.json", r.reason)


if __name__ == "__main__":
    unittest.main()
