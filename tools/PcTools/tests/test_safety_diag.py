#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_get_diag()/SafetyDiag.describe() --
backs the new safety_get_diag MCP tool (mcp_server_safety.py), added to close
the gap that an unexpected Pico reboot had no PC-side readout of *why*
(watchdog vs. power-on vs. brownout). The wire already carries this
(kilnlink_diag.h's boot_reason byte, GET_DIAG 0x0C, 27 bytes) -- this only
adds the human-facing describe() and proves the decode is byte-exact,
matching devices_safety.py's parse_safety_response() GET_DIAG branch.

No real UART/serial connection is used.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_GET_DIAG  # noqa: E402


def _build_vector(
    *,
    ever_received=True,
    trip_reason=0,
    warn_mask=0,
    trip_mask=0,
    uptime_ms=12345,
    boot_reason=0x02,  # KILNLINK_DIAG_BOOT_WATCHDOG
    context_age_100ms=5,
    context_frames_ok=100,
    context_frames_bad=0,
    tx_frames_dropped=0,
    state=2,  # armed
    flags=0,
):
    """Byte-exact mirror of the GET_DIAG (0x0C) reply layout -- see
    devices_safety.py's parse_safety_response() doc comment."""
    out = bytearray()
    out.append(SAFETY_CMD_GET_DIAG)
    out += struct.pack(
        "<BBHHIBBIIIBB",
        1 if ever_received else 0,
        trip_reason,
        warn_mask,
        trip_mask,
        uptime_ms,
        boot_reason,
        context_age_100ms,
        context_frames_ok,
        context_frames_bad,
        tx_frames_dropped,
        state,
        flags,
    )
    return bytes(out)


class SafetyGetDiagRequestTests(unittest.TestCase):
    def test_request_is_cmd_byte_only(self):
        self.assertEqual(devices.safety_get_diag(), bytes([SAFETY_CMD_GET_DIAG]))


class SafetyDiagParseTests(unittest.TestCase):
    def test_never_received_reports_ever_received_false(self):
        vector = _build_vector(ever_received=False)
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_DIAG)
        self.assertIsInstance(value, devices.SafetyDiag)
        self.assertFalse(value.ever_received)

    def test_watchdog_boot_reason_decoded(self):
        vector = _build_vector(ever_received=True, boot_reason=0x02)
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(value.boot_reason, 0x02)

    def test_wrong_length_is_rejected(self):
        vector = _build_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)


class SafetyDiagDescribeTests(unittest.TestCase):
    """describe() backs the safety_get_diag MCP tool -- these check its text
    directly rather than going through the tool wrapper, since the wrapper
    is a one-line passthrough."""

    def test_never_received_is_named_explicitly(self):
        vector = _build_vector(ever_received=False)
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("never received", text)

    def test_watchdog_reboot_is_named(self):
        vector = _build_vector(ever_received=True, boot_reason=0x02)
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("watchdog", text)
        self.assertNotIn("power-on", text)

    def test_poweron_reboot_is_named(self):
        vector = _build_vector(ever_received=True, boot_reason=0x01)
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("power-on", text)
        self.assertNotIn("watchdog", text)

    def test_combined_watchdog_and_brownout_names_both(self):
        # Prove multiple simultaneous boot_reason bits are not silently
        # collapsed to just the first match -- both must be named.
        vector = _build_vector(ever_received=True, boot_reason=0x02 | 0x04)
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("watchdog", text)
        self.assertIn("brownout", text)

    def test_unknown_boot_reason_bits_are_not_hidden(self):
        # Negative test: prove describe() actually branches on the known
        # bits rather than always printing something plausible-looking --
        # an all-zero boot_reason (no bit set, a value the real firmware
        # never sends but the wire format does not forbid) must render as
        # explicitly unknown, not silently as one of the named reasons.
        vector = _build_vector(ever_received=True, boot_reason=0x00)
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("unknown", text)
        self.assertNotIn("watchdog", text)
        self.assertNotIn("power-on", text)
        self.assertNotIn("brownout", text)

    def test_state_and_masks_are_reported(self):
        vector = _build_vector(
            ever_received=True,
            boot_reason=0x01,
            state=4,
            trip_reason=3,
            warn_mask=0x0002,
            trip_mask=0x0004,
        )
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("tripped", text)
        self.assertIn("trip_reason 3", text)
        self.assertIn("0x0002", text)
        self.assertIn("0x0004", text)

    def test_context_never_received_is_named(self):
        vector = _build_vector(ever_received=True, context_age_100ms=0xFF)
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertTrue(value.context_never_received)
        text = value.describe()
        self.assertIn("context age never received", text)

    def test_trip_mask_for_reason_matches_firmware_mapping(self):
        """Pins safety_trip_mask_for_reason() against SaftyFW's own
        safety_trip_t enum (firmware/SaftyFW/docs/ARCHITECTURE.md) and its
        link_frame_trip_mask_for_reason() (link_frame.c): mask = 1 <<
        (reason - 1), reason 0 -> mask 0. This is the exact relationship
        CLAUDE.md got wrong for S6a on 2026-09-09 (documented bit 6/0x0040,
        actually bit 5/0x0020) -- if this ever regresses (e.g. a future
        edit reintroduces an off-by-one), this test must go red."""
        from kilnctrl.devices_safety import safety_trip_mask_for_reason

        expected = {
            0: 0x0000,  # SAFETY_TRIP_NONE
            1: 0x0001,  # S1  SAFETY_TRIP_OVERTEMP
            2: 0x0002,  # S2  SAFETY_TRIP_OVER_SETPOINT
            3: 0x0004,  # S3  SAFETY_TRIP_LOAD_STUCK_ON
            5: 0x0010,  # S5  SAFETY_TRIP_SENSOR_INVALID
            6: 0x0020,  # S6a SAFETY_TRIP_MAIN_FAULT  (NOT 0x0040)
            7: 0x0040,  # S6b SAFETY_TRIP_LINK_DEAD
            8: 0x0080,  # S7  SAFETY_TRIP_ESTOP
            9: 0x0100,  # S8  SAFETY_TRIP_RATE
            10: 0x0200,  # S9  SAFETY_TRIP_INEFFECTIVE
            12: 0x0800,  # S11 SAFETY_TRIP_FROZEN_SENSOR
            13: 0x1000,  # S12 SAFETY_TRIP_ENCLOSURE_TEMP
            14: 0x2000,  # S13 SAFETY_TRIP_BORROWED_STALE
            15: 0x4000,  # SAFETY_TRIP_CONFIG_CORRUPT
            16: 0x8000,  # SAFETY_TRIP_SELF_TEST
        }
        for reason, mask in expected.items():
            self.assertEqual(
                safety_trip_mask_for_reason(reason),
                mask,
                f"reason {reason} should produce mask 0x{mask:04x}",
            )

    def test_describe_names_the_reason_and_flags_mask_mismatch(self):
        """describe() must decode trip_reason into its enum name/guard tag
        (so a reader never has to compute 1 << (reason - 1) by hand), and
        must call out a wire trip_mask that disagrees with what trip_reason
        implies rather than printing it silently -- the exact ambiguity
        that let 'trip_reason 6 | trip_mask 0x0040' pass as innocuous."""
        vector = _build_vector(ever_received=True, state=4, trip_reason=6, trip_mask=0x0020)
        _subcommand, value = devices.parse_safety_response(vector)
        text = value.describe()
        self.assertIn("SAFETY_TRIP_MAIN_FAULT", text)
        self.assertIn("S6a", text)
        self.assertNotIn("MISMATCH", text)

        bad_vector = _build_vector(ever_received=True, state=4, trip_reason=6, trip_mask=0x0040)
        _subcommand, bad_value = devices.parse_safety_response(bad_vector)
        bad_text = bad_value.describe()
        self.assertIn("MISMATCH", bad_text)


if __name__ == "__main__":
    unittest.main()
