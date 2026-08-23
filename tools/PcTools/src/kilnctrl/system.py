"""Client for the firmware's SYSTEM task (task 6): bench/debug controls.

Only :data:`~kilnctrl.protocol.SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED` is a
*query* -- like INFO, the request DATA frame is ACKed as usual (delivery
confirmation only), and the answer arrives afterwards as a **separate DATA
frame** from (ESP, UART_TASK_ID_SYSTEM) addressed back to whoever asked,
exactly as ``system_bridge_task()`` in uart_bridge.c does it. The PC
therefore has to be registered as task 6 itself to receive anything.

Unlike INFO's replies, SYSTEM's are self-describing (byte0 echoes the
subcommand -- see ``devices.parse_system_response``), so there is no
boot-push ambiguity to resolve here: any reply that arrives with nothing
outstanding is simply dropped as stale/unsolicited.

RESTART_UART, FACTORY_RESET, and SET_WATCHDOG_PANIC_DISABLED have no reply
frame at all -- those go straight through ``UartLink.send()`` (see
devices.system_restart_uart / system_factory_reset, and
gui.py/mcp_server.py for how they call it) and do not need this client.
This module exists only for the one query.

Register early (at :class:`~kilnctrl.serial_link.UartLink` construction, not
lazily when some window opens), same discipline as InfoClient, so this task
is always ready to receive its reply.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import SystemResponseError
from .protocol import (
    SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED,
    UART_TASK_ID_SYSTEM,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: The firmware answers from a single persisted flag, so this only has to
#: cover one more UART round trip plus its own send retries.
DEFAULT_REPLY_TIMEOUT_S = 2.0


class SystemQueryError(RuntimeError):
    """Raised when a SYSTEM query cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer
    (the request never got an ACK), and None when the request was delivered
    but no valid reply came back in time.
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


class SystemClient:
    """Owns task :data:`UART_TASK_ID_SYSTEM` on the PC side of a link.

    Registers the task, drains its inbox on a background thread, and
    satisfies outstanding GET_WATCHDOG_PANIC_DISABLED queries.
    SET_WATCHDOG_PANIC_DISABLED is a plain fire-and-forget send (no reply
    frame), included here so callers have one place to reach for both
    halves of the flag.

    Thread-safety: :meth:`get_watchdog_panic_disabled` blocks and must not
    be called from a GUI thread. :meth:`set_watchdog_panic_disabled` just
    sends and returns the SendResult, so it is safe from a GUI thread the
    same way any other fire-and-forget send is.
    """

    def __init__(
        self,
        link: UartLink,
        task_id: int = UART_TASK_ID_SYSTEM,
    ) -> None:
        self.link = link
        self.task_id = task_id

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        #: Serializes queries so at most one reply is ever outstanding.
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-system-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 6. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- queries -------------------------------------------------------
    def get_watchdog_panic_disabled(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        """Ask the device whether the task-watchdog panic is disabled.

        Raises :class:`SystemQueryError` on an undelivered request or a
        missing or malformed reply.
        """
        value = self._query(
            SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED,
            devices.system_get_watchdog_panic_disabled(),
            timeout,
        )
        return value  # type: ignore[return-value]

    # -- fire-and-forget -----------------------------------------------
    def set_watchdog_panic_disabled(self, disabled: bool) -> SendResult:
        """Set the task-watchdog panic flag. No reply frame -- the ACK
        (reflected in the returned SendResult) is the only confirmation.
        Poll :meth:`get_watchdog_panic_disabled` afterward to read back the
        applied value.
        """
        return self.link.send(
            dst_task=self.task_id,
            src_task=self.task_id,
            payload=devices.system_set_watchdog_panic_disabled(disabled),
            dst_device=Device.ESP,
        )

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
                    raise SystemQueryError(
                        f"SYSTEM request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise SystemQueryError(
                        f"SYSTEM request 0x{subcommand:02X} was ACKed but no reply "
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
                log.exception("error handling SYSTEM frame")

    def _handle_reply(self, frame: Frame) -> None:
        with self._pending_lock:
            pending = self._pending

        try:
            subcommand, value = devices.parse_system_response(frame.payload)
        except SystemResponseError as exc:
            log.warning("dropping malformed SYSTEM response: %s", exc)
            return

        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding for this subcommand -- stale/duplicate reply
        # we already gave up on (SYSTEM has no boot-push convention like
        # INFO's, so there is no other legitimate source of an unsolicited
        # reply here).
        log.debug("ignoring unsolicited SYSTEM response 0x%02X", subcommand)
