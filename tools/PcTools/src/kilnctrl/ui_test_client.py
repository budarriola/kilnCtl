"""Client for the firmware's UI_TEST task (task 14): a PC-driven probe into
the LCD's LVGL UI, for the LCD half of the UI regression-test framework.

Same shape as :class:`kilnctrl.touch.TouchClient`: every subcommand here is a
query (request DATA frame ACKed for delivery only, reply arrives as a
separate DATA frame from ``(ESP, UART_TASK_ID_UI_TEST)``, matched by
subcommand) -- nothing is ever pushed unsolicited on this task.
"""

from __future__ import annotations

import logging
import queue
import struct
import threading
from typing import Optional

from .protocol import (
    UART_TASK_ID_UI_TEST,
    UI_TEST_CLICK_AMBIGUOUS,
    UI_TEST_CLICK_HIDDEN,
    UI_TEST_CLICK_NOT_FOUND,
    UI_TEST_CLICK_OK,
    UI_TEST_CMD_CLICK_BY_NAME,
    UI_TEST_CMD_GET_CURRENT_PAGE,
    UI_TEST_CMD_LIST_TAP_TARGETS,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: None of these read the physical panel, only in-memory LVGL state -- short
#: timeout is fine, same reasoning as touch.py's DEFAULT_REPLY_TIMEOUT_S.
DEFAULT_REPLY_TIMEOUT_S = 2.0

_CLICK_RESULT_NAMES = {
    UI_TEST_CLICK_OK: "ok",
    UI_TEST_CLICK_NOT_FOUND: "not_found",
    UI_TEST_CLICK_AMBIGUOUS: "ambiguous",
    UI_TEST_CLICK_HIDDEN: "hidden",
}


class UiTestQueryError(RuntimeError):
    """Raised when a UI_TEST query cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer (the
    request never got an ACK), and None when the request was delivered but no
    valid reply came back in time.
    """

    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class UiTestResponseError(ValueError):
    """Raised when a UI_TEST response payload does not match its wire layout."""


def _pack_str8(text: str) -> bytes:
    encoded = text.encode("ascii", errors="replace")
    if len(encoded) > 255:
        raise ValueError(f"target name too long: {len(encoded)} bytes > 255")
    return struct.pack("<B", len(encoded)) + encoded


def _unpack_str8(payload: bytes, offset: int) -> "tuple[str, int]":
    if offset >= len(payload):
        raise UiTestResponseError(f"UI_TEST response truncated at string length, offset {offset}")
    length = payload[offset]
    start = offset + 1
    end = start + length
    if end > len(payload):
        raise UiTestResponseError(f"UI_TEST response truncated: string of {length} bytes at {start}")
    return payload[start:end].decode("ascii", errors="replace"), end


class _Pending:
    """A single outstanding query: what we asked for, and where to put it."""

    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None
        self.error: "Optional[UiTestResponseError]" = None


class UiTestClient:
    """Owns task :data:`UART_TASK_ID_UI_TEST` on the PC side of a link."""

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_UI_TEST) -> None:
        self.link = link
        self.task_id = task_id

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-ui-test-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 14. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- queries -------------------------------------------------------------
    def get_current_page(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> str:
        """The name of whatever page is currently loaded on the LCD."""
        payload = self._query(
            UI_TEST_CMD_GET_CURRENT_PAGE, struct.pack("<B", UI_TEST_CMD_GET_CURRENT_PAGE), timeout
        )
        name, _ = _unpack_str8(payload, 1)
        return name

    def list_tap_targets(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> dict:
        """Every currently-hittable tap target on the current page.

        Returns ``{"targets": [{"name":str,"cx":int,"cy":int,"hidden":bool}, ...],
        "truncated": bool}`` -- ``truncated`` mirrors the firmware's own
        flag byte (uart_task_ids.h's UI_TEST_CMD_LIST_TAP_TARGETS layout):
        True means more targets existed than BRIDGE_REPLY_MAX could carry
        (same log_tap_targets() tree the device log dump walks), so a caller
        relying on completeness should fall back to touch.py's
        log_tap_targets() + the device log instead.
        """
        payload = self._query(
            UI_TEST_CMD_LIST_TAP_TARGETS, struct.pack("<B", UI_TEST_CMD_LIST_TAP_TARGETS), timeout
        )
        if len(payload) < 3:
            raise UiTestResponseError("LIST_TAP_TARGETS response too short for count/truncated bytes")
        count = payload[1]
        truncated = bool(payload[2])
        offset = 3
        targets = []
        for _ in range(count):
            name, offset = _unpack_str8(payload, offset)
            if offset + 5 > len(payload):
                raise UiTestResponseError("LIST_TAP_TARGETS response truncated in a target entry")
            cx, cy, hidden = struct.unpack_from("<hhB", payload, offset)
            offset += 5
            targets.append({"name": name, "cx": cx, "cy": cy, "hidden": bool(hidden)})
        return {"targets": targets, "truncated": truncated}

    def click_by_name(self, name: str, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> dict:
        """Inject a tap at the named target's centre.

        Returns ``{"result":"ok"|"not_found"|"ambiguous"|"hidden","cx":int,"cy":int}``
        -- unlike touch.py's inject()/set_tap_dump(), a firmware-level refusal
        here (target not found, ambiguous, or hidden) is not an
        exceptional/transport failure, so it comes back as a result code
        rather than raising: only delivery failure or a malformed reply
        raises :class:`UiTestQueryError`/:class:`UiTestResponseError`.
        """
        payload = self._query(UI_TEST_CMD_CLICK_BY_NAME, _pack_click_request(name), timeout)
        if len(payload) < 6:
            raise UiTestResponseError(f"CLICK_BY_NAME response too short: {len(payload)} bytes")
        result_code, cx, cy = struct.unpack_from("<Bhh", payload, 1)
        result = _CLICK_RESULT_NAMES.get(result_code)
        if result is None:
            raise UiTestResponseError(f"unknown CLICK_BY_NAME result code {result_code}")
        return {"result": result, "cx": cx, "cy": cy}

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> bytes:
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
                    raise UiTestQueryError(
                        f"UI_TEST request 0x{subcommand:02X} not delivered: {result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise UiTestQueryError(
                        f"UI_TEST request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s"
                    )
                if pending.error is not None:
                    raise pending.error
                return pending.value  # type: ignore[return-value]
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- receive -------------------------------------------------------------
    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue  # poll interval so close() is noticed promptly
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling UI_TEST frame")

    def _handle_reply(self, frame: Frame) -> None:
        if not frame.payload:
            log.warning("dropping empty UI_TEST response")
            return
        subcommand = frame.payload[0]

        with self._pending_lock:
            pending = self._pending
        if pending is None or pending.subcommand != subcommand:
            # Nothing outstanding: a stale reply to a query we already gave
            # up on. Nothing on this task is ever pushed unsolicited.
            log.debug("ignoring unsolicited UI_TEST response 0x%02X", subcommand)
            return
        pending.value = frame.payload
        pending.event.set()


def _pack_click_request(name: str) -> bytes:
    return struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + _pack_str8(name)
