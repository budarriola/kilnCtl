"""Client for the firmware's PCF8575 task (task 7): 16-bit I/O expander.

Most of this task's subcommands are ordinary fire-and-forget writes, reachable
straight through ``link.send()`` like the DAC/AD9833/OLED ones. Two of them --
``READ_PORT`` and ``SCAN`` -- are *queries*, and queries work here the same way
they do on task INFO: the request DATA frame is ACKed for delivery only, and
the answer arrives afterwards as a **separate DATA frame** from
``(ESP, UART_TASK_ID_PCF8575)`` addressed back to whoever asked. The PC
therefore has to be registered on task 7 itself to receive anything.

Unlike INFO there is no unsolicited push on this task, and the replies do carry
their subcommand back in byte0 (see ``parse_pcf8575_response``), so this client
is a simpler shape than :class:`~uart_control.info.InfoClient`: one outstanding
query at a time, matched by subcommand, no boot-push inference to make.

Writes go through :meth:`send` here too rather than a bare ``link.send`` so
there is exactly one place that knows this task's ``(dst_task, src_task)``
pairing.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import ExpanderPort, ExpanderResponseError
from .protocol import (
    PCF8575_CMD_READ_PORT,
    PCF8575_CMD_SCAN,
    UART_TASK_ID_PCF8575,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: A READ_PORT costs one 2-byte I2C read; a SCAN probes eight addresses with a
#: 50 ms per-address timeout, so its worst case is noticeably longer.
DEFAULT_REPLY_TIMEOUT_S = 2.0
SCAN_REPLY_TIMEOUT_S = 4.0


class ExpanderQueryError(RuntimeError):
    """Raised when a PCF8575 query cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer (the
    request never got an ACK), and None when the request was delivered but no
    valid reply came back in time -- which for this task also covers "the
    firmware's I2C transfer failed", since a failed device operation is logged
    on the ESP and simply produces no reply frame.
    """

    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    """A single outstanding query: what we asked for, and where to put it."""

    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class ExpanderClient:
    """Owns task :data:`UART_TASK_ID_PCF8575` on the PC side of a link.

    Thread-safety: every method here blocks on a UART round trip and must not
    be called from a GUI thread.
    """

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_PCF8575) -> None:
        self.link = link
        self.task_id = task_id

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        #: Serializes queries so at most one reply is ever outstanding.
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-pcf8575-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 7. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- writes ------------------------------------------------------------
    def send(self, payload: bytes) -> SendResult:
        """Send one non-query subcommand payload (built by ``devices.py``)."""
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id, payload=payload
        )

    # -- queries -----------------------------------------------------------
    def read_port(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> ExpanderPort:
        """Read the live pin states, plus the firmware's output shadow and the
        address it is currently talking to."""
        value = self._query(PCF8575_CMD_READ_PORT, devices.pcf8575_read_port(), timeout)
        return value  # type: ignore[return-value]

    def scan(self, timeout: float = SCAN_REPLY_TIMEOUT_S) -> "list[int]":
        """Probe 0x20-0x27 on the device's I2C bus; return the addresses that
        ACKed (the eight a PCF8575's address pins can select)."""
        value = self._query(PCF8575_CMD_SCAN, devices.pcf8575_scan(), timeout)
        return value  # type: ignore[return-value]

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> object:
        with self._query_lock:
            pending = _Pending(subcommand)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self.link.send(
                    dst_task=self.task_id,
                    src_task=self.task_id,
                    payload=payload,
                    dst_device=Device.ESP,
                )
                if not result.ok:
                    raise ExpanderQueryError(
                        f"PCF8575 request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise ExpanderQueryError(
                        f"PCF8575 request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s (the firmware sends no reply "
                        "when its own I2C transfer fails -- check the Device Console)"
                    )
                return pending.value
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- receive -----------------------------------------------------------
    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue  # poll interval so close() is noticed promptly
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling PCF8575 frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_pcf8575_response(frame.payload)
        except ExpanderResponseError as exc:
            log.warning("dropping malformed PCF8575 response: %s", exc)
            return

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding for this subcommand: a stale reply to a query we
        # already gave up on. Nothing on this task is ever pushed unsolicited.
        log.debug("ignoring unsolicited PCF8575 response 0x%02X", subcommand)
