"""Pure-Python mirror of firmware/CommonFW's kilnlink *payload* codecs
(the safety-link frames carried inside a kilnlink_frame_t.payload -- see
kilnctrl.protocol for pc_tools' existing, already-proven mirror of the
*framing* layer, kilnlink_frame.c/kilnlink_crc.c).

pc_tools is the third implementation of this wire protocol
(firmware/CommonFW/README.md), the same role kilnctrl.protocol already plays
for the framing layer. This module exists so selfcheck.py can prove Python
produces byte-identical output to the C encoders for every payload codec
that has host tests and test/vectors/ in CommonFW, not just the framing
layer -- see commonfw_payload_vector_checks() in selfcheck.py.

Deliberately minimal, and mostly encode-only: each encoder takes the same
field dict shape the corresponding test/vectors/<name>_vectors.json "fields"
(or, for the older-style manifests, top-level vector) uses, and returns the
exact wire bytes -- struct.pack calls whose field order and offsets are
copied 1:1 from the matching firmware/CommonFW/src/kilnlink_<name>.c "Offsets"
comment. No decode side exists for most of these frames (they are not wired
into either firmware's real dispatch either -- ROADMAP.md M2), so there is
nothing here for a decoder to feed; add one only when a real caller needs
it, mirroring firmware/CommonFW/include/kilnlink/kilnlink_<name>.h
field-for-field the way the encoders already do. decode_power() is the one
exception (Opus review of 51c084f/c49bb0e, finding 7) -- added purely so
encode_power()'s new V1/V2 handling has a decoder to round-trip against in
test_kilnlink_codec.py; nothing in pc_tools calls it on live traffic yet
(safety_capture_ct_counts()/mcp_server_safety.py reads counts_avg via GET
/api/status's JSON `ct_counts` field, not by parsing a raw kilnlink frame).
"""

from __future__ import annotations

import struct

__all__ = [
    "encode_context",
    "encode_status",
    "encode_announce",
    "encode_diag",
    "encode_trip",
    "encode_power",
    "decode_power",
    "encode_ceiling",
    "encode_clear_trip",
    "encode_get_fw_version",
    "encode_set_clock",
    "encode_get_param",
    "decode_param",
]


def _num(v):
    """test/vectors/*.json spells float NaN as the string "NaN" (JSON has no
    NaN literal). Everything else (a real number) passes through unchanged."""
    if isinstance(v, str) and v.strip().upper() == "NAN":
        return float("nan")
    return v


def _nums(vs):
    return [_num(v) for v in vs]


def _resolve_enum(value, mapping: dict[str, int]) -> int:
    """diag_vectors.json spells enum/flag fields as symbolic names (optionally
    OR'd with " | "), e.g. "KILNLINK_DIAG_STATE_ARMED" or
    "KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN | KILNLINK_DIAG_FLAG_CALIBRATION_MISSING".
    A plain int also passes straight through, for vector files that use raw
    numbers instead (context/status/power/trip/ceiling/clear_trip/set_clock)."""
    if isinstance(value, int):
        return value
    result = 0
    for name in value.split("|"):
        result |= mapping[name.strip()]
    return result


# -- SAFETY_CMD_PUSH_CONTEXT = 0x07 (ESP -> Pico) ----------------------------
# kilnlink_context.c: cmd(1) flags(1) boot_id(1) seq(u32) uptime_ms(u32)
# relay_now_mask(1) relay_recent_mask(1) recent_window_s(1) zone_count(1) =
# 15 fixed bytes, then zone_count * 14-byte zone blocks (zone_index(1)
# flags(1) setpoint_c(f32) measured_c(f32) sample_counter(1) tc_type(1)
# tc_fault(1) reserved(1)=0).

def encode_context(f: dict) -> bytes:
    out = bytearray(
        struct.pack(
            "<BBBIIBBBB",
            0x07,
            f["flags"],
            f["boot_id"],
            f["seq"],
            f["uptime_ms"],
            f["relay_now_mask"],
            f["relay_recent_mask"],
            f["recent_window_s"],
            f["zone_count"],
        )
    )
    for z in f["zones"]:
        out += struct.pack(
            "<BBffBBBB",
            z["zone_index"],
            z["flags"],
            _num(z["setpoint_c"]),
            _num(z["measured_c"]),
            z["sample_counter"],
            z["tc_type"],
            z["tc_fault"],
            0,
        )
    return bytes(out)


# -- SAFETY_CMD_GET_STATUS = 0x01 (Pico -> ESP, Frame A) ---------------------
# kilnlink_status.c: cmd(1) flags(1) safety_tc_c(f32) cold_junction_c(f32)
# tc_fault(1) current1_a(f32) current2_a(f32) current3_a(f32) = 23 bytes.

def encode_status(f: dict) -> bytes:
    return struct.pack(
        "<BBffBfff",
        0x01,
        f["flags"],
        _num(f["safety_tc_c"]),
        _num(f["cold_junction_c"]),
        f["tc_fault"],
        _num(f["current1_a"]),
        _num(f["current2_a"]),
        _num(f["current3_a"]),
    )


# -- SAFETY_CMD_ANNOUNCE_VERSION = 0x0F (ESP -> Pico) ------------------------
# kilnlink_announce.c: cmd(1) protocol_version(u16) min_compatible(u16)
# dirty(1) commit_len(1) commit(N1) datetime_len(1) datetime(N2) boot_id(1).

def encode_announce(f: dict) -> bytes:
    commit = f["commit"].encode("ascii")
    datetime = f["datetime"].encode("ascii")
    out = bytearray(
        struct.pack(
            "<BHHBB",
            0x0F,
            f["protocol_version"],
            f["min_compatible"],
            f["dirty"],
            len(commit),
        )
    )
    out += commit
    out += struct.pack("<B", len(datetime))
    out += datetime
    out += struct.pack("<B", f["boot_id"])
    return bytes(out)


# -- SAFETY_CMD_DIAG = 0x08 (Pico -> ESP, Frame B) ---------------------------
# kilnlink_diag.c: cmd(1) trip_reason(1) warn_mask(u16) trip_mask(u16)
# uptime_ms(u32) boot_reason(1) context_age_100ms(1) context_frames_ok(u32)
# context_frames_bad(u32) tx_frames_dropped(u32) state(1) flags(1)
# log_frames_dropped(u32) = 30 bytes. (KILNLINK_PROTOCOL_VERSION 15 -> 16)

_DIAG_BOOT = {
    "KILNLINK_DIAG_BOOT_POWERON": 0x01,
    "KILNLINK_DIAG_BOOT_WATCHDOG": 0x02,
    "KILNLINK_DIAG_BOOT_BROWNOUT": 0x04,
}
_DIAG_STATE = {
    "KILNLINK_DIAG_STATE_INIT": 0,
    "KILNLINK_DIAG_STATE_GRACE": 1,
    "KILNLINK_DIAG_STATE_ARMED": 2,
    "KILNLINK_DIAG_STATE_WARN": 3,
    "KILNLINK_DIAG_STATE_TRIPPED": 4,
}
_DIAG_FLAG = {
    "KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN": 0x01,
    "KILNLINK_DIAG_FLAG_CALIBRATION_MISSING": 0x02,
    "KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT": 0x04,
    # Backfilled 2026-09-22: bits 3/4 were already live on the wire
    # (kilnlink_diag.h) but missing from this table -- pre-existing gap,
    # found while adding the two bits below for S1/S8 shipping
    # disabled-by-zero. Bits 5/6 are new in this change.
    "KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT": 0x08,
    "KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP": 0x10,
    "KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED": 0x20,
    "KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED": 0x40,
    # 2026-09-23: bit7, the last spare bit in this byte -- the next diag
    # flag needs a new byte on the wire, not just a table entry here.
    "KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY": 0x80,
}


def encode_diag(f: dict) -> bytes:
    return struct.pack(
        "<BBHHIBBIIIBBI",
        0x08,
        f["trip_reason"],
        f["warn_mask"],
        f["trip_mask"],
        f["uptime_ms"],
        _resolve_enum(f["boot_reason"], _DIAG_BOOT),
        f["context_age_100ms"],
        f["context_frames_ok"],
        f["context_frames_bad"],
        f["tx_frames_dropped"],
        _resolve_enum(f["state"], _DIAG_STATE),
        _resolve_enum(f["flags"], _DIAG_FLAG),
        f["log_frames_dropped"],
    )


# -- SAFETY_CMD_TRIP_EVENT = 0x0D (Pico -> ESP, Frame D) ---------------------
# kilnlink_trip.c: cmd(1) trip_seq(1) trip_reason(1) uptime_ms(u32)
# safety_tc_c(f32) deciding_threshold(f32) current_a[3](f32 x3)
# relay_recent_mask(1) context_age_100ms(1) = 29 bytes.

def encode_trip(f: dict) -> bytes:
    current_a = _nums(f["current_a"])
    return struct.pack(
        "<BBBIff",
        0x0D,
        f["trip_seq"],
        f["trip_reason"],
        f["uptime_ms"],
        _num(f["safety_tc_c"]),
        _num(f["deciding_threshold"]),
    ) + struct.pack("<fff", *current_a) + struct.pack(
        "<BB", f["relay_recent_mask"], f["context_age_100ms"]
    )


# -- SAFETY_CMD_POWER = 0x0E (Pico -> ESP, Frame E) --------------------------
# kilnlink_power.c: cmd(1) power_window_s(1) flags(1) mains_voltage_v(f32)
# 3 x (i_conducting_a(f32) conduction_fraction(f32)) 3 x p_avg_w(f32)
# p_total_w(f32) energy_wh(f64) = 55 bytes.

#: kilnlink_power.h: bit set in the flags byte whenever the frame carries a
#: real counts_avg tail (V2, 61 bytes) -- distinguishes "counts_avg is real"
#: from "counts_avg is zero-filled padding" for a peer that never learned to
#: set it (an old V1 sender, or a V2-length frame from untrusted input that
#: didn't set the bit).
KILNLINK_POWER_FLAG_COUNTS_VALID = 0x08

#: Fixed lengths, kilnlink_power.h.
KILNLINK_POWER_LEN_V1 = 55
KILNLINK_POWER_LEN_V2 = 61


def encode_power(f: dict) -> bytes:
    """Mirrors firmware/CommonFW/src/kilnlink_power.c's
    kilnlink_power_encode() -- which, as of 2026-09-06, ALWAYS emits the
    61-byte V2 layout (55-byte V1 body + 3x u16 LE counts_avg) and ALWAYS
    forces KILNLINK_POWER_FLAG_COUNTS_VALID on in the flags byte, regardless
    of what the caller passed in `f["flags"]` -- a caller never has to
    remember the bit, same as the C encoder's own doc comment says. Opus
    review of 51c084f/c49bb0e, finding 7: this function previously still
    emitted the old 55-byte V1 layout unconditionally and never set the
    flag, silently drifting from the real encoder with no vector coverage
    to catch it (power_vectors.json predates the V2 extension and only
    records the 55-byte V1 body -- see selfcheck_commonfw.py's power
    special-case for how that older vector file is still honored).

    `f["counts_avg"]` is optional (defaults to [0, 0, 0]) since the existing
    vector file has no such field; a real caller normally supplies the
    current_snapshot_t.counts_avg triple.
    """
    i_conducting = _nums(f["i_conducting_a"])
    conduction = _nums(f["conduction_fraction"])
    p_avg = _nums(f["p_avg_w"])
    flags = f["flags"] | KILNLINK_POWER_FLAG_COUNTS_VALID
    out = bytearray(
        struct.pack(
            "<BBBf",
            0x0E,
            f["power_window_s"],
            flags,
            _num(f["mains_voltage_v"]),
        )
    )
    for ch in range(3):
        out += struct.pack("<ff", i_conducting[ch], conduction[ch])
    for ch in range(3):
        out += struct.pack("<f", p_avg[ch])
    out += struct.pack("<f", _num(f["p_total_w"]))
    out += struct.pack("<d", f["energy_wh"])
    counts_avg = f.get("counts_avg", [0, 0, 0])
    for ch in range(3):
        out += struct.pack("<H", counts_avg[ch])
    return bytes(out)


def decode_power(payload: bytes) -> dict:
    """Mirrors kilnlink_power_decode(): accepts EITHER the 55-byte V1 layout
    or the 61-byte V2 layout (kilnlink_power.h's own "a receiver must accept
    any of the lengths it knows" discipline, same family as every other
    additive frame in this protocol -- LINK_PROTOCOL.md's V1/V2/V3 status
    frame precedent). Returns a plain dict with the same field names
    encode_power() takes. `counts_avg` decodes to [0, 0, 0] for a V1-length
    frame, or for a V2-length frame whose COUNTS_VALID bit is clear --
    never garbage from an unset region of the buffer, matching the C
    decoder's explicit zero-fill for exactly those two cases.

    Raises ValueError (not a specific exception type -- this module has no
    decode error taxonomy yet, unlike kilnctrl.protocol's Frame/FrameError
    for the framing layer) on a wrong command byte or a length that is
    neither V1 nor V2.
    """
    if len(payload) not in (KILNLINK_POWER_LEN_V1, KILNLINK_POWER_LEN_V2):
        raise ValueError(
            f"kilnlink power payload must be {KILNLINK_POWER_LEN_V1} or "
            f"{KILNLINK_POWER_LEN_V2} bytes, got {len(payload)}"
        )
    if payload[0] != 0x0E:
        raise ValueError(f"kilnlink power payload has wrong cmd byte: {payload[0]:#04x}, expected 0x0e")

    power_window_s, flags, mains_voltage_v = struct.unpack_from("<BBf", payload, 1)
    i_conducting_a = []
    conduction_fraction = []
    for ch in range(3):
        i_a, frac = struct.unpack_from("<ff", payload, 7 + ch * 8)
        i_conducting_a.append(i_a)
        conduction_fraction.append(frac)
    p_avg_w = list(struct.unpack_from("<fff", payload, 31))
    (p_total_w,) = struct.unpack_from("<f", payload, 43)
    (energy_wh,) = struct.unpack_from("<d", payload, 47)

    if len(payload) == KILNLINK_POWER_LEN_V2 and (flags & KILNLINK_POWER_FLAG_COUNTS_VALID):
        counts_avg = list(struct.unpack_from("<HHH", payload, 55))
    else:
        counts_avg = [0, 0, 0]

    return {
        "power_window_s": power_window_s,
        "flags": flags,
        "mains_voltage_v": mains_voltage_v,
        "i_conducting_a": i_conducting_a,
        "conduction_fraction": conduction_fraction,
        "p_avg_w": p_avg_w,
        "p_total_w": p_total_w,
        "energy_wh": energy_wh,
        "counts_avg": counts_avg,
    }


# -- SAFETY_CMD_SET_FIRING_CEILING = 0x09 (ESP -> Pico) ----------------------
# kilnlink_ceiling.c: cmd(1) firing_max_c(f32) = 5 bytes.

def encode_ceiling(f: dict) -> bytes:
    return struct.pack("<Bf", 0x09, _num(f["firing_max_c"]))


# -- SAFETY_CMD_CLEAR_TRIP = 0x0A (ESP -> Pico) ------------------------------
# kilnlink_clear_trip.c: cmd(1) trip_mask(u16) = 3 bytes.

def encode_clear_trip(f: dict) -> bytes:
    return struct.pack("<BH", 0x0A, f["trip_mask"])


# -- SAFETY_CMD_GET_FW_VERSION = 0x0B (ESP -> Pico) --------------------------
# kilnlink_get_fw_version.c: cmd(1), no fields = 1 byte.

def encode_get_fw_version(f: dict) -> bytes:
    del f  # no fields to read -- present only for a uniform call shape
    return bytes([0x0B])


# -- SAFETY_CMD_SET_CLOCK = 0x0C (ESP -> Pico), optional ---------------------
# kilnlink_set_clock.c: cmd(1) epoch_ms(u64) = 9 bytes.

def encode_set_clock(f: dict) -> bytes:
    return struct.pack("<BQ", 0x0C, f["epoch_ms"])


# -- SAFETY_CMD_GET_PARAM = 0x23 (ESP -> Pico) -------------------------------
# kilnlink_get_param.c: cmd(1) param_id(u16 LE) = 3 bytes.

def encode_get_param(f: dict) -> bytes:
    """Mirrors firmware/CommonFW/src/kilnlink_get_param.c's
    kilnlink_get_param_encode() -- cmd(1)=0x23, param_id u16 LE(2), 3 bytes
    total. `param_id` is opaque here, same as the C codec's own doc comment:
    this encoder does not know the id table, only that one is being asked
    for."""
    return struct.pack("<BH", 0x23, f["param_id"])


# -- SAFETY_CMD_PARAM = 0x1E (Pico -> ESP) -----------------------------------
# kilnlink_param.c: cmd(1) param_id(u16 LE) found(1) type(1) [value(N)] =
# 5 bytes header, +1/1/2/4 value bytes when found=1 (type 0=bool,1=u8,2=u16,
# LE; 3=f32 LE). found=0 means header-only, exactly 5 bytes, no value bytes
# at all -- type is then meaningless and firmware always sends 0 for it.

_PARAM_VALUE_FMT = {0x00: "<B", 0x01: "<B", 0x02: "<H", 0x03: "<f"}


def decode_param(payload: bytes) -> dict:
    """Mirrors kilnlink_param.c's kilnlink_param_decode(). Returns a plain
    dict: {"param_id", "found", "type", "value"} -- `value` is None and
    `type` is 0 when `found` is False, matching the C decoder leaving those
    fields untouched (never garbage from an unset region) for a "not found"
    reply.

    Raises ValueError (this module has no decode error taxonomy yet, same as
    decode_power()) on a wrong command byte, a too-short payload, a bad
    found byte, an unknown type tag, or a length that doesn't match what
    found/type imply.
    """
    if len(payload) < 5:
        raise ValueError(f"kilnlink param payload too short: {len(payload)} bytes, need >= 5")
    cmd, param_id, found, type_ = struct.unpack_from("<BHBB", payload, 0)
    if cmd != 0x1E:
        raise ValueError(f"kilnlink param payload has wrong cmd byte: {cmd:#04x}, expected 0x1e")
    if found not in (0, 1):
        raise ValueError(f"kilnlink param payload has bad found byte: {found}")
    if not found:
        if len(payload) != 5:
            raise ValueError(f"kilnlink param 'not found' payload must be 5 bytes, got {len(payload)}")
        return {"param_id": param_id, "found": 0, "type": 0, "value": None}
    value_fmt = _PARAM_VALUE_FMT.get(type_)
    if value_fmt is None:
        raise ValueError(f"kilnlink param payload has unknown type tag: {type_:#04x}")
    value_len = struct.calcsize(value_fmt)
    if len(payload) != 5 + value_len:
        raise ValueError(
            f"kilnlink param payload length mismatch: type={type_} implies {5 + value_len} bytes, got {len(payload)}"
        )
    (raw_value,) = struct.unpack_from(value_fmt, payload, 5)
    value = bool(raw_value) if type_ == 0x00 else raw_value
    return {"param_id": param_id, "found": 1, "type": type_, "value": value}
