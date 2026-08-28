"""pyserial-backed host implementation of the uart_protocol link layer.

PC-side mirror of ``uart_protocol.c``:

* a background reader thread drains the port and runs the same SLIP framing
  state machine (:class:`~uart_control.protocol.FrameDecoder`);
* CRC failures are dropped silently -- the sender's retransmit recovers them;
* :meth:`UartLink.send` transmits a DATA frame and blocks for a matching
  ACK/NACK, retransmitting the *same* MSG_INDEX up to
  ``UART_PROTO_MAX_RETRIES`` (10) times, serialized by a TX lock exactly like
  the firmware's ``tx_lock``;
* inbound DATA frames for a registered task are deduped against a small ring of
  recently seen (src_device, src_task, msg_index) tuples and ACKed; frames for
  an unregistered task are NACKed ("undeliverable").

Also provides COM-port autodiscovery. The board exposes this UART on a
*separate* USB-C port from the ESP32-S3's built-in USB-Serial-JTAG peripheral;
scoring therefore rejects anything advertising JTAG and favours the usual
USB-UART bridge silicon (CP210x, CH340/CH9102, FTDI, generic "USB to UART").
"""

from __future__ import annotations

import logging
import queue
import secrets
import threading
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

import serial
from serial.tools import list_ports as _list_ports

from .protocol import (
    DEFAULT_BAUD_RATE,
    UART_PROTO_DEDUP_DEPTH,
    UART_PROTO_MAX_PAYLOAD,
    UART_PROTO_MAX_RETRIES,
    Device,
    Frame,
    FrameDecoder,
    FrameError,
    MsgType,
)

log = logging.getLogger(__name__)

#: Per-attempt ACK wait. The firmware defaults to 200 ms, but a USB-serial
#: round trip through a host driver deserves more headroom; overridable.
DEFAULT_ACK_TIMEOUT_S = 0.4


def _random_msg_index() -> int:
    """A random starting MSG_INDEX for a host session.

    Not cosmetic. The firmware dedups inbound DATA against a ring of the last
    ``UART_PROTO_DEDUP_DEPTH`` (4) ``(src_device, src_task, msg_index)`` tuples
    *per task*, and that ring lives as long as the board stays powered -- it
    has no notion of "the host restarted". Starting every host session at
    index 0 therefore made the first few sends of a new session look like
    retransmits of the previous session's: the firmware re-ACKed them
    **without delivering them to the task**, which on a query task shows up as
    the maddening "request 0x02 was ACKed but no reply arrived" -- a delivered,
    acknowledged, silently discarded message.

    It went unnoticed while opening the port also reset the board (see
    connect()), since a reboot cleared the rings; fixing that reset exposed
    this. Randomizing the start makes a collision a 4-in-65536 accident per
    task instead of a certainty, the same reasoning behind randomized TCP
    initial sequence numbers.
    """
    return secrets.randbelow(0x10000)


class SendResult(str, Enum):
    """Outcome of a send-and-await-ack cycle (mirrors uart_protocol_send)."""

    OK = "ok"  # ACKed: delivered to the destination task's inbox
    UNDELIVERABLE = "undeliverable"  # NACKed: dst task not registered there
    TIMEOUT = "timeout"  # no reply after all retries (link/peer down)
    NOT_CONNECTED = "not_connected"  # local: no port open

    @property
    def ok(self) -> bool:
        return self is SendResult.OK

    def describe(self) -> str:
        return {
            SendResult.OK: "delivered to the destination task's inbox (ACK)",
            SendResult.UNDELIVERABLE: "destination task not registered on the peer (NACK)",
            SendResult.TIMEOUT: "no reply after all retries - link or peer is down",
            SendResult.NOT_CONNECTED: "no serial port open - connect first",
        }[self]


# ---------------------------------------------------------------------------
# port discovery
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class PortInfo:
    device: str
    description: str
    manufacturer: str
    hwid: str
    score: int

    @property
    def label(self) -> str:
        return f"{self.device} - {self.description}" if self.description else self.device

    @property
    def recommended(self) -> bool:
        return self.score > 0


#: (substring, score delta). Matched case-insensitively against the port's
#: description + manufacturer + hwid.
_PORT_HINTS: tuple[tuple[str, int], ...] = (
    # The ESP32-S3's native USB-Serial-JTAG port is NOT our UART.
    ("jtag", -100),
    ("usb-serial-jtag", -100),
    ("debug", -20),
    # External USB-UART bridge silicon: this is the port we want.
    ("cp210", 50),
    ("cp2102", 10),
    ("cp2104", 10),
    ("silicon labs", 20),
    ("ch340", 50),
    ("ch910", 50),
    ("ch343", 50),
    ("wch", 20),
    ("ft232", 50),
    ("ftdi", 30),
    ("future technology", 20),
    ("usb-serial", 25),
    ("usb serial", 25),
    ("usb to uart", 40),
    ("usb-to-uart", 40),
    ("uart bridge", 30),
    ("prolific", 20),
    ("pl2303", 30),
)

#: USB VID:PID prefixes for common bridge chips (extra confidence).
_VID_HINTS: tuple[tuple[str, int], ...] = (
    ("10C4:EA60", 40),  # Silicon Labs CP210x
    ("1A86:7523", 40),  # WCH CH340
    ("1A86:55D4", 40),  # WCH CH9102
    ("1A86:55D3", 40),  # WCH CH343 (this board's UART bridge)
    ("0403:6001", 40),  # FTDI FT232R
    ("0403:6015", 40),  # FTDI FT231X
    ("303A:1001", -80),  # Espressif native USB-Serial-JTAG
)


def _score_port(port) -> int:
    haystack = " ".join(
        str(x or "") for x in (port.description, port.manufacturer, port.product, port.hwid)
    ).lower()
    score = 0
    for needle, delta in _PORT_HINTS:
        if needle in haystack:
            score += delta
    upper = str(port.hwid or "").upper()
    for vidpid, delta in _VID_HINTS:
        if vidpid in upper:
            score += delta
    return score


def list_ports() -> list[PortInfo]:
    """All serial ports, best candidate first (score desc, then device name)."""
    infos = [
        PortInfo(
            device=p.device,
            description=p.description or "",
            manufacturer=p.manufacturer or "",
            hwid=p.hwid or "",
            score=_score_port(p),
        )
        for p in _list_ports.comports()
    ]
    infos.sort(key=lambda i: (-i.score, i.device))
    return infos


def recommend_port() -> Optional[str]:
    """Best-guess COM port for the USB-UART bridge, or None if nothing scores."""
    for info in list_ports():
        if info.score > 0:
            return info.device
    return None


# ---------------------------------------------------------------------------
# link
# ---------------------------------------------------------------------------
@dataclass
class _TaskSlot:
    task_id: int
    inbox: "queue.Queue[Frame]"
    dedup: list[Optional[tuple[int, int, int]]] = field(
        default_factory=lambda: [None] * UART_PROTO_DEDUP_DEPTH
    )
    dedup_next: int = 0

    def check_and_record(self, src_device: int, src_task: int, msg_index: int) -> bool:
        """True if this exact (src_device, src_task, msg_index) was already seen."""
        key = (int(src_device), int(src_task), int(msg_index))
        if key in self.dedup:
            return True
        self.dedup[self.dedup_next] = key
        self.dedup_next = (self.dedup_next + 1) % UART_PROTO_DEDUP_DEPTH
        return False


class UartLink:
    """Reliable, addressed message link to the ESP32-S3 over a serial port."""

    def __init__(
        self,
        own_device: Device = Device.HOST,
        baudrate: int = DEFAULT_BAUD_RATE,
        ack_timeout: float = DEFAULT_ACK_TIMEOUT_S,
        max_retries: int = UART_PROTO_MAX_RETRIES,
    ) -> None:
        self.own_device = own_device
        self.baudrate = baudrate
        self.ack_timeout = ack_timeout
        self.max_retries = max_retries

        self._serial: Optional[serial.Serial] = None
        self._port: Optional[str] = None
        self._decoder = FrameDecoder()

        self._reader: Optional[threading.Thread] = None
        self._stop = threading.Event()

        self._tasks: dict[int, _TaskSlot] = {}
        self._tasks_lock = threading.Lock()

        # Serializes the whole send-and-wait-for-ack cycle (firmware tx_lock).
        self._tx_lock = threading.RLock()
        self._write_lock = threading.Lock()  # guards raw port writes (ACKs vs DATA)
        self._next_tx_index = _random_msg_index()

        self._await_lock = threading.Lock()
        self._awaited: Optional[tuple[int, int, int]] = None  # (device, task, index)
        self._ack_result: Optional[MsgType] = None
        self._ack_event = threading.Event()

    # -- lifecycle ---------------------------------------------------------
    @property
    def is_connected(self) -> bool:
        return self._serial is not None and self._serial.is_open

    @property
    def port(self) -> Optional[str]:
        return self._port

    def connect(self, port: Optional[str] = None) -> str:
        """Open ``port`` (or the autodiscovered recommendation) and start RX.

        Returns the port actually opened. Raises RuntimeError if no port was
        given and autodiscovery found no plausible USB-UART bridge.
        """
        if self.is_connected:
            raise RuntimeError(f"already connected to {self._port}")

        if port is None:
            port = recommend_port()
            if port is None:
                raise RuntimeError(
                    "no USB-UART bridge port found; pass an explicit port "
                    "(note: the ESP32-S3 native USB-Serial-JTAG port is not this UART)"
                )

        # Constructed *without* a port so it stays closed, then opened
        # explicitly below. This matters: the board's USB-UART bridge straps
        # EN/BOOT off DTR/RTS (the esptool auto-reset circuit), and passing
        # port= to the constructor opens the port with both lines asserted at
        # their driver defaults -- which pulses EN and reboots the ESP32-S3.
        # Deasserting them afterwards is too late; the reset already happened,
        # and every first query after a connect died against a rebooting board
        # ("ACKed but no reply"), recovering only because the firmware's
        # once-per-boot version push happened to land a moment later.
        # Assigning dtr/rts while closed instead records the desired state,
        # which pyserial applies as part of opening the port, so the lines are
        # never asserted in the first place.
        ser = serial.Serial(
            baudrate=self.baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.2,  # poll interval so the reader notices shutdown
            write_timeout=1.0,
            rtscts=False,
            dsrdtr=False,
            xonxoff=False,
        )
        try:
            ser.dtr = False
            ser.rts = False
        except (OSError, serial.SerialException):  # pragma: no cover - driver dependent
            # Some backends refuse this while closed; the post-open deassert
            # below is the fallback (it still stops the lines being *held*
            # asserted, it just can't prevent the open-time pulse).
            log.debug("could not preset DTR/RTS before open", exc_info=True)
        ser.port = port
        ser.open()
        try:
            ser.dtr = False
            ser.rts = False
        except (OSError, serial.SerialException):  # pragma: no cover - driver dependent
            pass
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        self._serial = ser
        self._port = port
        # Fresh session, fresh index space -- a reconnect to a board that has
        # been up the whole time is exactly the case the firmware's dedup ring
        # cannot distinguish from a retransmit (see _random_msg_index).
        with self._tx_lock:
            self._next_tx_index = _random_msg_index()
        self._decoder.reset()
        self._stop.clear()
        self._reader = threading.Thread(
            target=self._rx_loop, name="uart-proto-rx", daemon=True
        )
        self._reader.start()
        log.info("connected to %s @ %d baud", port, self.baudrate)
        return port

    def disconnect(self) -> None:
        """Stop the reader thread and close the port. Safe to call twice."""
        self._stop.set()
        reader, self._reader = self._reader, None
        ser, self._serial = self._serial, None
        if reader is not None and reader is not threading.current_thread():
            reader.join(timeout=2.0)
        if ser is not None:
            try:
                ser.close()
            except Exception:  # pragma: no cover
                log.debug("error closing port", exc_info=True)
        log.info("disconnected from %s", self._port)
        self._port = None

    def close(self) -> None:
        self.disconnect()

    def __enter__(self) -> "UartLink":
        return self

    def __exit__(self, *exc) -> None:
        self.disconnect()

    def status(self) -> dict:
        return {
            "connected": self.is_connected,
            "port": self._port,
            "baudrate": self.baudrate,
            "own_device": int(self.own_device),
            "ack_timeout_s": self.ack_timeout,
            "max_retries": self.max_retries,
            "registered_tasks": sorted(self._tasks),
        }

    # -- task registry -----------------------------------------------------
    def register_task(self, task_id: int, inbox_len: int = 8) -> "queue.Queue[Frame]":
        """Register ``task_id`` as a valid local destination; returns its inbox."""
        with self._tasks_lock:
            if task_id in self._tasks:
                raise ValueError(f"task {task_id} already registered")
            slot = _TaskSlot(task_id=task_id, inbox=queue.Queue(maxsize=inbox_len))
            self._tasks[task_id] = slot
            return slot.inbox

    def unregister_task(self, task_id: int) -> None:
        with self._tasks_lock:
            self._tasks.pop(task_id, None)

    # -- transmit ----------------------------------------------------------
    def send(
        self,
        dst_task: int,
        src_task: int,
        payload: bytes = b"",
        dst_device: Device = Device.ESP,
        timeout: Optional[float] = None,
    ) -> SendResult:
        """Send a DATA frame and block until ACK/NACK or all retries time out.

        NOTE: :attr:`SendResult.OK` only means the frame reached the destination
        task's *inbox*. uart_bridge.c never reports application-level results
        back over the link, so this cannot tell you whether the underlying I2C /
        SPI device operation actually succeeded.
        """
        if len(payload) > UART_PROTO_MAX_PAYLOAD:
            raise ValueError(
                f"payload too long: {len(payload)} > {UART_PROTO_MAX_PAYLOAD}"
            )
        if not self.is_connected:
            return SendResult.NOT_CONNECTED

        ack_timeout = self.ack_timeout if timeout is None else timeout

        with self._tx_lock:
            msg_index = self._next_tx_index & 0xFFFF
            self._next_tx_index = (self._next_tx_index + 1) & 0xFFFF

            frame = Frame(
                msg_type=MsgType.DATA,
                msg_index=msg_index,
                src_device=self.own_device,
                src_task=src_task,
                dst_device=dst_device,
                dst_task=dst_task,
                payload=payload,
            )
            wire = frame.to_wire()

            with self._await_lock:
                self._awaited = (int(dst_device), int(dst_task), msg_index)
                self._ack_result = None
                self._ack_event.clear()

            try:
                for attempt in range(1, self.max_retries + 1):
                    if not self._write(wire):
                        continue  # TX itself failed; still worth retrying
                    if self._ack_event.wait(ack_timeout):
                        result = self._ack_result
                        return (
                            SendResult.OK
                            if result is MsgType.ACK
                            else SendResult.UNDELIVERABLE
                        )
                    log.warning(
                        "no reply for msg %u to dev%s/task%s, retry %d/%d",
                        msg_index,
                        int(dst_device),
                        dst_task,
                        attempt,
                        self.max_retries,
                    )
                return SendResult.TIMEOUT
            finally:
                with self._await_lock:
                    self._awaited = None

    def _write(self, data: bytes) -> bool:
        ser = self._serial
        if ser is None or not ser.is_open:
            return False
        try:
            with self._write_lock:
                ser.write(data)
                ser.flush()
            return True
        except (serial.SerialException, OSError):
            log.warning("frame tx failed", exc_info=True)
            return False

    def _send_control_frame(
        self,
        msg_type: MsgType,
        msg_index: int,
        dst_device: int,
        dst_task: int,
        src_task: int,
    ) -> None:
        """Reply ACK/NACK. src_task is the task we were addressed *as*, matching
        ``send_control_frame()`` in uart_protocol.c."""
        self._write(
            Frame(
                msg_type=msg_type,
                msg_index=msg_index,
                src_device=self.own_device,
                src_task=src_task,
                dst_device=dst_device,
                dst_task=dst_task,
            ).to_wire()
        )

    # -- receive -----------------------------------------------------------
    def _rx_loop(self) -> None:
        ser = self._serial
        while not self._stop.is_set():
            try:
                if ser is None or not ser.is_open:
                    break
                chunk = ser.read(max(1, ser.in_waiting or 1))
            except (serial.SerialException, OSError, TypeError):
                if not self._stop.is_set():
                    log.warning("serial read failed; reader thread exiting", exc_info=True)
                break
            if not chunk:
                continue  # read timeout is just a poll interval
            for raw in self._decoder.feed(chunk):
                try:
                    frame = Frame.from_raw(raw)
                except FrameError as exc:
                    log.debug("dropping frame: %s", exc)
                    continue  # silent drop; the sender's retry recovers it
                try:
                    self._handle_frame(frame)
                except Exception:  # pragma: no cover - never kill the reader
                    log.exception("error handling frame")

    def _handle_frame(self, frame: Frame) -> None:
        """Mirror of ``handle_raw_frame()`` (post CRC/length validation)."""
        if frame.msg_type in (MsgType.ACK, MsgType.NACK):
            # A reply to something *we* sent: the replier is our original dst.
            with self._await_lock:
                awaited = self._awaited
                if awaited is not None and awaited == (
                    int(frame.src_device),
                    int(frame.src_task),
                    frame.msg_index,
                ):
                    self._ack_result = frame.msg_type
                    self._ack_event.set()
            return

        if frame.msg_type is not MsgType.DATA:
            return

        if int(frame.dst_device) != int(self.own_device):
            return  # not for us (shouldn't happen point-to-point, but be safe)

        with self._tasks_lock:
            slot = self._tasks.get(frame.dst_task)
            if slot is None:
                reply: Optional[MsgType] = MsgType.NACK
            elif slot.check_and_record(
                int(frame.src_device), int(frame.src_task), frame.msg_index
            ):
                # Retransmit of something we already delivered (our earlier ACK
                # was lost): re-ACK without pushing to the inbox again.
                reply = MsgType.ACK
            else:
                try:
                    slot.inbox.put_nowait(frame)
                    reply = MsgType.ACK
                except queue.Full:
                    # Inbox full: withhold the ACK. The sender's retry gives the
                    # consumer time to drain before we accept this message.
                    log.warning(
                        "inbox full for task %s, withholding ACK (sender will retry)",
                        frame.dst_task,
                    )
                    reply = None

        if reply is None:
            return
        if reply is MsgType.NACK:
            log.warning(
                "dst task %s not registered, replying NACK (undeliverable)", frame.dst_task
            )
        self._send_control_frame(
            reply,
            frame.msg_index,
            int(frame.src_device),
            frame.src_task,
            frame.dst_task,
        )
