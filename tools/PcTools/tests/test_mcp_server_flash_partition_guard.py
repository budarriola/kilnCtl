#!/usr/bin/env python3
"""Unit tests for mcp_server_flash._check_app_flash_offset_matches_chip() --
the refuse-rather-than-guess guard for the app-image write target.

2026-09-17 fix: flash_firmware() used to write the app image to a hardcoded
offset (APP_FLASH_OFFSET). That offset silently went wrong when
docs/OTA_SINGLE_SLOT_PLAN.md's partitions.csv redesign landed (the app
image ended up written into the small `recovery` partition and overflowed
it). The fix resolves the write target dynamically from partitions.csv
(`_resolve_app_flash_target()`) and matches the board's own live table BY
NAME (`app_target.name`) instead of by (type, subtype) -- matching by
factory-subtype alone is exactly what would have accidentally matched
`recovery` (which is also factory-subtype) instead of `app` under the new
table.

These tests drive the REAL guard function (not a test-local
reimplementation) against fake board data, via the same `get_partitions_fn`
injection seam `partition_table.read_chip_partition_table_from_http`
already exposes -- no real socket, no board, no OpenOCD/JTAG involved
anywhere in this file.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_flash as flash_mod  # noqa: E402
from kilnctrl import partition_http_client  # noqa: E402
from kilnctrl import partition_table  # noqa: E402


def _fake_partitions_fn(entries):
    """Builds a `get_partitions_fn(host, timeout=...)` stand-in returning the
    GET /api/partitions JSON shape (partition_http_client.get_partitions'
    contract) for a fixed list of (label, type, subtype, offset, size)."""

    def _fn(host, timeout=5.0):
        return {
            "running": entries[0][0] if entries else "unknown",
            "partitions": [
                {
                    "label": label, "type": ptype, "subtype": subtype,
                    "offset": offset, "size": size, "encrypted": False,
                }
                for (label, ptype, subtype, offset, size) in entries
            ],
        }

    return _fn


# The CURRENT single-slot table (docs/OTA_SINGLE_SLOT_PLAN.md step 3):
# app/ota_0 named "app" at 0x210000, matching the resolved target.
_CURRENT_TABLE_MATCHING = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("otadata", 0x01, 0x00, 0x200000, 0x2000),
    ("app", 0x00, 0x10, 0x210000, 0x800000),
    ("recovery", 0x00, 0x00, 0xA10000, 0x1E0000),
    ("coredump", 0x01, 0x03, 0xBF0000, 0x100000),
]

# A board still carrying the OLD dual-OTA-slot table: it has no partition
# literally named "app" (only ota_0/ota_1/factory), so the guard must
# refuse rather than guess.
_OLD_TABLE_NO_APP_NAME = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("otadata", 0x01, 0x00, 0x200000, 0x2000),
    ("ota_0", 0x00, 0x10, 0x210000, 0x300000),
    ("ota_1", 0x00, 0x11, 0x510000, 0x300000),
    ("factory", 0x00, 0x00, 0x810000, 0x300000),
]

# A pathological table where a partition IS named "app" but at the wrong
# offset -- must be refused, naming both offsets.
_APP_NAME_WRONG_OFFSET = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("app", 0x00, 0x10, 0x999000, 0x800000),
]

_APP_TARGET = partition_table.PartitionEntry(
    name="app", type=0x00, subtype=0x10, offset=0x210000, size=0x800000,
)


class TestPartitionOffsetGuard(unittest.TestCase):
    def test_matched_state_returns_none(self):
        """Board's live table agrees with the resolved app_target -- guard
        is a no-op and flash_firmware() proceeds."""
        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", _APP_TARGET, get_partitions_fn=_fake_partitions_fn(_CURRENT_TABLE_MATCHING)
        )
        self.assertIsNone(result)

    def test_mismatched_offset_refuses_and_names_both_offsets(self):
        """Board reports a partition named "app" but at a different offset
        -- guard MUST refuse, not guess, and must name both offsets."""
        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", _APP_TARGET, get_partitions_fn=_fake_partitions_fn(_APP_NAME_WRONG_OFFSET)
        )
        self.assertIsNotNone(result)
        self.assertTrue(result.startswith("error:"))
        self.assertIn(f"0x{_APP_TARGET.offset:x}", result)
        self.assertIn("0x999000", result.lower())

    def test_no_matching_name_refuses(self):
        """A live table (e.g. the OLD dual-OTA-slot layout) with nothing
        named "app" at all is just as unsafe to write blindly into -- must
        also refuse, and must not accidentally match "factory" or
        "recovery" just because they share app-type/factory-subtype."""
        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", _APP_TARGET, get_partitions_fn=_fake_partitions_fn(_OLD_TABLE_NO_APP_NAME)
        )
        self.assertIsNotNone(result)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("app", result)

    def test_unreachable_board_is_not_a_mismatch(self):
        """host=None (board never observed up -- the bring-up / verify=False
        case) must NOT be refused: there is nothing to disagree with."""
        result = flash_mod._check_app_flash_offset_matches_chip(None, _APP_TARGET)
        self.assertIsNone(result)

    def test_transport_error_does_not_raise_and_does_not_refuse(self):
        """A genuine transport failure while probing (board vanished between
        the preflight probe and this check) must not raise an uncaught
        exception out of the guard, and must not be treated as a hard
        mismatch -- it is logged and treated as 'could not confirm', same as
        an unreachable host."""

        def _raises(host, timeout=5.0):
            raise partition_http_client.PartitionHttpError("connection refused")

        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", _APP_TARGET, get_partitions_fn=_raises
        )
        self.assertIsNone(result)


class TestResolveAppFlashTarget(unittest.TestCase):
    """_resolve_app_flash_target() reads partitions.csv fresh per flash --
    this is the fix for the hardcoded-offset defect itself, so it gets its
    own direct coverage rather than only through the guard above."""

    def test_resolves_app_partition_from_csv(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            csv_path = os.path.join(tmp, "partitions.csv")
            with open(csv_path, "w", encoding="utf-8") as f:
                f.write(
                    "# Name,   Type, SubType, Offset,  Size\n"
                    "otadata,  data, ota,     0x200000, 0x2000,\n"
                    "app,      app,  ota_0,   0x210000, 0x800000,\n"
                    "recovery, app,  factory, 0xA10000, 0x1E0000,\n"
                    "coredump, data, coredump,0xBF0000, 0x100000,\n"
                )
            target = flash_mod._resolve_app_flash_target(tmp)
            self.assertEqual(target.name, "app")
            self.assertEqual(target.offset, 0x210000)
            self.assertEqual(target.size, 0x800000)

    def test_raises_when_no_app_partition_named(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            csv_path = os.path.join(tmp, "partitions.csv")
            with open(csv_path, "w", encoding="utf-8") as f:
                f.write(
                    "# Name,   Type, SubType, Offset,  Size\n"
                    "factory,  app,  factory, 0x10000, 0x300000,\n"
                )
            with self.assertRaises(ValueError) as ctx:
                flash_mod._resolve_app_flash_target(tmp)
            self.assertIn("app", str(ctx.exception))

    def test_raises_when_csv_missing(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                flash_mod._resolve_app_flash_target(tmp)


if __name__ == "__main__":
    unittest.main()
