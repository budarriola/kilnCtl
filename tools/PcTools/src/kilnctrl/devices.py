"""Command payload builders and response parsers for the KilnCtrl UART tasks.

Single source of truth for the byte packing described in
``App/drivers/uart_task_ids.h`` and consumed by ``App/drivers/uart_bridge.c``.
Both the MCP server and the Tkinter GUI build payloads through here so the
layout lives in exactly one place.

Endianness: every multi-byte numeric field in a *payload* is LITTLE-endian --
uart_bridge.c ``memcpy``s straight into a native ESP32 ``float``. The protocol
*header* is big-endian; see protocol.py. That split is intentional.

Which tasks answer, and which don't:

* THERMO (1), IO (2), DISPLAY (4) and SAFETY (7) all have *query* subcommands
  whose replies arrive as separate DATA frames echoing their subcommand in
  byte0 -- see the ``parse_*_response`` functions below and the client classes
  in thermo.py / io_expander.py / display.py / safety.py.
* Every other subcommand is fire-and-forget: an ``ok`` SendResult means the
  frame reached the ESP task's inbox, NOT that the SPI/I2C transfer to the
  MAX31856 / SX1509 / ILI9488 succeeded. The firmware logs such failures
  locally, which reaches the PC only as a LOG frame (Device Console).
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass

from .protocol import (
    AUTOTUNE_CMD_ABORT,
    AUTOTUNE_CMD_ACCEPT,
    AUTOTUNE_CMD_GET_STATUS,
    AUTOTUNE_CMD_START,
    AUTOTUNE_METHOD_RELAY,
    AUTOTUNE_METHOD_STEP,
    AUTOTUNE_RULE_TL,
    AUTOTUNE_RULE_ZN,
    CONTROL_CMD_GET_ZONES,
    CONTROL_CMD_SET_ZONE_MODEL,
    CONTROL_CMD_SET_ZONE_PID,
    CONTROL_ZONE_RECORD_LEN,
    FACTORY_RESET_SCOPE_ALL,
    PROFILES_CMD_ACK_LAST_RUN,
    PROFILES_CMD_DELETE,
    PROFILES_CMD_GET,
    PROFILES_CMD_GET_EXEC_STATUS,
    PROFILES_CMD_LIST,
    PROFILES_CMD_PAUSE,
    PROFILES_CMD_RESUME,
    PROFILES_CMD_SAVE,
    PROFILES_CMD_START,
    PROFILES_CMD_STOP,
    PROFILES_SAVE_ID_NEW,
    PROFILES_SEGMENT_LEN,
    SYSTEM_CMD_FACTORY_RESET,
    UART_TASK_ID_AUTOTUNE,
    UART_TASK_ID_CONTROL,
    UART_TASK_ID_PROFILES,
    UART_TASK_ID_WIFI,
    WIFI_CMD_ADD_NETWORK,
    WIFI_CMD_FORGET,
    WIFI_CMD_GET_NETWORKS,
    WIFI_CMD_GET_STATUS,
    WIFI_CMD_SCAN,
    WIFI_CMD_SET_AP_IDENTITY,
    WIFI_CMD_SET_MODE,
    WIFI_MODE_AP,
    WIFI_MODE_HOME,
    GPIO_PROBE_CMD_READ,
    GPIO_PROBE_CMD_READ_ALL,
    GPIO_PROBE_CMD_SET_MODE,
    GPIO_PROBE_CMD_WRITE,
    GPIO_PROBE_MODE_INPUT,
    GPIO_PROBE_MODE_INPUT_PULLDOWN,
    GPIO_PROBE_MODE_INPUT_PULLUP,
    GPIO_PROBE_MODE_OUTPUT,
    DISPLAY_CMD_BLIT_BEGIN,
    DISPLAY_CMD_BLIT_DATA,
    DISPLAY_CMD_BLIT_END,
    DISPLAY_CMD_CLEAR,
    DISPLAY_CMD_DRAW_LINE,
    DISPLAY_CMD_DRAW_RECT,
    DISPLAY_CMD_FILL_RECT,
    DISPLAY_CMD_PRINT,
    DISPLAY_CMD_READ_ID,
    DISPLAY_CMD_RESET,
    DISPLAY_CMD_SET_INVERT,
    DISPLAY_CMD_SET_POWER,
    DISPLAY_CMD_SET_ROTATION,
    DISPLAY_CMD_SET_TEXT_CURSOR,
    DISPLAY_CMD_SET_TEXT_STYLE,
    DISPLAY_NATIVE_HEIGHT,
    DISPLAY_NATIVE_WIDTH,
    INFO_CMD_GET_FW_VERSION,
    INFO_CMD_GET_PIN_CONFIG,
    INFO_CMD_GET_WIFI_STATUS,
    IO_CMD_ALL_RELAYS_OFF,
    IO_CMD_READ,
    IO_CMD_SET_AUTO_REPORT,
    IO_CMD_SET_IO,
    IO_CMD_SET_IO_DIR,
    IO_CMD_SET_RELAY,
    IO_CMD_SET_RELAY_MASK,
    IO_CMD_SX_LED_DRIVER,
    IO_CMD_SX_READ_REG,
    IO_CMD_SX_RESET,
    IO_CMD_SX_SCAN,
    IO_CMD_SX_SET_DEBOUNCE,
    IO_CMD_SX_SET_DIR,
    IO_CMD_SX_SET_INT_MASK,
    IO_CMD_SX_SET_OPENDRAIN,
    IO_CMD_SX_SET_PULLUP,
    IO_CMD_SX_WRITE_REG,
    IO_DIGITAL_COUNT,
    IO_EXPANDER_PIN_COUNT,
    IO_REG_READ_MAX,
    IO_RELAY_COUNT,
    SAFETY_AGE_NEVER,
    SAFETY_CMD_GET_LINK_STATS,
    SAFETY_CMD_GET_STATUS,
    SAFETY_CMD_PING,
    SAFETY_CMD_REQUEST_ENABLE,
    SAFETY_CMD_SET_FAULT_OUT,
    SAFETY_CMD_SET_POLL_PERIOD,
    SYSTEM_CMD_RESTART_UART,
    THERMO_CHANNEL_ALL,
    THERMO_CHANNEL_COUNT,
    THERMO_CMD_CLEAR_FAULTS,
    THERMO_CMD_CONFIG_CHANNEL,
    THERMO_CMD_ONE_SHOT,
    THERMO_CMD_READ,
    THERMO_CMD_READ_FAULTS,
    THERMO_CMD_READ_REG,
    THERMO_CMD_SET_AUTO_REPORT,
    THERMO_CMD_SET_CJ_OFFSET,
    THERMO_CMD_SET_THRESHOLDS,
    THERMO_CMD_WRITE_REG,
    THERMO_FAULT_ENTRY_LEN,
    THERMO_READ_ENTRY_LEN,
    THERMO_REG_READ_MAX,
    UART_PROTO_MAX_PAYLOAD,
    UART_PROTOCOL_VERSION,
    UART_TASK_ID_DISPLAY,
    UART_TASK_ID_INFO,
    UART_TASK_ID_IO,
    UART_TASK_ID_LOG,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    UART_TASK_ID_THERMO,
    AvgMode,
    LogLevel,
    PinFunction,
    SafetyFlag,
    TcType,
    ThermoFault,
    ThermoReadFlag,
)

__all__ = [
    "UART_PROTOCOL_VERSION",
    "UART_TASK_ID_THERMO",
    "UART_TASK_ID_IO",
    "UART_TASK_ID_INFO",
    "UART_TASK_ID_DISPLAY",
    "UART_TASK_ID_LOG",
    "UART_TASK_ID_SYSTEM",
    "UART_TASK_ID_SAFETY",
    "UART_TASK_ID_CONTROL",
    "UART_TASK_ID_PROFILES",
    "UART_TASK_ID_AUTOTUNE",
    "UART_TASK_ID_WIFI",
    "system_restart_uart",
    "system_factory_reset",
    # CONTROL
    "ZoneConfig",
    "ControlResponseError",
    "control_get_zones",
    "control_set_zone_pid",
    "control_set_zone_model",
    "parse_control_response",
    # PROFILES
    "ProfileSummary",
    "ProfileSegment",
    "ProfileDetail",
    "ProfileSaveResult",
    "ZoneExecStatus",
    "ProfileExecStatus",
    "ProfilesResponseError",
    "profiles_list",
    "profiles_get",
    "profiles_save",
    "profiles_delete",
    "profiles_get_exec_status",
    "profiles_start",
    "profiles_stop",
    "profiles_pause",
    "profiles_resume",
    "profiles_ack_last_run",
    "parse_profiles_response",
    # AUTOTUNE
    "AutotuneModel",
    "AutotuneGains",
    "AutotuneRelayResult",
    "AutotuneStatus",
    "AutotuneResponseError",
    "autotune_get_status",
    "autotune_start",
    "autotune_abort",
    "autotune_accept",
    "parse_autotune_response",
    # WIFI (UART)
    "UartWifiStatus",
    "UartWifiScanEntry",
    "UartWifiSavedNetwork",
    "WifiUartResponseError",
    "wifi_uart_get_status",
    "wifi_uart_scan",
    "wifi_uart_add_network",
    "wifi_uart_set_mode",
    "wifi_uart_set_ap_identity",
    "wifi_uart_get_networks",
    "wifi_uart_forget",
    "parse_wifi_uart_response",
    # THERMO
    "THERMO_CHANNEL_COUNT",
    "THERMO_CHANNEL_ALL",
    "TcType",
    "AvgMode",
    "ThermoFault",
    "ThermoReadFlag",
    "TC_TYPE_LABELS",
    "AVG_MODE_LABELS",
    "THERMO_FAULT_LABELS",
    "ThermoReading",
    "ThermoFaultStatus",
    "ThermoRegisters",
    "ThermoResponseError",
    "thermo_config_channel",
    "thermo_set_thresholds",
    "thermo_set_cj_offset",
    "thermo_one_shot",
    "thermo_read",
    "thermo_read_faults",
    "thermo_clear_faults",
    "thermo_set_auto_report",
    "thermo_read_reg",
    "thermo_write_reg",
    "thermo_fault_labels",
    "parse_thermo_response",
    # IO
    "IO_RELAY_COUNT",
    "IO_DIGITAL_COUNT",
    "IO_EXPANDER_PIN_COUNT",
    "RELAY_DESIGNATORS",
    "DIGITAL_IO_LABELS",
    "EXPANDER_PIN_NAMES",
    "relay_label",
    "digital_io_label",
    "expander_pin_name",
    "IoState",
    "ExpanderRegisters",
    "IoResponseError",
    "io_set_relay",
    "io_set_relay_mask",
    "io_set_io",
    "io_set_io_dir",
    "io_read",
    "io_set_auto_report",
    "io_all_relays_off",
    "sx_write_reg",
    "sx_read_reg",
    "sx_set_dir",
    "sx_set_pullup",
    "sx_set_opendrain",
    "sx_set_debounce",
    "sx_set_int_mask",
    "sx_led_driver",
    "sx_reset",
    "sx_scan",
    "parse_io_response",
    # DISPLAY
    "DISPLAY_NATIVE_WIDTH",
    "DISPLAY_NATIVE_HEIGHT",
    "DISPLAY_MAX_TEXT_LEN",
    "DISPLAY_BLIT_CHUNK_PIXELS",
    "DisplayId",
    "DisplayResponseError",
    "rgb565",
    "rgb565_to_rgb",
    "display_reset",
    "display_set_power",
    "display_set_rotation",
    "display_set_invert",
    "display_clear",
    "display_fill_rect",
    "display_draw_rect",
    "display_draw_line",
    "display_set_text_cursor",
    "display_set_text_style",
    "display_print",
    "display_blit_begin",
    "display_blit_data",
    "display_blit_end",
    "display_read_id",
    "iter_blit_chunks",
    "parse_display_response",
    # SAFETY
    "SafetyFlag",
    "SAFETY_FLAG_LABELS",
    "SAFETY_AGE_NEVER",
    "SafetyStatus",
    "SafetyLinkStats",
    "SafetyResponseError",
    "safety_get_status",
    "safety_request_enable",
    "safety_ping",
    "safety_get_link_stats",
    "safety_set_poll_period",
    "safety_set_fault_out",
    "parse_safety_response",
    # INFO / LOG
    "PinFunction",
    "LogLevel",
    "LogLine",
    "parse_log_frame",
    "PIN_FUNCTION_LABELS",
    "PIN_FUNCTION_ABBREV",
    "PinConfigEntry",
    "FirmwareVersion",
    "InfoResponseError",
    "pin_function_label",
    "pin_function_abbrev",
    "info_get_pin_config",
    "info_get_fw_version",
    "parse_pin_config_response",
    "parse_fw_version_response",
    "parse_info_response",
]


#: What each PIN_FUNC_* id actually means on this board. Kept PC-side on
#: purpose (uart_task_ids.h says so): the wire carries only the id. The text
#: names the peripheral/driver that owns the pin in App/main.c and, where the
#: schematic net name is actively misleading, says what the silicon does --
#: see docs/HARDWARE.md's isolation-barrier section for SAFETY_TX/RX.
PIN_FUNCTION_LABELS: dict[int, str] = {
    PinFunction.I2C_SDA: "I2C SDA (SX1509 expander, and J2/J6 pass-through)",
    PinFunction.I2C_SCL: "I2C SCL (SX1509 expander, and J2/J6 pass-through)",
    PinFunction.UART_TX: "UART0 TX (this link, dev board USB-UART bridge)",
    PinFunction.UART_RX: "UART0 RX (this link, dev board USB-UART bridge)",
    PinFunction.SPI_SCLK: "SPI SCLK (shared: 3x MAX31856 + ILI9488)",
    PinFunction.SPI_MOSI: "SPI MOSI (shared: 3x MAX31856 + ILI9488)",
    PinFunction.SPI_CS: "SPI ~CS (CS0/CS1/CS2 thermocouples, CS3 display)",
    PinFunction.LED_HEARTBEAT: "Heartbeat LED (no MCU-driven LED on this board)",
    PinFunction.SPI_MISO: "SPI MISO (shared: 3x MAX31856 + ILI9488)",
    PinFunction.THERMO_FAULT: "MAX31856 ~FAULT input, active low",
    PinFunction.EXPANDER_IRQ: "SX1509 ~INT input, active low",
    PinFunction.EXPANDER_RST: "SX1509 ~RESET output, active low",
    # The ESP's TXD_INV/RXD_INV cancels each optocoupler's inversion, so the
    # barrier is transparent to the far end: the RP2040 uses a plain hardware
    # UART, with no PIO trickery or external inverter needed on its side.
    PinFunction.SAFETY_TX: "Safety link TX to the RP2040 (opto-isolated; ESP inverts, barrier is transparent to the Pico)",
    PinFunction.SAFETY_RX: "Safety link RX from the RP2040 (opto-isolated; ESP inverts, barrier is transparent to the Pico)",
    PinFunction.SAFETY_FAULT: "Isolated Fault line OUT to the RP2040 (we drive it)",
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
    PinFunction.SPI_MISO: "MISO",
    PinFunction.THERMO_FAULT: "TCFLT",
    PinFunction.EXPANDER_IRQ: "IOIRQ",
    PinFunction.EXPANDER_RST: "IORST",
    PinFunction.SAFETY_TX: "SFTX",
    PinFunction.SAFETY_RX: "SFRX",
    PinFunction.SAFETY_FAULT: "SFFLT",
}


def pin_function_label(function_id: int) -> str:
    """Human-readable description for a function id, tolerating unknown ids."""
    return PIN_FUNCTION_LABELS.get(function_id, f"unknown function 0x{function_id:02X}")


def pin_function_abbrev(function_id: int) -> str:
    """Short callout text for a function id, tolerating unknown ids."""
    return PIN_FUNCTION_ABBREV.get(function_id, f"0x{function_id:02X}")


# ---------------------------------------------------------------------------
# shared validation helpers
# ---------------------------------------------------------------------------
def _check_bool_byte(value: object) -> int:
    return 1 if bool(value) else 0


def _check_u8(value: int, name: str) -> int:
    value = int(value)
    if not 0 <= value <= 0xFF:
        raise ValueError(f"{name} must be 0..255, got {value}")
    return value


def _check_u16(value: int, name: str) -> int:
    value = int(value)
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"{name} must be a 16-bit value 0..0xFFFF, got {value}")
    return value


def _check_i8(value: int, name: str) -> int:
    value = int(value)
    if not -128 <= value <= 127:
        raise ValueError(f"{name} must be -128..127, got {value}")
    return value


def _check_range(value: int, low: int, high: int, name: str) -> int:
    value = int(value)
    if not low <= value <= high:
        raise ValueError(f"{name} must be {low}..{high}, got {value}")
    return value


def _check_finite(value: float, name: str) -> float:
    """Reject NaN/inf before they reach the wire.

    struct.pack("<f", float("inf")) succeeds and the firmware memcpy's it
    straight into a threshold register, so a stray inf becomes a silently
    dead trip point rather than an error anyone sees.
    """
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{name} must be a finite number, got {value}")
    return value


def _decoded_float(value: float, name: str, allow_nan: bool = False) -> float:
    """Validate a float that came *off* the wire.

    ``allow_nan`` is set only where the protocol defines NaN as meaningful
    (a thermocouple channel whose SPI read failed). An infinity is never
    meaningful in any of these fields: it is line noise that would otherwise
    propagate into a chart axis or a control loop as a real reading.
    """
    if math.isnan(value):
        if allow_nan:
            return value
        raise ValueError(f"{name} is NaN")
    if math.isinf(value):
        raise ValueError(f"{name} is infinite")
    return value


# ---------------------------------------------------------------------------
# SYSTEM (task_id = UART_TASK_ID_SYSTEM)
# ---------------------------------------------------------------------------
def system_restart_uart() -> bytes:
    """0x01 RESTART_UART request: byte0 = subcommand, no args."""
    return struct.pack("<B", SYSTEM_CMD_RESTART_UART)


def system_factory_reset(scope: int = FACTORY_RESET_SCOPE_ALL) -> bytes:
    """0x02 FACTORY_RESET: byte1=scope (0=wifi 1=kiln 2=profiles 3=all).

    Mirrors POST /api/factory_reset exactly: same per-partition NVS erase,
    same unconditional reboot ~500ms later. No reply frame either way -- the
    ACK is the only delivery confirmation, and the reboot itself (a fresh
    unsolicited GET_FW_VERSION push) is the real evidence the erase happened.
    An out-of-range scope byte is rejected by the firmware with no erase and
    no reboot, so validate it here too rather than letting a typo silently
    no-op on the device.
    """
    return struct.pack(
        "<BB", SYSTEM_CMD_FACTORY_RESET, _check_range(scope, 0, 3, "scope")
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
        if not 1 <= length <= THERMO_REG_READ_MAX:
            raise ThermoResponseError(
                f"READ_REG len={length} outside 1..{THERMO_REG_READ_MAX}"
            )
        if len(payload) != 4 + length:
            raise ThermoResponseError(
                f"READ_REG len={length} implies {4 + length} bytes, got {len(payload)}"
            )
        return subcommand, ThermoRegisters(
            channel=channel, reg=reg, data=bytes(payload[4:])
        )

    raise ThermoResponseError(f"unknown THERMO response subcommand 0x{subcommand:02X}")


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
            f"R{n}={int(self.relay(n))}" for n in range(1, IO_RELAY_COUNT + 1)
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


def parse_io_response(
    payload: bytes,
) -> "tuple[int, IoState | ExpanderRegisters | list[int]]":
    """Decode an IO query reply (or auto-report push) into ``(subcmd, value)``.

    Layouts (uart_task_ids.h)::

        READ / AUTO_REPORT: byte0=0x05, RegData u16 LE, RegDir u16 LE,
                            relay shadow u8, io levels u8, DRDY u8, flags u8
        SX_READ_REG:        byte0=0x11, reg u8, len(N) u8, N bytes
        SX_SCAN:            byte0=0x19, count(N) u8, N address bytes

    Raises :class:`IoResponseError` on anything that doesn't match.
    """
    if len(payload) < 1:
        raise IoResponseError("IO response is empty")
    subcommand = payload[0]

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


# ---------------------------------------------------------------------------
# DISPLAY -- ILI9488 480x320 on J2 (task_id = UART_TASK_ID_DISPLAY)
#
# D/C and ~RESET hang off the SX1509, so every command/data transition costs
# an I2C transfer. The firmware batches one D/C toggle per command and sends
# that command's whole payload in a single SPI transaction; the corollary
# here is that full-screen work must go through FILL_RECT/BLIT, never
# repeated small writes.
# ---------------------------------------------------------------------------
#: PRINT's payload is [subcmd][text], so the text can't exceed the protocol's
#: max payload minus that one byte.
DISPLAY_MAX_TEXT_LEN = UART_PROTO_MAX_PAYLOAD - 1

#: Pixels per BLIT_DATA frame. 128-byte payload, 1 subcommand byte, 2 bytes
#: per RGB565 pixel, and an odd trailing byte is an error -- so 126 bytes of
#: pixel data, i.e. 63 pixels, is the largest legal chunk.
DISPLAY_BLIT_CHUNK_PIXELS = (UART_PROTO_MAX_PAYLOAD - 1) // 2


class DisplayResponseError(ValueError):
    """Raised when a DISPLAY response payload does not match its wire layout."""


def rgb565(r: int, g: int, b: int) -> int:
    """Pack 8-bit R/G/B into the RGB565 u16 the wire carries."""
    r = _check_u8(r, "r")
    g = _check_u8(g, "g")
    b = _check_u8(b, "b")
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_rgb(color: int) -> "tuple[int, int, int]":
    """Inverse of :func:`rgb565` (lossy -- low bits are replicated, not restored)."""
    color = _check_u16(color, "color")
    r = (color >> 11) & 0x1F
    g = (color >> 5) & 0x3F
    b = color & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _check_coord(value: int, name: str) -> int:
    """Coordinates and sizes are u16 on the wire; the firmware clips to the
    panel, so this only enforces the field width."""
    return _check_u16(value, name)


def display_reset(hard: bool = False) -> bytes:
    """0x01 RESET: 0 = software reset command, 1 = pulse ~RESET via the expander."""
    return struct.pack("<BB", DISPLAY_CMD_RESET, _check_bool_byte(hard))


def display_set_power(on: bool) -> bytes:
    """0x02 SET_POWER: 0 sends display-off + sleep-in."""
    return struct.pack("<BB", DISPLAY_CMD_SET_POWER, _check_bool_byte(on))


def display_set_rotation(rotation: int) -> bytes:
    """0x03 SET_ROTATION: MADCTL 0-3. 0/2 portrait 320x480, 1/3 landscape 480x320."""
    return struct.pack(
        "<BB", DISPLAY_CMD_SET_ROTATION, _check_range(rotation, 0, 3, "rotation")
    )


def display_set_invert(invert: bool) -> bytes:
    """0x04 SET_INVERT: invert 0/1."""
    return struct.pack("<BB", DISPLAY_CMD_SET_INVERT, _check_bool_byte(invert))


def display_clear(color: int = 0x0000) -> bytes:
    """0x05 CLEAR: fill the whole screen with an RGB565 color."""
    return struct.pack("<BH", DISPLAY_CMD_CLEAR, _check_u16(color, "color"))


def display_fill_rect(x: int, y: int, w: int, h: int, color: int) -> bytes:
    """0x06 FILL_RECT: x, y, w, h, color -- all u16 LE."""
    return struct.pack(
        "<BHHHHH",
        DISPLAY_CMD_FILL_RECT,
        _check_coord(x, "x"),
        _check_coord(y, "y"),
        _check_coord(w, "w"),
        _check_coord(h, "h"),
        _check_u16(color, "color"),
    )


def display_draw_rect(x: int, y: int, w: int, h: int, color: int) -> bytes:
    """0x07 DRAW_RECT: same args as FILL_RECT, 1px outline."""
    return struct.pack(
        "<BHHHHH",
        DISPLAY_CMD_DRAW_RECT,
        _check_coord(x, "x"),
        _check_coord(y, "y"),
        _check_coord(w, "w"),
        _check_coord(h, "h"),
        _check_u16(color, "color"),
    )


def display_draw_line(x0: int, y0: int, x1: int, y1: int, color: int) -> bytes:
    """0x08 DRAW_LINE: x0, y0, x1, y1, color -- all u16 LE."""
    return struct.pack(
        "<BHHHHH",
        DISPLAY_CMD_DRAW_LINE,
        _check_coord(x0, "x0"),
        _check_coord(y0, "y0"),
        _check_coord(x1, "x1"),
        _check_coord(y1, "y1"),
        _check_u16(color, "color"),
    )


def display_set_text_cursor(x: int, y: int) -> bytes:
    """0x09 SET_TEXT_CURSOR: pixel coordinates of the glyph's top-left."""
    return struct.pack(
        "<BHH", DISPLAY_CMD_SET_TEXT_CURSOR, _check_coord(x, "x"), _check_coord(y, "y")
    )


def display_set_text_style(
    fg: int, bg: int = 0x0000, size: int = 1, opaque_background: bool = True
) -> bytes:
    """0x0A SET_TEXT_STYLE: fg, bg (u16 LE), size 1-8, opaque background 0/1."""
    return struct.pack(
        "<BHHBB",
        DISPLAY_CMD_SET_TEXT_STYLE,
        _check_u16(fg, "fg"),
        _check_u16(bg, "bg"),
        _check_range(size, 1, 8, "size"),
        _check_bool_byte(opaque_background),
    )


def display_print(text: str) -> bytes:
    """0x0B PRINT: ASCII at the cursor, which advances and wraps at the edge.

    Non-ASCII becomes ``?`` rather than raising -- the panel's font has no
    glyph for it either way -- but a non-string is a caller mistake and is
    named as one instead of surfacing as an AttributeError.
    """
    if not isinstance(text, str):
        raise ValueError(f"text must be a string, got {type(text).__name__}")
    encoded = text.encode("ascii", errors="replace")
    if len(encoded) > DISPLAY_MAX_TEXT_LEN:
        raise ValueError(
            f"text too long: {len(encoded)} bytes > {DISPLAY_MAX_TEXT_LEN}-byte limit"
        )
    return struct.pack("<B", DISPLAY_CMD_PRINT) + encoded


def display_blit_begin(x: int, y: int, w: int, h: int) -> bytes:
    """0x0C BLIT_BEGIN: open a pixel window; BLIT_DATA then streams into it."""
    if int(w) <= 0 or int(h) <= 0:
        raise ValueError(f"blit window must be non-empty, got {w}x{h}")
    return struct.pack(
        "<BHHHH",
        DISPLAY_CMD_BLIT_BEGIN,
        _check_coord(x, "x"),
        _check_coord(y, "y"),
        _check_coord(w, "w"),
        _check_coord(h, "h"),
    )


def display_blit_data(pixels: bytes) -> bytes:
    """0x0D BLIT_DATA: RGB565 pixels, u16 LE, row-major, continuing the window.

    An odd number of bytes is an error on the firmware side (it would split a
    pixel across two frames), so it is rejected here rather than sent.
    """
    pixels = bytes(pixels)
    if not pixels:
        raise ValueError("blit chunk is empty")
    if len(pixels) % 2:
        raise ValueError(f"blit chunk must be a whole number of pixels, got {len(pixels)} bytes")
    if len(pixels) > UART_PROTO_MAX_PAYLOAD - 1:
        raise ValueError(
            f"blit chunk too long: {len(pixels)} bytes > "
            f"{UART_PROTO_MAX_PAYLOAD - 1} (use iter_blit_chunks)"
        )
    return struct.pack("<B", DISPLAY_CMD_BLIT_DATA) + pixels


def display_blit_end() -> bytes:
    """0x0E BLIT_END: close the window.

    Sending anything other than BLIT_DATA/BLIT_END while a blit is open is an
    error and aborts the blit, so callers must not interleave other display
    commands mid-stream.
    """
    return struct.pack("<B", DISPLAY_CMD_BLIT_END)


def display_read_id() -> bytes:
    """0x0F READ_ID request (query): no args."""
    return struct.pack("<B", DISPLAY_CMD_READ_ID)


def iter_blit_chunks(pixels: bytes, chunk_pixels: int = DISPLAY_BLIT_CHUNK_PIXELS):
    """Yield BLIT_DATA payloads covering ``pixels`` (RGB565 u16 LE, row-major).

    Splitting is on pixel boundaries by construction: an odd byte count in any
    frame is a protocol error, and the receiving window just continues where
    the previous chunk stopped, so the split points themselves carry no
    meaning to the firmware.
    """
    pixels = bytes(pixels)
    if len(pixels) % 2:
        raise ValueError(f"pixel buffer must be even-length, got {len(pixels)} bytes")
    chunk_bytes = _check_range(
        chunk_pixels, 1, DISPLAY_BLIT_CHUNK_PIXELS, "chunk_pixels"
    ) * 2
    for offset in range(0, len(pixels), chunk_bytes):
        yield display_blit_data(pixels[offset : offset + chunk_bytes])


@dataclass(frozen=True)
class DisplayId:
    """Decoded READ_ID reply: the panel's RDDID bytes plus its live geometry."""

    ok: bool
    id_bytes: bytes
    width: int
    height: int

    def describe(self) -> str:
        ident = " ".join(f"{b:02X}" for b in self.id_bytes)
        status = "ok" if self.ok else "FAILED (no answer, or all-zero ID)"
        return f"ID {ident} [{status}], {self.width}x{self.height} as rotated"


def parse_display_response(payload: bytes) -> "tuple[int, DisplayId]":
    """Decode a DISPLAY query reply into ``(subcommand, value)``.

    Layout::

        READ_ID: byte0=0x0F, byte1=ok(0/1), bytes2..4 = RDDID bytes,
                 bytes5..6 = width u16 LE, bytes7..8 = height u16 LE
    """
    if len(payload) < 1:
        raise DisplayResponseError("DISPLAY response is empty")
    subcommand = payload[0]
    if subcommand != DISPLAY_CMD_READ_ID:
        raise DisplayResponseError(
            f"unknown DISPLAY response subcommand 0x{subcommand:02X}"
        )
    if len(payload) != 9:
        raise DisplayResponseError(
            f"READ_ID response must be 9 bytes, got {len(payload)}"
        )
    ok = payload[1]
    if ok > 1:
        raise DisplayResponseError(f"READ_ID ok flag must be 0 or 1, got {ok}")
    width, height = struct.unpack_from("<HH", payload, 5)
    # A panel that answered (ok=1) must have reported a real geometry; 0x0
    # from a successful read means the payload is not what it claims to be.
    if ok and (width == 0 or height == 0):
        raise DisplayResponseError(
            f"READ_ID reports ok but a {width}x{height} geometry"
        )
    return subcommand, DisplayId(
        ok=bool(ok), id_bytes=bytes(payload[2:5]), width=width, height=height
    )


# ---------------------------------------------------------------------------
# SAFETY -- the isolated link to the RP2040 (task_id = UART_TASK_ID_SAFETY)
#
# The Pico owns the safety thermocouple board on J7, the three current-sense
# channels, the E-stop input and the safety relay K4; the ESP only polls it
# across three optocouplers and caches the last good answer. The one thing
# this firmware genuinely *drives* is the Fault line (GPIO6, an output) --
# there is no hardware path for the Pico to signal the ESP outside the UART.
#
# The Pico firmware that answers this protocol does not exist in this
# repository yet, so link_up = 0 with age = 0xFFFF is the expected state
# today, not a fault to chase.
# ---------------------------------------------------------------------------
SAFETY_FLAG_LABELS: dict[int, str] = {
    SafetyFlag.LINK_UP: "link up",
    SafetyFlag.FAULT: "fault line asserted (by us)",
    SafetyFlag.ESTOP: "E-stop asserted",
    SafetyFlag.RELAY: "safety relay K4 energized",
    SafetyFlag.ENABLED: "heating enable granted",
    SafetyFlag.TEMP_VALID: "safety thermocouple valid",
}


class SafetyResponseError(ValueError):
    """Raised when a SAFETY response payload does not match its wire layout."""


def safety_get_status() -> bytes:
    """0x01 GET_STATUS request (query): no args."""
    return struct.pack("<B", SAFETY_CMD_GET_STATUS)


def safety_request_enable(enable: bool) -> bytes:
    """0x02 REQUEST_ENABLE: ask the safety processor to permit (or drop) heating.

    Advisory only -- the Pico can refuse, and its own interlocks always win.
    """
    return struct.pack("<BB", SAFETY_CMD_REQUEST_ENABLE, _check_bool_byte(enable))


def safety_ping() -> bytes:
    """0x03 PING: force an immediate poll instead of waiting for the next tick."""
    return struct.pack("<B", SAFETY_CMD_PING)


def safety_get_link_stats() -> bytes:
    """0x04 GET_LINK_STATS request (query): no args."""
    return struct.pack("<B", SAFETY_CMD_GET_LINK_STATS)


def safety_set_poll_period(period_ms: int) -> bytes:
    """0x05 SET_POLL_PERIOD: u16 LE ms, 0 = stop polling."""
    return struct.pack(
        "<BH", SAFETY_CMD_SET_POLL_PERIOD, _check_u16(period_ms, "period_ms")
    )


def safety_set_fault_out(assert_fault: bool) -> bytes:
    """0x06 SET_FAULT_OUT: drive the isolated Fault line (GPIO6).

    A manual override of a line the firmware otherwise asserts on its own
    (loss of the PC link, a thermocouple fault, the watchdog).
    """
    return struct.pack("<BB", SAFETY_CMD_SET_FAULT_OUT, _check_bool_byte(assert_fault))


@dataclass(frozen=True)
class SafetyStatus:
    """Decoded GET_STATUS reply -- the ESP's cache of the last good poll."""

    flags: SafetyFlag
    temperature_c: float
    cold_junction_c: float
    fault_status: ThermoFault
    current_a: "tuple[float, float, float]"
    age_ms: int

    @property
    def link_up(self) -> bool:
        return bool(self.flags & SafetyFlag.LINK_UP)

    @property
    def fault_asserted(self) -> bool:
        return bool(self.flags & SafetyFlag.FAULT)

    @property
    def estop(self) -> bool:
        return bool(self.flags & SafetyFlag.ESTOP)

    @property
    def relay_energized(self) -> bool:
        return bool(self.flags & SafetyFlag.RELAY)

    @property
    def enabled(self) -> bool:
        return bool(self.flags & SafetyFlag.ENABLED)

    @property
    def temp_valid(self) -> bool:
        return bool(self.flags & SafetyFlag.TEMP_VALID)

    @property
    def never_received(self) -> bool:
        """True when no status has ever arrived from the Pico.

        Today this is the normal state: the RP2040 firmware isn't written yet.
        """
        return self.age_ms == SAFETY_AGE_NEVER

    @property
    def flag_labels(self) -> "list[str]":
        return [text for bit, text in SAFETY_FLAG_LABELS.items() if self.flags & bit]

    @property
    def fault_labels(self) -> "list[str]":
        return thermo_fault_labels(int(self.fault_status))

    def describe(self) -> str:
        if self.never_received:
            age = "never received"
        else:
            age = f"{self.age_ms} ms old"
        set_flags = self.flag_labels or ["no flags set"]
        temp = (
            f"{self.temperature_c:.2f} C (CJ {self.cold_junction_c:.2f} C)"
            if self.temp_valid
            else "safety TC invalid"
        )
        currents = ", ".join(f"{a:.2f} A" for a in self.current_a)
        return f"{'; '.join(set_flags)} | {temp} | currents {currents} | {age}"


@dataclass(frozen=True)
class SafetyLinkStats:
    """Decoded GET_LINK_STATS reply -- the ESP's own counters for the isolated
    UART, so these move even while the far side is silent."""

    frames_sent: int
    frames_received: int
    crc_errors: int
    timeouts: int
    poll_period_ms: int

    def describe(self) -> str:
        return (
            f"sent {self.frames_sent}, received {self.frames_received}, "
            f"crc/framing errors {self.crc_errors}, timeouts {self.timeouts}, "
            f"poll period {self.poll_period_ms} ms"
        )


def parse_safety_response(
    payload: bytes,
) -> "tuple[int, SafetyStatus | SafetyLinkStats]":
    """Decode a SAFETY query reply into ``(subcommand, value)``.

    Layouts (uart_task_ids.h)::

        GET_STATUS:     byte0=0x01, flags u8, tc f32, cj f32, SR u8,
                        current1..3 f32, age u16 LE                (25 bytes)
        GET_LINK_STATS: byte0=0x04, sent u32, received u32, crc u32,
                        timeouts u32, poll period u16 LE           (19 bytes)
    """
    if len(payload) < 1:
        raise SafetyResponseError("SAFETY response is empty")
    subcommand = payload[0]

    if subcommand == SAFETY_CMD_GET_STATUS:
        if len(payload) != 25:
            raise SafetyResponseError(
                f"GET_STATUS response must be 25 bytes, got {len(payload)}"
            )
        (
            flags,
            temperature,
            cold_junction,
            fault_status,
            current1,
            current2,
            current3,
            age_ms,
        ) = struct.unpack_from("<BffBfffH", payload, 1)
        # NaN is how "no valid reading" arrives here (the Pico's own TC can be
        # invalid); an infinity is not a value this protocol can carry, and a
        # bogus current reading is exactly the kind of thing that must not
        # reach a safety readout looking legitimate.
        try:
            temperature = _decoded_float(temperature, "safety temperature", allow_nan=True)
            cold_junction = _decoded_float(
                cold_junction, "safety cold junction", allow_nan=True
            )
            current1 = _decoded_float(current1, "current sense 1", allow_nan=True)
            current2 = _decoded_float(current2, "current sense 2", allow_nan=True)
            current3 = _decoded_float(current3, "current sense 3", allow_nan=True)
        except ValueError as exc:
            raise SafetyResponseError(str(exc)) from exc
        return subcommand, SafetyStatus(
            flags=SafetyFlag(flags & 0x3F),
            temperature_c=temperature,
            cold_junction_c=cold_junction,
            fault_status=ThermoFault(fault_status),
            current_a=(current1, current2, current3),
            age_ms=age_ms,
        )

    if subcommand == SAFETY_CMD_GET_LINK_STATS:
        if len(payload) != 19:
            raise SafetyResponseError(
                f"GET_LINK_STATS response must be 19 bytes, got {len(payload)}"
            )
        sent, received, crc_errors, timeouts, poll_period = struct.unpack_from(
            "<IIIIH", payload, 1
        )
        return subcommand, SafetyLinkStats(
            frames_sent=sent,
            frames_received=received,
            crc_errors=crc_errors,
            timeouts=timeouts,
            poll_period_ms=poll_period,
        )

    raise SafetyResponseError(f"unknown SAFETY response subcommand 0x{subcommand:02X}")


# ---------------------------------------------------------------------------
# INFO queries (task_id = UART_TASK_ID_INFO)
#
# These are the one place where the firmware talks back with no subcommand
# byte at all. Requests are trivial (one subcommand byte, no args); the
# interesting code is the response parsing below, which mirrors
# build_pin_config_reply() and build_fw_version_reply() in uart_bridge.c byte
# for byte.
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


def info_get_wifi_status() -> bytes:
    """0x03 GET_WIFI_STATUS request: byte0 = subcommand, no args."""
    return struct.pack("<B", INFO_CMD_GET_WIFI_STATUS)


class InfoResponseError(ValueError):
    """Raised when an INFO response payload does not match its wire layout."""


@dataclass(frozen=True)
class PinConfigEntry:
    """One ``{gpio, function_id}`` pair from a GET_PIN_CONFIG response."""

    gpio: int
    function_id: int

    @property
    def label(self) -> str:
        """Full human-readable description, e.g. "SPI MISO (shared: ...)"."""
        return pin_function_label(self.function_id)

    @property
    def abbrev(self) -> str:
        """Short callout text, e.g. "MISO"."""
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
        disagree about task_id/subcommand numbering or payload layouts. v1
        firmware is the unit-test fixture, where task 1 is a DAC rather than
        three thermocouples: there is no meaningful "forward compatible" case
        to special-case.
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


@dataclass(frozen=True)
class WifiStatus:
    """Decoded GET_WIFI_STATUS response (build_wifi_status_reply).

    ``ip`` is empty whenever ``connected`` is False -- there is nothing to
    report, not a malformed reply (mirrors wifi_prov_get_sta_ip's own
    "empty string, not a failure" convention).
    """

    connected: bool
    ip: str

    @property
    def url(self) -> "str | None":
        """``http://<ip>/`` for opening the dashboard, or None if not
        connected."""
        return f"http://{self.ip}/" if self.connected and self.ip else None


def parse_wifi_status_response(payload: bytes) -> WifiStatus:
    """Decode a GET_WIFI_STATUS response payload.

    Layout (build_wifi_status_reply)::

        byte0    connected flag (0/1)
        byte1    ip_len (N, 0 if not connected)
        N bytes  station IP, dotted-quad ASCII, NOT null-terminated

    Raises :class:`InfoResponseError` if the length does not match N exactly.
    """
    if len(payload) < 2:
        raise InfoResponseError(
            f"wifi status response too short: {len(payload)} bytes (need >= 2)"
        )
    connected = payload[0]
    if connected > 1:
        raise InfoResponseError(f"wifi status connected flag must be 0 or 1, got {connected}")
    ip_len = payload[1]
    expected = 2 + ip_len
    if len(payload) != expected:
        raise InfoResponseError(
            f"wifi status length mismatch: ip_len={ip_len} implies {expected} bytes, "
            f"got {len(payload)}"
        )
    ip = payload[2:expected].decode("ascii", errors="replace")
    return WifiStatus(connected=bool(connected), ip=ip)


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
    :data:`~kilnctrl.protocol.UART_PROTOCOL_VERSION`, the rest of the
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
) -> "tuple[int, list[PinConfigEntry] | FirmwareVersion | WifiStatus]":
    """Classify and decode an INFO response payload structurally.

    Returns ``(subcommand, value)`` where subcommand is
    :data:`~kilnctrl.protocol.INFO_CMD_GET_PIN_CONFIG` (value = list of
    :class:`PinConfigEntry`),
    :data:`~kilnctrl.protocol.INFO_CMD_GET_FW_VERSION` (value =
    :class:`FirmwareVersion`), or
    :data:`~kilnctrl.protocol.INFO_CMD_GET_WIFI_STATUS` (value =
    :class:`WifiStatus`).

    Why this is needed: uart_bridge.c's replies carry *no* subcommand/type
    byte -- ``build_pin_config_reply`` starts with the entry count and
    ``build_fw_version_reply`` starts with the protocol version (currently
    2, i.e. bytes ``02 00``). Both arrive as plain DATA frames on task INFO,
    so the payload alone is not self-describing and the layouts are only
    distinguishable by whether their internal length fields add up. A real
    version reply starts with 2 and is far too short to be a 2-entry pin
    config (which would need 5 bytes and a plausible function id), but the
    caller can pass ``prefer`` -- the subcommand it currently has
    outstanding -- to settle any tie deterministically. GET_WIFI_STATUS is
    never sent unsolicited, so it only ever gets classified via ``prefer``.

    Raises :class:`InfoResponseError` if the payload fits no known layout.
    """
    parsers = (
        (INFO_CMD_GET_FW_VERSION, parse_fw_version_response),
        (INFO_CMD_GET_PIN_CONFIG, parse_pin_config_response),
        (INFO_CMD_GET_WIFI_STATUS, parse_wifi_status_response),
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
    color/severity tag is harmless to show, unlike a THERMO/IO payload where
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
) -> "tuple[int, tuple[int, int, list[ZoneConfig]] | bool]":
    """Decode a CONTROL reply into ``(subcmd, value)``.

    GET_ZONES value is ``(thermo_count, relay_count, [ZoneConfig, ...])``;
    SET_ZONE_PID/SET_ZONE_MODEL value is a plain ``ok`` bool.
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

    if subcommand in (CONTROL_CMD_SET_ZONE_PID, CONTROL_CMD_SET_ZONE_MODEL):
        if len(payload) < 2:
            raise ControlResponseError("SET_ZONE_* response is missing its ok byte")
        return subcommand, bool(payload[1])

    raise ControlResponseError(f"unknown CONTROL response subcommand 0x{subcommand:02X}")


# ---------------------------------------------------------------------------
# PROFILES -- fire profile CRUD + execution control
# (task_id = UART_TASK_ID_PROFILES)
# ---------------------------------------------------------------------------
class ProfilesResponseError(ValueError):
    """Raised when a PROFILES response payload does not match its wire layout."""


@dataclass(frozen=True)
class ProfileSegment:
    target_c: float
    ramp_c_per_hr: float
    dwell_min: int


@dataclass(frozen=True)
class ProfileSummary:
    """One entry from a LIST reply."""

    id: int
    name: str
    zone_mask: int
    segment_count: int


@dataclass(frozen=True)
class ProfileDetail:
    """A GET reply's full profile (segments included)."""

    id: int
    name: str
    zone_mask: int
    segments: "list[ProfileSegment]"


@dataclass(frozen=True)
class ProfileSaveResult:
    ok: bool
    id: "Optional[int]" = None
    warning_count: int = 0
    error: str = ""


@dataclass(frozen=True)
class ZoneExecStatus:
    zone: int
    control_mode: int
    actual_c: float
    actual_valid: bool
    duty: float
    relay_commanded_on: bool
    faulted: bool
    fault_guard: int


@dataclass(frozen=True)
class ProfileExecStatus:
    state: int
    profile_id: int
    name: str
    zone_mask: int
    segment_index: int
    segment_count: int
    dwelling: bool
    target_c: float
    segment_elapsed_s: int
    dwell_remaining_s: int
    ramp_lock_held: bool
    ramp_lock_lagging_mask: int
    fault_guard: int
    zones: "list[ZoneExecStatus]"

    #: profile_exec_state_t values.
    STATE_NAMES = {0: "idle", 1: "running", 2: "paused", 3: "done", 4: "faulted"}

    @property
    def state_name(self) -> str:
        return self.STATE_NAMES.get(self.state, f"unknown({self.state})")


def _pack_str8(text: str, max_len: int, name: str) -> bytes:
    encoded = text.encode("ascii", errors="replace")
    if len(encoded) > max_len:
        raise ValueError(f"{name} too long: {len(encoded)} bytes > {max_len}")
    return struct.pack("<B", len(encoded)) + encoded


def profiles_list() -> bytes:
    """0x01 LIST request (query): no args."""
    return struct.pack("<B", PROFILES_CMD_LIST)


def profiles_get(profile_id: int) -> bytes:
    """0x02 GET request (query): id 0-7."""
    return struct.pack("<BB", PROFILES_CMD_GET, _check_range(profile_id, 0, 7, "profile_id"))


def profiles_save(
    profile_id: int, name: str, zone_mask: int, segments: "list[ProfileSegment]"
) -> bytes:
    """0x03 SAVE request: id (or PROFILES_SAVE_ID_NEW), name, zone_mask,
    segment_count, then 12 bytes/segment (target_c f32, ramp f32, dwell u32).

    Replies ok/fail (see :func:`parse_profiles_response`).
    """
    if profile_id != PROFILES_SAVE_ID_NEW:
        _check_range(profile_id, 0, 7, "profile_id")
    if not 1 <= len(segments) <= 12:
        raise ValueError(f"segment count must be 1..12, got {len(segments)}")
    body = struct.pack("<BB", PROFILES_CMD_SAVE, profile_id)
    body += _pack_str8(name, 15, "name")
    body += struct.pack("<BB", _check_u8(zone_mask, "zone_mask"), len(segments))
    for i, seg in enumerate(segments):
        body += struct.pack(
            "<ffI",
            _check_finite(seg.target_c, f"segment {i} target_c"),
            _check_finite(seg.ramp_c_per_hr, f"segment {i} ramp_c_per_hr"),
            _check_range(seg.dwell_min, 0, 0xFFFFFFFF, f"segment {i} dwell_min"),
        )
    return body


def profiles_delete(profile_id: int) -> bytes:
    """0x04 DELETE request: id 0-7. Replies ok/fail."""
    return struct.pack("<BB", PROFILES_CMD_DELETE, _check_range(profile_id, 0, 7, "profile_id"))


def profiles_get_exec_status() -> bytes:
    """0x05 GET_EXEC_STATUS request (query): no args."""
    return struct.pack("<B", PROFILES_CMD_GET_EXEC_STATUS)


def profiles_start(profile_id: int) -> bytes:
    """0x06 START request: id 0-7. Replies ok/fail (+ error text on failure)."""
    return struct.pack("<BB", PROFILES_CMD_START, _check_range(profile_id, 0, 7, "profile_id"))


def profiles_stop() -> bytes:
    """0x07 STOP request: no args. Always replies ok."""
    return struct.pack("<B", PROFILES_CMD_STOP)


def profiles_pause() -> bytes:
    """0x08 PAUSE request: no args. Replies ok/fail."""
    return struct.pack("<B", PROFILES_CMD_PAUSE)


def profiles_resume() -> bytes:
    """0x09 RESUME request: no args. Replies ok/fail."""
    return struct.pack("<B", PROFILES_CMD_RESUME)


def profiles_ack_last_run() -> bytes:
    """0x0A ACK_LAST_RUN request: no args. Replies ok/fail."""
    return struct.pack("<B", PROFILES_CMD_ACK_LAST_RUN)


def parse_profiles_response(payload: bytes) -> "tuple[int, object]":
    """Decode a PROFILES reply into ``(subcmd, value)``.

    Layouts (uart_task_ids.h) -- see the module docstring cross-reference for
    the byte-level offsets; this mirrors them field for field.
    """
    if len(payload) < 1:
        raise ProfilesResponseError("PROFILES response is empty")
    subcommand = payload[0]

    if subcommand == PROFILES_CMD_LIST:
        if len(payload) < 2:
            raise ProfilesResponseError("LIST response is missing its count byte")
        count = payload[1]
        offset = 2
        summaries = []
        for i in range(count):
            if offset + 2 > len(payload):
                raise ProfilesResponseError(f"LIST entry {i} header truncated")
            pid = payload[offset]
            name_len = payload[offset + 1]
            name_start = offset + 2
            name_end = name_start + name_len
            if name_end + 2 > len(payload):
                raise ProfilesResponseError(f"LIST entry {i} truncated")
            name = payload[name_start:name_end].decode("ascii", errors="replace")
            zone_mask = payload[name_end]
            segment_count = payload[name_end + 1]
            summaries.append(
                ProfileSummary(id=pid, name=name, zone_mask=zone_mask, segment_count=segment_count)
            )
            offset = name_end + 2
        if offset != len(payload):
            raise ProfilesResponseError(
                f"LIST response has {len(payload) - offset} trailing bytes"
            )
        return subcommand, summaries

    if subcommand == PROFILES_CMD_GET:
        if len(payload) < 2:
            raise ProfilesResponseError("GET response is missing its ok byte")
        ok = payload[1]
        if not ok:
            return subcommand, None
        if len(payload) < 4:
            raise ProfilesResponseError("GET response header is truncated")
        pid = payload[2]
        name_len = payload[3]
        name_start = 4
        name_end = name_start + name_len
        if name_end + 2 > len(payload):
            raise ProfilesResponseError("GET response name/header truncated")
        name = payload[name_start:name_end].decode("ascii", errors="replace")
        zone_mask = payload[name_end]
        segment_count = payload[name_end + 1]
        seg_start = name_end + 2
        expected = seg_start + segment_count * PROFILES_SEGMENT_LEN
        if len(payload) != expected:
            raise ProfilesResponseError(
                f"GET segment_count={segment_count} implies {expected} bytes, "
                f"got {len(payload)}"
            )
        segments = []
        for i in range(segment_count):
            off = seg_start + i * PROFILES_SEGMENT_LEN
            target_c, ramp, dwell = struct.unpack_from("<ffI", payload, off)
            segments.append(
                ProfileSegment(target_c=target_c, ramp_c_per_hr=ramp, dwell_min=dwell)
            )
        return subcommand, ProfileDetail(
            id=pid, name=name, zone_mask=zone_mask, segments=segments
        )

    if subcommand == PROFILES_CMD_SAVE:
        if len(payload) < 2:
            raise ProfilesResponseError("SAVE response is missing its ok byte")
        ok = payload[1]
        if ok:
            if len(payload) < 4:
                raise ProfilesResponseError("SAVE ok response header is truncated")
            return subcommand, ProfileSaveResult(
                ok=True, id=payload[2], warning_count=payload[3]
            )
        err_len = payload[2] if len(payload) > 2 else 0
        error = ""
        if err_len:
            error = payload[3 : 3 + err_len].decode("ascii", errors="replace")
        return subcommand, ProfileSaveResult(ok=False, error=error)

    if subcommand in (
        PROFILES_CMD_DELETE,
        PROFILES_CMD_PAUSE,
        PROFILES_CMD_RESUME,
        PROFILES_CMD_ACK_LAST_RUN,
        PROFILES_CMD_STOP,
    ):
        if len(payload) < 2:
            raise ProfilesResponseError("response is missing its ok byte")
        return subcommand, bool(payload[1])

    if subcommand == PROFILES_CMD_START:
        if len(payload) < 2:
            raise ProfilesResponseError("START response is missing its ok byte")
        ok = payload[1]
        if ok:
            return subcommand, ProfileSaveResult(ok=True)
        err_len = payload[2] if len(payload) > 2 else 0
        error = ""
        if err_len:
            error = payload[3 : 3 + err_len].decode("ascii", errors="replace")
        return subcommand, ProfileSaveResult(ok=False, error=error)

    if subcommand == PROFILES_CMD_GET_EXEC_STATUS:
        if len(payload) < 4:
            raise ProfilesResponseError("GET_EXEC_STATUS response header is truncated")
        state = payload[1]
        profile_id = payload[2]
        name_len = payload[3]
        name_start = 4
        name_end = name_start + name_len
        # fixed-width block after the name: zone_mask(1) segment_index(1)
        # segment_count(1) dwelling(1) target_c(4) segment_elapsed_s(4)
        # dwell_remaining_s(4) ramp_lock_held(1) ramp_lock_lagging_mask(1)
        # fault_guard(1) zone_count(1) = 20 bytes
        fixed_end = name_end + 20
        if fixed_end > len(payload):
            raise ProfilesResponseError("GET_EXEC_STATUS fixed block truncated")
        name = payload[name_start:name_end].decode("ascii", errors="replace")
        zone_mask = payload[name_end]
        segment_index = payload[name_end + 1]
        segment_count = payload[name_end + 2]
        dwelling = bool(payload[name_end + 3])
        target_c, segment_elapsed_s, dwell_remaining_s = struct.unpack_from(
            "<fII", payload, name_end + 4
        )
        ramp_lock_held = bool(payload[name_end + 16])
        ramp_lock_lagging_mask = payload[name_end + 17]
        fault_guard = payload[name_end + 18]
        zone_count = payload[name_end + 19]
        zones_start = fixed_end
        expected = zones_start + zone_count * 14
        if len(payload) != expected:
            raise ProfilesResponseError(
                f"GET_EXEC_STATUS zone_count={zone_count} implies {expected} bytes, "
                f"got {len(payload)}"
            )
        zones = []
        for i in range(zone_count):
            off = zones_start + i * 14
            zone, control_mode = payload[off], payload[off + 1]
            actual_c = struct.unpack_from("<f", payload, off + 2)[0]
            actual_valid = bool(payload[off + 6])
            duty = struct.unpack_from("<f", payload, off + 7)[0]
            relay_on = bool(payload[off + 11])
            faulted = bool(payload[off + 12])
            fg = payload[off + 13]
            zones.append(
                ZoneExecStatus(
                    zone=zone,
                    control_mode=control_mode,
                    actual_c=actual_c,
                    actual_valid=actual_valid,
                    duty=duty,
                    relay_commanded_on=relay_on,
                    faulted=faulted,
                    fault_guard=fg,
                )
            )
        return subcommand, ProfileExecStatus(
            state=state,
            profile_id=profile_id,
            name=name,
            zone_mask=zone_mask,
            segment_index=segment_index,
            segment_count=segment_count,
            dwelling=dwelling,
            target_c=target_c,
            segment_elapsed_s=segment_elapsed_s,
            dwell_remaining_s=dwell_remaining_s,
            ramp_lock_held=ramp_lock_held,
            ramp_lock_lagging_mask=ramp_lock_lagging_mask,
            fault_guard=fault_guard,
            zones=zones,
        )

    raise ProfilesResponseError(f"unknown PROFILES response subcommand 0x{subcommand:02X}")


# ---------------------------------------------------------------------------
# AUTOTUNE -- PID autotune (step/relay methods)
# (task_id = UART_TASK_ID_AUTOTUNE)
# ---------------------------------------------------------------------------
class AutotuneResponseError(ValueError):
    """Raised when an AUTOTUNE response payload does not match its wire layout."""


@dataclass(frozen=True)
class AutotuneModel:
    k_gain_c_per_duty: float
    tau_s: float
    dead_time_s: float


@dataclass(frozen=True)
class AutotuneGains:
    kp: float
    ki: float
    kd: float
    rule: int


@dataclass(frozen=True)
class AutotuneRelayResult:
    ku: float
    tu_s: float
    amplitude_c: float


@dataclass(frozen=True)
class AutotuneStatus:
    state: int
    method: int
    zone: int
    elapsed_s: int
    sample_count: int
    actual_c: float
    actual_valid: bool
    duty: float
    model_valid: bool
    model: AutotuneModel
    proposed_gains: AutotuneGains
    predicted_max_ramp_c_per_hr: float
    relay_valid: bool
    relay: AutotuneRelayResult
    abort_reason: str

    STATE_NAMES = {
        0: "idle", 1: "settling", 2: "stepping", 3: "relay_approach",
        4: "relay_cycling", 5: "done", 6: "aborted",
    }

    @property
    def state_name(self) -> str:
        return self.STATE_NAMES.get(self.state, f"unknown({self.state})")


def autotune_get_status() -> bytes:
    """0x01 GET_STATUS request (query): no args."""
    return struct.pack("<B", AUTOTUNE_CMD_GET_STATUS)


def autotune_start(
    zone: int,
    method: int,
    step_duty_or_setpoint_c: float,
    relay_d: float = -1.0,
    relay_h_c: float = -1.0,
    rule: int = AUTOTUNE_RULE_TL,
) -> bytes:
    """0x02 START: zone, method(0=step,1=relay), setpoint/duty, relay_d,
    relay_h_c, rule -- always sends all 16 argument bytes (fields the
    selected method doesn't use are ignored, same as the HTTP form's
    server-side defaulting). ``relay_d``/``relay_h_c`` <=0 means "engine
    default". Replies ok/fail (+ error text on failure).
    """
    if method not in (AUTOTUNE_METHOD_STEP, AUTOTUNE_METHOD_RELAY):
        raise ValueError(f"method must be 0 (step) or 1 (relay), got {method}")
    if rule not in (AUTOTUNE_RULE_TL, AUTOTUNE_RULE_ZN):
        raise ValueError(f"rule must be 0 (tyreus-luyben) or 1 (ziegler-nichols), got {rule}")
    return struct.pack(
        "<BBBfffB",
        AUTOTUNE_CMD_START,
        _check_u8(zone, "zone"),
        method,
        _check_finite(step_duty_or_setpoint_c, "step_duty_or_setpoint_c"),
        _check_finite(relay_d, "relay_d"),
        _check_finite(relay_h_c, "relay_h_c"),
        rule,
    )


def autotune_abort() -> bytes:
    """0x03 ABORT: no args. Always replies ok."""
    return struct.pack("<B", AUTOTUNE_CMD_ABORT)


def autotune_accept() -> bytes:
    """0x04 ACCEPT: no args. Replies ok/fail."""
    return struct.pack("<B", AUTOTUNE_CMD_ACCEPT)


def parse_autotune_response(payload: bytes) -> "tuple[int, object]":
    """Decode an AUTOTUNE reply into ``(subcmd, value)``."""
    if len(payload) < 1:
        raise AutotuneResponseError("AUTOTUNE response is empty")
    subcommand = payload[0]

    if subcommand == AUTOTUNE_CMD_GET_STATUS:
        if len(payload) < 63:
            raise AutotuneResponseError(
                f"GET_STATUS response must be >= 63 bytes, got {len(payload)}"
            )
        state, method, zone = payload[1], payload[2], payload[3]
        elapsed_s = struct.unpack_from("<I", payload, 4)[0]
        sample_count = struct.unpack_from("<H", payload, 8)[0]
        actual_c = struct.unpack_from("<f", payload, 10)[0]
        actual_valid = bool(payload[14])
        duty = struct.unpack_from("<f", payload, 15)[0]
        model_valid = bool(payload[19])
        k_gain, tau_s, dead_time_s = struct.unpack_from("<fff", payload, 20)
        kp, ki, kd = struct.unpack_from("<fff", payload, 32)
        rule = payload[44]
        predicted_max_ramp = struct.unpack_from("<f", payload, 45)[0]
        relay_valid = bool(payload[49])
        ku, tu_s, amplitude_c = struct.unpack_from("<fff", payload, 50)
        abort_len = payload[62]
        abort_reason = ""
        if abort_len:
            if len(payload) < 63 + abort_len:
                raise AutotuneResponseError("GET_STATUS abort_reason truncated")
            abort_reason = payload[63 : 63 + abort_len].decode("ascii", errors="replace")
        return subcommand, AutotuneStatus(
            state=state,
            method=method,
            zone=zone,
            elapsed_s=elapsed_s,
            sample_count=sample_count,
            actual_c=actual_c,
            actual_valid=actual_valid,
            duty=duty,
            model_valid=model_valid,
            model=AutotuneModel(k_gain_c_per_duty=k_gain, tau_s=tau_s, dead_time_s=dead_time_s),
            proposed_gains=AutotuneGains(kp=kp, ki=ki, kd=kd, rule=rule),
            predicted_max_ramp_c_per_hr=predicted_max_ramp,
            relay_valid=relay_valid,
            relay=AutotuneRelayResult(ku=ku, tu_s=tu_s, amplitude_c=amplitude_c),
            abort_reason=abort_reason,
        )

    if subcommand == AUTOTUNE_CMD_START:
        if len(payload) < 2:
            raise AutotuneResponseError("START response is missing its ok byte")
        ok = payload[1]
        if ok:
            return subcommand, (True, "")
        err_len = payload[2] if len(payload) > 2 else 0
        error = ""
        if err_len:
            error = payload[3 : 3 + err_len].decode("ascii", errors="replace")
        return subcommand, (False, error)

    if subcommand in (AUTOTUNE_CMD_ABORT, AUTOTUNE_CMD_ACCEPT):
        if len(payload) < 2:
            raise AutotuneResponseError("response is missing its ok byte")
        return subcommand, bool(payload[1])

    raise AutotuneResponseError(f"unknown AUTOTUNE response subcommand 0x{subcommand:02X}")


# ---------------------------------------------------------------------------
# WIFI (UART) -- status/scan/provision/forget, mirrors wifi_provision_http.c
# (task_id = UART_TASK_ID_WIFI)
#
# Exists so Wi-Fi can be provisioned over a link that works even when Wi-Fi
# itself is down or unconfigured -- never touches kiln_io/relay_authority/
# safety_link.
# ---------------------------------------------------------------------------
class WifiUartResponseError(ValueError):
    """Raised when a WIFI (UART) response payload does not match its wire layout."""


@dataclass(frozen=True)
class UartWifiStatus:
    mode: int
    state: int
    sta_connected: bool
    ssid: str
    ap_ssid: str
    ap_password: str
    sta_ip: str
    sta_rssi: int
    ap_clients: int

    @property
    def mode_name(self) -> str:
        return "ap" if self.mode == WIFI_MODE_AP else "home"


@dataclass(frozen=True)
class UartWifiScanEntry:
    ssid: str
    rssi: int
    secure: bool


@dataclass(frozen=True)
class UartWifiSavedNetwork:
    ssid: str
    saved: bool
    in_range: bool
    rssi: int
    secure: bool
    connected: bool


def wifi_uart_get_status() -> bytes:
    """0x01 GET_STATUS request (query): no args."""
    return struct.pack("<B", WIFI_CMD_GET_STATUS)


def wifi_uart_scan() -> bytes:
    """0x02 SCAN request (query): no args. Capped at 6 entries + truncated flag."""
    return struct.pack("<B", WIFI_CMD_SCAN)


def wifi_uart_add_network(ssid: str, password: str = "") -> bytes:
    """0x03 ADD_NETWORK: ssid_len+ssid, password_len+password. Replies ok/fail."""
    return (
        struct.pack("<B", WIFI_CMD_ADD_NETWORK)
        + _pack_str8(ssid, 32, "ssid")
        + _pack_str8(password, 64, "password")
    )


def wifi_uart_set_mode(mode: int) -> bytes:
    """0x04 SET_MODE: mode(0=home,1=ap). Replies ok/fail."""
    if mode not in (WIFI_MODE_HOME, WIFI_MODE_AP):
        raise ValueError(f"mode must be 0 (home) or 1 (ap), got {mode}")
    return struct.pack("<BB", WIFI_CMD_SET_MODE, mode)


def wifi_uart_set_ap_identity(
    ap_ssid: "Optional[str]" = None, ap_password: "Optional[str]" = None
) -> bytes:
    """0x05 SET_AP_IDENTITY: has_ssid[+ssid], has_password[+password].

    Either field may be omitted (None) to leave it unchanged. Replies ok/fail.
    """
    body = struct.pack("<B", WIFI_CMD_SET_AP_IDENTITY)
    if ap_ssid is not None:
        body += struct.pack("<B", 1) + _pack_str8(ap_ssid, 32, "ap_ssid")
    else:
        body += struct.pack("<B", 0)
    if ap_password is not None:
        body += struct.pack("<B", 1) + _pack_str8(ap_password, 63, "ap_password")
    else:
        body += struct.pack("<B", 0)
    return body


def wifi_uart_get_networks() -> bytes:
    """0x06 GET_NETWORKS request (query): no args. Capped at 5 entries."""
    return struct.pack("<B", WIFI_CMD_GET_NETWORKS)


def wifi_uart_forget(ssid: str) -> bytes:
    """0x07 FORGET: ssid_len+ssid. Replies ok/fail."""
    return struct.pack("<B", WIFI_CMD_FORGET) + _pack_str8(ssid, 32, "ssid")


def _unpack_str8(payload: bytes, offset: int, name: str) -> "tuple[str, int]":
    if offset >= len(payload):
        raise WifiUartResponseError(f"{name}: length byte out of range")
    length = payload[offset]
    start = offset + 1
    end = start + length
    if end > len(payload):
        raise WifiUartResponseError(f"{name}: {length}-byte string overruns payload")
    return payload[start:end].decode("ascii", errors="replace"), end


def parse_wifi_uart_response(payload: bytes) -> "tuple[int, object]":
    """Decode a WIFI (UART) reply into ``(subcmd, value)``."""
    if len(payload) < 1:
        raise WifiUartResponseError("WIFI response is empty")
    subcommand = payload[0]

    if subcommand == WIFI_CMD_GET_STATUS:
        if len(payload) < 4:
            raise WifiUartResponseError("GET_STATUS response header is truncated")
        mode, state, sta_connected = payload[1], payload[2], bool(payload[3])
        offset = 4
        ssid, offset = _unpack_str8(payload, offset, "ssid")
        ap_ssid, offset = _unpack_str8(payload, offset, "ap_ssid")
        ap_password, offset = _unpack_str8(payload, offset, "ap_password")
        sta_ip, offset = _unpack_str8(payload, offset, "sta_ip")
        if offset + 2 > len(payload):
            raise WifiUartResponseError("GET_STATUS response tail is truncated")
        sta_rssi = struct.unpack_from("<b", payload, offset)[0]
        ap_clients = payload[offset + 1]
        return subcommand, UartWifiStatus(
            mode=mode,
            state=state,
            sta_connected=sta_connected,
            ssid=ssid,
            ap_ssid=ap_ssid,
            ap_password=ap_password,
            sta_ip=sta_ip,
            sta_rssi=sta_rssi,
            ap_clients=ap_clients,
        )

    if subcommand == WIFI_CMD_SCAN:
        if len(payload) < 3:
            raise WifiUartResponseError("SCAN response header is truncated")
        count, truncated = payload[1], bool(payload[2])
        offset = 3
        entries = []
        for i in range(count):
            ssid, offset = _unpack_str8(payload, offset, f"SCAN entry {i} ssid")
            if offset + 2 > len(payload):
                raise WifiUartResponseError(f"SCAN entry {i} rssi/secure truncated")
            rssi = struct.unpack_from("<b", payload, offset)[0]
            secure = bool(payload[offset + 1])
            offset += 2
            entries.append(UartWifiScanEntry(ssid=ssid, rssi=rssi, secure=secure))
        return subcommand, (entries, truncated)

    if subcommand == WIFI_CMD_GET_NETWORKS:
        if len(payload) < 3:
            raise WifiUartResponseError("GET_NETWORKS response header is truncated")
        count, truncated = payload[1], bool(payload[2])
        offset = 3
        entries = []
        for i in range(count):
            ssid, offset = _unpack_str8(payload, offset, f"network {i} ssid")
            if offset + 4 > len(payload):
                raise WifiUartResponseError(f"network {i} flags truncated")
            saved = bool(payload[offset])
            in_range = bool(payload[offset + 1])
            rssi = struct.unpack_from("<b", payload, offset + 2)[0]
            secure = bool(payload[offset + 3])
            # connected flag was documented as a 6th field; guard for firmware
            # that omits it rather than raising on an otherwise-valid frame.
            if offset + 5 <= len(payload):
                connected = bool(payload[offset + 4])
                offset += 5
            else:
                connected = False
                offset += 4
            entries.append(
                UartWifiSavedNetwork(
                    ssid=ssid, saved=saved, in_range=in_range, rssi=rssi, secure=secure,
                    connected=connected,
                )
            )
        return subcommand, (entries, truncated)

    if subcommand in (
        WIFI_CMD_ADD_NETWORK,
        WIFI_CMD_SET_MODE,
        WIFI_CMD_SET_AP_IDENTITY,
        WIFI_CMD_FORGET,
    ):
        if len(payload) < 2:
            raise WifiUartResponseError("response is missing its ok byte")
        return subcommand, bool(payload[1])

    raise WifiUartResponseError(f"unknown WIFI response subcommand 0x{subcommand:02X}")


# ---------------------------------------------------------------------------
# GPIO_PROBE (task_id = UART_TASK_ID_GPIO_PROBE)
# ---------------------------------------------------------------------------
# Only answered on a firmware built with CONFIG_KILNCTL_ENABLE_GPIO_PROBE
# (default off, gpio_probe.c). Every accepted call is deny-list-checked and
# refused while a profile is RUNNING or PAUSED on the firmware side -- this
# client does not duplicate that policy, it just carries the refusal reason
# back.


class GpioProbeResponseError(ValueError):
    """Raised when a GPIO_PROBE response payload does not match its wire layout."""


class GpioProbeRefused(RuntimeError):
    """Raised when the firmware answered but refused the request (ok=0):
    an unknown mode, a deny-listed pin, a profile running/paused, or a
    WRITE to a pin never SET_MODE'd as OUTPUT. ``reason`` is the firmware's
    own ASCII explanation."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


@dataclass(frozen=True)
class GpioProbePin:
    gpio_num: int
    mode: int
    level: int

    @property
    def mode_name(self) -> str:
        return {
            GPIO_PROBE_MODE_INPUT: "input",
            GPIO_PROBE_MODE_INPUT_PULLUP: "input_pullup",
            GPIO_PROBE_MODE_INPUT_PULLDOWN: "input_pulldown",
            GPIO_PROBE_MODE_OUTPUT: "output",
        }.get(self.mode, f"unknown(0x{self.mode:02X})")


def gpio_probe_set_mode(gpio_num: int, mode: int) -> bytes:
    """0x01 SET_MODE: gpio_num, mode (GPIO_PROBE_MODE_*). Replies ok/fail."""
    return struct.pack("<BBB", GPIO_PROBE_CMD_SET_MODE, gpio_num, mode)


def gpio_probe_write(gpio_num: int, level: bool) -> bytes:
    """0x02 WRITE: gpio_num, level. Firmware refuses unless gpio_num was
    already SET_MODE'd OUTPUT. Replies ok/fail."""
    return struct.pack("<BBB", GPIO_PROBE_CMD_WRITE, gpio_num, 1 if level else 0)


def gpio_probe_read(gpio_num: int) -> bytes:
    """0x03 READ: gpio_num (query)."""
    return struct.pack("<BB", GPIO_PROBE_CMD_READ, gpio_num)


def gpio_probe_read_all() -> bytes:
    """0x04 READ_ALL: no args (query). Returns every pin SET_MODE'd since
    the probe task started, up to GPIO_PROBE_MAX_TRACKED."""
    return struct.pack("<B", GPIO_PROBE_CMD_READ_ALL)


def _unpack_gpio_probe_reason(payload: bytes, offset: int) -> str:
    if offset >= len(payload):
        raise GpioProbeResponseError("response is missing its reason length byte")
    length = payload[offset]
    start = offset + 1
    end = start + length
    if end > len(payload):
        raise GpioProbeResponseError("reason string overruns payload")
    return payload[start:end].decode("ascii", errors="replace")


def parse_gpio_probe_response(payload: bytes) -> "tuple[int, object]":
    """Decode a GPIO_PROBE reply into ``(subcmd, value)``.

    Raises :class:`GpioProbeRefused` (not just returning False) when the
    firmware answered ok=0, since a refusal here always carries a reason
    worth surfacing rather than a bare boolean.
    """
    if len(payload) < 2:
        raise GpioProbeResponseError("GPIO_PROBE response is missing its ok byte")
    subcommand = payload[0]

    if subcommand in (GPIO_PROBE_CMD_SET_MODE, GPIO_PROBE_CMD_WRITE):
        ok = bool(payload[1])
        if not ok:
            raise GpioProbeRefused(_unpack_gpio_probe_reason(payload, 2))
        return subcommand, True

    if subcommand == GPIO_PROBE_CMD_READ:
        ok = bool(payload[1])
        if not ok:
            raise GpioProbeRefused(_unpack_gpio_probe_reason(payload, 2))
        if len(payload) < 3:
            raise GpioProbeResponseError("READ response is missing its level byte")
        return subcommand, bool(payload[2])

    if subcommand == GPIO_PROBE_CMD_READ_ALL:
        if len(payload) < 2:
            raise GpioProbeResponseError("READ_ALL response is missing its count byte")
        count = payload[1]
        offset = 2
        pins = []
        for i in range(count):
            if offset + 3 > len(payload):
                raise GpioProbeResponseError(f"READ_ALL entry {i} is truncated")
            pins.append(
                GpioProbePin(
                    gpio_num=payload[offset], mode=payload[offset + 1], level=payload[offset + 2]
                )
            )
            offset += 3
        return subcommand, pins

    raise GpioProbeResponseError(f"unknown GPIO_PROBE response subcommand 0x{subcommand:02X}")
