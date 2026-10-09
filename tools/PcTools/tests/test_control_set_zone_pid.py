#!/usr/bin/env python3
"""Unit tests for mcp_server.control_set_zone_pid() -- writes a zone's PID
gains over the CONTROL task (CONTROL_CMD_SET_ZONE_PID).

No real socket and no live board: ControlClient.set_zone_pid() is mocked
directly, same convention as test_safety_rate_guard.py mocking
safety_cfg_http_client.apply_safety_fields(). The malformed/short-frame path
is exercised against the real wire decoder (devices_control.parse_control_response)
so a truncated SET_ZONE_PID reply cannot be silently decoded as success.

Run with: python -m pytest tools/PcTools/tests/test_control_set_zone_pid.py -q
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
from kilnctrl.devices import OkReason, ControlResponseError  # noqa: E402
from kilnctrl.control import ControlQueryError  # noqa: E402
from kilnctrl.protocol import CONTROL_CMD_SET_ZONE_PID  # noqa: E402


class ControlSetZonePidHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_with_zone(self):
        with unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid", return_value=OkReason(ok=True)
        ) as mock_set:
            result = mcp_server.control_set_zone_pid(1, 10.0, 0.5, 2.0)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("zone 1", result)
        mock_set.assert_called_once_with(1, 10.0, 0.5, 2.0)

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid",
            return_value=OkReason(ok=False, reason="zone index out of range"),
        ):
            result = mcp_server.control_set_zone_pid(9, 10.0, 0.5, 2.0)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("zone index out of range", result)


class ControlSetZonePidMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_control_response(); prove a truncated SET_ZONE_PID reply
    (missing even the ok byte) is rejected rather than decoded as success."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", CONTROL_CMD_SET_ZONE_PID)  # subcmd only, no ok byte
        with self.assertRaises(ControlResponseError):
            devices.parse_control_response(payload)

    def test_well_formed_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", CONTROL_CMD_SET_ZONE_PID, 1)
        subcommand, value = devices.parse_control_response(payload)
        self.assertEqual(subcommand, CONTROL_CMD_SET_ZONE_PID)
        self.assertTrue(bool(value))


class ControlSetZonePidErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid",
            side_effect=ControlQueryError("timed out"),
        ):
            result = mcp_server.control_set_zone_pid(0, 10.0, 0.5, 2.0)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
