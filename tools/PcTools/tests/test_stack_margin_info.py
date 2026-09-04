#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.parse_stack_margin_response()/
parse_stack_margin_page()/StackMarginEntry -- the PC-side decode of
INFO_CMD_GET_STACK_MARGIN (0x04), added to unblock KilnFW TODO.md section 13:
internal-only FreeRTOS task stacks that must not be resized "from the numbers
in this entry alone" without a real uxTaskGetStackHighWaterMark() reading.
This is the PC-side half of that measurement path -- see
App/drivers/stack_margin.c/build_stack_margin_reply() in uart_bridge_info.c
for the firmware side this mirrors byte-for-byte.

No real UART/serial connection is used -- this only checks the byte-exact
wire decoding, built directly from build_stack_margin_reply()'s layout
(DRAM_PSRAM_PLAN.md section 7, 2026-09-02 cap-raise pass added the
truncated/next_start_index page header so a caller can never mistake a
partial reply for the whole registry):

    byte0     count-this-page (N)
    byte1     truncated (0/1)
    byte2     next_start_index (valid only when byte1==1)
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
import threading
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.devices import InfoResponseError, StackMarginEntry  # noqa: E402
from kilnctrl.info import InfoClient  # noqa: E402
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


def _page_header(count: int, *, truncated: bool = False, next_start_index: int = 0) -> bytes:
    return struct.pack("<BBB", count, 1 if truncated else 0, next_start_index)


class ParseStackMarginResponseTests(unittest.TestCase):
    def test_empty_registry_decodes_to_empty_list(self):
        payload = _page_header(0)
        self.assertEqual(devices.parse_stack_margin_response(payload), [])

    def test_single_alive_ok_entry_round_trips_exactly(self):
        payload = _page_header(1) + _entry_bytes(
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
        payload = _page_header(1) + _entry_bytes(
            "system_uart_bridge", 3072, 0, alive=False, level=StackMarginLevel.OK
        )
        entries = devices.parse_stack_margin_response(payload)
        self.assertFalse(entries[0].alive)
        self.assertEqual(entries[0].hwm_bytes, 0)

    def test_multiple_entries_in_order(self):
        payload = _page_header(2)
        payload += _entry_bytes("uart_owner_task", 4096, 3800, alive=True, level=StackMarginLevel.OK)
        payload += _entry_bytes("rules_watchdog", 2048, 100, alive=True, level=StackMarginLevel.CRITICAL)
        entries = devices.parse_stack_margin_response(payload)
        self.assertEqual([e.name for e in entries], ["uart_owner_task", "rules_watchdog"])
        self.assertEqual(entries[1].level, StackMarginLevel.CRITICAL)

    def test_truncated_payload_raises_not_silently_drops_entries(self):
        # count says 2 but only one full entry follows.
        payload = _page_header(2) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(payload)

    def test_trailing_garbage_after_last_entry_raises(self):
        payload = (
            _page_header(1)
            + _entry_bytes("rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK)
            + b"\x00\x00"
        )
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(payload)

    def test_name_len_overrunning_payload_raises(self):
        # name_len claims 20 bytes of name but the payload doesn't have them.
        payload = _page_header(1) + struct.pack("<B", 20) + b"short"
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(payload)

    def test_empty_payload_raises(self):
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(b"")

    def test_header_shorter_than_3_bytes_raises(self):
        # Pre-paging firmware would send just byte0=count; a pc_tools build
        # with this pagination change must refuse to misread byte1/byte2 of
        # a real entry as truncated/next_start_index.
        with self.assertRaises(devices.InfoResponseError):
            devices.parse_stack_margin_response(struct.pack("<B", 0))


class ParseStackMarginPageTests(unittest.TestCase):
    """parse_stack_margin_page() -- the full (entries, truncated,
    next_start_index) tuple KilnInfo.get_stack_margin() pages with."""

    def test_not_truncated_reports_next_index_zero(self):
        payload = _page_header(1) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        entries, truncated, next_start_index = devices.parse_stack_margin_page(payload)
        self.assertEqual(len(entries), 1)
        self.assertFalse(truncated)
        self.assertEqual(next_start_index, 0)

    def test_truncated_page_reports_next_start_index(self):
        payload = _page_header(1, truncated=True, next_start_index=7) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        entries, truncated, next_start_index = devices.parse_stack_margin_page(payload)
        self.assertEqual(len(entries), 1)
        self.assertTrue(truncated)
        self.assertEqual(next_start_index, 7)


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
    def test_info_get_stack_margin_request_defaults_to_start_index_zero(self):
        self.assertEqual(
            devices.info_get_stack_margin(),
            struct.pack("<BB", INFO_CMD_GET_STACK_MARGIN, 0),
        )

    def test_info_get_stack_margin_request_carries_start_index(self):
        self.assertEqual(
            devices.info_get_stack_margin(7),
            struct.pack("<BB", INFO_CMD_GET_STACK_MARGIN, 7),
        )


class ParseInfoResponseDisambiguationTests(unittest.TestCase):
    """parse_info_response() classifies structurally since INFO replies carry
    no subcommand byte -- confirms GET_STACK_MARGIN is reachable through
    that dispatcher. GET_STACK_MARGIN's page header (added 2026-09-02) makes
    its minimum reply 3 bytes, so it and GET_PIN_CONFIG's 1-byte empty reply
    no longer tie byte-for-byte; `prefer` is still exercised below since it
    remains the deciding factor whenever a genuine tie IS possible (see
    test_fw_version_reply_still_wins_without_prefer)."""

    def test_stack_margin_reply_classified_via_prefer(self):
        payload = _page_header(1) + _entry_bytes(
            "rules_task", 3072, 2048, alive=True, level=StackMarginLevel.OK
        )
        subcommand, value = devices.parse_info_response(payload, prefer=INFO_CMD_GET_STACK_MARGIN)
        self.assertEqual(subcommand, INFO_CMD_GET_STACK_MARGIN)
        self.assertEqual(len(value), 1)
        self.assertEqual(value[0].name, "rules_task")

    def test_empty_pin_config_reply_classifies_without_prefer(self):
        payload = struct.pack("<B", 0)
        subcommand, value = devices.parse_info_response(payload, prefer=INFO_CMD_GET_PIN_CONFIG)
        self.assertEqual(subcommand, INFO_CMD_GET_PIN_CONFIG)
        self.assertEqual(value, [])

    def test_empty_stack_margin_reply_classified_via_prefer(self):
        payload = _page_header(0)
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


def _fake_entry(name: str) -> StackMarginEntry:
    return StackMarginEntry(
        name=name,
        configured_stack_bytes=3072,
        hwm_bytes=2048,
        alive=True,
        level=StackMarginLevel.OK,
    )


def _bare_info_client() -> InfoClient:
    """An InfoClient with no real link/consumer thread -- get_stack_margin()'s
    paging loop only touches self._query, self._query_lock and self._pending
    (via _query, which we replace outright), so bypass __init__ (which needs
    a real UartLink and spawns a background thread) rather than faking one."""
    client = InfoClient.__new__(InfoClient)
    client._query_lock = threading.RLock()
    return client


class GetStackMarginPagingTests(unittest.TestCase):
    """InfoClient.get_stack_margin() (kilnctrl/info.py) -- the loop that
    walks start_index/next_start_index across pages and aggregates them into
    one list. This is exactly the kind of off-by-one-drops-the-last-page bug
    class this repo keeps shipping (see MEMORY project_split_module_missing_
    name_class.md and friends), so it gets its own coverage independent of
    the byte-decode tests above."""

    def test_single_page_not_truncated_returns_its_entries(self):
        client = _bare_info_client()
        client._query = unittest.mock.Mock(
            return_value=([_fake_entry("a"), _fake_entry("b")], False, 0)
        )
        result = client.get_stack_margin()
        self.assertEqual([e.name for e in result], ["a", "b"])
        client._query.assert_called_once()

    def test_three_pages_all_aggregated_including_the_last(self):
        # 25 tasks, ~9 per page (matches the real ~10-short-names-per-253B
        # bound) -- the exact shape that motivated pagination in the first
        # place. The regression this guards against: an off-by-one that
        # stops one page early would silently drop the final page's entries
        # and this test would still see a shorter list than expected.
        page1 = ([_fake_entry(f"t{i}") for i in range(9)], True, 9)
        page2 = ([_fake_entry(f"t{i}") for i in range(9, 18)], True, 18)
        page3 = ([_fake_entry(f"t{i}") for i in range(18, 25)], False, 0)
        client = _bare_info_client()
        client._query = unittest.mock.Mock(side_effect=[page1, page2, page3])
        result = client.get_stack_margin()
        self.assertEqual([e.name for e in result], [f"t{i}" for i in range(25)])
        self.assertEqual(client._query.call_count, 3)
        # start_index actually advanced across calls, in order.
        start_indices = [call.args[1] for call in client._query.call_args_list]
        self.assertEqual(
            [devices.info_get_stack_margin(0), devices.info_get_stack_margin(9),
             devices.info_get_stack_margin(18)],
            start_indices,
        )

    def test_repeated_next_start_index_raises_instead_of_looping_forever(self):
        """NEGATIVE TEST: a firmware bug that reports next_start_index=9 on
        both page 1 and page 2 (an off-by-one that fails to advance) must be
        caught and named, not silently re-fetched forever or silently
        under-report. This is the exact failure mode the docstring on
        get_stack_margin() promises to guard against."""
        page1 = ([_fake_entry(f"t{i}") for i in range(9)], True, 9)
        page2_stuck = ([_fake_entry(f"t{i}") for i in range(9, 18)], True, 9)  # BUG: repeats 9
        client = _bare_info_client()
        client._query = unittest.mock.Mock(side_effect=[page1, page2_stuck, page1, page2_stuck])
        with self.assertRaises(InfoResponseError) as ctx:
            client.get_stack_margin()
        self.assertIn("start_index=9", str(ctx.exception))
        self.assertIn("repeated", str(ctx.exception))
        # And it must have stopped after the second call, not spun through
        # the mock's whole side_effect list.
        self.assertEqual(client._query.call_count, 2)


if __name__ == "__main__":
    unittest.main()
