"""Command payload builders for the DAC and AD9833 UART bridge tasks.

Single source of truth for the byte packing described in
``App/drivers/uart_task_ids.h`` and consumed by ``App/drivers/uart_bridge.c``.
Both the MCP server and the Tkinter GUI build payloads through here so the
layout lives in exactly one place.

Endianness: every multi-byte numeric field in a *payload* is LITTLE-endian --
uart_bridge.c ``memcpy``s straight into a native ESP32 ``float``/``double``.
The protocol *header* is big-endian; see protocol.py. That split is intentional.

Limitation, by design of the current firmware: uart_bridge.c only ACKs/NACKs at
the protocol layer. It never sends an application-level status DATA frame back.
So a successful send means "the command reached the ESP task's inbox", NOT that
e.g. ``DcDac_set_channel_percent`` succeeded on the I2C bus -- the bridge task
only logs such failures locally over its own console. There is no PC-visible
result path for device-level errors.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import (
    AD9833_CMD_RESET,
    AD9833_CMD_SELECT_FREQ_REG,
    AD9833_CMD_SELECT_PHASE_REG,
    AD9833_CMD_SET_FREQUENCY,
    AD9833_CMD_SET_PHASE,
    AD9833_CMD_SET_WAVEFORM,
    AD9833_CMD_SLEEP,
    DAC_CMD_POWER_DOWN,
    DAC_CMD_SET_ALL_PERCENT,
    DAC_CMD_SET_CHANNEL_PERCENT,
    INFO_CMD_GET_FW_VERSION,
    INFO_CMD_GET_PIN_CONFIG,
    OLED_CMD_CLEAR,
    OLED_CMD_DISPLAY,
    OLED_CMD_PRINT,
    OLED_CMD_SET_CONTRAST,
    OLED_CMD_SET_CURSOR,
    OLED_CMD_SET_INVERT,
    OLED_CMD_SET_POWER,
    SYSTEM_CMD_RESTART_UART,
    UART_PROTO_MAX_PAYLOAD,
    UART_PROTOCOL_VERSION,
    UART_TASK_ID_AD9833,
    UART_TASK_ID_DAC,
    UART_TASK_ID_INFO,
    UART_TASK_ID_LOG,
    UART_TASK_ID_OLED,
    UART_TASK_ID_SYSTEM,
    LogLevel,
    PinFunction,
    Waveform,
)

__all__ = [
    "UART_PROTOCOL_VERSION",
    "UART_TASK_ID_DAC",
    "UART_TASK_ID_AD9833",
    "UART_TASK_ID_INFO",
    "UART_TASK_ID_OLED",
    "UART_TASK_ID_LOG",
    "UART_TASK_ID_SYSTEM",
    "system_restart_uart",
    "PinFunction",
    "Waveform",
    "LogLevel",
    "LogLine",
    "parse_log_frame",
    "DAC_CHANNELS",
    "OLED_MAX_TEXT_LEN",
    "WAVEFORM_LABELS",
    "PIN_FUNCTION_LABELS",
    "PIN_FUNCTION_ABBREV",
    "PinConfigEntry",
    "FirmwareVersion",
    "InfoResponseError",
    "pin_function_label",
    "pin_function_abbrev",
    "dac_set_channel_percent",
    "dac_set_all_percent",
    "dac_power_down",
    "ad9833_set_frequency",
    "ad9833_set_phase",
    "ad9833_set_waveform",
    "ad9833_select_freq_reg",
    "ad9833_select_phase_reg",
    "ad9833_reset",
    "ad9833_sleep",
    "oled_clear",
    "oled_set_cursor",
    "oled_print",
    "oled_display",
    "oled_set_contrast",
    "oled_set_invert",
    "oled_set_power",
    "info_get_pin_config",
    "info_get_fw_version",
    "parse_pin_config_response",
    "parse_fw_version_response",
    "parse_info_response",
]

DAC_CHANNELS = 4

#: PRINT's payload is [subcmd byte][text bytes]; the text can't exceed the
#: protocol's max payload minus that one byte.
OLED_MAX_TEXT_LEN = UART_PROTO_MAX_PAYLOAD - 1

WAVEFORM_LABELS: dict[int, str] = {
    Waveform.SINE: "Sine",
    Waveform.TRIANGLE: "Triangle",
    Waveform.SQUARE: "Square",
    Waveform.SQUARE_DIV2: "Square/2",
}

#: What each PIN_FUNC_* id actually means on this board. Kept PC-side on
#: purpose (uart_task_ids.h says so): the wire carries only the id. The text
#: names the peripheral/driver that owns the pin in App/main.c, sourced from
#: the same App/drivers/settings.h macros s_pin_config[] is built from.
PIN_FUNCTION_LABELS: dict[int, str] = {
    PinFunction.I2C_SDA: "I2C SDA (MCP4728 DAC + SSD1306 OLED)",
    PinFunction.I2C_SCL: "I2C SCL (MCP4728 DAC + SSD1306 OLED)",
    PinFunction.UART_TX: "UART0 TX (this link)",
    PinFunction.UART_RX: "UART0 RX (this link)",
    PinFunction.SPI_SCLK: "SPI SCLK (AD9833)",
    PinFunction.SPI_MOSI: "SPI MOSI (AD9833)",
    PinFunction.SPI_CS: "SPI CS (AD9833)",
    PinFunction.LED_HEARTBEAT: "Heartbeat LED",
}

#: Short forms for the pinout-image callouts, where there is no room for the
#: full label (that lives in the legend beside the diagram).
PIN_FUNCTION_ABBREV: dict[int, str] = {
    PinFunction.I2C_SDA: "SDA",
    PinFunction.I2C_SCL: "SCL",
    PinFunction.UART_TX: "TX",
    PinFunction.UART_RX: "RX",
    PinFunction.SPI_SCLK: "SCLK",
    PinFunction.SPI_MOSI: "MOSI",
    PinFunction.SPI_CS: "CS",
    PinFunction.LED_HEARTBEAT: "LED",
}


def pin_function_label(function_id: int) -> str:
    """Human-readable description for a function id, tolerating unknown ids."""
    return PIN_FUNCTION_LABELS.get(function_id, f"unknown function 0x{function_id:02X}")


def pin_function_abbrev(function_id: int) -> str:
    """Short callout text for a function id, tolerating unknown ids."""
    return PIN_FUNCTION_ABBREV.get(function_id, f"0x{function_id:02X}")


# ---------------------------------------------------------------------------
# validation helpers
# ---------------------------------------------------------------------------
def _check_channel(channel: int) -> int:
    channel = int(channel)
    if not 0 <= channel < DAC_CHANNELS:
        raise ValueError(f"channel must be 0..{DAC_CHANNELS - 1}, got {channel}")
    return channel


def _check_percent(percent: float) -> float:
    percent = float(percent)
    if not 0.0 <= percent <= 100.0:
        raise ValueError(f"percent must be 0..100, got {percent}")
    return percent


def _check_reg(reg: int) -> int:
    reg = int(reg)
    if reg not in (0, 1):
        raise ValueError(f"register must be 0 or 1, got {reg}")
    return reg


def _check_bool_byte(value: object, name: str) -> int:
    return 1 if bool(value) else 0


# ---------------------------------------------------------------------------
# MCP4728 DAC (task_id = UART_TASK_ID_DAC)
# ---------------------------------------------------------------------------
def dac_set_channel_percent(channel: int, percent: float) -> bytes:
    """0x01 SET_CHANNEL_PERCENT: byte1=channel(0-3), bytes2..5 = f32 LE percent."""
    return struct.pack(
        "<BBf", DAC_CMD_SET_CHANNEL_PERCENT, _check_channel(channel), _check_percent(percent)
    )


def dac_set_all_percent(percents: "list[float] | tuple[float, ...]") -> bytes:
    """0x02 SET_ALL_PERCENT: bytes1..16 = f32 LE percent[4] (ch0..ch3)."""
    values = list(percents)
    if len(values) != DAC_CHANNELS:
        raise ValueError(f"expected {DAC_CHANNELS} percent values, got {len(values)}")
    return struct.pack(
        "<B4f", DAC_CMD_SET_ALL_PERCENT, *(_check_percent(v) for v in values)
    )


def dac_power_down(channel: int, power_mode: int = 0) -> bytes:
    """0x03 POWER_DOWN: byte1=channel(0-3), byte2=power_mode(0-3)."""
    power_mode = int(power_mode)
    if not 0 <= power_mode <= 3:
        raise ValueError(f"power_mode must be 0..3, got {power_mode}")
    return struct.pack("<BBB", DAC_CMD_POWER_DOWN, _check_channel(channel), power_mode)


# ---------------------------------------------------------------------------
# SYSTEM (task_id = UART_TASK_ID_SYSTEM)
# ---------------------------------------------------------------------------
def system_restart_uart() -> bytes:
    """0x01 RESTART_UART request: byte0 = subcommand, no args."""
    return struct.pack("<B", SYSTEM_CMD_RESTART_UART)


# ---------------------------------------------------------------------------
# AD9833 function generator (task_id = UART_TASK_ID_AD9833)
# ---------------------------------------------------------------------------
def ad9833_set_frequency(reg: int, freq_hz: float) -> bytes:
    """0x01 SET_FREQUENCY: byte1=reg(0=FREQ0/1=FREQ1), bytes2..9 = f64 LE Hz."""
    freq_hz = float(freq_hz)
    if freq_hz < 0.0:
        raise ValueError(f"freq_hz must be >= 0, got {freq_hz}")
    return struct.pack("<BBd", AD9833_CMD_SET_FREQUENCY, _check_reg(reg), freq_hz)


def ad9833_set_phase(reg: int, degrees: float) -> bytes:
    """0x02 SET_PHASE: byte1=reg(0=PHASE0/1=PHASE1), bytes2..9 = f64 LE degrees."""
    return struct.pack("<BBd", AD9833_CMD_SET_PHASE, _check_reg(reg), float(degrees))


def ad9833_set_waveform(waveform: int) -> bytes:
    """0x03 SET_WAVEFORM: byte1 = 0 sine / 1 triangle / 2 square / 3 square-div2."""
    waveform = int(waveform)
    if waveform not in tuple(int(w) for w in Waveform):
        raise ValueError(f"waveform must be 0..3, got {waveform}")
    return struct.pack("<BB", AD9833_CMD_SET_WAVEFORM, waveform)


def ad9833_select_freq_reg(reg: int) -> bytes:
    """0x04 SELECT_FREQ_REG: byte1=reg(0/1)."""
    return struct.pack("<BB", AD9833_CMD_SELECT_FREQ_REG, _check_reg(reg))


def ad9833_select_phase_reg(reg: int) -> bytes:
    """0x05 SELECT_PHASE_REG: byte1=reg(0/1)."""
    return struct.pack("<BB", AD9833_CMD_SELECT_PHASE_REG, _check_reg(reg))


def ad9833_reset(hold: bool) -> bytes:
    """0x06 RESET: byte1=hold(0/1). hold=True asserts reset, False releases it."""
    return struct.pack("<BB", AD9833_CMD_RESET, _check_bool_byte(hold, "hold"))


def ad9833_sleep(dac_power_down: bool, mclk_power_down: bool) -> bytes:
    """0x07 SLEEP: byte1=dac_power_down(0/1), byte2=mclk_power_down(0/1)."""
    return struct.pack(
        "<BBB",
        AD9833_CMD_SLEEP,
        _check_bool_byte(dac_power_down, "dac_power_down"),
        _check_bool_byte(mclk_power_down, "mclk_power_down"),
    )


# ---------------------------------------------------------------------------
# SSD1306 OLED (task_id = UART_TASK_ID_OLED)
# ---------------------------------------------------------------------------
def oled_clear() -> bytes:
    """0x01 CLEAR: no args. Clears the in-RAM framebuffer only."""
    return struct.pack("<B", OLED_CMD_CLEAR)


def oled_set_cursor(col: int, row: int) -> bytes:
    """0x02 SET_CURSOR: byte1=col, byte2=row (text-cell coordinates)."""
    col = int(col)
    row = int(row)
    if not 0 <= col <= 255:
        raise ValueError(f"col must be 0..255, got {col}")
    if not 0 <= row <= 255:
        raise ValueError(f"row must be 0..255, got {row}")
    return struct.pack("<BBB", OLED_CMD_SET_CURSOR, col, row)


def oled_print(text: str) -> bytes:
    """0x03 PRINT: bytes1.. = ASCII text, NOT null-terminated, written at the cursor."""
    encoded = text.encode("ascii", errors="replace")
    if len(encoded) > OLED_MAX_TEXT_LEN:
        raise ValueError(
            f"text too long: {len(encoded)} bytes > {OLED_MAX_TEXT_LEN}-byte limit"
        )
    return struct.pack("<B", OLED_CMD_PRINT) + encoded


def oled_display() -> bytes:
    """0x04 DISPLAY: no args. Flushes the framebuffer to the panel over I2C."""
    return struct.pack("<B", OLED_CMD_DISPLAY)


def oled_set_contrast(contrast: int) -> bytes:
    """0x05 SET_CONTRAST: byte1=contrast(0-255)."""
    contrast = int(contrast)
    if not 0 <= contrast <= 255:
        raise ValueError(f"contrast must be 0..255, got {contrast}")
    return struct.pack("<BB", OLED_CMD_SET_CONTRAST, contrast)


def oled_set_invert(invert: bool) -> bytes:
    """0x06 SET_INVERT: byte1=invert(0/1)."""
    return struct.pack("<BB", OLED_CMD_SET_INVERT, _check_bool_byte(invert, "invert"))


def oled_set_power(on: bool) -> bytes:
    """0x07 SET_POWER: byte1=on(0/1)."""
    return struct.pack("<BB", OLED_CMD_SET_POWER, _check_bool_byte(on, "on"))


# ---------------------------------------------------------------------------
# INFO queries (task_id = UART_TASK_ID_INFO)
#
# These are the one place where the firmware talks *back* in payload form.
# Requests are trivial (one subcommand byte, no args); the interesting code is
# the response parsing below, which mirrors build_pin_config_reply() and
# build_fw_version_reply() in uart_bridge.c byte for byte.
#
# Note what is NOT on the wire: a response carries no subcommand/type byte, so
# a received INFO frame is not self-describing. parse_info_response() below
# classifies structurally; see its docstring.
# ---------------------------------------------------------------------------
def info_get_pin_config() -> bytes:
    """0x01 GET_PIN_CONFIG request: byte0 = subcommand, no args."""
    return struct.pack("<B", INFO_CMD_GET_PIN_CONFIG)


def info_get_fw_version() -> bytes:
    """0x02 GET_FW_VERSION request: byte0 = subcommand, no args."""
    return struct.pack("<B", INFO_CMD_GET_FW_VERSION)


class InfoResponseError(ValueError):
    """Raised when an INFO response payload does not match its wire layout."""


@dataclass(frozen=True)
class PinConfigEntry:
    """One ``{gpio, function_id}`` pair from a GET_PIN_CONFIG response."""

    gpio: int
    function_id: int

    @property
    def label(self) -> str:
        """Full human-readable description, e.g. "SPI SCLK (AD9833)"."""
        return pin_function_label(self.function_id)

    @property
    def abbrev(self) -> str:
        """Short callout text, e.g. "SCLK"."""
        return pin_function_abbrev(self.function_id)


@dataclass(frozen=True)
class FirmwareVersion:
    """Decoded GET_FW_VERSION response (build_fw_version_reply)."""

    protocol_version: int
    dirty: bool
    commit: str
    built: str

    @property
    def compatible(self) -> bool:
        """Whether this firmware speaks the same wire protocol we do.

        Equality, not >=: UART_PROTOCOL_VERSION only changes for
        wire-incompatible changes (see the bump policy in uart_task_ids.h),
        so *any* mismatch -- newer or older -- means the two sides can
        disagree about task_id/subcommand numbering or payload layouts.
        There is no meaningful "forward compatible" case to special-case.
        """
        return self.protocol_version == UART_PROTOCOL_VERSION

    def describe(self) -> str:
        """One-line summary for the status bar, e.g.
        ``FW a1b2c3d (clean) built 2026-08-06 12:34:56Z``."""
        text = (
            f"FW {self.commit or '?'} ({'dirty' if self.dirty else 'clean'}) "
            f"built {self.built or '?'}"
        )
        if not self.compatible:
            text += (
                f" -- INCOMPATIBLE protocol v{self.protocol_version} "
                f"(pc_tools speaks v{UART_PROTOCOL_VERSION})"
            )
        return text


def parse_pin_config_response(payload: bytes) -> list[PinConfigEntry]:
    """Decode a GET_PIN_CONFIG response payload.

    Layout (build_pin_config_reply)::

        byte0            entry_count (N)
        N * { u8 gpio, u8 function_id }

    Raises :class:`InfoResponseError` if the length does not match N exactly.
    """
    if len(payload) < 1:
        raise InfoResponseError("pin config response is empty")
    count = payload[0]
    expected = 1 + count * 2
    if len(payload) != expected:
        raise InfoResponseError(
            f"pin config length mismatch: entry_count={count} implies {expected} "
            f"bytes, got {len(payload)}"
        )
    return [
        PinConfigEntry(gpio=payload[1 + i * 2], function_id=payload[1 + i * 2 + 1])
        for i in range(count)
    ]


def parse_fw_version_response(payload: bytes) -> FirmwareVersion:
    """Decode a GET_FW_VERSION response payload.

    Layout (build_fw_version_reply)::

        byte0-1      UART_PROTOCOL_VERSION, u16 LE -- fixed offset across all
                     versions; read and compare this *before* trusting
                     anything else in the payload, since an incompatible
                     peer's idea of what follows may not match this layout
                     at all
        byte2        dirty flag (0 = clean, 1 = dirty or unknown)
        byte3        commit_len (N1)
        N1 bytes     git commit, ASCII, NOT null-terminated
        byte(4+N1)   datetime_len (N2)
        N2 bytes     "YYYY-MM-DD HH:MM:SSZ", ASCII, NOT null-terminated

    Raises :class:`InfoResponseError` if any length field overruns the payload
    or if trailing bytes are left over. If ``protocol_version`` doesn't match
    :data:`~uart_control.protocol.UART_PROTOCOL_VERSION`, the rest of the
    payload is parsed on a best-effort basis (it may not even be this
    layout) -- callers MUST check ``.compatible`` before trusting anything
    beyond the version number itself.
    """
    if len(payload) < 2:
        raise InfoResponseError(
            f"fw version response too short: {len(payload)} bytes (need >= 2 "
            "just to read the protocol version)"
        )
    protocol_version = payload[0] | (payload[1] << 8)

    if len(payload) < 5:
        raise InfoResponseError(
            f"fw version response too short: {len(payload)} bytes (need >= 5)"
        )
    dirty = payload[2]
    if dirty > 1:
        raise InfoResponseError(f"fw version dirty flag must be 0 or 1, got {dirty}")

    commit_len = payload[3]
    commit_end = 4 + commit_len
    if commit_end >= len(payload):
        raise InfoResponseError(
            f"fw version commit_len={commit_len} overruns {len(payload)}-byte payload"
        )
    commit = payload[4:commit_end].decode("ascii", errors="replace")

    datetime_len = payload[commit_end]
    datetime_end = commit_end + 1 + datetime_len
    if datetime_end != len(payload):
        raise InfoResponseError(
            f"fw version datetime_len={datetime_len} implies {datetime_end} bytes, "
            f"got {len(payload)}"
        )
    built = payload[commit_end + 1 : datetime_end].decode("ascii", errors="replace")

    return FirmwareVersion(
        protocol_version=protocol_version, dirty=bool(dirty), commit=commit, built=built
    )


def parse_info_response(
    payload: bytes, prefer: int | None = None
) -> "tuple[int, list[PinConfigEntry] | FirmwareVersion]":
    """Classify and decode an INFO response payload structurally.

    Returns ``(subcommand, value)`` where subcommand is
    :data:`~uart_control.protocol.INFO_CMD_GET_PIN_CONFIG` (value = list of
    :class:`PinConfigEntry`) or
    :data:`~uart_control.protocol.INFO_CMD_GET_FW_VERSION` (value =
    :class:`FirmwareVersion`).

    Why this is needed: uart_bridge.c's replies carry *no* subcommand/type
    byte -- ``build_pin_config_reply`` starts with the entry count and
    ``build_fw_version_reply`` starts with the protocol version (currently
    1, i.e. bytes ``01 00``). Both arrive as plain DATA frames on task INFO,
    so the payload alone is not self-describing and the two layouts are
    only distinguishable by whether their internal length fields add up. In
    practice they never collide for our fixed 8-entry pin table (a real
    pin-config reply is 17 bytes starting with 8; a real version reply
    starts with 1), but the caller can pass ``prefer`` -- the subcommand it
    currently has outstanding -- to settle any tie deterministically.

    Raises :class:`InfoResponseError` if the payload fits neither layout.
    """
    parsers = (
        (INFO_CMD_GET_FW_VERSION, parse_fw_version_response),
        (INFO_CMD_GET_PIN_CONFIG, parse_pin_config_response),
    )
    if prefer is not None:
        parsers = tuple(sorted(parsers, key=lambda p: p[0] != prefer))

    errors = []
    for subcommand, parser in parsers:
        try:
            return subcommand, parser(payload)
        except InfoResponseError as exc:
            errors.append(str(exc))
    raise InfoResponseError(
        f"{len(payload)}-byte INFO response matches no known layout: "
        + "; ".join(errors)
    )


# ---------------------------------------------------------------------------
# LOG (task_id = UART_TASK_ID_LOG) -- firmware -> PC only, unsolicited
# ---------------------------------------------------------------------------
_LOG_LEVEL_LETTER: dict[LogLevel, str] = {
    LogLevel.ERROR: "E",
    LogLevel.WARN: "W",
    LogLevel.INFO: "I",
    LogLevel.DEBUG: "D",
    LogLevel.VERBOSE: "V",
}


@dataclass(frozen=True)
class LogLine:
    """One decoded LOG frame: a device-side ESP_LOGx call, captured and
    forwarded by ``uart_log_bridge.c`` instead of going to the USB-Serial-JTAG
    console (see App/drivers/uart_log_bridge.c)."""

    level: LogLevel
    text: str

    @property
    def letter(self) -> str:
        """Single-character level tag, e.g. 'E', matching ESP-IDF's own
        convention -- for display prefixes that don't have room for the full
        level name."""
        return _LOG_LEVEL_LETTER.get(self.level, "?")


def parse_log_frame(payload: bytes) -> LogLine:
    """Decode a LOG task payload: byte0 = level, the rest is ASCII text.

    Unknown level bytes (a newer firmware speaking a level we don't know
    about) fall back to INFO rather than raising -- a log line with a wrong
    color/severity tag is harmless to show, unlike a DAC/AD9833 payload where
    a wrong field is a real bug in disguise.
    """
    if len(payload) < 1:
        raise ValueError("log frame is empty")
    try:
        level = LogLevel(payload[0])
    except ValueError:
        level = LogLevel.INFO
    text = payload[1:].decode("ascii", errors="replace")
    return LogLine(level=level, text=text)
