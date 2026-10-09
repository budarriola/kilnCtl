#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_request_enable() -- the advisory
enable/disable request to the safety processor (SAFETY_CMD_REQUEST_ENABLE).

No real socket and no live board: SafetyClient.request_enable() is mocked
directly, same convention as test_safety_rate_guard.py mocking
safety_cfg_http_client.apply_safety_fields(). The malformed/short-frame path
is exercised against the real wire decoder (devices_safety.parse_safety_response)
so a truncated REQUEST_ENABLE reply is proven to raise rather than being
silently accepted as an ok=True request_enable() result.

Run with: python -m pytest tools/PcTools/tests/test_safety_request_enable.py -q
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
from kilnctrl.safety import SafetyQueryError  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_REQUEST_ENABLE  # noqa: E402


class SafetyRequestEnableHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_with_requested_value(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "request_enable", return_value=OkReason(ok=True)
        ) as mock_req:
            result = mcp_server.safety_request_enable(True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("enable=True", result)
        mock_req.assert_called_once_with(True)

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "request_enable",
            return_value=OkReason(ok=False, reason="link down"),
        ):
            result = mcp_server.safety_request_enable(False)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("link down", result)


class SafetyRequestEnableMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_safety_response(); prove a truncated REQUEST_ENABLE reply
    (missing even the ok byte) is rejected rather than decoded as a
    successful OkReason."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        from kilnctrl.safety import SafetyResponseError

        payload = struct.pack("<B", SAFETY_CMD_REQUEST_ENABLE)  # subcmd only, no ok byte
        with self.assertRaises(SafetyResponseError):
            devices.parse_safety_response(payload)

    def test_well_formed_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", SAFETY_CMD_REQUEST_ENABLE, 1)
        subcommand, value = devices.parse_safety_response(payload)
        self.assertEqual(subcommand, SAFETY_CMD_REQUEST_ENABLE)
        self.assertTrue(bool(value))


class SafetyRequestEnableErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "request_enable",
            side_effect=SafetyQueryError("timed out"),
        ):
            result = mcp_server.safety_request_enable(True)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
