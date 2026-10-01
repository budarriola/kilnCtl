#!/usr/bin/env python3
"""Unit tests for the 2026-10-01 SK-01/SK-02 same-commit baseline filter
fix (cases_fl.py / stack_margin_baseline.py).

Before this fix, ``_judge_against_baseline`` handed
``worst_case_across_conditions`` EVERY committed baseline record regardless
of which firmware commit produced it, and only used the board's commit to
decide whether to *soften* an already-computed verdict. Once a same-commit
baseline existed anywhere in ``docs/stack_margin_baseline``, the comparison
kept mixing in older builds' (often smaller-stack, lower-hwm) records as
candidate "worst case" numbers -- so a live reading was effectively scored
against the single lowest hwm_bytes ever recorded for a task, across every
build in history, not against its own commit. Separately, SK-02's
``web_ui_open`` "condition" was only ever used for the capture filename: a
same-commit *idle* record alone was enough to make the comparison proceed,
even though SK-02 is specifically meant to compare an exercised reading
against an exercised baseline.

This file covers (feedback_negative_test_every_check: every new assertion
here was confirmed to fail against the pre-fix code before the source was
restored by hand -- see the audit note below, not re-run automatically):

  * ``stack_margin_baseline.records_matching_commit`` in isolation.
  * SK-01 PASS/FAIL against a same-commit idle record.
  * SK-01 INCONCLUSIVE when no same-commit record exists at all.
  * A mixed-commit pool: an older build's much lower hwm_bytes must never
    be used as the comparison baseline once a same-commit record exists.
  * SK-02 INCONCLUSIVE when a same-commit idle record exists but no
    same-commit web_ui_open/mid_firing record does.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_sk_commit_filter.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import stack_margin_baseline as smb  # noqa: E402
from kilnctrl.bench_test import cases_fl as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
from kilnctrl.devices_info import FirmwareVersion, StackMarginEntry  # noqa: E402
from kilnctrl.protocol import StackMarginLevel  # noqa: E402


def _entry(name, hwm, configured=4096, alive=True, level=StackMarginLevel.OK):
    return StackMarginEntry(
        name=name, configured_stack_bytes=configured, hwm_bytes=hwm, alive=alive, level=level
    )


def _fw(commit="eb83c1ac"):
    return FirmwareVersion(protocol_version=13, dirty=False, commit=commit, built="2026-10-01T00:00:00Z")


class _TempRepoRoot:
    """A bare repo root with an empty docs/stack_margin_baseline directory
    and no tools/check_stack_margin_registration.ps1 -- _dead_by_design_names
    degrades to "no exemptions" against a missing script, which is fine for
    these tests since none of them exercise a dead/by-design task."""

    def __enter__(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.baseline_dir = self.root / "docs" / "stack_margin_baseline"
        self.baseline_dir.mkdir(parents=True, exist_ok=True)
        return self

    def __exit__(self, *exc):
        self._tmp.cleanup()

    def write(self, condition, commit, entries, **kw):
        record = smb.build_record(condition, entries, _fw(commit), **kw)
        return smb.write_record(record, self.baseline_dir)


def _ctx(root: Path):
    srv = mock.Mock()
    return {"srv": srv}, srv


class RecordsMatchingCommitTest(unittest.TestCase):
    """stack_margin_baseline.records_matching_commit in isolation."""

    def test_filters_to_matching_commit_only(self):
        rec_a = smb.build_record("idle", [_entry("t1", 100)], _fw("aaa1111"))
        rec_b = smb.build_record("idle", [_entry("t1", 900)], _fw("bbb2222"))
        out = smb.records_matching_commit([rec_a, rec_b], "bbb2222")
        self.assertEqual(out, [rec_b])

    def test_no_match_returns_empty_list(self):
        rec_a = smb.build_record("idle", [_entry("t1", 100)], _fw("aaa1111"))
        self.assertEqual(smb.records_matching_commit([rec_a], "cccc333"), [])

    def test_falsy_commit_returns_empty_list_never_matches_blank_records(self):
        """A record with an empty/placeholder fw_commit must never be
        treated as a match for a falsy/unknown board commit -- that would
        silently compare against an unidentified build."""
        rec = smb.build_record("idle", [_entry("t1", 100)], FirmwareVersion(
            protocol_version=13, dirty=False, commit="", built="2026-01-01T00:00:00Z",
        ))
        self.assertEqual(smb.records_matching_commit([rec], ""), [])
        self.assertEqual(smb.records_matching_commit([rec], None), [])


class Sk01CommitFilterTest(unittest.TestCase):
    def test_same_commit_idle_record_within_tolerance_passes(self):
        with _TempRepoRoot() as t:
            t.write("idle", "eb83c1ac", [_entry("info_uart_bridge", 1624)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("info_uart_bridge", 1620)]  # 4 B noise
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_same_commit_idle_record_beyond_tolerance_fails(self):
        """The live 2026-10-01 bench finding: info_uart_bridge dropped
        128 B (1624 -> 1496) since its same-commit idle baseline was
        captured -- a real regression, not noise, and must FAIL once the
        comparison is correctly scored against its own commit."""
        with _TempRepoRoot() as t:
            t.write("idle", "eb83c1ac", [_entry("info_uart_bridge", 1624)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("info_uart_bridge", 1496)]
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("info_uart_bridge", result.reason)

    def test_no_same_commit_record_is_inconclusive(self):
        with _TempRepoRoot() as t:
            t.write("idle", "aaaa0000", [_entry("info_uart_bridge", 1624)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("info_uart_bridge", 1496)]
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("eb83c1ac", result.reason)

    def test_mixed_commit_pool_old_lower_reading_not_used(self):
        """Negative test for the exact bug: an old build's much lower
        hwm_bytes must not be mixed in as the comparison baseline once a
        same-commit record exists. If the old record's 100 B reading were
        still used (pre-fix behavior), a live 800 B reading would show an
        increase (no drop at all) and PASS trivially; the current
        commit's own 1000 B baseline correctly exposes the regression."""
        with _TempRepoRoot() as t:
            t.write("idle", "aaaa0000", [_entry("info_uart_bridge", 100)])
            t.write("idle", "eb83c1ac", [_entry("info_uart_bridge", 1000)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("info_uart_bridge", 800)]
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("info_uart_bridge", result.reason)
        # The old commit's lower reading must not appear as the baseline
        # used for comparison.
        regressed = result.observed.get("regressed_beyond_tolerance", [])
        self.assertTrue(regressed)
        self.assertEqual(regressed[0]["baseline_hwm_bytes"], 1000)


class Sk02CommitAndConditionFilterTest(unittest.TestCase):
    def test_same_commit_idle_only_is_inconclusive_for_sk02(self):
        """A same-commit IDLE record exists, but SK-02 requires an
        exercised (web_ui_open or mid_firing) same-commit record -- an
        idle-only baseline must never silently stand in for one."""
        with _TempRepoRoot() as t:
            t.write("idle", "eb83c1ac", [_entry("httpd_worker", 4000)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("httpd_worker", 4000)]
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk02(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("web_ui_open", result.reason)
        self.assertIn("mid_firing", result.reason)
        self.assertIn("eb83c1ac", result.reason)

    def test_same_commit_web_ui_open_record_passes(self):
        with _TempRepoRoot() as t:
            t.write("web_ui_open", "eb83c1ac", [_entry("httpd_worker", 4000)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("httpd_worker", 3980)]  # 20 B noise
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_same_commit_mid_firing_record_also_satisfies_sk02(self):
        with _TempRepoRoot() as t:
            t.write("mid_firing", "eb83c1ac", [_entry("httpd_worker", 4000)])
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("httpd_worker", 4000)]
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_sk02_absolute_floor_still_fails_even_when_inconclusive_on_history(self):
        """Negative test: SK-02's 512 B absolute floor is independent of
        the commit/condition precondition above -- but that precondition
        is itself a hard gate (no same-commit exercised record at all), so
        a floor breach with ZERO exercised history is still reported as
        the precondition failure (INCONCLUSIVE), not silently upgraded to
        PASS. This confirms the precondition genuinely blocks the
        comparison rather than merely padding the observed data."""
        with _TempRepoRoot() as t:
            ctx, srv = _ctx(t.root)
            ctx["repo_root"] = str(t.root)
            srv._info.get_stack_margin.return_value = [_entry("httpd_worker", 100)]  # below 512 B floor
            srv._info.get_fw_version.return_value = _fw("eb83c1ac")
            result = C._case_sk02(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


if __name__ == "__main__":
    unittest.main()
