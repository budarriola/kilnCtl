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


if __name__ == "__main__":
    unittest.main()
