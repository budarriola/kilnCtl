#!/usr/bin/env python3
"""Byte-exact cross-check of kilnsim.benchproto_codec against
firmware/CommonFW/test/vectors/benchproto_frame_vectors.json -- the shared
manifest firmware/CommonFW/docs/BENCHPROTO.md sec 7 says a second,
independent implementation should be checked against. Mirrors the existing
convention tools/PcTools/src/kilnctrl/selfcheck.py established for kilnlink's
own vector manifests.

Also covers: reliability-layer (BenchprotoLink/PendingRequest) unit behavior,
CRC/framing hostile-input error handling, and round-trip encode/decode.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import benchproto_codec as bp  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
VECTORS_PATH = os.path.join(
    REPO_ROOT, "firmware", "CommonFW", "test", "vectors", "benchproto_frame_vectors.json"
)


def _load_vectors():
    with open(VECTORS_PATH, "r", encoding="utf-8") as fh:
        return json.load(fh)


class CrcKnownAnswerTests(unittest.TestCase):
    def test_crc_known_answer_from_manifest(self):
        data = _load_vectors()
        ka = data["crc_known_answer"]
        expected = int(ka["crc16_ccitt_false"], 16)
        self.assertEqual(bp.crc16_ccitt_false(ka["input_ascii"].encode("ascii")), expected)


class VectorCrossCheckTests(unittest.TestCase):
    """Byte-identical proof against the C implementation's manifest."""

    def setUp(self):
        self.data = _load_vectors()

    def test_every_vector_raw_and_wire_bytes(self):
        for v in self.data["vectors"]:
            with self.subTest(name=v["name"]):
                frame = bp.Frame(
                    msg_type=bp.MsgType[v["msg_type"]],
                    msg_index=v["msg_index"],
                    src_device=v["src_device"],
                    src_task=v["src_task"],
                    dst_device=v["dst_device"],
                    dst_task=v["dst_task"],
                    payload=bytes.fromhex(v["payload_hex"]),
                )
                raw = bp.encode_raw(frame)
                self.assertEqual(raw.hex(), v["raw_hex"], f"{v['name']}: raw mismatch")

                wire = bp.stuff(raw)
                self.assertEqual(wire.hex(), v["wire_hex"], f"{v['name']}: wire mismatch")

                # Round trip back.
                decoded_raw = bp.decode_raw(bytes.fromhex(v["raw_hex"]))
                self.assertEqual(decoded_raw.msg_type, frame.msg_type)
                self.assertEqual(decoded_raw.msg_index, frame.msg_index)
                self.assertEqual(decoded_raw.src_device, frame.src_device)
                self.assertEqual(decoded_raw.src_task, frame.src_task)
                self.assertEqual(decoded_raw.dst_device, frame.dst_device)
                self.assertEqual(decoded_raw.dst_task, frame.dst_task)
                self.assertEqual(decoded_raw.payload, frame.payload)

                unstuffed = bp.unstuff(bytes.fromhex(v["wire_hex"]))
                self.assertEqual(unstuffed.hex(), v["raw_hex"], f"{v['name']}: unstuff mismatch")

                full = bp.decode_frame(bytes.fromhex(v["wire_hex"]))
                self.assertEqual(full.payload, frame.payload)


class HostileInputTests(unittest.TestCase):
    """Section 58's "hostile_vectors" comment lists these by description
    (no concrete bytes in the manifest, same as the C test) -- constructed
    here directly, same as test_benchproto_frame.c does."""

    def test_truncated_frame(self):
        with self.assertRaises(bp.FrameError):
            bp.decode_raw(bytes([0x01, 0x00]))

    def test_length_too_long(self):
        raw = bytes([0x01, 0, 0, 0, 0, 0, 0, 200]) + b"\x00" * 5
        with self.assertRaises(bp.FrameError):
            bp.decode_raw(raw)

    def test_length_mismatch(self):
        raw = bytes([0x01, 0, 0, 0, 0, 0, 0, 4]) + b"\x00\x00\x00\x00\xFF\xFF"[:5]
        with self.assertRaises(bp.FrameError):
            bp.decode_raw(raw)

    def test_corrupted_crc(self):
        frame = bp.Frame(bp.MsgType.DATA, 1, 0, 1, 1, 1, payload=b"\x01\x02")
        raw = bytearray(bp.encode_raw(frame))
        raw[-1] ^= 0xFF
        with self.assertRaises(bp.FrameError):
            bp.decode_raw(bytes(raw))

    def test_unknown_type_byte(self):
        raw = bytes([0x99, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00])
        with self.assertRaises(bp.FrameError):
            bp.decode_raw(raw)

    def test_unterminated_escape(self):
        with self.assertRaises(bp.FrameError):
            bp.unstuff(bytes([0x7E, 0x01, 0x7D]))

    def test_payload_too_long_to_encode(self):
        frame = bp.Frame(bp.MsgType.DATA, 0, 0, 1, 1, 1, payload=b"\x00" * 200)
        with self.assertRaises(bp.FrameError):
            bp.encode_raw(frame)


class StuffingTests(unittest.TestCase):
    def test_back_to_back_delimiters_ignored_on_unstuff(self):
        # Empty-frame noise: leading/trailing DELIM stripped even with none
        # between (BENCHPROTO.md sec 2).
        self.assertEqual(bp.unstuff(bytes([0x7E, 0x7E])), b"")

    def test_stuff_unstuff_round_trip_arbitrary_bytes(self):
        raw = bytes(range(256)) * 2
        self.assertEqual(bp.unstuff(bp.stuff(raw)), raw)


class ReliabilityTaskRegistrationTests(unittest.TestCase):
    def test_register_and_is_registered(self):
        link = bp.BenchprotoLink(own_device=0)
        link.register_task(1)
        self.assertTrue(link.is_registered(1))
        self.assertFalse(link.is_registered(2))

    def test_double_register_raises(self):
        link = bp.BenchprotoLink(own_device=0)
        link.register_task(1)
        with self.assertRaises(bp.BenchprotoLinkError):
            link.register_task(1)

    def test_unregister(self):
        link = bp.BenchprotoLink(own_device=0)
        link.register_task(1)
        link.unregister_task(1)
        self.assertFalse(link.is_registered(1))

    def test_next_msg_index_increments_and_wraps(self):
        link = bp.BenchprotoLink(own_device=0)
        self.assertEqual(link.next_msg_index(), 0)
        self.assertEqual(link.next_msg_index(), 1)
        link._next_tx_index = 0xFFFF
        self.assertEqual(link.next_msg_index(), 0xFFFF)
        self.assertEqual(link.next_msg_index(), 0)  # wraps like a plain uint16_t


class ReliabilityOnFrameTests(unittest.TestCase):
    def setUp(self):
        self.link = bp.BenchprotoLink(own_device=0)  # HOST
        self.link.register_task(1)

    def test_ack_no_pending_is_ignored(self):
        frame = bp.Frame(bp.MsgType.ACK, 5, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.IGNORE)

    def test_ack_matches_pending(self):
        pending = bp.PendingRequest()
        pending.begin(dst_device=1, dst_task=1, msg_index=5)
        frame = bp.Frame(bp.MsgType.ACK, 5, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(pending, frame), bp.LinkAction.ACK_MATCHED)

    def test_nack_matches_pending(self):
        pending = bp.PendingRequest()
        pending.begin(dst_device=1, dst_task=1, msg_index=5)
        frame = bp.Frame(bp.MsgType.NACK, 5, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(pending, frame), bp.LinkAction.NACK_MATCHED)

    def test_ack_mismatched_index_is_ignored(self):
        pending = bp.PendingRequest()
        pending.begin(dst_device=1, dst_task=1, msg_index=5)
        frame = bp.Frame(bp.MsgType.ACK, 6, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(pending, frame), bp.LinkAction.IGNORE)

    def test_data_to_unregistered_task_is_nack_unroutable(self):
        frame = bp.Frame(bp.MsgType.DATA, 1, 1, 1, 0, 99)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.NACK_UNROUTABLE)

    def test_broadcast_to_unregistered_task_is_ignored_not_nacked(self):
        frame = bp.Frame(bp.MsgType.BROADCAST, 1, 1, 8, 0, 99)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.IGNORE)

    def test_data_to_registered_task_delivers(self):
        frame = bp.Frame(bp.MsgType.DATA, 1, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.DELIVER)

    def test_data_wrong_dst_device_ignored(self):
        frame = bp.Frame(bp.MsgType.DATA, 1, 1, 1, 5, 1)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.IGNORE)

    def test_duplicate_data_after_mark_delivered_is_reack(self):
        frame = bp.Frame(bp.MsgType.DATA, 1, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.DELIVER)
        self.link.mark_delivered(1, src_device=1, src_task=1, msg_index=1)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.DUPLICATE_REACK)

    def test_dedup_ring_depth_evicts_oldest(self):
        # BENCHPROTO_DEDUP_DEPTH = 4: mark 4 different msg_indexes delivered,
        # then a 5th delivery evicts the oldest (index 1), so a retransmit of
        # index 1 is treated as DELIVER again, not DUPLICATE_REACK.
        for idx in range(1, 5):
            self.link.mark_delivered(1, src_device=1, src_task=1, msg_index=idx)
        self.link.mark_delivered(1, src_device=1, src_task=1, msg_index=5)
        frame_idx1 = bp.Frame(bp.MsgType.DATA, 1, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(None, frame_idx1), bp.LinkAction.DELIVER)
        frame_idx5 = bp.Frame(bp.MsgType.DATA, 5, 1, 1, 0, 1)
        self.assertEqual(self.link.on_frame(None, frame_idx5), bp.LinkAction.DUPLICATE_REACK)

    def test_broadcast_never_deduped(self):
        frame = bp.Frame(bp.MsgType.BROADCAST, 1, 1, 8, 0, 1)
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.DELIVER)
        # No mark_delivered call is legal/expected for BROADCAST; a second
        # identical broadcast still DELIVERs (never deduped, sec 4).
        self.assertEqual(self.link.on_frame(None, frame), bp.LinkAction.DELIVER)

    def test_mark_delivered_unregistered_task_raises(self):
        with self.assertRaises(bp.BenchprotoLinkError):
            self.link.mark_delivered(99, src_device=1, src_task=1, msg_index=1)


class PendingRequestRetryTests(unittest.TestCase):
    def test_begin_sets_attempt_1(self):
        p = bp.PendingRequest()
        p.begin(dst_device=1, dst_task=2, msg_index=9)
        self.assertTrue(p.active)
        self.assertEqual(p.attempt, 1)

    def test_note_retry_increments_up_to_max(self):
        p = bp.PendingRequest()
        p.begin(dst_device=1, dst_task=2, msg_index=9)
        for expected in range(2, bp.MAX_RETRIES + 1):
            self.assertTrue(p.note_retry())
            self.assertEqual(p.attempt, expected)

    def test_note_retry_returns_false_and_clears_past_max(self):
        p = bp.PendingRequest()
        p.begin(dst_device=1, dst_task=2, msg_index=9)
        for _ in range(bp.MAX_RETRIES - 1):
            self.assertTrue(p.note_retry())
        self.assertFalse(p.note_retry())
        self.assertFalse(p.active)

    def test_note_retry_on_inactive_returns_false(self):
        p = bp.PendingRequest()
        self.assertFalse(p.note_retry())

    def test_clear(self):
        p = bp.PendingRequest()
        p.begin(dst_device=1, dst_task=2, msg_index=9)
        p.clear()
        self.assertFalse(p.active)
        self.assertEqual(p.msg_index, 0)


if __name__ == "__main__":
    unittest.main()
