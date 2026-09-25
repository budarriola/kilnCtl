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
import time
from typing import Optional

from .protocol import (
    UART_TASK_ID_UI_TEST,
    UI_TEST_CLICK_AMBIGUOUS,
    UI_TEST_CLICK_HIDDEN,
    UI_TEST_CLICK_INJECT_FAILED,
    UI_TEST_CLICK_NOT_FOUND,
    UI_TEST_CLICK_OFFSCREEN,
    UI_TEST_CLICK_OK,
    UI_TEST_CLICK_SWALLOWED,
    UI_TEST_CLICK_VERDICT_UNKNOWN,
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

#: enter_pin()'s first-digit retry poll, 2026-09-25 (LCD-19 bench root
#: cause, 20260925T170357Z_full/summary.json): the keypad overlay was
#: confirmed raised (list_tap_targets showed every digit plus OK/Cancel)
#: immediately before enter_pin() ran, yet its very first click_by_name()
#: call still came back "not_found" -- the overlay's tap-target registry and
#: its actual clickable widgets were not yet in sync on that exact frame,
#: the same class of race bench_test/cases_lcd.py's other click_by_name()
#: callers poll-and-retry around (ui_test_client.py:208-232 module comment;
#: cases_lcd.py's _click_resolving_swallow()/_click_then_page()). Every
#: later digit and OK in that same run succeeded first try, so this is a
#: narrow one-shot startup race, not a systemic keypad flake -- a short poll
#: then a single retry is enough, same bounded-retry discipline as the rest
#: of this module (never a bare re-click ). Kept tiny relative to
#: DEFAULT_REPLY_TIMEOUT_S so a genuinely absent target still reports
#: not_found promptly.
_ENTER_PIN_FIRST_DIGIT_RETRY_POLL_S = 0.1

_CLICK_RESULT_NAMES = {
    UI_TEST_CLICK_OK: "ok",
    UI_TEST_CLICK_NOT_FOUND: "not_found",
    UI_TEST_CLICK_AMBIGUOUS: "ambiguous",
    UI_TEST_CLICK_HIDDEN: "hidden",
    UI_TEST_CLICK_SWALLOWED: "swallowed",
    #: 2026-09-24 follow-up: the bounded verdict wait in kiln_ui_click_by_name()
    #: timed out before it could tell whether the press was swallowed. Neither
    #: a pass nor a genuine_defect -- see click_by_name()'s own doc comment.
    UI_TEST_CLICK_VERDICT_UNKNOWN: "verdict_unknown",
    #: 2026-09-24 follow-up: no press was ever sent (lvgl_port_inject_touch()
    #: itself refused) -- distinct from "verdict_unknown", where a press WAS
    #: sent but its swallow verdict couldn't be confirmed. Never a pass, and
    #: never grounds to poll for a page change.
    UI_TEST_CLICK_INJECT_FAILED: "inject_failed",
    #: 2026-09-25: the (first) match's centre lies off the panel entirely --
    #: LVGL never clamps an injected point to the display, so a press there
    #: used to be silently swallowed by the hit test and come back "ok", a
    #: false pass. Checked ahead of the hidden/visible split.
    UI_TEST_CLICK_OFFSCREEN: "offscreen",
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

        Returns ``{"result":"ok"|"not_found"|"ambiguous"|"hidden"|"swallowed"|"verdict_unknown"|"inject_failed","cx":int,"cy":int}``
        -- unlike touch.py's inject()/set_tap_dump(), a firmware-level refusal
        here (target not found, ambiguous, or hidden) is not an
        exceptional/transport failure, so it comes back as a result code
        rather than raising: only delivery failure or a malformed reply
        raises :class:`UiTestQueryError`/:class:`UiTestResponseError`.
        ``"swallowed"`` means the press was delivered but
        screen_idle_touch_swallow() ate it (a wake or ERROR_HOLD dismissal) --
        the target was found and tapped, but nothing under it ran; a caller
        should retry the click rather than treat it as a defect.
        ``"verdict_unknown"`` (2026-09-24) means the press was injected (the
        target was found and visible) but kiln_ui_click_by_name()'s own
        bounded wait for the swallow verdict timed out before it could be
        read -- a slow LVGL flush can outrun that wait even on a press that
        landed cleanly. This is neither "ok" nor "swallowed": a caller must
        not count it as a pass, and must not attribute it as a genuine
        defect either -- see bench_test/cases_lcd.py's handling for the
        expected shape (judged only by the observable page change that
        follows, never blind re-clicked since the press may have landed,
        its own distinct attribution, never folded into "ok" or
        "swallowed").
        ``"inject_failed"`` (2026-09-24) means lvgl_port_inject_touch() itself
        refused the press (returned 0, its documented "never queued"
        sentinel) -- no press was ever sent, so unlike "verdict_unknown"
        there is nothing that might have landed. A caller must treat this
        exactly like "not_found"/"ambiguous"/"hidden": never a pass, and
        never grounds to poll for a page change this click could not have
        caused.
        """
        payload = self._query(UI_TEST_CMD_CLICK_BY_NAME, _pack_click_request(name), timeout)
        if len(payload) < 6:
            raise UiTestResponseError(f"CLICK_BY_NAME response too short: {len(payload)} bytes")
        result_code, cx, cy = struct.unpack_from("<Bhh", payload, 1)
        result = _CLICK_RESULT_NAMES.get(result_code)
        if result is None:
            raise UiTestResponseError(f"unknown CLICK_BY_NAME result code {result_code}")
        return {"result": result, "cx": cx, "cy": cy}

    def enter_pin(self, pin: str, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> dict:
        """Type ``pin``'s digits into an already-open PIN keypad overlay
        (``ui_lcd_keypad.c``) via :meth:`click_by_name`, then press "OK".

        Each digit and "OK" are individual buttonmatrix keys, each its own
        named tap target (``kiln_ui.c``'s buttonmatrix walk reports the key's
        own text as its name, e.g. "0".."9", "OK") -- so this is just a
        sequence of ordinary click_by_name() calls, not a new wire command.
        This method does not itself poll for the keypad appearing first or
        for the submit's effect afterward (closing on a correct PIN,
        resetting the dot count on a wrong one, or a lockout message) --
        same click-then-read race as any other click_by_name() use here, so
        callers must poll with their own ``_wait_for_*`` helper before
        trusting the result of an entry, exactly as they already do around
        any other click_by_name().

        Returns ``{"digit_results": [...], "ok_result": {...}}``, each entry
        shaped like click_by_name()'s own return value. A caller can inspect
        ``digit_results`` for a "not_found"/"hidden"/"ambiguous" entry (e.g.
        the keypad closed mid-entry) without this method itself raising or
        guessing what that means for the case's verdict.

        The FIRST digit only is retried once, after a short poll, if its
        click comes back "not_found" -- see
        ``_ENTER_PIN_FIRST_DIGIT_RETRY_POLL_S``'s comment for the observed
        race (the keypad overlay reads as raised a moment before its
        widgets are actually clickable). This never re-clicks a digit that
        was already reported clicked ("ok"/"swallowed"/"verdict_unknown"/
        anything but "not_found") -- doing so on a digit that actually
        landed would type it twice and corrupt the PIN -- and only ever
        applies to the first digit, since every other digit and OK in the
        bench evidence that motivated this succeeded on the first try.
        """
        digit_results = []
        for index, ch in enumerate(pin):
            click = self.click_by_name(ch, timeout=timeout)
            if index == 0 and click.get("result") == "not_found":
                time.sleep(_ENTER_PIN_FIRST_DIGIT_RETRY_POLL_S)
                click = self.click_by_name(ch, timeout=timeout)
            digit_results.append(click)
        ok_result = self.click_by_name("OK", timeout=timeout)
        return {"digit_results": digit_results, "ok_result": ok_result}

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


#: uart_bridge_ui_test.c's CLICK_BY_NAME handler copies the name into a local
#: `char name[32]` and null-terminates it, so the wire name must fit in 31
#: bytes -- one byte short of the buffer -- or the firmware would otherwise
#: truncate it silently. Refuse here instead of shipping a truncated name.
_MAX_CLICK_NAME_BYTES = 31


def _pack_click_request(name: str) -> bytes:
    """Encode a CLICK_BY_NAME request.

    uart_task_ids.h's CLICK_BY_NAME layout is ``byte0 = subcommand,
    bytes1..(length-1) = ASCII target name, NOT null-terminated`` -- unlike
    every reply on this task (GET_CURRENT_PAGE/LIST_TAP_TARGETS), the request
    carries no length-prefix byte of its own: the frame's own `length` field
    is the only length. A length-prefixed encoding (as an earlier, now-deleted
    ``_pack_str8`` helper produced) would silently corrupt the name on the
    wire -- the firmware reads the prefix byte as the first character of the
    name and everything shifts by one.
    """
    if not name:
        raise ValueError("target name must not be empty")
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError(f"target name {name!r} is not ASCII: {exc}") from exc
    if len(encoded) > _MAX_CLICK_NAME_BYTES:
        raise ValueError(
            f"target name too long: {len(encoded)} bytes > {_MAX_CLICK_NAME_BYTES} "
            "(uart_bridge_ui_test.c's CLICK_BY_NAME name buffer is 32 bytes "
            "including the NUL terminator)"
        )
    return struct.pack("<B", UI_TEST_CMD_CLICK_BY_NAME) + encoded
