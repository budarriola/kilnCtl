#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_set_fault_out() -- drives the isolated
Fault line to the safety processor (SAFETY_CMD_SET_FAULT_OUT).

No real socket and no live board: SafetyClient.set_fault_out() is mocked
directly, same convention as test_safety_request_enable.py mocking
SafetyClient.request_enable(). The malformed/short-frame path is exercised
against the real wire decoder (devices.parse_safety_response) so a truncated
SET_FAULT_OUT reply cannot be silently decoded as success.

Run with: python -m pytest tools/PcTools/tests/test_safety_set_fault_out.py -q
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
from kilnctrl.protocol import SAFETY_CMD_SET_FAULT_OUT  # noqa: E402


class SafetySetFaultOutHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_with_state(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_fault_out", return_value=OkReason(ok=True)
        ) as mock_set:
            result = mcp_server.safety_set_fault_out(True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("fault out=True", result)
        mock_set.assert_called_once_with(True)

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_fault_out",
            return_value=OkReason(ok=False, reason="link down"),
        ):
            result = mcp_server.safety_set_fault_out(False, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("link down", result)


class SafetySetFaultOutMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_safety_response(); prove a truncated SET_FAULT_OUT reply
    (missing even the ok byte) is rejected rather than decoded as a
    successful OkReason."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", SAFETY_CMD_SET_FAULT_OUT)  # subcmd only
        with self.assertRaises(SafetyResponseError):
            devices.parse_safety_response(payload)

    def test_well_formed_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", SAFETY_CMD_SET_FAULT_OUT, 1)
        subcommand, value = devices.parse_safety_response(payload)
        self.assertEqual(subcommand, SAFETY_CMD_SET_FAULT_OUT)
        self.assertTrue(bool(value))


class SafetySetFaultOutErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_fault_out",
            side_effect=SafetyQueryError("timed out"),
        ):
            result = mcp_server.safety_set_fault_out(True)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
