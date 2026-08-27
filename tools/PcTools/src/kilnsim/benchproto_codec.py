"""Pure-Python mirror of firmware/CommonFW's ``benchproto`` frame envelope
(``benchproto_frame.c``/``.h``) and reliability layer (``benchproto_link.c``/
``.h``) -- see ``firmware/CommonFW/docs/BENCHPROTO.md`` for the spec this
module implements.

This is ``benchproto``'s "second, independent implementation for
cross-checking" that ``BENCHPROTO.md`` section 7 calls out by name: the C
library has host tests and a shared vector manifest
(``firmware/CommonFW/test/vectors/benchproto_frame_vectors.json``) but, until
now, no second production implementation to check it against. ``kilnsim``'s
:mod:`test_kilnsim_benchproto_codec` proves this module byte-identical to the
C encoder/decoder against that same manifest, the same "prove it twice"
pattern ``tools/PcTools/src/kilnctrl/kilnlink_codec.py`` +
``selfcheck.py`` already established for the unrelated ``kilnlink`` protocol
family (see that module's docstring).

Deliberately freestanding, like its C counterpart: every function here is a
pure transform on caller-supplied bytes/values -- no I/O, no threading, no
global state. The reliability *state machine* (:class:`BenchprotoLink`,
:class:`PendingRequest`) mirrors ``benchproto_link.c`` field-for-field and
function-for-function; the actual byte pipe and retry *timer* are
:mod:`kilnsim.link`'s job (``BENCHPROTO.md`` section 5's transport-vs-contract
split), same as it is for the firmware side.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import Optional

from kilnctrl.crc16 import crc16_ccitt_false

__all__ = [
    "FRAME_MAX_PAYLOAD",
    "FRAME_HEADER_LEN",
    "FRAME_CRC_LEN",
    "DELIM",
    "ESC",
    "ESC_XOR",
    "MsgType",
    "FrameError",
    "Frame",
    "crc16_ccitt_false",
    "stuff",
    "unstuff",
    "encode_raw",
    "decode_raw",
    "encode_frame",
    "decode_frame",
    "MAX_TASKS",
    "MAX_RETRIES",
    "DEDUP_DEPTH",
    "DEFAULT_ACK_TIMEOUT_S",
    "LinkAction",
    "TaskSlot",
    "BenchprotoLink",
    "PendingRequest",
]

# ---------------------------------------------------------------------------
# Section 2/3: framing constants (BENCHPROTO.md)
# ---------------------------------------------------------------------------
FRAME_MAX_PAYLOAD = 128
FRAME_HEADER_LEN = 8
FRAME_CRC_LEN = 2
FRAME_RAW_MAX = FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD + FRAME_CRC_LEN
FRAME_STUFFED_MAX = FRAME_RAW_MAX * 2 + 2

DELIM = 0x7E
ESC = 0x7D
ESC_XOR = 0x20


class MsgType(enum.IntEnum):
    DATA = 0x01
    ACK = 0x02
    NACK = 0x03
    BROADCAST = 0x04


class FrameError(ValueError):
    """Raised by the decode side on any of benchproto_frame_status_t's
    error conditions (BENCHPROTO_FRAME_ERR_*) except OK."""


@dataclass
class Frame:
    msg_type: MsgType
    msg_index: int
    src_device: int
    src_task: int
    dst_device: int
    dst_task: int
    payload: bytes = b""

    @property
    def length(self) -> int:
        return len(self.payload)


# ---------------------------------------------------------------------------
# Section 3: CRC-16/CCITT-FALSE (see kilnctrl.crc16 for the parameters and
# the algorithm itself). benchproto_crc16_ccitt_false() (the C side,
# BENCHPROTO.md sec 3) is its own from-scratch implementation, matching the
# algorithm but not sharing code with kilnlink's -- that's the "second,
# independent implementation for cross-checking" this module's docstring
# describes. The *Python* side is different: kilnctrl.protocol already
# carries a pure-Python port of the same CRC for the unrelated kilnlink
# framing layer (justified there because pc_tools can't link the C
# library). A second, independently-typed Python copy of the same bit-loop
# would just be the thing tools/check_no_duplicate_crc.ps1 exists to catch
# -- both codecs are pure Python living side by side under
# tools/PcTools/src, so they share kilnctrl.crc16.crc16_ccitt_false
# instead. Re-exported here (see __all__) so existing callers of
# kilnsim.benchproto_codec.crc16_ccitt_false keep working unchanged.
# ---------------------------------------------------------------------------

# Known-answer test embedded in BENCHPROTO.md sec 3 itself.
assert crc16_ccitt_false(b"123456789") == 0x29B1


# ---------------------------------------------------------------------------
# Section 2: SLIP-style byte stuffing (benchproto_stuff/_unstuff)
# ---------------------------------------------------------------------------
def stuff(raw: bytes) -> bytes:
    out = bytearray()
    out.append(DELIM)
    for b in raw:
        if b == DELIM or b == ESC:
            out.append(ESC)
            out.append(b ^ ESC_XOR)
        else:
            out.append(b)
    out.append(DELIM)
    return bytes(out)


def unstuff(data: bytes) -> bytes:
    """Reverse of :func:`stuff` for a single frame body. Accepts `data` with
    or without its surrounding 0x7E delimiters, mirroring
    benchproto_unstuff()'s leniency. Raises FrameError on an unterminated
    trailing 0x7D escape."""
    start = 0
    end = len(data)
    if end > 0 and data[0] == DELIM:
        start = 1
    if end > start and data[end - 1] == DELIM:
        end -= 1

    out = bytearray()
    escaped = False
    for i in range(start, end):
        b = data[i]
        if escaped:
            out.append(b ^ ESC_XOR)
            escaped = False
        elif b == ESC:
            escaped = True
        else:
            out.append(b)
    if escaped:
        raise FrameError("unterminated trailing 0x7D escape")
    return bytes(out)


# ---------------------------------------------------------------------------
# Section 3: raw (unstuffed) frame encode/decode
# (benchproto_frame_encode_raw / benchproto_frame_decode)
# ---------------------------------------------------------------------------
def encode_raw(frame: Frame) -> bytes:
    if frame.length > FRAME_MAX_PAYLOAD:
        raise FrameError(
            f"payload length {frame.length} exceeds FRAME_MAX_PAYLOAD ({FRAME_MAX_PAYLOAD})"
        )
    header = bytes(
        (
            int(frame.msg_type),
            (frame.msg_index >> 8) & 0xFF,
            frame.msg_index & 0xFF,
            frame.src_device & 0xFF,
            frame.src_task & 0xFF,
            frame.dst_device & 0xFF,
            frame.dst_task & 0xFF,
            frame.length & 0xFF,
        )
    )
    body = header + frame.payload
    crc = crc16_ccitt_false(body)
    return body + bytes(((crc >> 8) & 0xFF, crc & 0xFF))


def decode_raw(raw: bytes) -> Frame:
    if len(raw) < FRAME_HEADER_LEN + FRAME_CRC_LEN:
        raise FrameError(
            f"frame too short: {len(raw)} bytes, need at least "
            f"{FRAME_HEADER_LEN + FRAME_CRC_LEN}"
        )
    length = raw[7]
    if length > FRAME_MAX_PAYLOAD:
        raise FrameError(f"LENGTH byte {length} exceeds FRAME_MAX_PAYLOAD ({FRAME_MAX_PAYLOAD})")
    expected_len = FRAME_HEADER_LEN + length + FRAME_CRC_LEN
    if len(raw) != expected_len:
        raise FrameError(f"LENGTH/buffer-size mismatch: expected {expected_len}, got {len(raw)}")

    expected_crc = crc16_ccitt_false(raw[: FRAME_HEADER_LEN + length])
    actual_crc = (raw[FRAME_HEADER_LEN + length] << 8) | raw[FRAME_HEADER_LEN + length + 1]
    if expected_crc != actual_crc:
        raise FrameError(f"CRC mismatch: expected 0x{expected_crc:04X}, got 0x{actual_crc:04X}")

    type_byte = raw[0]
    try:
        msg_type = MsgType(type_byte)
    except ValueError as exc:
        raise FrameError(f"unknown MSG_TYPE byte 0x{type_byte:02X}") from exc

    return Frame(
        msg_type=msg_type,
        msg_index=(raw[1] << 8) | raw[2],
        src_device=raw[3],
        src_task=raw[4],
        dst_device=raw[5],
        dst_task=raw[6],
        payload=bytes(raw[FRAME_HEADER_LEN : FRAME_HEADER_LEN + length]),
    )


# ---------------------------------------------------------------------------
# Convenience: full wire round trip (stuff(encode_raw()) / decode_raw(unstuff()))
# ---------------------------------------------------------------------------
def encode_frame(frame: Frame) -> bytes:
    return stuff(encode_raw(frame))


def decode_frame(wire: bytes) -> Frame:
    return decode_raw(unstuff(wire))


# ---------------------------------------------------------------------------
# Section 4/6 (BENCHPROTO.md): reliability layer -- Python mirror of
# benchproto_link.c/.h. See that file's own doc comments; this is a
# line-for-line port, not a reinterpretation.
# ---------------------------------------------------------------------------
MAX_TASKS = 16
MAX_RETRIES = 10
DEDUP_DEPTH = 4
DEFAULT_ACK_TIMEOUT_S = 0.2  # BENCHPROTO_DEFAULT_ACK_TIMEOUT_MS = 200


class LinkAction(enum.Enum):
    IGNORE = "ignore"
    DELIVER = "deliver"
    DUPLICATE_REACK = "duplicate_reack"
    NACK_UNROUTABLE = "nack_unroutable"
    ACK_MATCHED = "ack_matched"
    NACK_MATCHED = "nack_matched"


@dataclass
class TaskSlot:
    task_id: int
    # Ring of recently-delivered (src_device, src_task, msg_index) tuples,
    # mirroring benchproto_task_slot_t.dedup[BENCHPROTO_DEDUP_DEPTH].
    dedup: list = field(default_factory=lambda: [None] * DEDUP_DEPTH)
    dedup_next: int = 0

    def dedup_check(self, src_device: int, src_task: int, msg_index: int) -> bool:
        return (src_device, src_task, msg_index) in [d for d in self.dedup if d is not None]

    def dedup_record(self, src_device: int, src_task: int, msg_index: int) -> None:
        self.dedup[self.dedup_next] = (src_device, src_task, msg_index)
        self.dedup_next = (self.dedup_next + 1) % DEDUP_DEPTH


class BenchprotoLinkError(RuntimeError):
    pass


class BenchprotoLink:
    """Mirrors benchproto_link_t + its free functions. task registration,
    msg_index issuance, and frame classification -- BENCHPROTO.md sec 4/6."""

    def __init__(self, own_device: int) -> None:
        self.own_device = own_device
        self._next_tx_index = 0
        self._tasks: dict[int, TaskSlot] = {}

    def register_task(self, task_id: int) -> None:
        if task_id in self._tasks:
            raise BenchprotoLinkError(f"task {task_id} already registered")
        if len(self._tasks) >= MAX_TASKS:
            raise BenchprotoLinkError("no task slots left")
        self._tasks[task_id] = TaskSlot(task_id=task_id)

    def unregister_task(self, task_id: int) -> None:
        if task_id not in self._tasks:
            raise BenchprotoLinkError(f"task {task_id} not registered")
        del self._tasks[task_id]

    def is_registered(self, task_id: int) -> bool:
        return task_id in self._tasks

    def next_msg_index(self) -> int:
        idx = self._next_tx_index & 0xFFFF
        self._next_tx_index = (self._next_tx_index + 1) & 0xFFFF
        return idx

    def on_frame(self, pending: "Optional[PendingRequest]", frame: Frame) -> LinkAction:
        if frame.msg_type in (MsgType.ACK, MsgType.NACK):
            if pending is None or not pending.active:
                return LinkAction.IGNORE
            if (
                frame.src_device != pending.dst_device
                or frame.src_task != pending.dst_task
                or frame.msg_index != pending.msg_index
            ):
                return LinkAction.IGNORE
            return LinkAction.ACK_MATCHED if frame.msg_type == MsgType.ACK else LinkAction.NACK_MATCHED

        if frame.msg_type not in (MsgType.DATA, MsgType.BROADCAST):
            return LinkAction.IGNORE

        if frame.dst_device != self.own_device:
            return LinkAction.IGNORE

        slot = self._tasks.get(frame.dst_task)
        if slot is None:
            if frame.msg_type == MsgType.BROADCAST:
                return LinkAction.IGNORE
            return LinkAction.NACK_UNROUTABLE

        if frame.msg_type == MsgType.DATA and slot.dedup_check(
            frame.src_device, frame.src_task, frame.msg_index
        ):
            return LinkAction.DUPLICATE_REACK

        return LinkAction.DELIVER

    def mark_delivered(self, task_id: int, src_device: int, src_task: int, msg_index: int) -> None:
        slot = self._tasks.get(task_id)
        if slot is None:
            raise BenchprotoLinkError(f"task {task_id} not registered")
        slot.dedup_record(src_device, src_task, msg_index)


@dataclass
class PendingRequest:
    """Mirrors benchproto_pending_request_t: outstanding send-and-await-reply
    state for ONE in-flight request (one per allowed concurrency slot,
    BENCHPROTO.md sec 4)."""

    active: bool = False
    dst_device: int = 0
    dst_task: int = 0
    msg_index: int = 0
    attempt: int = 0  # 1-based once begin() is called

    def begin(self, dst_device: int, dst_task: int, msg_index: int) -> None:
        self.active = True
        self.dst_device = dst_device
        self.dst_task = dst_task
        self.msg_index = msg_index
        self.attempt = 1

    def note_retry(self) -> bool:
        """Returns False (and clears self) once another attempt would
        exceed MAX_RETRIES -- the caller's send has timed out."""
        if not self.active:
            return False
        if self.attempt >= MAX_RETRIES:
            self.clear()
            return False
        self.attempt += 1
        return True

    def clear(self) -> None:
        self.active = False
        self.dst_device = 0
        self.dst_task = 0
        self.msg_index = 0
        self.attempt = 0
