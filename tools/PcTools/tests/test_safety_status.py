#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.SafetyStatus/parse_safety_response()'s
SAFETY_CMD_GET_STATUS (0x01) path -- the PC-facing mirror of the isolated
Pico<->ESP link's Frame A, described in devices.py's own parse_safety_
response() docstring and built ESP-side by safety_link_build_status_payload()
(firmware/KilnFW/App/drivers/safety_link.c).

2026-08-23, the DIAG-frame-went-dark investigation: this payload grew from a
fixed 25 bytes to an ADDITIVE 25-or-27 (byte25 tx_dropped_sat, byte26
extra_flags bit0 tx_dropped_known) -- closing the last hop of a TX-ring drop
counter that reached SaftyFW's own Frame A and KilnFW's safety_link_status_t
cache but stopped there, invisible to this tool. No test of this reply
existed in this suite before that change (grep found none), so this file
covers both the pre-existing byte layout (temperature/flags/etc, now proven
correct by these tests rather than merely assumed) and the new V1/V2 split.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_GET_STATUS, SafetyFlag, ThermoFault  # noqa: E402


def _build_v1_vector(
    *,
    flags=0,
    temperature=500.0,
    cold_junction=25.0,
    fault_status=0,
    current=(1.0, 2.0, 3.0),
    age_ms=100,
):
    """Byte-exact mirror of safety_link_build_status_payload()'s first 25
    bytes (V1 layout, unchanged by the 2026-08-23 pass)."""
    return struct.pack(
        "<BBffBfffH",
        SAFETY_CMD_GET_STATUS,
        flags,
        temperature,
        cold_junction,
        fault_status,
        current[0],
        current[1],
        current[2],
        age_ms,
    )


def _build_v2_vector(*, tx_dropped_sat=0, tx_dropped_known=True, **v1_kwargs):
    """V1 vector plus the two 2026-08-23 bytes."""
    extra_flags = 0x01 if tx_dropped_known else 0x00
    return _build_v1_vector(**v1_kwargs) + struct.pack("<BB", tx_dropped_sat, extra_flags)


class TestSafetyStatusV1(unittest.TestCase):
    """The original 25-byte layout must keep decoding exactly as before,
    with tx_dropped_sat now explicitly None (not 0, not omitted) rather than
    silently absent."""

    def test_v1_decodes_and_tx_dropped_is_unknown(self):
        vector = _build_v1_vector(
            flags=int(SafetyFlag.ESTOP | SafetyFlag.RELAY),
            temperature=651.5,
            cold_junction=22.25,
            fault_status=0x03,
            current=(1.25, 2.5, 3.75),
            age_ms=250,
        )
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_STATUS)
        self.assertTrue(value.estop)
        self.assertTrue(value.relay_energized)
        self.assertAlmostEqual(value.temperature_c, 651.5, places=3)
        self.assertAlmostEqual(value.cold_junction_c, 22.25, places=3)
        self.assertEqual(value.fault_status, ThermoFault(0x03))
        self.assertEqual(value.current_a, (1.25, 2.5, 3.75))
        self.assertEqual(value.age_ms, 250)
        self.assertIsNone(
            value.tx_dropped_sat,
            "a V1 (25-byte) reply must decode tx_dropped_sat as None -- "
            "there is no byte 25/26 to read it from",
        )

    def test_v1_describe_reports_unknown_not_zero(self):
        subcommand, value = devices.parse_safety_response(_build_v1_vector())
        text = value.describe()
        self.assertIn("unknown", text.lower())
        self.assertNotIn("tx_dropped 0", text)


class TestSafetyStatusV2(unittest.TestCase):
    """The new 27-byte layout: tx_dropped_sat is real data only when
    extra_flags bit0 is set."""

    def test_v2_known_decodes_the_real_count(self):
        subcommand, value = devices.parse_safety_response(
            _build_v2_vector(tx_dropped_sat=42, tx_dropped_known=True)
        )
        self.assertEqual(value.tx_dropped_sat, 42)

    def test_v2_unknown_bit_clear_is_still_none_despite_a_nonzero_byte(self):
        # The byte itself carries a nonzero value, but extra_flags bit0 is
        # clear -- proves tx_dropped_known gates the byte, the byte's own
        # value is never trusted on its own (same discipline
        # safety_apply_status()/mirror_apply_status() already use for
        # tx_dropped_known on the Pico<->ESP hop this mirrors).
        subcommand, value = devices.parse_safety_response(
            _build_v2_vector(tx_dropped_sat=77, tx_dropped_known=False)
        )
        self.assertIsNone(value.tx_dropped_sat)

    def test_v2_saturated_value_reports_as_254_plus_in_describe(self):
        subcommand, value = devices.parse_safety_response(
            _build_v2_vector(tx_dropped_sat=255, tx_dropped_known=True)
        )
        self.assertEqual(value.tx_dropped_sat, 255)
        self.assertIn("254+", value.describe())

    def test_v2_zero_known_is_a_real_zero_not_unknown(self):
        # The other half of the "must distinguish no-drops from
        # cannot-tell-me" requirement: 0 with the known bit SET is a real,
        # trustworthy zero.
        subcommand, value = devices.parse_safety_response(
            _build_v2_vector(tx_dropped_sat=0, tx_dropped_known=True)
        )
        self.assertEqual(value.tx_dropped_sat, 0)
        self.assertIn("tx_dropped 0", value.describe())


class TestSafetyStatusNegative(unittest.TestCase):
    """Length is not "at least 25" or "any length" -- exactly {25, 27},
    same discipline the isolated link's own V1/V2 split enforces."""

    def test_length_24_rejected(self):
        vector = _build_v1_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_26_rejected(self):
        # One byte short of V2 -- not V1 (too long) and not V2 (too short).
        vector = _build_v2_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_28_rejected(self):
        vector = _build_v2_vector() + b"\x00"
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)


if __name__ == "__main__":
    unittest.main()
