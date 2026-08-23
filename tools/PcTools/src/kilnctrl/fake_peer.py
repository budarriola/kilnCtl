"""Pure-Python software peer stubs for the ESP<->Pico kilnlink, plus fault
injection, for testing kilnctrl.protocol's framing codec (and, where
reachable, kilnlink_codec's payload encoders) with no board and no real
UART -- see firmware/CommonFW/docs/LINK_PROTOCOL.md and
tools/PcTools/TODO.md, "5. Software peer stub, both directions, with fault
injection".

This is a TEST support module, not production code: nothing in gui.py,
mcp_server.py, or serial_link.py imports it. It exists so a test can wire
two fake peers together over a byte-queue "wire" and assert how
kilnctrl.protocol's real Frame/FrameDecoder/stuff/unstuff behave when that
wire drops, corrupts, truncates, or duplicates frames -- the failure modes
LINK_PROTOCOL.md's own rules make claims about:

* "The Pico never retransmits. A lost frame is lost." (sec 2, rule 2) --
  DROP is meaningful because there is no retry to mask it.
* "Receiver robustness ... required, not optional ... Resynchronise on 0x7E
  unconditionally, from any state." (sec 3) -- TRUNCATE tests that a
  half-frame followed by a fresh DELIM does not wedge the decoder or bleed
  into the next frame.
* CRC covers header+payload, and a corrupt frame must be dropped rather
  than mis-parsed (sec 3 framing table + "Receiver robustness"). CORRUPT_CRC
  exercises that.
* BROADCAST is documented as "no ACK, no retry, no dedup" (protocol.py's
  MsgType.BROADCAST docstring, mirroring uart_protocol.c) -- so a duplicate
  on the wire is not an error state to guard against, it is normal, and the
  receiving side must decode it the same way twice. DUPLICATE tests that.

Deliberately NOT implemented: byte-level reordering. The link is a single
UART byte stream (sec 3: "8N1, 9600 baud", one TX pin, one RX pin, no
flow control) -- frames cannot arrive out of the order they were clocked
out in. Nothing in LINK_PROTOCOL.md claims otherwise; the *seq*/*boot_id*
fields in the context and status frames exist to detect *loss*, not
*reordering* (sec 4's "seq detects loss without needing an ACK"). A fault
type with no protocol claim to falsify would just be untested scaffolding.
"""

from __future__ import annotations

import random
from dataclasses import dataclass, field
from typing import Callable, Optional

from .protocol import (
    FRAME_DELIM,
    FrameDecoder,
)

__all__ = [
    "FakeWire",
    "FaultInjector",
    "FaultConfig",
    "FakeEspPeer",
    "FakeSaftyPeer",
]


# ---------------------------------------------------------------------------
# The wire: a byte queue standing in for the UART, matching the
# write(bytes)/read(n)-shaped interface kilnctrl.serial_link.UartLink wraps
# around pyserial's Serial object (see serial_link.py's use of
# self._ser.write()/self._ser.read()).
# ---------------------------------------------------------------------------
@dataclass
class FakeWire:
    """One direction of a byte-queue transport.

    ``write()`` is the sender's side (mimics ``Serial.write``); ``read()``
    is the receiver's side (mimics ``Serial.read``, non-blocking here since
    everything is already buffered -- there is no real timing to wait on in
    a pure-Python stub).
    """

    _buf: bytearray = field(default_factory=bytearray, init=False)

    def write(self, data: bytes) -> int:
        self._buf.extend(data)
        return len(data)

    def read(self, n: int = -1) -> bytes:
        if n < 0 or n >= len(self._buf):
            out = bytes(self._buf)
            self._buf.clear()
            return out
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def __len__(self) -> int:
        return len(self._buf)


# ---------------------------------------------------------------------------
# Fault injection
# ---------------------------------------------------------------------------
@dataclass
class FaultConfig:
    """Per-fault trigger probabilities, each independent and in [0, 1].

    Deterministic tests should pass explicit fault calls on
    :class:`FaultInjector` instead of relying on probabilities; the
    probabilities exist for soak-style fuzzing across many frames.
    """

    drop_p: float = 0.0
    corrupt_crc_p: float = 0.0
    truncate_p: float = 0.0
    duplicate_p: float = 0.0
    rng: random.Random = field(default_factory=random.Random)


class FaultInjector:
    """Wraps the send path between two :class:`FakeWire` objects.

    Each ``send_stuffed_frame`` call is one already-stuffed, delimiter
    wrapped frame (i.e. the output of ``Frame.to_wire()``). The injector
    decides, per the configured probabilities or an explicit ``force``
    override, what actually lands on the destination wire.
    """

    def __init__(self, dest: FakeWire, config: Optional[FaultConfig] = None):
        self.dest = dest
        self.config = config or FaultConfig()
        #: record of what happened to each frame, for test assertions.
        self.log: list[str] = []

    def send_stuffed_frame(self, frame_bytes: bytes, force: Optional[str] = None) -> None:
        """Deliver (possibly mangled) ``frame_bytes`` to ``self.dest``.

        ``force`` overrides the probabilistic config with one of "drop",
        "corrupt_crc", "truncate", "duplicate", or None (deliver clean) --
        this is the deterministic path tests should use.
        """
        action = force if force is not None else self._roll()
        if action == "drop":
            self.log.append("drop")
            return
        if action == "corrupt_crc":
            self.log.append("corrupt_crc")
            self.dest.write(_corrupt_crc(frame_bytes))
            return
        if action == "truncate":
            self.log.append("truncate")
            self.dest.write(_truncate(frame_bytes))
            return
        if action == "duplicate":
            self.log.append("duplicate")
            self.dest.write(frame_bytes)
            self.dest.write(frame_bytes)
            return
        self.log.append("clean")
        self.dest.write(frame_bytes)

    def _roll(self) -> Optional[str]:
        cfg = self.config
        r = cfg.rng
        if cfg.drop_p and r.random() < cfg.drop_p:
            return "drop"
        if cfg.corrupt_crc_p and r.random() < cfg.corrupt_crc_p:
            return "corrupt_crc"
        if cfg.truncate_p and r.random() < cfg.truncate_p:
            return "truncate"
        if cfg.duplicate_p and r.random() < cfg.duplicate_p:
            return "duplicate"
        return None


def _corrupt_crc(frame_bytes: bytes) -> bytes:
    """Flip a bit in the last non-delimiter byte, i.e. inside the CRC's low
    byte -- reliably invalidates the CRC without touching the framing
    delimiters, so the decoder still recognises frame boundaries and the
    corruption is caught by ``Frame.from_raw``'s CRC check (protocol.py),
    exactly the case LINK_PROTOCOL.md sec 3's "Receiver robustness" list
    requires the receiver to survive.
    """
    b = bytearray(frame_bytes)
    # Walk backward from the trailing delimiter to find the last byte that
    # isn't itself a delimiter (it may be escaped, but flipping any bit of
    # an escaped or unescaped CRC byte still changes the decoded CRC).
    for i in range(len(b) - 2, 0, -1):
        if b[i] != FRAME_DELIM:
            b[i] ^= 0x01
            break
    return bytes(b)


def _truncate(frame_bytes: bytes) -> bytes:
    """Cut the frame off partway through, dropping the trailing delimiter
    (and possibly more) so the receiver is left mid-frame -- exactly the
    "half-frame from an ESP mid-reset" case sec 3 names explicitly.
    """
    if len(frame_bytes) <= 2:
        return b""
    cut = max(1, len(frame_bytes) // 2)
    return frame_bytes[:cut]


# ---------------------------------------------------------------------------
# Fake peers
# ---------------------------------------------------------------------------
@dataclass
class _ReceivedFrame:
    raw: bytes  # unstuffed frame body, undecoded (caller applies Frame.from_raw)


class _FakePeerBase:
    """Shared decode-and-dispatch plumbing for both fake peers.

    Each peer owns a :class:`FrameDecoder` (the real one from protocol.py)
    fed with whatever bytes arrive on its inbound :class:`FakeWire`, and
    keeps every successfully-*framed* (but not yet CRC/type validated) raw
    body it has seen, plus counters mirroring the ones LINK_PROTOCOL.md sec
    4's DIAG frame documents (context_frames_ok / context_frames_bad),
    which is what makes this stub useful for asserting loss/corruption
    behaviour rather than just plumbing bytes around.
    """

    def __init__(self, inbound: FakeWire, outbound: FakeWire):
        self.inbound = inbound
        self.outbound = outbound
        self._decoder = FrameDecoder()
        self.frames_ok = 0
        self.frames_bad = 0
        self.received: list[bytes] = []  # raw (unstuffed) frame bodies that decoded cleanly
        self.on_frame: Optional[Callable[[bytes], None]] = None

    def poll(self) -> None:
        """Drain the inbound wire and decode whatever whole frames arrived.

        Mirrors the non-blocking, buffer-fed nature of the real RX path
        (serial_link.py reads whatever bytes are available, never blocks
        waiting for a specific count) -- there is nothing here that can
        hang, matching LINK_PROTOCOL.md sec 2 rule 3's non-blocking-RX
        requirement translated to a test double.
        """
        data = self.inbound.read()
        if not data:
            return
        for raw in self._decoder.feed(data):
            from .protocol import Frame, FrameError

            try:
                frame = Frame.from_raw(raw)
            except FrameError:
                self.frames_bad += 1
                continue
            self.frames_ok += 1
            self.received.append(bytes(raw))
            if self.on_frame is not None:
                self.on_frame(frame.payload)


class FakeEspPeer(_FakePeerBase):
    """Stands in for KilnFW's side of the link.

    Per LINK_PROTOCOL.md sec 2, the ESP is the side allowed to block and
    retry -- e.g. the boot-time ANNOUNCE_VERSION/GET_FW_VERSION exchange
    (sec 4.3/sec 2 "Retries live entirely on the ESP side"). This stub
    exposes that as an explicit retry loop rather than silently retrying,
    so a test can assert *how many* attempts were made.
    """

    def send_broadcast(self, frame_wire_bytes: bytes, injector: Optional[FaultInjector] = None,
                        force_fault: Optional[str] = None) -> None:
        if injector is not None:
            injector.send_stuffed_frame(frame_wire_bytes, force=force_fault)
        else:
            self.outbound.write(frame_wire_bytes)

    def request_with_retry(
        self,
        build_request: Callable[[], bytes],
        max_attempts: int,
        got_reply: Callable[[], bool],
        injector: Optional[FaultInjector] = None,
    ) -> int:
        """Send ``build_request()`` up to ``max_attempts`` times, polling
        this peer's inbound wire after each send, stopping as soon as
        ``got_reply()`` is true. Returns the number of attempts made.

        This models sec 2's "The ESP re-sends the request until it gets an
        answer or gives up" -- the one retry obligation LINK_PROTOCOL.md
        actually specifies (the Pico side has none, see FakeSaftyPeer).
        """
        attempts = 0
        for _ in range(max_attempts):
            attempts += 1
            self.send_broadcast(build_request(), injector=injector)
            self.poll()
            if got_reply():
                break
        return attempts


class FakeSaftyPeer(_FakePeerBase):
    """Stands in for SaftyFW's side of the link.

    Enforces the asymmetric rules from LINK_PROTOCOL.md sec 2 as hard
    behavioural constraints on the stub itself, not just as documentation:

    1. never sends an ACK, never expects one -- there is no ACK path here
       at all, only ``send_telemetry``.
    2. never retransmits -- ``send_telemetry`` has no retry parameter,
       unlike ``FakeEspPeer.request_with_retry``.
    3. never blocks on TX -- ``send_telemetry`` drops the frame and counts
       it instead of blocking when the outbound ring is "full" (modelled
       as a max queued-byte high-water mark), mirroring "if the ring is
       full, the frame is dropped, a counter is incremented, and the
       caller returns immediately."
    """

    def __init__(self, inbound: FakeWire, outbound: FakeWire, tx_ring_capacity: int = 4096):
        super().__init__(inbound, outbound)
        self.tx_ring_capacity = tx_ring_capacity
        self.tx_frames_dropped = 0

    def send_telemetry(self, frame_wire_bytes: bytes, injector: Optional[FaultInjector] = None,
                        force_fault: Optional[str] = None) -> bool:
        """Fire-and-forget send. Returns False (and increments
        ``tx_frames_dropped``) instead of ever blocking, if the modelled TX
        ring is full -- never raises, never retries.
        """
        if len(self.outbound) + len(frame_wire_bytes) > self.tx_ring_capacity:
            self.tx_frames_dropped += 1
            return False
        if injector is not None:
            injector.send_stuffed_frame(frame_wire_bytes, force=force_fault)
        else:
            self.outbound.write(frame_wire_bytes)
        return True
