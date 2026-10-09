#!/usr/bin/env python3
"""Tests for kilnctrl.fake_peer -- the software peer stub + fault injector
for the ESP<->Pico kilnlink, per tools/PcTools/TODO.md item 5.

These tests exercise the REAL codec (kilnctrl.protocol's Frame/stuff/
unstuff/FrameDecoder, and kilnlink_codec's payload encoders) against the
fake transport, not just the fake peer's own bookkeeping -- the point is
to prove the client-side codec behaves correctly when the wire drops,
corrupts, truncates, or duplicates a frame, per the specific
firmware/CommonFW/docs/LINK_PROTOCOL.md rules cited in fake_peer.py's
module docstring.

No real UART, no board, no subprocess -- pure in-process byte queues.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import kilnlink_codec  # noqa: E402
from kilnctrl.fake_peer import (  # noqa: E402
    FakeEspPeer,
    FakeSaftyPeer,
    FakeWire,
    FaultInjector,
)
from kilnctrl.protocol import (  # noqa: E402
    Device,
    Frame,
    MsgType,
)


def _make_broadcast_frame(payload: bytes, msg_index: int = 1) -> Frame:
    return Frame(
        msg_type=MsgType.BROADCAST,
        msg_index=msg_index,
        src_device=Device.ESP,
        src_task=7,
        dst_device=Device.HOST,
        dst_task=7,
        payload=payload,
    )


class CleanDeliveryTests(unittest.TestCase):
    """Baseline: two fake peers wired together with no faults deliver a
    real Frame end to end, decoded byte-identically."""

    def setUp(self):
        # esp -> safety wire and safety -> esp wire, each direction its own
        # FakeWire, matching the real link's two independent optocoupled
        # signal paths (LINK_PROTOCOL.md sec 1/3).
        self.esp_to_safety = FakeWire()
        self.safety_to_esp = FakeWire()
        self.esp = FakeEspPeer(inbound=self.safety_to_esp, outbound=self.esp_to_safety)
        self.safety = FakeSaftyPeer(inbound=self.esp_to_safety, outbound=self.safety_to_esp)

    def test_broadcast_roundtrip_no_faults(self):
        payload = kilnlink_codec.encode_get_fw_version({})
        frame = _make_broadcast_frame(payload)
        self.esp.send_broadcast(frame.to_wire())
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 1)
        self.assertEqual(self.safety.frames_bad, 0)
        self.assertEqual(self.safety.received[0], frame.to_raw())


class DropFaultTests(unittest.TestCase):
    """LINK_PROTOCOL.md sec 2 rule 2: 'The Pico never retransmits. A lost
    frame is lost; the next one is 500 ms away.' -- so a dropped BROADCAST
    must simply vanish: no exception, no frame seen, no retry from this
    side of the link (retries, if any, are the ESP's job per sec 2's
    closing paragraph, not the transport's)."""

    def setUp(self):
        self.wire = FakeWire()
        self.safety = FakeSaftyPeer(inbound=self.wire, outbound=FakeWire())
        self.injector = FaultInjector(dest=self.wire)

    def test_dropped_frame_never_arrives(self):
        frame = _make_broadcast_frame(b"\x01\x00")
        self.injector.send_stuffed_frame(frame.to_wire(), force="drop")
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 0)
        self.assertEqual(self.safety.frames_bad, 0)
        self.assertEqual(self.safety.received, [])

    def test_next_frame_after_a_drop_still_arrives(self):
        f1 = _make_broadcast_frame(b"\x01\x00", msg_index=1)
        f2 = _make_broadcast_frame(b"\x01\x00", msg_index=2)
        self.injector.send_stuffed_frame(f1.to_wire(), force="drop")
        self.injector.send_stuffed_frame(f2.to_wire())
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 1)
        self.assertEqual(self.safety.received[0], f2.to_raw())


class CorruptCrcFaultTests(unittest.TestCase):
    """LINK_PROTOCOL.md sec 3, 'Receiver robustness': a corrupt frame must
    be recognised and dropped, never mis-parsed as valid data. Frame.from_raw
    raising FrameError on CRC mismatch (protocol.py) is what the real
    receive path relies on."""

    def setUp(self):
        self.wire = FakeWire()
        self.safety = FakeSaftyPeer(inbound=self.wire, outbound=FakeWire())
        self.injector = FaultInjector(dest=self.wire)

    def test_corrupted_crc_is_rejected_not_silently_accepted(self):
        frame = _make_broadcast_frame(b"\x07\x01\x02\x03\x04\x05\x06")
        self.injector.send_stuffed_frame(frame.to_wire(), force="corrupt_crc")
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 0)
        self.assertEqual(self.safety.frames_bad, 1)

    def test_resyncs_cleanly_after_a_corrupted_frame(self):
        bad = _make_broadcast_frame(b"\x07\x01\x02", msg_index=1)
        good = _make_broadcast_frame(b"\x07\x03\x04", msg_index=2)
        self.injector.send_stuffed_frame(bad.to_wire(), force="corrupt_crc")
        self.injector.send_stuffed_frame(good.to_wire())
        self.safety.poll()
        self.assertEqual(self.safety.frames_bad, 1)
        self.assertEqual(self.safety.frames_ok, 1)
        self.assertEqual(self.safety.received[0], good.to_raw())


class TruncateFaultTests(unittest.TestCase):
    """LINK_PROTOCOL.md sec 3: 'Resynchronise on 0x7E unconditionally, from
    any state. A delimiter is always a frame boundary.' A half-frame (e.g.
    an ESP mid-reset) must not wedge the decoder or bleed into the next
    frame's bytes."""

    def setUp(self):
        self.wire = FakeWire()
        self.safety = FakeSaftyPeer(inbound=self.wire, outbound=FakeWire())
        self.injector = FaultInjector(dest=self.wire)

    def test_truncated_frame_produces_no_valid_frame(self):
        frame = _make_broadcast_frame(b"\x07" + bytes(range(20)))
        self.injector.send_stuffed_frame(frame.to_wire(), force="truncate")
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 0)

    def test_decoder_resyncs_on_next_delimiter_after_truncation(self):
        truncated = _make_broadcast_frame(b"\x07" + bytes(range(20)), msg_index=1)
        good = _make_broadcast_frame(b"\x01\x02", msg_index=2)
        self.injector.send_stuffed_frame(truncated.to_wire(), force="truncate")
        self.injector.send_stuffed_frame(good.to_wire())
        self.safety.poll()
        # The truncated frame must not have produced a false-positive
        # decode, and the good frame right behind it must still arrive
        # cleanly -- proving 0x7E resync isn't corrupted by the preceding
        # partial frame.
        self.assertIn(good.to_raw(), self.safety.received)
        self.assertEqual(self.safety.frames_ok, 1)


class DuplicateFaultTests(unittest.TestCase):
    """protocol.py's MsgType.BROADCAST docstring: 'no ACK, no retry, no
    dedup' -- a duplicate BROADCAST is not a wire error to guard against,
    it is within spec, and idempotent handling is the caller's job, not
    the framing layer's. This asserts the framing layer decodes both
    copies identically rather than choking on the repeat."""

    def setUp(self):
        self.wire = FakeWire()
        self.safety = FakeSaftyPeer(inbound=self.wire, outbound=FakeWire())
        self.injector = FaultInjector(dest=self.wire)

    def test_duplicate_frame_decodes_twice_identically(self):
        frame = _make_broadcast_frame(b"\x0f\x00\x01", msg_index=5)
        self.injector.send_stuffed_frame(frame.to_wire(), force="duplicate")
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 2)
        self.assertEqual(self.safety.received[0], self.safety.received[1])
        self.assertEqual(self.safety.received[0], frame.to_raw())


class NonBlockingTxRingTests(unittest.TestCase):
    """LINK_PROTOCOL.md sec 2 rule 3: 'The Pico never blocks on TX ... if
    the ring is full, the frame is dropped, a counter is incremented, and
    the caller returns immediately.'"""

    def test_full_ring_drops_and_counts_without_blocking(self):
        outbound = FakeWire()
        safety = FakeSaftyPeer(inbound=FakeWire(), outbound=outbound, tx_ring_capacity=10)
        frame = _make_broadcast_frame(b"\x01" + bytes(30))  # payload alone exceeds capacity
        ok = safety.send_telemetry(frame.to_wire())
        self.assertFalse(ok)
        self.assertEqual(safety.tx_frames_dropped, 1)
        self.assertEqual(len(outbound), 0)

    def test_frame_fits_ring_sends_normally(self):
        outbound = FakeWire()
        safety = FakeSaftyPeer(inbound=FakeWire(), outbound=outbound, tx_ring_capacity=4096)
        frame = _make_broadcast_frame(b"\x01\x00")
        ok = safety.send_telemetry(frame.to_wire())
        self.assertTrue(ok)
        self.assertEqual(safety.tx_frames_dropped, 0)
        self.assertGreater(len(outbound), 0)


class EspRetryTests(unittest.TestCase):
    """LINK_PROTOCOL.md sec 2: 'The ESP re-sends the request until it gets
    an answer or gives up.' -- the retry obligation belongs to the ESP
    side only; this proves the stub models that asymmetry (FakeSaftyPeer
    has no equivalent retry method at all)."""

    def test_esp_retries_until_reply_arrives(self):
        esp_inbound = FakeWire()  # safety -> esp
        esp_outbound = FakeWire()  # esp -> safety
        esp = FakeEspPeer(inbound=esp_inbound, outbound=esp_outbound)

        got = {"reply": False}

        def build_request():
            return _make_broadcast_frame(b"\x0b").to_wire()

        # Simulate the Pico only answering on the 3rd attempt by directly
        # injecting a reply frame onto esp_inbound once enough requests
        # have accumulated on esp_outbound.
        attempts_seen = []

        def got_reply():
            attempts_seen.append(len(esp_outbound))
            if len(attempts_seen) >= 3:
                reply = _make_broadcast_frame(b"\x0b\x00\x01", msg_index=99)
                esp_inbound.write(reply.to_wire())
                esp.poll()
                got["reply"] = bool(esp.received)
            return got["reply"]

        attempts = esp.request_with_retry(build_request, max_attempts=10, got_reply=got_reply)
        self.assertEqual(attempts, 3)
        self.assertTrue(got["reply"])

    def test_esp_gives_up_after_max_attempts_if_never_answered(self):
        esp = FakeEspPeer(inbound=FakeWire(), outbound=FakeWire())
        attempts = esp.request_with_retry(
            lambda: _make_broadcast_frame(b"\x0b").to_wire(),
            max_attempts=5,
            got_reply=lambda: False,
        )
        self.assertEqual(attempts, 5)
        self.assertEqual(esp.frames_ok, 0)


class PayloadCodecOverFakeTransportTests(unittest.TestCase):
    """Ties kilnlink_codec's real payload encoders to the fake transport --
    proving the payload bytes a firmware-facing encoder produces survive a
    round trip through the framing layer unmodified, and that corruption
    injected on the wire is caught before the payload codec would ever see
    garbage."""

    def setUp(self):
        self.wire = FakeWire()
        self.safety = FakeSaftyPeer(inbound=self.wire, outbound=FakeWire())
        self.injector = FaultInjector(dest=self.wire)

    def test_clear_trip_payload_survives_clean_transport(self):
        payload = kilnlink_codec.encode_clear_trip({"trip_mask": 0x0003})
        frame = _make_broadcast_frame(payload)
        self.injector.send_stuffed_frame(frame.to_wire())
        self.safety.poll()
        self.assertEqual(self.safety.frames_ok, 1)
        received_payload = Frame.from_raw(self.safety.received[0]).payload
        self.assertEqual(received_payload, payload)

    def test_clear_trip_payload_never_reaches_codec_when_crc_corrupted(self):
        payload = kilnlink_codec.encode_clear_trip({"trip_mask": 0x0003})
        frame = _make_broadcast_frame(payload)
        self.injector.send_stuffed_frame(frame.to_wire(), force="corrupt_crc")
        self.safety.poll()
        # Rejected at the framing layer -- never even reached the point
        # where a payload decoder would run on it.
        self.assertEqual(self.safety.frames_ok, 0)
        self.assertEqual(self.safety.received, [])


if __name__ == "__main__":
    unittest.main()
