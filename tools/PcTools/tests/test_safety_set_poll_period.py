#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_set_poll_period() -- sets how often the
ESP polls the safety processor (SAFETY_CMD_SET_POLL_PERIOD).

No real socket and no live board: SafetyClient.set_poll_period() is mocked
directly, same convention as test_safety_request_enable.py mocking
SafetyClient.request_enable(). The malformed/short-frame path is exercised
against the real wire decoder (devices.parse_safety_response) so a truncated
SET_POLL_PERIOD reply cannot be silently decoded as success.

Run with: python -m pytest tools/PcTools/tests/test_safety_set_poll_period.py -q
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
from kilnctrl.devices import OkReason  # noqa: E402
from kilnctrl.devices_safety import SafetyResponseError  # noqa: E402
from kilnctrl.safety import SafetyQueryError  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_SET_POLL_PERIOD  # noqa: E402


class SafetySetPollPeriodHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_with_period(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_poll_period", return_value=OkReason(ok=True)
        ) as mock_set:
            result = mcp_server.safety_set_poll_period(500)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("500", result)
        mock_set.assert_called_once_with(500)

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_poll_period",
            return_value=OkReason(ok=False, reason="out of range"),
        ):
            result = mcp_server.safety_set_poll_period(0)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("out of range", result)


class SafetySetPollPeriodMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_safety_response(); prove a truncated SET_POLL_PERIOD reply
    (missing even the ok byte) is rejected rather than decoded as a
    successful OkReason."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", SAFETY_CMD_SET_POLL_PERIOD)  # subcmd only
        with self.assertRaises(SafetyResponseError):
            devices.parse_safety_response(payload)

    def test_well_formed_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", SAFETY_CMD_SET_POLL_PERIOD, 1)
        subcommand, value = devices.parse_safety_response(payload)
        self.assertEqual(subcommand, SAFETY_CMD_SET_POLL_PERIOD)
        self.assertTrue(bool(value))


class SafetySetPollPeriodErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_poll_period",
            side_effect=SafetyQueryError("timed out"),
        ):
            result = mcp_server.safety_set_poll_period(500)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
