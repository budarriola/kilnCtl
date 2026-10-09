#!/usr/bin/env python3
"""Unit tests for mcp_server.thermo_read_faults() -- reads each MAX31856's SR
(fault status) and MASK registers (THERMO_CMD_READ_FAULTS over task
UART_TASK_ID_THERMO). Unlike thermo_read this does not depend on a
conversion having happened, so it is the tool an agent reaches for when a
channel looks dead -- misdecoding "no faults" vs. a real open-circuit/
over-temperature condition here is exactly backwards from what this tool
exists to prevent.

No real socket and no live board: ThermoClient.read_faults() is mocked
directly, same convention as test_control_set_zone_model.py. The malformed/
short-frame path is exercised against the real wire decoder
(devices.parse_thermo_response).

Run with: python -m pytest tools/PcTools/tests/test_thermo_read_faults_mcp.py -q
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
from kilnctrl.devices import ThermoFaultStatus, ThermoResponseError  # noqa: E402
from kilnctrl.protocol import THERMO_CMD_READ_FAULTS, ThermoFault  # noqa: E402
from kilnctrl.thermo import ThermoQueryError  # noqa: E402


class ThermoReadFaultsHappyPathTests(unittest.TestCase):
    def test_no_faults_reported_plainly(self):
        status = ThermoFaultStatus(channel=0, status=ThermoFault(0), mask=0x00)
        with unittest.mock.patch.object(
            mcp_server._thermo, "read_faults", return_value=[status]
        ) as mock_read:
            result = mcp_server.thermo_read_faults(0)
        self.assertIn("no faults", result)
        mock_read.assert_called_once_with(0)

    def test_open_circuit_fault_named_not_just_hex(self):
        status = ThermoFaultStatus(channel=2, status=ThermoFault.OPEN, mask=0x00)
        with unittest.mock.patch.object(mcp_server._thermo, "read_faults", return_value=[status]):
            result = mcp_server.thermo_read_faults(2)
        self.assertIn("open circuit", result)


class ThermoReadFaultsMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_thermo_response(); prove a truncated READ_FAULTS reply
    (count says entries follow, but they don't) is rejected rather than
    decoded as a clean "no faults" state."""

    def test_short_reply_missing_entry_bytes_is_rejected(self):
        payload = struct.pack("<BB", THERMO_CMD_READ_FAULTS, 1)  # count=1, no entry bytes
        with self.assertRaises(ThermoResponseError):
            devices.parse_thermo_response(payload)

    def test_well_formed_reply_decodes_fault_and_mask(self):
        entry = struct.pack("<BBB", 1, ThermoFault.TCHIGH, 0x07)
        payload = struct.pack("<BB", THERMO_CMD_READ_FAULTS, 1) + entry
        subcommand, entries = devices.parse_thermo_response(payload)
        self.assertEqual(subcommand, THERMO_CMD_READ_FAULTS)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].channel, 1)
        self.assertIn("TC above high threshold", entries[0].fault_labels)
        self.assertEqual(entries[0].mask, 0x07)

    def test_out_of_range_channel_is_rejected(self):
        entry = struct.pack("<BBB", 9, 0, 0)  # only channels 0-2 exist
        payload = struct.pack("<BB", THERMO_CMD_READ_FAULTS, 1) + entry
        with self.assertRaises(ThermoResponseError):
            devices.parse_thermo_response(payload)


class ThermoReadFaultsErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._thermo, "read_faults", side_effect=ThermoQueryError("timed out"),
        ):
            result = mcp_server.thermo_read_faults(0)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
