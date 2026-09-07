#!/usr/bin/env python3
"""Unit tests for mcp_server.io_set_relay() -- the actual relay-energizing
MCP tool (IO_CMD_SET_RELAY over task UART_TASK_ID_IO). Not to be confused
with tools/check_relay_authority_paths.py's negative test
(test_check_relay_authority_paths.py), which only proves a *static check*
can catch a bypass of IoClient.set_relay() -- it never calls this tool or
exercises the real RelayResult/RelayRefusal decoder.

No real socket and no live board: IoClient.set_relay() is mocked directly,
same convention as test_control_set_zone_model.py. The malformed/short-frame
path is exercised against the real wire decoder (devices.parse_io_response)
so a truncated SET_RELAY refusal reply cannot be silently read as success --
this command's firmware side (io_bridge_task()) replies with NOTHING at all
on success and only replies when it refuses (owned by a profile / safety
fault / OTA in progress / truncated / out of range / driver error), so
getting this decode wrong in either direction is a real "did the relay
actually energize" hazard, not a cosmetic one.

Run with: python -m pytest tools/PcTools/tests/test_io_set_relay_mcp.py -q
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
from kilnctrl.devices import RelayResult, RelayRefusal, IoResponseError  # noqa: E402
from kilnctrl.io_expander import IoQueryError  # noqa: E402
from kilnctrl.protocol import IO_CMD_SET_RELAY  # noqa: E402


class IoSetRelayHappyPathTests(unittest.TestCase):
    def test_silent_success_reports_ok(self):
        result_obj = RelayResult(ok=True, reason_text=None, refusal=RelayRefusal.OTHER)
        with unittest.mock.patch.object(
            mcp_server._io, "set_relay", return_value=result_obj
        ) as mock_set:
            result = mcp_server.io_set_relay(1, True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("relay 1", result)
        self.assertIn("on", result)
        mock_set.assert_called_once_with(1, True)


class IoSetRelayRefusalTests(unittest.TestCase):
    def test_owned_by_profile_refusal_reports_reason(self):
        result_obj = RelayResult(ok=False, reason_text="owned", refusal=RelayRefusal.OWNED)
        with unittest.mock.patch.object(mcp_server._io, "set_relay", return_value=result_obj):
            result = mcp_server.io_set_relay(2, False)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("owned", result)

    def test_safety_fault_refusal_reports_reason(self):
        result_obj = RelayResult(ok=False, reason_text="safety", refusal=RelayRefusal.SAFETY)
        with unittest.mock.patch.object(mcp_server._io, "set_relay", return_value=result_obj):
            result = mcp_server.io_set_relay(3, True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("safety", result)


class IoSetRelayMalformedFrameTests(unittest.TestCase):
    """The optional refusal reply this tool's client waits on is decoded by
    devices.parse_io_response(); prove a truncated SET_RELAY refusal reply
    (missing even the ok byte) is rejected rather than decoded as a
    (mis-classified) success or refusal."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", IO_CMD_SET_RELAY)  # subcmd only, no ok byte
        with self.assertRaises(IoResponseError):
            devices.parse_io_response(payload)

    def test_well_formed_refusal_with_reason_decodes(self):
        reason = b"owned"
        payload = struct.pack("<BBB", IO_CMD_SET_RELAY, 0, len(reason)) + reason
        subcommand, value = devices.parse_io_response(payload)
        self.assertEqual(subcommand, IO_CMD_SET_RELAY)
        self.assertIsInstance(value, RelayResult)
        self.assertFalse(value.ok)
        self.assertEqual(value.refusal, RelayRefusal.OWNED)


class IoSetRelayErrorPathTests(unittest.TestCase):
    def test_undelivered_request_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._io, "set_relay", side_effect=IoQueryError("not delivered"),
        ):
            result = mcp_server.io_set_relay(1, True)
        self.assertTrue(result.startswith("error"))
        self.assertIn("not delivered", result)


if __name__ == "__main__":
    unittest.main()
