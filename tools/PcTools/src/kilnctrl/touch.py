"""Client for the firmware's TOUCH task (task 13): the NS2009 touch
controller on the same J2 panel as DISPLAY, and the screen_idle auto-blank
state machine it feeds.

Same shape as :class:`kilnctrl.display.DisplayClient`: GET_STATE is the one
query (request DATA frame ACKed for delivery only, reply arrives as a
separate DATA frame from ``(ESP, UART_TASK_ID_TOUCH)``, matched by
subcommand -- nothing is ever pushed unsolicited on this task). INJECT is
fire-and-forget, like DISPLAY's drawing subcommands: it is the mechanism
that lets this side send a touch "as if from the screen" -- the firmware
treats it exactly like a real NS2009 press for the idle timer and the
wake-on-touch decision.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import OkReason, TouchResponseError, TouchState
from .protocol import (
    TOUCH_CMD_GET_STATE,
    TOUCH_CMD_INJECT,
    TOUCH_CMD_LOG_TAP_TARGETS,
    TOUCH_CMD_SET_TAP_DUMP,
    UART_TASK_ID_TOUCH,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: GET_STATE is answered from screen_idle's in-memory state, no hardware
#: round trip involved -- short timeout is fine.
DEFAULT_REPLY_TIMEOUT_S = 2.0

#: How long inject()/set_tap_dump()/log_tap_targets() wait for an optional
#: driver-error refusal reply before concluding the write went through.
#: touch_bridge_task() only ever replies to these three subcommands when the
#: driver call itself failed (bridge_reply_reject(..., "driver error"));
#: success is silent. Same shape and same reasoning as
#: io_expander.py's SET_RELAY_REJECT_WINDOW_S -- sized with margin over a
#: normal round trip, not over the transport ACK timeout.
DRIVER_ERROR_REJECT_WINDOW_S = 0.5


class TouchQueryError(RuntimeError):
    """Raised when a TOUCH query cannot be completed.

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


class TouchClient:
    """Owns task :data:`UART_TASK_ID_TOUCH` on the PC side of a link."""

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_TOUCH) -> None:
        self.link = link
        self.task_id = task_id

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-touch-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 13. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- writes ------------------------------------------------------------
    def inject(
        self, x: int, y: int, pressed: bool, timeout: float = DRIVER_ERROR_REJECT_WINDOW_S
    ) -> OkReason:
        """Send a synthetic touch, and learn if the driver call refused it.

        See :data:`DRIVER_ERROR_REJECT_WINDOW_S` -- this waits out that
        window for an optional refusal reply; silence means it went through.
        Raises :class:`TouchQueryError` only if the request itself was not
        delivered (no transport ACK).
        """
        return self._write(TOUCH_CMD_INJECT, devices.touch_inject(x, y, pressed), timeout)

    def set_tap_dump(
        self, enable: bool, timeout: float = DRIVER_ERROR_REJECT_WINDOW_S
    ) -> OkReason:
        """Turn the firmware's automatic per-page-switch tap-target dump
        on/off -- see devices.touch_set_tap_dump and :data:`DRIVER_ERROR_REJECT_WINDOW_S`."""
        return self._write(TOUCH_CMD_SET_TAP_DUMP, devices.touch_set_tap_dump(enable), timeout)

    def log_tap_targets(self, timeout: float = DRIVER_ERROR_REJECT_WINDOW_S) -> OkReason:
        """Request an on-demand tap-target dump for the current screen --
        see devices.touch_log_tap_targets and :data:`DRIVER_ERROR_REJECT_WINDOW_S`.
        The dump itself arrives as ESP_LOGI lines over the device log
        (get_device_log / get_device_log_json), not as a reply here."""
        return self._write(TOUCH_CMD_LOG_TAP_TARGETS, devices.touch_log_tap_targets(), timeout)

    def _write(self, subcommand: int, payload: bytes, timeout: float) -> OkReason:
        """Send one write subcommand and wait out ``timeout`` for the
        *optional* driver-error refusal reply -- same send/wait-window shape
        as :class:`kilnctrl.io_expander.IoClient`'s ``set_relay()``."""
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
                    raise TouchQueryError(
                        f"TOUCH request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if pending.event.wait(timeout):
                    # A reply arrived -- touch_bridge_task() only ever sends
                    # one for these subcommands on refusal.
                    return pending.value  # type: ignore[return-value]
                # Silence within the window: the write happened.
                return OkReason(ok=True)
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- queries -----------------------------------------------------------
    def get_state(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> TouchState:
        """Whether the screen is on and how long it's been idle.

        Raises :class:`TouchQueryError` if ``screen_idle_get_state()`` itself
        failed on the firmware side -- decoded from the driver-error refusal
        reply (see ``parse_touch_response``'s GET_STATE branch) rather than
        surfacing as a generic timeout or an ``AttributeError`` from treating
        an :class:`~kilnctrl.devices.OkReason` as a :class:`TouchState`.
        """
        value = self._query(TOUCH_CMD_GET_STATE, devices.touch_get_state(), timeout)
        if isinstance(value, OkReason):
            raise TouchQueryError(f"GET_STATE refused: {value.describe()}")
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
                    raise TouchQueryError(
                        f"TOUCH request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise TouchQueryError(
                        f"TOUCH request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s"
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
                log.exception("error handling TOUCH frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_touch_response(frame.payload)
        except TouchResponseError as exc:
            log.warning("dropping malformed TOUCH response: %s", exc)
            return

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding: a stale reply to a query we already gave up on.
        # Nothing on this task is ever pushed unsolicited.
        log.debug("ignoring unsolicited TOUCH response 0x%02X", subcommand)
