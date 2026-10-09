"""THERMO wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import math
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
# THERMO -- 3x MAX31856 (task_id = UART_TASK_ID_THERMO)
#
# The parts live on the thermocouple daughterboard behind J6 and hang off the
# main board's shared SPI bus (CS0/CS1/CS2 = channels 0/1/2). Each part's
# ~FAULT comes back to a real ESP32-S3 GPIO; each part's ~DRDY goes to the
# SX1509 instead, so "is a conversion ready" is only observable through the
# IO task -- see docs/HARDWARE.md.
# ---------------------------------------------------------------------------
TC_TYPE_LABELS: dict[int, str] = {
    TcType.B: "Type B",
    TcType.E: "Type E",
    TcType.J: "Type J",
    TcType.K: "Type K (this kiln)",
    TcType.N: "Type N",
    TcType.R: "Type R",
    TcType.S: "Type S",
    TcType.T: "Type T",
    TcType.VMODE_G8: "Voltage mode, gain 8",
    TcType.VMODE_G32: "Voltage mode, gain 32",
}

AVG_MODE_LABELS: dict[int, str] = {
    AvgMode.AVG_1: "1 sample",
    AvgMode.AVG_2: "2 samples",
    AvgMode.AVG_4: "4 samples",
    AvgMode.AVG_8: "8 samples",
    AvgMode.AVG_16: "16 samples",
}

#: Readable text for each SR bit. Shown instead of the hex byte everywhere a
#: human or an agent reads a fault -- "thermocouple open circuit" is
#: actionable, "SR=0x01" is a lookup task.
THERMO_FAULT_LABELS: dict[int, str] = {
    ThermoFault.OPEN: "open circuit (no thermocouple?)",
    ThermoFault.OVUV: "over/under voltage on an input",
    ThermoFault.TCLOW: "TC below low threshold",
    ThermoFault.TCHIGH: "TC above high threshold",
    ThermoFault.CJLOW: "cold junction below low threshold",
    ThermoFault.CJHIGH: "cold junction above high threshold",
    ThermoFault.TCRANGE: "TC outside the type's range",
    ThermoFault.CJRANGE: "cold junction outside -55..+125 C",
}

#: Same idea for the per-reading driver flags.
THERMO_READ_FLAG_LABELS: dict[int, str] = {
    ThermoReadFlag.FAULT_PIN: "~FAULT pin asserted",
    ThermoReadFlag.SPI_FAILED: "SPI read failed",
    ThermoReadFlag.STALE: "stale (no new conversion)",
}


class ThermoResponseError(ValueError):
    """Raised when a THERMO response payload does not match its wire layout."""


def thermo_fault_labels(status: int) -> "list[str]":
    """Decode an SR byte into readable fault descriptions (empty = no faults)."""
    status = _check_u8(status, "status")
    return [text for bit, text in THERMO_FAULT_LABELS.items() if status & bit]


def _check_thermo_channel(channel: int, allow_all: bool = False) -> int:
    channel = int(channel)
    if allow_all and channel == THERMO_CHANNEL_ALL:
        return channel
    if not 0 <= channel < THERMO_CHANNEL_COUNT:
        suffix = " (or 0xFF for all)" if allow_all else ""
        raise ValueError(
            f"channel must be 0..{THERMO_CHANNEL_COUNT - 1}{suffix}, got {channel}"
        )
    return channel


def thermo_config_channel(
    channel: int,
    tc_type: int = TcType.K,
    avg_mode: int = AvgMode.AVG_1,
    filter_50hz: bool = False,
    auto_convert: bool = True,
) -> bytes:
    """0x01 CONFIG_CHANNEL: ch, tc_type, avg_mode, filter(0=60Hz/1=50Hz), conv_mode.

    ``auto_convert`` False selects the part's one-shot / normally-off mode, in
    which nothing converts until :func:`thermo_one_shot`. The 50/60 Hz bit can
    only be changed with conversions stopped, which the firmware handles by
    stopping, writing, and restoring ``conv_mode`` around the write.
    """
    tc_type = int(tc_type)
    if tc_type not in tuple(int(t) for t in TcType):
        raise ValueError(f"tc_type must be one of TcType, got {tc_type}")
    avg_mode = int(avg_mode)
    if avg_mode not in tuple(int(a) for a in AvgMode):
        raise ValueError(f"avg_mode must be one of AvgMode, got {avg_mode}")
    return struct.pack(
        "<BBBBBB",
        THERMO_CMD_CONFIG_CHANNEL,
        _check_thermo_channel(channel),
        tc_type,
        avg_mode,
        _check_bool_byte(filter_50hz),
        _check_bool_byte(auto_convert),
    )


def thermo_set_thresholds(
    channel: int, tc_high: float, tc_low: float, cj_high: int, cj_low: int
) -> bytes:
    """0x02 SET_THRESHOLDS: ch, tc_high f32, tc_low f32, cj_high i8, cj_low i8.

    Thermocouple thresholds are stored by the part as int16 at 0.0625 degC per
    LSB, so the firmware rounds; the cold-junction pair are whole degrees.
    """
    return struct.pack(
        "<BBffbb",
        THERMO_CMD_SET_THRESHOLDS,
        _check_thermo_channel(channel),
        _check_finite(tc_high, "tc_high"),
        _check_finite(tc_low, "tc_low"),
        _check_i8(cj_high, "cj_high"),
        _check_i8(cj_low, "cj_low"),
    )


def thermo_set_cj_offset(channel: int, offset_c: float) -> bytes:
    """0x03 SET_CJ_OFFSET: ch, offset f32 LE in degC (part range +-8 degC)."""
    offset_c = float(offset_c)
    if not -8.0 <= offset_c <= 8.0:
        raise ValueError(f"cold-junction offset must be -8..+8 degC, got {offset_c}")
    return struct.pack(
        "<BBf", THERMO_CMD_SET_CJ_OFFSET, _check_thermo_channel(channel), offset_c
    )


def thermo_one_shot(channel: int) -> bytes:
    """0x04 ONE_SHOT: trigger a single conversion on one channel.

    The result is not returned by this command -- poll with :func:`thermo_read`
    or turn on auto-reporting.
    """
    return struct.pack("<BB", THERMO_CMD_ONE_SHOT, _check_thermo_channel(channel))


def thermo_read(channel: int = THERMO_CHANNEL_ALL) -> bytes:
    """0x05 READ request (query): one channel, or 0xFF for all three."""
    return struct.pack(
        "<BB", THERMO_CMD_READ, _check_thermo_channel(channel, allow_all=True)
    )


def thermo_read_faults(channel: int = THERMO_CHANNEL_ALL) -> bytes:
    """0x06 READ_FAULTS request (query): one channel, or 0xFF for all three."""
    return struct.pack(
        "<BB", THERMO_CMD_READ_FAULTS, _check_thermo_channel(channel, allow_all=True)
    )


def thermo_clear_faults(channel: int) -> bytes:
    """0x07 CLEAR_FAULTS: pulse CR0.FAULTCLR on one channel.

    Only does anything in the part's *interrupt* fault mode. In the comparator
    mode this driver uses, fault bits clear themselves once the condition goes
    away, so this is a debug lever rather than an operational one.
    """
    return struct.pack("<BB", THERMO_CMD_CLEAR_FAULTS, _check_thermo_channel(channel))


def thermo_set_auto_report(channel_mask: int, period_ms: int) -> bytes:
    """0x08 SET_AUTO_REPORT: bit N of ``channel_mask`` = channel N; period u16 LE.

    ``period_ms`` 0 turns reporting off. While on, the firmware pushes
    unsolicited READ replies (identical layout to the 0x05 answer) for the
    selected channels -- which is how the GUI's live temperature readout is
    fed, rather than by polling in a tight loop.
    """
    channel_mask = _check_range(
        channel_mask, 0, (1 << THERMO_CHANNEL_COUNT) - 1, "channel_mask"
    )
    return struct.pack(
        "<BBH", THERMO_CMD_SET_AUTO_REPORT, channel_mask, _check_u16(period_ms, "period_ms")
    )


def thermo_read_reg(channel: int, reg: int, length: int = 1) -> bytes:
    """0x09 READ_REG request (query, debug): ch, reg address, length 1..16."""
    return struct.pack(
        "<BBBB",
        THERMO_CMD_READ_REG,
        _check_thermo_channel(channel),
        _check_u8(reg, "reg"),
        _check_range(length, 1, THERMO_REG_READ_MAX, "length"),
    )


def thermo_write_reg(channel: int, reg: int, value: int) -> bytes:
    """0x0A WRITE_REG (debug): ch, reg address, value."""
    return struct.pack(
        "<BBBB",
        THERMO_CMD_WRITE_REG,
        _check_thermo_channel(channel),
        _check_u8(reg, "reg"),
        _check_u8(value, "value"),
    )


@dataclass(frozen=True)
class ThermoReading:
    """One 12-byte channel entry from a READ (or auto-report) reply.

    A channel whose SPI read failed still appears, with
    :attr:`ThermoReadFlag.SPI_FAILED` set and both temperatures NaN -- the
    firmware reports an explicitly-bad channel rather than omitting it,
    because a missing channel is the more confusing failure.
    """

    channel: int
    temperature_c: float
    cold_junction_c: float
    status: ThermoFault
    flags: ThermoReadFlag

    @property
    def valid(self) -> bool:
        """Whether the temperature reading is usable at all."""
        return not (self.flags & ThermoReadFlag.SPI_FAILED) and not math.isnan(
            self.temperature_c
        )

    @property
    def fault_labels(self) -> "list[str]":
        """Readable descriptions of every asserted SR bit."""
        return thermo_fault_labels(int(self.status))

    @property
    def flag_labels(self) -> "list[str]":
        return [text for bit, text in THERMO_READ_FLAG_LABELS.items() if self.flags & bit]

    def describe(self) -> str:
        if not self.valid:
            body = "invalid"
        else:
            body = f"{self.temperature_c:.2f} C (CJ {self.cold_junction_c:.2f} C)"
        notes = self.fault_labels + self.flag_labels
        return f"CH{self.channel}: {body}" + (f" [{'; '.join(notes)}]" if notes else "")


@dataclass(frozen=True)
class ThermoFaultStatus:
    """One 3-byte entry from a READ_FAULTS reply: SR plus the MASK register."""

    channel: int
    status: ThermoFault
    mask: int

    @property
    def fault_labels(self) -> "list[str]":
        return thermo_fault_labels(int(self.status))

    def describe(self) -> str:
        faults = self.fault_labels
        return (
            f"CH{self.channel}: "
            + ("; ".join(faults) if faults else "no faults")
            + f" (SR 0x{int(self.status):02X}, MASK 0x{self.mask:02X})"
        )


@dataclass(frozen=True)
class ThermoRegisters:
    """A READ_REG reply: raw register bytes from one MAX31856."""

    channel: int
    reg: int
    data: bytes

    def describe(self) -> str:
        return (
            f"CH{self.channel} reg 0x{self.reg:02X}: "
            + " ".join(f"{b:02X}" for b in self.data)
        )


def parse_thermo_response(
    payload: bytes,
) -> "tuple[int, list[ThermoReading] | list[ThermoFaultStatus] | ThermoRegisters]":
    """Decode a THERMO query reply (or auto-report push) into ``(subcmd, value)``.

    Layouts (uart_task_ids.h)::

        READ / AUTO_REPORT: byte0=0x05, byte1=count(N), then N * 12 bytes of
                            {channel, f32 tc, f32 cj, u8 SR, u8 flags, u8 rsvd}
        READ_FAULTS:        byte0=0x06, byte1=count(N), then N * {ch, SR, MASK}
        READ_REG:           byte0=0x09, byte1=ch, byte2=reg, byte3=len(N), N bytes

    Unlike the INFO replies these carry their subcommand back, so no structural
    guessing is needed. Raises :class:`ThermoResponseError` on any mismatch.
    """
    if len(payload) < 1:
        raise ThermoResponseError("THERMO response is empty")
    subcommand = payload[0]

    if subcommand == THERMO_CMD_READ:
        if len(payload) < 2:
            raise ThermoResponseError("READ response is missing its count byte")
        count = payload[1]
        if count > THERMO_CHANNEL_COUNT:
            raise ThermoResponseError(
                f"READ count={count} exceeds the board's {THERMO_CHANNEL_COUNT} channels"
            )
        expected = 2 + count * THERMO_READ_ENTRY_LEN
        if len(payload) != expected:
            raise ThermoResponseError(
                f"READ count={count} implies {expected} bytes, got {len(payload)}"
            )
        readings = []
        for i in range(count):
            offset = 2 + i * THERMO_READ_ENTRY_LEN
            channel, temperature, cold_junction, status, flags = struct.unpack_from(
                "<BffBB", payload, offset
            )
            if not 0 <= channel < THERMO_CHANNEL_COUNT:
                raise ThermoResponseError(
                    f"READ entry {i} reports channel {channel}, outside "
                    f"0..{THERMO_CHANNEL_COUNT - 1}"
                )
            # NaN is defined here (SPI read failed); an infinity is not, and
            # would otherwise reach the GUI readout and any control loop as a
            # perfectly ordinary temperature.
            try:
                temperature = _decoded_float(
                    temperature, f"READ entry {i} temperature", allow_nan=True
                )
                cold_junction = _decoded_float(
                    cold_junction, f"READ entry {i} cold junction", allow_nan=True
                )
            except ValueError as exc:
                raise ThermoResponseError(str(exc)) from exc
            readings.append(
                ThermoReading(
                    channel=channel,
                    temperature_c=temperature,
                    cold_junction_c=cold_junction,
                    status=ThermoFault(status),
                    flags=ThermoReadFlag(flags & 0x07),
                )
            )
        return subcommand, readings

    if subcommand == THERMO_CMD_READ_FAULTS:
        if len(payload) < 2:
            raise ThermoResponseError("READ_FAULTS response is missing its count byte")
        count = payload[1]
        if count > THERMO_CHANNEL_COUNT:
            raise ThermoResponseError(
                f"READ_FAULTS count={count} exceeds the board's "
                f"{THERMO_CHANNEL_COUNT} channels"
            )
        expected = 2 + count * THERMO_FAULT_ENTRY_LEN
        if len(payload) != expected:
            raise ThermoResponseError(
                f"READ_FAULTS count={count} implies {expected} bytes, got {len(payload)}"
            )
        entries = []
        for i in range(count):
            channel = payload[2 + i * 3]
            if not 0 <= channel < THERMO_CHANNEL_COUNT:
                raise ThermoResponseError(
                    f"READ_FAULTS entry {i} reports channel {channel}, outside "
                    f"0..{THERMO_CHANNEL_COUNT - 1}"
                )
            entries.append(
                ThermoFaultStatus(
                    channel=channel,
                    status=ThermoFault(payload[3 + i * 3]),
                    mask=payload[4 + i * 3],
                )
            )
        return subcommand, entries

    if subcommand == THERMO_CMD_READ_REG:
        if len(payload) < 4:
            raise ThermoResponseError("READ_REG response header is truncated")
        channel, reg, length = payload[1], payload[2], payload[3]
        if not 0 <= channel < THERMO_CHANNEL_COUNT:
            raise ThermoResponseError(
                f"READ_REG reports channel {channel}, outside "
                f"0..{THERMO_CHANNEL_COUNT - 1}"
            )
        # length 0 is not a request the PC ever sends (thermo_read_reg's own
        # range is 1..THERMO_REG_READ_MAX) -- it is the firmware's failure
        # marker for a channel whose SPI read did not succeed (uart_bridge.c,
        # THERMO_CMD_READ_REG case, 2026-08-20: "reply with 0 data bytes
        # instead of dropping the reply"). Accept it structurally here and let
        # ThermoClient.read_reg() turn it into a ThermoQueryError -- this
        # layer only validates the wire shape, not what the firmware meant.
        if not 0 <= length <= THERMO_REG_READ_MAX:
            raise ThermoResponseError(
                f"READ_REG len={length} outside 0..{THERMO_REG_READ_MAX}"
            )
        if len(payload) != 4 + length:
            raise ThermoResponseError(
                f"READ_REG len={length} implies {4 + length} bytes, got {len(payload)}"
            )
        return subcommand, ThermoRegisters(
            channel=channel, reg=reg, data=bytes(payload[4:])
        )

    if subcommand in (
        THERMO_CMD_CONFIG_CHANNEL,
        THERMO_CMD_SET_THRESHOLDS,
        THERMO_CMD_SET_CJ_OFFSET,
        THERMO_CMD_ONE_SHOT,
        THERMO_CMD_CLEAR_FAULTS,
        THERMO_CMD_SET_AUTO_REPORT,
        THERMO_CMD_WRITE_REG,
    ):
        # thermo_bridge_task() replies nothing at all when one of these
        # mutating subcommands succeeds; a reply only ever means
        # bridge_reply_reject() fired -- "truncated"/"out of range" caught
        # before the driver call, or "driver error" from thermo_owner's
        # bottom `if (err != ESP_OK)` block (uart_bridge.c). Decoded the same
        # way CONTROL's SET_ZONE_*/IO's relay refusals are, so the reason
        # text survives instead of raising "unknown response subcommand" on
        # what used to be an unhandled reply shape.
        return subcommand, _decode_ok_reason(
            payload,
            ThermoResponseError,
            "CONFIG_CHANNEL/SET_THRESHOLDS/SET_CJ_OFFSET/ONE_SHOT/CLEAR_FAULTS/"
            "SET_AUTO_REPORT/WRITE_REG",
        )

    raise ThermoResponseError(f"unknown THERMO response subcommand 0x{subcommand:02X}")


