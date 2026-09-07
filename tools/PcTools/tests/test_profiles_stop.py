#!/usr/bin/env python3
"""Unit tests for mcp_server.profiles_stop() -- stops the current firing,
per its docstring "Relays off" (PROFILES_CMD_STOP over the PROFILES task).

No real socket and no live board: ProfilesClient.stop() is mocked directly,
same convention as test_safety_rate_guard.py mocking
mcp_server._profiles.get_exec_status(). The malformed/short-frame path is
exercised against the real wire decoder (devices_profiles.parse_profiles_response)
so a truncated STOP reply cannot be silently decoded as success -- this
matters more than most refusals here since a firing that is *not* actually
stopped needs to be visible, not swallowed.

Run with: python -m pytest tools/PcTools/tests/test_profiles_stop.py -q
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
from kilnctrl.devices import OkReason, ProfilesResponseError  # noqa: E402
from kilnctrl.profiles import ProfilesQueryError  # noqa: E402
from kilnctrl.protocol import PROFILES_CMD_STOP  # noqa: E402


class ProfilesStopHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_stopped(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "stop", return_value=OkReason(ok=True)
        ) as mock_stop:
            result = mcp_server.profiles_stop()
        self.assertEqual(result, "ok - stopped")
        mock_stop.assert_called_once_with()

    def test_refused_reports_nothing_running(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "stop",
            return_value=OkReason(ok=False, reason="no firing in progress"),
        ):
            result = mcp_server.profiles_stop()
        self.assertTrue(result.startswith("refused"))
        self.assertIn("no firing in progress", result)


class ProfilesStopMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_profiles_response(); prove a truncated STOP reply (missing
    even the ok byte) is rejected rather than decoded as a stopped firing."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", PROFILES_CMD_STOP)  # subcmd only, no ok byte
        with self.assertRaises(ProfilesResponseError):
            devices.parse_profiles_response(payload)

    def test_well_formed_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", PROFILES_CMD_STOP, 1)
        subcommand, value = devices.parse_profiles_response(payload)
        self.assertEqual(subcommand, PROFILES_CMD_STOP)
        self.assertTrue(bool(value))


class ProfilesStopErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "stop",
            side_effect=ProfilesQueryError("timed out"),
        ):
            result = mcp_server.profiles_stop()
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
