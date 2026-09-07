#!/usr/bin/env python3
"""Unit tests for mcp_server.profiles_pause()/profiles_resume()/
profiles_ack_last_run() -- the three PROFILES-task run-control mutators
(PROFILES_CMD_PAUSE, PROFILES_CMD_RESUME, PROFILES_CMD_ACK_LAST_RUN). Grouped
in one file since all three share the identical OkReason-returning shape
already covered per-tool by test_profiles_stop.py.

No real socket and no live board: ProfilesClient.pause()/resume()/
ack_last_run() are mocked directly. The malformed/short-frame path is
exercised against the real wire decoder (devices_profiles.parse_profiles_response)
so a truncated reply cannot be silently decoded as success.

Run with: python -m pytest tools/PcTools/tests/test_profiles_pause_resume_ack.py -q
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
from kilnctrl.devices_profiles import ProfilesResponseError  # noqa: E402
from kilnctrl.profiles import ProfilesQueryError  # noqa: E402
from kilnctrl.protocol import (  # noqa: E402
    PROFILES_CMD_PAUSE,
    PROFILES_CMD_RESUME,
    PROFILES_CMD_ACK_LAST_RUN,
)


class ProfilesPauseHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_paused(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "pause", return_value=OkReason(ok=True)
        ) as mock_pause:
            result = mcp_server.profiles_pause()
        self.assertEqual(result, "ok - paused")
        mock_pause.assert_called_once_with()

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "pause",
            return_value=OkReason(ok=False, reason="nothing running"),
        ):
            result = mcp_server.profiles_pause()
        self.assertTrue(result.startswith("refused"))
        self.assertIn("nothing running", result)

    def test_query_error_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "pause", side_effect=ProfilesQueryError("timed out"),
        ):
            result = mcp_server.profiles_pause()
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


class ProfilesResumeHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_resumed(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "resume", return_value=OkReason(ok=True)
        ) as mock_resume:
            result = mcp_server.profiles_resume()
        self.assertEqual(result, "ok - resumed")
        mock_resume.assert_called_once_with()

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "resume",
            return_value=OkReason(ok=False, reason="nothing paused"),
        ):
            result = mcp_server.profiles_resume()
        self.assertTrue(result.startswith("refused"))
        self.assertIn("nothing paused", result)

    def test_query_error_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "resume", side_effect=ProfilesQueryError("timed out"),
        ):
            result = mcp_server.profiles_resume()
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


class ProfilesAckLastRunHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_acknowledged(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "ack_last_run", return_value=OkReason(ok=True)
        ) as mock_ack:
            result = mcp_server.profiles_ack_last_run()
        self.assertEqual(result, "ok - acknowledged")
        mock_ack.assert_called_once_with()

    def test_refused_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "ack_last_run",
            return_value=OkReason(ok=False, reason="nothing to acknowledge"),
        ):
            result = mcp_server.profiles_ack_last_run()
        self.assertTrue(result.startswith("refused"))
        self.assertIn("nothing to acknowledge", result)

    def test_query_error_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mcp_server._profiles, "ack_last_run", side_effect=ProfilesQueryError("timed out"),
        ):
            result = mcp_server.profiles_ack_last_run()
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


class ProfilesPauseResumeAckMalformedFrameTests(unittest.TestCase):
    """All three replies are decoded by devices.parse_profiles_response();
    prove a truncated reply for each subcommand (missing even the ok byte)
    is rejected rather than decoded as success."""

    def test_short_pause_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", PROFILES_CMD_PAUSE)
        with self.assertRaises(ProfilesResponseError):
            devices.parse_profiles_response(payload)

    def test_short_resume_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", PROFILES_CMD_RESUME)
        with self.assertRaises(ProfilesResponseError):
            devices.parse_profiles_response(payload)

    def test_short_ack_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", PROFILES_CMD_ACK_LAST_RUN)
        with self.assertRaises(ProfilesResponseError):
            devices.parse_profiles_response(payload)

    def test_well_formed_pause_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", PROFILES_CMD_PAUSE, 1)
        subcommand, value = devices.parse_profiles_response(payload)
        self.assertEqual(subcommand, PROFILES_CMD_PAUSE)
        self.assertTrue(bool(value))

    def test_well_formed_resume_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", PROFILES_CMD_RESUME, 1)
        subcommand, value = devices.parse_profiles_response(payload)
        self.assertEqual(subcommand, PROFILES_CMD_RESUME)
        self.assertTrue(bool(value))

    def test_well_formed_ack_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", PROFILES_CMD_ACK_LAST_RUN, 1)
        subcommand, value = devices.parse_profiles_response(payload)
        self.assertEqual(subcommand, PROFILES_CMD_ACK_LAST_RUN)
        self.assertTrue(bool(value))


if __name__ == "__main__":
    unittest.main()
