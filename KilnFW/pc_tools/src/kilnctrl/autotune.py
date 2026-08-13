"""Client for the firmware's AUTOTUNE task (task 10): PID autotune
(mirrors ``dashboard_http.c``'s ``/api/autotune`` GET/start/abort/accept).

Not mirrored over UART: ``/api/autotune/matrix`` and the trace/history CSV
dumps -- bulk/table data that doesn't fit a 253-byte frame. Use HTTP for
those (see docs/UART_PROTOCOL.md).
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import AutotuneResponseError, AutotuneStatus
from .protocol import (
    AUTOTUNE_CMD_ABORT,
    AUTOTUNE_CMD_ACCEPT,
    AUTOTUNE_CMD_GET_STATUS,
    AUTOTUNE_CMD_START,
    UART_TASK_ID_AUTOTUNE,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

DEFAULT_REPLY_TIMEOUT_S = 3.0


class AutotuneQueryError(RuntimeError):
    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class AutotuneClient:
    """Owns task :data:`UART_TASK_ID_AUTOTUNE` on the PC side of a link."""

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_AUTOTUNE) -> None:
        self.link = link
        self.task_id = task_id
        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-autotune-rx", daemon=True
        )
        self._consumer.start()

    def close(self) -> None:
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- requests ------------------------------------------------------------
    def get_status(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> AutotuneStatus:
        return self._query(AUTOTUNE_CMD_GET_STATUS, devices.autotune_get_status(), timeout)  # type: ignore[return-value]

    def start(
        self, zone: int, method: int, step_duty_or_setpoint_c: float,
        relay_d: float = -1.0, relay_h_c: float = -1.0, rule: int = 0,
        timeout: float = DEFAULT_REPLY_TIMEOUT_S,
    ) -> "tuple[bool, str]":
        return self._query(
            AUTOTUNE_CMD_START,
            devices.autotune_start(zone, method, step_duty_or_setpoint_c, relay_d, relay_h_c, rule),
            timeout,
        )  # type: ignore[return-value]

    def abort(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(AUTOTUNE_CMD_ABORT, devices.autotune_abort(), timeout)  # type: ignore[return-value]

    def accept(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(AUTOTUNE_CMD_ACCEPT, devices.autotune_accept(), timeout)  # type: ignore[return-value]

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> object:
        with self._query_lock:
            pending = _Pending(subcommand)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self.link.send(
                    dst_task=self.task_id, src_task=self.task_id, payload=payload,
                    dst_device=Device.ESP,
                )
                if not result.ok:
                    raise AutotuneQueryError(
                        f"AUTOTUNE request 0x{subcommand:02X} not delivered: {result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise AutotuneQueryError(
                        f"AUTOTUNE request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s"
                    )
                return pending.value
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling AUTOTUNE frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_autotune_response(frame.payload)
        except AutotuneResponseError as exc:
            log.warning("dropping malformed AUTOTUNE response: %s", exc)
            return
        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return
        log.debug("ignoring unsolicited AUTOTUNE response 0x%02X", subcommand)


__all__ = ["AutotuneClient", "AutotuneQueryError"]
