"""IO wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import enum
import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403
from .devices_thermo import _check_thermo_channel  # noqa: F401
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
# IO -- SX1509 expander at 0x3E (task_id = UART_TASK_ID_IO)
#
# Board-level view (relays and digital I/O by their schematic names, 1-based)
# plus raw register access. The board-level commands are what the GUI and any
# control loop should use; the SX_* ones exist for bring-up and debugging.
# ---------------------------------------------------------------------------
#: The schematic's Relay1..Relay4 are the expander's IO0..IO3 in bit order,
#: which is NOT the K-designator order -- Relay1 drives K3, not K1. The
#: firmware exposes the schematic numbering and documents the mapping rather
#: than silently renumbering, so every UI that shows a relay shows all three
#: names at once. relay -> (K designator, terminal block).
RELAY_DESIGNATORS: dict[int, tuple[str, str]] = {
    1: ("K3", "J8"),
    2: ("K1", "J3"),
    3: ("K2", "J4"),
    4: ("K5", "J11"),
}

#: What each of the seven general-purpose I/O actually reaches. IO_1 and IO_2
#: are opto-isolated (in and out respectively); the rest go straight out to
#: terminal blocks.
DIGITAL_IO_LABELS: dict[int, str] = {
    1: "opto-isolated input from J24 (U13)",
    2: "opto-isolated output to J25 (U14)",
    3: "J20 pin 1 (direct)",
    4: "J20 pin 2 (direct)",
    5: "J21 pin 1",
    6: "J21 pin 2",
    7: "J23 pin 1 (J23 pin 2 is GND)",
}

#: Every SX1509 pin by its schematic net name, for the raw-register view --
#: bare bit numbers are unreadable next to a board with named nets.
EXPANDER_PIN_NAMES: dict[int, str] = {
    0: "Relay1 (K3/J8)",
    1: "Relay2 (K1/J3)",
    2: "Relay3 (K2/J4)",
    3: "Relay4 (K5/J11)",
    4: "IO_1",
    5: "IO_2",
    6: "IO_3",
    7: "IO_4",
    8: "thermoDrdy_0",
    9: "thermoDrdy_1",
    10: "thermoDrdy_2",
    11: "IO_5",
    12: "IO_6",
    13: "IO_7",
    14: "LCD_IORQ (display D/C)",
    15: "LCD_Reset (display ~RESET)",
}

#: Expander bit for digital I/O 1..7: IO_1..IO_4 are pins 4-7, IO_5..IO_7 are
#: pins 11-13 (pins 8-10 are the DRDY inputs in between).
DIGITAL_IO_PINS: dict[int, int] = {1: 4, 2: 5, 3: 6, 4: 7, 5: 11, 6: 12, 7: 13}

#: Expander bits carrying the three ~DRDY inputs, channel 0..2.
DRDY_PINS: tuple[int, ...] = (8, 9, 10)


class IoResponseError(ValueError):
    """Raised when an IO response payload does not match its wire layout."""


class RelayRefusal(enum.Enum):
    """Why uart_bridge.c's io_bridge_task() refused a SET_RELAY/SET_RELAY_MASK.

    Mirrors the exact ASCII reason strings bridge_reply_reject() sends for
    IO_CMD_SET_RELAY/SET_RELAY_MASK (uart_bridge.c, io_bridge_task() -- the
    KILN_IO_OWNER_RELAY_ERR_OWNED/ERR_SAFETY/ERR_UPDATING cases plus the
    truncated-payload/out-of-range/driver-error guards every subcommand
    shares). Kept as a closed set of *known* strings rather than a numeric
    wire code -- the firmware side already shipped free-text reasons
    (commit 5df2190) and there is no protocol version bump backing a switch
    to a numeric enum; classifying the text here gets the same "tell them
    apart programmatically" property without another wire change. A string
    this doesn't recognize (e.g. a future reason, or a build predating one
    of these) still round-trips as OTHER with the raw text preserved.
    """

    TRUNCATED = "truncated"
    OUT_OF_RANGE = "out of range"
    OWNED = "owned"
    SAFETY = "safety"
    UPDATING = "updating"
    DRIVER_ERROR = "driver error"
    # docs/SYSTEM_MODE_GATE.md, owner decision 2026-09-25 (Q1/Q4):
    # KILN_IO_OWNER_RELAY_ERR_RUNNING's UART reject word (uart_bridge_io.c):
    # a firing or autotune run, or (2026-09-28) a backup restore, is active.
    RUNNING = "running"
    # KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK's UART reject word, added
    # 2026-09-15 but never mirrored here until this same review pass.
    CRASH_UNACKED = "crash_unacked"
    OTHER = "other"

    @classmethod
    def from_wire(cls, reason: "str | None") -> "RelayRefusal":
        if not reason:
            return cls.OTHER
        for member in cls:
            if member is not cls.OTHER and member.value == reason:
                return member
        return cls.OTHER


@dataclass(frozen=True)
class RelayResult:
    """Decoded reply to IO_CMD_SET_RELAY/SET_RELAY_MASK.

    io_bridge_task() replies nothing at all when the write actually happens
    (bridge_reply_reject() is only ever called from a refusal path), so any
    reply this decodes IS a refusal -- ``ok`` is carried anyway rather than
    hardcoded True/False so a future firmware that starts ACKing success
    explicitly doesn't get silently misread as a refusal.
    """

    ok: bool
    reason_text: "str | None"
    refusal: RelayRefusal

    def __bool__(self) -> bool:
        return self.ok

    def describe(self) -> str:
        if self.ok:
            return "ok"
        return f"refused ({self.refusal.value})" + (
            f": {self.reason_text}" if self.reason_text and self.refusal is RelayRefusal.OTHER else ""
        )


def relay_label(relay: int) -> str:
    """e.g. ``"Relay1 (K3 -> J8)"`` -- always show all three names together."""
    designator, block = RELAY_DESIGNATORS[_check_range(relay, 1, IO_RELAY_COUNT, "relay")]
    return f"Relay{relay} ({designator} -> {block})"


def digital_io_label(io: int) -> str:
    """e.g. ``"IO 1: opto-isolated input from J24 (U13)"``."""
    io = _check_range(io, 1, IO_DIGITAL_COUNT, "io")
    return f"IO {io}: {DIGITAL_IO_LABELS[io]}"


def expander_pin_name(pin: int) -> str:
    """Schematic net name for one SX1509 pin (0-15)."""
    return EXPANDER_PIN_NAMES[_check_range(pin, 0, IO_EXPANDER_PIN_COUNT - 1, "pin")]


def io_set_relay(relay: int, on: bool) -> bytes:
    """0x01 SET_RELAY: relay 1-4 (schematic numbering), on 0/1.

    High = coil energized: the expander pin drives a BSS138 low-side switch on
    a 12 V coil.
    """
    return struct.pack(
        "<BBB",
        IO_CMD_SET_RELAY,
        _check_range(relay, 1, IO_RELAY_COUNT, "relay"),
        _check_bool_byte(on),
    )


def io_set_relay_mask(mask: int, value: int) -> bytes:
    """0x02 SET_RELAY_MASK: which relays to change, and their new levels.

    Bits 0-3 are Relay1..Relay4 in both fields. One atomic register write, so
    several relays switch on the same I2C transfer instead of sequentially.
    """
    return struct.pack(
        "<BBB",
        IO_CMD_SET_RELAY_MASK,
        _check_range(mask, 0, 0x0F, "mask"),
        _check_range(value, 0, 0x0F, "value"),
    )


def io_set_io(io: int, level: bool) -> bytes:
    """0x03 SET_IO: digital I/O 1-7 to a level. Only meaningful when it's an output."""
    return struct.pack(
        "<BBB",
        IO_CMD_SET_IO,
        _check_range(io, 1, IO_DIGITAL_COUNT, "io"),
        _check_bool_byte(level),
    )


def io_set_io_dir(io: int, is_input: bool, pullup: bool = False) -> bytes:
    """0x04 SET_IO_DIR: digital I/O 1-7 direction (+ pull-up, inputs only).

    ``is_input`` matches the part's RegDir polarity (1 = input).
    """
    return struct.pack(
        "<BBBB",
        IO_CMD_SET_IO_DIR,
        _check_range(io, 1, IO_DIGITAL_COUNT, "io"),
        _check_bool_byte(is_input),
        _check_bool_byte(pullup),
    )


def io_read() -> bytes:
    """0x05 READ request (query): no args."""
    return struct.pack("<B", IO_CMD_READ)


def io_set_auto_report(period_ms: int) -> bytes:
    """0x06 SET_AUTO_REPORT: period u16 LE, 0 = off.

    While on, the firmware pushes unsolicited READ replies at that period
    *and* immediately on every ~INT edge, so an input change is reported
    without waiting out the period.
    """
    return struct.pack("<BH", IO_CMD_SET_AUTO_REPORT, _check_u16(period_ms, "period_ms"))


def io_all_relays_off() -> bytes:
    """0x07 ALL_RELAYS_OFF: no args, unconditional.

    This is also the state the firmware falls back to on link loss or a safety
    fault. It is its own subcommand precisely so it is one short frame that
    cannot be misparsed as anything else.
    """
    return struct.pack("<B", IO_CMD_ALL_RELAYS_OFF)


def sx_write_reg(reg: int, value: int) -> bytes:
    """0x10 SX_WRITE_REG: raw register write (debug)."""
    return struct.pack(
        "<BBB", IO_CMD_SX_WRITE_REG, _check_u8(reg, "reg"), _check_u8(value, "value")
    )


def sx_read_reg(reg: int, length: int = 1) -> bytes:
    """0x11 SX_READ_REG request (query, debug): reg address, length 1..16."""
    return struct.pack(
        "<BBB",
        IO_CMD_SX_READ_REG,
        _check_u8(reg, "reg"),
        _check_range(length, 1, IO_REG_READ_MAX, "length"),
    )


def sx_set_dir(mask: int) -> bytes:
    """0x12 SX_SET_DIR: u16 LE, bit N 1 = input (the part's RegDir polarity)."""
    return struct.pack("<BH", IO_CMD_SX_SET_DIR, _check_u16(mask, "mask"))


def sx_set_pullup(mask: int) -> bytes:
    """0x13 SX_SET_PULLUP: u16 LE."""
    return struct.pack("<BH", IO_CMD_SX_SET_PULLUP, _check_u16(mask, "mask"))


def sx_set_opendrain(mask: int) -> bytes:
    """0x14 SX_SET_OPENDRAIN: u16 LE."""
    return struct.pack("<BH", IO_CMD_SX_SET_OPENDRAIN, _check_u16(mask, "mask"))


def sx_set_debounce(enable_mask: int, config: int) -> bytes:
    """0x15 SX_SET_DEBOUNCE: enable mask u16 LE, config 0-7.

    ``config`` is RegDebounceConfig: the debounce time is 0.5 ms << config at
    the part's 2 MHz internal oscillator.
    """
    return struct.pack(
        "<BHB",
        IO_CMD_SX_SET_DEBOUNCE,
        _check_u16(enable_mask, "enable_mask"),
        _check_range(config, 0, 7, "config"),
    )


def sx_set_int_mask(mask: int, sense: int) -> bytes:
    """0x16 SX_SET_INT_MASK: mask u16 LE, sense u16 LE.

    ``mask`` matches RegInterruptMask (bit N = 1 *disables* pin N's
    interrupt). ``sense`` is 2 bits per pin pair, exactly as the part's
    RegSense registers already encode them.
    """
    return struct.pack(
        "<BHH", IO_CMD_SX_SET_INT_MASK, _check_u16(mask, "mask"), _check_u16(sense, "sense")
    )


def sx_led_driver(pin: int, enable: bool, intensity: int = 0) -> bytes:
    """0x17 SX_LED_DRIVER: pin 0-15, enable, intensity 0-255.

    0 is *full on* for this part's sink driver, not off -- the register sets
    the amount the driver pulls down.
    """
    return struct.pack(
        "<BBBB",
        IO_CMD_SX_LED_DRIVER,
        _check_range(pin, 0, IO_EXPANDER_PIN_COUNT - 1, "pin"),
        _check_bool_byte(enable),
        _check_u8(intensity, "intensity"),
    )


def sx_reset(hard: bool) -> bytes:
    """0x18 SX_RESET: 0 = software reset via RegReset, 1 = pulse ~RESET (GPIO10)."""
    return struct.pack("<BB", IO_CMD_SX_RESET, _check_bool_byte(hard))


def sx_scan() -> bytes:
    """0x19 SX_SCAN request (query): probes 0x3E/0x3F/0x70/0x71."""
    return struct.pack("<B", IO_CMD_SX_SCAN)


@dataclass(frozen=True)
class IoState:
    """Decoded IO READ / auto-report reply.

    ``data``/``dir`` are the raw expander registers; the rest is the same
    state already split into the board-level view (relays, digital I/O, DRDY)
    so nothing downstream has to know which bit is which pin.
    """

    data: int
    dir: int
    relays: int
    io_levels: int
    drdy: int
    flags: int

    #: flags bit0 -- ~INT is currently asserted (an input changed).
    FLAG_INT = 0x01
    #: flags bit1 -- the firmware's last I2C transfer to the part failed, so
    #: everything else in this reply is the previous state at best.
    FLAG_I2C_FAILED = 0x02
    #: flags bit2 -- the relay state could not be established (a coil may be
    #: energised even though ``relays`` reads 0); treat as a fault, not as OFF.
    FLAG_RELAY_UNKNOWN = 0x04

    @property
    def relay_state_unknown(self) -> bool:
        return bool(self.flags & self.FLAG_RELAY_UNKNOWN)

    @property
    def int_asserted(self) -> bool:
        return bool(self.flags & self.FLAG_INT)

    @property
    def i2c_failed(self) -> bool:
        return bool(self.flags & self.FLAG_I2C_FAILED)

    def relay(self, relay: int) -> bool:
        """Commanded state of relay 1-4 (the firmware's shadow, not a read-back:
        the pin is an output, so its own drive is all a read would show)."""
        return bool(self.relays & (1 << (_check_range(relay, 1, IO_RELAY_COUNT, "relay") - 1)))

    def io_level(self, io: int) -> bool:
        """Level of digital I/O 1-7, as actually read from the pin."""
        return bool(self.io_levels & (1 << (_check_range(io, 1, IO_DIGITAL_COUNT, "io") - 1)))

    def io_is_input(self, io: int) -> bool:
        """Whether digital I/O 1-7 is currently configured as an input."""
        pin = DIGITAL_IO_PINS[_check_range(io, 1, IO_DIGITAL_COUNT, "io")]
        return bool(self.dir & (1 << pin))

    def drdy_asserted(self, channel: int) -> bool:
        """Whether thermocouple channel 0-2 has a conversion ready.

        ~DRDY is active low on the part; the firmware has already inverted it
        here, so True means "ready", not "pin is high".
        """
        return bool(self.drdy & (1 << _check_thermo_channel(channel)))

    def pin(self, pin: int) -> bool:
        """Raw state of one expander pin 0-15."""
        return bool(self.data & (1 << _check_range(pin, 0, IO_EXPANDER_PIN_COUNT - 1, "pin")))

    def describe(self) -> str:
        relays = " ".join(
            f"R{n}={int(self.relay(n))}" for n in range(1, IO_RELAY_COUNT)
        )
        relays += (
            f"  K4_bit={int(self.relay(IO_RELAY_COUNT))} (expander bit only; NOT heat "
            "state -- K4 is Pico-owned via SAFETY_CMD_REQUEST_ENABLE, see "
            "safety_request_enable)"
        )
        ios = " ".join(
            f"IO{n}={int(self.io_level(n))}{'i' if self.io_is_input(n) else 'o'}"
            for n in range(1, IO_DIGITAL_COUNT + 1)
        )
        drdy = " ".join(
            f"DRDY{c}={int(self.drdy_asserted(c))}" for c in range(THERMO_CHANNEL_COUNT)
        )
        notes = []
        if self.int_asserted:
            notes.append("~INT asserted")
        if self.i2c_failed:
            notes.append("I2C FAILED")
        if self.relay_state_unknown:
            notes.append("RELAY STATE UNKNOWN - a coil may be energised")
        return (
            f"data 0x{self.data:04X} dir 0x{self.dir:04X}  {relays}  {ios}  {drdy}"
            + (f"  [{'; '.join(notes)}]" if notes else "")
        )


@dataclass(frozen=True)
class ExpanderRegisters:
    """An SX_READ_REG reply: raw register bytes from the SX1509."""

    reg: int
    data: bytes

    def describe(self) -> str:
        return f"reg 0x{self.reg:02X}: " + " ".join(f"{b:02X}" for b in self.data)


#: Every other IO write subcommand (not SET_RELAY/SET_RELAY_MASK, which get
#: their own RelayResult/RelayRefusal classification above) is fire-and-
#: forget on success -- io_bridge_task() sends nothing back. A reply under
#: one of these ids is always a refusal: truncated/out-of-range args, the
#: "safety" guard SX_WRITE_REG/SX_SET_DIR share with the relay commands (see
#: uart_bridge.c), or the generic bottom-of-task driver-error reject. Before
#: this, any of these ids raised "unknown IO response subcommand" and the
#: refusal was dropped in IoClient._handle_reply, invisible to a caller that
#: used the fire-and-forget ``send()``.
_IO_WRITE_SUBCOMMANDS = frozenset(
    {
        IO_CMD_SET_IO,
        IO_CMD_SET_IO_DIR,
        IO_CMD_SET_AUTO_REPORT,
        IO_CMD_ALL_RELAYS_OFF,
        IO_CMD_SX_WRITE_REG,
        IO_CMD_SX_SET_DIR,
        IO_CMD_SX_SET_PULLUP,
        IO_CMD_SX_SET_OPENDRAIN,
        IO_CMD_SX_SET_DEBOUNCE,
        IO_CMD_SX_SET_INT_MASK,
        IO_CMD_SX_LED_DRIVER,
        IO_CMD_SX_RESET,
    }
)


def parse_io_response(
    payload: bytes,
) -> "tuple[int, IoState | ExpanderRegisters | list[int] | RelayResult | OkReason]":
    """Decode an IO query reply (or auto-report push) into ``(subcmd, value)``.

    Layouts (uart_task_ids.h)::

        READ / AUTO_REPORT: byte0=0x05, RegData u16 LE, RegDir u16 LE,
                            relay shadow u8, io levels u8, DRDY u8, flags u8
        SX_READ_REG:        byte0=0x11, reg u8, len(N) u8, N bytes
        SX_SCAN:            byte0=0x19, count(N) u8, N address bytes
        SET_RELAY / SET_RELAY_MASK: byte0=0x01/0x02, ok(=0) u8,
                            [len u8, reason ASCII] -- io_bridge_task() never
                            replies on success, so any frame with this
                            subcmd IS a refusal; see :class:`RelayResult` /
                            :class:`RelayRefusal`.
        Every other write subcommand (SET_IO, SET_IO_DIR, SET_AUTO_REPORT,
                            ALL_RELAYS_OFF, SX_WRITE_REG, SX_SET_DIR,
                            SX_SET_PULLUP, SX_SET_OPENDRAIN, SX_SET_DEBOUNCE,
                            SX_SET_INT_MASK, SX_LED_DRIVER, SX_RESET): same
                            ``{subcmd, ok=0, [len, reason]}`` refusal shape,
                            decoded into a plain :class:`OkReason` (no
                            per-reason classification -- RelayRefusal is
                            SET_RELAY/SET_RELAY_MASK-specific).

    Raises :class:`IoResponseError` on anything that doesn't match.
    """
    if len(payload) < 1:
        raise IoResponseError("IO response is empty")
    subcommand = payload[0]

    if subcommand in (IO_CMD_SET_RELAY, IO_CMD_SET_RELAY_MASK):
        ok_reason = _decode_ok_reason(payload, IoResponseError, "SET_RELAY/SET_RELAY_MASK")
        return subcommand, RelayResult(
            ok=ok_reason.ok,
            reason_text=ok_reason.reason,
            refusal=RelayRefusal.from_wire(ok_reason.reason),
        )

    if subcommand in _IO_WRITE_SUBCOMMANDS:
        return subcommand, _decode_ok_reason(payload, IoResponseError, "IO write")

    if subcommand == IO_CMD_READ:
        if len(payload) != 9:
            raise IoResponseError(f"READ response must be 9 bytes, got {len(payload)}")
        data, direction, relays, io_levels, drdy, flags = struct.unpack_from(
            "<HHBBBB", payload, 1
        )
        return subcommand, IoState(
            data=data,
            dir=direction,
            relays=relays,
            io_levels=io_levels,
            drdy=drdy,
            flags=flags,
        )

    if subcommand == IO_CMD_SX_READ_REG:
        if len(payload) < 3:
            raise IoResponseError("SX_READ_REG response header is truncated")
        reg, length = payload[1], payload[2]
        if not 1 <= length <= IO_REG_READ_MAX:
            raise IoResponseError(
                f"SX_READ_REG len={length} outside 1..{IO_REG_READ_MAX}"
            )
        if len(payload) != 3 + length:
            raise IoResponseError(
                f"SX_READ_REG len={length} implies {3 + length} bytes, got {len(payload)}"
            )
        return subcommand, ExpanderRegisters(reg=reg, data=bytes(payload[3:]))

    if subcommand == IO_CMD_SX_SCAN:
        if len(payload) < 2:
            raise IoResponseError("SX_SCAN response is missing its count byte")
        count = payload[1]
        if len(payload) != 2 + count:
            raise IoResponseError(
                f"SX_SCAN count={count} implies {2 + count} bytes, got {len(payload)}"
            )
        addresses = list(payload[2:])
        # 7-bit I2C addressing: anything above 0x7F is not an address the
        # firmware could have probed, so the payload isn't a scan result.
        bad = [a for a in addresses if a > 0x7F]
        if bad:
            raise IoResponseError(
                "SX_SCAN reported non-7-bit I2C address(es): "
                + ", ".join(f"0x{a:02X}" for a in bad)
            )
        return subcommand, addresses

    raise IoResponseError(f"unknown IO response subcommand 0x{subcommand:02X}")


