"""Decode a Saleae Async Serial capture of the isolated ESP<->Pico
``kilnlink`` UART link into a structured, human-readable frame timeline.

Ground truth: ``firmware/CommonFW/docs/LINK_PROTOCOL.md`` and the actual C
codec (``firmware/CommonFW/src/kilnlink_*.c``) -- where the two disagree, the
C wins (see that document's own header note). This module is a *fourth*
consumer of the wire format, after the two firmwares and ``kilnctrl.protocol``
(the PC<->ESP framing mirror) -- it deliberately reuses ``kilnctrl.protocol``'s
CRC and the byte-stuffing constants rather than re-deriving them, since
``kilnlink_frame.h`` states outright that the envelope (delimiter, escape,
CRC16/CCITT-FALSE, 8-byte header) is shared byte-for-byte between the PC<->ESP
link and the isolated ESP<->Pico link -- only the device/task *values* that
travel inside that header differ (this link's header can carry
``src_device``/``dst_device`` == 2, SAFETY -- a value ``kilnctrl.protocol``'s
own ``Device`` enum deliberately omits for an unrelated reason, see
``tests/test_link_hub_routing.py``; this module does not use that enum).

``tools/PcTools/TODO.md`` capability 2 ("kilnlink frame decoding for a Saleae
capture") was left unbuilt deliberately, pending a board+analyzer on the
bench at once. This module still has no real capture to validate against, so
it is built and tested purely against synthetic fixtures encoded with this
same protocol's own rules (``kilnlink_codec.py``'s encoders, and
``kilnctrl.protocol.Frame`` for framing) -- see ``tests/fixtures/kilnlink/``.
Malformed/partial frames are reported with their byte offset in the captured
stream, never silently dropped -- that is the entire point of this tool: a
human trying to explain *why* the wire went quiet needs to see exactly where
and how framing broke, not just the frames that decoded cleanly.

Two independent input shapes are accepted -- see ``load_bytes()``:

* a raw binary capture (the concatenated bytes exactly as they crossed the
  wire), and
* a Saleae "Export Table Data" CSV from the Async Serial analyzer (columns
  ``Time [s], Value, Parity Error, Framing Error`` -- Value as ``0xHH`` or a
  decimal byte per row). Parity/Framing Error columns are not currently
  interpreted (no real capture to learn their exact export spelling from);
  a byte flagged by the analyzer itself still gets the normal CRC/resync
  treatment below, which is what actually catches it.
"""

from __future__ import annotations

import csv
import enum
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Union

from . import kilnlink_codec
from .protocol import (
    CRC_LEN,
    FRAME_DELIM,
    FRAME_ESC,
    FRAME_ESC_XOR,
    HEADER_LEN,
    UART_PROTO_MAX_PAYLOAD,
    UART_TASK_ID_LOG,
    UART_TASK_ID_SAFETY,
    LogLevel,
    MsgType,
    crc16_ccitt_false,
)

__all__ = [
    "LinkDevice",
    "UART_TASK_ID_SAFETY",
    "UART_TASK_ID_LOG",
    "CMD_NAMES",
    "CapturedFrame",
    "load_bytes",
    "load_saleae_csv",
    "decode_capture",
    "format_timeline",
]


# ---------------------------------------------------------------------------
# Device/task ids as they appear on THIS link's wire header.
#
# uart_proto_device_t (firmware/hwAbstraction/esp/uart/uart_protocol.h) has a
# third member the PC<->ESP mirror (kilnctrl.protocol.Device) deliberately
# does not: SAFETY = 2. That omission is intentional over there (the RP2040
# is reached as a *task* through the ESP on the PC link, never addressed as
# a Device -- see test_link_hub_routing.py's own comment), but on the
# isolated link itself SAFETY really is a wire-level device id, so this
# module defines its own, separate, three-member enum rather than extending
# protocol.Device and risking that other module's deliberate omission.
# ---------------------------------------------------------------------------


class LinkDevice(enum.IntEnum):
    ESP = 0
    HOST = 1
    SAFETY = 2


# UART_TASK_ID_LOG / UART_TASK_ID_SAFETY are re-exported from
# kilnctrl.protocol above rather than restated here: that module is the
# maintained Python mirror of uart_task_ids.h, so a value bumped there can
# never leave this decoder reading a stale copy.

#: Frame ids (LINK_PROTOCOL.md secs 4/6), name only -- for the readable
#: timeline. Not every id here has a structured decoder below; anything
#: without one still gets a name and a raw hex payload dump.
CMD_NAMES: dict[int, str] = {
    0x01: "GET_STATUS / STATUS (Frame A)",
    0x02: "REQUEST_ENABLE",
    0x07: "PUSH_CONTEXT",
    0x08: "DIAG (Frame B)",
    0x09: "SET_FIRING_CEILING",
    0x0A: "CLEAR_TRIP",
    0x0B: "GET_FW_VERSION / FW_VERSION (Frame C)",
    0x0C: "SET_CLOCK",
    0x0D: "TRIP_EVENT (Frame D)",
    0x0E: "POWER (Frame E)",
    0x0F: "ANNOUNCE_VERSION",
    0x16: "SET_CONFIG",
    0x17: "ROLLBACK",
    0x18: "ANNOUNCE_REBOOT",
    0x19: "SET_CT_CAL",
    0x1A: "CT_CAL / GET_CT_CAL (Frame G)",
    0x1B: "SET_LOG_LEVEL",
    0x1C: "SET_PARAM",
    0x1D: "COMMIT_CONFIG",
    0x1E: "PARAM",
    0x1F: "CONFIG_PAGE",
    0x20: "COMMIT_CONFIG_REJECTED",
    0x21: "INJECT_TC",
    0x22: "GET_CT_CAL",
    0x23: "GET_PARAM",
    0x24: "GET_CONFIG_PAGE",
    0x25: "ROLLBACK_RESULT",
    0x26: "CT_AUTO_ZERO_BEGIN",
    0x27: "GET_CT_AUTO_ZERO",
    0x28: "CT_AUTO_ZERO_STATUS (Frame H)",
    0x29: "REBOOT",
    0x2A: "REBOOT_RESULT",
    0x2B: "GET_STACK_MARGIN",
    0x2C: "STACK_MARGIN",
    0x2D: "APPLY_CONFIG_VOLATILE",
}
# Every KILNLINK_*_CMD id defined in firmware/CommonFW/include/kilnlink/ must
# appear above; tests/test_kilnlink_capture.py parses those headers and fails
# if one is missing, so a new firmware command cannot silently decode as
# UNKNOWN(0x..) here. Ids without a structured decoder below still get their
# name plus a raw hex payload dump, which is the deliberate fallback.


# ---------------------------------------------------------------------------
# Payload decoders, keyed by cmd byte. Each takes the full payload (including
# its own leading cmd byte, matching kilnlink_codec.py's encode_* convention)
# and returns a plain dict. Raises ValueError on a bad length; callers catch
# that and report it as a decode note rather than a hard failure -- an
# unrecognised-length payload for a KNOWN cmd id is exactly the kind of thing
# this tool exists to surface, not hide.
# ---------------------------------------------------------------------------


def _decode_status_frame_a(payload: bytes) -> dict:
    """LINK_PROTOCOL.md sec 6 Frame A -- Pico -> ESP, 23/24/26 bytes.

    NOT the same shape as devices_safety.py's GET_STATUS decoder -- that one
    decodes the *PC-facing* relay reply (25/27 bytes, with an ESP-appended
    age_ms field the raw isolated-link frame never carries).
    """
    if len(payload) not in (23, 24, 26):
        raise ValueError(f"Frame A must be 23, 24 or 26 bytes, got {len(payload)}")
    flags, tc_c, cj_c, tc_fault, i1, i2, i3 = struct.unpack_from("<BffBfff", payload, 1)
    out = {
        "flags": flags,
        "safety_tc_c": tc_c,
        "cold_junction_c": cj_c,
        "tc_fault": tc_fault,
        "current1_a": i1,
        "current2_a": i2,
        "current3_a": i3,
    }
    if len(payload) >= 24:
        (out["tx_dropped_sat"],) = struct.unpack_from("<B", payload, 23)
    if len(payload) == 26:
        flags2, borrowed_zone_index = struct.unpack_from("<BB", payload, 24)
        out["flags2"] = flags2
        out["borrowed"] = bool(flags2 & 0x01)
        out["borrowed_zone_index"] = borrowed_zone_index
    return out


def _decode_request_enable(payload: bytes) -> dict:
    if len(payload) != 2:
        raise ValueError(f"REQUEST_ENABLE must be 2 bytes, got {len(payload)}")
    return {"enable": bool(payload[1])}


def _decode_context(payload: bytes) -> dict:
    """LINK_PROTOCOL.md sec 4 PUSH_CONTEXT -- mirrors kilnlink_codec.encode_context."""
    if len(payload) < 15:
        raise ValueError(f"PUSH_CONTEXT header needs >=15 bytes, got {len(payload)}")
    flags, boot_id, seq, uptime_ms, relay_now_mask, relay_recent_mask, recent_window_s, zone_count = (
        struct.unpack_from("<BBIIBBBB", payload, 1)
    )
    out = {
        "flags": flags,
        "boot_id": boot_id,
        "seq": seq,
        "uptime_ms": uptime_ms,
        "relay_now_mask": relay_now_mask,
        "relay_recent_mask": relay_recent_mask,
        "recent_window_s": recent_window_s,
        "zone_count": zone_count,
        "zones": [],
    }
    expected_len = 15 + zone_count * 14
    if len(payload) != expected_len:
        raise ValueError(
            f"PUSH_CONTEXT with zone_count={zone_count} must be {expected_len} bytes, "
            f"got {len(payload)}"
        )
    for i in range(zone_count):
        base = 15 + i * 14
        zone_index, zflags, setpoint_c, measured_c, sample_counter, tc_type, tc_fault, _reserved = (
            struct.unpack_from("<BBffBBBB", payload, base)
        )
        out["zones"].append(
            {
                "zone_index": zone_index,
                "flags": zflags,
                "setpoint_c": setpoint_c,
                "measured_c": measured_c,
                "sample_counter": sample_counter,
                "tc_type": tc_type,
                "tc_fault": tc_fault,
            }
        )
    return out


def _decode_diag(payload: bytes) -> dict:
    """LINK_PROTOCOL.md sec 6 Frame B -- mirrors kilnlink_codec.encode_diag."""
    if len(payload) != 26:
        raise ValueError(f"DIAG (Frame B) must be 26 bytes, got {len(payload)}")
    (
        trip_reason,
        warn_mask,
        trip_mask,
        uptime_ms,
        boot_reason,
        context_age_100ms,
        context_frames_ok,
        context_frames_bad,
        tx_frames_dropped,
        state,
        flags,
    ) = struct.unpack_from("<BHHIBBIIIBB", payload, 1)
    return {
        "trip_reason": trip_reason,
        "warn_mask": warn_mask,
        "trip_mask": trip_mask,
        "uptime_ms": uptime_ms,
        "boot_reason": boot_reason,
        "context_age_100ms": context_age_100ms,
        "context_frames_ok": context_frames_ok,
        "context_frames_bad": context_frames_bad,
        "tx_frames_dropped": tx_frames_dropped,
        "state": state,
        "flags": flags,
    }


def _decode_trip(payload: bytes) -> dict:
    """LINK_PROTOCOL.md sec 6 Frame D -- mirrors kilnlink_codec.encode_trip."""
    if len(payload) != 29:
        raise ValueError(f"TRIP_EVENT (Frame D) must be 29 bytes, got {len(payload)}")
    trip_seq, trip_reason, uptime_ms, safety_tc_c, deciding_threshold = struct.unpack_from(
        "<BBIff", payload, 1
    )
    i1, i2, i3 = struct.unpack_from("<fff", payload, 15)
    relay_recent_mask, context_age_100ms = struct.unpack_from("<BB", payload, 27)
    return {
        "trip_seq": trip_seq,
        "trip_reason": trip_reason,
        "uptime_ms": uptime_ms,
        "safety_tc_c": safety_tc_c,
        "deciding_threshold": deciding_threshold,
        "current_a": [i1, i2, i3],
        "relay_recent_mask": relay_recent_mask,
        "context_age_100ms": context_age_100ms,
    }


def _decode_power(payload: bytes) -> dict:
    return kilnlink_codec.decode_power(payload)


def _decode_ceiling(payload: bytes) -> dict:
    if len(payload) != 5:
        raise ValueError(f"SET_FIRING_CEILING must be 5 bytes, got {len(payload)}")
    (firing_max_c,) = struct.unpack_from("<f", payload, 1)
    return {"firing_max_c": firing_max_c}


def _decode_clear_trip(payload: bytes) -> dict:
    if len(payload) != 3:
        raise ValueError(f"CLEAR_TRIP must be 3 bytes, got {len(payload)}")
    (trip_mask,) = struct.unpack_from("<H", payload, 1)
    return {"trip_mask": trip_mask}


def _decode_set_clock(payload: bytes) -> dict:
    if len(payload) != 9:
        raise ValueError(f"SET_CLOCK must be 9 bytes, got {len(payload)}")
    (epoch_ms,) = struct.unpack_from("<Q", payload, 1)
    return {"epoch_ms": epoch_ms}


def _decode_announce_version_or_fw_version(payload: bytes) -> dict:
    """cmd 0x0F ANNOUNCE_VERSION (ESP->Pico) and cmd 0x0B's Frame C reply
    (Pico->ESP, when longer than the bare 1-byte GET_FW_VERSION request)
    share the same prefix layout -- see LINK_PROTOCOL.md's own note that
    Frame C "deliberately mirrors" ANNOUNCE_VERSION. Frame C additionally
    carries two trailing fields this shared parser reports as None when
    decoding a plain ANNOUNCE_VERSION (cmd 0x0F never has them).
    """
    if len(payload) < 6:
        raise ValueError(f"ANNOUNCE_VERSION/FW_VERSION needs >=6 bytes, got {len(payload)}")
    protocol_version, min_compatible, dirty, commit_len = struct.unpack_from("<HHBB", payload, 1)
    off = 7
    if len(payload) < off + commit_len + 1:
        raise ValueError("truncated before datetime_len")
    commit = payload[off : off + commit_len].decode("ascii", errors="replace")
    off += commit_len
    (datetime_len,) = struct.unpack_from("<B", payload, off)
    off += 1
    if len(payload) < off + datetime_len + 1:
        raise ValueError("truncated before boot_id")
    datetime = payload[off : off + datetime_len].decode("ascii", errors="replace")
    off += datetime_len
    (boot_id,) = struct.unpack_from("<B", payload, off)
    off += 1
    out = {
        "protocol_version": protocol_version,
        "min_compatible": min_compatible,
        "dirty": bool(dirty),
        "commit": commit,
        "datetime": datetime,
        "boot_id": boot_id,
        "config_version": None,
        "config_crc": None,
    }
    if payload[0] == 0x0B and len(payload) >= off + 3:
        config_version, config_crc = struct.unpack_from("<BH", payload, off)
        out["config_version"] = config_version
        out["config_crc"] = config_crc
        off += 3
    if off != len(payload):
        raise ValueError(f"{len(payload) - off} trailing byte(s) not consumed")
    return out


def _decode_set_config(payload: bytes) -> dict:
    if len(payload) != 2:
        raise ValueError(f"SET_CONFIG must be 2 bytes, got {len(payload)}")
    return {"tc_type": payload[1]}


def _decode_set_ct_cal(payload: bytes) -> dict:
    if len(payload) != 11:
        raise ValueError(f"SET_CT_CAL must be 11 bytes, got {len(payload)}")
    channel, calibrated, gain, offset = struct.unpack_from("<BBff", payload, 1)
    return {"channel": channel, "calibrated": bool(calibrated), "gain": gain, "offset": offset}


def _decode_ct_cal_reply(payload: bytes) -> dict:
    """Frame G, cmd 0x1A, 28 bytes: 3 channels x (calibrated u8, gain f32, offset f32)."""
    if len(payload) != 28:
        raise ValueError(f"CT_CAL (Frame G) must be 28 bytes, got {len(payload)}")
    channels = []
    for ch in range(3):
        calibrated, gain, offset = struct.unpack_from("<Bff", payload, 1 + ch * 9)
        channels.append({"calibrated": bool(calibrated), "gain": gain, "offset": offset})
    return {"channels": channels}


def _decode_ct_auto_zero_begin(payload: bytes) -> dict:
    if len(payload) != 2:
        raise ValueError(f"CT_AUTO_ZERO_BEGIN must be 2 bytes, got {len(payload)}")
    return {"channel": payload[1]}


def _decode_ct_auto_zero_status(payload: bytes) -> dict:
    """Frame H, cmd 0x28, fixed 9 bytes."""
    if len(payload) != 9:
        raise ValueError(f"CT_AUTO_ZERO_STATUS (Frame H) must be 9 bytes, got {len(payload)}")
    state, channel, samples_taken, samples_target, zero_counts = struct.unpack_from(
        "<BBHHH", payload, 1
    )
    return {
        "state": state,
        "channel": channel,
        "samples_taken": samples_taken,
        "samples_target": samples_target,
        "zero_counts": zero_counts,
    }


def _decode_inject_tc(payload: bytes) -> dict:
    if len(payload) != 11:
        raise ValueError(f"INJECT_TC must be 11 bytes, got {len(payload)}")
    # fault_bits is uint8_t in kilnlink_inject_tc.h (SAFETY_THERMO_FAULT_*
    # bits) -- unpack unsigned, or a mask with bit 7 set reads as a negative
    # number in the timeline.
    valid, tc_c, cj_c, fault_bits = struct.unpack_from("<BffB", payload, 1)
    return {"valid": bool(valid), "tc_c": tc_c, "cj_c": cj_c, "fault_bits": fault_bits}


def _decode_accepted_reason(payload: bytes, name: str) -> dict:
    """ROLLBACK_RESULT (0x25) / REBOOT_RESULT (0x2A): cmd, accepted, reason -- 3 bytes."""
    if len(payload) != 3:
        raise ValueError(f"{name} must be 3 bytes, got {len(payload)}")
    return {"accepted": bool(payload[1]), "reason": payload[2]}


def _decode_bare(payload: bytes, name: str) -> dict:
    """A request-only frame with no fields beyond its own cmd byte."""
    if len(payload) != 1:
        raise ValueError(f"{name} must be exactly 1 byte, got {len(payload)}")
    return {}


_CMD_DECODERS = {
    0x01: _decode_status_frame_a,
    0x02: _decode_request_enable,
    0x07: _decode_context,
    0x08: _decode_diag,
    0x09: _decode_ceiling,
    0x0A: _decode_clear_trip,
    0x0B: _decode_announce_version_or_fw_version,  # branches on length
    0x0C: _decode_set_clock,
    0x0D: _decode_trip,
    0x0E: _decode_power,
    0x0F: _decode_announce_version_or_fw_version,
    0x16: _decode_set_config,
    0x17: lambda p: _decode_bare(p, "ROLLBACK"),
    0x18: lambda p: _decode_bare(p, "ANNOUNCE_REBOOT"),
    0x19: _decode_set_ct_cal,
    0x1A: _decode_ct_cal_reply,
    0x21: _decode_inject_tc,
    0x22: lambda p: _decode_bare(p, "GET_CT_CAL"),
    0x25: lambda p: _decode_accepted_reason(p, "ROLLBACK_RESULT"),
    0x26: _decode_ct_auto_zero_begin,
    0x27: lambda p: _decode_bare(p, "GET_CT_AUTO_ZERO"),
    0x28: _decode_ct_auto_zero_status,
    0x29: lambda p: _decode_bare(p, "REBOOT"),
    0x2A: lambda p: _decode_accepted_reason(p, "REBOOT_RESULT"),
}
# 0x0B is special-cased below (the bare 1-byte GET_FW_VERSION request shares
# its id with the much longer Frame C reply -- LINK_PROTOCOL.md's own
# "request/reply ids must never be shared" rule post-dates this one and
# explicitly leaves it alone, see that section).


def _decode_log(payload: bytes) -> dict:
    if len(payload) < 1:
        raise ValueError("LOG payload is empty")
    level = payload[0]
    try:
        level_name = LogLevel(level).name
    except ValueError:
        level_name = f"0x{level:02X}"
    message = payload[1:].decode("ascii", errors="replace")
    return {"level": level, "level_name": level_name, "message": message}


def decode_payload(src_task: int, dst_task: int, payload: bytes) -> "tuple[Optional[str], Optional[dict], Optional[str]]":
    """Returns ``(cmd_name, decoded_fields, error)``.

    ``cmd_name`` is None only when the payload is empty (nothing to name).
    ``error`` is set (and ``decoded_fields`` is None) when the payload has a
    recognised cmd id but a decoder for it raised -- e.g. an unexpected
    length. An unrecognised cmd id is not an error: ``decoded_fields`` comes
    back as ``{"raw_hex": ...}`` instead, same as a recognised id with a
    length this module has no per-command decoder for.
    """
    if UART_TASK_ID_LOG in (src_task, dst_task):
        try:
            return "LOG", _decode_log(payload), None
        except (ValueError, IndexError) as exc:
            return "LOG", None, str(exc)

    if not payload:
        return None, {}, None

    cmd = payload[0]
    name = CMD_NAMES.get(cmd, f"UNKNOWN(0x{cmd:02X})")

    if cmd == 0x0B and len(payload) == 1:
        return "GET_FW_VERSION (request)", {}, None
    # KILNLINK_GET_CT_CAL_CMD == KILNLINK_CT_CAL_CMD == 0x1A (uart_task_ids.h
    # line 1025): the 1-byte request and the 28-byte reply share one id, same
    # as 0x0B above.
    if cmd == 0x1A and len(payload) == 1:
        return "GET_CT_CAL (request)", {}, None

    decoder = _CMD_DECODERS.get(cmd)
    if decoder is None:
        return name, {"raw_hex": payload.hex()}, None
    try:
        return name, decoder(payload), None
    except (struct.error, ValueError, IndexError) as exc:
        return name, None, str(exc)


# ---------------------------------------------------------------------------
# Framing / resync
# ---------------------------------------------------------------------------


@dataclass
class CapturedFrame:
    """One record in the decoded timeline -- either a clean frame or a
    diagnosable failure to produce one, always anchored to a byte offset in
    the ORIGINAL (stuffed) capture stream so a human can go find the bytes.
    """

    offset: int
    end_offset: int
    ok: bool
    error: Optional[str] = None
    msg_type: Optional[int] = None
    msg_type_name: Optional[str] = None
    msg_index: Optional[int] = None
    src_device: Optional[int] = None
    dst_device: Optional[int] = None
    src_task: Optional[int] = None
    dst_task: Optional[int] = None
    length_declared: Optional[int] = None
    payload: Optional[bytes] = None
    crc_expected: Optional[int] = None
    crc_actual: Optional[int] = None
    cmd_name: Optional[str] = None
    decoded: Optional[dict] = None
    raw_stuffed: bytes = b""

    def _device_name(self, value: Optional[int]) -> str:
        if value is None:
            return "?"
        try:
            return LinkDevice(value).name
        except ValueError:
            return f"0x{value:02X}"

    def summary(self) -> str:
        if not self.ok:
            return f"offset {self.offset}: MALFORMED -- {self.error} (raw={self.raw_stuffed.hex()})"
        src = f"{self._device_name(self.src_device)}/{self.src_task}"
        dst = f"{self._device_name(self.dst_device)}/{self.dst_task}"
        head = (
            f"offset {self.offset}: {self.msg_type_name} idx={self.msg_index} "
            f"{src} -> {dst} len={self.length_declared} cmd={self.cmd_name}"
        )
        if self.decoded is None:
            return head + " [decode error above]"
        if not self.decoded:
            return head
        return head + " " + " ".join(f"{k}={v}" for k, v in self.decoded.items())


def _unstuff(body: bytes) -> bytes:
    """Reverse SLIP-style byte-stuffing for one delimiter-bounded span.

    Raises ValueError on a trailing, unterminated escape byte -- mirrors
    KILNLINK_FRAME_ERR_UNTERMINATED_ESC.
    """
    out = bytearray()
    escaped = False
    for b in body:
        if escaped:
            out.append(b ^ FRAME_ESC_XOR)
            escaped = False
        elif b == FRAME_ESC:
            escaped = True
        else:
            out.append(b)
    if escaped:
        raise ValueError("unterminated escape byte (0x7D) at end of frame")
    return bytes(out)


def _decode_raw_header_and_body(raw: bytes) -> CapturedFrame:
    """Parse one unstuffed, delimiter-stripped span into a CapturedFrame
    (offset/end_offset left for the caller to fill in). Extracts as much of
    the header as it safely can even when the frame is later judged
    malformed, so a resync report still names what device/task looked like
    they were talking, not just "corrupt"."""
    cf = CapturedFrame(offset=0, end_offset=0, ok=False, raw_stuffed=raw)
    if len(raw) < HEADER_LEN:
        cf.error = f"too short for an 8-byte header: {len(raw)} byte(s)"
        return cf

    msg_type_byte, hi, lo, src_device, src_task, dst_device, dst_task, length = raw[0:8]
    cf.msg_index = (hi << 8) | lo
    cf.src_device = src_device
    cf.src_task = src_task
    cf.dst_device = dst_device
    cf.dst_task = dst_task
    cf.length_declared = length

    if length > UART_PROTO_MAX_PAYLOAD:
        cf.error = f"header LENGTH byte {length} exceeds max payload {UART_PROTO_MAX_PAYLOAD}"
        return cf

    want = HEADER_LEN + length + CRC_LEN
    if len(raw) < want:
        cf.error = (
            f"frame too short for its own declared length: header says "
            f"{length}-byte payload ({want} bytes total incl. CRC), got only "
            f"{len(raw)} (truncated/partial frame -- likely capture cut off "
            f"mid-frame, or a dropped delimiter merged it with the next one)"
        )
        return cf
    if len(raw) > want:
        cf.error = (
            f"frame length mismatch: header says {length}-byte payload "
            f"({want} bytes total incl. CRC) but this span is {len(raw)} bytes "
            f"(extra bytes -- likely a missing delimiter merged two frames)"
        )
        return cf

    payload = raw[HEADER_LEN : HEADER_LEN + length]
    expected = crc16_ccitt_false(raw[: HEADER_LEN + length])
    actual = (raw[HEADER_LEN + length] << 8) | raw[HEADER_LEN + length + 1]
    cf.crc_expected = expected
    cf.crc_actual = actual
    cf.payload = payload
    if expected != actual:
        cf.error = f"CRC mismatch: expected 0x{expected:04X}, got 0x{actual:04X}"
        return cf

    cf.msg_type = msg_type_byte
    try:
        cf.msg_type_name = MsgType(msg_type_byte).name
    except ValueError:
        cf.error = f"unknown msg type byte 0x{msg_type_byte:02X}"
        return cf

    cf.cmd_name, cf.decoded, decode_err = decode_payload(src_task, dst_task, payload)
    if decode_err is not None:
        cf.error = f"payload decode error for cmd {cf.cmd_name}: {decode_err}"
        return cf

    cf.ok = True
    return cf


def _iter_spans(data: bytes):
    """Yield ``(start_offset, end_offset, body, kind)`` for every span this
    stream implies, where ``kind`` is one of:

    * ``"garbage"`` -- bytes seen before the first delimiter (the capture
      started mid-frame, or the analyzer picked up line noise).
    * ``"tail"`` -- bytes after the last delimiter with no closing delimiter:
      a frame that was still arriving when the capture stopped. Reported as
      TRUNCATED specifically, never merged into "garbage" -- "the capture
      ended mid-frame" and "these bytes were never part of a frame" are
      different diagnoses.
    * ``"empty"`` -- a zero-length span between two adjacent delimiters
      (back-to-back DELIMs are just noise, per kilnlink_frame.c / the
      firmware's own FrameDecoder -- not reported as an error).
    * ``"frame"`` -- a normal delimiter-bounded span, ready to unstuff.

    Offsets are always into ``data`` as given (the ORIGINAL, stuffed
    capture), so a caller can point a human at the exact bytes.
    """
    delims = [i for i, b in enumerate(data) if b == FRAME_DELIM]
    if not delims:
        if data:
            yield (0, len(data), data, "garbage")
        return

    if delims[0] > 0:
        yield (0, delims[0], data[: delims[0]], "garbage")

    for a, b in zip(delims, delims[1:]):
        body = data[a + 1 : b]
        yield (a + 1, b, body, "empty" if not body else "frame")

    last = delims[-1]
    if last + 1 < len(data):
        yield (last + 1, len(data), data[last + 1 :], "tail")


def decode_capture(data: bytes) -> list[CapturedFrame]:
    """Decode a raw (already-loaded) byte capture into a frame timeline.

    Resynchronises after garbage exactly the way the firmware's own receiver
    does (unconditional resync on 0x7E, from any state -- LINK_PROTOCOL.md
    "Receiver robustness"): a span of non-frame bytes is reported as its own
    malformed record and decoding continues with the next delimiter-bounded
    span, rather than aborting the whole capture.
    """
    records: list[CapturedFrame] = []
    for start, end, body, kind in _iter_spans(data):
        if kind == "empty":
            continue
        if kind in ("garbage", "tail"):
            if kind == "tail":
                why = (
                    f"TRUNCATED: capture ends mid-frame -- {end - start} byte(s) "
                    f"follow the last delimiter with no closing delimiter"
                )
            else:
                why = (
                    f"{end - start} byte(s) of non-frame data before the first "
                    f"delimiter (no enclosing delimiter)"
                )
            records.append(
                CapturedFrame(offset=start, end_offset=end, ok=False, error=why, raw_stuffed=body)
            )
            continue
        try:
            raw = _unstuff(body)
        except ValueError as exc:
            records.append(
                CapturedFrame(offset=start, end_offset=end, ok=False, error=str(exc), raw_stuffed=body)
            )
            continue
        cf = _decode_raw_header_and_body(raw)
        cf.offset = start
        cf.end_offset = end
        cf.raw_stuffed = body
        records.append(cf)
    return records


# ---------------------------------------------------------------------------
# Loaders
# ---------------------------------------------------------------------------


def load_bytes(path: Union[str, Path]) -> bytes:
    """Load a capture from a raw binary file (the concatenated wire bytes,
    exactly as they would be read off a UART -- e.g. a fixture built with
    ``kilnctrl.protocol.Frame.to_wire()``)."""
    return Path(path).read_bytes()


def load_saleae_csv(path: Union[str, Path]) -> bytes:
    """Load a capture from a Saleae Logic 2 Async Serial analyzer's
    "Export Table Data" CSV. Expects a ``Value`` column holding one
    received byte per row, as ``0xHH`` hex or a plain decimal integer.
    Rows are taken in file order, which is capture time order.
    """
    out = bytearray()
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None or "Value" not in reader.fieldnames:
            raise ValueError(
                f"{path}: expected a 'Value' column (Saleae Async Serial "
                f"'Export Table Data' CSV), got columns {reader.fieldnames}"
            )
        for row in reader:
            raw = (row.get("Value") or "").strip()
            if not raw:
                continue
            out.append(int(raw, 16) if raw.lower().startswith("0x") else int(raw, 10))
    return bytes(out)


def format_timeline(records: list[CapturedFrame]) -> str:
    """Render a decoded capture as a readable, line-per-frame timeline."""
    if not records:
        return "(empty capture -- no bytes decoded)"
    lines = [r.summary() for r in records]
    ok = sum(1 for r in records if r.ok)
    lines.append(f"-- {ok}/{len(records)} frame(s) decoded cleanly --")
    return "\n".join(lines)
