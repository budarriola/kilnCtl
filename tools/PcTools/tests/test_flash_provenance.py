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


if __name__ == "__main__":
    unittest.main()
