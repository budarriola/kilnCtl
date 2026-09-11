#!/usr/bin/env python3
"""Unit tests for elf_archive.py -- archiving the exact ELF a flash actually
sent to a board, keyed so it can be found later from what the board reports,
and proving a lookup with no match fails loudly instead of guessing.

All against a temp directory standing in for firmware/KilnFW|SaftyFW/build/ --
no real repo state, no board, no OpenOCD, no idf.py/cmake build.

Run with: python -m pytest tools/PcTools/tests/test_elf_archive.py -q
"""
from __future__ import annotations

import json
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

    def test_preexisting_raw_key_entry_is_reachable_after_normalization_fix(self):
        """2026-09-10 (opus review round 3, defect 1): a manifest entry
        written under its RAW (un-normalized, double-space day padding) key
        -- either by pre-fix code, or by hand-repair as happened for real in
        firmware/KilnFW/build/elf_archive/manifest.json's 'Sep  9 2026
        14:18:51' entry -- must still resolve once normalize_build_timestamp
        is applied consistently. Seed the manifest file directly (bypassing
        archive_kiln_elf, which now always writes normalized keys) to
        reproduce the pre-existing-raw-key situation, then look it up both
        ways."""
        elf_path = os.path.join(self.archive_dir, "KilnCtrl-deadbeef0001.elf")
        _write_fake_elf(elf_path, b"raw-key legacy entry")
        os.makedirs(self.archive_dir, exist_ok=True)
        raw_key = "Sep  9 2026 14:18:51"  # double space before single-digit day
        manifest = {
            raw_key: {
                "elf_key": "deadbeef0001",
                "identity": raw_key,
                "seq": 1,
                "archived_at": "2026-09-09T14:20:00Z",
                "git_commit": "0dddd435",
                "source": "test-seeded-raw-key",
            }
        }
        elf_archive._write_manifest(self.archive_dir, manifest)

        # Before the fix, find_kiln_elf_for_build normalized the lookup key
        # but _load_manifest returned the raw key verbatim, so this exact
        # reproduction (queried with either spacing) returned None.
        path_norm, msg_norm = elf_archive.find_kiln_elf_for_build("Sep 9 2026 14:18:51")
        self.assertIsNotNone(path_norm, msg_norm)
        self.assertTrue(path_norm.endswith("KilnCtrl-deadbeef0001.elf"))

        path_raw, msg_raw = elf_archive.find_kiln_elf_for_build(raw_key)
        self.assertIsNotNone(path_raw, msg_raw)
        self.assertEqual(path_raw, path_norm)

    def test_negative_without_load_time_migration_raw_key_is_unreachable(self):
        """Proves the migration in _load_manifest (not something incidental)
        is what makes the previous test pass: monkeypatch _load_manifest
        back to a raw passthrough (no key normalization -- the pre-fix
        shape) and confirm the identical raw-keyed entry from the previous
        test's setup becomes unreachable again, reproducing the live
        incident this defect describes."""
        elf_path = os.path.join(self.archive_dir, "KilnCtrl-deadbeef0002.elf")
        _write_fake_elf(elf_path, b"raw-key legacy entry 2")
        os.makedirs(self.archive_dir, exist_ok=True)
        raw_key = "Sep  8 2026 09:00:00"
        manifest = {
            raw_key: {
                "elf_key": "deadbeef0002",
                "identity": raw_key,
                "seq": 1,
                "archived_at": "2026-09-08T09:01:00Z",
                "git_commit": "cafefeed",
                "source": "test-seeded-raw-key",
            }
        }
        elf_archive._write_manifest(self.archive_dir, manifest)

        def _raw_passthrough(archive_dir):
            path = os.path.join(archive_dir, elf_archive.MANIFEST_NAME)
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)

        with unittest.mock.patch.object(elf_archive, "_load_manifest", side_effect=_raw_passthrough):
            path, message = elf_archive.find_kiln_elf_for_build("Sep 8 2026 09:00:00")
        self.assertIsNone(path, "without key migration this lookup should miss, reproducing the incident")
        self.assertIn("no archived ELF found", message)


class SupersededIdentityTest(unittest.TestCase):
    """2026-09-10 (opus review round 3, defect 4): firmware/KilnFW/build/
    elf_archive/ held 60 real KilnCtrl-*.elf files against only 3 manifest
    entries -- 57 files simultaneously unreachable by lookup (the manifest
    never named them) and unprotected from _prune (only manifest-referenced
    elf_keys survive pruning). Root cause reproduced here: two DIFFERENT
    ELF contents can legitimately share one fw_build identity string when a
    rebuild doesn't touch the translation unit embedding __DATE__/__TIME__.
    archive_kiln_elf() used to just overwrite manifest[identity], stranding
    the older elf_key. It now records the older entry in a superseded
    registry that both _prune and find_kiln_elf_for_build consult."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "elf_archive")
        self._orig = elf_archive.kiln_archive_dir
        elf_archive.kiln_archive_dir = lambda: self.archive_dir

    def tearDown(self):
        elf_archive.kiln_archive_dir = self._orig
        self._tmp.cleanup()

    def test_superseded_elf_is_recorded_and_protected_from_pruning(self):
        elf_a = os.path.join(self._tmp.name, "a.elf")
        elf_b = os.path.join(self._tmp.name, "b.elf")
        _write_fake_elf(elf_a, b"first build, same fw_build identity")
        _write_fake_elf(elf_b, b"second build, DIFFERENT content, same fw_build identity")

        same_identity = "Sep 8 2026 16:02:17"
        result_a = elf_archive.archive_kiln_elf(elf_a, same_identity, "commitA", "test")
        result_b = elf_archive.archive_kiln_elf(elf_b, same_identity, "commitB", "test")
        self.assertNotEqual(result_a.elf_key, result_b.elf_key)

        # The manifest's live entry for this identity must be the most
        # recently archived one (result_b) -- matches existing "flash just
        # verified" recency semantics.
        path, message = elf_archive.find_kiln_elf_for_build(same_identity)
        self.assertEqual(path, result_b.archived_path)
        # ... and the message must disclose that an older build shares this
        # identity, rather than silently hiding the ambiguity.
        self.assertIn(result_a.elf_key, message)

        superseded = elf_archive._load_superseded(self.archive_dir)
        self.assertIn(same_identity, superseded)
        self.assertEqual(superseded[same_identity][0]["elf_key"], result_a.elf_key)

        # The superseded file must still be ON DISK (not deleted) and must
        # still exist so it can be recovered/inspected by hand.
        self.assertTrue(os.path.isfile(result_a.archived_path))

    def test_negative_without_superseded_tracking_older_elf_is_unprotected(self):
        """Proves the tracking above is load-bearing for pruning, not just
        cosmetic: with archive_dir's superseded file deleted right before a
        prune that must evict something, the previously-superseded elf_key
        is no longer in the protected set and can be deleted -- reproducing
        the exact "unreachable AND unprotected" incident this defect
        describes."""
        orig_max = elf_archive.MAX_ARCHIVED_ELFS
        orig_keep = elf_archive.KEEP_RECENT_ENTRIES
        elf_archive.MAX_ARCHIVED_ELFS = 2
        elf_archive.KEEP_RECENT_ENTRIES = 1
        try:
            elf_a = os.path.join(self._tmp.name, "a.elf")
            elf_b = os.path.join(self._tmp.name, "b.elf")
            _write_fake_elf(elf_a, b"first build, same fw_build identity")
            _write_fake_elf(elf_b, b"second build, DIFFERENT content, same fw_build identity")
            same_identity = "Sep 8 2026 16:02:17"
            result_a = elf_archive.archive_kiln_elf(elf_a, same_identity, "commitA", "test")
            result_b = elf_archive.archive_kiln_elf(elf_b, same_identity, "commitB", "test")
            self.assertTrue(os.path.isfile(result_a.archived_path),
                             "with tracking live, superseded elf_key must survive pruning")

            # Now simulate "without superseded tracking" by wiping the
            # registry right before a third, differently-identified build
            # is archived -- that archive call's own _prune (loading a
            # freshly-empty superseded registry) must evict something to
            # stay at the MAX_ARCHIVED_ELFS=2 cap, and with no tracking the
            # older same-identity build (result_a) is not in anyone's
            # protected set.
            os.remove(os.path.join(self.archive_dir, elf_archive.SUPERSEDED_NAME))
            elf_c = os.path.join(self._tmp.name, "c.elf")
            _write_fake_elf(elf_c, b"third build, different identity entirely")
            elf_archive.archive_kiln_elf(elf_c, "Sep 9 2026 00:00:00", "commitC", "test")

            self.assertFalse(
                os.path.isfile(result_a.archived_path),
                "without superseded protection the older same-identity build gets pruned -- "
                "reproducing the incident",
            )
        finally:
            elf_archive.MAX_ARCHIVED_ELFS = orig_max
            elf_archive.KEEP_RECENT_ENTRIES = orig_keep


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
        """Proves the guard is not vacuous -- WITHOUT touching the real
        canonical archive at all.

        2026-09-10 (opus review round 3, defect 3): the original version of
        this test achieved its proof by actually calling archive_kiln_elf()
        against the real kiln_archive_dir() (only _guard_against_test_write
        patched out), writing a fabricated 'REGRESSION-TEST-MARKER-DO-NOT-
        TRUST' manifest entry and a fake ELF into
        firmware/KilnFW/build/elf_archive/ on every single pytest run,
        cleaned up only in tearDown. Three concrete failure modes followed
        from that: a killed process (429 rate-limit kills are routine in
        this environment) leaves the contamination permanently; tearDown's
        read-modify-write of the manifest races another agent's concurrent
        pytest run in this shared tree; and the marker entry consumed one of
        the KEEP_RECENT_ENTRIES prune-protection slots, nudging a real entry
        closer to eviction. None of that was necessary: the guard's job is
        to refuse a write to the two CANONICAL directories, and that can be
        proven with archive_dir pointed at a monkeypatched tmp dir the whole
        time -- the guard itself decides refuse/allow purely from whether
        its archive_dir argument matches _canonical_archive_dirs(), so
        patching _canonical_archive_dirs() to include the tmp dir exercises
        the exact same branch with zero real-archive side effects.
        """
        with tempfile.TemporaryDirectory() as d:
            fake_canonical_dir = os.path.join(d, "fake_canonical_elf_archive")
            elf_path = os.path.join(d, "KilnCtrl.elf")
            _write_fake_elf(elf_path, b"regression-test-fake-elf-content-2")
            with unittest.mock.patch.object(
                elf_archive, "_canonical_archive_dirs",
                return_value={os.path.normpath(fake_canonical_dir)},
            ), unittest.mock.patch.object(
                elf_archive, "kiln_archive_dir", return_value=fake_canonical_dir,
            ):
                # Sanity: with the guard live and archive_dir now "canonical"
                # (per the patched _canonical_archive_dirs), the call must
                # still be refused -- same as the real canonical dirs.
                with self.assertRaises(RuntimeError):
                    elf_archive.archive_kiln_elf(elf_path, self.MARKER_IDENTITY, "deadbeef", "test")
                self.assertFalse(os.path.isdir(fake_canonical_dir),
                                  "guard must refuse before touching disk at all")

                # Now remove the guard (simulating the pre-fix code, which had
                # no such call) and confirm the SAME call that was just
                # refused instead succeeds -- proving the guard, not
                # something else, was what stood in the way.
                with unittest.mock.patch.object(elf_archive, "_guard_against_test_write", return_value=None):
                    result = elf_archive.archive_kiln_elf(elf_path, self.MARKER_IDENTITY, "deadbeef", "test")
                self.assertTrue(os.path.isfile(result.archived_path))
                manifest = elf_archive._load_manifest(fake_canonical_dir)
                self.assertIn(self.MARKER_IDENTITY, manifest)
        # fake_canonical_dir lived under the TemporaryDirectory the whole
        # time and is gone with it -- nothing to clean up in the real archive.


if __name__ == "__main__":
    unittest.main()
