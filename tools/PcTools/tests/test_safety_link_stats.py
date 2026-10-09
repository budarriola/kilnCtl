#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.SafetyLinkStats/parse_safety_response()'s
SAFETY_CMD_GET_LINK_STATS (0x04) path.

2026-08-23, the DIAG-frame-went-dark investigation: this payload grew four
times the same day, all additive:
  - V1 (19 bytes) -> V2 (23, +broadcast_dropped u32 LE): the ESP-side counter
    for a BROADCAST frame whose delivery into task 7's inbox failed because
    the inbox was already full.
  - V2 -> V3 (31, +diag_applied u32 LE +power_applied u32 LE): real
    "applied N times" counters for DIAG/POWER, added once tx_dropped_sat==0
    and broadcast_dropped==0 both measured clean while DIAG stayed dark.
  - V3 -> V4 (51, +frames_deframed +frames_routed_nowhere
    +frame_length_mismatch +frame_crc_mismatch +frame_resync, all u32 LE):
    deframer/dispatch-level counters, added once diag_applied/power_applied
    themselves pinned at exactly 1 per boot with both V2/V3's counters
    clean -- meaning the loss was happening below the per-task inbox,
    somewhere neither bracket covered.
  - V4 -> V5 (60, +dequeued_total u32 LE +unmatched_cmd_count u32 LE
    +last_unmatched_cmd_byte u8): once frames_deframed proved DIAG/POWER
    arrive CRC-valid at roughly the expected rate and frames_routed_nowhere/
    broadcast_dropped both stayed clean, this narrows the remaining gap to
    "does the one confirmed consumer of the safety inbox
    (safety_drain_inbox_ex()) actually pull them out, and if so, what
    unmatched command byte does it see".
  - V5 -> V6 (96, +9 u32 LE per-command dequeue counts): dequeued_total
    tracked frames_deframed almost exactly and unmatched_cmd_count stayed 0,
    yet diag_applied/power_applied both stayed 0 against ~80 unaccounted
    messages per 30s window -- only possible if the "non-status traffic is
    DIAG/POWER" assumption was wrong. This is the measurement that answers
    it directly: a per-command histogram, one field per switch case, that
    must sum with unmatched_cmd_count to exactly dequeued_total.

No test of this reply existed in this suite before the 2026-08-23 pass (grep
found none), so this file covers the pre-existing byte layout and all of
V1/V2/V3/V4/V5/V6.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_GET_LINK_STATS  # noqa: E402


def _build_v1_vector(
    *, sent=100, received=95, crc_errors=0, timeouts=3, poll_period_ms=500
):
    """Byte-exact mirror of safety_link_build_stats_payload()'s first 19
    bytes (V1 layout, unchanged since before the 2026-08-23 pass)."""
    return struct.pack(
        "<BIIIIH",
        SAFETY_CMD_GET_LINK_STATS,
        sent,
        received,
        crc_errors,
        timeouts,
        poll_period_ms,
    )


def _build_v2_vector(*, broadcast_dropped=0, **v1_kwargs):
    """V1 vector plus V2's field."""
    return _build_v1_vector(**v1_kwargs) + struct.pack("<I", broadcast_dropped)


def _build_v3_vector(*, diag_applied=0, power_applied=0, **v2_kwargs):
    """V2 vector plus V3's fields."""
    return _build_v2_vector(**v2_kwargs) + struct.pack("<II", diag_applied, power_applied)


def _build_v4_vector(
    *,
    frames_deframed=0,
    frames_routed_nowhere=0,
    frame_length_mismatch=0,
    frame_crc_mismatch=0,
    frame_resync=0,
    **v3_kwargs,
):
    """V3 vector plus V4's fields."""
    return _build_v3_vector(**v3_kwargs) + struct.pack(
        "<IIIII",
        frames_deframed,
        frames_routed_nowhere,
        frame_length_mismatch,
        frame_crc_mismatch,
        frame_resync,
    )


def _build_v5_vector(
    *,
    dequeued_total=0,
    unmatched_cmd_count=0,
    last_unmatched_cmd_byte=0,
    **v4_kwargs,
):
    """V4 vector plus V5's fields."""
    return _build_v4_vector(**v4_kwargs) + struct.pack(
        "<IIB", dequeued_total, unmatched_cmd_count, last_unmatched_cmd_byte
    )


def _build_v6_vector(
    *,
    cmd_status_count=0,
    cmd_fw_version_count=0,
    cmd_update_status_count=0,
    cmd_power_count=0,
    cmd_diag_count=0,
    cmd_trip_event_count=0,
    cmd_ct_cal_count=0,
    cmd_config_page_count=0,
    cmd_commit_config_rejected_count=0,
    **v5_kwargs,
):
    """V5 vector plus V6's nine per-command counters, in wire order."""
    return _build_v5_vector(**v5_kwargs) + struct.pack(
        "<IIIIIIIII",
        cmd_status_count,
        cmd_fw_version_count,
        cmd_update_status_count,
        cmd_power_count,
        cmd_diag_count,
        cmd_trip_event_count,
        cmd_ct_cal_count,
        cmd_config_page_count,
        cmd_commit_config_rejected_count,
    )


class TestSafetyLinkStatsV1(unittest.TestCase):
    def test_v1_decodes_and_all_added_fields_are_unknown(self):
        vector = _build_v1_vector(
            sent=242, received=324, crc_errors=0, timeouts=6, poll_period_ms=500
        )
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_LINK_STATS)
        self.assertEqual(value.frames_sent, 242)
        self.assertEqual(value.frames_received, 324)
        self.assertEqual(value.crc_errors, 0)
        self.assertEqual(value.timeouts, 6)
        self.assertEqual(value.poll_period_ms, 500)
        for field in (
            "broadcast_dropped",
            "diag_applied",
            "power_applied",
            "frames_deframed",
            "frames_routed_nowhere",
            "frame_length_mismatch",
            "frame_crc_mismatch",
            "frame_resync",
            "dequeued_total",
            "unmatched_cmd_count",
            "last_unmatched_cmd_byte",
            "cmd_status_count",
            "cmd_fw_version_count",
            "cmd_update_status_count",
            "cmd_power_count",
            "cmd_diag_count",
            "cmd_trip_event_count",
            "cmd_ct_cal_count",
            "cmd_config_page_count",
            "cmd_commit_config_rejected_count",
        ):
            self.assertIsNone(
                getattr(value, field),
                f"a V1 (19-byte) reply must decode {field} as None, not 0",
            )

    def test_v1_describe_reports_unknown_not_zero(self):
        subcommand, value = devices.parse_safety_response(_build_v1_vector())
        text = value.describe()
        for label in (
            "broadcast dropped unknown",
            "diag applied unknown",
            "power applied unknown",
            "frames deframed unknown",
            "routed nowhere unknown",
            "length mismatch unknown",
            "crc mismatch unknown",
            "resync unknown",
            "dequeued unknown",
            "unmatched cmd count unknown",
            "last unmatched cmd byte unknown",
            "status=unknown",
            "fw_version=unknown",
            "update_status=unknown",
            "power=unknown",
            "diag=unknown",
            "trip_event=unknown",
            "ct_cal=unknown",
            "config_page=unknown",
            "commit_config_rejected=unknown",
        ):
            self.assertIn(label, text)


class TestSafetyLinkStatsV2(unittest.TestCase):
    def test_v2_decodes_broadcast_dropped_but_not_later_fields(self):
        subcommand, value = devices.parse_safety_response(
            _build_v2_vector(broadcast_dropped=17)
        )
        self.assertEqual(value.broadcast_dropped, 17)
        self.assertIn("broadcast dropped 17", value.describe())
        self.assertIsNone(value.diag_applied)
        self.assertIsNone(value.frames_deframed)


class TestSafetyLinkStatsV3(unittest.TestCase):
    def test_v3_decodes_apply_counts_but_not_v4_fields(self):
        subcommand, value = devices.parse_safety_response(
            _build_v3_vector(broadcast_dropped=0, diag_applied=1, power_applied=1)
        )
        self.assertEqual(value.diag_applied, 1)
        self.assertEqual(value.power_applied, 1)
        self.assertIsNone(value.frames_deframed)
        self.assertIsNone(value.frame_crc_mismatch)


class TestSafetyLinkStatsV4(unittest.TestCase):
    def test_v4_decodes_all_five_deframer_counters(self):
        subcommand, value = devices.parse_safety_response(
            _build_v4_vector(
                diag_applied=1,
                power_applied=1,
                frames_deframed=841,
                frames_routed_nowhere=3,
                frame_length_mismatch=2,
                frame_crc_mismatch=1,
                frame_resync=0,
            )
        )
        self.assertEqual(value.frames_deframed, 841)
        self.assertEqual(value.frames_routed_nowhere, 3)
        self.assertEqual(value.frame_length_mismatch, 2)
        self.assertEqual(value.frame_crc_mismatch, 1)
        self.assertEqual(value.frame_resync, 0)
        text = value.describe()
        self.assertIn("frames deframed 841", text)
        self.assertIn("routed nowhere 3", text)
        self.assertIn("length mismatch 2", text)
        self.assertIn("crc mismatch 1", text)
        self.assertIn("resync 0", text)
        self.assertIsNone(value.dequeued_total)

    def test_v4_zero_frames_deframed_is_real_not_unknown(self):
        # The specific case this round is trying to distinguish: a real 0
        # (nothing at all arrived at the deframer) must read differently
        # from None (this ESP build cannot say).
        subcommand, value = devices.parse_safety_response(
            _build_v4_vector(frames_deframed=0)
        )
        self.assertEqual(value.frames_deframed, 0)
        self.assertIn("frames deframed 0", value.describe())
        self.assertNotIn("frames deframed unknown", value.describe())

    def test_v4_does_not_decode_v5_fields(self):
        subcommand, value = devices.parse_safety_response(_build_v4_vector())
        self.assertIsNone(value.dequeued_total)
        self.assertIsNone(value.unmatched_cmd_count)
        self.assertIsNone(value.last_unmatched_cmd_byte)


class TestSafetyLinkStatsV5(unittest.TestCase):
    def test_v5_decodes_dequeued_and_unmatched_fields(self):
        subcommand, value = devices.parse_safety_response(
            _build_v5_vector(
                dequeued_total=54,
                unmatched_cmd_count=0,
                last_unmatched_cmd_byte=0,
            )
        )
        self.assertEqual(value.dequeued_total, 54)
        self.assertEqual(value.unmatched_cmd_count, 0)
        self.assertEqual(value.last_unmatched_cmd_byte, 0)
        text = value.describe()
        self.assertIn("dequeued 54", text)
        self.assertIn("unmatched cmd count 0", text)
        self.assertIn("last unmatched cmd byte 0", text)
        # A V5 frame carries no per-command histogram -- that arrived in V6 --
        # so those nine buckets MUST read "unknown" here, not 0: this is the
        # unknown-vs-zero distinction the whole stats struct is built around,
        # and a V5 peer genuinely cannot tell us its histogram. Assert that
        # positively rather than blanket-asserting no "unknown" appears
        # anywhere in describe(), which is what this check used to do and
        # which broke the moment V6 added fields a V5 frame cannot fill.
        self.assertIn("status=unknown", text)
        self.assertIn("diag=unknown", text)
        # The V5 fields themselves must still be real values, not unknown.
        self.assertNotIn("dequeued unknown", text)
        self.assertNotIn("unmatched cmd count unknown", text)

    def test_v5_captures_the_actual_unmatched_byte_value(self):
        # The whole point of last_unmatched_cmd_byte over a plain count: a
        # caller must be able to read the real, specific offending byte.
        subcommand, value = devices.parse_safety_response(
            _build_v5_vector(unmatched_cmd_count=4, last_unmatched_cmd_byte=0x2A)
        )
        self.assertEqual(value.unmatched_cmd_count, 4)
        self.assertEqual(value.last_unmatched_cmd_byte, 0x2A)
        self.assertIn("last unmatched cmd byte 42", value.describe())

    def test_v5_zero_dequeued_total_is_real_not_unknown(self):
        subcommand, value = devices.parse_safety_response(
            _build_v5_vector(dequeued_total=0)
        )
        self.assertEqual(value.dequeued_total, 0)
        self.assertIn("dequeued 0", value.describe())
        self.assertNotIn("dequeued unknown", value.describe())


class TestSafetyLinkStatsV6(unittest.TestCase):
    def test_v6_decodes_the_full_histogram(self):
        subcommand, value = devices.parse_safety_response(
            _build_v6_vector(
                dequeued_total=128,
                cmd_status_count=48,
                cmd_fw_version_count=4,
                cmd_update_status_count=0,
                cmd_power_count=1,
                cmd_diag_count=0,
                cmd_trip_event_count=0,
                cmd_ct_cal_count=0,
                cmd_config_page_count=75,
                cmd_commit_config_rejected_count=0,
            )
        )
        self.assertEqual(value.cmd_status_count, 48)
        self.assertEqual(value.cmd_fw_version_count, 4)
        self.assertEqual(value.cmd_update_status_count, 0)
        self.assertEqual(value.cmd_power_count, 1)
        self.assertEqual(value.cmd_diag_count, 0)
        self.assertEqual(value.cmd_trip_event_count, 0)
        self.assertEqual(value.cmd_ct_cal_count, 0)
        self.assertEqual(value.cmd_config_page_count, 75)
        self.assertEqual(value.cmd_commit_config_rejected_count, 0)
        text = value.describe()
        self.assertIn("status=48", text)
        self.assertIn("fw_version=4", text)
        self.assertIn("power=1", text)
        self.assertIn("config_page=75", text)
        self.assertNotIn("unknown", text)

    def test_v6_histogram_plus_unmatched_sums_to_dequeued_total(self):
        # The invariant safety_count_cmd_byte()'s own doc comment states:
        # every dequeued message lands in exactly one of the nine command
        # buckets or unmatched_cmd_count, never more than one, never none.
        # This test proves the WIRE decode preserves that arithmetic (the
        # firmware side is not host-testable, so this is the closest this
        # suite can get to pinning the invariant down).
        subcommand, value = devices.parse_safety_response(
            _build_v6_vector(
                dequeued_total=128,
                unmatched_cmd_count=0,
                cmd_status_count=48,
                cmd_fw_version_count=4,
                cmd_update_status_count=1,
                cmd_power_count=1,
                cmd_diag_count=0,
                cmd_trip_event_count=0,
                cmd_ct_cal_count=0,
                cmd_config_page_count=74,
                cmd_commit_config_rejected_count=0,
            )
        )
        histogram_sum = (
            value.cmd_status_count
            + value.cmd_fw_version_count
            + value.cmd_update_status_count
            + value.cmd_power_count
            + value.cmd_diag_count
            + value.cmd_trip_event_count
            + value.cmd_ct_cal_count
            + value.cmd_config_page_count
            + value.cmd_commit_config_rejected_count
            + value.unmatched_cmd_count
        )
        self.assertEqual(histogram_sum, value.dequeued_total)

    def test_v6_zero_diag_count_is_real_not_unknown(self):
        # The specific case this whole round exists to distinguish: a real,
        # confirmed 0 (DIAG genuinely never dequeued) must read differently
        # from None (this ESP build cannot say).
        subcommand, value = devices.parse_safety_response(
            _build_v6_vector(cmd_diag_count=0)
        )
        self.assertEqual(value.cmd_diag_count, 0)
        self.assertIn("diag=0", value.describe())


class TestSafetyLinkStatsNegative(unittest.TestCase):
    def test_length_18_rejected(self):
        vector = _build_v1_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_22_rejected(self):
        vector = _build_v2_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_24_rejected(self):
        vector = _build_v2_vector() + b"\x00"
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_30_rejected(self):
        vector = _build_v3_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_32_rejected(self):
        vector = _build_v3_vector() + b"\x00"
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_50_rejected(self):
        vector = _build_v4_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_52_rejected(self):
        vector = _build_v4_vector() + b"\x00"
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_59_rejected(self):
        vector = _build_v5_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_61_rejected(self):
        vector = _build_v5_vector() + b"\x00"
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_95_rejected(self):
        vector = _build_v6_vector()[:-1]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_length_97_rejected(self):
        vector = _build_v6_vector() + bytes([0])
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)


if __name__ == "__main__":
    unittest.main()
