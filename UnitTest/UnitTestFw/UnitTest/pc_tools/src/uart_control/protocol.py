"""Wire protocol for the ESP32-S3 <-> PC hardened UART link.

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
UART_PROTO_MAX_PAYLOAD = 128
UART_PROTO_MAX_RETRIES = 10
UART_PROTO_DEFAULT_ACK_TIMEOUT_MS = 200
UART_PROTO_DEDUP_DEPTH = 4

#: header(8) + max payload + crc(2), before stuffing
RAW_FRAME_MAX = HEADER_LEN + UART_PROTO_MAX_PAYLOAD + CRC_LEN

#: UART_OWNER_BAUD_RATE in App/drivers/settings.h
DEFAULT_BAUD_RATE = 115200

#: Python side of UART_PROTOCOL_VERSION in App/drivers/uart_task_ids.h.
#: Bump policy lives there -- read it before touching this number. Carried
#: at a fixed offset (bytes 0-1) in the GET_FW_VERSION response so a
#: mismatch can always be detected before trusting the rest of that payload;
#: see devices.parse_fw_version_response().
UART_PROTOCOL_VERSION = 1


class Device(enum.IntEnum):
    """uart_proto_device_t"""

    ESP = 0
    HOST = 1


class MsgType(enum.IntEnum):
    """uart_proto_msg_type_t"""

    DATA = 0x01
    ACK = 0x02
    NACK = 0x03  # destination task not registered ("undeliverable")


# --- task ids (Python side of App/drivers/uart_task_ids.h) -----------------
UART_TASK_ID_DAC = 1
UART_TASK_ID_AD9833 = 2
UART_TASK_ID_INFO = 3
UART_TASK_ID_OLED = 4
UART_TASK_ID_LOG = 5
UART_TASK_ID_SYSTEM = 6
UART_TASK_ID_PCF8575 = 7

# SYSTEM subcommands. RESTART_UART is deliberately RX-only on the firmware
# side (see uart_task_ids.h) -- it flushes the stuck/garbage bytes a wedged
# link is actually made of, without risking eating this very request's own
# in-flight ACK off the TX side.
SYSTEM_CMD_RESTART_UART = 0x01

# DAC subcommands
DAC_CMD_SET_CHANNEL_PERCENT = 0x01
DAC_CMD_SET_ALL_PERCENT = 0x02
DAC_CMD_POWER_DOWN = 0x03

# AD9833 subcommands
AD9833_CMD_SET_FREQUENCY = 0x01
AD9833_CMD_SET_PHASE = 0x02
AD9833_CMD_SET_WAVEFORM = 0x03
AD9833_CMD_SELECT_FREQ_REG = 0x04
AD9833_CMD_SELECT_PHASE_REG = 0x05
AD9833_CMD_RESET = 0x06
AD9833_CMD_SLEEP = 0x07

# SSD1306 OLED subcommands. CLEAR/SET_CURSOR/PRINT only touch the firmware's
# in-RAM framebuffer -- nothing reaches the panel until DISPLAY is sent, same
# as calling the SSD1306_* functions directly on the ESP side.
OLED_CMD_CLEAR = 0x01
OLED_CMD_SET_CURSOR = 0x02
OLED_CMD_PRINT = 0x03
OLED_CMD_DISPLAY = 0x04
OLED_CMD_SET_CONTRAST = 0x05
OLED_CMD_SET_INVERT = 0x06
OLED_CMD_SET_POWER = 0x07

# PCF8575 I/O expander subcommands. WRITE_PORT/SET_MASK/CLEAR_MASK/TOGGLE_MASK
# carry a u16 LE; the mask forms are read-modify-write against the firmware's
# shadow of the last value *written* (the part has no readable output
# register). READ_PORT and SCAN are queries: like INFO, the request is ACKed
# for delivery only and the answer arrives as a separate DATA frame -- but
# unlike INFO, those replies echo their subcommand in byte0, so they're
# self-describing (see expander.py).
PCF8575_CMD_WRITE_PORT = 0x01
PCF8575_CMD_WRITE_PIN = 0x02
PCF8575_CMD_SET_MASK = 0x03
PCF8575_CMD_CLEAR_MASK = 0x04
PCF8575_CMD_TOGGLE_MASK = 0x05
PCF8575_CMD_READ_PORT = 0x06
PCF8575_CMD_SET_ADDRESS = 0x07
PCF8575_CMD_SCAN = 0x08

#: The three address pins select one of these eight addresses; all are
#: supported both at build time (Kconfig) and at runtime (SET_ADDRESS).
PCF8575_ADDR_MIN = 0x20
PCF8575_ADDR_MAX = 0x27
PCF8575_PIN_COUNT = 16


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
# Unlike DAC/AD9833 these are *queries*: the request DATA frame is ACKed as
# usual (delivery confirmation only), and the answer comes back as a separate
# DATA frame from (ESP, UART_TASK_ID_INFO) addressed to whatever
# (device, task_id) sent the request -- so the requester must itself have
# registered that task id. See info_bridge_task() in uart_bridge.c.
INFO_CMD_GET_PIN_CONFIG = 0x01
INFO_CMD_GET_FW_VERSION = 0x02


class PinFunction(enum.IntEnum):
    """PIN_FUNC_* function ids in a GET_PIN_CONFIG response entry.

    The wire carries only this id; the human-readable meaning (which driver
    owns the pin) lives PC-side in devices.PIN_FUNCTION_LABELS, deliberately
    keeping the response small -- see the comment above PIN_FUNC_I2C_SDA in
    uart_task_ids.h.
    """

    I2C_SDA = 0x01
    I2C_SCL = 0x02
    UART_TX = 0x03
    UART_RX = 0x04
    SPI_SCLK = 0x05
    SPI_MOSI = 0x06
    SPI_CS = 0x07
    LED_HEARTBEAT = 0x08


class Waveform(enum.IntEnum):
    SINE = 0
    TRIANGLE = 1
    SQUARE = 2
    SQUARE_DIV2 = 3


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
