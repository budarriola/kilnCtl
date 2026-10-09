#!/usr/bin/env python3
"""Unit tests for mcp_server_info._read_last_flash_warning() -- get_fw_version's
last-flash-outcome reader.

L3 (docs/audits/review_elf_archive_fixes_a6f4a624_2026-09-15.md): the move of
flash_provenance.json out of build/ (M1 of the a6f4a624 fix pass) made
get_fw_version() read ONLY the new canonical path -- silently dropping the
last-flash warning for a board whose most recent recorded outcome (e.g. a
refused sensitive-dirty flash) was still sitting at the OLD
KilnFW/build/flash_provenance.json location. These tests cover: reading the
new path when present, falling back to the legacy path when the new one is
absent, and migrating the legacy file into place as a side effect.

All against a temp directory standing in for firmware/KilnFW/ -- no real
repo state, no board.

Run with: python -m pytest tools/PcTools/tests/test_get_fw_version_provenance.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import elf_archive, mcp_server_info  # noqa: E402


def _write_json(path: str, data: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)


class ReadLastFlashWarningTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        self.new_path = os.path.join(self.root, "firmware", "KilnFW", "flash_provenance.json")
        self.legacy_path = os.path.join(self.root, "firmware", "KilnFW", "build", "flash_provenance.json")
        self._patches = [
            unittest.mock.patch.object(elf_archive, "kiln_provenance_path", return_value=self.new_path),
            unittest.mock.patch.object(elf_archive, "legacy_kiln_provenance_path", return_value=self.legacy_path),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)

    def _refused_provenance(self):
        return {
            "outcome": "refused_sensitive_dirty",
            "detail": "zones_config_json.c is dirty",
            "commit": "deadbeef",
            "dirty": True,
            "sensitive_files": ["firmware/KilnFW/App/drivers/persist/zones_config_json.c"],
            "captured_at": "2026-09-04T12:00:00Z",
        }

    def test_reads_the_new_canonical_path_when_present(self):
        _write_json(self.new_path, self._refused_provenance())

        warning = mcp_server_info._read_last_flash_warning()

        self.assertIsNotNone(warning)
        self.assertIn("refused_sensitive_dirty", warning)

    def test_falls_back_to_the_legacy_path_when_the_new_one_is_absent_and_cannot_be_migrated(self):
        _write_json(self.legacy_path, self._refused_provenance())
        # Simulate migration being unavailable (e.g. guarded) by making it a no-op.
        with unittest.mock.patch.object(elf_archive, "migrate_legacy_provenance", return_value=False):
            warning = mcp_server_info._read_last_flash_warning()

        self.assertIsNotNone(warning)
        self.assertIn("refused_sensitive_dirty", warning)

    def test_migrates_the_legacy_file_into_the_new_location_as_a_side_effect(self):
        _write_json(self.legacy_path, self._refused_provenance())

        warning = mcp_server_info._read_last_flash_warning()

        self.assertIsNotNone(warning)
        self.assertTrue(os.path.isfile(self.new_path))
        self.assertFalse(os.path.isfile(self.legacy_path))

    def test_no_provenance_anywhere_returns_no_warning(self):
        warning = mcp_server_info._read_last_flash_warning()
        self.assertIsNone(warning)

    def test_negative_without_the_legacy_fallback_the_warning_is_silently_dropped(self):
        """Proves the fallback test above is genuinely negative-testable:
        reproduce the OLD (pre-L3) behavior directly -- read ONLY the new
        path, ignoring the legacy one entirely."""
        _write_json(self.legacy_path, self._refused_provenance())
        from kilnctrl import flash_provenance
        # The old code's exact call: read_provenance_json(kiln_provenance_path())
        prov = flash_provenance.read_provenance_json(elf_archive.kiln_provenance_path())
        self.assertIsNone(prov)  # the refusal record is invisible without the fallback


if __name__ == "__main__":
    unittest.main()
