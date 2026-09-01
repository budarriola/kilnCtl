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

import enum
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
    AUTOTUNE_RULES,
    CONTROL_CMD_GET_UNIT_PREF,
    CONTROL_CMD_GET_ZONES,
    CONTROL_CMD_SET_UNIT_PREF,
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
    PROFILES_BUILTIN_ID_BASE,
    PROFILES_MAX_COUNT,
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
    INFO_CMD_GET_STACK_MARGIN,
    INFO_CMD_GET_WIFI_STATUS,
    StackMarginLevel,
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
    SAFETY_CMD_CT_CAL,
    SAFETY_CMD_GET_CT_CAL,
    SAFETY_CMD_GET_DIAG,
    SAFETY_CMD_CLEAR_TRIP,
    SAFETY_CMD_GET_FW_VERSION,
    SAFETY_CMD_GET_LINK_STATS,
    SAFETY_CMD_GET_STATUS,
    SAFETY_CMD_GET_TRIP_EVENT,
    SAFETY_CMD_PING,
    SAFETY_CMD_REQUEST_ENABLE,
    SAFETY_CMD_ROLLBACK,
    SAFETY_CMD_SET_CONFIG,
    SAFETY_CMD_SET_CT_CAL,
    SAFETY_CMD_SET_FAULT_OUT,
    SAFETY_CMD_SET_POLL_PERIOD,
    SAFETY_CT_CAL_NUM_CHANNELS,
    SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED,
    SYSTEM_CMD_RESTART_UART,
    SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED,
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
    TOUCH_CMD_GET_STATE,
    TOUCH_CMD_INJECT,
    TOUCH_CMD_LOG_TAP_TARGETS,
    TOUCH_CMD_SET_TAP_DUMP,
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
    "system_get_watchdog_panic_disabled",
    "system_set_watchdog_panic_disabled",
    "SystemResponseError",
    "parse_system_response",
    # Shared {subcmd, ok, [reason]} reply decoding (ROADMAP.md "KilnFW
    # PC-link command acknowledgement")
    "OkReason",
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
    "RelayRefusal",
    "RelayResult",
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
    "safety_set_ct_cal",
    "safety_get_ct_cal",
    "SafetyCtCal",
    "SafetyCtCalChannel",
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
# The rest of this module used to live here inline. It is now split into
# per-subsystem submodules (pure refactor, zero behavior change) -- re-export
# everything so `from kilnctrl.devices import X` keeps working unchanged.
# ---------------------------------------------------------------------------
from .devices_common import *  # noqa: F401,F403
from .devices_common import _decode_ok_reason  # noqa: F401
from .devices_system import *  # noqa: F401,F403
from .devices_thermo import *  # noqa: F401,F403
from .devices_io import *  # noqa: F401,F403
from .devices_display import *  # noqa: F401,F403
from .devices_touch import *  # noqa: F401,F403
from .devices_safety import *  # noqa: F401,F403
from .devices_info import *  # noqa: F401,F403
from .devices_log import *  # noqa: F401,F403
from .devices_control import *  # noqa: F401,F403
from .devices_profiles import *  # noqa: F401,F403
from .devices_autotune import *  # noqa: F401,F403
from .devices_wifi_uart import *  # noqa: F401,F403
from .devices_gpio_probe import *  # noqa: F401,F403
