#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_get_fw_version()/SafetyFwVersion --
the PC-facing mirror of SAFETY_CMD_GET_FW_VERSION (0x0B,
CommonFW/docs/LINK_PROTOCOL.md sec 6 Frame C), the last piece of TODO 0.9's
"mirror the safety-processor data on the PC-link SAFETY task" alongside
SafetyDiag/SafetyTripEvent.

No real UART/serial connection is used -- this only checks the byte-exact
wire encoding/decoding, built directly from firmware/SaftyFW/src/tasks/
link_frame.c's link_frame_pack_fw_version() layout:

    offset 0      cmd (0x0B)
    offset 1..2   protocol_version u16 LE
    offset 3..4   min_compatible   u16 LE
    offset 5      dirty            u8
    offset 6      commit_len (N1)  u8
    offset 7..    N1 * u8          commit, ASCII, not null-terminated
    next 1        u8               datetime_len (N2)
    next N2       N2 * u8          datetime, ASCII
    next 1        u8               boot_id
    next 1        u8               config_version
    next 2        u16 LE           config_crc

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_GET_FW_VERSION  # noqa: E402


def _build_vector(
    *,
    protocol_version=7,
    min_compatible=5,
    dirty=1,
    commit=b"",
    datetime=b"",
    boot_id=3,
    config_version=0,
    config_crc=0,
):
    """Byte-exact mirror of link_frame_pack_fw_version() -- see link_frame.c."""
    out = bytearray()
    out.append(SAFETY_CMD_GET_FW_VERSION)
    out += struct.pack("<H", protocol_version)
    out += struct.pack("<H", min_compatible)
    out.append(dirty)
    out.append(len(commit))
    out += commit
    out.append(len(datetime))
    out += datetime
    out.append(boot_id)
    out.append(config_version)
    out += struct.pack("<H", config_crc)
    return bytes(out)


class SafetyGetFwVersionRequestTests(unittest.TestCase):
    def test_request_is_cmd_byte_only(self):
        self.assertEqual(devices.safety_get_fw_version(), bytes([SAFETY_CMD_GET_FW_VERSION]))


class SafetyFwVersionParseTests(unittest.TestCase):
    def test_never_commissioned_unknown_identity(self):
        # The real steady state today: link_task_send_fw_version() hardcodes
        # dirty=1 with empty commit/datetime, and a never-commissioned board's
        # config_store_default() reports config_version=0/config_crc=0.
        vector = _build_vector(
            protocol_version=7,
            min_compatible=5,
            dirty=1,
            commit=b"",
            datetime=b"",
            boot_id=3,
            config_version=0,
            config_crc=0,
        )
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_FW_VERSION)
        self.assertIsInstance(value, devices.SafetyFwVersion)
        self.assertEqual(value.protocol_version, 7)
        self.assertEqual(value.min_compatible, 5)
        self.assertTrue(value.dirty)
        self.assertEqual(value.commit, "")
        self.assertEqual(value.built, "")
        self.assertEqual(value.boot_id, 3)
        self.assertEqual(value.config_version, 0)
        self.assertEqual(value.config_crc, 0)
        self.assertFalse(value.commissioned)

    def test_known_commit_and_datetime_and_commissioned(self):
        vector = _build_vector(
            protocol_version=7,
            min_compatible=6,
            dirty=0,
            commit=b"a1b2c3d",
            datetime=b"2026-08-21 12:00:00Z",
            boot_id=9,
            config_version=2,
            config_crc=0xBEEF,
        )
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_FW_VERSION)
        self.assertFalse(value.dirty)
        self.assertEqual(value.commit, "a1b2c3d")
        self.assertEqual(value.built, "2026-08-21 12:00:00Z")
        self.assertEqual(value.boot_id, 9)
        self.assertEqual(value.config_version, 2)
        self.assertEqual(value.config_crc, 0xBEEF)
        self.assertTrue(value.commissioned)

    def test_dirty_and_unknown_share_the_same_wire_value(self):
        # LINK_PROTOCOL.md sec 6: "unknown must map to dirty = 1" -- an empty
        # commit alongside dirty=1 is how "no known identity" actually looks
        # on the wire; this must never be reported as clean.
        vector = _build_vector(dirty=1, commit=b"", datetime=b"")
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertTrue(value.dirty)
        self.assertEqual(value.commit, "")

    def test_never_fabricates_a_placeholder_for_unknown_commit(self):
        vector = _build_vector(commit=b"", datetime=b"")
        _subcommand, value = devices.parse_safety_response(vector)
        # Empty string, not "?"/"unknown"/None-rendered-as-text -- callers
        # decide how to display "not known", the parser must not guess for
        # them.
        self.assertEqual(value.commit, "")
        self.assertEqual(value.built, "")

    # -- negative tests: prove each new assertion can actually fail ---------

    def test_baseline_vector_is_accepted(self):
        # Sanity check the vector builder itself before trusting the
        # deliberately-broken variants below.
        vector = _build_vector(commit=b"abc", datetime=b"2026")
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_FW_VERSION)
        self.assertEqual(value.commit, "abc")

    def test_truncated_before_commit_len_is_rejected(self):
        vector = _build_vector(commit=b"abc", datetime=b"2026")
        # Cut the buffer off right before commit_len's offset (6) -- 6 bytes
        # total instead of 7+.
        broken = vector[:6]
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(broken)
        # Prove the check is actually load-bearing: without it, a naive
        # unpack_from would silently return garbage/zeros instead of raising.
        # struct.unpack_from would in fact raise on an insufficient buffer
        # too, so demonstrate the manual bounds check reports OUR message
        # (too short to reach commit_len), not an opaque struct error.
        try:
            devices.parse_safety_response(broken)
        except devices.SafetyResponseError as exc:
            self.assertIn("too short", str(exc))
        else:
            self.fail("expected SafetyResponseError")

    def test_commit_len_overruns_buffer_is_rejected(self):
        vector = bytearray(
            _build_vector(protocol_version=1, min_compatible=1, dirty=0, commit=b"abc", datetime=b"")
        )
        # commit_len is at offset 6; claim 200 bytes of commit that are not
        # actually present.
        vector[6] = 200
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(bytes(vector))

    def test_trailing_garbage_after_config_crc_is_rejected(self):
        vector = _build_vector(commit=b"abc", datetime=b"2026") + b"\x00\x00\x00"
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)

    def test_datetime_len_overruns_buffer_is_rejected(self):
        vector = bytearray(_build_vector(commit=b"abc", datetime=b"2026"))
        # datetime_len sits right after commit (offset 7 + len("abc") = 10).
        datetime_len_offset = 7 + len(b"abc")
        vector[datetime_len_offset] = 250
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(bytes(vector))

    def test_wrong_length_flips_commissioned_property_check(self):
        # Prove test_known_commit_and_datetime_and_commissioned's
        # `assertTrue(value.commissioned)` can actually fail: a config_crc of
        # 0 on an otherwise-identical vector must report commissioned=False.
        vector = _build_vector(config_version=2, config_crc=0)
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertFalse(value.commissioned)


if __name__ == "__main__":
    unittest.main()
