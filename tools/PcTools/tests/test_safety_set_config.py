#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_set_config()/SAFETY_TC_TYPE_NAMES --
the PC-facing side of SAFETY_CMD_SET_CONFIG (0x16, CommonFW/docs/LINK_PROTOCOL.md
sec 4), and mcp_server.safety_set_tc_type()'s name-to-byte resolution.

No real UART/serial connection is used -- this only checks the byte-exact
wire encoding and the human-readable-name mapping, mirroring the byte-exact
vector convention firmware/CommonFW/test uses for the same codec on the
firmware side (test/vectors/set_config_vectors.json).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl import mcp_server  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_SET_CONFIG  # noqa: E402


class SafetySetConfigEncodeTests(unittest.TestCase):
    def test_tc_type_k_matches_commonfw_vector(self):
        # firmware/CommonFW/test/vectors/set_config_vectors.json's "tc_type_k"
        # vector: cmd 0x16, tc_type 3 -> bytes 1603.
        self.assertEqual(devices.safety_set_config(0x03), bytes([0x16, 0x03]))

    def test_tc_type_zero(self):
        self.assertEqual(devices.safety_set_config(0x00), bytes([SAFETY_CMD_SET_CONFIG, 0x00]))

    def test_tc_type_max_nibble(self):
        self.assertEqual(devices.safety_set_config(0x0F), bytes([SAFETY_CMD_SET_CONFIG, 0x0F]))

    def test_tc_type_out_of_range_rejected(self):
        # Wire field is the MAX31856 CR1 TC[3:0] nibble -- 0x10 can never be
        # a legal register value, so this must be refused locally rather
        # than silently sent.
        with self.assertRaises(ValueError):
            devices.safety_set_config(0x10)

    def test_tc_type_negative_rejected(self):
        with self.assertRaises(ValueError):
            devices.safety_set_config(-1)


class SafetyTcTypeNameTableTests(unittest.TestCase):
    def test_all_eight_names_present(self):
        self.assertEqual(
            set(devices.SAFETY_TC_TYPE_NAMES),
            {"B", "E", "J", "K", "N", "R", "S", "T"},
        )

    def test_k_is_three_matching_max31856_tc_type_k(self):
        # firmware/SaftyFW/src/max31856.h: MAX31856_TC_TYPE_K = 0x03.
        self.assertEqual(devices.SAFETY_TC_TYPE_NAMES["K"], 0x03)

    def test_values_are_unique(self):
        values = list(devices.SAFETY_TC_TYPE_NAMES.values())
        self.assertEqual(len(values), len(set(values)), "no two names share a wire value")


class SafetySetTcTypeToolTests(unittest.TestCase):
    # 2026-08-24: safety_set_tc_type() now goes through
    # SafetyClient.set_config() (waits a short window for the optional
    # ESP-side refusal reply -- ROADMAP.md "KilnFW PC-link command
    # acknowledgement") instead of the raw fire-and-forget mcp_server._send.
    # These patch SafetyClient.set_config directly rather than _send.
    def test_known_name_sends_expected_bytes(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_config", return_value=devices.OkReason(ok=True)
        ) as mock_set_config:
            result = mcp_server.safety_set_tc_type("K")
        self.assertTrue(result.startswith("ok"))
        mock_set_config.assert_called_once_with(0x03)

    def test_lowercase_name_accepted(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_config", return_value=devices.OkReason(ok=True)
        ) as mock_set_config:
            mcp_server.safety_set_tc_type("k")
        mock_set_config.assert_called_once_with(0x03)

    def test_unknown_name_never_reaches_set_config(self):
        with unittest.mock.patch.object(mcp_server._safety, "set_config") as mock_set_config:
            result = mcp_server.safety_set_tc_type("not-a-type")
        mock_set_config.assert_not_called()
        self.assertTrue(result.startswith("error:"))

    def test_refusal_reason_reaches_the_caller(self):
        # The regression this pass fixes: a truncated/out-of-range refusal
        # used to be silently dropped in SafetyClient's own consumer thread
        # (mcp_server called the fire-and-forget _send, which never waited
        # for the reply). Now it must show up in the returned string.
        with unittest.mock.patch.object(
            mcp_server._safety,
            "set_config",
            return_value=devices.OkReason(ok=False, reason="out of range"),
        ):
            result = mcp_server.safety_set_tc_type("K")
        self.assertTrue(result.startswith("refused"))
        self.assertIn("out of range", result)


if __name__ == "__main__":
    unittest.main()
