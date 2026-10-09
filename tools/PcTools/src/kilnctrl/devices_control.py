"""CONTROL wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403
from .devices_common import (  # noqa: F401
    OkReason,
    _check_bool_byte,
    _check_finite,
    _check_i8,
    _check_range,
    _check_u8,
    _check_u16,
    _decode_ok_reason,
    _decoded_float,
)


# ---------------------------------------------------------------------------
# CONTROL -- zone config reads + narrow PID/model writes
# (task_id = UART_TASK_ID_CONTROL)
#
# Scope cap: /api/zones' whole-page fields (naming, relay assignment, heater
# window timing, cross-zone guard) are NOT writable here -- see
# docs/UART_PROTOCOL.md. Manual relay control is IO task's SET_RELAY/
# SET_RELAY_MASK, not duplicated here.
# ---------------------------------------------------------------------------
class ControlResponseError(ValueError):
    """Raised when a CONTROL response payload does not match its wire layout."""


@dataclass(frozen=True)
class ZoneConfig:
    """One 31-byte zone record from a GET_ZONES reply."""

    index: int
    relay_mask: int
    control_mode: int
    cal_offset_c: float
    pid_kp: float
    pid_ki: float
    pid_kd: float
    max_ramp_c_per_hr: float
    max_temp_c: float
    min_temp_c: float

    def describe(self) -> str:
        return (
            f"Zone {self.index} (relay_mask 0x{self.relay_mask:02X}, "
            f"mode {self.control_mode}): Kp={self.pid_kp:.4f} Ki={self.pid_ki:.5f} "
            f"Kd={self.pid_kd:.4f}  cal={self.cal_offset_c:+.2f}C  "
            f"ramp<={self.max_ramp_c_per_hr:.0f}C/hr  range={self.min_temp_c:.0f}.."
            f"{self.max_temp_c:.0f}C"
        )


def control_get_zones() -> bytes:
    """0x01 GET_ZONES request (query): no args."""
    return struct.pack("<B", CONTROL_CMD_GET_ZONES)


def control_set_zone_pid(zone: int, kp: float, ki: float, kd: float) -> bytes:
    """0x02 SET_ZONE_PID: zone, kp/ki/kd f32 LE. Replies ok/fail."""
    return struct.pack(
        "<BBfff",
        CONTROL_CMD_SET_ZONE_PID,
        _check_u8(zone, "zone"),
        _check_finite(kp, "kp"),
        _check_finite(ki, "ki"),
        _check_finite(kd, "kd"),
    )


def control_set_zone_model(zone: int, k_dc: float, tau_s: float, dead_time_s: float) -> bytes:
    """0x03 SET_ZONE_MODEL: zone, k_dc/tau_s/dead_time_s f32 LE. Replies ok/fail.

    An all-zero triple clears the model (the documented "no model" encoding).
    """
    return struct.pack(
        "<BBfff",
        CONTROL_CMD_SET_ZONE_MODEL,
        _check_u8(zone, "zone"),
        _check_finite(k_dc, "k_dc"),
        _check_finite(tau_s, "tau_s"),
        _check_finite(dead_time_s, "dead_time_s"),
    )


def parse_control_response(
    payload: bytes,
) -> "tuple[int, tuple[int, int, list[ZoneConfig]] | OkReason | int]":
    """Decode a CONTROL reply into ``(subcmd, value)``.

    GET_ZONES value is ``(thermo_count, relay_count, [ZoneConfig, ...])``;
    SET_ZONE_PID/SET_ZONE_MODEL/SET_UNIT_PREF value is an :class:`OkReason`
    (bx_reply_ok_err() in uart_bridge_ext.c appends a reason string on
    refusal -- e.g. an out-of-range zone index -- that used to be decoded and
    then discarded here; ``OkReason`` is still truthy/falsy like the old bare
    bool, so ``if not result:`` call sites keep working unchanged);
    GET_UNIT_PREF value is the raw unit_pref_t byte (0=Celsius, 1=Fahrenheit).
    """
    if len(payload) < 1:
        raise ControlResponseError("CONTROL response is empty")
    subcommand = payload[0]

    if subcommand == CONTROL_CMD_GET_ZONES:
        if len(payload) < 4:
            raise ControlResponseError("GET_ZONES response header is truncated")
        thermo_count, relay_count, count = payload[1], payload[2], payload[3]
        expected = 4 + count * CONTROL_ZONE_RECORD_LEN
        if len(payload) != expected:
            raise ControlResponseError(
                f"GET_ZONES count={count} implies {expected} bytes, got {len(payload)}"
            )
        zones = []
        for i in range(count):
            offset = 4 + i * CONTROL_ZONE_RECORD_LEN
            (
                index,
                relay_mask,
                control_mode,
                cal_offset_c,
                pid_kp,
                pid_ki,
                pid_kd,
                max_ramp,
                max_temp,
                min_temp,
            ) = struct.unpack_from("<BBBfffffff", payload, offset)
            zones.append(
                ZoneConfig(
                    index=index,
                    relay_mask=relay_mask,
                    control_mode=control_mode,
                    cal_offset_c=cal_offset_c,
                    pid_kp=pid_kp,
                    pid_ki=pid_ki,
                    pid_kd=pid_kd,
                    max_ramp_c_per_hr=max_ramp,
                    max_temp_c=max_temp,
                    min_temp_c=min_temp,
                )
            )
        return subcommand, (thermo_count, relay_count, zones)

    if subcommand in (CONTROL_CMD_SET_ZONE_PID, CONTROL_CMD_SET_ZONE_MODEL, CONTROL_CMD_SET_UNIT_PREF):
        return subcommand, _decode_ok_reason(payload, ControlResponseError, "SET_ZONE_*/SET_UNIT_PREF")

    if subcommand == CONTROL_CMD_GET_UNIT_PREF:
        if len(payload) < 2:
            raise ControlResponseError("GET_UNIT_PREF response is missing its value byte")
        return subcommand, payload[1]

    raise ControlResponseError(f"unknown CONTROL response subcommand 0x{subcommand:02X}")


