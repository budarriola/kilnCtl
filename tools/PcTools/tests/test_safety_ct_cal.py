#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_get_ct_cal()/SafetyCtCal -- the
PC-facing side of SAFETY_CMD_GET_CT_CAL (request, 0x22) / SAFETY_CMD_CT_CAL
(reply, 0x1A -- CommonFW/docs/LINK_PROTOCOL.md sec 4/6), plus
mcp_server.safety_get_ct_cal().

2026-09-08: the SET_CT_CAL (0x19) write surface -- devices.safety_set_ct_cal(),
SafetyClient.set_ct_cal(), mcp_server.safety_set_ct_cal() -- was removed. It
wrote config_store.h's legacy `ct_cal[]` end-to-end amps CORRECTION, which is
applied to `amps[]` after the ADC-counts-to-amps conversion and only ever fed
the S14/S15 WARN-only over/under-current display thresholds -- never S3/S4/S9
presence detection (that reads `zero_counts`/`k_ct_v_per_a` directly, a
completely different field). An agent used it trying to clear a latched S3
trip; it silently did nothing relevant. See
`docs/CURRENT_SENSE.md` section 5 / `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`
for the real calibration surface (A_fs/zero_mv via the ESP commissioning
page, a differently-shaped endpoint that happens to share the "ct_cal" name).
`CtCalWriteSurfaceRemovedTests` below is the negative test proving the
removal: it fails loudly if either function/tool ever comes back.

Through firmware protocol version 6, GET_CT_CAL's request and reply shared
one wire id (0x1A), distinguished only by direction and length. Version 7
split the request onto its own id (0x22) so a driver-error refusal reply --
neither 1 byte nor 28 -- can be told apart from a malformed/truncated
success reply; see protocol.py's SAFETY_CMD_GET_CT_CAL doc comment and
SafetyGetCtCalRefusalTests below for the case this split exists to enable.

No real UART/serial connection is used -- this only checks the byte-exact
wire encoding/decoding. The GET reply vector mirrors
firmware/CommonFW/test/test_ct_cal.c's test_vector_all_zero (28 zero bytes
after the 0x1A command byte -- the REPLY's own id, unchanged).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl import mcp_server  # noqa: E402
from kilnctrl.protocol import (  # noqa: E402
    SAFETY_CMD_CT_CAL,
    SAFETY_CMD_GET_CT_CAL,
)


class CtCalWriteSurfaceRemovedTests(unittest.TestCase):
    """Negative test for the removal itself: proves the dead write surface
    cannot silently come back under any of its three names. See module
    docstring."""

    def test_devices_has_no_safety_set_ct_cal(self):
        self.assertFalse(
            hasattr(devices, "safety_set_ct_cal"),
            "devices.safety_set_ct_cal() was removed 2026-09-08 -- see module docstring",
        )

    def test_safety_client_has_no_set_ct_cal_method(self):
        from kilnctrl.safety import SafetyClient

        self.assertFalse(
            hasattr(SafetyClient, "set_ct_cal"),
            "SafetyClient.set_ct_cal() was removed 2026-09-08 -- see module docstring",
        )

    def test_mcp_server_has_no_safety_set_ct_cal_tool(self):
        self.assertFalse(
            hasattr(mcp_server, "safety_set_ct_cal"),
            "mcp_server.safety_set_ct_cal() was removed 2026-09-08 -- see module docstring",
        )


class SafetyGetCtCalEncodeTests(unittest.TestCase):
    def test_request_is_cmd_byte_only(self):
        self.assertEqual(devices.safety_get_ct_cal(), bytes([SAFETY_CMD_GET_CT_CAL]))


def _build_ct_cal_vector(channels):
    """Byte-exact mirror of kilnlink_ct_cal_encode() -- each channel is
    calibrated(u8) + gain(f32 LE) + offset(f32 LE), 9 bytes, in order. Uses
    the REPLY's own id (SAFETY_CMD_CT_CAL, 0x1A) -- not
    SAFETY_CMD_GET_CT_CAL (0x22, the request's id) -- since version 7."""
    out = bytearray([SAFETY_CMD_CT_CAL])
    for calibrated, gain, offset in channels:
        out.append(1 if calibrated else 0)
        out += struct.pack("<f", gain)
        out += struct.pack("<f", offset)
    return bytes(out)


class SafetyCtCalParseTests(unittest.TestCase):
    def test_all_zero_vector_matches_commonfw_test_ct_cal(self):
        # firmware/CommonFW/test/test_ct_cal.c's test_vector_all_zero(): 28
        # zero bytes (cmd 0x1A + 3 * 9 zero bytes).
        vector = bytes([0x1A] + [0x00] * 27)
        self.assertEqual(len(vector), 28)
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_CT_CAL)
        self.assertIsInstance(value, devices.SafetyCtCal)
        self.assertEqual(len(value.channels), 3)
        for ch in value.channels:
            self.assertFalse(ch.calibrated)
            self.assertEqual(ch.gain, 0.0)
            self.assertEqual(ch.offset, 0.0)

    def test_mixed_channels_round_trip_independently(self):
        # Mirrors test_ct_cal.c's test_round_trip_mixed(): channel 0
        # calibrated, channel 1 uncalibrated, channel 2 calibrated with
        # different constants -- proves per-channel independence on decode.
        vector = _build_ct_cal_vector(
            [
                (True, 1.021, -0.03),
                (False, 0.0, 0.0),
                (True, 0.987, 0.11),
            ]
        )
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_CT_CAL)
        self.assertTrue(value.channels[0].calibrated)
        self.assertAlmostEqual(value.channels[0].gain, 1.021, places=5)
        self.assertAlmostEqual(value.channels[0].offset, -0.03, places=5)
        self.assertFalse(value.channels[1].calibrated)
        self.assertEqual(value.channels[1].gain, 0.0)
        self.assertTrue(value.channels[2].calibrated)
        self.assertAlmostEqual(value.channels[2].gain, 0.987, places=5)
        self.assertAlmostEqual(value.channels[2].offset, 0.11, places=5)

    # -- negative tests: prove each check can actually fail ------------------

    def test_baseline_vector_is_accepted(self):
        vector = _build_ct_cal_vector([(True, 1.0, 0.0), (True, 1.0, 0.0), (True, 1.0, 0.0)])
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_CT_CAL)
        self.assertTrue(value.channels[0].calibrated)

    def test_truncated_payload_rejected(self):
        vector = _build_ct_cal_vector([(True, 1.0, 0.0), (True, 1.0, 0.0), (True, 1.0, 0.0)])
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector[:-1])

    def test_oversized_payload_rejected(self):
        vector = _build_ct_cal_vector([(True, 1.0, 0.0), (True, 1.0, 0.0), (True, 1.0, 0.0)])
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector + b"\x00")

    def test_calibrated_flag_flip_is_visible(self):
        # Prove test_mixed_channels_round_trip_independently's
        # assertFalse(value.channels[1].calibrated) can actually fail: an
        # otherwise-identical vector with channel 1 calibrated must report
        # True instead.
        vector = _build_ct_cal_vector(
            [(True, 1.0, 0.0), (True, 2.0, 0.5), (True, 1.0, 0.0)]
        )
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertTrue(value.channels[1].calibrated)
        self.assertEqual(value.channels[1].gain, 2.0)


class SafetyGetCtCalIdSeparationTests(unittest.TestCase):
    """Proves the version-7 fix this whole pass exists for: GET_CT_CAL's
    request and reply must decode under DIFFERENT ids, and a driver-error
    refusal (uart_bridge.c's bridge_reply_reject(), {0x22, ok=0, reason})
    must be decodable at all -- something the old shared-0x1A scheme could
    never do without colliding with a truncated/malformed success reply."""

    def test_request_and_reply_ids_differ(self):
        self.assertNotEqual(SAFETY_CMD_GET_CT_CAL, SAFETY_CMD_CT_CAL)

    def test_refusal_reply_decodes_as_ok_reason(self):
        # {subcmd=0x22, ok=0, len=12, "driver error"} -- exactly what
        # uart_bridge.c's safety_bridge_task() now sends when
        # safety_link_get_ct_cal()'s round trip to the Pico fails outright.
        reason = b"driver error"
        vector = bytes([SAFETY_CMD_GET_CT_CAL, 0, len(reason)]) + reason
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_CT_CAL)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")

    def test_refusal_reply_is_never_mistaken_for_a_success_reply(self):
        reason = b"driver error"
        vector = bytes([SAFETY_CMD_GET_CT_CAL, 0, len(reason)]) + reason
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertNotIsInstance(value, devices.SafetyCtCal)

    def test_get_ct_cal_raises_on_refusal_not_returns_wrong_type(self):
        # SafetyClient.get_ct_cal() must turn an OkReason refusal into
        # SafetyQueryError, not hand a caller expecting SafetyCtCal something
        # of the wrong shape.
        from kilnctrl.safety import SafetyQueryError

        with unittest.mock.patch.object(
            mcp_server._safety,
            "_query",
            return_value=devices.OkReason(ok=False, reason="driver error"),
        ):
            with self.assertRaises(SafetyQueryError):
                mcp_server._safety.get_ct_cal()


class SafetyGetCtCalToolTests(unittest.TestCase):
    def test_reports_calibrated_and_uncalibrated_channels(self):
        cal = devices.SafetyCtCal(
            channels=(
                devices.SafetyCtCalChannel(calibrated=True, gain=1.021, offset=-0.03),
                devices.SafetyCtCalChannel(calibrated=False, gain=0.0, offset=0.0),
                devices.SafetyCtCalChannel(calibrated=True, gain=0.987, offset=0.11),
            )
        )
        with unittest.mock.patch.object(mcp_server._safety, "get_ct_cal", return_value=cal):
            result = mcp_server.safety_get_ct_cal()
        self.assertIn("channel 0: calibrated", result)
        self.assertIn("channel 1: uncalibrated", result)
        self.assertIn("channel 2: calibrated", result)

    def test_query_error_surfaces_as_error_string_not_exception(self):
        from kilnctrl.safety import SafetyQueryError

        with unittest.mock.patch.object(
            mcp_server._safety, "get_ct_cal", side_effect=SafetyQueryError("timed out")
        ):
            result = mcp_server.safety_get_ct_cal()
        self.assertEqual(result, "error: timed out")


if __name__ == "__main__":
    unittest.main()
