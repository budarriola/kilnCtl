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
from .devices import TouchResponseError, TouchState
from .protocol import (
    TOUCH_CMD_GET_STATE,
    UART_TASK_ID_TOUCH,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: GET_STATE is answered from screen_idle's in-memory state, no hardware
#: round trip involved -- short timeout is fine.
DEFAULT_REPLY_TIMEOUT_S = 2.0


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
    def inject(self, x: int, y: int, pressed: bool) -> SendResult:
        """Send a synthetic touch, fire-and-forget -- see devices.touch_inject."""
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id,
            payload=devices.touch_inject(x, y, pressed),
        )

    def set_tap_dump(self, enable: bool) -> SendResult:
        """Turn the firmware's automatic per-page-switch tap-target dump
        on/off, fire-and-forget -- see devices.touch_set_tap_dump."""
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id,
            payload=devices.touch_set_tap_dump(enable),
        )

    def log_tap_targets(self) -> SendResult:
        """Request an on-demand tap-target dump for the current screen,
        fire-and-forget -- see devices.touch_log_tap_targets. The dump itself
        arrives as ESP_LOGI lines over the device log (get_device_log /
        get_device_log_json), not as a reply here."""
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id,
            payload=devices.touch_log_tap_targets(),
        )

    # -- queries -----------------------------------------------------------
    def get_state(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> TouchState:
        """Whether the screen is on and how long it's been idle."""
        value = self._query(TOUCH_CMD_GET_STATE, devices.touch_get_state(), timeout)
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
