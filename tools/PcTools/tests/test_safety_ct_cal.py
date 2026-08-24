#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_set_ct_cal()/safety_get_ct_cal()/
SafetyCtCal -- the PC-facing side of SAFETY_CMD_SET_CT_CAL (0x19) and
SAFETY_CMD_GET_CT_CAL (request, 0x22) / SAFETY_CMD_CT_CAL (reply, 0x1A --
CommonFW/docs/LINK_PROTOCOL.md sec 4/6), plus mcp_server.safety_set_ct_cal()/
safety_get_ct_cal().

Through firmware protocol version 6, GET_CT_CAL's request and reply shared
one wire id (0x1A), distinguished only by direction and length. Version 7
split the request onto its own id (0x22) so a driver-error refusal reply --
neither 1 byte nor 28 -- can be told apart from a malformed/truncated
success reply; see protocol.py's SAFETY_CMD_GET_CT_CAL doc comment and
SafetyGetCtCalRefusalTests below for the case this split exists to enable.

No real UART/serial connection is used -- this only checks the byte-exact
wire encoding/decoding. The SET_CT_CAL vectors below are the same two
byte-exact vectors firmware/CommonFW/test/test_set_ct_cal.c hand-computed
(there is no vectors.json for this codec yet, on either side):

    test_vector_zero:            {0x19,0,0,0,0,0,0,0,0,0,0}
    test_vector_channel2_gain1:  {0x19,0x02,0x01,0x00,0x00,0x80,0x3f,
                                   0x00,0x00,0x00,0x00}

and the GET reply vector mirrors test_ct_cal.c's test_vector_all_zero (28
zero bytes after the 0x1A command byte -- the REPLY's own id, unchanged).

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
    SAFETY_CMD_SET_CT_CAL,
)


class SafetySetCtCalEncodeTests(unittest.TestCase):
    def test_vector_zero_matches_commonfw_test_set_ct_cal(self):
        # firmware/CommonFW/test/test_set_ct_cal.c's test_vector_zero().
        expected = bytes([0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        self.assertEqual(devices.safety_set_ct_cal(0, False, 0.0, 0.0), expected)

    def test_vector_channel2_gain1_matches_commonfw_test_set_ct_cal(self):
        # firmware/CommonFW/test/test_set_ct_cal.c's test_vector_channel2_gain1():
        # channel=2, calibrated=1, gain=1.0f (0x3F800000 LE), offset=0.0f.
        expected = bytes(
            [0x19, 0x02, 0x01, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00]
        )
        self.assertEqual(devices.safety_set_ct_cal(2, True, 1.0, 0.0), expected)

    def test_length_is_eleven_bytes(self):
        self.assertEqual(len(devices.safety_set_ct_cal(1, True, 1.0321, -0.045)), 11)

    def test_channel_out_of_range_rejected(self):
        with self.assertRaises(ValueError):
            devices.safety_set_ct_cal(3, True, 1.0, 0.0)

    def test_channel_negative_rejected(self):
        with self.assertRaises(ValueError):
            devices.safety_set_ct_cal(-1, True, 1.0, 0.0)

    def test_nan_gain_rejected(self):
        with self.assertRaises(ValueError):
            devices.safety_set_ct_cal(0, True, float("nan"), 0.0)

    def test_inf_offset_rejected(self):
        with self.assertRaises(ValueError):
            devices.safety_set_ct_cal(0, True, 1.0, float("inf"))


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


class SafetySetCtCalToolTests(unittest.TestCase):
    # 2026-08-24: safety_set_ct_cal() now goes through
    # SafetyClient.set_ct_cal() (waits a short window for the optional
    # ESP-side refusal reply -- ROADMAP.md "KilnFW PC-link command
    # acknowledgement") instead of the raw fire-and-forget mcp_server._send.
    # These patch SafetyClient.set_ct_cal directly rather than _send.
    def test_sends_expected_bytes(self):
        with unittest.mock.patch.object(
            mcp_server._safety, "set_ct_cal", return_value=devices.OkReason(ok=True)
        ) as mock_set_ct_cal:
            result = mcp_server.safety_set_ct_cal(2, True, 1.0, 0.0)
        self.assertTrue(result.startswith("ok"))
        mock_set_ct_cal.assert_called_once_with(2, True, 1.0, 0.0)

    def test_out_of_range_channel_never_reaches_the_link(self):
        # devices.safety_set_ct_cal() validates the channel range while
        # building the payload, inside SafetyClient.set_ct_cal() -- one layer
        # deeper than before this pass, but still before anything touches the
        # wire. Patch the link itself (not set_ct_cal, which would mock the
        # validation away too) to prove that.
        with unittest.mock.patch.object(mcp_server._link, "send") as mock_send:
            result = mcp_server.safety_set_ct_cal(5, True, 1.0, 0.0)
        mock_send.assert_not_called()
        self.assertTrue(result.startswith("error:"))

    def test_refusal_reason_reaches_the_caller(self):
        # The regression this pass fixes: a truncated/out-of-range refusal
        # used to be silently dropped in SafetyClient's own consumer thread
        # (mcp_server called the fire-and-forget _send, which never waited
        # for the reply). Now it must show up in the returned string.
        with unittest.mock.patch.object(
            mcp_server._safety,
            "set_ct_cal",
            return_value=devices.OkReason(ok=False, reason="out of range"),
        ):
            result = mcp_server.safety_set_ct_cal(2, True, 1.0, 0.0)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("out of range", result)


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
