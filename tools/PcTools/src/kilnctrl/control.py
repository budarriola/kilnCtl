"""Client for the firmware's CONTROL task (task 8): zone PID/model config.

GET_ZONES is a query, with the same shape as THERMO/IO's: the request DATA
frame is ACKed for delivery only, and the answer arrives as a separate DATA
frame from ``(ESP, UART_TASK_ID_CONTROL)``. SET_ZONE_PID/SET_ZONE_MODEL are
*not* silent like THERMO/IO's SET_*: they reply with an explicit ok/fail byte
(same shape as a query reply), because a rejected zone index or out-of-range
gain is exactly what a GUI needs to know immediately -- so this client treats
every subcommand here as a query for the purposes of "wait for a reply".

Manual relay control is NOT part of this task -- use :class:`IoClient`
(io_expander.py)'s ``set_relay``/``set_relay_mask``.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import ControlResponseError, ZoneConfig
from .protocol import (
    CONTROL_CMD_GET_ZONES,
    CONTROL_CMD_SET_ZONE_MODEL,
    CONTROL_CMD_SET_ZONE_PID,
    UART_TASK_ID_CONTROL,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: GET_ZONES touches NVS-backed config on the ESP; SET_* validates and
#: possibly writes. Neither is I/O-latency bound, but give it room.
DEFAULT_REPLY_TIMEOUT_S = 3.0


class ControlQueryError(RuntimeError):
    """Raised when a CONTROL request cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer;
    None when the request was delivered but no valid reply came back in time.
    """

    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class ControlClient:
    """Owns task :data:`UART_TASK_ID_CONTROL` on the PC side of a link."""

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_CONTROL) -> None:
        self.link = link
        self.task_id = task_id
        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-control-rx", daemon=True
        )
        self._consumer.start()

    def close(self) -> None:
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- requests ------------------------------------------------------------
    def get_zones(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "tuple[int, int, list[ZoneConfig]]":
        return self._query(CONTROL_CMD_GET_ZONES, devices.control_get_zones(), timeout)  # type: ignore[return-value]

    def set_zone_pid(
        self, zone: int, kp: float, ki: float, kd: float, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> devices.OkReason:
        """Returns an :class:`devices.OkReason` -- truthy/falsy like a plain
        bool, but carrying the refusal text (e.g. an out-of-range zone or
        gain) when the firmware rejects the write, instead of discarding it."""
        return self._query(
            CONTROL_CMD_SET_ZONE_PID,
            devices.control_set_zone_pid(zone, kp, ki, kd),
            timeout,
        )  # type: ignore[return-value]

    def set_zone_model(
        self, zone: int, k_dc: float, tau_s: float, dead_time_s: float,
        timeout: float = DEFAULT_REPLY_TIMEOUT_S,
    ) -> devices.OkReason:
        """See :meth:`set_zone_pid` -- same ``OkReason`` return shape."""
        return self._query(
            CONTROL_CMD_SET_ZONE_MODEL,
            devices.control_set_zone_model(zone, k_dc, tau_s, dead_time_s),
            timeout,
        )  # type: ignore[return-value]

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
                    raise ControlQueryError(
                        f"CONTROL request 0x{subcommand:02X} not delivered: {result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise ControlQueryError(
                        f"CONTROL request 0x{subcommand:02X} was ACKed but no reply "
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
                log.exception("error handling CONTROL frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_control_response(frame.payload)
        except ControlResponseError as exc:
            log.warning("dropping malformed CONTROL response: %s", exc)
            return
        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return
        log.debug("ignoring unsolicited CONTROL response 0x%02X", subcommand)
