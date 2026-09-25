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
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import (  # noqa: E402
    AUTOTUNE_CMD_ABORT,
    AUTOTUNE_CMD_ACCEPT,
    CONTROL_CMD_SET_ZONE_MODEL,
    CONTROL_CMD_SET_ZONE_PID,
    DISPLAY_CMD_CLEAR,
    DISPLAY_CMD_PRINT,
    DISPLAY_CMD_READ_ID,
    DISPLAY_CMD_RESET,
    IO_CMD_ALL_RELAYS_OFF,
    IO_CMD_SET_IO,
    IO_CMD_SET_RELAY,
    IO_CMD_SET_RELAY_MASK,
    IO_CMD_SX_RESET,
    IO_CMD_SX_SCAN,
    IO_CMD_SX_WRITE_REG,
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
    THERMO_CMD_READ_FAULTS,
    THERMO_CMD_SET_CJ_OFFSET,
    THERMO_CMD_SET_THRESHOLDS,
    THERMO_CMD_WRITE_REG,
    TOUCH_CMD_GET_STATE,
    TOUCH_CMD_INJECT,
    TOUCH_CMD_LOG_TAP_TARGETS,
    TOUCH_CMD_SET_TAP_DUMP,
    WIFI_CMD_ADD_NETWORK,
    WIFI_CMD_FORGET,
    WIFI_CMD_SET_AP_IDENTITY,
    WIFI_CMD_SET_MODE,
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


class DisplayTouchDriverErrorTests(unittest.TestCase):
    """2026-08-24: display_bridge_task()/touch_bridge_task() now send this
    same {subcmd, ok=0, reason} refusal on a driver error too (uart_bridge.c),
    same fix as THERMO/IO/SAFETY. Both queries answer under the SAME id as
    their own success reply (there is no second id to split onto, unlike
    GET_CT_CAL/GET_PARAM/GET_CONFIG_PAGE), so the risk is a decoder with a
    loose length check misreading the refusal as a malformed-but-accepted
    success reply instead of telling refusal and success apart -- exactly
    the class of bug CommonFW/docs/LINK_PROTOCOL.md's "Request/reply ids
    must never be shared" rule is about, just solved here with a strict
    length check instead of a second id.

    READ_ID and GET_STATE differ in what "told apart" means, though:
    DISPLAY_CMD_READ_ID's case sets ``err = ESP_OK`` unconditionally, so it
    can never actually fall into the bottom-of-task driver-error reject --
    a non-9-byte frame under that id is simply malformed, and
    parse_display_response() keeps raising. TOUCH_CMD_GET_STATE's
    screen_idle_get_state() call genuinely can fail, so a non-{6,23}-byte
    frame under that id is decoded into an OkReason refusal instead of
    being rejected as unparseable -- see the two tests below."""

    def test_display_read_id_refusal_raises_not_silently_wrong(self):
        frame = _reject_frame(DISPLAY_CMD_READ_ID, "driver error")
        self.assertNotEqual(len(frame), 9)  # proves this can't collide with the 9-byte success shape
        with self.assertRaises(devices.DisplayResponseError):
            devices.parse_display_response(frame)

    def test_touch_get_state_refusal_decodes_not_silently_wrong(self):
        # The actual regression this test proves fixed: before
        # parse_touch_response()'s length check was tightened to exactly
        # {6, 23}, this 16-byte refusal passed the old `>= 6` check and
        # decoded as a bogus "success" (screen_on=False, garbage idle_ms)
        # with no exception at all. It must never collide with that success
        # shape -- but unlike the READ_ID case below (which really can't
        # reply under its own id -- see display_bridge_task()'s READ_ID
        # case), GET_STATE's own screen_idle_get_state() call CAN fail, so
        # this is decoded into an OkReason (matching every other refusal in
        # this file) rather than just rejected as unparseable.
        frame = _reject_frame(TOUCH_CMD_GET_STATE, "driver error")
        self.assertNotIn(len(frame), (6, 23))
        subcmd, value = devices.parse_touch_response(frame)
        self.assertEqual(subcmd, TOUCH_CMD_GET_STATE)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")

    def test_touch_get_state_bare_refusal_also_decodes(self):
        # No-reason refusal (2 bytes) -- also must not collide with the
        # 6-byte success shape, and still decodes to a reasonless OkReason.
        frame = _reject_frame(TOUCH_CMD_GET_STATE, None)
        self.assertEqual(len(frame), 2)
        subcmd, value = devices.parse_touch_response(frame)
        self.assertEqual(subcmd, TOUCH_CMD_GET_STATE)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)
        self.assertIsNone(value.reason)

    def test_touch_get_state_success_shapes_still_decode(self):
        # Negative-test control: prove the tightened check didn't also
        # break the two real success lengths.
        six_byte = bytes([TOUCH_CMD_GET_STATE, 0, 42, 0, 0, 0])
        subcmd, value = devices.parse_touch_response(six_byte)
        self.assertEqual(subcmd, TOUCH_CMD_GET_STATE)
        self.assertFalse(value.screen_on)
        self.assertEqual(value.idle_ms, 42)

        twentythree_byte = six_byte + bytes(17)
        subcmd, value = devices.parse_touch_response(twentythree_byte)
        self.assertEqual(subcmd, TOUCH_CMD_GET_STATE)
        self.assertIsNotNone(value.input_enabled)
        self.assertIsNone(value.power_state)  # not present at this length

    def test_touch_get_state_power_swallow_fields_decode(self):
        # 2026-09-24: the new 29-byte shape (23-byte diag reply + power_state,
        # swallow_count, last_swallow_reason). Negative-test control for the
        # (6, 23, 29) tightened length set below -- this is the one real new
        # success length that must still decode.
        twentythree_byte = bytes([TOUCH_CMD_GET_STATE, 1, 7, 0, 0, 0]) + bytes(17)
        twentynine_byte = twentythree_byte + bytes([2]) + struct.pack("<I", 5) + bytes([1])
        self.assertEqual(len(twentynine_byte), 29)
        subcmd, value = devices.parse_touch_response(twentynine_byte)
        self.assertEqual(subcmd, TOUCH_CMD_GET_STATE)
        self.assertTrue(value.screen_on)
        self.assertEqual(value.idle_ms, 7)
        self.assertEqual(value.power_state, 2)  # TOUCH_POWER_STATE_ERROR_HOLD
        self.assertEqual(value.swallow_count, 5)
        self.assertEqual(value.last_swallow_reason, 1)  # TOUCH_SWALLOW_REASON_WAKE

    def test_touch_get_state_15_byte_refusal_is_not_misread_as_truncated_success(self):
        # The regression this guards: a length one byte short of (or past)
        # the new 29-byte real success shape must never be silently misread
        # as a truncated/overlong TouchState -- it must still fall into the
        # refusal decode path, same as before 29 was added to the allowed set.
        # _reject_frame(TOUCH_CMD_GET_STATE, "driver error") actually builds a
        # 15-byte refusal ([subcmd, 0, len("driver error")=12] + 12 bytes of
        # reason text = 15), not a 28-byte one -- the old name here named the
        # wrong length.
        frame = _reject_frame(TOUCH_CMD_GET_STATE, "driver error")
        self.assertNotIn(len(frame), (6, 23, 29))
        subcmd, value = devices.parse_touch_response(frame)
        self.assertEqual(subcmd, TOUCH_CMD_GET_STATE)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)


class DisplayWriteRefusalTests(unittest.TestCase):
    """DISPLAY's write subcommands (RESET..BLIT_END) are fire-and-forget on
    success -- display_bridge_task() sends nothing back. The only reply
    that can ever arrive under one of these ids is the bottom-of-task
    driver-error refusal. Before this, any of these ids raised "unknown
    DISPLAY response subcommand" in parse_display_response(), so the
    refusal was dropped in DisplayClient._handle_reply -- invisible to
    whoever sent the write."""

    def test_reset_driver_error_reason_preserved(self):
        subcmd, value = devices.parse_display_response(
            _reject_frame(DISPLAY_CMD_RESET, "driver error")
        )
        self.assertEqual(subcmd, DISPLAY_CMD_RESET)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")

    def test_clear_driver_error_reason_preserved(self):
        subcmd, value = devices.parse_display_response(
            _reject_frame(DISPLAY_CMD_CLEAR, "driver error")
        )
        self.assertEqual(subcmd, DISPLAY_CMD_CLEAR)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")

    def test_print_bare_refusal_decodes(self):
        subcmd, value = devices.parse_display_response(_reject_frame(DISPLAY_CMD_PRINT, None))
        self.assertEqual(subcmd, DISPLAY_CMD_PRINT)
        self.assertFalse(value.ok)
        self.assertIsNone(value.reason)


class TouchWriteRefusalTests(unittest.TestCase):
    """INJECT/SET_TAP_DUMP/LOG_TAP_TARGETS are fire-and-forget on success;
    the only reply under one of these ids is the bottom-of-task
    driver-error refusal. Before this, any of these ids raised "unknown
    TOUCH response subcommand" and the refusal was dropped in
    TouchClient._handle_reply."""

    def test_inject_driver_error_reason_preserved(self):
        subcmd, value = devices.parse_touch_response(
            _reject_frame(TOUCH_CMD_INJECT, "driver error")
        )
        self.assertEqual(subcmd, TOUCH_CMD_INJECT)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")

    def test_set_tap_dump_bare_refusal_decodes(self):
        subcmd, value = devices.parse_touch_response(
            _reject_frame(TOUCH_CMD_SET_TAP_DUMP, None)
        )
        self.assertEqual(subcmd, TOUCH_CMD_SET_TAP_DUMP)
        self.assertFalse(value.ok)
        self.assertIsNone(value.reason)

    def test_log_tap_targets_driver_error_reason_preserved(self):
        subcmd, value = devices.parse_touch_response(
            _reject_frame(TOUCH_CMD_LOG_TAP_TARGETS, "driver error")
        )
        self.assertEqual(subcmd, TOUCH_CMD_LOG_TAP_TARGETS)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")


class IoOtherWriteRefusalTests(unittest.TestCase):
    """IO's write subcommands other than SET_RELAY/SET_RELAY_MASK (which
    get RelayResult/RelayRefusal classification) -- SET_IO, ALL_RELAYS_OFF,
    SX_WRITE_REG, SX_RESET, etc -- can refuse the same way (truncated,
    out-of-range, the "safety" guard SX_WRITE_REG/SX_SET_DIR share with the
    relay commands, or the generic driver-error reject), but
    parse_io_response() had no case for any of them: any reply under one of
    these ids raised "unknown IO response subcommand" and was dropped in
    IoClient._handle_reply."""

    def test_set_io_out_of_range_reason_preserved(self):
        subcmd, value = devices.parse_io_response(
            _reject_frame(IO_CMD_SET_IO, "out of range")
        )
        self.assertEqual(subcmd, IO_CMD_SET_IO)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "out of range")

    def test_all_relays_off_success_decodes_ok_true(self):
        subcmd, value = devices.parse_io_response(_ok_frame(IO_CMD_ALL_RELAYS_OFF))
        self.assertEqual(subcmd, IO_CMD_ALL_RELAYS_OFF)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_sx_write_reg_safety_reason_preserved(self):
        subcmd, value = devices.parse_io_response(
            _reject_frame(IO_CMD_SX_WRITE_REG, "safety")
        )
        self.assertEqual(subcmd, IO_CMD_SX_WRITE_REG)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "safety")

    def test_sx_reset_driver_error_reason_preserved(self):
        subcmd, value = devices.parse_io_response(
            _reject_frame(IO_CMD_SX_RESET, "driver error")
        )
        self.assertEqual(subcmd, IO_CMD_SX_RESET)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")


class AutotuneAbortAcceptReasonTests(unittest.TestCase):
    """ABORT/ACCEPT used to decode the reply and then discard everything but
    the ok byte (``return subcommand, bool(payload[1])``) -- the same bug
    class as CONTROL's SET_ZONE_PID/SET_ZONE_MODEL and PROFILES' DELETE/
    PAUSE/RESUME/ACK_LAST_RUN/STOP. ACCEPT's "no completed autotune result
    to accept" refusal (autotune_handle_message()'s ACCEPT case) used to
    vanish entirely."""

    def test_abort_success_decodes_ok_true(self):
        subcmd, value = devices.parse_autotune_response(_ok_frame(AUTOTUNE_CMD_ABORT))
        self.assertEqual(subcmd, AUTOTUNE_CMD_ABORT)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_accept_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_autotune_response(
            _reject_frame(AUTOTUNE_CMD_ACCEPT, "no completed autotune result to accept")
        )
        self.assertEqual(subcmd, AUTOTUNE_CMD_ACCEPT)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "no completed autotune result to accept")


class WifiUartReasonTests(unittest.TestCase):
    """ADD_NETWORK/SET_MODE/SET_AP_IDENTITY/FORGET used to decode the reply
    and then discard everything but the ok byte
    (``return subcommand, bool(payload[1])``) -- the same bug class as
    CONTROL/PROFILES/AUTOTUNE above. wifi_bridge_task() (uart_bridge_ext.c)
    gives every one of these a real reason on refusal ("ssid too long",
    "saved network list is full", "ap_password must be empty or 8-63
    characters", "could not forget network", etc) that this was silently
    dropping."""

    def test_add_network_success_decodes_ok_true(self):
        subcmd, value = devices.parse_wifi_uart_response(_ok_frame(WIFI_CMD_ADD_NETWORK))
        self.assertEqual(subcmd, WIFI_CMD_ADD_NETWORK)
        self.assertIsInstance(value, devices.OkReason)
        self.assertTrue(value.ok)

    def test_add_network_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_wifi_uart_response(
            _reject_frame(WIFI_CMD_ADD_NETWORK, "saved network list is full")
        )
        self.assertEqual(subcmd, WIFI_CMD_ADD_NETWORK)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "saved network list is full")

    def test_set_mode_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_wifi_uart_response(
            _reject_frame(WIFI_CMD_SET_MODE, "could not change mode")
        )
        self.assertEqual(subcmd, WIFI_CMD_SET_MODE)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "could not change mode")

    def test_set_ap_identity_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_wifi_uart_response(
            _reject_frame(WIFI_CMD_SET_AP_IDENTITY, "ap_password must be empty or 8-63 characters")
        )
        self.assertEqual(subcmd, WIFI_CMD_SET_AP_IDENTITY)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "ap_password must be empty or 8-63 characters")

    def test_forget_reason_is_preserved_not_discarded(self):
        subcmd, value = devices.parse_wifi_uart_response(
            _reject_frame(WIFI_CMD_FORGET, "could not forget network")
        )
        self.assertEqual(subcmd, WIFI_CMD_FORGET)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "could not forget network")


class UnsupportedVsEmptySuccessCollisionTests(unittest.TestCase):
    """THERMO_CMD_READ_FAULTS and IO_CMD_SX_SCAN both reply {subcmd, count}
    where byte[1] is a count that can legitimately be 0 -- an honest empty
    result. bridge_reply_unsupported() (uart_bridge.c's default: case) used
    to emit exactly {subcmd, 0} for an unrecognized subcommand: 2 bytes,
    byte-identical to that empty-success shape (TODO.md section 11's second
    open item, flagged in commit 5df2190's message). Firmware now gives that
    default: case a real reason ("unsupported") instead of NULL, so the
    refusal is always longer than 2 bytes and the two can never collide.

    ``test_..._old_bare_unsupported_would_have_collided`` proves the ambiguity
    was real: the pre-fix 2-byte shape decodes as a clean, wrong "empty
    success" instead of raising.

    WHAT THIS CLASS DOES NOT DO. These tests build their own frames with a
    hardcoded reason string, so they cover the HOST DECODER only. They CANNOT
    fail if the firmware regresses -- flip bridge_reply_unsupported() back to a
    NULL reason in uart_bridge.c and every test here stays green, because no
    test in this file reads C. The C-side rule has its own C-side guard,
    ``tools/check_bridge_reject_reason.ps1``, which fails if any
    bridge_reply_reject() call passes NULL or "". Do not treat this class as
    the negative test for the firmware fix; an earlier revision of KilnFW/
    TODO.md section 11 did, and was wrong."""

    def test_read_faults_empty_success_still_decodes(self):
        # Negative-test control: the real empty-success wire shape must keep
        # decoding cleanly -- the fix must not turn an honest "no faults on
        # any channel" into an error.
        subcmd, entries = devices.parse_thermo_response(bytes([THERMO_CMD_READ_FAULTS, 0]))
        self.assertEqual(subcmd, THERMO_CMD_READ_FAULTS)
        self.assertEqual(entries, [])

    def test_read_faults_old_bare_unsupported_would_have_collided(self):
        # bridge_reply_unsupported()'s PRE-FIX output (NULL reason -> bare
        # {subcmd, 0}) is byte-identical to the empty-success shape above.
        # This decodes without raising, which is exactly the confusion
        # TODO.md warned about: a firmware build with no READ_FAULTS support
        # would be misread as "we asked, and no channel has a fault."
        frame = _reject_frame(THERMO_CMD_READ_FAULTS, None)
        self.assertEqual(len(frame), 2)
        subcmd, entries = devices.parse_thermo_response(frame)
        self.assertEqual(entries, [])

    def test_read_faults_reasoned_unsupported_cannot_collide(self):
        # The POST-FIX shape: bridge_reply_unsupported() now passes
        # "unsupported" as the reason, so this is always longer than the
        # 2-byte empty-success shape and parse_thermo_response() must raise
        # instead of silently returning an empty list.
        frame = _reject_frame(THERMO_CMD_READ_FAULTS, "unsupported")
        self.assertNotEqual(len(frame), 2)
        with self.assertRaises(devices.ThermoResponseError):
            devices.parse_thermo_response(frame)

    def test_sx_scan_empty_success_still_decodes(self):
        subcmd, addresses = devices.parse_io_response(bytes([IO_CMD_SX_SCAN, 0]))
        self.assertEqual(subcmd, IO_CMD_SX_SCAN)
        self.assertEqual(addresses, [])

    def test_sx_scan_old_bare_unsupported_would_have_collided(self):
        frame = _reject_frame(IO_CMD_SX_SCAN, None)
        self.assertEqual(len(frame), 2)
        subcmd, addresses = devices.parse_io_response(frame)
        self.assertEqual(addresses, [])

    def test_sx_scan_reasoned_unsupported_cannot_collide(self):
        frame = _reject_frame(IO_CMD_SX_SCAN, "unsupported")
        self.assertNotEqual(len(frame), 2)
        with self.assertRaises(devices.IoResponseError):
            devices.parse_io_response(frame)


if __name__ == "__main__":
    unittest.main()
