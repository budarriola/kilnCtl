"""Client for the firmware's THERMO task (task 1): three MAX31856 channels.

Most of this task's subcommands are ordinary fire-and-forget writes, reachable
straight through ``link.send()``. Three of them -- ``READ``, ``READ_FAULTS``
and ``READ_REG`` -- are *queries*, and queries work here the same way they do
on task INFO: the request DATA frame is ACKed for delivery only, and the answer
arrives afterwards as a **separate DATA frame** from
``(ESP, UART_TASK_ID_THERMO)`` addressed back to whoever asked. The PC
therefore has to be registered on task 1 itself to receive anything.

Unlike INFO, the replies echo their subcommand in byte0 (see
``parse_thermo_response``), so no structural guessing is needed. *Like* INFO,
though, there is an unsolicited push to reason about: with ``SET_AUTO_REPORT``
on, the firmware sends READ replies at a fixed period with nobody having
asked. A pushed report and a query answer are byte-identical, so the only
thing that tells them apart is whether a READ was outstanding -- which is why
this client owns task 1's inbox with a single consumer thread instead of
letting each caller drain the queue, exactly as
:class:`~kilnctrl.info.InfoClient` does for the boot push.

Register early (at app construction, not lazily when a window opens) so
auto-report pushes that start before any GUI page exists are not NACKed and
lost.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Callable, Optional

from . import devices
from .devices import OkReason, ThermoFaultStatus, ThermoReading, ThermoRegisters, ThermoResponseError
from .protocol import (
    THERMO_CHANNEL_ALL,
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
    UART_TASK_ID_THERMO,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: A READ is three SPI transfers at most; the firmware answers from whatever
#: the last conversion produced rather than waiting for a new one.
DEFAULT_REPLY_TIMEOUT_S = 2.0

#: How long the mutating-command helpers below wait for an *optional*
#: refusal reply before concluding the write went through. Same shape and
#: same reasoning as io_expander.py's SET_RELAY_REJECT_WINDOW_S:
#: thermo_bridge_task() replies nothing on success for any of these and
#: replies {subcmd, ok=0, reason} only on refusal (truncated/out of
#: range/driver error), decided synchronously with no SPI transfer on the
#: refusal path itself, so a short window is enough.
MUTATING_REJECT_WINDOW_S = 0.5


class ThermoQueryError(RuntimeError):
    """Raised when a THERMO query cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer (the
    request never got an ACK), and None when the request was delivered but no
    valid reply came back in time.
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


class ThermoClient:
    """Owns task :data:`UART_TASK_ID_THERMO` on the PC side of a link.

    Registers the task, drains its inbox on a background thread, satisfies
    outstanding queries, and hands unsolicited auto-report pushes to
    ``on_report``.

    Thread-safety: every query method blocks on a UART round trip and must not
    be called from a GUI thread; ``on_report`` is invoked on the consumer
    thread, so a GUI callback has to marshal back itself.
    """

    def __init__(
        self,
        link: UartLink,
        on_report: Optional[Callable[["list[ThermoReading]"], None]] = None,
        task_id: int = UART_TASK_ID_THERMO,
    ) -> None:
        self.link = link
        self.task_id = task_id
        self.on_report = on_report

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        #: Last readings seen from any source (query or push), so a page that
        #: opens between reports has something to draw immediately.
        self.last_readings: "list[ThermoReading]" = []
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        #: Serializes queries so at most one reply is ever outstanding, which
        #: is what makes "unsolicited == nobody asked" a sound inference.
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-thermo-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 1. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- writes ------------------------------------------------------------
    def send(self, payload: bytes) -> SendResult:
        """Send one non-query subcommand payload (built by ``devices.py``).

        Fire-and-forget: the returned :class:`SendResult` proves delivery to
        the task's inbox only, never that the subcommand switch did
        anything. Prefer :meth:`config_channel`/:meth:`set_thresholds`/
        :meth:`set_cj_offset`/:meth:`one_shot`/:meth:`clear_faults`/
        :meth:`set_auto_report`/:meth:`write_reg` instead -- they wait out a
        short window for the optional refusal reply thermo_bridge_task() now
        sends on a truncated frame, an out-of-range argument, or a driver
        error (ROADMAP.md "KilnFW PC-link command acknowledgement").
        """
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id, payload=payload
        )

    def config_channel(
        self,
        channel: int,
        tc_type: int = 3,
        avg_mode: int = 0,
        filter_50hz: bool = False,
        auto_convert: bool = True,
        timeout: float = MUTATING_REJECT_WINDOW_S,
    ) -> OkReason:
        """0x01 CONFIG_CHANNEL, and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_CONFIG_CHANNEL,
            devices.thermo_config_channel(
                channel, tc_type, avg_mode, filter_50hz, auto_convert
            ),
            timeout,
        )

    def set_thresholds(
        self,
        channel: int,
        tc_high: float,
        tc_low: float,
        cj_high: int,
        cj_low: int,
        timeout: float = MUTATING_REJECT_WINDOW_S,
    ) -> OkReason:
        """0x02 SET_THRESHOLDS, and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_SET_THRESHOLDS,
            devices.thermo_set_thresholds(channel, tc_high, tc_low, cj_high, cj_low),
            timeout,
        )

    def set_cj_offset(
        self, channel: int, offset_c: float, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x03 SET_CJ_OFFSET, and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_SET_CJ_OFFSET, devices.thermo_set_cj_offset(channel, offset_c), timeout
        )

    def one_shot(self, channel: int, timeout: float = MUTATING_REJECT_WINDOW_S) -> OkReason:
        """0x04 ONE_SHOT, and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_ONE_SHOT, devices.thermo_one_shot(channel), timeout
        )

    def clear_faults(self, channel: int, timeout: float = MUTATING_REJECT_WINDOW_S) -> OkReason:
        """0x07 CLEAR_FAULTS, and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_CLEAR_FAULTS, devices.thermo_clear_faults(channel), timeout
        )

    def set_auto_report(
        self, channel_mask: int, period_ms: int, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x08 SET_AUTO_REPORT, and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_SET_AUTO_REPORT,
            devices.thermo_set_auto_report(channel_mask, period_ms),
            timeout,
        )

    def write_reg(
        self, channel: int, reg: int, value: int, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x0A WRITE_REG (debug), and learn *why* if the firmware refuses."""
        return self._send_reject_window(
            THERMO_CMD_WRITE_REG, devices.thermo_write_reg(channel, reg, value), timeout
        )

    def _send_reject_window(self, subcommand: int, payload: bytes, timeout: float) -> OkReason:
        """Send a mutating subcommand and wait out ``timeout`` for the
        *optional* refusal reply, same shape as io_expander.py's
        ``_set_relay_style``: a reply within the window IS the refusal;
        silence means the command went through.
        """
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
                    raise ThermoQueryError(
                        f"THERMO request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if pending.event.wait(timeout):
                    return pending.value  # type: ignore[return-value]
                return OkReason(ok=True)
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- queries -----------------------------------------------------------
    def read(
        self, channel: int = THERMO_CHANNEL_ALL, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> "list[ThermoReading]":
        """Read one channel (or all three) -- temperature, cold junction, faults."""
        value = self._query(THERMO_CMD_READ, devices.thermo_read(channel), timeout)
        return value  # type: ignore[return-value]

    def read_faults(
        self, channel: int = THERMO_CHANNEL_ALL, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> "list[ThermoFaultStatus]":
        """Read the fault status (SR) and MASK registers without a conversion."""
        value = self._query(
            THERMO_CMD_READ_FAULTS, devices.thermo_read_faults(channel), timeout
        )
        return value  # type: ignore[return-value]

    def read_reg(
        self,
        channel: int,
        reg: int,
        length: int = 1,
        timeout: float = DEFAULT_REPLY_TIMEOUT_S,
    ) -> ThermoRegisters:
        """Raw register read from one MAX31856 (debug).

        Raises :class:`ThermoQueryError` if the firmware answers with zero
        data bytes -- the reply firmware now sends (uart_bridge.c,
        THERMO_CMD_READ_REG case, 2026-08-20) when the channel's owner-side
        read failed (e.g. the channel never came up: no thermocouple
        daughterboard attached, see thermo_owner.h's BENCH NOTE) instead of
        the old silent timeout. A 0-length body is a definite "could not
        read this register," never a legitimate answer -- the caller always
        asks for at least 1 byte (``devices.thermo_read_reg``'s ``length``
        range is 1..16), so an empty ``data`` can only be the firmware's
        failure marker.
        """
        value = self._query(
            THERMO_CMD_READ_REG, devices.thermo_read_reg(channel, reg, length), timeout
        )
        registers = value  # type: ThermoRegisters
        if not registers.data:
            raise ThermoQueryError(
                f"THERMO READ_REG ch{channel} reg 0x{reg:02X}: firmware reported "
                "0 data bytes -- the channel's SPI read failed (channel never "
                "came up, e.g. no thermocouple daughterboard attached)"
            )
        return registers

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
                    raise ThermoQueryError(
                        f"THERMO request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise ThermoQueryError(
                        f"THERMO request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s (the firmware sends no reply "
                        "when its own SPI transfer fails -- check the Device Console)"
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
                log.exception("error handling THERMO frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_thermo_response(frame.payload)
        except ThermoResponseError as exc:
            log.warning("dropping malformed THERMO response: %s", exc)
            return

        if subcommand == THERMO_CMD_READ and isinstance(value, list):
            self.last_readings = value

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding for this subcommand. A READ reply here is an
        # auto-report push (SET_AUTO_REPORT is on); anything else is a stale
        # reply to a query we already gave up on, so drop it.
        if subcommand == THERMO_CMD_READ and isinstance(value, list):
            if self.on_report is not None:
                self.on_report(value)
        else:
            log.debug("ignoring unsolicited THERMO response 0x%02X", subcommand)
