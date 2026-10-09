#!/usr/bin/env python3
"""Unit tests for kilnctrl.settings -- the atomic-write and
skip-if-unchanged behaviour of save()/_update().

Run with: python -m pytest tools/PcTools/tests/test_settings.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import settings  # noqa: E402


class SettingsAtomicWriteTest(unittest.TestCase):
    def setUp(self):
        fd, self._path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        os.remove(self._path)
        self.path = Path(self._path)
        self.addCleanup(lambda: os.path.exists(self._path) and os.remove(self._path))

    def test_save_then_load_round_trips(self):
        settings.save({"last_port": "COM6", "keep_logs": 5}, self.path)
        self.assertEqual(settings.load(self.path), {"last_port": "COM6", "keep_logs": 5})

    def test_save_writes_via_a_tempfile_and_replaces_atomically(self):
        """No stray .tmp file should survive a successful save, and no
        other file in the directory should be disturbed."""
        settings.save({"a": 1}, self.path)
        siblings = os.listdir(self.path.parent)
        leftover_tmp = [n for n in siblings if n.startswith(f".{self.path.name}.") and n.endswith(".tmp")]
        self.assertEqual(leftover_tmp, [])
        self.assertTrue(self.path.exists())

    def test_a_failed_write_never_truncates_the_existing_file(self):
        """The bug this guards against: a plain truncate-then-write leaves
        a window where a reader sees a partial/empty file and silently
        treats it as {} -- dropping every other key already on disk. Fake
        a failure partway through json.dump and confirm the ORIGINAL file
        (with its original content) is untouched."""
        settings.save({"last_port": "COM6", "openocd_exe": "C:/oo.exe"}, self.path)
        original_bytes = self.path.read_bytes()

        with unittest.mock.patch("kilnctrl.settings.json.dump", side_effect=ValueError("boom")):
            with self.assertRaises(ValueError):
                # save() only swallows OSError, not an error inside json.dump
                # itself -- that's intentional (a programming-error TypeError
                # from unserializable data should not look like a quiet I/O
                # failure), but the ORIGINAL file must still be intact.
                settings.save({"last_port": "COM9"}, self.path)

        self.assertEqual(self.path.read_bytes(), original_bytes)
        self.assertEqual(settings.load(self.path),
                          {"last_port": "COM6", "openocd_exe": "C:/oo.exe"})
        # no leftover tempfile either
        siblings = os.listdir(self.path.parent)
        leftover_tmp = [n for n in siblings if n.startswith(f".{self.path.name}.") and n.endswith(".tmp")]
        self.assertEqual(leftover_tmp, [])

    def test_concurrent_readers_never_see_a_partial_file(self):
        """os.replace is atomic: a reader opening the path at any instant
        sees either the fully-old or fully-new content, never a truncated
        in-between state. Simulate this directly by asserting the file's
        bytes are always valid JSON immediately before and after save()."""
        settings.save({"k": "v1"}, self.path)
        self.assertTrue(json.loads(self.path.read_text(encoding="utf-8")))
        settings.save({"k": "v2", "other": "kept"}, self.path)
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")),
                          {"k": "v2", "other": "kept"})


class SettingsSkipUnchangedTest(unittest.TestCase):
    def setUp(self):
        fd, self._path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        os.remove(self._path)
        self.path = Path(self._path)
        self.addCleanup(lambda: os.path.exists(self._path) and os.remove(self._path))

    def test_update_skips_the_write_when_value_is_unchanged(self):
        settings.set_last_port("COM6", self.path)
        mtime_before = self.path.stat().st_mtime_ns
        with unittest.mock.patch.object(settings, "save") as mock_save:
            settings.set_last_port("COM6", self.path)  # same value again
        mock_save.assert_not_called()
        self.assertEqual(self.path.stat().st_mtime_ns, mtime_before)

    def test_update_still_writes_when_value_changes(self):
        settings.set_last_port("COM6", self.path)
        with unittest.mock.patch.object(settings, "save") as mock_save:
            settings.set_last_port("COM9", self.path)
        mock_save.assert_called_once()

    def test_update_writes_on_first_use_even_though_key_is_absent(self):
        with unittest.mock.patch.object(settings, "save") as mock_save:
            settings.set_last_port("COM6", self.path)
        mock_save.assert_called_once()


if __name__ == "__main__":
    unittest.main()
