"""Client for the firmware's PROFILES task (task 9): fire profile CRUD and
execution control (mirrors ``profiles_http.c`` / ``dashboard_http.c``'s
``/api/profile_exec*``).

Every subcommand here replies -- LIST/GET/GET_EXEC_STATUS are queries in the
THERMO/IO sense (separate DATA frame with the answer); SAVE/DELETE/START/
STOP/PAUSE/RESUME/ACK_LAST_RUN reply with an explicit ok/fail byte the same
way CONTROL's SET_* does, so this client treats all of them uniformly as
"send, then wait for exactly one reply frame".
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import ProfileDetail, ProfileExecStatus, ProfileSaveResult, ProfileSegment, ProfileSummary, ProfilesResponseError
from .protocol import (
    PROFILES_CMD_ACK_LAST_RUN,
    PROFILES_CMD_DELETE,
    PROFILES_CMD_GET,
    PROFILES_CMD_GET_EXEC_STATUS,
    PROFILES_CMD_LIST,
    PROFILES_CMD_PAUSE,
    PROFILES_CMD_RESUME,
    PROFILES_CMD_SAVE,
    PROFILES_CMD_START,
    PROFILES_CMD_STOP,
    PROFILES_SAVE_ID_NEW,
    UART_TASK_ID_PROFILES,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

DEFAULT_REPLY_TIMEOUT_S = 3.0


class ProfilesQueryError(RuntimeError):
    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class ProfilesClient:
    """Owns task :data:`UART_TASK_ID_PROFILES` on the PC side of a link."""

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_PROFILES) -> None:
        self.link = link
        self.task_id = task_id
        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-profiles-rx", daemon=True
        )
        self._consumer.start()

    def close(self) -> None:
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- requests ------------------------------------------------------------
    def list(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "list[ProfileSummary]":
        return self._query(PROFILES_CMD_LIST, devices.profiles_list(), timeout)  # type: ignore[return-value]

    def get(self, profile_id: int, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "Optional[ProfileDetail]":
        return self._query(PROFILES_CMD_GET, devices.profiles_get(profile_id), timeout)  # type: ignore[return-value]

    def save(
        self, profile_id: int, name: str, zone_mask: int, segments: "list[ProfileSegment]",
        timeout: float = DEFAULT_REPLY_TIMEOUT_S,
    ) -> ProfileSaveResult:
        return self._query(
            PROFILES_CMD_SAVE, devices.profiles_save(profile_id, name, zone_mask, segments), timeout
        )  # type: ignore[return-value]

    def delete(self, profile_id: int, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(PROFILES_CMD_DELETE, devices.profiles_delete(profile_id), timeout)  # type: ignore[return-value]

    def get_exec_status(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> ProfileExecStatus:
        return self._query(
            PROFILES_CMD_GET_EXEC_STATUS, devices.profiles_get_exec_status(), timeout
        )  # type: ignore[return-value]

    def start(self, profile_id: int, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> ProfileSaveResult:
        return self._query(PROFILES_CMD_START, devices.profiles_start(profile_id), timeout)  # type: ignore[return-value]

    def stop(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(PROFILES_CMD_STOP, devices.profiles_stop(), timeout)  # type: ignore[return-value]

    def pause(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(PROFILES_CMD_PAUSE, devices.profiles_pause(), timeout)  # type: ignore[return-value]

    def resume(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(PROFILES_CMD_RESUME, devices.profiles_resume(), timeout)  # type: ignore[return-value]

    def ack_last_run(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        return self._query(PROFILES_CMD_ACK_LAST_RUN, devices.profiles_ack_last_run(), timeout)  # type: ignore[return-value]

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
                    raise ProfilesQueryError(
                        f"PROFILES request 0x{subcommand:02X} not delivered: {result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise ProfilesQueryError(
                        f"PROFILES request 0x{subcommand:02X} was ACKed but no reply "
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
                log.exception("error handling PROFILES frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_profiles_response(frame.payload)
        except ProfilesResponseError as exc:
            log.warning("dropping malformed PROFILES response: %s", exc)
            return
        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return
        log.debug("ignoring unsolicited PROFILES response 0x%02X", subcommand)


__all__ = ["ProfilesClient", "ProfilesQueryError", "PROFILES_SAVE_ID_NEW"]
