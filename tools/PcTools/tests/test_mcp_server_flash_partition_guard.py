#!/usr/bin/env python3
"""Unit tests for mcp_server_flash._check_app_flash_offset_matches_chip() --
the refuse-rather-than-guess guard added alongside the 2026-09-16
single-application-slot partitions.csv change (docs/OTA_SINGLE_SLOT_PLAN.md).

flash_firmware() writes the app image to a hardcoded offset
(mcp_server_flash.APP_FLASH_OFFSET). The new partitions.csv deliberately
left that offset untouched because the bench board still carries the OLD
table -- but nothing previously detected the day a board's own live table
and that hardcoded offset actually disagree. These tests drive the REAL
guard function (not a test-local reimplementation) against fake board data,
via the same `get_partitions_fn` injection seam
`partition_table.read_chip_partition_table_from_http` already exposes -- no
real socket, no board, no OpenOCD/JTAG involved anywhere in this file.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_flash as flash_mod  # noqa: E402
from kilnctrl import partition_http_client  # noqa: E402


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


# The OLD table (what the bench board actually carries as of 2026-09-16):
# app/factory-subtype partition named "factory" at APP_FLASH_OFFSET.
_OLD_TABLE_MATCHING = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("otadata", 0x01, 0x00, 0x200000, 0x2000),
    ("ota_0", 0x00, 0x10, 0x210000, 0x300000),
    ("ota_1", 0x00, 0x11, 0x510000, 0x300000),
    ("factory", 0x00, 0x00, flash_mod.APP_FLASH_OFFSET, 0x300000),
]

# The NEW table (docs/OTA_SINGLE_SLOT_PLAN.md step 3, landed in the repo's
# partitions.csv 2026-09-16): app/factory-subtype partition ("recovery") now
# sits at 0xA10000, NOT APP_FLASH_OFFSET (0x810000) -- a board actually
# migrated to this table would disagree with the still-hardcoded tool.
_NEW_TABLE_MISMATCHED = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("otadata", 0x01, 0x00, 0x200000, 0x2000),
    ("app", 0x00, 0x10, 0x210000, 0x800000),
    ("recovery", 0x00, 0x00, 0xA10000, 0x1E0000),
]

# A pathological table with no app/factory-subtype partition at all.
_NO_FACTORY_TABLE = [
    ("nvs", 0x01, 0x02, 0x9000, 0x6000),
    ("app", 0x00, 0x10, 0x210000, 0x800000),
]


class TestPartitionOffsetGuard(unittest.TestCase):
    def test_matched_state_returns_none(self):
        """Board's live table agrees with APP_FLASH_OFFSET -- guard is a
        no-op and flash_firmware() proceeds."""
        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", get_partitions_fn=_fake_partitions_fn(_OLD_TABLE_MATCHING)
        )
        self.assertIsNone(result)

    def test_mismatched_state_refuses_and_names_both_offsets(self):
        """Board's live table (post-migration shape) disagrees with the
        hardcoded write offset -- guard MUST refuse, not guess, and must
        name both the expected (tool) and actual (board) offsets."""
        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", get_partitions_fn=_fake_partitions_fn(_NEW_TABLE_MISMATCHED)
        )
        self.assertIsNotNone(result)
        self.assertTrue(result.startswith("error:"))
        self.assertIn(f"0x{flash_mod.APP_FLASH_OFFSET:x}", result)
        self.assertIn("0xa10000", result.lower())
        self.assertIn("recovery", result)

    def test_no_factory_partition_refuses(self):
        """A live table with no app/factory-subtype partition at all is
        just as unsafe to write blindly into -- must also refuse."""
        result = flash_mod._check_app_flash_offset_matches_chip(
            "10.0.0.5", get_partitions_fn=_fake_partitions_fn(_NO_FACTORY_TABLE)
        )
        self.assertIsNotNone(result)
        self.assertTrue(result.startswith("error:"))

    def test_unreachable_board_is_not_a_mismatch(self):
        """host=None (board never observed up -- the bring-up / verify=False
        case) must NOT be refused: there is nothing to disagree with."""
        result = flash_mod._check_app_flash_offset_matches_chip(None)
        self.assertIsNone(result)

    def test_transport_error_does_not_raise_and_does_not_refuse(self):
        """A genuine transport failure while probing (board vanished between
        the preflight probe and this check) must not raise an uncaught
        exception out of the guard, and must not be treated as a hard
        mismatch -- it is logged and treated as 'could not confirm', same as
        an unreachable host."""

        def _raises(host, timeout=5.0):
            raise partition_http_client.PartitionHttpError("connection refused")

        result = flash_mod._check_app_flash_offset_matches_chip("10.0.0.5", get_partitions_fn=_raises)
        self.assertIsNone(result)


if __name__ == "__main__":
    unittest.main()
