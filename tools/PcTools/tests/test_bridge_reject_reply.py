#!/usr/bin/env python3
"""Unit tests for the PC-side decode of the {subcmd, ok, [reason]} reply
uart_bridge.c's bridge_reply_reject() and uart_bridge_ext.c's
bx_reply_ok_err() send for a *refused* mutating command on the PC<->ESP
link (ROADMAP.md "Future work -- KilnFW PC-link command acknowledgement",
firmware commit 5df2190).

Before this change, that reply either didn't exist (IO_CMD_SET_RELAY/
SET_RELAY_MASK, THERMO's silent SET_* paths) or existed but was decoded and
then thrown away (CONTROL_CMD_SET_ZONE_PID/SET_ZONE_MODEL/SET_UNIT_PREF --
devices.parse_control_response used to return a bare ``bool(payload[1])``
and never look past it). These tests are byte-exact against the wire shape
those two firmware functions actually emit, mirroring the vector-testing
convention test_safety_set_config.py already uses for SAFETY_CMD_SET_CONFIG.

No real UART/serial connection is used.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import (  # noqa: E402
    CONTROL_CMD_SET_ZONE_MODEL,
    CONTROL_CMD_SET_ZONE_PID,
    IO_CMD_SET_RELAY,
    IO_CMD_SET_RELAY_MASK,
    PROFILES_CMD_ACK_LAST_RUN,
    PROFILES_CMD_DELETE,
    PROFILES_CMD_PAUSE,
    PROFILES_CMD_RESUME,
    PROFILES_CMD_STOP,
    SAFETY_CMD_REQUEST_ENABLE,
    SAFETY_CMD_SET_CT_CAL,
    SAFETY_CMD_SET_FAULT_OUT,
    SAFETY_CMD_SET_POLL_PERIOD,
    THERMO_CMD_CLEAR_FAULTS,
    THERMO_CMD_CONFIG_CHANNEL,
    THERMO_CMD_SET_CJ_OFFSET,
    THERMO_CMD_SET_THRESHOLDS,
    THERMO_CMD_WRITE_REG,
)


def _reject_frame(subcmd: int, reason: "str | None") -> bytes:
    """Build the exact byte layout bridge_reply_reject()/bx_reply_ok_err()
    emit for a refusal: [subcmd, 0, len(reason), reason...] or just
    [subcmd, 0] when there is no reason."""
    out = bytes([subcmd, 0])
    if reason:
        encoded = reason.encode("ascii")
        out += bytes([len(encoded)]) + encoded
    return out


def _ok_frame(subcmd: int) -> bytes:
    return bytes([subcmd, 1])


class OkReasonDecodeTests(unittest.TestCase):
    """Direct tests of the shared decoder every task-specific parser calls."""

    def test_success_has_no_reason(self):
        result = devices._decode_ok_reason(_ok_frame(0xAA), devices.ControlResponseError, "x")
        self.assertTrue(result.ok)
        self.assertIsNone(result.reason)
        self.assertTrue(bool(result))  # OkReason must stay truthy on success

    def test_refusal_with_no_reason_text_is_still_a_refusal(self):
        # The plain 2-byte {subcmd, 0} shape every pre-existing caller could
        # already see (e.g. the unrecognized-subcommand default: case).
        result = devices._decode_ok_reason(_reject_frame(0xAA, None), devices.ControlResponseError, "x")
        self.assertFalse(result.ok)
        self.assertIsNone(result.reason)
        self.assertFalse(bool(result))

    def test_refusal_reason_text_survives_the_round_trip(self):
        result = devices._decode_ok_reason(
            _reject_frame(0xAA, "out of range"), devices.ControlResponseError, "x"
        )
        self.assertFalse(result.ok)
        self.assertEqual(result.reason, "out of range")

    def test_missing_ok_byte_raises(self):
        with self.assertRaises(devices.ControlResponseError):
            devices._decode_ok_reason(bytes([0xAA]), devices.ControlResponseError, "x")


class ControlSetZonePidReasonTests(unittest.TestCase):
    """CONTROL_CMD_SET_ZONE_PID/SET_ZONE_MODEL: the reason used to be decoded
    and silently discarded (parse_control_response returned bare bool).
    This is the exact regression this task's ROADMAP item calls out under
    "Update the tools/PcTools/src/kilnctrl side so the distinction reaches
    the operator... rather than being encoded and then discarded."."""

    def test_success_decodes_ok_true(self):
        subcmd, value = devices.parse_control_response(_ok_frame(CONTROL_CMD_SET_ZONE_PID))
        self.assertEqual(subcmd, CONTROL_CMD_SET_ZONE_PID)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_out_of_range_zone_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_control_response(
            _reject_frame(CONTROL_CMD_SET_ZONE_PID, "zone out of range")
        )
        self.assertEqual(subcmd, CONTROL_CMD_SET_ZONE_PID)
        self.assertFalse(value.ok)
        # This is the assertion that fails if the fix is reverted to
        # `bool(payload[1])`: the reason text would vanish entirely.
        self.assertEqual(value.reason, "zone out of range")

    def test_set_zone_model_reason_is_preserved(self):
        subcmd, value = devices.parse_control_response(
            _reject_frame(CONTROL_CMD_SET_ZONE_MODEL, "bad gain")
        )
        self.assertEqual(subcmd, CONTROL_CMD_SET_ZONE_MODEL)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "bad gain")

    def test_ok_reason_falsy_on_refusal_bool_compat(self):
        # OkReason must still satisfy `if not result:` the way the old bare
        # bool did, so pre-existing call sites that only check truthiness
        # keep working unchanged.
        _subcmd, value = devices.parse_control_response(
            _reject_frame(CONTROL_CMD_SET_ZONE_PID, "whatever")
        )
        self.assertFalse(value)


class IoSetRelayRefusalTests(unittest.TestCase):
    """IO_CMD_SET_RELAY/SET_RELAY_MASK: these had NO reply at all before
    firmware commit 5df2190, so parse_io_response previously raised
    IoResponseError("unknown IO response subcommand...") for any frame
    carrying this subcmd. The reason strings tested here (owned/safety/
    updating/truncated/out of range/driver error) are the literal ASCII
    uart_bridge.c's io_bridge_task() sends -- see its IO_CMD_SET_RELAY and
    IO_CMD_SET_RELAY_MASK cases."""

    def _assert_relay_refusal(self, subcmd: int, wire_reason: str, expected: devices.RelayRefusal):
        parsed_subcmd, value = devices.parse_io_response(_reject_frame(subcmd, wire_reason))
        self.assertEqual(parsed_subcmd, subcmd)
        self.assertIsInstance(value, devices.RelayResult)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason_text, wire_reason)
        self.assertEqual(value.refusal, expected)

    def test_owned_distinguishable_from_safety(self):
        self._assert_relay_refusal(IO_CMD_SET_RELAY, "owned", devices.RelayRefusal.OWNED)
        self._assert_relay_refusal(IO_CMD_SET_RELAY, "safety", devices.RelayRefusal.SAFETY)
        # The whole point: these two must not classify the same.
        owned = devices.parse_io_response(_reject_frame(IO_CMD_SET_RELAY, "owned"))[1]
        safety = devices.parse_io_response(_reject_frame(IO_CMD_SET_RELAY, "safety"))[1]
        self.assertNotEqual(owned.refusal, safety.refusal)

    def test_updating_distinguishable(self):
        self._assert_relay_refusal(IO_CMD_SET_RELAY, "updating", devices.RelayRefusal.UPDATING)

    def test_truncated_distinguishable(self):
        self._assert_relay_refusal(IO_CMD_SET_RELAY, "truncated", devices.RelayRefusal.TRUNCATED)

    def test_out_of_range_distinguishable(self):
        self._assert_relay_refusal(IO_CMD_SET_RELAY, "out of range", devices.RelayRefusal.OUT_OF_RANGE)

    def test_driver_error_distinguishable(self):
        self._assert_relay_refusal(IO_CMD_SET_RELAY, "driver error", devices.RelayRefusal.DRIVER_ERROR)

    def test_relay_mask_same_shape(self):
        self._assert_relay_refusal(IO_CMD_SET_RELAY_MASK, "owned", devices.RelayRefusal.OWNED)

    def test_unrecognized_reason_text_falls_back_to_other_but_keeps_text(self):
        parsed_subcmd, value = devices.parse_io_response(
            _reject_frame(IO_CMD_SET_RELAY, "some future reason")
        )
        self.assertEqual(parsed_subcmd, IO_CMD_SET_RELAY)
        self.assertEqual(value.refusal, devices.RelayRefusal.OTHER)
        self.assertEqual(value.reason_text, "some future reason")

    def test_relay_refusal_never_equals_success_shape(self):
        # A refusal and a hypothetical "ok" reply must not collapse into the
        # same decoded value -- this is the "success and safety-refusal look
        # identical" failure mode ROADMAP.md names explicitly.
        refusal_subcmd, refusal_value = devices.parse_io_response(
            _reject_frame(IO_CMD_SET_RELAY, "safety")
        )
        self.assertFalse(refusal_value.ok)
        self.assertNotEqual(refusal_value.ok, True)


class ThermoMutatingReasonTests(unittest.TestCase):
    """THERMO's mutating subcommands (CONFIG_CHANNEL/SET_THRESHOLDS/
    SET_CJ_OFFSET/CLEAR_FAULTS/WRITE_REG, etc) had NO reply at all before
    firmware commit 5df2190 -- thermo_bridge_task() fell straight through to
    the shared "log and drop" handling. parse_thermo_response() therefore
    used to raise ThermoResponseError("unknown THERMO response subcommand")
    for any frame carrying one of these subcmds; this is the PC-side half
    that decodes the reply the firmware now sends instead."""

    def test_config_channel_success_decodes_ok_true(self):
        subcmd, value = devices.parse_thermo_response(_ok_frame(THERMO_CMD_CONFIG_CHANNEL))
        self.assertEqual(subcmd, THERMO_CMD_CONFIG_CHANNEL)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_config_channel_out_of_range_reason_preserved(self):
        subcmd, value = devices.parse_thermo_response(
            _reject_frame(THERMO_CMD_CONFIG_CHANNEL, "out of range")
        )
        self.assertEqual(subcmd, THERMO_CMD_CONFIG_CHANNEL)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "out of range")

    def test_set_thresholds_truncated_reason_preserved(self):
        subcmd, value = devices.parse_thermo_response(
            _reject_frame(THERMO_CMD_SET_THRESHOLDS, "truncated")
        )
        self.assertEqual(subcmd, THERMO_CMD_SET_THRESHOLDS)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "truncated")

    def test_set_cj_offset_driver_error_reason_preserved(self):
        subcmd, value = devices.parse_thermo_response(
            _reject_frame(THERMO_CMD_SET_CJ_OFFSET, "driver error")
        )
        self.assertEqual(subcmd, THERMO_CMD_SET_CJ_OFFSET)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")

    def test_clear_faults_refusal_decodes_not_raises(self):
        subcmd, value = devices.parse_thermo_response(
            _reject_frame(THERMO_CMD_CLEAR_FAULTS, "out of range")
        )
        self.assertEqual(subcmd, THERMO_CMD_CLEAR_FAULTS)
        self.assertFalse(value.ok)

    def test_write_reg_refusal_decodes_not_raises(self):
        subcmd, value = devices.parse_thermo_response(
            _reject_frame(THERMO_CMD_WRITE_REG, "out of range")
        )
        self.assertEqual(subcmd, THERMO_CMD_WRITE_REG)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "out of range")


class SafetyMutatingReasonTests(unittest.TestCase):
    """SAFETY's REQUEST_ENABLE/SET_POLL_PERIOD/SET_FAULT_OUT/SET_CT_CAL had
    truncated/out-of-range guards added to safety_bridge_task() alongside
    THERMO/IO (5df2190's commit message: "truncated args and out-of-range
    args across THERMO/IO/SAFETY"), but nothing on the PC side waited for or
    decoded the reply -- parse_safety_response() had no case for any of
    these subcmds at all."""

    def test_request_enable_success_decodes_ok_true(self):
        subcmd, value = devices.parse_safety_response(_ok_frame(SAFETY_CMD_REQUEST_ENABLE))
        self.assertEqual(subcmd, SAFETY_CMD_REQUEST_ENABLE)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_request_enable_truncated_reason_preserved(self):
        subcmd, value = devices.parse_safety_response(
            _reject_frame(SAFETY_CMD_REQUEST_ENABLE, "truncated")
        )
        self.assertEqual(subcmd, SAFETY_CMD_REQUEST_ENABLE)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "truncated")

    def test_set_poll_period_truncated_reason_preserved(self):
        subcmd, value = devices.parse_safety_response(
            _reject_frame(SAFETY_CMD_SET_POLL_PERIOD, "truncated")
        )
        self.assertEqual(subcmd, SAFETY_CMD_SET_POLL_PERIOD)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "truncated")

    def test_set_fault_out_truncated_reason_preserved(self):
        subcmd, value = devices.parse_safety_response(
            _reject_frame(SAFETY_CMD_SET_FAULT_OUT, "truncated")
        )
        self.assertEqual(subcmd, SAFETY_CMD_SET_FAULT_OUT)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "truncated")

    def test_set_ct_cal_out_of_range_reason_preserved(self):
        subcmd, value = devices.parse_safety_response(
            _reject_frame(SAFETY_CMD_SET_CT_CAL, "out of range")
        )
        self.assertEqual(subcmd, SAFETY_CMD_SET_CT_CAL)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "out of range")


class ProfilesLifecycleReasonTests(unittest.TestCase):
    """DELETE/PAUSE/RESUME/ACK_LAST_RUN/STOP used to decode the reply and
    then discard everything but the ok byte
    (``return subcommand, bool(payload[1])``) -- the same bug class as
    CONTROL's SET_ZONE_PID/SET_ZONE_MODEL fix above, just not reached by
    that pass's scope. e.g. DELETE's "cannot delete a builtin profile"
    refusal used to vanish entirely."""

    def test_delete_success_decodes_ok_true(self):
        subcmd, value = devices.parse_profiles_response(_ok_frame(PROFILES_CMD_DELETE))
        self.assertEqual(subcmd, PROFILES_CMD_DELETE)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_delete_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_profiles_response(
            _reject_frame(PROFILES_CMD_DELETE, "cannot delete a builtin profile")
        )
        self.assertEqual(subcmd, PROFILES_CMD_DELETE)
        self.assertFalse(value.ok)
        # This is the assertion that fails if the fix is reverted to
        # `bool(payload[1])`: the reason text would vanish entirely.
        self.assertEqual(value.reason, "cannot delete a builtin profile")

    def test_pause_reason_is_preserved(self):
        subcmd, value = devices.parse_profiles_response(
            _reject_frame(PROFILES_CMD_PAUSE, "nothing running")
        )
        self.assertEqual(subcmd, PROFILES_CMD_PAUSE)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "nothing running")

    def test_resume_reason_is_preserved(self):
        subcmd, value = devices.parse_profiles_response(
            _reject_frame(PROFILES_CMD_RESUME, "nothing paused")
        )
        self.assertEqual(subcmd, PROFILES_CMD_RESUME)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "nothing paused")

    def test_ack_last_run_reason_is_preserved(self):
        subcmd, value = devices.parse_profiles_response(
            _reject_frame(PROFILES_CMD_ACK_LAST_RUN, "nothing to acknowledge")
        )
        self.assertEqual(subcmd, PROFILES_CMD_ACK_LAST_RUN)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "nothing to acknowledge")

    def test_stop_reason_is_preserved(self):
        subcmd, value = devices.parse_profiles_response(
            _reject_frame(PROFILES_CMD_STOP, "nothing running to stop")
        )
        self.assertEqual(subcmd, PROFILES_CMD_STOP)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "nothing running to stop")

    def test_ok_reason_falsy_on_refusal_bool_compat(self):
        # OkReason must still satisfy `if not result:` the way the old bare
        # bool did, so pre-existing call sites that only check truthiness
        # keep working unchanged.
        _subcmd, value = devices.parse_profiles_response(
            _reject_frame(PROFILES_CMD_DELETE, "whatever")
        )
        self.assertFalse(value)


if __name__ == "__main__":
    unittest.main()
