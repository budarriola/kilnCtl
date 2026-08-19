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

Deliberately minimal and encode-only: each function takes the same field
dict shape the corresponding test/vectors/<name>_vectors.json "fields" (or,
for the three older-style manifests, top-level vector) uses, and returns the
exact wire bytes -- struct.pack calls whose field order and offsets are
copied 1:1 from the matching firmware/CommonFW/src/kilnlink_<name>.c "Offsets"
comment. No decode side: nothing in pc_tools parses these frames yet (they
are not wired into either firmware's real dispatch either -- ROADMAP.md M2),
so there is nothing here for a decoder to feed. Add one only when a real
caller needs it, mirroring firmware/CommonFW/include/kilnlink/kilnlink_<name>.h
field-for-field the way these encoders already do.
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
    "encode_ceiling",
    "encode_clear_trip",
    "encode_get_fw_version",
    "encode_set_clock",
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
# context_frames_bad(u32) tx_frames_dropped(u32) state(1) flags(1) = 26 bytes.

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
}


def encode_diag(f: dict) -> bytes:
    return struct.pack(
        "<BBHHIBBIIIBB",
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

def encode_power(f: dict) -> bytes:
    i_conducting = _nums(f["i_conducting_a"])
    conduction = _nums(f["conduction_fraction"])
    p_avg = _nums(f["p_avg_w"])
    out = bytearray(
        struct.pack(
            "<BBBf",
            0x0E,
            f["power_window_s"],
            f["flags"],
            _num(f["mains_voltage_v"]),
        )
    )
    for ch in range(3):
        out += struct.pack("<ff", i_conducting[ch], conduction[ch])
    for ch in range(3):
        out += struct.pack("<f", p_avg[ch])
    out += struct.pack("<f", _num(f["p_total_w"]))
    out += struct.pack("<d", f["energy_wh"])
    return bytes(out)


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
