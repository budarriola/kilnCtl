#!/usr/bin/env python3
"""Unit tests for mcp_server.thermo_read() -- the temperature-reading MCP
tool (THERMO_CMD_READ over task UART_TASK_ID_THERMO). This is the value a
control loop or an agent watching a firing ultimately trusts, so a
misdecoded reply (e.g. a stale/failed channel silently read as a normal
temperature) is a real hazard, not a cosmetic one.

No real socket and no live board: ThermoClient.read() is mocked directly,
same convention as test_control_set_zone_model.py. The malformed/short-frame
path is exercised against the real wire decoder
(devices.parse_thermo_response), proving a truncated READ reply is rejected
rather than decoded into a bogus reading, and that a SPI-failed channel
(NaN temperature, SPI_FAILED flag) round-trips as invalid rather than as an
ordinary number.

Run with: python -m pytest tools/PcTools/tests/test_thermo_read_mcp.py -q
"""
from __future__ import annotations

import math
import os
import struct
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl import devices  # noqa: E402
from kilnctrl.devices import ThermoReading, ThermoResponseError  # noqa: E402
from kilnctrl.protocol import THERMO_CMD_READ, ThermoFault, ThermoReadFlag  # noqa: E402
from kilnctrl.thermo import ThermoQueryError  # noqa: E402


class ThermoReadHappyPathTests(unittest.TestCase):
    def test_normal_reading_reports_temperature(self):
        reading = ThermoReading(
            channel=0, temperature_c=850.25, cold_junction_c=23.5,
            status=ThermoFault(0), flags=ThermoReadFlag(0),
        )
        with unittest.mock.patch.object(
            mcp_server._thermo, "read", return_value=[reading]
        ) as mock_read:
            result = mcp_server.thermo_read(0)
        self.assertIn("CH0", result)
        self.assertIn("850.25", result)
        mock_read.assert_called_once_with(0)

    def test_spi_failed_channel_reported_invalid_not_as_a_number(self):
        reading = ThermoReading(
            channel=1, temperature_c=float("nan"), cold_junction_c=float("nan"),
            status=ThermoFault(0), flags=ThermoReadFlag.SPI_FAILED,
        )
        with unittest.mock.patch.object(mcp_server._thermo, "read", return_value=[reading]):
            result = mcp_server.thermo_read(1)
        self.assertIn("invalid", result)
        self.assertIn("SPI read failed", result)


class ThermoReadMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_thermo_response(); prove a truncated READ reply is
    rejected rather than decoded into a bogus reading."""

    def test_short_reply_missing_entry_bytes_is_rejected(self):
        # Header says 1 entry (12 bytes) but only 4 bytes follow.
        payload = struct.pack("<BB", THERMO_CMD_READ, 1) + b"\x00\x00\x00\x00"
        with self.assertRaises(ThermoResponseError):
            devices.parse_thermo_response(payload)

    def test_well_formed_reply_decodes_temperature_and_flags(self):
        entry = struct.pack("<BffBB", 2, 500.0, 22.0, 0, ThermoReadFlag.STALE) + b"\x00"
        payload = struct.pack("<BB", THERMO_CMD_READ, 1) + entry
        subcommand, readings = devices.parse_thermo_response(payload)
        self.assertEqual(subcommand, THERMO_CMD_READ)
        self.assertEqual(len(readings), 1)
        self.assertEqual(readings[0].channel, 2)
        self.assertAlmostEqual(readings[0].temperature_c, 500.0)
        self.assertTrue(readings[0].flags & ThermoReadFlag.STALE)

    def test_infinity_temperature_is_rejected_not_treated_as_valid(self):
        entry = struct.pack("<BffBB", 0, math.inf, 22.0, 0, 0) + b"\x00"
        payload = struct.pack("<BB", THERMO_CMD_READ, 1) + entry
        with self.assertRaises(ThermoResponseError):
            devices.parse_thermo_response(payload)


class ThermoReadErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._thermo, "read", side_effect=ThermoQueryError("timed out"),
        ):
            result = mcp_server.thermo_read(0)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
