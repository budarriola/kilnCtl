#!/usr/bin/env python3
"""Unit tests for elf_archive.py -- archiving the exact ELF a flash actually
sent to a board, keyed so it can be found later from what the board reports,
and proving a lookup with no match fails loudly instead of guessing.

All against a temp directory standing in for firmware/KilnFW|SaftyFW/build/ --
no real repo state, no board, no OpenOCD, no idf.py/cmake build.

Run with: python -m pytest tools/PcTools/tests/test_elf_archive.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import elf_archive  # noqa: E402


def _write_fake_elf(path: str, content: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(content)


class ArchiveKilnElfTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "elf_archive")
        # patch the module's directory resolver rather than touching the real repo
        self._orig = elf_archive.kiln_archive_dir
        elf_archive.kiln_archive_dir = lambda: self.archive_dir

    def tearDown(self):
        elf_archive.kiln_archive_dir = self._orig
        self._tmp.cleanup()

    def test_archive_then_find_round_trip(self):
        elf_path = os.path.join(self._tmp.name, "KilnCtrl.elf")
        _write_fake_elf(elf_path, b"fake elf bytes v1")
        result = elf_archive.archive_kiln_elf(elf_path, "Sep 10 2026 12:00:00", "0dddd435", "test")
        self.assertTrue(result.newly_archived)
        self.assertTrue(os.path.isfile(result.archived_path))

        path, message = elf_archive.find_kiln_elf_for_build("Sep 10 2026 12:00:00")
        self.assertEqual(path, result.archived_path)
        self.assertIn("found", message)

    def test_lookup_tolerates_single_digit_day_padding_drift(self):
        # 2026-09-10 (opus review round 2, defect D): elf_archive.py used to
        # key/look up the manifest on the RAW fw_build string, unnormalized --
        # but ESP-IDF's __DATE__ double-pads single-digit days ("Sep  3
        # 2026"), and that padding is easy to gain or lose passing through
        # JSON/logging (esp_app_desc.build_timestamps_match() already
        # tolerates exactly this). Archive with single-space padding, look up
        # with double-space padding (as if the day were single-digit and the
        # caller's copy of the string lost/gained a space) -- must still find
        # it, not report "no archived ELF found" for a build that IS archived.
        elf_path = os.path.join(self._tmp.name, "KilnCtrl.elf")
        _write_fake_elf(elf_path, b"fake elf bytes v1")
        result = elf_archive.archive_kiln_elf(elf_path, "Sep  3 2026 20:13:41", "0dddd435", "test")

        path, message = elf_archive.find_kiln_elf_for_build("Sep 3 2026 20:13:41")
        self.assertEqual(path, result.archived_path,
                          "single- vs double-space day padding must not change the lookup result")
        self.assertIn("found", message)

        # And the reverse direction: archived with single-space, looked up
        # with the double-space form.
        path2, message2 = elf_archive.find_kiln_elf_for_build("Sep  3 2026 20:13:41")
        self.assertEqual(path2, result.archived_path)
        self.assertIn("found", message2)

    def test_lookup_with_no_match_fails_loudly(self):
        elf_path = os.path.join(self._tmp.name, "KilnCtrl.elf")
        _write_fake_elf(elf_path, b"fake elf bytes v1")
        elf_archive.archive_kiln_elf(elf_path, "Sep 10 2026 12:00:00", "0dddd435", "test")

        path, message = elf_archive.find_kiln_elf_for_build("Sep 09 2026 08:00:00")
        self.assertIsNone(path)
        self.assertIn("no archived ELF found", message)
        self.assertIn("Sep 09 2026 08:00:00", message)

    def test_lookup_never_falls_back_to_latest_or_newest(self):
        """Two different builds archived; asking for a build that was never
        archived must not silently return either of them."""
        elf_a = os.path.join(self._tmp.name, "a.elf")
        elf_b = os.path.join(self._tmp.name, "b.elf")
        _write_fake_elf(elf_a, b"content A")
        _write_fake_elf(elf_b, b"content B, different length")
        elf_archive.archive_kiln_elf(elf_a, "Sep 08 2026 01:00:00", "aaa111", "test")
        result_b = elf_archive.archive_kiln_elf(elf_b, "Sep 09 2026 02:00:00", "bbb222", "test")

        path, message = elf_archive.find_kiln_elf_for_build("Sep 10 2026 03:00:00")
        self.assertIsNone(path)
        self.assertNotEqual(path, result_b.archived_path)

    def test_identical_content_collapses_to_one_file(self):
        """Rebuilding to the exact same bytes must not accumulate duplicate
        archive entries -- see elf_archive.py's module docstring."""
        elf_path = os.path.join(self._tmp.name, "KilnCtrl.elf")
        _write_fake_elf(elf_path, b"identical content")
        r1 = elf_archive.archive_kiln_elf(elf_path, "Sep 10 2026 12:00:00", "commit1", "test")
        r2 = elf_archive.archive_kiln_elf(elf_path, "Sep 10 2026 12:05:00", "commit1", "test")
        self.assertTrue(r1.newly_archived)
        self.assertFalse(r2.newly_archived)
        self.assertEqual(r1.elf_key, r2.elf_key)
        # both build-timestamp identities should resolve, since manifest keys
        # by identity, not by content
        self.assertIsNotNone(elf_archive.find_kiln_elf_for_build("Sep 10 2026 12:00:00")[0])
        self.assertIsNotNone(elf_archive.find_kiln_elf_for_build("Sep 10 2026 12:05:00")[0])

    def test_manifest_entry_missing_file_reports_error_not_none_silently(self):
        elf_path = os.path.join(self._tmp.name, "KilnCtrl.elf")
        _write_fake_elf(elf_path, b"soon to be deleted")
        result = elf_archive.archive_kiln_elf(elf_path, "Sep 10 2026 12:00:00", "c1", "test")
        os.remove(result.archived_path)  # simulate manual/partial deletion

        path, message = elf_archive.find_kiln_elf_for_build("Sep 10 2026 12:00:00")
        self.assertIsNone(path)
        self.assertIn("missing on disk", message)


class PruneRetentionTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "elf_archive")
        self._orig = elf_archive.kiln_archive_dir
        elf_archive.kiln_archive_dir = lambda: self.archive_dir
        self._orig_max = elf_archive.MAX_ARCHIVED_ELFS
        self._orig_keep_recent = elf_archive.KEEP_RECENT_ENTRIES
        elf_archive.MAX_ARCHIVED_ELFS = 3
        elf_archive.KEEP_RECENT_ENTRIES = 2

    def tearDown(self):
        elf_archive.kiln_archive_dir = self._orig
        elf_archive.MAX_ARCHIVED_ELFS = self._orig_max
        elf_archive.KEEP_RECENT_ENTRIES = self._orig_keep_recent
        self._tmp.cleanup()

    def test_archive_is_capped_and_recent_entries_survive(self):
        n = 8
        results = []
        for i in range(n):
            elf_path = os.path.join(self._tmp.name, f"build{i}.elf")
            _write_fake_elf(elf_path, f"distinct content #{i}".encode())
            results.append(
                elf_archive.archive_kiln_elf(elf_path, f"build-{i}", f"commit{i}", "test")
            )

        remaining = [
            name for name in os.listdir(self.archive_dir)
            if name.startswith("KilnCtrl-") and name.endswith(".elf")
            and name != "KilnCtrl-latest.elf"
        ]
        self.assertLessEqual(len(remaining), elf_archive.MAX_ARCHIVED_ELFS)

        # the most recently archived entries must not have been pruned away
        last_two = results[-elf_archive.KEEP_RECENT_ENTRIES:]
        for r in last_two:
            self.assertTrue(os.path.isfile(r.archived_path), f"{r.archived_path} should survive pruning")


class ArchiveSaftyElfTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "safty_elf_archive")
        self._orig = elf_archive.safty_archive_dir
        elf_archive.safty_archive_dir = lambda: self.archive_dir

    def tearDown(self):
        elf_archive.safty_archive_dir = self._orig
        self._tmp.cleanup()

    def _fake_safty_fw_root(self, commit: str, date: str, time_: str) -> str:
        root = os.path.join(self._tmp.name, "SaftyFW")
        header = os.path.join(root, "build", "saftyfw_build_info.h")
        os.makedirs(os.path.dirname(header), exist_ok=True)
        with open(header, "w", encoding="utf-8") as f:
            f.write(
                "#ifndef SAFTYFW_BUILD_INFO_H\n#define SAFTYFW_BUILD_INFO_H\n"
                f'#define SAFTYFW_GIT_COMMIT "{commit}"\n'
                "#define SAFTYFW_GIT_DIRTY 0\n"
                f'#define SAFTYFW_BUILD_DATE "{date}"\n'
                f'#define SAFTYFW_BUILD_TIME "{time_}"\n'
                "#endif\n"
            )
        elf_path = os.path.join(root, "build", "SaftyFW.elf")
        _write_fake_elf(elf_path, f"safty elf for {commit}".encode())
        return root, elf_path

    def test_archive_then_find_by_exact_identity(self):
        root, elf_path = self._fake_safty_fw_root("8f53d16c", "2026-09-10", "18:40:03Z")
        result = elf_archive.archive_safty_elf(elf_path, root, "test")
        self.assertTrue(os.path.isfile(result.archived_path))

        path, message = elf_archive.find_safty_elf_for_identity("8f53d16c", "2026-09-10", "18:40:03Z")
        self.assertEqual(path, result.archived_path)

    def test_find_by_commit_only_when_unambiguous(self):
        root, elf_path = self._fake_safty_fw_root("cafefeed", "2026-09-10", "09:00:00Z")
        elf_archive.archive_safty_elf(elf_path, root, "test")

        path, message = elf_archive.find_safty_elf_for_identity("cafefeed")
        self.assertIsNotNone(path)

    def test_ambiguous_commit_with_multiple_builds_is_refused(self):
        root1, elf1 = self._fake_safty_fw_root("dupe1234", "2026-09-10", "09:00:00Z")
        elf_archive.archive_safty_elf(elf1, root1, "test")
        # second build, same commit (rebuilt without recommitting), different content/time
        header = os.path.join(root1, "build", "saftyfw_build_info.h")
        with open(header, "w", encoding="utf-8") as f:
            f.write(
                "#ifndef X\n#define X\n"
                '#define SAFTYFW_GIT_COMMIT "dupe1234"\n'
                "#define SAFTYFW_GIT_DIRTY 0\n"
                '#define SAFTYFW_BUILD_DATE "2026-09-10"\n'
                '#define SAFTYFW_BUILD_TIME "10:00:00Z"\n'
                "#endif\n"
            )
        _write_fake_elf(elf1, b"different rebuild content")
        elf_archive.archive_safty_elf(elf1, root1, "test")

        path, message = elf_archive.find_safty_elf_for_identity("dupe1234")
        self.assertIsNone(path)
        self.assertIn("matches", message)

    def test_no_match_fails_loudly(self):
        path, message = elf_archive.find_safty_elf_for_identity("neverseen00")
        self.assertIsNone(path)
        self.assertIn("no archived ELF found", message)


class CanonicalArchiveWriteGuardTest(unittest.TestCase):
    """2026-09-10: test_flash_board_pinning.py and test_flash_firmware_verify.py
    drove flash_firmware() end-to-end without mocking elf_archive, so
    archive_kiln_elf() ran for real against the CANONICAL (main-tree) archive
    and overwrote a genuine manifest entry with a fabricated commit
    ('abc1234', a test fixture string) -- exactly the failure mode the
    archive exists to prevent (a later panic symbolized against a mislabeled
    ELF). Fixed structurally: _guard_against_test_write() refuses any write
    to the two real archive directories whenever pytest is running, unless
    the caller explicitly monkeypatched kiln_archive_dir()/safty_archive_dir()
    (as every well-behaved test here does) or set
    KILNCTL_ALLOW_TEST_ARCHIVE_WRITE=1. This class proves that guard fires
    for real, not just when told to."""

    MARKER_IDENTITY = "REGRESSION-TEST-MARKER-DO-NOT-TRUST"

    def tearDown(self):
        # Belt-and-suspenders cleanup in case the guard is ever broken and
        # this test actually reaches disk: never leave a marker entry behind
        # in the real archive.
        manifest = elf_archive._load_manifest(elf_archive.kiln_archive_dir())
        entry = manifest.pop(self.MARKER_IDENTITY, None)
        if entry is not None:
            elf_path = os.path.join(elf_archive.kiln_archive_dir(), f"KilnCtrl-{entry['elf_key']}.elf")
            try:
                os.remove(elf_path)
            except OSError:
                pass
            elf_archive._write_manifest(elf_archive.kiln_archive_dir(), manifest)

    def test_direct_guard_refuses_the_real_kiln_dir(self):
        with self.assertRaises(RuntimeError) as ctx:
            elf_archive._guard_against_test_write(elf_archive.kiln_archive_dir())
        self.assertIn("CANONICAL", str(ctx.exception))

    def test_direct_guard_refuses_the_real_safty_dir(self):
        with self.assertRaises(RuntimeError):
            elf_archive._guard_against_test_write(elf_archive.safty_archive_dir())

    def test_guard_allows_a_monkeypatched_tmp_dir(self):
        with tempfile.TemporaryDirectory() as d:
            # Must not raise -- this is the well-behaved shape every other
            # test class in this file uses.
            elf_archive._guard_against_test_write(d)

    def test_archive_kiln_elf_against_the_real_dir_is_refused_end_to_end(self):
        """The production entry point, not just the guard helper: calling
        archive_kiln_elf() WITHOUT monkeypatching kiln_archive_dir() (the
        exact mistake test_flash_board_pinning.py made) must raise before
        touching disk."""
        with tempfile.TemporaryDirectory() as d:
            elf_path = os.path.join(d, "KilnCtrl.elf")
            _write_fake_elf(elf_path, b"regression-test-fake-elf-content")
            with self.assertRaises(RuntimeError):
                elf_archive.archive_kiln_elf(elf_path, self.MARKER_IDENTITY, "deadbeef", "test")
        # Confirm nothing was written to the real manifest.
        manifest = elf_archive._load_manifest(elf_archive.kiln_archive_dir())
        self.assertNotIn(self.MARKER_IDENTITY, manifest)

    def test_negative_removing_the_guard_reproduces_the_incident(self):
        """Proves the guard test above is not vacuous: with
        _guard_against_test_write patched to a no-op (simulating the
        pre-fix code, which had no such call at all), the same call that
        was just refused instead SUCCEEDS and writes into the real
        canonical archive -- reproducing the exact contamination this
        module's docstring and CLAUDE.md describe. Cleaned up in tearDown."""
        with tempfile.TemporaryDirectory() as d:
            elf_path = os.path.join(d, "KilnCtrl.elf")
            _write_fake_elf(elf_path, b"regression-test-fake-elf-content-2")
            with unittest.mock.patch.object(elf_archive, "_guard_against_test_write", return_value=None):
                result = elf_archive.archive_kiln_elf(elf_path, self.MARKER_IDENTITY, "deadbeef", "test")
        self.assertTrue(os.path.isfile(result.archived_path))
        manifest = elf_archive._load_manifest(elf_archive.kiln_archive_dir())
        self.assertIn(self.MARKER_IDENTITY, manifest)  # ... which is exactly the bug -- cleaned up in tearDown


if __name__ == "__main__":
    unittest.main()
