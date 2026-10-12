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


def _idle():
    return unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                               return_value=None)


def _zones(kp=10.0, ki=0.5, kd=2.0, idx=1):
    z = unittest.mock.Mock(index=idx, pid_kp=kp, pid_ki=ki, pid_kd=kd)
    return (3, 3, [z])


class ControlSetZonePidHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_with_zone_after_readback(self):
        with _idle(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid", return_value=OkReason(ok=True)
        ) as mock_set, unittest.mock.patch.object(
            mcp_server._control, "get_zones", return_value=_zones()
        ):
            result = mcp_server.control_set_zone_pid(1, 10.0, 0.5, 2.0, confirm=True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("zone 1", result)
        mock_set.assert_called_once_with(1, 10.0, 0.5, 2.0)

    def test_refused_reports_reason(self):
        with _idle(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid",
            return_value=OkReason(ok=False, reason="zone index out of range"),
        ):
            result = mcp_server.control_set_zone_pid(9, 10.0, 0.5, 2.0, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("zone index out of range", result)

    def test_unconfirmed_and_truthy_non_bool_refuse_without_writing(self):
        for bad in (False, "yes", 1):
            with unittest.mock.patch.object(mcp_server._control, "set_zone_pid") as mock_set:
                result = mcp_server.control_set_zone_pid(1, 10.0, 0.5, 2.0, confirm=bad)
            self.assertTrue(result.startswith("refused"), bad)
            mock_set.assert_not_called()

    def test_mid_run_refuses_without_writing(self):
        with unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                                 return_value="a profile is currently running"),                 unittest.mock.patch.object(mcp_server._control, "set_zone_pid") as mock_set:
            result = mcp_server.control_set_zone_pid(1, 10.0, 0.5, 2.0, confirm=True)
        self.assertTrue(result.startswith("refused"))
        mock_set.assert_not_called()

    def test_readback_mismatch_fails_loud(self):
        with _idle(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid", return_value=OkReason(ok=True)
        ), unittest.mock.patch.object(mcp_server._control, "get_zones", return_value=_zones(kp=9.0)):
            result = mcp_server.control_set_zone_pid(1, 10.0, 0.5, 2.0, confirm=True)
        self.assertTrue(result.startswith("FAILED"))
        self.assertIn("kp", result)


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
        with _idle(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_pid",
            side_effect=ControlQueryError("timed out"),
        ):
            result = mcp_server.control_set_zone_pid(0, 10.0, 0.5, 2.0, confirm=True)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
