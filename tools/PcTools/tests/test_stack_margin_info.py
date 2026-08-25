#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.parse_stack_margin_response()/StackMarginEntry
-- the PC-side decode of INFO_CMD_GET_STACK_MARGIN (0x04), added to unblock
KilnFW TODO.md section 13: six internal-only FreeRTOS task stacks (~20.5KB)
that must not be resized "from the numbers in this entry alone" without a
real uxTaskGetStackHighWaterMark() reading. This is the PC-side half of
that measurement path -- see App/drivers/stack_margin.c/build_stack_margin_
reply() in uart_bridge.c for the firmware side this mirrors byte-for-byte.

No real UART/serial connection is used -- this only checks the byte-exact
wire decoding, built directly from build_stack_margin_reply()'s layout:

    byte0     count (N)
    N * {
        u8   name_len (Nn)
        Nn   ASCII task name, NOT null-terminated
        u32  configured_stack_bytes, LE
        u32  hwm_bytes, LE
        u8   flags: bit0 alive, bits1-2 level
    }

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import (  # noqa: E402
    INFO_CMD_GET_FW_VERSION,
    INFO_CMD_GET_PIN_CONFIG,
    INFO_CMD_GET_STACK_MARGIN,
    StackMarginLevel,
)


def _entry_bytes(name: str, configured: int, hwm: int, *, alive: bool, level: int) -> bytes:
    flags = (1 if alive else 0) | ((level & 0x03) << 1)
    name_b = name.encode("ascii")
    return struct.pack("<B", len(name_b)) + name_b + struct.pack("<IIB", configured, hwm, flags)


class ParseStackMarginResponseTests(unittest.TestCase):
    def test_empty_registry_decodes_to_empty_list(self):
        payload = struct.pack("<B", 0)
        self.assertEqual(devices.parse_stack_margin_response(payload), [])

    def test_single_alive_ok_entry_round_trips_exactly(self):
        payload = struct.pack("<B", 1) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        entries = devices.parse_stack_margin_response(payload)
        self.assertEqual(len(entries), 1)
        e = entries[0]
        self.assertEqual(e.name, "rules_task")
        self.assertEqual(e.configured_stack_bytes, 3072)
        self.assertEqual(e.hwm_bytes, 2048)
        self.assertTrue(e.alive)
        self.assertEqual(e.level, StackMarginLevel.OK)

    def test_dead_task_reports_zero_hwm_and_ok_level_not_stale_data(self):
        # Mirrors stack_margin_read()'s own contract: alive=false always
        # carries hwm_bytes=0, level=OK -- never a leftover reading.
        payload = struct.pack("<B", 1) + _entry_bytes(
            "system_uart_bridge", 3072, 0, alive=False, level=StackMarginLevel.OK
        )
        entries = devices.parse_stack_margin_response(payload)
        self.assertFalse(entries[0].alive)
        self.assertEqual(entries[0].hwm_bytes, 0)

    def test_multiple_entries_in_order(self):
        payload = struct.pack("<B", 2)
        payload += _entry_bytes("uart_owner_task", 4096, 3800, alive=True, level=StackMarginLevel.OK)
        payload += _entry_bytes("rules_watchdog", 2048, 100, alive=True, level=StackMarginLevel.CRITICAL)
        entries = devices.parse_stack_margin_response(payload)
        self.assertEqual([e.name for e in entries], ["uart_owner_task", "rules_watchdog"])
        self.assertEqual(entries[1].level, StackMarginLevel.CRITICAL)

    def test_truncated_payload_raises_not_silently_drops_entries(self):
        # count says 2 but only one full entry follows.
        payload = struct.pack("<B", 2) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(payload)

    def test_trailing_garbage_after_last_entry_raises(self):
        payload = (
            struct.pack("<B", 1)
            + _entry_bytes("rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK)
            + b"\x00\x00"
        )
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(payload)

    def test_name_len_overrunning_payload_raises(self):
        # name_len claims 20 bytes of name but the payload doesn't have them.
        payload = struct.pack("<B", 1) + struct.pack("<B", 20) + b"short"
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(payload)

    def test_empty_payload_raises(self):
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(b"")


class StackMarginEntryHelpersTests(unittest.TestCase):
    def test_headroom_pct_matches_manual_division(self):
        e = devices.StackMarginEntry(
            name="rules_task",
            configured_stack_bytes=3072,
            hwm_bytes=768,
            alive=True,
            level=StackMarginLevel.LOW,
        )
        # Absolute check against a hand-computed value, not against the
        # implementation's own formula restated.
        self.assertAlmostEqual(e.headroom_pct, 25.0, places=3)

    def test_headroom_pct_is_none_when_not_alive(self):
        e = devices.StackMarginEntry(
            name="rules_task", configured_stack_bytes=3072, hwm_bytes=0, alive=False,
            level=StackMarginLevel.OK,
        )
        self.assertIsNone(e.headroom_pct)

    def test_headroom_pct_is_none_for_zero_configured_bytes(self):
        e = devices.StackMarginEntry(
            name="broken", configured_stack_bytes=0, hwm_bytes=0, alive=True,
            level=StackMarginLevel.CRITICAL,
        )
        self.assertIsNone(e.headroom_pct)

    def test_describe_not_running(self):
        e = devices.StackMarginEntry(
            name="rules_task", configured_stack_bytes=3072, hwm_bytes=0, alive=False,
            level=StackMarginLevel.OK,
        )
        self.assertIn("not running", e.describe())


class InfoRequestBuilderTests(unittest.TestCase):
    def test_info_get_stack_margin_request_is_one_byte_subcommand(self):
        self.assertEqual(devices.info_get_stack_margin(), struct.pack("<B", INFO_CMD_GET_STACK_MARGIN))


class ParseInfoResponseDisambiguationTests(unittest.TestCase):
    """parse_info_response() classifies structurally since INFO replies carry
    no subcommand byte -- confirms GET_STACK_MARGIN is reachable through
    that dispatcher, and that `prefer` breaks the empty-reply tie against
    GET_PIN_CONFIG's own empty-response shape (both are just byte ``00``)."""

    def test_stack_margin_reply_classified_via_prefer(self):
        payload = struct.pack("<B", 1) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        subcommand, value = devices.parse_info_response(payload, prefer=INFO_CMD_GET_STACK_MARGIN)
        self.assertEqual(subcommand, INFO_CMD_GET_STACK_MARGIN)
        self.assertEqual(len(value), 1)
        self.assertEqual(value[0].name, "rules_task")

    def test_empty_reply_prefers_pin_config_over_stack_margin_when_asked(self):
        # An empty count byte (0x00) is byte-identical between an empty
        # GET_PIN_CONFIG and an empty GET_STACK_MARGIN reply -- only
        # `prefer` can tell them apart. This proves the PIN_CONFIG side of
        # that tie still resolves correctly now that a second candidate
        # parser exists.
        payload = struct.pack("<B", 0)
        subcommand, value = devices.parse_info_response(payload, prefer=INFO_CMD_GET_PIN_CONFIG)
        self.assertEqual(subcommand, INFO_CMD_GET_PIN_CONFIG)
        self.assertEqual(value, [])

    def test_empty_reply_prefers_stack_margin_when_asked(self):
        payload = struct.pack("<B", 0)
        subcommand, value = devices.parse_info_response(payload, prefer=INFO_CMD_GET_STACK_MARGIN)
        self.assertEqual(subcommand, INFO_CMD_GET_STACK_MARGIN)
        self.assertEqual(value, [])

    def test_fw_version_reply_still_wins_without_prefer(self):
        # No regression check: a real FW version reply must still classify
        # as FW_VERSION even with GET_STACK_MARGIN now in the candidate
        # list. Layout (build_fw_version_reply): protocol_version u16 LE,
        # dirty u8, commit_len u8 + commit, datetime_len u8 + datetime.
        commit = b"abc1234"
        datetime_ = b"2026-08-24 00:00:00Z"
        fw_payload = (
            struct.pack("<H", devices.UART_PROTOCOL_VERSION)
            + struct.pack("<B", 0)  # dirty
            + struct.pack("<B", len(commit))
            + commit
            + struct.pack("<B", len(datetime_))
            + datetime_
        )
        subcommand, _value = devices.parse_info_response(fw_payload)
        self.assertEqual(subcommand, INFO_CMD_GET_FW_VERSION)


if __name__ == "__main__":
    unittest.main()
