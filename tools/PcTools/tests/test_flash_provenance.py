#!/usr/bin/env python3
"""Unit tests for flash_provenance.py -- capture of what was actually dirty
at flash time, and the sensitive-dirty-file guard added after the 2026-09-04
incident (an agent flashing for an unrelated diagnosis carried another
session's in-progress zones_config schema-migration edits onto the board).

All against synthetic `git status --porcelain` output -- no real repo state,
no board, no OpenOCD.

Run with: python -m pytest tools/PcTools/tests/test_flash_provenance.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import flash_provenance as fp  # noqa: E402


class ClassifyDirtyTest(unittest.TestCase):
    def test_clean_tree_has_no_sensitive_files(self):
        self.assertEqual(fp.classify_dirty([]), [])

    def test_benign_dirty_files_are_not_flagged(self):
        files = [
            "firmware/KilnFW/App/drivers/ui_page_home.c",
            "docs/PROJECT_STATUS.md",
            "tools/PcTools/tests/test_pid.py",
        ]
        self.assertEqual(fp.classify_dirty(files), [])

    def test_zones_config_files_are_flagged_sensitive(self):
        """The exact incident: zones_config_* schema/migration edits dirty
        in the tree while an unrelated flash goes out."""
        files = [
            "firmware/KilnFW/App/drivers/zones_config_migrate.c",
            "firmware/KilnFW/App/drivers/zones_config_accessors.c",
            "firmware/KilnFW/App/drivers/ui_page_home.c",  # benign, mixed in
        ]
        sensitive = fp.classify_dirty(files)
        self.assertIn("firmware/KilnFW/App/drivers/zones_config_migrate.c", sensitive)
        self.assertIn("firmware/KilnFW/App/drivers/zones_config_accessors.c", sensitive)
        self.assertNotIn("firmware/KilnFW/App/drivers/ui_page_home.c", sensitive)

    def test_safety_and_cfg_store_files_are_flagged_sensitive(self):
        files = [
            "firmware/KilnFW/App/drivers/safety_cfg_store.c",
            "firmware/KilnFW/App/drivers/kiln_cfg_store.c",
        ]
        self.assertEqual(sorted(fp.classify_dirty(files)), sorted(files))


class DecideGuardTest(unittest.TestCase):
    def _state(self, dirty, sensitive):
        return fp.TreeState(timestamp=0.0, head="abc1234", dirty_files=dirty, sensitive_files=sensitive)

    def test_clean_tree_never_refused(self):
        state = self._state([], [])
        self.assertIsNone(fp.decide_guard(state))

    def test_dirty_but_benign_tree_not_refused(self):
        """Normal state of this repo -- must NOT be blocked, or the guard
        gets disabled within a day."""
        dirty = ["firmware/KilnFW/App/drivers/ui_page_home.c"]
        state = self._state(dirty, [])
        self.assertIsNone(fp.decide_guard(state))

    def test_sensitive_dirty_file_is_refused_by_default(self):
        dirty = ["firmware/KilnFW/App/drivers/zones_config_migrate.c"]
        state = self._state(dirty, dirty)
        reason = fp.decide_guard(state)
        self.assertIsNotNone(reason)
        self.assertIn("zones_config_migrate.c", reason)
        self.assertIn("allow_sensitive_dirty", reason)

    def test_sensitive_dirty_file_allowed_with_explicit_opt_in(self):
        dirty = ["firmware/KilnFW/App/drivers/zones_config_migrate.c"]
        state = self._state(dirty, dirty)
        self.assertIsNone(fp.decide_guard(state, allow_sensitive_dirty=True))

    def test_incident_scenario_mixed_dirty_set_is_refused_and_names_only_sensitive(self):
        """Reproduces the actual incident tree: an unrelated display-path
        edit alongside another session's zones_config schema work. The
        guard must refuse, and must name the schema files specifically."""
        dirty = [
            "firmware/KilnFW/App/drivers/zones_config_migrate.c",
            "firmware/KilnFW/App/drivers/zones_config_accessors.c",
            "firmware/KilnFW/App/drivers/ui_page_home.c",
        ]
        sensitive = fp.classify_dirty(dirty)
        state = self._state(dirty, sensitive)
        reason = fp.decide_guard(state)
        self.assertIsNotNone(reason)
        self.assertIn("zones_config_migrate.c", reason)
        self.assertIn("zones_config_accessors.c", reason)
        self.assertNotIn("ui_page_home.c", reason)


class FormatReportTest(unittest.TestCase):
    def test_clean_tree_report(self):
        state = fp.TreeState(timestamp=0.0, head="abc1234", dirty_files=[], sensitive_files=[])
        report = fp.format_report(state)
        self.assertIn("abc1234", report)
        self.assertIn("clean", report)

    def test_dirty_report_marks_sensitive_files(self):
        dirty = ["a/b.c", "firmware/KilnFW/App/drivers/zones_config_migrate.c"]
        sensitive = ["firmware/KilnFW/App/drivers/zones_config_migrate.c"]
        state = fp.TreeState(timestamp=0.0, head="abc1234", dirty_files=dirty, sensitive_files=sensitive)
        report = fp.format_report(state)
        self.assertIn("a/b.c", report)
        self.assertIn("zones_config_migrate.c [SENSITIVE]", report)

    def test_git_unavailable_reports_warning_not_crash(self):
        state = fp.TreeState(timestamp=0.0, head=None, git_available=False)
        report = fp.format_report(state)
        self.assertIn("WARNING", report)


class ParsePorcelainTest(unittest.TestCase):
    def test_parses_modified_and_untracked_and_renamed(self):
        raw = (
            " M firmware/KilnFW/App/drivers/zones_config_migrate.c\n"
            "?? firmware/KilnFW/App/test/test_zone_sweep_relay_off_wiring.c\n"
            "R  old_name.c -> new_name.c\n"
        )
        files = fp._parse_porcelain(raw)
        self.assertIn("firmware/KilnFW/App/drivers/zones_config_migrate.c", files)
        self.assertIn("firmware/KilnFW/App/test/test_zone_sweep_relay_off_wiring.c", files)
        self.assertIn("new_name.c", files)
        self.assertNotIn("old_name.c", files)


class WriteProvenanceJsonTest(unittest.TestCase):
    def test_writes_readable_json(self):
        dirty = ["firmware/KilnFW/App/drivers/zones_config_migrate.c"]
        state = fp.TreeState(timestamp=123.0, head="abc1234", dirty_files=dirty, sensitive_files=dirty)
        with tempfile.TemporaryDirectory() as d:
            out_path = os.path.join(d, "nested", "flash_provenance.json")
            fp.write_provenance_json(state, out_path)
            with open(out_path, "r", encoding="utf-8") as f:
                data = json.load(f)
        self.assertEqual(data["head"], "abc1234")
        self.assertEqual(data["dirty_files"], dirty)
        self.assertEqual(data["sensitive_files"], dirty)

    def test_write_failure_does_not_raise(self):
        state = fp.TreeState(timestamp=123.0, head="abc1234")
        # A path with a null byte is invalid on every platform and can't be
        # created as a directory -- this must be swallowed, not raised.
        bad_path = os.path.join(tempfile.gettempdir(), "no\0pe", "flash_provenance.json")
        try:
            fp.write_provenance_json(state, bad_path)
        except Exception as exc:  # noqa: BLE001
            self.fail(f"write_provenance_json raised on a bad path: {exc}")

    def test_outcome_and_detail_are_persisted(self):
        # 2026-09-04 fix: a refused flash used to write the exact same JSON
        # shape as a successful one -- nothing on disk said "this didn't
        # happen". outcome/detail are the fields that make the two
        # distinguishable after the fact, from a different session.
        state = fp.TreeState(timestamp=123.0, head="abc1234", dirty_files=["x"], sensitive_files=["x"])
        with tempfile.TemporaryDirectory() as d:
            out_path = os.path.join(d, "flash_provenance.json")
            fp.write_provenance_json(
                state, out_path, outcome=fp.OUTCOME_REFUSED_SENSITIVE_DIRTY, detail="refusing: x"
            )
            with open(out_path, "r", encoding="utf-8") as f:
                data = json.load(f)
        self.assertEqual(data["outcome"], fp.OUTCOME_REFUSED_SENSITIVE_DIRTY)
        self.assertEqual(data["detail"], "refusing: x")

    def test_outcome_defaults_to_pending(self):
        state = fp.TreeState(timestamp=123.0, head="abc1234")
        with tempfile.TemporaryDirectory() as d:
            out_path = os.path.join(d, "flash_provenance.json")
            fp.write_provenance_json(state, out_path)
            with open(out_path, "r", encoding="utf-8") as f:
                data = json.load(f)
        self.assertEqual(data["outcome"], fp.OUTCOME_PENDING)
        self.assertIsNone(data["detail"])


class ReadProvenanceJsonTest(unittest.TestCase):
    def test_missing_file_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(fp.read_provenance_json(os.path.join(d, "nope.json")))

    def test_corrupt_json_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "flash_provenance.json")
            with open(path, "w", encoding="utf-8") as f:
                f.write("{not valid json")
            self.assertIsNone(fp.read_provenance_json(path))

    def test_round_trips_a_written_record(self):
        state = fp.TreeState(timestamp=123.0, head="abc1234", dirty_files=["x"], sensitive_files=["x"])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "flash_provenance.json")
            fp.write_provenance_json(state, path, outcome=fp.OUTCOME_FLASHED_OK)
            data = fp.read_provenance_json(path)
        self.assertEqual(data["outcome"], fp.OUTCOME_FLASHED_OK)
        self.assertEqual(data["head"], "abc1234")


class FormatLastFlashWarningTest(unittest.TestCase):
    """MANDATORY negative test: prove a refused record fires the warning,
    and prove a clean/successful record does not -- both against synthetic
    data, no board, no real flash."""

    def test_none_input_is_silent(self):
        self.assertIsNone(fp.format_last_flash_warning(None))

    def test_refused_sensitive_dirty_fires_loudly(self):
        prov = {
            "timestamp": 1000.0,
            "head": "51e1ef5",
            "outcome": fp.OUTCOME_REFUSED_SENSITIVE_DIRTY,
            "detail": "refusing: zones_config_migrate.c",
        }
        warning = fp.format_last_flash_warning(prov)
        self.assertIsNotNone(warning)
        self.assertIn("REFUSED", warning)
        self.assertIn("51e1ef5", warning)
        self.assertIn("zones_config_migrate.c", warning)

    def test_refused_stale_binary_fires_loudly(self):
        prov = {"timestamp": 1000.0, "head": "abc", "outcome": fp.OUTCOME_REFUSED_STALE_BINARY, "detail": "stale"}
        self.assertIsNotNone(fp.format_last_flash_warning(prov))

    def test_flashed_ok_is_silent(self):
        prov = {"timestamp": 1000.0, "head": "abc1234", "outcome": fp.OUTCOME_FLASHED_OK, "detail": None}
        self.assertIsNone(fp.format_last_flash_warning(prov))

    def test_flash_failed_is_silent(self):
        # A failed (not refused) flash attempt is already loud in its own
        # return string / session log at the time -- this warning exists
        # specifically for refusals, which used to leave no trace at all.
        prov = {"timestamp": 1000.0, "head": "abc1234", "outcome": fp.OUTCOME_FLASH_FAILED, "detail": "openocd error"}
        self.assertIsNone(fp.format_last_flash_warning(prov))

    def test_pending_is_silent(self):
        prov = {"timestamp": 1000.0, "head": "abc1234", "outcome": fp.OUTCOME_PENDING, "detail": None}
        self.assertIsNone(fp.format_last_flash_warning(prov))

    def test_missing_outcome_key_is_silent(self):
        # Older provenance files written before this field existed.
        prov = {"timestamp": 1000.0, "head": "abc1234"}
        self.assertIsNone(fp.format_last_flash_warning(prov))


class DescribeHeadGapTest(unittest.TestCase):
    """Uses THIS repo's real git history (no board) since describe_head_gap
    shells out to git -- still synthetic in the sense that no board or
    flash is involved, just commit-graph arithmetic against known refs."""

    def test_no_board_commit_reported(self):
        self.assertIn("did not report a commit", fp.describe_head_gap(None))
        self.assertIn("did not report a commit", fp.describe_head_gap(""))

    def test_board_matches_head(self):
        head = fp._git(["rev-parse", "--short", "HEAD"], fp._repo_root())
        self.assertIsNotNone(head, "this test requires a real git checkout")
        head = head.strip()
        result = fp.describe_head_gap(head)
        self.assertIn("OK", result)
        self.assertIn(head, result)

    def test_board_behind_head_is_loud(self):
        head = fp._git(["rev-parse", "--short", "HEAD"], fp._repo_root())
        older = fp._git(["rev-parse", "--short", "HEAD~3"], fp._repo_root())
        self.assertIsNotNone(head, "this test requires a real git checkout")
        self.assertIsNotNone(older, "this test requires at least 3 commits of history")
        result = fp.describe_head_gap(older.strip())
        self.assertIn("behind HEAD", result)
        self.assertIn("!!!", result)

    def test_unknown_commit_degrades_gracefully(self):
        # Not in this clone's history at all -- must report uncertainty,
        # not raise and not falsely claim a specific commit count.
        result = fp.describe_head_gap("deadbee")
        self.assertNotIn("!!!", result)
        self.assertIn("could not determine", result)

    def test_unavailable_repo_root_does_not_raise(self):
        with tempfile.TemporaryDirectory() as d:
            # Not a git repo at all.
            try:
                result = fp.describe_head_gap("abc1234", repo_root=d)
            except Exception as exc:  # noqa: BLE001
                self.fail(f"describe_head_gap raised outside a git repo: {exc}")
            self.assertIn("could not read local HEAD", result)


if __name__ == "__main__":
    unittest.main()
