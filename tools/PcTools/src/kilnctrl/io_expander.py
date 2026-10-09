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
from .devices import ExpanderRegisters, IoResponseError, IoState, RelayResult
from .protocol import (
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

#: How long set_relay()/set_relay_mask() wait for an optional refusal reply
#: before concluding the write went through. io_bridge_task() only ever
#: replies to these two subcommands when it refuses them (owned by a
#: profile / safety fault asserted / OTA in progress / truncated / out of
#: range / driver error -- see uart_bridge.c's bridge_reply_reject() call
#: sites in IO_CMD_SET_RELAY/SET_RELAY_MASK); a refusal is decided
#: synchronously against in-RAM state before any I2C transfer, so it comes
#: back fast if it comes back at all. This is sized with real margin over a
#: normal round trip, not over BRIDGE_REPLY_ACK_TIMEOUT_MS (that timeout
#: governs the firmware's own wait for *our* transport ACK of its reply, a
#: different leg of the trip).
SET_RELAY_REJECT_WINDOW_S = 0.5


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
        """Send one non-query subcommand payload (built by ``devices.py``).

        Fire-and-forget: the returned :class:`SendResult` proves delivery to
        the task's inbox only, never that the subcommand switch did
        anything. Every write below (:meth:`set_relay`/:meth:`set_relay_mask`
        and the plain :meth:`set_io`/:meth:`set_io_dir`/:meth:`set_auto_report`/
        :meth:`all_relays_off`/:meth:`sx_write_reg`/:meth:`sx_set_dir`/
        :meth:`sx_set_pullup`/:meth:`sx_set_opendrain`/:meth:`sx_set_debounce`/
        :meth:`sx_set_int_mask`/:meth:`sx_led_driver`/:meth:`sx_reset`) should
        go through its own method instead of this one -- see their
        docstrings for why plain ``send()`` cannot distinguish "it happened"
        from "refused because a profile owns it" / "safety is faulted" /
        "truncated" / "out of range" / "driver error"
        (ROADMAP.md "KilnFW PC-link command acknowledgement").
        """
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id, payload=payload
        )

    def set_relay(
        self, relay: int, on: bool, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> RelayResult:
        """Set relay 1-4 on/off, and learn *why* if the firmware refuses.

        IO_CMD_SET_RELAY replies nothing at all when the write actually
        happens; it replies ``{subcmd, ok=0, reason}`` when refused --
        owned by a running profile, a safety fault is asserted, an OTA is in
        progress, the frame was truncated, the relay index is out of range,
        or the SX1509 write itself failed (uart_bridge.c's io_bridge_task(),
        IO_CMD_SET_RELAY case). This waits out :data:`SET_RELAY_REJECT_WINDOW_S`
        for that optional reply: if one arrives, it IS the refusal; if
        nothing arrives in that window, the write went through.

        Raises :class:`IoQueryError` only if the request itself was not
        delivered (no transport ACK) -- a *refusal* is a normal
        ``RelayResult(ok=False, ...)`` return, not an exception, so a caller
        that only checks ``if not result:`` still gets a clean signal.
        """
        return self._set_relay_style(
            IO_CMD_SET_RELAY, devices.io_set_relay(relay, on), timeout
        )

    def set_relay_mask(
        self, mask: int, value: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> RelayResult:
        """Set several relays (bits 0-3 = Relay1..Relay4) in one write.

        Same silent-success / {subcmd, ok=0, reason} refusal shape as
        :meth:`set_relay` -- see that docstring.
        """
        return self._set_relay_style(
            IO_CMD_SET_RELAY_MASK, devices.io_set_relay_mask(mask, value), timeout
        )

    def _set_relay_style(self, subcommand: int, payload: bytes, timeout: float) -> RelayResult:
        on_silence = RelayResult(ok=True, reason_text=None, refusal=devices.RelayRefusal.OTHER)
        return self._write_style(subcommand, payload, timeout, on_silence)

    # -- other IO writes (truncated/out-of-range/safety/driver-error can all
    # refuse these too, same as the relay commands above -- see
    # parse_io_response()'s _IO_WRITE_SUBCOMMANDS) --------------------------
    def set_io(
        self, io: int, level: bool, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Drive one of the seven general-purpose digital outputs."""
        return self._write_style(IO_CMD_SET_IO, devices.io_set_io(io, level), timeout)

    def set_io_dir(
        self, io: int, is_input: bool, pullup: bool, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Set one digital I/O's direction, and its pull-up if it's an input."""
        return self._write_style(
            IO_CMD_SET_IO_DIR, devices.io_set_io_dir(io, is_input, pullup), timeout
        )

    def set_auto_report(
        self, period_ms: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Enable/disable (period_ms=0) the periodic + ~INT-edge READ push."""
        return self._write_style(
            IO_CMD_SET_AUTO_REPORT, devices.io_set_auto_report(period_ms), timeout
        )

    def all_relays_off(self, timeout: float = SET_RELAY_REJECT_WINDOW_S) -> "devices.OkReason":
        """Force all four relays off in one call, bypassing individual ownership."""
        return self._write_style(
            IO_CMD_ALL_RELAYS_OFF, devices.io_all_relays_off(), timeout
        )

    def sx_write_reg(
        self, reg: int, value: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Raw SX1509 register write (debug) -- refused if it would touch a relay pin."""
        return self._write_style(
            IO_CMD_SX_WRITE_REG, devices.sx_write_reg(reg, value), timeout
        )

    def sx_set_dir(self, mask: int, timeout: float = SET_RELAY_REJECT_WINDOW_S) -> "devices.OkReason":
        """Raw SX1509 direction-register write -- refused if it would retarget a relay pin."""
        return self._write_style(IO_CMD_SX_SET_DIR, devices.sx_set_dir(mask), timeout)

    def sx_set_pullup(self, mask: int, timeout: float = SET_RELAY_REJECT_WINDOW_S) -> "devices.OkReason":
        """Raw SX1509 pull-up register write."""
        return self._write_style(IO_CMD_SX_SET_PULLUP, devices.sx_set_pullup(mask), timeout)

    def sx_set_opendrain(
        self, mask: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Raw SX1509 open-drain register write."""
        return self._write_style(
            IO_CMD_SX_SET_OPENDRAIN, devices.sx_set_opendrain(mask), timeout
        )

    def sx_set_debounce(
        self, enable_mask: int, config: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Raw SX1509 debounce config write."""
        return self._write_style(
            IO_CMD_SX_SET_DEBOUNCE, devices.sx_set_debounce(enable_mask, config), timeout
        )

    def sx_set_int_mask(
        self, mask: int, sense: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Raw SX1509 interrupt mask/sense write."""
        return self._write_style(
            IO_CMD_SX_SET_INT_MASK, devices.sx_set_int_mask(mask, sense), timeout
        )

    def sx_led_driver(
        self, pin: int, enable: bool, intensity: int, timeout: float = SET_RELAY_REJECT_WINDOW_S
    ) -> "devices.OkReason":
        """Enable/disable one pin's LED driver (ClkX/breathe) at a given intensity."""
        return self._write_style(
            IO_CMD_SX_LED_DRIVER, devices.sx_led_driver(pin, enable, intensity), timeout
        )

    def sx_reset(self, hard: bool, timeout: float = SET_RELAY_REJECT_WINDOW_S) -> "devices.OkReason":
        """Reset the SX1509 (software register reset, or pulse ~RESET)."""
        return self._write_style(IO_CMD_SX_RESET, devices.sx_reset(hard), timeout)

    def _write_style(self, subcommand: int, payload: bytes, timeout: float, on_silence: object = None):
        """Send one write subcommand and wait out ``timeout`` for the
        *optional* refusal reply -- shared by :meth:`set_relay`/
        :meth:`set_relay_mask` (``on_silence`` a :class:`RelayResult`) and
        every other IO write above (``on_silence`` defaults to a plain
        ``OkReason(ok=True)``): a reply within the window IS the refusal,
        decoded by ``parse_io_response``; silence means the write happened.

        Raises :class:`IoQueryError` only if the request itself was not
        delivered (no transport ACK).
        """
        if on_silence is None:
            on_silence = devices.OkReason(ok=True)
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
                if pending.event.wait(timeout):
                    # A reply arrived -- io_bridge_task() only ever sends one
                    # for these subcommands on refusal.
                    return pending.value
                # Silence within the window: the write happened.
                return on_silence
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

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
