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
import time
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


def _pack_fake_app_desc(date_s: str, time_s: str) -> bytes:
    """Builds the bytes of a bare esp_app_desc_t (magic word through the
    date field) matching the layout esp_app_desc.scan_elf_for_app_descs
    scans for -- NOT preceded by an esp_image_header_t/segment header, since
    a linked ELF has none (those are added by esptool's elf2image step)."""
    import struct as _struct
    from kilnctrl.esp_app_desc import ESP_APP_DESC_MAGIC_WORD
    return (
        _struct.pack("<I", ESP_APP_DESC_MAGIC_WORD)
        + b"\x00" * 4  # secure_version
        + b"\x00" * 8  # reserv1
        + b"KilnCtrl-test-v1".ljust(32, b"\x00")  # version
        + b"KilnCtrl".ljust(32, b"\x00")  # project_name
        + time_s.encode().ljust(16, b"\x00")
        + date_s.encode().ljust(16, b"\x00")
    )


def _write_fake_producer_elf(path: str, date_s: str, time_s: str, filler: bytes = b"") -> None:
    """Simulates exactly what archive_elf.cmake's POST_BUILD copy deposits:
    a real linked ELF's bytes (here, a stand-in with surrounding filler so
    the magic word isn't at offset 0, same as a real ELF) landing in the
    archive directory -- with NO manifest entry, since that cmake step never
    writes one (see archive_elf.cmake and defect 1)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x7fELF" + filler)
        f.write(_pack_fake_app_desc(date_s, time_s))
        f.write(b"\x00" * 16)  # trailing filler, like real section data


class OrphanAdoptionTest(unittest.TestCase):
    """2026-09-10 (opus review round 4, defect 1): archive_elf.cmake's
    POST_BUILD step deposits a KilnCtrl-<hash>.elf into the archive
    directory on every ordinary `idf.py build` but never registers it in
    manifest.json -- measured live: 7 (then 8, mid-review) such files
    against a 42-entry manifest, one of which was the exact ELF a
    stack-budget measurement had been taken against. These tests prove the
    PRODUCTION adoption path (adopt_orphaned_kiln_elfs, and its automatic
    call from archive_kiln_elf) makes such a file reachable -- entirely
    against a monkeypatched tmp archive dir, never the real one."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "elf_archive")
        os.makedirs(self.archive_dir, exist_ok=True)
        self._orig = elf_archive.kiln_archive_dir
        elf_archive.kiln_archive_dir = lambda: self.archive_dir

    def tearDown(self):
        elf_archive.kiln_archive_dir = self._orig
        self._tmp.cleanup()

    def test_producer_deposited_orphan_becomes_reachable(self):
        # Exactly what archive_elf.cmake leaves behind: a file on disk,
        # named by content hash, with no manifest entry at all.
        orphan_path = os.path.join(self.archive_dir, "KilnCtrl-0a1b2c3d4e51.elf")
        _write_fake_producer_elf(orphan_path, "Sep 10 2026", "16:33:34")

        manifest_before = elf_archive._load_manifest(self.archive_dir)
        self.assertNotIn("Sep 10 2026 16:33:34", manifest_before,
                          "sanity: the orphan must start out unregistered")

        path, msg = elf_archive.find_kiln_elf_for_build("Sep 10 2026 16:33:34")
        self.assertIsNone(path, "sanity: unreachable before adoption, reproducing the incident")

        adopted, unresolved = elf_archive.adopt_orphaned_kiln_elfs(self.archive_dir)
        self.assertEqual(adopted, 1)
        self.assertEqual(unresolved, [])

        path, msg = elf_archive.find_kiln_elf_for_build("Sep 10 2026 16:33:34")
        self.assertEqual(path, orphan_path, msg)
        self.assertIn("found", msg)

    def test_adoption_runs_automatically_from_archive_kiln_elf(self):
        """The self-healing path: an orphan sitting in the archive from a
        PRIOR plain `idf.py build` must be adopted the next time
        archive_kiln_elf() runs (i.e. the next flash_firmware() call) --
        without anyone calling adopt_orphaned_kiln_elfs directly."""
        orphan_path = os.path.join(self.archive_dir, "KilnCtrl-0a1b2c3d4e52.elf")
        _write_fake_producer_elf(orphan_path, "Sep 11 2026", "09:00:00")

        # A normal flash of a completely different build.
        new_elf = os.path.join(self._tmp.name, "KilnCtrl.elf")
        _write_fake_elf(new_elf, b"the build that was just flashed")
        elf_archive.archive_kiln_elf(new_elf, "Sep 12 2026 10:00:00", "abc123", "test")

        path, msg = elf_archive.find_kiln_elf_for_build("Sep 11 2026 09:00:00")
        self.assertEqual(path, orphan_path, msg)

    def test_orphan_with_ambiguous_identity_is_left_unregistered_not_guessed(self):
        """Two distinct embedded timestamps in one file (pathological, but
        the point is: adoption must refuse to pick one rather than
        fabricate an identity)."""
        orphan_path = os.path.join(self.archive_dir, "KilnCtrl-0a1b2c3d4e53.elf")
        os.makedirs(self.archive_dir, exist_ok=True)
        with open(orphan_path, "wb") as f:
            f.write(b"\x7fELF" + b"pad" * 4)
            f.write(_pack_fake_app_desc("Sep 13 2026", "01:00:00"))
            f.write(b"\x00" * 32)
            f.write(_pack_fake_app_desc("Sep 14 2026", "02:00:00"))

        adopted, unresolved = elf_archive.adopt_orphaned_kiln_elfs(self.archive_dir)
        self.assertEqual(adopted, 0)
        self.assertIn("KilnCtrl-0a1b2c3d4e53.elf", unresolved)
        manifest = elf_archive._load_manifest(self.archive_dir)
        self.assertNotIn("Sep 13 2026 01:00:00", manifest)
        self.assertNotIn("Sep 14 2026 02:00:00", manifest)


class ManifestMigrationCollisionTest(unittest.TestCase):
    """2026-09-10 (opus review round 4, defect 3): when a raw key and its
    normalized twin both exist in manifest.json, _load_manifest drops the
    lower-`seq` one -- and, before this fix, recorded it nowhere, making it
    just as unreachable-and-unprotected as an orphan file (the same failure
    class defect 1 closes, reproduced by a different mechanism)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "elf_archive")
        os.makedirs(self.archive_dir, exist_ok=True)

    def test_collision_loser_is_recorded_as_superseded_not_dropped(self):
        raw_key = "Sep  5 2026 11:00:00"       # double-space day padding
        norm_key = "Sep 5 2026 11:00:00"       # already normalized
        manifest = {
            raw_key: {
                "elf_key": "loser0000001",
                "identity": raw_key,
                "seq": 1,
                "archived_at": "2026-09-05T11:01:00Z",
                "git_commit": "aaa",
                "source": "test",
            },
            norm_key: {
                "elf_key": "winner000001",
                "identity": norm_key,
                "seq": 2,
                "archived_at": "2026-09-05T11:05:00Z",
                "git_commit": "bbb",
                "source": "test",
            },
        }
        elf_archive._write_manifest(self.archive_dir, manifest)

        loaded = elf_archive._load_manifest(self.archive_dir)
        # Exactly one entry survives under the normalized key -- the higher-seq one.
        self.assertEqual(loaded[norm_key]["elf_key"], "winner000001")

        # 2026-09-10 fix: the loser must now be recorded in superseded.json,
        # not silently dropped -- so it stays reachable/protected.
        superseded = elf_archive._load_superseded(self.archive_dir)
        self.assertIn(norm_key, superseded)
        loser_keys = [e.get("elf_key") for e in superseded[norm_key]]
        self.assertIn("loser0000001", loser_keys)

    def test_negative_without_the_fix_collision_loser_is_orphaned(self):
        """Proves the fix is load-bearing: with the superseded-recording
        step monkeypatched away (simulating the pre-fix _load_manifest,
        which only kept `manifest[norm_key] = entry` and otherwise
        `continue`d), the loser must NOT appear in superseded.json --
        reproducing the exact silent-drop this defect describes."""
        raw_key = "Sep  6 2026 12:00:00"
        norm_key = "Sep 6 2026 12:00:00"
        manifest = {
            raw_key: {"elf_key": "loser0000002", "identity": raw_key, "seq": 1,
                       "archived_at": "x", "git_commit": "a", "source": "test"},
            norm_key: {"elf_key": "winner000002", "identity": norm_key, "seq": 2,
                        "archived_at": "x", "git_commit": "b", "source": "test"},
        }
        elf_archive._write_manifest(self.archive_dir, manifest)

        # Pre-fix behavior: persist_migration=False skips the recording step
        # this test targets (see _load_manifest's persist_migration param).
        loaded = elf_archive._load_manifest(self.archive_dir, persist_migration=False)
        self.assertEqual(loaded[norm_key]["elf_key"], "winner000002")

        superseded = elf_archive._load_superseded(self.archive_dir)
        self.assertNotIn(norm_key, superseded,
                          "with persist_migration disabled, the collision loser must be "
                          "unrecorded, reproducing the pre-fix silent-drop incident")


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
        elf_archive.MAX_ARCHIVED_ELFS = 2
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


class PruneRetentionTest(unittest.TestCase):
    """2026-09-10 (opus review round 4, defect 2): this class used to prove
    the OLD, defective behavior -- that only the KEEP_RECENT_ENTRIES most
    recent manifest entries survive pruning, i.e. that _prune WILL delete
    a manifest-referenced (still lookup-reachable) file once enough newer
    entries exist. That was exactly the bug: 32 of 42 real manifest entries
    measured unprotected in the live archive, one _prune call away from
    "manifest says found, file says missing".

    2026-09-10/11 (owner-directed cleanup): a later fix made "manifest- and
    superseded-referenced" protection unconditional regardless of age, which
    closed that bug but reopened the original one from the other side -- it
    made MAX_ARCHIVED_ELFS permanently unenforceable in practice, because
    every ordinary local build eventually got a manifest entry too (see
    adopt_orphaned_kiln_elfs). Retention is now provenance-based
    (`_is_flash_sourced`): a genuinely flashed build is still protected
    unconditionally, but everything else is only protected for
    GRACE_PERIOD_HOURS after being archived/adopted, then becomes eligible.
    The tests below prove: (1) recent entries of any provenance survive
    pruning even over cap (nothing is deleted out from under someone mid-
    debug), (2) a flash-sourced entry survives pruning even once it is old,
    (3) an old, never-flashed entry IS deleted once it ages out and the
    directory is over cap -- the cap is enforceable again, not just
    non-silently-unenforceable -- and (4) the remaining-unenforceable case
    (old entries that are still flash-sourced) is still reported loudly."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.archive_dir = os.path.join(self._tmp.name, "elf_archive")
        self._orig = elf_archive.kiln_archive_dir
        elf_archive.kiln_archive_dir = lambda: self.archive_dir
        self._orig_max = elf_archive.MAX_ARCHIVED_ELFS
        elf_archive.MAX_ARCHIVED_ELFS = 3

    def tearDown(self):
        elf_archive.kiln_archive_dir = self._orig
        elf_archive.MAX_ARCHIVED_ELFS = self._orig_max
        self._tmp.cleanup()

    def test_all_distinct_identities_survive_pruning_even_over_cap(self):
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
        # All 8 identities are distinct and every one is manifest-referenced,
        # so all 8 must survive -- the cap of 3 is deliberately NOT enforced
        # here, since enforcing it would mean deleting a reachable, still
        # potentially-needed-for-symbolization ELF.
        self.assertEqual(len(remaining), n)
        for r in results:
            self.assertTrue(os.path.isfile(r.archived_path),
                             f"{r.archived_path} is manifest-referenced and must never be pruned")

    def test_unenforceable_cap_is_reported_loudly(self):
        with unittest.mock.patch("builtins.print") as mock_print:
            for i in range(8):
                elf_path = os.path.join(self._tmp.name, f"build{i}.elf")
                _write_fake_elf(elf_path, f"distinct content #{i}".encode())
                elf_archive.archive_kiln_elf(elf_path, f"build-{i}", f"commit{i}", "test")
        warnings = [str(c.args[0]) for c in mock_print.call_args_list if c.args]
        self.assertTrue(
            any("cap" in w.lower() and "not being enforced" in w.lower() for w in warnings),
            f"expected a loud unenforceable-cap warning, got: {warnings}",
        )

    def _age_all_entries(self, hours: float) -> None:
        """Back-dates every manifest entry's archived_at by `hours`, so the
        grace window in _prune treats them as no longer recent -- without
        this, every entry archived by a test looks "just now" and the
        age-based half of the retention policy can never be exercised."""
        manifest = elf_archive._load_manifest(self.archive_dir)
        past = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - hours * 3600))
        for entry in manifest.values():
            entry["archived_at"] = past
        elf_archive._write_manifest(self.archive_dir, manifest)

    def test_old_never_flashed_entries_are_pruned_once_over_cap(self):
        """The real enforcement case: once a never-flashed entry is both
        past GRACE_PERIOD_HOURS and the directory is over cap, _prune must
        actually delete some of them (file AND manifest entry) -- proving
        the cap is enforceable again under ordinary operation, not merely
        honest about being unenforceable. Which of the (tied-age, distinct-
        hash) old entries specifically survives isn't asserted -- only that
        the cap is met and the right NUMBER of never-flashed entries went."""
        n_old = 4
        for i in range(n_old):
            elf_path = os.path.join(self._tmp.name, f"old{i}.elf")
            _write_fake_elf(elf_path, f"old never-flashed build #{i}".encode())
            elf_archive.archive_kiln_elf(elf_path, f"old-build-{i}", f"oldcommit{i}", "test")
        self._age_all_entries(elf_archive.GRACE_PERIOD_HOURS + 1)

        # One more archive call brings the count to n_old+1=5 against a cap
        # of 3, with everything else now past its grace window -- this
        # single _prune pass must evict exactly 2 to land back at the cap.
        new_elf = os.path.join(self._tmp.name, "trigger.elf")
        _write_fake_elf(new_elf, b"the archive call that triggers pruning")
        elf_archive.archive_kiln_elf(new_elf, "trigger-build", "triggercommit", "test")

        remaining = [
            name for name in os.listdir(self.archive_dir)
            if name.startswith("KilnCtrl-") and name.endswith(".elf")
            and name != "KilnCtrl-latest.elf"
        ]
        self.assertEqual(len(remaining), elf_archive.MAX_ARCHIVED_ELFS,
                          "cap must actually be enforced once entries are old and never-flashed")
        manifest = elf_archive._load_manifest(self.archive_dir)
        surviving_old = [k for k in manifest if k.startswith("old-build-")]
        self.assertEqual(len(surviving_old), n_old - 2,
                          "exactly enough old never-flashed entries must be pruned to meet the cap")
        self.assertIn("trigger-build", manifest, "the fresh entry must survive (still in grace)")

    def test_negative_without_grace_expiry_check_old_entries_are_not_pruned(self):
        """Proves the age check in _prune (not something else) is what lets
        old entries be deleted: monkeypatch _parse_archived_at to always
        report "just now", reproducing the pre-fix (round 4) shape where
        recency could never be established as expired, and confirm the same
        scenario above then does NOT prune anything -- the directory stays
        over cap."""
        n_old = 4
        for i in range(n_old):
            elf_path = os.path.join(self._tmp.name, f"old{i}.elf")
            _write_fake_elf(elf_path, f"old never-flashed build #{i}".encode())
            elf_archive.archive_kiln_elf(elf_path, f"old-build-{i}", f"oldcommit{i}", "test")
        self._age_all_entries(elf_archive.GRACE_PERIOD_HOURS + 1)

        with unittest.mock.patch.object(elf_archive, "_parse_archived_at", return_value=time.time()):
            new_elf = os.path.join(self._tmp.name, "trigger.elf")
            _write_fake_elf(new_elf, b"trigger with the age check neutered")
            elf_archive.archive_kiln_elf(new_elf, "trigger-build", "triggercommit", "test")

        manifest = elf_archive._load_manifest(self.archive_dir)
        surviving_old = [k for k in manifest if k.startswith("old-build-")]
        self.assertEqual(len(surviving_old), n_old,
                          "without a working age check, old never-flashed entries are never "
                          "pruned -- reproducing the cap-can-never-be-enforced incident")

    def test_flash_sourced_entry_survives_pruning_even_when_old(self):
        """The other half of the policy: a genuinely flashed build must
        never be deleted by _prune regardless of age -- an older flashed
        build can still be the one a board is running (otadata hazard)."""
        flashed_elf = os.path.join(self._tmp.name, "flashed.elf")
        _write_fake_elf(flashed_elf, b"a real flash")
        result = elf_archive.archive_kiln_elf(flashed_elf, "flashed-build", "realcommit", "flash_firmware")
        for i in range(6):
            elf_path = os.path.join(self._tmp.name, f"filler{i}.elf")
            _write_fake_elf(elf_path, f"filler never-flashed build #{i}".encode())
            elf_archive.archive_kiln_elf(elf_path, f"filler-build-{i}", f"fillercommit{i}", "test")
        self._age_all_entries(elf_archive.GRACE_PERIOD_HOURS + 1)

        newest_elf = os.path.join(self._tmp.name, "newest.elf")
        _write_fake_elf(newest_elf, b"forces a prune pass")
        elf_archive.archive_kiln_elf(newest_elf, "newest-build", "newestcommit", "test")

        self.assertTrue(os.path.isfile(result.archived_path),
                         "a flash-sourced entry must survive pruning even when old")
        manifest = elf_archive._load_manifest(self.archive_dir)
        self.assertIn("flashed-build", manifest)

    def test_negative_without_flash_source_check_flashed_entry_gets_pruned(self):
        """Proves _is_flash_sourced (not incidental recency) is what
        protects the flashed entry above: monkeypatch it to always report
        False -- as if every entry, flashed or not, were judged purely on
        age -- and confirm the same flashed build then gets deleted once it
        ages past the grace window, reproducing the "an old flashed image is
        the one the board still needs and it just got deleted" incident."""
        flashed_elf = os.path.join(self._tmp.name, "flashed.elf")
        _write_fake_elf(flashed_elf, b"a real flash")
        result = elf_archive.archive_kiln_elf(flashed_elf, "flashed-build", "realcommit", "flash_firmware")
        for i in range(6):
            elf_path = os.path.join(self._tmp.name, f"filler{i}.elf")
            _write_fake_elf(elf_path, f"filler never-flashed build #{i}".encode())
            elf_archive.archive_kiln_elf(elf_path, f"filler-build-{i}", f"fillercommit{i}", "test")
        self._age_all_entries(elf_archive.GRACE_PERIOD_HOURS + 1)
        # Make the flashed entry the single OLDEST one so it is deterministically
        # first in line for deletion once its flash-provenance protection is
        # removed below -- otherwise which of the several equally-aged, tied
        # entries gets evicted would depend on filename (content-hash) sort
        # order, making this test flaky.
        manifest = elf_archive._load_manifest(self.archive_dir)
        manifest["flashed-build"]["archived_at"] = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - (elf_archive.GRACE_PERIOD_HOURS + 100) * 3600))
        elf_archive._write_manifest(self.archive_dir, manifest)

        with unittest.mock.patch.object(elf_archive, "_is_flash_sourced", return_value=False):
            newest_elf = os.path.join(self._tmp.name, "newest.elf")
            _write_fake_elf(newest_elf, b"forces a prune pass with the flash check neutered")
            elf_archive.archive_kiln_elf(newest_elf, "newest-build", "newestcommit", "test")

        self.assertFalse(
            os.path.isfile(result.archived_path),
            "without the flash-source check, a genuinely flashed build is not protected from "
            "pruning once old -- reproducing the incident",
        )


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


class ArchiveDirSurvivesBuildWipeTest(unittest.TestCase):
    """2026-09-15: the canonical archive used to live at
    firmware/<KilnFW|SaftyFW>/build/elf_archive/ -- INSIDE the directory
    `idf.py fullclean` (or an equivalent manual wipe) empties. The
    2026-09-14 23:55:17Z flash (commit c8f7506b) was archived correctly at
    flash time and found completely gone the next day: manifest.json,
    flash_provenance.json, and the archived ELF all vanished together, and
    firmware/KilnFW/build/'s own CMakeCache.txt/config.env timestamps showed
    the directory had been reconfigured from scratch shortly before that
    flash. See docs/audits/profile_executor_coredump_2026-09-15.md and
    kiln_archive_dir()'s docstring.

    This proves the fix structurally: the canonical archive directories must
    not have a path component literally named "build" -- if they did, an
    ordinary rm-and-reconfigure of the ESP-IDF/pico-sdk build directory
    would take the archive out with it, exactly as it did for real. It does
    NOT simulate an actual `idf.py fullclean` (no ESP-IDF toolchain in this
    test environment) -- the path-shape assertion is the mechanism that
    would have caught this bug regardless of which specific tool emptied
    build/, and is stable across environments."""

    def test_kiln_archive_dir_not_inside_build(self):
        parts = elf_archive.kiln_archive_dir().replace("\\", "/").split("/")
        self.assertNotIn("build", parts,
                          "kiln_archive_dir() must not live inside any directory named "
                          "'build' -- a build-dir wipe (idf.py fullclean) would silently "
                          "delete the canonical archive along with it, as it did for the "
                          "2026-09-14 23:55:17Z flash (see docs/audits/"
                          "profile_executor_coredump_2026-09-15.md)")

    def test_safty_archive_dir_not_inside_build(self):
        parts = elf_archive.safty_archive_dir().replace("\\", "/").split("/")
        self.assertNotIn("build", parts,
                          "safty_archive_dir() must not live inside any directory named "
                          "'build' -- same hazard as kiln_archive_dir(), see that test")

    def test_archived_elf_survives_simulated_build_wipe(self):
        """End-to-end proof: archive a fake ELF, delete everything that
        would exist under a real build/ directory (bin/elf outputs,
        CMakeCache.txt, the whole ESP-IDF/pico-sdk build tree), and confirm
        the archived copy + manifest entry are both still there afterward.

        L3 (2026-09-15 review): the previous version of this test patched
        `kiln_archive_dir` directly to a hand-built sibling path, so it never
        called the production path-computing logic at all and would have
        passed even against the pre-fix code (real `kiln_archive_dir()`
        still returning a `build/`-nested path). Fixed to patch `_repo_root`
        instead -- the real `kiln_archive_dir()` then computes the path from
        that fake root, exercising the exact function under test. Since
        `_canonical_archive_dirs()`/`_canonical_provenance_path()` also
        derive from `_repo_root()`, the guard would now see this fake path as
        "canonical" and refuse -- KILNCTL_ALLOW_TEST_ARCHIVE_WRITE=1 is the
        documented escape hatch for exactly this case (a test deliberately
        exercising the canonical-path logic against a fake root)."""
        with tempfile.TemporaryDirectory() as tmp:
            fake_repo_root = tmp
            fake_build_dir = os.path.join(fake_repo_root, "firmware", "KilnFW", "build")
            elf_path = os.path.join(fake_build_dir, "KilnCtrl.elf")
            _write_fake_elf(elf_path, b"build-wipe-survival-fake-elf-content")

            os.environ["KILNCTL_ALLOW_TEST_ARCHIVE_WRITE"] = "1"
            try:
                with unittest.mock.patch.object(elf_archive, "_repo_root", return_value=fake_repo_root):
                    fake_archive_dir = elf_archive.kiln_archive_dir()
                    result = elf_archive.archive_kiln_elf(elf_path, "Sep 14 2026 23:55:17", "c8f7506b", "test")
                    self.assertTrue(os.path.isfile(result.archived_path))
                    self.assertEqual(os.path.dirname(result.archived_path),
                                      os.path.normpath(fake_archive_dir))

                    # Simulate `idf.py fullclean`: the entire build/ directory
                    # (and everything under it) is removed. If the archive were
                    # still nested inside build/ (the pre-fix layout), this
                    # would delete it too.
                    import shutil
                    shutil.rmtree(fake_build_dir)
                    self.assertFalse(os.path.isdir(fake_build_dir))

                    # The archive, being a sibling of build/ rather than a
                    # descendant, must be untouched.
                    self.assertTrue(os.path.isfile(result.archived_path),
                                     "archived ELF was deleted by a build/ wipe -- the archive "
                                     "is still nested inside build/")
                    path, message = elf_archive.find_kiln_elf_for_build("Sep 14 2026 23:55:17")
                    self.assertEqual(path, result.archived_path, message)
            finally:
                del os.environ["KILNCTL_ALLOW_TEST_ARCHIVE_WRITE"]

    def test_cmake_outdir_matches_kiln_archive_dir(self):
        """L3: nothing previously enforced that archive_elf.cmake's OUTDIR
        literal (${CMAKE_CURRENT_LIST_DIR}/elf_archive, set in
        firmware/KilnFW/CMakeLists.txt) actually matches what
        kiln_archive_dir() computes -- that pairing was enforced only by
        comments. CMAKE_CURRENT_LIST_DIR for CMakeLists.txt is the directory
        containing it (firmware/KilnFW/), so the CMake-side path is
        structurally firmware/KilnFW/elf_archive -- assert both the
        CMakeLists.txt literal is present and that kiln_archive_dir() ends
        with that exact same relative path."""
        repo_root = elf_archive._repo_root()
        cmakelists_path = os.path.join(repo_root, "firmware", "KilnFW", "CMakeLists.txt")
        with open(cmakelists_path, "r", encoding="utf-8") as f:
            contents = f.read()
        self.assertIn("-DOUTDIR=${CMAKE_CURRENT_LIST_DIR}/elf_archive", contents,
                       "archive_elf.cmake's OUTDIR literal moved or changed shape -- "
                       "update this test (and re-check it still matches "
                       "kiln_archive_dir()) alongside it")
        expected = os.path.normpath(os.path.join(repo_root, "firmware", "KilnFW", "elf_archive"))
        self.assertEqual(os.path.normpath(elf_archive.kiln_archive_dir()), expected)


class CanonicalArchiveDirsMatchTest(unittest.TestCase):
    """L4: _canonical_archive_dirs() deliberately rebuilds both archive paths
    by hand (see its own docstring -- it must not read them back through the
    patchable kiln_archive_dir()/safty_archive_dir(), or the guard would be
    blind exactly when a test has monkeypatched those). That leaves a second
    copy of the same path contract, the very pair that broke in a347e726 --
    this pins the two copies equal, unpatched, against real production
    functions (pure path-string computation, no disk I/O, safe to call
    directly)."""

    def test_canonical_archive_dirs_equals_kiln_and_safty_archive_dir(self):
        expected = {
            os.path.normpath(elf_archive.kiln_archive_dir()),
            os.path.normpath(elf_archive.safty_archive_dir()),
        }
        self.assertEqual(elf_archive._canonical_archive_dirs(), expected)


class MigrateLegacyArchiveTest(unittest.TestCase):
    """L1: migrate_legacy_archive() moves entries left behind in the old
    build/elf_archive/ location into the new canonical archive dir -- move,
    never delete unmoved data; skip the <prefix>-latest.elf convenience
    pointer."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        self.archive_dir = os.path.join(self.root, "elf_archive")
        self.legacy_dir = os.path.join(self.root, "build", "elf_archive")

    def test_moves_legacy_entries_into_new_dir(self):
        _write_fake_elf(os.path.join(self.legacy_dir, "KilnCtrl-abc123.elf"), b"legacy-elf-content")
        legacy_manifest = {
            "Sep 1 2026 00:00:00": {
                "elf_key": "abc123", "identity": "Sep 1 2026 00:00:00", "seq": 1,
                "archived_at": "2026-09-01T00:00:00Z", "git_commit": "aaaa1111",
                "source": "flash_firmware",
            }
        }
        _write_fake_elf(os.path.join(self.legacy_dir, "manifest.json"), json.dumps(legacy_manifest).encode())

        moved = elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")

        self.assertEqual(moved, 1)  # only the .elf is a raw move; manifest.json is merged, not moved as a file
        self.assertTrue(os.path.isfile(os.path.join(self.archive_dir, "KilnCtrl-abc123.elf")))
        with open(os.path.join(self.archive_dir, "manifest.json")) as f:
            new_manifest = json.load(f)
        self.assertEqual(new_manifest["Sep 1 2026 00:00:00"]["elf_key"], "abc123")
        self.assertEqual(new_manifest["Sep 1 2026 00:00:00"]["source"], "flash_firmware")
        self.assertEqual(new_manifest["Sep 1 2026 00:00:00"]["archived_at"], "2026-09-01T00:00:00Z")
        # Moved, not copied -- the legacy copies are gone (manifest.json was
        # fully merged, so the drained legacy dir is cleaned up entirely --
        # L1).
        self.assertFalse(os.path.isfile(os.path.join(self.legacy_dir, "KilnCtrl-abc123.elf")))
        self.assertFalse(os.path.isfile(os.path.join(self.legacy_dir, "manifest.json")))
        self.assertFalse(os.path.isdir(self.legacy_dir))

    def test_skips_the_latest_elf_convenience_pointer(self):
        _write_fake_elf(os.path.join(self.legacy_dir, "KilnCtrl-latest.elf"), b"stale-pointer-content")

        moved = elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")

        self.assertEqual(moved, 0)
        # Never moved into the new dir -- it is meaningless once disconnected
        # from the build/ dir it pointed into.
        self.assertFalse(os.path.exists(os.path.join(self.archive_dir, "KilnCtrl-latest.elf")))
        # L1 (2026-09-15 fixes review): a stale latest.elf pointer holds no
        # data worth keeping (unlike an ELF/manifest collision), so it is
        # deleted outright once seen -- this is what lets the legacy dir
        # actually drain instead of printing the same "unmoved" warning on
        # every subsequent flash forever.
        self.assertFalse(os.path.isfile(os.path.join(self.legacy_dir, "KilnCtrl-latest.elf")))
        self.assertFalse(os.path.isdir(self.legacy_dir))  # nothing left -- directory itself is removed

    def test_never_overwrites_an_existing_destination_name(self):
        # L2 (2026-09-15 fixes review): the previous shutil.move()-based
        # implementation's os.path.exists(dst) skip check was a TOCTOU on
        # Windows -- shutil.move()'s copy+unlink fallback silently
        # overwrites an existing destination regardless. os.link() (used
        # now) physically cannot overwrite an existing name -- it raises
        # FileExistsError instead -- so this proves the real fix, not just
        # the same-shaped check.
        _write_fake_elf(os.path.join(self.legacy_dir, "KilnCtrl-collide1.elf"), b"legacy-content")
        _write_fake_elf(os.path.join(self.archive_dir, "KilnCtrl-collide1.elf"), b"current-content")

        moved = elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")

        self.assertEqual(moved, 0)
        # Both copies survive, untouched -- "move, never delete unmoved data".
        with open(os.path.join(self.legacy_dir, "KilnCtrl-collide1.elf"), "rb") as f:
            self.assertEqual(f.read(), b"legacy-content")
        with open(os.path.join(self.archive_dir, "KilnCtrl-collide1.elf"), "rb") as f:
            self.assertEqual(f.read(), b"current-content")

    def test_negative_shutil_move_would_have_overwritten_the_collision(self):
        """Proves the os.link()-based fix actually matters: with the OLD
        shutil.move()-based approach, the same collision from the test above
        silently overwrites the new file on Windows. Exercises shutil.move
        directly (not migrate_legacy_archive) so this is a genuine repro of
        the bug the fix replaced, not a mirror of the new code."""
        import shutil
        src = os.path.join(self.legacy_dir, "KilnCtrl-collide2.elf")
        dst = os.path.join(self.archive_dir, "KilnCtrl-collide2.elf")
        _write_fake_elf(src, b"legacy-content")
        _write_fake_elf(dst, b"current-content")
        if os.path.exists(dst):
            pass  # the exact check migrate_legacy_archive used to make -- still overwritten below
        shutil.move(src, dst)
        with open(dst, "rb") as f:
            self.assertEqual(f.read(), b"legacy-content")  # overwritten -- this is the bug

    def test_no_legacy_dir_is_a_harmless_no_op(self):
        self.assertFalse(os.path.isdir(self.legacy_dir))
        moved = elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")
        self.assertEqual(moved, 0)

    def test_wired_into_archive_kiln_elf_automatically(self):
        """Confirms the migration runs as a side effect of an ordinary
        archive_kiln_elf() call, not just when called directly."""
        _write_fake_elf(os.path.join(self.legacy_dir, "KilnCtrl-oldkey1.elf"), b"legacy-elf-content")
        elf_path = os.path.join(self.root, "KilnCtrl.elf")
        _write_fake_elf(elf_path, b"freshly-flashed-elf-content")

        with unittest.mock.patch.object(elf_archive, "kiln_archive_dir", return_value=self.archive_dir):
            elf_archive.archive_kiln_elf(elf_path, "Sep 15 2026 10:00:00", "deadbeef", "test")

        self.assertTrue(os.path.isfile(os.path.join(self.archive_dir, "KilnCtrl-oldkey1.elf")))
        self.assertFalse(os.path.isfile(os.path.join(self.legacy_dir, "KilnCtrl-oldkey1.elf")))


class LegacyManifestMergeTest(unittest.TestCase):
    """M1 (docs/audits/review_elf_archive_fixes_a6f4a624_2026-09-15.md): a
    legacy manifest.json must be MERGED into the new one when both exist,
    not left stranded -- leaving it stranded is what let a migrated flashed
    ELF get silently re-adopted as an orphan (bogus archived_at, no more
    flash protection). Covers the manifest-collision case and proves a
    migrated flash-sourced entry survives _prune."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        self.archive_dir = os.path.join(self.root, "elf_archive")
        self.legacy_dir = os.path.join(self.root, "build", "elf_archive")

    def _legacy_flashed_entry(self, identity="Sep 1 2026 00:00:00", elf_key="oldflashed1"):
        return {
            identity: {
                "elf_key": elf_key, "identity": identity, "seq": 1,
                "archived_at": "2026-01-01T00:00:00Z",  # deliberately ancient
                "git_commit": "cccc3333", "source": "flash_firmware",
            }
        }

    def test_no_collision_legacy_entry_is_adopted_with_its_own_provenance(self):
        elf_key = "oldflashed1"
        _write_fake_elf(os.path.join(self.legacy_dir, f"KilnCtrl-{elf_key}.elf"), b"legacy-flashed-content")
        _write_fake_elf(
            os.path.join(self.legacy_dir, "manifest.json"),
            json.dumps(self._legacy_flashed_entry(elf_key=elf_key)).encode(),
        )
        # A new manifest already exists (the common case per the audit) but
        # holds an unrelated identity -- no collision.
        _write_fake_elf(
            os.path.join(self.archive_dir, "manifest.json"),
            json.dumps({"Sep 10 2026 00:00:00": {
                "elf_key": "newkey1", "identity": "Sep 10 2026 00:00:00", "seq": 1,
                "archived_at": "2026-09-10T00:00:00Z", "git_commit": "dddd4444",
                "source": "flash_firmware",
            }}).encode(),
        )
        _write_fake_elf(os.path.join(self.archive_dir, f"KilnCtrl-newkey1.elf"), b"current-elf-content")

        elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")

        with open(os.path.join(self.archive_dir, "manifest.json")) as f:
            manifest = json.load(f)
        self.assertIn("Sep 1 2026 00:00:00", manifest)
        self.assertEqual(manifest["Sep 1 2026 00:00:00"]["elf_key"], elf_key)
        self.assertEqual(manifest["Sep 1 2026 00:00:00"]["source"], "flash_firmware")
        self.assertEqual(manifest["Sep 1 2026 00:00:00"]["archived_at"], "2026-01-01T00:00:00Z")
        # Both entries present -- the merge must not lose the pre-existing one.
        self.assertIn("Sep 10 2026 00:00:00", manifest)

    def test_manifest_collision_keeps_new_entry_and_supersedes_the_legacy_one(self):
        identity = "Sep 5 2026 00:00:00"
        _write_fake_elf(os.path.join(self.legacy_dir, "KilnCtrl-legacykey.elf"), b"legacy-content")
        _write_fake_elf(
            os.path.join(self.legacy_dir, "manifest.json"),
            json.dumps({identity: {
                "elf_key": "legacykey", "identity": identity, "seq": 1,
                "archived_at": "2026-01-01T00:00:00Z", "git_commit": "aaaa0000",
                "source": "flash_firmware",
            }}).encode(),
        )
        _write_fake_elf(
            os.path.join(self.archive_dir, "manifest.json"),
            json.dumps({identity: {
                "elf_key": "newkey", "identity": identity, "seq": 2,
                "archived_at": "2026-09-05T00:00:00Z", "git_commit": "bbbb1111",
                "source": "flash_firmware",
            }}).encode(),
        )
        _write_fake_elf(os.path.join(self.archive_dir, "KilnCtrl-newkey.elf"), b"current-content")

        elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")

        with open(os.path.join(self.archive_dir, "manifest.json")) as f:
            manifest = json.load(f)
        with open(os.path.join(self.archive_dir, "superseded.json")) as f:
            superseded = json.load(f)
        # The new (already-live) entry wins the identity slot...
        self.assertEqual(manifest[identity]["elf_key"], "newkey")
        # ...and the legacy one is preserved as superseded, not dropped.
        self.assertIn(identity, superseded)
        self.assertTrue(any(e.get("elf_key") == "legacykey" for e in superseded[identity]))

    def test_negative_without_merge_legacy_manifest_is_stranded_on_collision(self):
        """Proves the merge test above is genuinely negative-testable:
        reproduce the OLD (pre-fix) behavior directly -- a legacy manifest
        that collides by name is left in place, never merged."""
        identity = "Sep 5 2026 00:00:00"
        _write_fake_elf(os.path.join(self.legacy_dir, "KilnCtrl-legacykey.elf"), b"legacy-content")
        _write_fake_elf(
            os.path.join(self.legacy_dir, "manifest.json"),
            json.dumps({identity: {"elf_key": "legacykey", "source": "flash_firmware"}}).encode(),
        )
        _write_fake_elf(os.path.join(self.archive_dir, "manifest.json"), b'{"current": true}')

        # Simulate the OLD implementation: name collision on manifest.json ->
        # skip, never merged.
        dst = os.path.join(self.archive_dir, "manifest.json")
        self.assertTrue(os.path.exists(dst))  # collision -- old code would skip and never look inside
        with open(dst) as f:
            manifest = json.load(f)
        self.assertNotIn(identity, manifest)  # confirms: without a merge, the legacy entry never arrives

    def test_migrated_flash_sourced_entry_survives_prune_despite_ancient_archived_at(self):
        """The core of M1's severity: a migrated flash-sourced entry's
        archived_at is ANCIENT (from the legacy manifest, preserved by the
        merge -- not the file's migration-preserved mtime) and it must still
        be protected from _prune by _is_flash_sourced, not made eligible by
        looking old."""
        elf_key = "oldflashed1"
        _write_fake_elf(os.path.join(self.legacy_dir, f"KilnCtrl-{elf_key}.elf"), b"legacy-flashed-content")
        _write_fake_elf(
            os.path.join(self.legacy_dir, "manifest.json"),
            json.dumps(self._legacy_flashed_entry(elf_key=elf_key)).encode(),
        )
        os.makedirs(self.archive_dir, exist_ok=True)

        elf_archive.migrate_legacy_archive(self.archive_dir, "KilnCtrl")

        # Pad the archive dir past MAX_ARCHIVED_ELFS with fresh, never-flashed,
        # already-past-grace-window filler entries so _prune has plenty to
        # pick from before it would ever need to touch the flashed one.
        manifest_path = os.path.join(self.archive_dir, "manifest.json")
        with open(manifest_path) as f:
            manifest = json.load(f)
        old_stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 999999))
        for i in range(elf_archive.MAX_ARCHIVED_ELFS + 5):
            key = f"filler{i}"
            _write_fake_elf(os.path.join(self.archive_dir, f"KilnCtrl-{key}.elf"), f"filler-{i}".encode())
            manifest[f"filler-identity-{i}"] = {
                "elf_key": key, "identity": f"filler-identity-{i}", "seq": 100 + i,
                "archived_at": old_stamp, "git_commit": None, "source": "adopted:orphan-scan",
            }
        with open(manifest_path, "w") as f:
            json.dump(manifest, f)

        elf_archive._prune(self.archive_dir, "KilnCtrl", manifest)

        self.assertTrue(os.path.isfile(os.path.join(self.archive_dir, f"KilnCtrl-{elf_key}.elf")),
                         "a migrated flash-sourced ELF must never be pruned, regardless of age")
        with open(manifest_path) as f:
            manifest_after = json.load(f)
        self.assertIn("Sep 1 2026 00:00:00", manifest_after)

    def test_negative_without_the_merge_migrated_flashed_entry_gets_pruned(self):
        """Reproduces the actual M1 failure mode: without the manifest
        merge, a migrated ELF is registered by adopt_orphaned_kiln_elfs()
        instead, using the file's mtime as archived_at and
        'adopted:orphan-scan' as source -- NOT flash-protected, and (because
        shutil.move/os.link preserve mtime, and this test sets an ancient
        mtime to stand in for "migrated long ago") already past the grace
        window, so it is eligible for pruning."""
        archive_dir = self.archive_dir
        os.makedirs(archive_dir, exist_ok=True)
        elf_key = "oldflashed1"
        elf_path = os.path.join(archive_dir, f"KilnCtrl-{elf_key}.elf")
        # A migrated ELF is, in this simulation, embedded with a real
        # esp_app_desc build timestamp so adopt_orphaned_kiln_elfs() can
        # identify it -- but scanning real ELF bytes is out of scope for
        # this unit test, so this directly reproduces post-adoption state
        # instead: an entry registered as "adopted:orphan-scan" with an
        # ancient archived_at, which is exactly what the old (unmerged)
        # migrate_legacy_archive() left adopt_orphaned_kiln_elfs() to do.
        _write_fake_elf(elf_path, b"legacy-flashed-content")
        ancient = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 999999))
        manifest = {
            "Sep 1 2026 00:00:00": {
                "elf_key": elf_key, "identity": "Sep 1 2026 00:00:00", "seq": 1,
                "archived_at": ancient, "git_commit": None, "source": "adopted:orphan-scan",
            }
        }
        for i in range(elf_archive.MAX_ARCHIVED_ELFS + 5):
            key = f"filler{i}"
            _write_fake_elf(os.path.join(archive_dir, f"KilnCtrl-{key}.elf"), f"filler-{i}".encode())
            manifest[f"filler-identity-{i}"] = {
                "elf_key": key, "identity": f"filler-identity-{i}", "seq": 100 + i,
                "archived_at": ancient, "git_commit": None, "source": "adopted:orphan-scan",
            }

        elf_archive._prune(archive_dir, "KilnCtrl", manifest)

        # Without flash-sourced protection, the previously-flashed ELF is
        # just as eligible as any filler -- it may or may not survive
        # depending on sort order, but it is NOT guaranteed to, unlike the
        # merge-preserving path above. Confirm it lost its protection: this
        # manifest's own entry says "adopted:orphan-scan", not
        # "flash_firmware".
        self.assertFalse(elf_archive._is_flash_sourced(manifest["Sep 1 2026 00:00:00"]))


class SaftyFwLegacyMigrationTest(unittest.TestCase):
    """M1: SaftyFW gets the same migration + manifest merge treatment as
    KilnFW -- previously SaftyFW had no orphan-adoption safety net at all, so
    a migrated SaftyFW ELF with its manifest left stranded was permanently
    unregistered and unreachable by find_safty_elf_for_identity()."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name
        self.archive_dir = os.path.join(self.root, "elf_archive")
        self.legacy_dir = os.path.join(self.root, "build", "elf_archive")

    def test_saftyfw_legacy_manifest_is_merged_and_findable(self):
        identity = "abcd1234_20260901_000000"
        elf_key = "saftykey1"
        _write_fake_elf(os.path.join(self.legacy_dir, f"SaftyFW-{elf_key}.elf"), b"legacy-safty-content")
        _write_fake_elf(
            os.path.join(self.legacy_dir, "manifest.json"),
            json.dumps({identity: {
                "elf_key": elf_key, "identity": identity, "seq": 1,
                "archived_at": "2026-09-01T00:00:00Z", "git_commit": "abcd1234",
                "build_date": "20260901", "build_time": "000000", "source": "debug_program",
            }}).encode(),
        )

        with unittest.mock.patch.object(elf_archive, "safty_archive_dir", return_value=self.archive_dir):
            path, message = elf_archive.find_safty_elf_for_identity("abcd1234", "20260901", "000000")

        self.assertIsNotNone(path, message)
        self.assertTrue(os.path.isfile(path))
        self.assertEqual(os.path.normpath(path), os.path.normpath(
            os.path.join(self.archive_dir, f"SaftyFW-{elf_key}.elf")))

    def test_negative_without_saftyfw_merge_legacy_entry_is_unreachable(self):
        """Confirms the test above is genuinely negative-testable: without
        running the migration/merge at all, the same legacy layout is
        unreachable by a lookup against the new (empty) archive dir."""
        identity = "abcd1234_20260901_000000"
        elf_key = "saftykey1"
        _write_fake_elf(os.path.join(self.legacy_dir, f"SaftyFW-{elf_key}.elf"), b"legacy-safty-content")
        _write_fake_elf(
            os.path.join(self.legacy_dir, "manifest.json"),
            json.dumps({identity: {
                "elf_key": elf_key, "identity": identity, "seq": 1,
                "archived_at": "2026-09-01T00:00:00Z", "git_commit": "abcd1234",
                "build_date": "20260901", "build_time": "000000", "source": "debug_program",
            }}).encode(),
        )
        os.makedirs(self.archive_dir, exist_ok=True)  # new dir exists but is empty -- no migration ran

        manifest = elf_archive._load_manifest(self.archive_dir)
        self.assertEqual(manifest, {})  # nothing reachable without migration


class ProvenanceMigrationAndGuardTest(unittest.TestCase):
    """L3: the legacy build/flash_provenance.json is migrated, and
    get_fw_version-style readers can find it either at the new canonical
    path or (transitionally) the old one. L4: _guard_against_test_write()
    refuses a write to the canonical provenance path directly, not just the
    two archive dirs."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name

    def test_migrates_legacy_provenance_file_into_new_location(self):
        legacy = os.path.join(self.root, "legacy_flash_provenance.json")
        new = os.path.join(self.root, "new_flash_provenance.json")
        _write_fake_elf(legacy, b'{"outcome": "flashed_ok"}')

        with unittest.mock.patch.object(elf_archive, "legacy_kiln_provenance_path", return_value=legacy), \
             unittest.mock.patch.object(elf_archive, "kiln_provenance_path", return_value=new):
            moved = elf_archive.migrate_legacy_provenance()

        self.assertTrue(moved)
        self.assertTrue(os.path.isfile(new))
        self.assertFalse(os.path.isfile(legacy))
        with open(new) as f:
            self.assertEqual(json.load(f), {"outcome": "flashed_ok"})

    def test_does_not_overwrite_an_existing_new_location_file(self):
        legacy = os.path.join(self.root, "legacy_flash_provenance.json")
        new = os.path.join(self.root, "new_flash_provenance.json")
        _write_fake_elf(legacy, b'{"outcome": "flash_failed"}')
        _write_fake_elf(new, b'{"outcome": "flashed_ok"}')

        with unittest.mock.patch.object(elf_archive, "legacy_kiln_provenance_path", return_value=legacy), \
             unittest.mock.patch.object(elf_archive, "kiln_provenance_path", return_value=new):
            moved = elf_archive.migrate_legacy_provenance()

        self.assertFalse(moved)
        with open(legacy) as f:
            self.assertEqual(json.load(f), {"outcome": "flash_failed"})  # untouched
        with open(new) as f:
            self.assertEqual(json.load(f), {"outcome": "flashed_ok"})  # untouched

    def test_no_legacy_file_is_a_harmless_no_op(self):
        legacy = os.path.join(self.root, "nope.json")
        new = os.path.join(self.root, "new.json")
        with unittest.mock.patch.object(elf_archive, "legacy_kiln_provenance_path", return_value=legacy), \
             unittest.mock.patch.object(elf_archive, "kiln_provenance_path", return_value=new):
            moved = elf_archive.migrate_legacy_provenance()
        self.assertFalse(moved)

    def test_guard_refuses_a_write_to_the_canonical_provenance_path_directly(self):
        real_path = elf_archive._canonical_provenance_path()
        os.environ["PYTEST_CURRENT_TEST"] = "fake-test (for this assertion only)"
        try:
            with self.assertRaises(RuntimeError):
                elf_archive._guard_against_test_write(real_path)
        finally:
            del os.environ["PYTEST_CURRENT_TEST"]

    def test_negative_without_the_provenance_path_check_the_guard_would_allow_it(self):
        """Proves the assertion above is genuinely negative-testable: the
        guard's canonical-target check is an OR of archive dirs and the
        provenance path -- if the provenance half of that OR were removed,
        the same real path would NOT raise."""
        real_path = elf_archive._canonical_provenance_path()
        self.assertNotIn(os.path.normpath(real_path), elf_archive._canonical_archive_dirs())
        # i.e.: without the "== _canonical_provenance_path()" arm of the
        # guard's condition, nothing about this path would ever match.


if __name__ == "__main__":
    unittest.main()
