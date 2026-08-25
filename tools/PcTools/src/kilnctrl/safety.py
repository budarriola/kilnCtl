"""Client for the firmware's SAFETY task (task 7): the isolated RP2040 link.

The RP2040 safety processor (A1) sits in its own ground domain. The only
things crossing the barrier are two opto-isolated UART lines and one
opto-isolated fault line, and the Pico -- not the ESP -- owns the safety
thermocouple board on J7, the three current-sense channels, the E-stop input
and the safety relay K4. This task is the PC's window onto that link.

Two queries (``GET_STATUS``, ``GET_LINK_STATS``) with the same shape as the
ones on task INFO: the request is ACKed for delivery only and the answer
arrives as a separate DATA frame from ``(ESP, UART_TASK_ID_SAFETY)``, echoing
its subcommand in byte0. Nothing is pushed unsolicited here, so this client is
the same simple shape as :class:`~kilnctrl.display.DisplayClient`.

Two things worth stating plainly, because both are easy to get backwards:

* **GET_STATUS never blocks on the far side.** The ESP polls the Pico on its
  own schedule and caches the last good answer; the reply is that cache plus
  its age. A dead link is therefore a *successful* query reporting
  ``link_up = 0`` and ``age = 65535``, not a timeout. :meth:`get_status`
  raising means the ESP didn't answer -- a different fault entirely.
* **The fault line is an output.** ``SET_FAULT_OUT`` drives ESP GPIO6, which
  lights U1's LED and pulls the Pico's ``mainFault`` input low. It is this
  firmware telling the safety processor that the main controller has faulted,
  not a signal coming back. The firmware asserts it by itself on PC-link loss,
  a thermocouple fault or a watchdog; :meth:`set_fault_out` is a manual
  override of that.

**The Pico firmware that answers this protocol does not exist in this
repository yet.** Until it does, ``link_up = 0`` with ``age = 65535`` is the
expected steady state, not something to debug -- every UI built on this client
should say so rather than painting it as an error.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import (
    OkReason,
    SafetyCtCal,
    SafetyDiag,
    SafetyFwVersion,
    SafetyLinkStats,
    SafetyResponseError,
    SafetyStatus,
    SafetyTripEvent,
)
from .protocol import (
    SAFETY_CMD_CT_CAL,
    SAFETY_CMD_GET_CT_CAL,
    SAFETY_CMD_GET_DIAG,
    SAFETY_CMD_GET_FW_VERSION,
    SAFETY_CMD_GET_LINK_STATS,
    SAFETY_CMD_GET_STATUS,
    SAFETY_CMD_GET_TRIP_EVENT,
    SAFETY_CMD_REQUEST_ENABLE,
    SAFETY_CMD_SET_CONFIG,
    SAFETY_CMD_SET_CT_CAL,
    SAFETY_CMD_SET_FAULT_OUT,
    SAFETY_CMD_SET_POLL_PERIOD,
    UART_TASK_ID_SAFETY,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: Both queries are answered from ESP-side state, so this only has to cover
#: one more UART round trip -- not the isolated link's own poll period.
DEFAULT_REPLY_TIMEOUT_S = 2.0

#: How long the mutating-command helpers below wait for an *optional*
#: refusal reply before concluding the command went through. Same shape and
#: reasoning as io_expander.py's SET_RELAY_REJECT_WINDOW_S: safety_bridge_task()
#: replies nothing on success for any of these (they're either a local state
#: change or a fire-and-forget broadcast to the Pico) and replies
#: {subcmd, ok=0, reason} only for a "truncated"/"out of range" argument
#: caught before anything was sent -- decided synchronously, no isolated-link
#: round trip on the refusal path itself, so a short window is enough.
MUTATING_REJECT_WINDOW_S = 0.5

#: GET_CT_CAL's reply timeout. UNLIKE every other query on this task, the
#: ESP does not answer from a cache -- uart_bridge.c's SAFETY_CMD_GET_CT_CAL
#: case calls safety_link_get_ct_cal(), a live, blocking round trip across
#: the isolated link to the Pico (SAFETY_LINK_REPLY_TIMEOUT_MS in
#: firmware/KilnFW/App/drivers/safety_link.h -- that constant is DERIVED from
#: the configured baud rather than fixed, so it shrank from ~1.2 s at the old
#: 9600 to roughly 146 ms at 230400; see KILNCTL_SAFETY_BAUD_RATE for the
#: current rate). This
#: budget must cover that ESP-side wait plus one more PC<->ESP round trip on
#: top of it, so it is DEFAULT_REPLY_TIMEOUT_S plus a multiple of
#: safety_link.h's own worst case rather than the same flat 2.0 s every
#: cache-only query uses. Deliberately NOT retightened alongside the baud
#: increase: this is a host-side patience budget, and the cost of it being
#: generous is a slower failure report, while the cost of it being tight is a
#: spurious timeout on a query that did nothing wrong.
CT_CAL_REPLY_TIMEOUT_S = 5.0

#: Maps a request id to the DIFFERENT id its successful reply carries, for
#: the commands firmware protocol version 7 split off their reply's shared
#: id (see protocol.py's SAFETY_CMD_GET_CT_CAL doc comment). Only GET_CT_CAL
#: is wired up as a live query on this task today; the mapping stays a dict
#: rather than a single special case so a future GET_PARAM/GET_CONFIG_PAGE
#: query added here does not have to rediscover this same fix.
_REPLY_ID_FOR_REQUEST: dict[int, int] = {
    SAFETY_CMD_GET_CT_CAL: SAFETY_CMD_CT_CAL,
}


def _reply_matches_request(request_subcommand: int, reply_subcommand: int) -> bool:
    """True if `reply_subcommand` is the successful-reply id for a request
    sent under `request_subcommand` -- the two now differ for GET_CT_CAL
    (0x22 request vs 0x1A reply) since a shared id would block a
    length-different driver-error refusal (bridge_reply_reject() echoes the
    REQUEST's id, which already equals `request_subcommand` and is matched
    by the plain `==` check at the call site; this only covers the success
    case)."""
    return _REPLY_ID_FOR_REQUEST.get(request_subcommand) == reply_subcommand


class SafetyQueryError(RuntimeError):
    """Raised when a SAFETY query cannot be completed.

    Note what this does *not* mean: a down isolated link is a normal,
    successful reply (see the module docstring). This is only raised when the
    **ESP** failed to answer.

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


class SafetyClient:
    """Owns task :data:`UART_TASK_ID_SAFETY` on the PC side of a link.

    Thread-safety: every method here blocks on a UART round trip and must not
    be called from a GUI thread.
    """

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_SAFETY) -> None:
        self.link = link
        self.task_id = task_id

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        #: Last status seen, so a page can render immediately on open.
        self.last_status: Optional[SafetyStatus] = None
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        # Replies that arrived with no request outstanding. See
        # _send_reject_window()'s docstring: non-zero means a mutating result
        # from this client may be reporting the previous request's outcome.
        self.late_replies = 0
        #: Serializes queries so at most one reply is ever outstanding.
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-safety-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 7. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- writes ------------------------------------------------------------
    def send(self, payload: bytes) -> SendResult:
        """Send one non-query subcommand payload (built by ``devices.py``).

        Fire-and-forget: the returned :class:`SendResult` proves delivery to
        the task's inbox only. Prefer :meth:`request_enable`/
        :meth:`set_poll_period`/:meth:`set_fault_out`/:meth:`set_ct_cal`/
        :meth:`set_config` instead -- they wait out a short window for the
        optional refusal reply safety_bridge_task() now sends on a truncated
        frame or an out-of-range argument (ROADMAP.md "KilnFW PC-link command
        acknowledgement"). Note that reply only ever covers that ESP-side
        argument check -- a refusal decided *on the Pico* (relay ARMED, an
        unrecognised tc_type, ...) still isn't returned here; read that back
        from the next status/diag/ct_cal poll, same as before.
        """
        return self.link.send(
            dst_task=self.task_id, src_task=self.task_id, payload=payload
        )

    def request_enable(
        self, enable: bool, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x02 REQUEST_ENABLE, and learn *why* if the ESP refuses outright.

        Advisory to the Pico either way -- its own interlocks always win, and
        that outcome is never reflected in this reply (see :meth:`send`'s
        docstring).
        """
        return self._send_reject_window(
            SAFETY_CMD_REQUEST_ENABLE, devices.safety_request_enable(enable), timeout
        )

    def set_poll_period(
        self, period_ms: int, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x05 SET_POLL_PERIOD, and learn *why* if the ESP refuses outright."""
        return self._send_reject_window(
            SAFETY_CMD_SET_POLL_PERIOD, devices.safety_set_poll_period(period_ms), timeout
        )

    def set_fault_out(
        self, assert_fault: bool, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x06 SET_FAULT_OUT, and learn *why* if the ESP refuses outright."""
        return self._send_reject_window(
            SAFETY_CMD_SET_FAULT_OUT, devices.safety_set_fault_out(assert_fault), timeout
        )

    def set_config(
        self, tc_type: int, timeout: float = MUTATING_REJECT_WINDOW_S
    ) -> OkReason:
        """0x16 SET_CONFIG, and learn *why* if the ESP refuses outright.

        Still fire-and-forget as far as the Pico's own say-so is concerned
        (see :func:`devices.safety_set_config`'s docstring) -- this only
        additionally catches a truncated frame or an out-of-range tc_type on
        the ESP side, before it would have reached the Pico at all.
        """
        return self._send_reject_window(
            SAFETY_CMD_SET_CONFIG, devices.safety_set_config(tc_type), timeout
        )

    def set_ct_cal(
        self,
        channel: int,
        calibrated: bool,
        gain: float,
        offset: float,
        timeout: float = MUTATING_REJECT_WINDOW_S,
    ) -> OkReason:
        """0x19 SET_CT_CAL, and learn *why* if the ESP refuses outright.

        Still fire-and-forget as far as the Pico's own say-so is concerned
        (relay ARMED, or an out-of-range channel it independently rejects --
        see :func:`devices.safety_set_ct_cal`'s docstring); this only
        additionally catches a truncated frame or an out-of-range channel on
        the ESP side. Read the outcome from the next :meth:`get_ct_cal` call
        either way.
        """
        return self._send_reject_window(
            SAFETY_CMD_SET_CT_CAL,
            devices.safety_set_ct_cal(channel, calibrated, gain, offset),
            timeout,
        )

    def _send_reject_window(self, subcommand: int, payload: bytes, timeout: float) -> OkReason:
        """Send a mutating subcommand and wait out ``timeout`` for the
        *optional* refusal reply, same shape as io_expander.py's
        ``_set_relay_style``: a reply within the window IS the refusal;
        silence means the command went through (at least as far as the ESP
        is concerned).

        KNOWN DEFECT -- the first result after a state change can be the
        PREVIOUS call's outcome. Do not trust a single call here to describe
        the state it was issued against; repeat it, or read the state back with
        the matching query method.

        Measured on the bench 2026-08-25. With the safety processor halted and
        its cached status 26 seconds stale -- so every request_enable(1) should
        have been refused -- four identical calls in a row returned:

            ok, refused, refused, refused

        The three refusals are correct. The leading "ok" is this defect: it is
        the reply to a call issued BEFORE the link went down, arriving after
        its own window had expired and being adopted by the next call.

        WHY IT HAPPENS. ``_handle_reply`` below matches an incoming reply to the
        outstanding request by SUBCOMMAND ALONE (plus the request/reply id
        aliasing in ``_reply_matches_request``). There is no request identity in
        the match, so a refusal that misses its own window is handed to the next
        call with the same subcommand, and once that starts it is
        self-sustaining: every call reports its predecessor's answer. The
        "nothing outstanding, so this is stale" guard further down only fires
        when the late reply lands in the GAP between calls; a reply that lands
        after the next call has already armed is indistinguishable, by
        subcommand, from that call's own answer.

        WHY IT IS NOT FIXED HERE. There is no correlation key to fix it with.
        The ESP's ``bridge_reply()`` (KilnFW/App/drivers/uart_bridge.c) sends a
        fresh frame via ``uart_protocol_send()`` rather than echoing the
        request's ``msg_index``, so the reply's index is the ESP's own outgoing
        sequence and says nothing about which request it answers. Filtering on
        an index high-water mark does not help either, because the offending
        reply is GENERATED after its own request was sent and merely ARRIVES
        late, so it is always "newer" than any mark taken at arming time.

        THE REAL FIX is protocol-level: have ``bridge_reply()`` echo the
        requesting frame's ``msg_index`` and match on it here. That touches the
        transport shared by every bridge task, not just this one, which is why
        it is written down rather than done in passing. This is the same
        reply-pairing bug class already recorded against benchproto's
        msg_index/dedup handling.

        Ruled out first, because the alternative was much worse: the enable
        polarity is NOT reversed. ``devices._check_bool_byte`` maps True to 1
        and False to 0, and ``uart_bridge.c`` reads ``payload[1] != 0``, so a
        disable request cannot enable. The inversion in the observation above is
        pairing, not polarity.

        ``late_replies`` counts replies that arrived with nothing outstanding,
        so this stops being invisible -- see ``_handle_reply``.
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
                    raise SafetyQueryError(
                        f"SAFETY request 0x{subcommand:02X} not delivered: "
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
    def get_status(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> SafetyStatus:
        """Read the cached safety status: flags, safety TC, three currents, age."""
        value = self._query(
            SAFETY_CMD_GET_STATUS, devices.safety_get_status(), timeout
        )
        return value  # type: ignore[return-value]

    def get_link_stats(
        self, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> SafetyLinkStats:
        """Read the ESP's own counters for the isolated UART.

        These move even while the far side is silent -- frames sent and
        timeouts both climbing with nothing received is exactly the signature
        of the missing Pico firmware.
        """
        value = self._query(
            SAFETY_CMD_GET_LINK_STATS, devices.safety_get_link_stats(), timeout
        )
        return value  # type: ignore[return-value]

    def get_diag(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> SafetyDiag:
        """Read the cached DIAG (Frame B) telemetry -- guard state, warn/trip
        masks, PUSH_CONTEXT liveness -- the same data dashboard_http.c and
        ui_page_safety.c already show, without needing Wi-Fi
        (CommonFW/docs/LINK_PROTOCOL.md sec 7's "mirror all of it on the
        PC-link SAFETY task"). ``ever_received`` is false (every other field
        meaningless) until the Pico has actually pushed one -- the expected
        state with no Pico firmware attached.
        """
        value = self._query(SAFETY_CMD_GET_DIAG, devices.safety_get_diag(), timeout)
        return value  # type: ignore[return-value]

    def get_trip_event(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> SafetyTripEvent:
        """Read the cached TRIP_EVENT (Frame D) telemetry -- the last trip the
        Pico latched, preserved until a newer one replaces it. Same
        cache-only mirror as :meth:`get_diag`. Check
        :attr:`SafetyTripEvent.ineffective` before treating a result like an
        ordinary trip -- SAFETY_TRIP_INEFFECTIVE (S9) means the contactor
        didn't actually open and calls for a different response entirely.
        """
        value = self._query(
            SAFETY_CMD_GET_TRIP_EVENT, devices.safety_get_trip_event(), timeout
        )
        return value  # type: ignore[return-value]

    def get_fw_version(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> SafetyFwVersion:
        """Read the cached FW_VERSION (Frame C) telemetry -- the Pico's own
        build identity and active config CRC, the other half of DIAG/
        TRIP_EVENT's "mirror it on the PC-link SAFETY task" ask
        (CommonFW/docs/LINK_PROTOCOL.md sec 7). Same cache-only shape as
        :meth:`get_diag`/:meth:`get_trip_event`: this never talks to the Pico
        live. ``commit``/``built`` are empty strings, not placeholders, when
        the Pico hasn't reported a known build identity yet.
        """
        value = self._query(
            SAFETY_CMD_GET_FW_VERSION, devices.safety_get_fw_version(), timeout
        )
        return value  # type: ignore[return-value]

    def get_ct_cal(self, timeout: float = CT_CAL_REPLY_TIMEOUT_S) -> SafetyCtCal:
        """Read the three CT channels' stored calibration -- LIVE, not cached.

        UNLIKE :meth:`get_status`/:meth:`get_diag`/:meth:`get_trip_event`/
        :meth:`get_fw_version`, this is not answered from ESP-side state:
        sending GET_CT_CAL makes the ESP do its own blocking round trip to
        the Pico first (safety_link_get_ct_cal()), so a healthy call to this
        method takes noticeably longer than the other queries here, and a
        dead isolated link surfaces as THIS raising :class:`SafetyQueryError`
        (a timeout) rather than as a successful reply with stale data -- the
        opposite of :meth:`get_status`'s cache-based behaviour. Each channel's
        ``calibrated`` flag must be checked before trusting its gain/offset;
        an uncalibrated channel's numbers are meaningless (see
        :class:`~kilnctrl.devices.SafetyCtCal`).

        Raises :class:`SafetyQueryError` both on a timeout (no reply at all)
        and now also on an explicit driver-error refusal -- uart_bridge.c's
        safety_bridge_task() replies {0x22, ok=0, "driver error"} when
        safety_link_get_ct_cal()'s round trip to the Pico itself fails
        (as opposed to timing out before any reply arrives). Firmware
        protocol version 7 is what makes this refusal distinguishable from
        a malformed success reply at all -- see protocol.py's
        SAFETY_CMD_GET_CT_CAL doc comment.
        """
        value = self._query(SAFETY_CMD_GET_CT_CAL, devices.safety_get_ct_cal(), timeout)
        if isinstance(value, OkReason):
            raise SafetyQueryError(f"GET_CT_CAL {value.describe()}")
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
                    raise SafetyQueryError(
                        f"SAFETY request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise SafetyQueryError(
                        f"SAFETY request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s -- note this means the ESP "
                        "didn't answer, not that the isolated link is down (a down "
                        "link is a normal reply with link_up = 0)"
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
                log.exception("error handling SAFETY frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_safety_response(frame.payload)
        except SafetyResponseError as exc:
            log.warning("dropping malformed SAFETY response: %s", exc)
            return

        if isinstance(value, SafetyStatus):
            self.last_status = value

        with self._pending_lock:
            pending = self._pending
        if pending is not None and (
            pending.subcommand == subcommand
            or _reply_matches_request(pending.subcommand, subcommand)
        ):
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding: a stale reply to a query we already gave up on.
        # Nothing on this task is ever pushed unsolicited.
        #
        # Counted, not just logged at debug level, because this is the visible
        # edge of the reply-pairing defect documented on _send_reject_window():
        # a reply late enough to land here is one that ALSO could have landed on
        # the next call's pending slot and been reported as that call's answer.
        # A non-zero late_replies is therefore the signal that any single
        # mutating result from this client may be describing the previous
        # request. Left as a counter plus one warning rather than an exception
        # because a late reply is not itself an error -- the command it belongs
        # to was still delivered and acted on.
        self.late_replies += 1
        log.warning(
            "SAFETY reply 0x%02X arrived with nothing outstanding (late_replies=%d) -- "
            "a mutating result reported around now may belong to the previous request; "
            "see SafetyClient._send_reject_window's docstring",
            subcommand,
            self.late_replies,
        )
