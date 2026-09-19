#!/usr/bin/env python3
"""Unit tests for mcp_server.io_read() -- queries the SX1509 expander state
(IO_CMD_READ) over task UART_TASK_ID_IO: relays, digital I/O levels/
directions, the three thermocouple ~DRDY lines, and raw registers.

No real socket and no live board: IoClient.read() is mocked directly, same
convention as test_control_set_zone_model.py mocking ControlClient methods.
The malformed/short-frame path is exercised against the real wire decoder
(devices.parse_io_response / devices_io.parse_io_response) so a truncated
READ reply cannot be silently decoded into a bogus relay/DRDY state --
which matters here specifically because io_read() is the only place the
thermocouple ~DRDY lines are visible at all.

Run with: python -m pytest tools/PcTools/tests/test_io_read.py -q
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
from kilnctrl.devices import IoState, IoResponseError  # noqa: E402
from kilnctrl.io_expander import IoQueryError  # noqa: E402
from kilnctrl.protocol import IO_CMD_READ  # noqa: E402


class IoReadHappyPathTests(unittest.TestCase):
    def test_relay_and_drdy_state_reported(self):
        state = IoState(data=0x0105, dir=0x0000, relays=0x05, io_levels=0x02, drdy=0x03, flags=0x00)
        with unittest.mock.patch.object(mcp_server._io, "read", return_value=state) as mock_read:
            result = mcp_server.io_read()
        self.assertIn("R1=1", result)
        self.assertIn("R2=0", result)
        self.assertIn("R3=1", result)
        self.assertIn("DRDY0=1", result)
        self.assertIn("DRDY1=1", result)
        mock_read.assert_called_once()

    def test_relay4_never_rendered_as_plain_relay_state(self):
        """R4's expander bit is not heat state -- K4 closes only via
        SAFETY_CMD_REQUEST_ENABLE (see firmware/KilnFW/App/drivers/control/
        heat_enable.h). io_read() must not print it in the same "R4=<n>"
        shape as R1-R3, which is exactly the confusion that cost a bench
        session (K4 bit read back 1, no element current ever flowed)."""
        state = IoState(data=0x000F, dir=0x0000, relays=0x0F, io_levels=0, drdy=0, flags=0x00)
        with unittest.mock.patch.object(mcp_server._io, "read", return_value=state):
            result = mcp_server.io_read()
        self.assertNotIn("R4=", result)
        self.assertIn("K4_bit=1", result)
        self.assertIn("NOT heat", result)
        self.assertIn("SAFETY_CMD_REQUEST_ENABLE", result)
        self.assertIn("safety_request_enable", result)

    def test_i2c_failure_flag_surfaced(self):
        state = IoState(data=0, dir=0, relays=0, io_levels=0, drdy=0, flags=IoState.FLAG_I2C_FAILED)
        with unittest.mock.patch.object(mcp_server._io, "read", return_value=state):
            result = mcp_server.io_read()
        self.assertIn("I2C FAILED", result)


class IoReadMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_io_response(); prove a truncated READ reply is rejected
    rather than decoded into a bogus relay/DRDY state."""

    def test_short_read_reply_missing_bytes_is_rejected(self):
        # READ must be exactly 9 bytes; this is 5 (header + one u16).
        payload = struct.pack("<BH", IO_CMD_READ, 0x1234)
        with self.assertRaises(IoResponseError):
            devices.parse_io_response(payload)

    def test_well_formed_read_reply_decodes_expected_fields(self):
        payload = struct.pack("<BHHBBBB", IO_CMD_READ, 0x00FF, 0x0000, 0x0A, 0x01, 0x03, 0x00)
        subcommand, value = devices.parse_io_response(payload)
        self.assertEqual(subcommand, IO_CMD_READ)
        self.assertIsInstance(value, IoState)
        self.assertEqual(value.relays, 0x0A)
        self.assertTrue(value.drdy_asserted(0))
        self.assertTrue(value.drdy_asserted(1))


class IoReadErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._io, "read", side_effect=IoQueryError("no reply within timeout"),
        ):
            result = mcp_server.io_read()
        self.assertTrue(result.startswith("error"))
        self.assertIn("no reply within timeout", result)


if __name__ == "__main__":
    unittest.main()
