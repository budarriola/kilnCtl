#!/usr/bin/env python3
"""Unit tests for mcp_server.profiles_get_exec_status() -- reads the current
(or last) run's state over the PROFILES task (PROFILES_CMD_GET_EXEC_STATUS).

No real socket and no live board: ProfilesClient.get_exec_status() is mocked
directly, same convention as test_profiles_stop.py mocking
ProfilesClient.stop(). The malformed/short-frame path is exercised against
the real wire decoder (devices_profiles.parse_profiles_response) so a
truncated GET_EXEC_STATUS reply cannot be silently decoded as a valid status.

Run with: python -m pytest tools/PcTools/tests/test_profiles_get_exec_status.py -q
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
from kilnctrl.devices_profiles import ProfileExecStatus, ProfilesResponseError  # noqa: E402
from kilnctrl.profiles import ProfilesQueryError  # noqa: E402
from kilnctrl.protocol import PROFILES_CMD_GET_EXEC_STATUS  # noqa: E402


class ProfilesGetExecStatusHappyPathTests(unittest.TestCase):
    def test_success_reports_state_and_zone_lines(self):
        status = ProfileExecStatus(
            state=2,
            profile_id=3,
            name="Bisque",
            zone_mask=0x01,
            segment_index=1,
            segment_count=4,
            dwelling=False,
            target_c=1000.0,
            segment_elapsed_s=60,
            dwell_remaining_s=0,
            ramp_lock_held=False,
            ramp_lock_lagging_mask=0,
            fault_guard=0,
            zones=[],
        )
        with unittest.mock.patch.object(
            mcp_server._profiles, "get_exec_status", return_value=status
        ) as mock_get:
            result = mcp_server.profiles_get_exec_status()
        self.assertIn("state=2", result)
        self.assertIn("Bisque", result)
        self.assertIn("segment=1/4", result)
        mock_get.assert_called_once_with()


class ProfilesGetExecStatusMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_profiles_response(); prove a truncated GET_EXEC_STATUS
    reply (missing even the fixed 4-byte header) is rejected rather than
    decoded as a valid status."""

    def test_short_reply_missing_header_is_rejected(self):
        payload = struct.pack("<BBB", PROFILES_CMD_GET_EXEC_STATUS, 0, 0)  # 3 bytes, need 4
        with self.assertRaises(ProfilesResponseError):
            devices.parse_profiles_response(payload)

    def test_well_formed_zero_zone_reply_decodes(self):
        header = struct.pack("<BBBB", PROFILES_CMD_GET_EXEC_STATUS, 2, 3, 0)  # name_len=0
        fixed = struct.pack(
            "<BBBBfIIBBBB",
            0,  # zone_mask
            1,  # segment_index
            4,  # segment_count
            0,  # dwelling
            1000.0,  # target_c
            60,  # segment_elapsed_s
            0,  # dwell_remaining_s
            0,  # ramp_lock_held
            0,  # ramp_lock_lagging_mask
            0,  # fault_guard
            0,  # zone_count
        )
        payload = header + fixed
        subcommand, value = devices.parse_profiles_response(payload)
        self.assertEqual(subcommand, PROFILES_CMD_GET_EXEC_STATUS)
        self.assertEqual(value.state, 2)
        self.assertEqual(value.profile_id, 3)
        self.assertEqual(value.zones, [])


class ProfilesGetExecStatusErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "get_exec_status",
            side_effect=ProfilesQueryError("timed out"),
        ):
            result = mcp_server.profiles_get_exec_status()
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
