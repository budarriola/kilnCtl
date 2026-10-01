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
        """A same, known-commit regression beyond tolerance is a hard FAIL.
        No commit info at all is "unverified" (same as a mismatch) per
        judge_stack_margin_against_baseline()'s commit-aware rule -- see
        test_beyond_tolerance_drop_unknown_commit_is_inconclusive_not_fail
        for that case -- so this test supplies a matching commit on both
        sides to isolate the "real regression on an unchanged build" case."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=200)]
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("thermo_task", r.reason)

    def test_regression_below_baseline_no_commit_info_is_inconclusive(self):
        """Same regression, but the caller supplied no commit info at all --
        that is "unknown", not "same build", so this must be INCONCLUSIVE,
        never a silent FAIL that looks like a confirmed regression."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=200)]
        r = J.judge_stack_margin_against_baseline(entries, baseline, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
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

    def test_drop_within_tolerance_same_commit_passes(self):
        """4-8 B drops on an unchanged commit are ordinary run-to-run noise
        (2026-09-24 bench finding) -- must not FAIL."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=794)]  # 6 B drop, well under the 64 B tolerance
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_drop_beyond_tolerance_fails(self):
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=700)]  # 100 B drop, beyond the 64 B tolerance
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("thermo_task", r.reason)

    def test_drop_within_tolerance_cross_commit_is_inconclusive(self):
        """The same 6 B drop, but the baseline was captured on a different
        commit than the one currently running -- cannot tell noise from a
        real (small) regression on a different build, so this downgrades to
        INCONCLUSIVE rather than a silent PASS or a FAIL."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=794)]
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit="1112222", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("mismatch", r.reason)
        self.assertIn("thermo_task", r.reason)

    def test_beyond_tolerance_drop_cross_commit_is_inconclusive_not_fail(self):
        """A cross-commit comparison can only ever be INCONCLUSIVE (unless
        the absolute floor is breached): two different firmware builds
        legitimately differ, so a beyond-tolerance drop there is not
        evidence of a regression. The numbers must still be visible in the
        reason (both the task and both commits), never silently dropped."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=700)]  # 100 B drop, beyond tolerance
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit="1112222", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("thermo_task", r.reason)
        self.assertIn("1112222", r.reason)
        self.assertIn("abcdef1", r.reason)

    def test_beyond_tolerance_drop_unknown_commit_is_inconclusive_not_fail(self):
        """Same rule when the commit is simply unverified (missing/placeholder
        on either side), not a positive mismatch."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=700)]  # 100 B drop, beyond tolerance
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit=None, baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("thermo_task", r.reason)

    def test_beyond_tolerance_drop_same_commit_still_fails(self):
        """Negative test: a same-commit, beyond-tolerance drop must still be
        a hard FAIL -- the cross-commit softening above must never leak into
        the same-build case."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=700)]  # 100 B drop, beyond tolerance
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=None,
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("thermo_task", r.reason)

    def test_floor_breach_still_fails_on_commit_mismatch(self):
        """Negative test: the absolute floor must FAIL even when the commit
        also mismatches and the drop is beyond tolerance -- the floor is
        never softened by a commit difference."""
        baseline = {"httpd_worker": _entry("httpd_worker", hwm=800)}
        entries = [_entry("httpd_worker", hwm=100)]  # huge drop, below the 512 B floor
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=512,
            board_fw_commit="1112222", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("floor", r.reason)

    def test_drop_within_tolerance_unknown_commit_is_inconclusive(self):
        """A missing/placeholder commit on either side is never treated as
        'same build' -- a within-tolerance drop must not silently PASS."""
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=794)]
        for board, base in ((None, {"abcdef1"}), ("abcdef1", None), ("abcdef1", set()),
                            ("unknown", {"unknown"}), ("", {"abcdef1"}), ("abcdef1", {"?"})):
            with self.subTest(board=board, base=base):
                r = J.judge_stack_margin_against_baseline(
                    entries, baseline, min_free_bytes=None,
                    board_fw_commit=board, baseline_fw_commits=base,
                )
                self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
                self.assertIn("unknown", r.reason)

    def test_no_drop_unknown_commit_still_passes(self):
        baseline = {"thermo_task": _entry("thermo_task", hwm=800)}
        entries = [_entry("thermo_task", hwm=800)]
        r = J.judge_stack_margin_against_baseline(entries, baseline, min_free_bytes=None)
        self.assertEqual(r.verdict, Verdict.PASS)

    def test_floor_fails_regardless_of_tolerance(self):
        """A task under its configured warn threshold FAILs even when its
        drop from baseline is within the noise tolerance."""
        baseline = {"httpd_worker": _entry("httpd_worker", hwm=520)}
        entries = [_entry("httpd_worker", hwm=500)]  # 20 B drop (within tolerance), below 512 B floor
        r = J.judge_stack_margin_against_baseline(
            entries, baseline, min_free_bytes=512,
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"},
        )
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("floor", r.reason)


class StackMarginTaskToleranceOverrideTest(unittest.TestCase):
    """2026-10-01 owner decision: the three UART bridge tasks get 384 B,
    every other task keeps 64 B; floor and dead-task FAIL are unchanged."""

    def _judge(self, name, base, live, **kw):
        return J.judge_stack_margin_against_baseline(
            [_entry(name, hwm=live)], {name: _entry(name, hwm=base)},
            min_free_bytes=kw.pop("min_free_bytes", None),
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"}, **kw,
        )

    def test_bridge_300b_drop_passes(self):
        for name in ("thermo_uart_bridge", "io_uart_bridge", "info_uart_bridge"):
            self.assertEqual(self._judge(name, 2000, 1700).verdict, Verdict.PASS, name)

    def test_bridge_400b_drop_fails(self):
        for name in ("thermo_uart_bridge", "io_uart_bridge", "info_uart_bridge"):
            r = self._judge(name, 2000, 1600)
            self.assertEqual(r.verdict, Verdict.FAIL, name)
            self.assertIn(name, r.reason)

    def test_non_bridge_100b_drop_still_fails(self):
        self.assertEqual(self._judge("lvgl", 4000, 3900).verdict, Verdict.FAIL)

    def test_wider_default_tolerance_never_narrowed_by_override(self):
        """tolerance_bytes=1000 must win over a bridge's 384 B override."""
        for name in ("thermo_uart_bridge", "io_uart_bridge", "info_uart_bridge", "lvgl"):
            self.assertEqual(self._judge(name, 2000, 1500, tolerance_bytes=1000).verdict, Verdict.PASS, name)

    def test_fail_reason_names_per_task_tolerance(self):
        r = self._judge("info_uart_bridge", 2000, 1600)
        self.assertIn("per-task noise tolerance", r.reason)
        self.assertIn("(-400 B > 384 B)", r.reason)

    def test_bridge_floor_still_fails(self):
        r = self._judge("thermo_uart_bridge", 600, 500, min_free_bytes=512)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("floor", r.reason)

    def test_bridge_dead_still_fails(self):
        r = J.judge_stack_margin_against_baseline(
            [_entry("info_uart_bridge", hwm=0, alive=False)],
            {"info_uart_bridge": _entry("info_uart_bridge", hwm=2000)},
            board_fw_commit="abcdef1", baseline_fw_commits={"abcdef1"},
        )
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
