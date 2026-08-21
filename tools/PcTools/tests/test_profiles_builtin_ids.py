#!/usr/bin/env python3
"""The built-in (shipped-in-flash) profile id space over the UART link.

Ids >= PROFILES_BUILTIN_ID_BASE (128) address the read-only firing schedules
in App/drivers/profiles_builtin.c. They were reachable over HTTP but rejected
host-side over the PC link, which made every shipped schedule invisible to the
GUI and the MCP tools. These tests pin the id-space rules both sides now agree
on:

  - GET / START accept a user slot OR a built-in id.
  - SAVE with a built-in id is a save-as-copy (encodes fine; the firmware
    redirects it to the first free user slot).
  - DELETE of a built-in is refused with a message pointing at hide.
  - The gap between the two ranges (8..127) is not addressable.
  - LIST is paged, so the 28-entry catalogue can actually be enumerated.

Run with: python -m pytest tools/PcTools/tests/test_profiles_builtin_ids.py
"""
from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl.protocol import (  # noqa: E402
    PROFILES_BUILTIN_ID_BASE,
    PROFILES_CMD_GET,
    PROFILES_CMD_LIST,
    PROFILES_CMD_START,
    PROFILES_MAX_COUNT,
    PROFILES_SAVE_ID_NEW,
    UART_PROTO_MAX_PAYLOAD,
    profile_id_is_builtin,
)

BUILTIN = PROFILES_BUILTIN_ID_BASE + 4  # id 132 -- the one that failed on the bench


class IdSpaceTests(unittest.TestCase):
    def test_ranges_are_disjoint(self):
        self.assertGreater(PROFILES_BUILTIN_ID_BASE, PROFILES_MAX_COUNT - 1)
        self.assertFalse(profile_id_is_builtin(PROFILES_MAX_COUNT - 1))
        self.assertTrue(profile_id_is_builtin(PROFILES_BUILTIN_ID_BASE))
        self.assertTrue(profile_id_is_builtin(0xFF))

    def test_get_accepts_user_slot_and_builtin(self):
        self.assertEqual(devices.profiles_get(3), bytes([PROFILES_CMD_GET, 3]))
        self.assertEqual(devices.profiles_get(BUILTIN), bytes([PROFILES_CMD_GET, BUILTIN]))

    def test_start_accepts_builtin(self):
        self.assertEqual(
            devices.profiles_start(BUILTIN), bytes([PROFILES_CMD_START, BUILTIN])
        )

    def test_unaddressable_gap_is_rejected(self):
        for bad in (PROFILES_MAX_COUNT, 64, PROFILES_BUILTIN_ID_BASE - 1, -1, 256):
            with self.assertRaises(ValueError):
                devices.profiles_get(bad)
            with self.assertRaises(ValueError):
                devices.profiles_start(bad)

    def test_delete_of_builtin_encodes_for_the_firmware_to_refuse(self):
        # Sent on the wire deliberately: the firmware owns the refusal text so
        # UART and HTTP say the same thing.
        self.assertEqual(devices.profiles_delete(BUILTIN)[1], BUILTIN)
        with self.assertRaises(ValueError):
            devices.profiles_delete(PROFILES_MAX_COUNT)


class SaveIsCopyTests(unittest.TestCase):
    def _segments(self, n: int):
        return [
            devices.ProfileSegment(target_c=100.0 * i, ramp_c_per_hr=80.0, dwell_min=10)
            for i in range(1, n + 1)
        ]

    def test_save_with_builtin_id_encodes(self):
        body = devices.profiles_save(BUILTIN, "copy", 0x01, self._segments(3))
        self.assertEqual(body[1], BUILTIN)  # firmware redirects to a free slot

    def test_save_new_still_works(self):
        body = devices.profiles_save(PROFILES_SAVE_ID_NEW, "n", 0x01, self._segments(1))
        self.assertEqual(body[1], PROFILES_SAVE_ID_NEW)

    def test_save_still_rejects_the_gap(self):
        with self.assertRaises(ValueError):
            devices.profiles_save(9, "n", 0x01, self._segments(1))


class ReplyBudgetTests(unittest.TestCase):
    """A 12-segment built-in must fit one reply frame, or GET would silently
    truncate a firing schedule. 165 bytes vs 253."""

    def test_worst_case_get_reply_fits(self):
        name = "X" * 15  # PROFILE_NAME_MAX_LEN
        segs = [
            devices.ProfileSegment(target_c=float(i * 100), ramp_c_per_hr=60.0, dwell_min=i)
            for i in range(12)
        ]
        payload = bytes([PROFILES_CMD_GET, 1, BUILTIN, len(name)]) + name.encode()
        payload += bytes([0x1F, len(segs)])
        for s in segs:
            payload += struct.pack("<ffI", s.target_c, s.ramp_c_per_hr, s.dwell_min)
        self.assertEqual(len(payload), 165)
        self.assertLessEqual(len(payload), UART_PROTO_MAX_PAYLOAD)

        sub, detail = devices.parse_profiles_response(payload)
        self.assertEqual(sub, PROFILES_CMD_GET)
        self.assertEqual(len(detail.segments), 12)
        self.assertTrue(detail.builtin)
        self.assertEqual(detail.segments[11].dwell_min, 11)


class PagedListTests(unittest.TestCase):
    def test_list_request_carries_start_id(self):
        self.assertEqual(devices.profiles_list(), bytes([PROFILES_CMD_LIST, 0]))
        self.assertEqual(
            devices.profiles_list(BUILTIN), bytes([PROFILES_CMD_LIST, BUILTIN])
        )

    @staticmethod
    def _list_reply(entries):
        out = bytes([PROFILES_CMD_LIST, len(entries)])
        for pid, name in entries:
            out += bytes([pid, len(name)]) + name.encode() + bytes([0x01, 4])
        return out

    def test_full_catalogue_does_not_fit_one_frame(self):
        # This is why LIST is paged rather than one-shot: 8 user slots plus 28
        # shipped schedules at up to 19 bytes each.
        entries = [(i, "SLOT%d" % i) for i in range(PROFILES_MAX_COUNT)]
        entries += [
            (PROFILES_BUILTIN_ID_BASE + i, "BUILTIN%02d" % i) for i in range(28)
        ]
        self.assertGreater(len(self._list_reply(entries)), UART_PROTO_MAX_PAYLOAD)

    def test_pages_parse_and_mark_builtins(self):
        page = self._list_reply([(0, "user"), (BUILTIN, "C6DHSC")])
        sub, summaries = devices.parse_profiles_response(page)
        self.assertEqual(sub, PROFILES_CMD_LIST)
        self.assertFalse(summaries[0].builtin)
        self.assertTrue(summaries[1].builtin)

    def test_list_all_pages_until_empty(self):
        pages = [
            [
                devices.ProfileSummary(id=0, name="user", zone_mask=1, segment_count=2),
                devices.ProfileSummary(
                    id=PROFILES_BUILTIN_ID_BASE, name="b0", zone_mask=1, segment_count=3
                ),
            ],
            [
                devices.ProfileSummary(
                    id=PROFILES_BUILTIN_ID_BASE + 1, name="b1", zone_mask=1, segment_count=3
                )
            ],
            [],
        ]
        asked: "list[int]" = []

        from kilnctrl.profiles import ProfilesClient

        class Stub:
            def list(self, start_id=0, timeout=0.0):
                asked.append(start_id)
                return pages.pop(0)

            list_all = ProfilesClient.list_all

        got = Stub().list_all()
        self.assertEqual(asked, [0, PROFILES_BUILTIN_ID_BASE + 1, PROFILES_BUILTIN_ID_BASE + 2])
        self.assertEqual([s.id for s in got], [0, PROFILES_BUILTIN_ID_BASE, PROFILES_BUILTIN_ID_BASE + 1])

    def test_list_all_stops_if_firmware_does_not_advance(self):
        stuck = [devices.ProfileSummary(id=0, name="u", zone_mask=1, segment_count=1)]

        from kilnctrl.profiles import ProfilesClient

        calls = []

        class Stub:
            def list(self, start_id=0, timeout=0.0):
                calls.append(start_id)
                return stuck if len(calls) < 5 else []

            list_all = ProfilesClient.list_all

        got = Stub().list_all()
        self.assertEqual([s.id for s in got], [0])
        self.assertLess(len(calls), 4)  # advanced past 0, then found nothing


if __name__ == "__main__":
    unittest.main()
