"""Wire protocol for the KilnCtrl ESP32-S3 <-> PC hardened UART link.

This is the Python side of:
    App/drivers/espInterfaces/uart_protocol.h
    App/drivers/espInterfaces/uart_protocol.c
    App/drivers/uart_task_ids.h

Keep the constants and the byte layout here in lockstep with those files.

Framing (SLIP-style byte stuffing, mirrors ``stuff_and_send`` /
``uart_protocol_rx_task`` in uart_protocol.c)::

    DELIM ( ...stuffed bytes... ) DELIM

Raw (unstuffed) frame layout -- all *header* multi-byte fields are BIG-endian::

    offset 0             MSG_TYPE   u8   (DATA=0x01 ACK=0x02 NACK=0x03)
    offset 1..2          MSG_INDEX  u16 BE
    offset 3             SRC_DEVICE u8   (ESP=0 HOST=1)
    offset 4             SRC_TASK   u8
    offset 5             DST_DEVICE u8
    offset 6             DST_TASK   u8
    offset 7             LENGTH     u8   (payload length, 0..128)
    offset 8..8+LEN-1    PAYLOAD
    offset 8+LEN..+1     CRC16      u16 BE (CRC-16/CCITT-FALSE over [0, 8+LEN))

Note the deliberate endian split: header fields (MSG_INDEX, CRC16) are
big-endian, while the *payload* command fields defined in uart_task_ids.h are
little-endian (natural ESP32 struct layout, memcpy'd straight into float/double
on the firmware side). This asymmetry is intentional -- do not "fix" it.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field

# --- framing ---------------------------------------------------------------
FRAME_DELIM = 0x7E
FRAME_ESC = 0x7D
FRAME_ESC_XOR = 0x20

# --- sizes / limits (uart_protocol.h) --------------------------------------
HEADER_LEN = 8
CRC_LEN = 2
#: Capped at 253 (LENGTH header field is one byte, 255 hard ceiling) -- see
#: uart_protocol.h's UART_PROTO_MAX_PAYLOAD comment. Bumped from 128.
UART_PROTO_MAX_PAYLOAD = 253
UART_PROTO_MAX_RETRIES = 10
UART_PROTO_DEFAULT_ACK_TIMEOUT_MS = 200
UART_PROTO_DEDUP_DEPTH = 4

#: header(8) + max payload + crc(2), before stuffing
RAW_FRAME_MAX = HEADER_LEN + UART_PROTO_MAX_PAYLOAD + CRC_LEN

#: UART_OWNER_BAUD_RATE in App/drivers/settings.h -- must match
#: KILNCTL_UART_BAUD_RATE (App/drivers/Kconfig) since neither side negotiates
#: this. Bumped from 115200; see that Kconfig entry's help text for why this
#: is safe (CRC'd + retried per frame) and what it does/doesn't fix.
DEFAULT_BAUD_RATE = 921600

#: Python side of UART_PROTOCOL_VERSION in App/drivers/uart_task_ids.h.
#: Bump policy lives there -- read it before touching this number. Carried
#: at a fixed offset (bytes 0-1) in the GET_FW_VERSION response so a
#: mismatch can always be detected before trusting the rest of that payload;
#: see devices.parse_fw_version_response().
#:
#: Version 2 = the kilnCtl main board. The unit-test fixture's task ids
#: (MCP4728 DAC 1, AD9833 2, SSD1306 4, PCF8575 7) are reused here for this
#: board's hardware, so a v1 firmware and this code disagree about what
#: every one of those task ids *means* -- hence the hard equality gate in
#: devices.FirmwareVersion.compatible.
#:
#: Version 3 (2026-08-11): UART_PROTO_MAX_PAYLOAD 128 -> 253. Frame layout
#: unchanged; bumped only so a v2 peer's smaller receive buffer can't
#: silently truncate a v3 sender's larger frame.
#: Version 4 (2026-08-13): four new task_ids -- CONTROL (8), PROFILES (9),
#: AUTOTUNE (10), WIFI (11) -- plus SYSTEM_CMD_FACTORY_RESET, so the GUI can
#: drive everything the HTTP dashboard offers without needing Wi-Fi. See
#: docs/UART_PROTOCOL.md's "Version 4" section and each task's block below.
#: Version 5 (2026-08-17): one new task_id, TOUCH (13), for the NS2009 touch
#: controller on the display panel -- GET_STATE plus INJECT, the latter
#: letting this side feed a synthetic touch the firmware treats exactly like
#: a real press. See docs/UART_PROTOCOL.md's "Version 5" section.
UART_PROTOCOL_VERSION = 5


class Device(enum.IntEnum):
    """uart_proto_device_t"""

    ESP = 0
    HOST = 1


class MsgType(enum.IntEnum):
    """uart_proto_msg_type_t"""

    DATA = 0x01
    ACK = 0x02
    NACK = 0x03  # destination task not registered ("undeliverable")
    #: Fire-and-forget: no ACK, no retry, no dedup. Added to
    #: uart_protocol.{c,h} 2026-08-16 for the safety link (the RP2040 must
    #: never be obliged to transmit in reply -- CommonFW/docs/LINK_PROTOCOL.md
    #: sec 1). UartLink.send() below does not build or accept this type yet --
    #: nothing on the PC<->ESP link uses it today -- so this constant exists
    #: for parity with the firmware enum and for decoding a capture, not as a
    #: send path.
    BROADCAST = 0x04


# --- task ids (Python side of App/drivers/uart_task_ids.h) -----------------
UART_TASK_ID_THERMO = 1  # MAX31856 x3 on the thermocouple board (J6)
UART_TASK_ID_IO = 2  # SX1509 expander: relays, digital I/O, DRDY
UART_TASK_ID_INFO = 3
UART_TASK_ID_DISPLAY = 4  # ILI9488 TFT on J2
UART_TASK_ID_LOG = 5
UART_TASK_ID_SYSTEM = 6
UART_TASK_ID_SAFETY = 7  # opto-isolated link to the RP2040 safety processor
UART_TASK_ID_CONTROL = 8  # zone config (PID/model/read-back) + manual relay control
UART_TASK_ID_PROFILES = 9  # fire profile CRUD + execution control
UART_TASK_ID_AUTOTUNE = 10  # PID autotune (step/relay methods)
UART_TASK_ID_WIFI = 11  # Wi-Fi status/scan/provision/forget
UART_TASK_ID_GPIO_PROBE = 12  # raw ESP32 GPIO probe -- CONFIG_KILNCTL_ENABLE_GPIO_PROBE, default off
UART_TASK_ID_TOUCH = 13  # NS2009 touch controller on the display panel (J2)

# SYSTEM subcommands. RESTART_UART is deliberately RX-only on the firmware
# side (see uart_task_ids.h) -- it flushes the stuck/garbage bytes a wedged
# link is actually made of, without risking eating this very request's own
# in-flight ACK off the TX side. FACTORY_RESET mirrors POST /api/factory_reset
# exactly (same NVS erase + unconditional reboot ~500ms later); no reply
# frame either way, an out-of-range scope byte is rejected with no erase.
SYSTEM_CMD_RESTART_UART = 0x01
SYSTEM_CMD_FACTORY_RESET = 0x02
#: Query -- like INFO, the request DATA frame is ACKed for delivery only and
#: the answer arrives as a separate DATA frame back to the requester. Reply
#: payload is 2 bytes: byte0 = 0x03 (echoed subcommand), byte1 = disabled(0/1).
SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED = 0x03
#: byte1 = disabled(0/1). No reply frame -- the ACK is the only confirmation;
#: poll SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED afterward to read back the
#: applied value.
SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED = 0x04

#: FACTORY_RESET scope byte values.
FACTORY_RESET_SCOPE_WIFI = 0
FACTORY_RESET_SCOPE_KILN = 1
FACTORY_RESET_SCOPE_PROFILES = 2
FACTORY_RESET_SCOPE_ALL = 3

# --- THERMO subcommands (3x MAX31856 over the shared SPI bus, CS0/CS1/CS2) --
#
# READ / READ_FAULTS / READ_REG are *queries*: like INFO, the request DATA
# frame is ACKed for delivery only and the answer arrives as a separate DATA
# frame -- but these replies echo their subcommand in byte0, so they are
# self-describing (see thermo.py). SET_AUTO_REPORT additionally makes the
# firmware push unsolicited READ replies at a fixed period.
THERMO_CMD_CONFIG_CHANNEL = 0x01
THERMO_CMD_SET_THRESHOLDS = 0x02
THERMO_CMD_SET_CJ_OFFSET = 0x03
THERMO_CMD_ONE_SHOT = 0x04
THERMO_CMD_READ = 0x05
THERMO_CMD_READ_FAULTS = 0x06
THERMO_CMD_CLEAR_FAULTS = 0x07
THERMO_CMD_SET_AUTO_REPORT = 0x08
THERMO_CMD_READ_REG = 0x09
THERMO_CMD_WRITE_REG = 0x0A

THERMO_CHANNEL_COUNT = 3
#: Channel selector meaning "all three" in READ / READ_FAULTS.
THERMO_CHANNEL_ALL = 0xFF

#: Bytes per channel entry in a READ / AUTO_REPORT reply.
THERMO_READ_ENTRY_LEN = 12
#: Bytes per channel entry in a READ_FAULTS reply.
THERMO_FAULT_ENTRY_LEN = 3
#: READ_REG's length field is bounded by the part's register file.
THERMO_REG_READ_MAX = 16

# --- IO subcommands (SX1509 at 0x3E) ---------------------------------------
#
# READ / SX_READ_REG / SX_SCAN are queries with the same shape as the THERMO
# ones. READ replies are also pushed unsolicited while SET_AUTO_REPORT is on
# (and immediately on every ~INT edge).
IO_CMD_SET_RELAY = 0x01
IO_CMD_SET_RELAY_MASK = 0x02
IO_CMD_SET_IO = 0x03
IO_CMD_SET_IO_DIR = 0x04
IO_CMD_READ = 0x05
IO_CMD_SET_AUTO_REPORT = 0x06
IO_CMD_ALL_RELAYS_OFF = 0x07
IO_CMD_SX_WRITE_REG = 0x10
IO_CMD_SX_READ_REG = 0x11
IO_CMD_SX_SET_DIR = 0x12
IO_CMD_SX_SET_PULLUP = 0x13
IO_CMD_SX_SET_OPENDRAIN = 0x14
IO_CMD_SX_SET_DEBOUNCE = 0x15
IO_CMD_SX_SET_INT_MASK = 0x16
IO_CMD_SX_LED_DRIVER = 0x17
IO_CMD_SX_RESET = 0x18
IO_CMD_SX_SCAN = 0x19

IO_RELAY_COUNT = 4
IO_DIGITAL_COUNT = 7
#: SX1509 pin count -- the raw-register half of this task addresses all 16.
IO_EXPANDER_PIN_COUNT = 16
#: SX_READ_REG's length field, same 1..16 bound as the THERMO one.
IO_REG_READ_MAX = 16

# --- DISPLAY subcommands (ILI9488 480x320 on J2) ---------------------------
#
# Colors are RGB565 u16 LE on the wire; the firmware expands to the 18-bit
# RGB666 the panel needs over SPI. READ_ID is the only query.
DISPLAY_CMD_RESET = 0x01
DISPLAY_CMD_SET_POWER = 0x02
DISPLAY_CMD_SET_ROTATION = 0x03
DISPLAY_CMD_SET_INVERT = 0x04
DISPLAY_CMD_CLEAR = 0x05
DISPLAY_CMD_FILL_RECT = 0x06
DISPLAY_CMD_DRAW_RECT = 0x07
DISPLAY_CMD_DRAW_LINE = 0x08
DISPLAY_CMD_SET_TEXT_CURSOR = 0x09
DISPLAY_CMD_SET_TEXT_STYLE = 0x0A
DISPLAY_CMD_PRINT = 0x0B
DISPLAY_CMD_BLIT_BEGIN = 0x0C
DISPLAY_CMD_BLIT_DATA = 0x0D
DISPLAY_CMD_BLIT_END = 0x0E
DISPLAY_CMD_READ_ID = 0x0F

#: Native panel geometry (the module is 480x320 in landscape rotations 1/3,
#: 320x480 in the portrait ones). READ_ID reports the live values.
DISPLAY_NATIVE_WIDTH = 480
DISPLAY_NATIVE_HEIGHT = 320

# --- TOUCH subcommands (NS2009 on the same J2 panel as DISPLAY) ------------
#
# GET_STATE is the only query. INJECT is fire-and-forget (no reply): it
# feeds the firmware's screen_idle state machine a synthetic touch that
# resets the idle timer and wakes the screen exactly like a real NS2009
# press -- this is what lets an MCP tool "send touches as if from the
# screen" without physical hardware.
TOUCH_CMD_GET_STATE = 0x01
TOUCH_CMD_INJECT = 0x02
#: Turns the AUTOMATIC per-page-switch tap-target dump on/off (off by
#: default); fire-and-forget, no reply. Payload: enable(u8).
TOUCH_CMD_SET_TAP_DUMP = 0x03
#: Requests an immediate tap-target dump for whatever screen is currently
#: loaded; fire-and-forget, no reply -- the dump itself arrives as ESP_LOGI
#: lines over the device log, not as a reply on this task.
TOUCH_CMD_LOG_TAP_TARGETS = 0x04

# --- SAFETY subcommands (opto-isolated link to the RP2040) ------------------
#
# GET_STATUS / GET_LINK_STATS are queries. GET_STATUS answers from the ESP's
# cache of the last good poll rather than blocking on the far side, so a dead
# link shows up as link_up = 0 with a stale age, not as a hung request.
SAFETY_CMD_GET_STATUS = 0x01
SAFETY_CMD_REQUEST_ENABLE = 0x02
SAFETY_CMD_PING = 0x03
SAFETY_CMD_GET_LINK_STATS = 0x04
SAFETY_CMD_SET_POLL_PERIOD = 0x05
SAFETY_CMD_SET_FAULT_OUT = 0x06
#: PC -> ESP, relayed to the Pico by uart_bridge.c's SAFETY_CMD_CLEAR_TRIP
#: case. Clears a latched safety trip. It takes NO arguments over this link:
#: the ESP derives the trip_mask itself from its own cached Pico DIAG state
#: rather than trusting one supplied over the PC link (see
#: safety_link_send_clear_trip()'s doc comment). Fire-and-forget, no reply on
#: the wire; the outcome shows up on the next GET_STATUS poll.
#:
#: This matters more than it looks: a safety trip LATCHES on the Pico
#: (safety_guards.c returns early once is_tripped is set), so deasserting
#: whatever caused it does NOT clear it. Without this command a tripped board
#: stays tripped until it is power-cycled.
SAFETY_CMD_CLEAR_TRIP = 0x0A
#: ESP -> Pico, CommonFW/docs/LINK_PROTOCOL.md sec 4 -- commissions
#: SaftyFW's config_store.h tc_type. Fire-and-forget, no reply on the wire;
#: the outcome shows up on the next GET_DIAG/GET_STATUS poll, not here.
SAFETY_CMD_SET_CONFIG = 0x16
#: ESP -> Pico, CommonFW/docs/LINK_PROTOCOL.md sec 4 -- tools/PcTools/TODO.md's
#: `ota_rollback(processor)` line, Pico half. Explicit "revert to the
#: previously-running bootloader slot, right now". No payload (cmd byte
#: only), fire-and-forget, no reply on the wire; refused (ARMED, or no valid
#: slot to fall back to) entirely on SaftyFW's own say-so -- the outcome
#: shows up as the link dropping and recovering with a new boot_id on the
#: next GET_STATUS poll, not here.
SAFETY_CMD_ROLLBACK = 0x17
#: PC -> ESP queries, CommonFW/docs/LINK_PROTOCOL.md sec 7: "Mirror all of it
#: on the PC-link SAFETY task as well" -- the same DIAG (Frame B) / TRIP_EVENT
#: (Frame D) telemetry dashboard_http.c and ui_page_safety.c already read,
#: answered from the ESP's cache only (never a live round trip to the Pico),
#: same shape as GET_STATUS/GET_LINK_STATS above. See uart_task_ids.h's
#: SAFETY_CMD_GET_DIAG doc comment for both response payload layouts.
SAFETY_CMD_GET_DIAG = 0x0C
SAFETY_CMD_GET_TRIP_EVENT = 0x15
#: PC -> ESP query, same shared-id/reply-on-request convention as
#: SAFETY_CMD_GET_DIAG/GET_TRIP_EVENT above -- mirrors the Pico's own
#: FW_VERSION (Frame C) push, answered from the ESP's cache only. Same value
#: as the Pico-side SAFETY_CMD_GET_FW_VERSION/SAFETY_CMD_FW_VERSION
#: (CommonFW/docs/LINK_PROTOCOL.md sec 4/6), reused here rather than given a
#: separate id -- the request is 1 byte (no args), the reply is the full
#: build-identity/config-CRC payload (protocol_version/min_compatible first,
#: then commit/datetime/boot_id/config_version/config_crc).
SAFETY_CMD_GET_FW_VERSION = 0x0B

#: Age field in GET_STATUS: "no valid status has ever been received".
SAFETY_AGE_NEVER = 0xFFFF

# --- CONTROL subcommands (task_id = UART_TASK_ID_CONTROL) -------------------
# Zone configuration reads plus narrow PID/plant-model writes. GET_ZONES is a
# query; SET_ZONE_PID/SET_ZONE_MODEL reply ok/fail (unlike THERMO/IO's silent
# SET_*). Manual relay control stays on IO (task 2) -- not duplicated here.
CONTROL_CMD_GET_ZONES = 0x01
CONTROL_CMD_SET_ZONE_PID = 0x02
CONTROL_CMD_SET_ZONE_MODEL = 0x03

#: Bytes per zone record in a GET_ZONES reply (uart_task_ids.h).
CONTROL_ZONE_RECORD_LEN = 31

# 2026-08-21 (ROADMAP.md shared unit preference): additive subcommands on the
# existing CONTROL task -- no UART_PROTOCOL_VERSION bump, since neither
# reorders/resizes an existing field (see uart_task_ids.h's doc comment).
# DISPLAY-ONLY: every zone/profile temperature on this link is still Celsius
# regardless of this setting -- see App/drivers/unit_pref.h.
CONTROL_CMD_GET_UNIT_PREF = 0x04
CONTROL_CMD_SET_UNIT_PREF = 0x05

#: unit_pref_t wire values (App/drivers/unit_pref.h) -- 0 = Celsius, 1 = Fahrenheit.
UNIT_PREF_CELSIUS = 0
UNIT_PREF_FAHRENHEIT = 1

# --- PROFILES subcommands (task_id = UART_TASK_ID_PROFILES) -----------------
PROFILES_CMD_LIST = 0x01
PROFILES_CMD_GET = 0x02
PROFILES_CMD_SAVE = 0x03
PROFILES_CMD_DELETE = 0x04
PROFILES_CMD_GET_EXEC_STATUS = 0x05
PROFILES_CMD_START = 0x06
PROFILES_CMD_STOP = 0x07
PROFILES_CMD_PAUSE = 0x08
PROFILES_CMD_RESUME = 0x09
PROFILES_CMD_ACK_LAST_RUN = 0x0A

#: SAVE's id byte requesting "first free slot" -- mirrors POST /api/profile's
#: empty/-1/out-of-range id field.
PROFILES_SAVE_ID_NEW = 0xFF
#: Max user profile slots (writable ids 0..7). NVS-backed, see profiles_http.c.
PROFILES_MAX_COUNT = 8

#: First id of the read-only shipped catalogue -- mirrors
#: ``PROFILE_BUILTIN_ID_BASE`` in ``App/drivers/profiles_builtin.h``. Ids
#: ``PROFILES_BUILTIN_ID_BASE + index`` address the firing schedules that ship
#: in flash: readable and runnable, never writable. The two id ranges do not
#: overlap by construction, so any id is unambiguously one or the other. This
#: is the single place the number 128 appears on the PC side.
PROFILES_BUILTIN_ID_BASE = 128


def profile_id_is_builtin(profile_id: int) -> bool:
    """True if ``profile_id`` addresses a shipped read-only schedule."""
    return profile_id >= PROFILES_BUILTIN_ID_BASE
#: Bytes per segment in a SAVE request / GET reply (target_c f32, ramp f32, dwell_min u32).
PROFILES_SEGMENT_LEN = 12

# --- AUTOTUNE subcommands (task_id = UART_TASK_ID_AUTOTUNE) -----------------
AUTOTUNE_CMD_GET_STATUS = 0x01
AUTOTUNE_CMD_START = 0x02
AUTOTUNE_CMD_ABORT = 0x03
AUTOTUNE_CMD_ACCEPT = 0x04

#: START method byte.
AUTOTUNE_METHOD_STEP = 0
AUTOTUNE_METHOD_RELAY = 1
#: START rule byte.
AUTOTUNE_RULE_TL = 0  # Tyreus-Luyben
AUTOTUNE_RULE_ZN = 1  # Ziegler-Nichols

# --- WIFI subcommands (task_id = UART_TASK_ID_WIFI) --------------------------
# Mirrors wifi_provision_http.c's GET /status, GET /scan, POST /provision,
# GET /networks, POST /forget -- exists specifically so Wi-Fi can be
# configured over a link that works even when Wi-Fi itself is down.
WIFI_CMD_GET_STATUS = 0x01
WIFI_CMD_SCAN = 0x02
WIFI_CMD_ADD_NETWORK = 0x03
WIFI_CMD_SET_MODE = 0x04
WIFI_CMD_SET_AP_IDENTITY = 0x05
WIFI_CMD_GET_NETWORKS = 0x06
WIFI_CMD_FORGET = 0x07

#: SCAN/GET_NETWORKS entry caps (uart_task_ids.h).
WIFI_WIRE_MAX_SCAN_ENTRIES = 6
WIFI_WIRE_MAX_NETWORK_ENTRIES = 5

#: SET_MODE's mode byte.
WIFI_MODE_HOME = 0
WIFI_MODE_AP = 1

# --- GPIO_PROBE subcommands (task_id = UART_TASK_ID_GPIO_PROBE) -------------
# Only answered when the board was built with CONFIG_KILNCTL_ENABLE_GPIO_PROBE
# (default off) -- see gpio_probe.c and uart_task_ids.h for the deny-list and
# the profile-running refusal. On a build without it, every call here times
# out exactly like any other unregistered task_id; that is not a bug in this
# client, it means the capability was not compiled in.
GPIO_PROBE_CMD_SET_MODE = 0x01
GPIO_PROBE_CMD_WRITE = 0x02
GPIO_PROBE_CMD_READ = 0x03
GPIO_PROBE_CMD_READ_ALL = 0x04

#: SET_MODE's mode byte.
GPIO_PROBE_MODE_INPUT = 0
GPIO_PROBE_MODE_INPUT_PULLUP = 1
GPIO_PROBE_MODE_INPUT_PULLDOWN = 2
GPIO_PROBE_MODE_OUTPUT = 3

GPIO_PROBE_MAX_TRACKED = 16


class LogLevel(enum.IntEnum):
    """UART_LOG_LEVEL_* in uart_task_ids.h -- byte0 of a LOG task payload.

    Firmware -> PC only, unsolicited: every ESP_LOGx call is captured and
    forwarded here instead of the USB-Serial-JTAG console (see
    App/drivers/uart_log_bridge.c), so device logging is visible over the
    same always-on link used for control, with no separate debugger/monitor
    session required.
    """

    ERROR = 0x00
    WARN = 0x01
    INFO = 0x02
    DEBUG = 0x03
    VERBOSE = 0x04


# INFO subcommands.
#
# Unlike the device-command subcommands these are *queries*: the request DATA frame is ACKed as
# usual (delivery confirmation only), and the answer comes back as a separate
# DATA frame from (ESP, UART_TASK_ID_INFO) addressed to whatever
# (device, task_id) sent the request -- so the requester must itself have
# registered that task id. See info_bridge_task() in uart_bridge.c.
INFO_CMD_GET_PIN_CONFIG = 0x01
INFO_CMD_GET_FW_VERSION = 0x02
INFO_CMD_GET_WIFI_STATUS = 0x03


class PinFunction(enum.IntEnum):
    """PIN_FUNC_* function ids in a GET_PIN_CONFIG response entry.

    The wire carries only this id; the human-readable meaning (which driver
    owns the pin) lives PC-side in devices.PIN_FUNCTION_LABELS, deliberately
    keeping the response small -- see the comment above PIN_FUNC_I2C_SDA in
    uart_task_ids.h.

    Only real ESP32-S3 GPIOs appear in a reply. The relay drives, the DRDY
    inputs and LCD_IORQ/LCD_Reset are SX1509 pins, not GPIOs, and are
    reported through the IO task's READ instead.
    """

    I2C_SDA = 0x01
    I2C_SCL = 0x02
    UART_TX = 0x03
    UART_RX = 0x04
    SPI_SCLK = 0x05
    SPI_MOSI = 0x06
    SPI_CS = 0x07
    LED_HEARTBEAT = 0x08
    SPI_MISO = 0x09
    THERMO_FAULT = 0x0A
    EXPANDER_IRQ = 0x0B
    EXPANDER_RST = 0x0C
    SAFETY_TX = 0x0D
    SAFETY_RX = 0x0E
    SAFETY_FAULT = 0x0F


class TcType(enum.IntEnum):
    """CR1.TC[3:0] -- MAX31856 thermocouple type.

    K is what this kiln ships with; the rest are the part's other supported
    types plus its two raw-voltage (gain) modes.
    """

    B = 0x00
    E = 0x01
    J = 0x02
    K = 0x03
    N = 0x04
    R = 0x05
    S = 0x06
    T = 0x07
    VMODE_G8 = 0x08
    VMODE_G32 = 0x0C


class AvgMode(enum.IntEnum):
    """CR1.AVGSEL[2:0] -- samples averaged per conversion.

    More averaging is quieter but slower: the automatic-conversion period
    grows by roughly 33 ms per extra sample beyond the first at 60 Hz.
    """

    AVG_1 = 0x00
    AVG_2 = 0x01
    AVG_4 = 0x02
    AVG_8 = 0x03
    AVG_16 = 0x04


class ThermoFault(enum.IntFlag):
    """MAX31856 fault status register (SR) bits, as carried in a READ reply.

    An IntFlag rather than bare constants so a status byte decodes into named
    members in one step -- the GUI and the MCP tools both show these as text,
    never as a hex byte.
    """

    OPEN = 0x01  # thermocouple open circuit
    OVUV = 0x02  # over/under voltage on an input
    TCLOW = 0x04  # TC temperature below the low threshold
    TCHIGH = 0x08  # TC temperature above the high threshold
    CJLOW = 0x10  # cold junction below the low threshold
    CJHIGH = 0x20  # cold junction above the high threshold
    TCRANGE = 0x40  # TC temperature outside the type's range
    CJRANGE = 0x80  # cold junction outside -55..+125 degC


class ThermoReadFlag(enum.IntFlag):
    """byte[10] of a READ entry -- link/driver state, not part faults.

    Distinct from :class:`ThermoFault` on purpose: these say something about
    *this reading* (did SPI work, is it fresh, is the ~FAULT pin low right
    now), while ThermoFault is the part's own opinion of the thermocouple.
    """

    FAULT_PIN = 0x01  # ~FAULT pin asserted (low)
    SPI_FAILED = 0x02  # SPI read failed; both temperatures are NaN
    STALE = 0x04  # no conversion since the last read


class SafetyFlag(enum.IntFlag):
    """byte1 of a SAFETY GET_STATUS reply.

    Everything except FAULT is the *Pico's* view, relayed through the ESP's
    cache of the last good poll; FAULT is the one bit this firmware owns,
    since GPIO6 is an ESP output (see docs/HARDWARE.md).
    """

    LINK_UP = 0x01  # a valid reply within 3 poll periods
    FAULT = 0x02  # Fault line currently asserted by this firmware
    ESTOP = 0x04  # estop asserted, as reported by the Pico
    RELAY = 0x08  # safety relay K4 energized
    ENABLED = 0x10  # heating enable currently granted
    TEMP_VALID = 0x20  # safety thermocouple reading valid


# ---------------------------------------------------------------------------
# CRC
# ---------------------------------------------------------------------------
def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.

    Bit-for-bit port of ``crc16_ccitt_false()`` in uart_protocol.c. Pure Python
    on purpose -- no external crc dependency.

    Check value: crc16_ccitt_false(b"123456789") == 0x29B1.
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


# ---------------------------------------------------------------------------
# Frame
# ---------------------------------------------------------------------------
class FrameError(ValueError):
    """Raised when a raw byte buffer cannot be decoded as a valid frame."""


def _as_device(value: int) -> Device | int:
    """Device enum if known, else the raw byte (be lenient about peers)."""
    try:
        return Device(value)
    except ValueError:
        return value


@dataclass(frozen=True)
class Frame:
    msg_type: MsgType
    msg_index: int
    src_device: Device
    src_task: int
    dst_device: Device
    dst_task: int
    payload: bytes = b""

    def __post_init__(self) -> None:
        if len(self.payload) > UART_PROTO_MAX_PAYLOAD:
            raise ValueError(
                f"payload too long: {len(self.payload)} > {UART_PROTO_MAX_PAYLOAD}"
            )

    # -- encode ------------------------------------------------------------
    def to_raw(self) -> bytes:
        """Serialize to the unstuffed on-wire frame including trailing CRC16."""
        body = bytes(
            (
                int(self.msg_type) & 0xFF,
                (self.msg_index >> 8) & 0xFF,
                self.msg_index & 0xFF,
                int(self.src_device) & 0xFF,
                self.src_task & 0xFF,
                int(self.dst_device) & 0xFF,
                self.dst_task & 0xFF,
                len(self.payload) & 0xFF,
            )
        ) + self.payload
        crc = crc16_ccitt_false(body)
        return body + bytes(((crc >> 8) & 0xFF, crc & 0xFF))

    def to_wire(self) -> bytes:
        """Serialize and byte-stuff, delimiter-wrapped and ready to write."""
        return stuff(self.to_raw())

    # -- decode ------------------------------------------------------------
    @staticmethod
    def from_raw(raw: bytes) -> "Frame":
        """Parse an unstuffed frame, validating length and CRC.

        Mirrors the front half of ``handle_raw_frame()``. Raises FrameError on
        anything malformed; callers drop such frames silently (the sender's
        retransmit recovers them).
        """
        if len(raw) < HEADER_LEN + CRC_LEN:
            raise FrameError(f"frame too short: {len(raw)} bytes")
        length = raw[7]
        # Checked before the length/CRC comparison below so an over-long
        # LENGTH byte can never reach Frame's constructor, whose own guard
        # raises a plain ValueError -- which the RX loop does not catch, and
        # which would therefore kill the reader thread instead of dropping
        # one bad frame.
        if length > UART_PROTO_MAX_PAYLOAD:
            raise FrameError(
                f"payload length {length} exceeds {UART_PROTO_MAX_PAYLOAD}"
            )
        if len(raw) != HEADER_LEN + length + CRC_LEN:
            raise FrameError(
                f"frame length mismatch (hdr says {length}, got {len(raw)} bytes)"
            )
        expected = crc16_ccitt_false(raw[: HEADER_LEN + length])
        actual = (raw[HEADER_LEN + length] << 8) | raw[HEADER_LEN + length + 1]
        if expected != actual:
            raise FrameError(f"CRC mismatch: expected 0x{expected:04X} got 0x{actual:04X}")

        try:
            msg_type = MsgType(raw[0])
        except ValueError as exc:  # unknown type byte -> not a frame we handle
            raise FrameError(f"unknown msg type 0x{raw[0]:02X}") from exc

        return Frame(
            msg_type=msg_type,
            msg_index=(raw[1] << 8) | raw[2],
            src_device=_as_device(raw[3]),
            src_task=raw[4],
            dst_device=_as_device(raw[5]),
            dst_task=raw[6],
            payload=bytes(raw[HEADER_LEN : HEADER_LEN + length]),
        )


# ---------------------------------------------------------------------------
# SLIP-style byte stuffing
# ---------------------------------------------------------------------------
def stuff(raw: bytes) -> bytes:
    """Byte-stuff ``raw`` and wrap it in start/end delimiters."""
    out = bytearray()
    out.append(FRAME_DELIM)
    for b in raw:
        if b == FRAME_DELIM or b == FRAME_ESC:
            out.append(FRAME_ESC)
            out.append(b ^ FRAME_ESC_XOR)
        else:
            out.append(b)
    out.append(FRAME_DELIM)
    return bytes(out)


def unstuff(stuffed: bytes) -> bytes:
    """Reverse :func:`stuff` for a single frame body.

    Accepts the body with or without its surrounding delimiters. Intended for
    tests/one-shot decoding; the live receive path uses :class:`FrameDecoder`.
    """
    body = stuffed
    if body[:1] == bytes((FRAME_DELIM,)):
        body = body[1:]
    if body[-1:] == bytes((FRAME_DELIM,)):
        body = body[:-1]

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
    return bytes(out)


@dataclass
class FrameDecoder:
    """Incremental byte-stream -> raw-frame state machine.

    Byte-for-byte mirror of the loop in ``uart_protocol_rx_task()``:

    * a DELIM both ends the frame in progress (if it holds any bytes) and
      starts a new one -- back-to-back DELIMs are just empty-frame noise;
    * bytes before the very first DELIM are discarded (resync after garbage);
    * ESC un-escapes the following byte;
    * an oversized frame drops out of frame-sync until the next DELIM.
    """

    _buf: bytearray = field(default_factory=bytearray, init=False)
    _in_frame: bool = field(default=False, init=False)
    _escaped: bool = field(default=False, init=False)

    def reset(self) -> None:
        self._buf.clear()
        self._in_frame = False
        self._escaped = False

    def feed(self, data: bytes) -> list[bytes]:
        """Push received bytes; return any complete *raw* (unstuffed) frames."""
        frames: list[bytes] = []
        for byte in data:
            if byte == FRAME_DELIM:
                if self._in_frame and self._buf:
                    frames.append(bytes(self._buf))
                self._buf.clear()
                self._in_frame = True
                self._escaped = False
                continue

            if not self._in_frame:
                continue  # discard noise before the first delimiter

            if self._escaped:
                byte ^= FRAME_ESC_XOR
                self._escaped = False
            elif byte == FRAME_ESC:
                self._escaped = True
                continue

            if len(self._buf) < RAW_FRAME_MAX:
                self._buf.append(byte)
            else:
                # Oversized/corrupt frame: resync on the next delimiter.
                self._in_frame = False
        return frames
