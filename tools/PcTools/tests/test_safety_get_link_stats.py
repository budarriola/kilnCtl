#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_get_link_stats() -- reads the ESP's own
counters for the isolated UART link to the safety processor
(SAFETY_CMD_GET_LINK_STATS).

No real socket and no live board: SafetyClient.get_link_stats() is mocked
directly, same convention as test_safety_request_enable.py mocking
SafetyClient.request_enable(). The malformed/short-frame path is exercised
against the real wire decoder (devices.parse_safety_response) so a truncated
GET_LINK_STATS reply is proven to raise rather than being silently decoded.

Run with: python -m pytest tools/PcTools/tests/test_safety_get_link_stats.py -q
"""
from __future__ import annotations

import os
import struct
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl import devices  # noqa: E402
from kilnctrl.safety import SafetyQueryError  # noqa: E402
from kilnctrl.devices_safety import SafetyLinkStats, SafetyResponseError  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_GET_LINK_STATS  # noqa: E402


class SafetyGetLinkStatsHappyPathTests(unittest.TestCase):
    def test_success_returns_describe_string(self):
        stats = SafetyLinkStats(
            frames_sent=10,
            frames_received=9,
            crc_errors=0,
            timeouts=1,
            poll_period_ms=500,
            broadcast_dropped=None,
            diag_applied=None,
            power_applied=None,
            frames_deframed=None,
            frames_routed_nowhere=None,
            frame_length_mismatch=None,
            frame_crc_mismatch=None,
            frame_resync=None,
            dequeued_total=None,
            unmatched_cmd_count=None,
            last_unmatched_cmd_byte=None,
            cmd_status_count=None,
            cmd_fw_version_count=None,
            cmd_update_status_count=None,
            cmd_power_count=None,
            cmd_diag_count=None,
            cmd_trip_event_count=None,
            cmd_ct_cal_count=None,
            cmd_config_page_count=None,
            cmd_commit_config_rejected_count=None,
        )
        with unittest.mock.patch.object(
            mcp_server._safety, "get_link_stats", return_value=stats
        ) as mock_get:
            result = mcp_server.safety_get_link_stats()
        self.assertEqual(result, stats.describe())
        mock_get.assert_called_once_with()


class SafetyGetLinkStatsMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_safety_response(); prove a truncated GET_LINK_STATS reply
    (shorter than the smallest valid V1 length) is rejected rather than
    decoded as a set of counters."""

    def test_short_reply_is_rejected(self):
        # V1 GET_LINK_STATS is 19 bytes (subcmd + sent/received/crc/timeouts
        # u32 + poll_period u16); one byte short of that must raise.
        payload = struct.pack("<BIIII", SAFETY_CMD_GET_LINK_STATS, 1, 2, 3, 4)
        self.assertEqual(len(payload), 17)
        with self.assertRaises(SafetyResponseError):
            devices.parse_safety_response(payload)

    def test_well_formed_v1_reply_decodes(self):
        payload = struct.pack(
            "<BIIIIH", SAFETY_CMD_GET_LINK_STATS, 10, 9, 0, 1, 500
        )
        subcommand, value = devices.parse_safety_response(payload)
        self.assertEqual(subcommand, SAFETY_CMD_GET_LINK_STATS)
        self.assertEqual(value.frames_sent, 10)
        self.assertEqual(value.frames_received, 9)
        self.assertIsNone(value.broadcast_dropped)


class SafetyGetLinkStatsErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "get_link_stats",
            side_effect=SafetyQueryError("timed out"),
        ):
            result = mcp_server.safety_get_link_stats()
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
