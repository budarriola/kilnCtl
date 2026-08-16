"""Client for the firmware's IO task (task 2): the SX1509 expander.

The SX1509 (U5, 0x3E) owns everything slow on this board -- the four relay
drives, the seven general-purpose digital I/O, the three thermocouple ~DRDY
inputs, and the display's D/C and ~RESET lines. Most of this task's
subcommands are ordinary fire-and-forget writes, reachable straight through
``link.send()``. Three of them -- ``READ``, ``SX_READ_REG`` and ``SX_SCAN`` --
are *queries*, with the same shape as the ones on task INFO: the request DATA
frame is ACKed for delivery only, and the answer arrives afterwards as a
**separate DATA frame** from ``(ESP, UART_TASK_ID_IO)`` addressed back to
whoever asked. The PC therefore has to be registered on task 2 itself to
receive anything.

The replies echo their subcommand in byte0 (see ``parse_io_response``), so no
structural guessing is needed. But like THERMO -- and unlike the fixture's old
expander task -- there *is* an unsolicited push: with ``SET_AUTO_REPORT`` on,
the firmware sends READ replies at a fixed period and, additionally,
immediately on every ~INT edge, so an input change is reported without waiting
out the period. A push and a query answer are byte-identical, so "nobody
asked" is the only thing that distinguishes them, which is why this client
owns task 2's inbox with a single consumer thread.

Writes go through :meth:`send` here rather than a bare ``link.send`` so there
is exactly one place that knows this task's ``(dst_task, src_task)`` pairing.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Callable, Optional

from . import devices
from .devices import ExpanderRegisters, IoResponseError, IoState
from .protocol import (
    IO_CMD_READ,
    IO_CMD_SX_READ_REG,
    IO_CMD_SX_SCAN,
    UART_TASK_ID_IO,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: A READ is two short I2C reads; a SCAN probes four addresses with a
#: per-address timeout, so its worst case is noticeably longer.
DEFAULT_REPLY_TIMEOUT_S = 2.0
SCAN_REPLY_TIMEOUT_S = 4.0


class IoQueryError(RuntimeError):
    """Raised when an IO query cannot be completed.

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


class IoClient:
    """Owns task :data:`UART_TASK_ID_IO` on the PC side of a link.

    Registers the task, drains its inbox on a background thread, satisfies
    outstanding queries, and hands unsolicited auto-report pushes (including
    the ~INT-edge ones) to ``on_report``.

    Thread-safety: every query method blocks on a UART round trip and must not
    be called from a GUI thread; ``on_report`` is invoked on the consumer
    thread, so a GUI callback has to marshal back itself.
    """

    def __init__(
        self,
        link: UartLink,
        on_report: Optional[Callable[[IoState], None]] = None,
        task_id: int = UART_TASK_ID_IO,
    ) -> None:
        self.link = link
        self.task_id = task_id
        self.on_report = on_report

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        #: Last state seen from any source (query or push), so a page that
        #: opens between reports has something to draw immediately.
        self.last_state: Optional[IoState] = None
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        #: Serializes queries so at most one reply is ever outstanding, which
        #: is what makes "unsolicited == nobody asked" a sound inference.
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-io-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 2. Safe to call twice."""
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
    def read(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> IoState:
        """Read the whole expander: raw registers, relays, digital I/O, DRDY."""
        value = self._query(IO_CMD_READ, devices.io_read(), timeout)
        return value  # type: ignore[return-value]

    def read_reg(
        self, reg: int, length: int = 1, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> ExpanderRegisters:
        """Raw SX1509 register read (debug)."""
        value = self._query(
            IO_CMD_SX_READ_REG, devices.sx_read_reg(reg, length), timeout
        )
        return value  # type: ignore[return-value]

    def scan(self, timeout: float = SCAN_REPLY_TIMEOUT_S) -> "list[int]":
        """Probe the four addresses an SX1509's ADDR1/ADDR0 pins can select
        (0x3E/0x3F/0x70/0x71); return the ones that ACKed.

        Unlike the fixture's PCF8575 this is diagnostic only -- the board's
        address pins are strapped to GND, so 0x3E is the only address that can
        answer, and anything else answering means the wrong part is fitted.
        """
        value = self._query(IO_CMD_SX_SCAN, devices.sx_scan(), timeout)
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
                    raise IoQueryError(
                        f"IO request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise IoQueryError(
                        f"IO request 0x{subcommand:02X} was ACKed but no reply "
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
                log.exception("error handling IO frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_io_response(frame.payload)
        except IoResponseError as exc:
            log.warning("dropping malformed IO response: %s", exc)
            return

        if subcommand == IO_CMD_READ and isinstance(value, IoState):
            self.last_state = value

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding for this subcommand. A READ reply here is an
        # auto-report push (periodic, or an ~INT edge); anything else is a
        # stale reply to a query we already gave up on, so drop it.
        if subcommand == IO_CMD_READ and isinstance(value, IoState):
            if self.on_report is not None:
                self.on_report(value)
        else:
            log.debug("ignoring unsolicited IO response 0x%02X", subcommand)
