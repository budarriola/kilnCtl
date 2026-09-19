#!/usr/bin/env python3
"""Unit tests for kilnctrl.partition_table -- FLASH_BUDGET.md section 8
item 3 ("confirm what table is actually on the chip").

All tests operate on synthetic partition-table blobs and a temp CSV file --
no OpenOCD, no board. ``read_chip_partition_table_bytes``/
``check_chip_partition_table`` are exercised with an injected
``read_memory_fn`` standing in for ``debug_probe.read_memory``, proving the
read -> parse -> diff pipeline end to end without hardware.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import partition_table as pt  # noqa: E402


def _entry_bytes(name: str, ptype: int, subtype: int, offset: int, size: int, flags: int = 0) -> bytes:
    label = name.encode("ascii")[:16].ljust(16, b"\x00")
    return struct.pack("<HBBII16sI", pt.ENTRY_MAGIC, ptype, subtype, offset, size, label, flags)


def _table_blob(entries: "list[tuple]", pad_to: int = 0x1000) -> bytes:
    """Builds a raw partition-table blob: each entry, then an MD5 marker
    entry, then 0xFF padding out to ``pad_to`` bytes -- matching the real
    on-flash shape (gen_esp32part.py always appends the checksum entry)."""
    data = b"".join(_entry_bytes(*e) for e in entries)
    data += struct.pack("<H", pt.MD5_MAGIC) + b"\x00" * (pt.ENTRY_SIZE - 2)
    data += b"\xff" * (pad_to - len(data))
    return data


# A small, realistic table mirroring firmware/KilnFW/partitions.csv's shape
# (a handful of nvs-subtype data partitions + one app partition), used by
# several tests below.
_SAMPLE_ENTRIES = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("phy_init", 0x01, 0x01, 0xF000, 0x1000),
    ("factory", 0x00, 0x00, 0x10000, 0x100000),
]

_SAMPLE_CSV_TEXT = """\
# comment line, and a blank line follow
nvs,          data, nvs,     0x9000,   0x6000,
phy_init,     data, phy,     0xf000,   0x1000,
factory,      app,  factory, 0x10000,  0x100000,
"""


def _write_csv(text: str) -> str:
    fd, path = tempfile.mkstemp(suffix=".csv")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    return path


def _memrd_output_for(data: bytes, base_address: int) -> str:
    lines = [f"MEMRD 0x{base_address + i:08x} 0x{b:02x}" for i, b in enumerate(data)]
    return "\n".join(lines) + "\n"


class ParseBinaryTests(unittest.TestCase):
    def test_parses_entries_and_stops_at_md5_marker(self):
        blob = _table_blob(_SAMPLE_ENTRIES)
        entries = pt.parse_partition_table_binary(blob)
        self.assertEqual(len(entries), 3)
        self.assertEqual(entries[0], pt.PartitionEntry("nvs", 0x01, 0x02, 0x9000, 0x6000, 0))
        self.assertEqual(entries[2], pt.PartitionEntry("factory", 0x00, 0x00, 0x10000, 0x100000, 0))

    def test_stops_at_unprogrammed_end_marker_with_no_md5(self):
        data = _entry_bytes(*_SAMPLE_ENTRIES[0]) + b"\xff" * (0x1000 - pt.ENTRY_SIZE)
        entries = pt.parse_partition_table_binary(data)
        self.assertEqual(len(entries), 1)

    def test_bad_magic_raises(self):
        bad = struct.pack("<H", 0x1234) + b"\x00" * (pt.ENTRY_SIZE - 2)
        with self.assertRaises(ValueError):
            pt.parse_partition_table_binary(bad)

    def test_all_unprogrammed_raises_no_entries_found(self):
        data = b"\xff" * 0x1000
        with self.assertRaises(ValueError):
            pt.parse_partition_table_binary(data)

    def test_label_nul_terminated_correctly(self):
        # A 16-byte label field with a name shorter than 16 bytes must not
        # pick up trailing garbage past the NUL.
        blob = _entry_bytes("nvs", 0x01, 0x02, 0x9000, 0x6000) + b"\xff" * (0x1000 - pt.ENTRY_SIZE)
        entries = pt.parse_partition_table_binary(blob)
        self.assertEqual(entries[0].name, "nvs")


class ParseCsvTests(unittest.TestCase):
    def test_parses_symbolic_types_to_numeric(self):
        path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            entries = pt.parse_partitions_csv(path)
        finally:
            os.remove(path)
        self.assertEqual(len(entries), 3)
        by_name = {e.name: e for e in entries}
        self.assertEqual(by_name["nvs"], pt.PartitionEntry("nvs", 0x01, 0x02, 0x9000, 0x6000))
        self.assertEqual(by_name["phy_init"], pt.PartitionEntry("phy_init", 0x01, 0x01, 0xF000, 0x1000))
        self.assertEqual(by_name["factory"], pt.PartitionEntry("factory", 0x00, 0x00, 0x10000, 0x100000))

    def test_ota_slot_subtype_encoding(self):
        path = _write_csv("ota_0, app, ota_0, 0x210000, 0x300000,\nota_1, app, ota_1, 0x510000, 0x300000,\n")
        try:
            entries = pt.parse_partitions_csv(path)
        finally:
            os.remove(path)
        by_name = {e.name: e for e in entries}
        self.assertEqual(by_name["ota_0"].subtype, 0x10)
        self.assertEqual(by_name["ota_1"].subtype, 0x11)

    def test_k_and_m_size_suffixes(self):
        path = _write_csv("a, data, undefined, 0x0, 24K,\nb, data, undefined, 0x6000, 1M,\n")
        try:
            entries = pt.parse_partitions_csv(path)
        finally:
            os.remove(path)
        by_name = {e.name: e for e in entries}
        self.assertEqual(by_name["a"].size, 24 * 1024)
        self.assertEqual(by_name["b"].size, 1024 * 1024)

    def test_empty_csv_raises(self):
        path = _write_csv("# only a comment\n\n")
        try:
            with self.assertRaises(ValueError):
                pt.parse_partitions_csv(path)
        finally:
            os.remove(path)

    def test_real_repo_csv_parses(self):
        repo_root = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        csv_path = os.path.join(repo_root, "firmware", "KilnFW", "partitions.csv")
        entries = pt.parse_partitions_csv(csv_path)
        names = {e.name for e in entries}
        # A handful of partitions this doc/task cares about. 'ota_0'/'ota_1'/
        # 'factory' were replaced by 'app'/'recovery' 2026-09-16 (single
        # application slot + recovery image, docs/OTA_SINGLE_SLOT_PLAN.md
        # section 1 / section 8 step 3) -- see partitions.csv's own header
        # block for that revision.
        for expected in ("pico_img", "wifi_nvs", "kiln_nvs", "profiles_nvs",
                         "otadata", "app", "recovery", "coredump", "logs"):
            self.assertIn(expected, names)


class DiffTests(unittest.TestCase):
    def test_identical_tables_match(self):
        chip = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        csv = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        diff = pt.diff_partition_tables(chip, csv)
        self.assertTrue(diff.ok)
        self.assertIn("MATCH", diff.report())

    def test_size_mismatch_is_reported_for_exactly_that_entry(self):
        chip = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        csv = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        # Mutate one entry on the "csv" side: phy_init grew from 0x1000 to 0x2000.
        csv[1] = pt.PartitionEntry("phy_init", 0x01, 0x01, 0xF000, 0x2000)
        diff = pt.diff_partition_tables(chip, csv)
        self.assertFalse(diff.ok)
        self.assertEqual(len(diff.mismatched), 1)
        name, field_mismatches = diff.mismatched[0]
        self.assertEqual(name, "phy_init")
        self.assertEqual(len(field_mismatches), 1)
        self.assertIn("size", field_mismatches[0])
        # The other two, unmodified entries must NOT show up as mismatches.
        self.assertEqual(diff.only_on_chip, [])
        self.assertEqual(diff.only_in_csv, [])
        report = diff.report()
        self.assertIn("MISMATCH", report)
        self.assertIn("phy_init", report)
        self.assertNotIn("nvs':", report)
        self.assertNotIn("factory':", report)

    def test_offset_mismatch_reported(self):
        chip = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        csv = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        csv[2] = pt.PartitionEntry("factory", 0x00, 0x00, 0xB10000, 0x100000)  # stale old offset
        diff = pt.diff_partition_tables(chip, csv)
        self.assertFalse(diff.ok)
        name, field_mismatches = diff.mismatched[0]
        self.assertEqual(name, "factory")
        self.assertTrue(any("offset" in m for m in field_mismatches))

    def test_missing_partition_reported_as_only_in_csv(self):
        chip = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES[:2]]  # "factory" missing on chip
        csv = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        diff = pt.diff_partition_tables(chip, csv)
        self.assertFalse(diff.ok)
        self.assertEqual([e.name for e in diff.only_in_csv], ["factory"])
        self.assertEqual(diff.only_on_chip, [])

    def test_extra_partition_reported_as_only_on_chip(self):
        chip = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES]
        csv = [pt.PartitionEntry(*e) for e in _SAMPLE_ENTRIES[:2]]
        diff = pt.diff_partition_tables(chip, csv)
        self.assertFalse(diff.ok)
        self.assertEqual([e.name for e in diff.only_on_chip], ["factory"])


class MemrdParsingTests(unittest.TestCase):
    def test_round_trips_bytes(self):
        data = bytes(range(256)) * 4  # 1024 bytes, includes 0x00 and 0xff
        output = _memrd_output_for(data, 0x8000)
        result = pt._bytes_from_memrd_output(output, len(data))
        self.assertEqual(result, data)

    def test_wrong_count_raises(self):
        output = _memrd_output_for(b"\x01\x02\x03", 0x8000)
        with self.assertRaises(ValueError):
            pt._bytes_from_memrd_output(output, 10)

    def test_ignores_non_memrd_lines(self):
        output = "some banner text\n" + _memrd_output_for(b"\xaa\xbb", 0x8000) + "shutdown chatter\n"
        result = pt._bytes_from_memrd_output(output, 2)
        self.assertEqual(result, b"\xaa\xbb")


class ReadChipPartitionTableBytesTests(unittest.TestCase):
    def test_uses_injected_read_memory_fn_and_returns_ordered_bytes(self):
        blob = _table_blob(_SAMPLE_ENTRIES)
        calls = []

        def fake_read_memory(peer, address, count=1, width=32, **kw):
            calls.append((peer, address, count, width))
            return True, _memrd_output_for(blob, address)

        result = pt.read_chip_partition_table_bytes(
            peer="esp", address=0x8000, size=len(blob), read_memory_fn=fake_read_memory
        )
        self.assertEqual(result, blob)
        self.assertEqual(calls, [("esp", 0x8000, len(blob), 8)])

    def test_read_failure_raises(self):
        def fake_read_memory(peer, address, count=1, width=32, **kw):
            return False, "Error: JTAG communication failure"

        with self.assertRaises(RuntimeError):
            pt.read_chip_partition_table_bytes(read_memory_fn=fake_read_memory)


class CheckChipPartitionTableEndToEndTests(unittest.TestCase):
    """Exercises the full read -> parse(chip) -> parse(csv) -> diff pipeline
    with an injected read_memory_fn, including the negative case: a mutated
    on-chip blob must be reported as exactly the entry that differs."""

    def _fake_read_memory_for(self, blob: bytes):
        def fake_read_memory(peer, address, count=1, width=32, **kw):
            return True, _memrd_output_for(blob, address)
        return fake_read_memory

    def test_matching_table_reports_ok(self):
        blob = _table_blob(_SAMPLE_ENTRIES)
        csv_path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            diff, chip_entries, csv_entries = pt.check_chip_partition_table(
                peer="esp", csv_path=csv_path, address=0x8000, size=0x1000,
                read_memory_fn=self._fake_read_memory_for(blob),
            )
        finally:
            os.remove(csv_path)
        self.assertTrue(diff.ok)
        self.assertEqual(len(chip_entries), 3)
        self.assertEqual(len(csv_entries), 3)

    def test_mutated_chip_entry_is_caught_and_named_exactly(self):
        # Mutate ONE entry's offset on the "chip" side (simulating pico_img
        # never actually having been relocated in a real reflash) -- proves
        # the diff catches a real mismatch, not just a contrived object diff.
        mutated_entries = list(_SAMPLE_ENTRIES)
        mutated_entries[2] = ("factory", 0x00, 0x00, 0xB10000, 0x100000)  # stale offset
        blob = _table_blob(mutated_entries)
        csv_path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            diff, _, _ = pt.check_chip_partition_table(
                peer="esp", csv_path=csv_path, address=0x8000, size=0x1000,
                read_memory_fn=self._fake_read_memory_for(blob),
            )
        finally:
            os.remove(csv_path)
        self.assertFalse(diff.ok)
        self.assertEqual(len(diff.mismatched), 1)
        name, field_mismatches = diff.mismatched[0]
        self.assertEqual(name, "factory")
        self.assertTrue(any("offset" in m for m in field_mismatches))
        # Everything else must come back clean -- only "factory" mismatched.
        self.assertEqual(diff.only_on_chip, [])
        self.assertEqual(diff.only_in_csv, [])
        report = diff.report()
        self.assertIn("factory", report)
        self.assertIn("0xb10000", report.lower())
        self.assertIn("0x10000", report.lower())

    def test_defaults_to_real_repo_csv(self):
        # No csv_path given -- should resolve to firmware/KilnFW/partitions.csv
        # and parse it without error (matching test_real_repo_csv_parses above).
        blob = _table_blob(_SAMPLE_ENTRIES)  # chip content is irrelevant here
        diff, chip_entries, csv_entries = pt.check_chip_partition_table(
            peer="esp", address=0x8000, size=0x1000,
            read_memory_fn=self._fake_read_memory_for(blob),
        )
        self.assertGreater(len(csv_entries), 3)  # real CSV has far more than 3 rows
        # This synthetic chip blob deliberately doesn't match the real CSV --
        # not asserting ok/not-ok here, just that both sides parsed.
        self.assertEqual(len(chip_entries), 3)


class ReadChipPartitionTableFromHttpTests(unittest.TestCase):
    """Exercises the CURRENT chip-read mechanism -- read_chip_partition_
    table_from_http()/check_chip_partition_table_via_http(), which replaced
    the broken JTAG-based read (see this module's docstring). Same
    "injected function" shape as ReadChipPartitionTableBytesTests above
    (read_memory_fn there, get_partitions_fn here) -- no real socket, no
    board, but this time proving the HTTP-JSON -> PartitionEntry conversion
    end to end instead of the MEMRD-bytes -> PartitionEntry conversion."""

    def _fake_get_partitions_for(self, running: str, entries: "list[tuple]"):
        def fake_get_partitions(host, timeout=5.0):
            return {
                "running": running,
                "partitions": [
                    {"label": e[0], "type": e[1], "subtype": e[2], "offset": e[3], "size": e[4], "encrypted": False}
                    for e in entries
                ],
            }
        return fake_get_partitions

    def test_converts_json_entries_to_partition_entries(self):
        fake = self._fake_get_partitions_for("factory", _SAMPLE_ENTRIES)
        entries = pt.read_chip_partition_table_from_http("192.168.1.156", get_partitions_fn=fake)
        self.assertEqual(len(entries), 3)
        self.assertEqual(entries[0], pt.PartitionEntry("nvs", 0x01, 0x02, 0x9000, 0x6000))
        self.assertEqual(entries[2], pt.PartitionEntry("factory", 0x00, 0x00, 0x10000, 0x100000))

    def test_matching_table_reports_ok_via_http(self):
        fake = self._fake_get_partitions_for("factory", _SAMPLE_ENTRIES)
        csv_path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            diff, chip_entries, csv_entries = pt.check_chip_partition_table_via_http(
                "192.168.1.156", csv_path=csv_path, get_partitions_fn=fake,
            )
        finally:
            os.remove(csv_path)
        self.assertTrue(diff.ok)
        self.assertEqual(len(chip_entries), 3)
        self.assertEqual(len(csv_entries), 3)

    def test_mutated_chip_entry_is_caught_and_named_exactly_via_http(self):
        # Same mutated-offset proof as
        # CheckChipPartitionTableEndToEndTests.test_mutated_chip_entry_is_caught_and_named_exactly,
        # replayed against the HTTP path this module now uses by default --
        # proves the diff logic re-pointed at JSON catches a real mismatch
        # exactly as precisely as it did reading raw JTAG bytes.
        mutated_entries = list(_SAMPLE_ENTRIES)
        mutated_entries[2] = ("factory", 0x00, 0x00, 0xB10000, 0x100000)  # stale offset
        fake = self._fake_get_partitions_for("factory", mutated_entries)
        csv_path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            diff, _, _ = pt.check_chip_partition_table_via_http(
                "192.168.1.156", csv_path=csv_path, get_partitions_fn=fake,
            )
        finally:
            os.remove(csv_path)
        self.assertFalse(diff.ok)
        self.assertEqual(len(diff.mismatched), 1)
        name, field_mismatches = diff.mismatched[0]
        self.assertEqual(name, "factory")
        self.assertTrue(any("offset" in m for m in field_mismatches))
        self.assertEqual(diff.only_on_chip, [])
        self.assertEqual(diff.only_in_csv, [])

    def test_missing_partition_on_chip_via_http(self):
        fake = self._fake_get_partitions_for("nvs", _SAMPLE_ENTRIES[:2])  # "factory" missing
        csv_path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            diff, _, _ = pt.check_chip_partition_table_via_http(
                "192.168.1.156", csv_path=csv_path, get_partitions_fn=fake,
            )
        finally:
            os.remove(csv_path)
        self.assertFalse(diff.ok)
        self.assertEqual([e.name for e in diff.only_in_csv], ["factory"])

    def test_defaults_to_real_repo_csv_via_http(self):
        fake = self._fake_get_partitions_for("factory", _SAMPLE_ENTRIES)
        diff, chip_entries, csv_entries = pt.check_chip_partition_table_via_http(
            "192.168.1.156", get_partitions_fn=fake,
        )
        self.assertGreater(len(csv_entries), 3)  # real CSV has far more than 3 rows
        self.assertEqual(len(chip_entries), 3)

    def test_recovery_image_response_raises_not_every_partition_missing(self):
        """A board running the recovery image answers GET /api/partitions
        with no "partitions" array at all (partition_http_client.
        RecoveryImageResponse). debug_check_partition_table()'s path
        (check_chip_partition_table_via_http() -> read_chip_partition_
        table_from_http()) must propagate that raise, not swallow it into
        an empty chip_entries list -- an empty list would diff against
        partitions.csv as "every single partition is only in the CSV,
        missing from the chip", which reads like a catastrophically wrong
        board rather than the much simpler, correct explanation: the board
        is just running the recovery image right now."""
        from kilnctrl import partition_http_client

        def fake_get_partitions(host, timeout=5.0):
            raise partition_http_client.RecoveryImageResponse(
                running="recovery", running_offset="0x009000", next_update="app",
            )

        csv_path = _write_csv(_SAMPLE_CSV_TEXT)
        try:
            with self.assertRaises(partition_http_client.RecoveryImageResponse) as ctx:
                pt.check_chip_partition_table_via_http(
                    "192.168.1.156", csv_path=csv_path, get_partitions_fn=fake_get_partitions,
                )
        finally:
            os.remove(csv_path)
        self.assertEqual(ctx.exception.running, "recovery")


if __name__ == "__main__":
    unittest.main()
