"""SAFETY wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403
from .devices_thermo import thermo_fault_labels  # noqa: F401
from .devices_common import (  # noqa: F401
    OkReason,
    _check_bool_byte,
    _check_finite,
    _check_i8,
    _check_range,
    _check_u8,
    _check_u16,
    _decode_ok_reason,
    _decoded_float,
)


# ---------------------------------------------------------------------------
# SAFETY -- the isolated link to the RP2040 (task_id = UART_TASK_ID_SAFETY)
#
# The Pico owns the safety thermocouple board on J7, the three current-sense
# channels, the E-stop input and the safety relay K4; the ESP only polls it
# across three optocouplers and caches the last good answer. The one thing
# this firmware genuinely *drives* is the Fault line (GPIO6, an output) --
# there is no hardware path for the Pico to signal the ESP outside the UART.
#
# The Pico firmware that answers this protocol does not exist in this
# repository yet, so link_up = 0 with age = 0xFFFF is the expected state
# today, not a fault to chase.
# ---------------------------------------------------------------------------
SAFETY_FLAG_LABELS: dict[int, str] = {
    SafetyFlag.LINK_UP: "link up",
    SafetyFlag.FAULT: "fault line asserted (by us)",
    SafetyFlag.ESTOP: "E-stop asserted",
    SafetyFlag.RELAY: "safety relay K4 energized",
    # NOT "heating enable granted" -- that label shipped a real bug on the ESP
    # side (fixed 2026-08-27) and was still live here. SaftyFW derives this bit
    # from relay_owner being in the ARMED state, so it is true on any healthy,
    # past-its-grace-period safety processor REGARDLESS of whether anyone ever
    # sent REQUEST_ENABLE. Reading it as "my enable request was granted" is
    # what produced a control that displayed the opposite of what it did.
    # Whether heat was actually granted is RELAY (did K4 close) above.
    SafetyFlag.ENABLED: "SaftyFW armed (relay_owner not tripped)",
    SafetyFlag.TEMP_VALID: "safety thermocouple valid",
}


class SafetyResponseError(ValueError):
    """Raised when a SAFETY response payload does not match its wire layout."""


def safety_get_status() -> bytes:
    """0x01 GET_STATUS request (query): no args."""
    return struct.pack("<B", SAFETY_CMD_GET_STATUS)


def safety_request_enable(enable: bool) -> bytes:
    """0x02 REQUEST_ENABLE: ask the safety processor to permit (or drop) heating.

    Advisory only -- the Pico can refuse, and its own interlocks always win.
    """
    return struct.pack("<BB", SAFETY_CMD_REQUEST_ENABLE, _check_bool_byte(enable))


def safety_ping() -> bytes:
    """0x03 PING: force an immediate poll instead of waiting for the next tick."""
    return struct.pack("<B", SAFETY_CMD_PING)


def safety_get_link_stats() -> bytes:
    """0x04 GET_LINK_STATS request (query): no args."""
    return struct.pack("<B", SAFETY_CMD_GET_LINK_STATS)


def safety_get_diag() -> bytes:
    """0x0C GET_DIAG request (query): no args.

    CommonFW/docs/LINK_PROTOCOL.md sec 7's "mirror all of it on the PC-link
    SAFETY task" -- answered from the ESP's cache of the Pico's last DIAG
    (Frame B) push, never a live round trip to the Pico. diag_ever_received
    false means no such frame has arrived this ESP boot -- the expected state
    with no Pico firmware attached, not an error.
    """
    return struct.pack("<B", SAFETY_CMD_GET_DIAG)


def safety_get_trip_event() -> bytes:
    """0x15 GET_TRIP_EVENT request (query): no args.

    Same cache-only mirror as :func:`safety_get_diag`, for the Pico's last
    TRIP_EVENT (Frame D) push -- "why did the kiln stop," preserved until a
    newer trip replaces it (never cleared by CLEAR_TRIP itself).
    """
    return struct.pack("<B", SAFETY_CMD_GET_TRIP_EVENT)


def safety_get_fw_version() -> bytes:
    """0x0B GET_FW_VERSION request (query): no args.

    Same cache-only mirror as :func:`safety_get_diag`/:func:`safety_get_trip_event`,
    for the Pico's own FW_VERSION (Frame C) push -- build identity and config
    CRC, not telemetry. Shares its command id with the Pico-side request/reply
    pair (CommonFW/docs/LINK_PROTOCOL.md sec 4/6); the ESP answers from its own
    cache of the last FW_VERSION frame the Pico sent, never a live round trip.
    """
    return struct.pack("<B", SAFETY_CMD_GET_FW_VERSION)


def safety_set_poll_period(period_ms: int) -> bytes:
    """0x05 SET_POLL_PERIOD: u16 LE ms, 0 = stop polling."""
    return struct.pack(
        "<BH", SAFETY_CMD_SET_POLL_PERIOD, _check_u16(period_ms, "period_ms")
    )


def safety_clear_trip() -> bytes:
    """0x0A CLEAR_TRIP: clear a latched safety trip on the Pico. No args.

    The trip_mask deliberately does not travel over this link. The ESP
    derives it from its own cached copy of the Pico's DIAG state rather than
    trusting one supplied by the PC (safety_link_send_clear_trip()'s doc
    comment), so this request is the bare command byte.

    This exists because a trip LATCHES. Once safety_guards_tick() sets
    is_tripped it returns early and stops re-evaluating, so removing whatever
    caused the trip does NOT clear it -- a board that tripped because the
    isolated fault line went high stays tripped after the line goes low
    again. Until this command existed on the PC side, the only way out was a
    power cycle.

    Clearing is a request, not an order: link_task_handle_clear_trip() calls
    safety_guards_try_clear(), which re-evaluates the guard against live
    inputs and refuses if the condition still holds. So this cannot be used
    to paper over a real fault -- asking to clear a trip whose cause is still
    present simply leaves it tripped.

    Fire-and-forget, like SET_CONFIG: no reply on the wire. Read the outcome
    from the next safety_get_status() poll.
    """
    return struct.pack("<B", SAFETY_CMD_CLEAR_TRIP)


def safety_set_fault_out(assert_fault: bool) -> bytes:
    """0x06 SET_FAULT_OUT: drive the isolated Fault line (GPIO6).

    A manual override of a line the firmware otherwise asserts on its own
    (loss of the PC link, a thermocouple fault, the watchdog).
    """
    return struct.pack("<BB", SAFETY_CMD_SET_FAULT_OUT, _check_bool_byte(assert_fault))


# safety_set_ct_cal() (0x19 SET_CT_CAL) removed 2026-09-08 -- see
# kilnctrl/safety.py's SafetyClient.set_ct_cal removal comment and
# mcp_server_safety.safety_get_ct_cal's docstring for why this write surface
# was dead-in-practice (it wrote config_store.h's `ct_cal[]` gain/offset
# correction, which never fed S3/S4/S9 presence detection -- only the
# S14/S15 WARN display thresholds) and what the real calibration path is
# (A_fs/zero_mv via the ESP commissioning page). SAFETY_CMD_SET_CT_CAL
# itself, and the Pico-side handler for it, are left in place -- see
# CT_COMMISSIONING_PLAN.md's "record layout" note; this file just no longer
# offers any PC-side way to reach it.


def safety_get_ct_cal() -> bytes:
    """0x22 GET_CT_CAL request (query): no args.

    Through firmware protocol version 6 this shared its wire id (0x1A) with
    the reply, distinguished only by length; version 7 split it onto its own
    id (0x22) so a driver-error refusal reply -- neither 1 byte nor
    SAFETY_CT_CAL_LEN (28) -- can be told apart from a malformed/truncated
    successful reply. See protocol.py's SAFETY_CMD_GET_CT_CAL doc comment.

    UNLIKE :func:`safety_get_status`/:func:`safety_get_diag`/
    :func:`safety_get_fw_version`, this is **not** answered from the ESP's
    cache -- there is no ct_cal state cached on the ESP side at all
    (safety_link.h's safety_link_get_ct_cal() doc comment: "every call is a
    live, blocking round trip to the Pico"). Sending this causes the Pico to
    broadcast a fresh SAFETY_CMD_CT_CAL (0x1A, unchanged) frame, which the
    ESP relays back to the PC under that reply id -- now DIFFERENT from this
    request's id, so a length mismatch or refusal is unambiguous. Expect this
    call to take noticeably longer than the other SAFETY queries -- it
    genuinely crosses the isolated link and can time out if the Pico never
    answers, not just if the ESP itself is unreachable.
    """
    return struct.pack("<B", SAFETY_CMD_GET_CT_CAL)


#: Human-readable thermocouple type names -> the MAX31856 CR1 TC[3:0] wire
#: value SAFETY_CMD_SET_CONFIG carries, mirroring firmware/SaftyFW/src/max31856.h's
#: MAX31856_TC_TYPE_* ordering (B=0, E=1, J=2, K=3, N=4, R=5, S=6, T=7). Only
#: those eight are recognised names here even though the wire field is a full
#: 0-0x0F nibble (uart_task_ids.h's SAFETY_CMD_SET_CONFIG doc comment) --
#: SaftyFW's own config_store only accepts these eight, so a name this table
#: doesn't know would always be refused on the Pico side anyway.
SAFETY_TC_TYPE_NAMES: dict[str, int] = {
    "B": 0x00,
    "E": 0x01,
    "J": 0x02,
    "K": 0x03,
    "N": 0x04,
    "R": 0x05,
    "S": 0x06,
    "T": 0x07,
}


def safety_set_config(tc_type: int) -> bytes:
    """0x16 SET_CONFIG: commission SaftyFW's config_store.h tc_type.

    Fire-and-forget, like CLEAR_TRIP -- no reply on the wire. Refused on the
    Pico side (logged there, not returned here) if the relay is currently
    ARMED, or if `tc_type` isn't a value SaftyFW recognises.

    LOW-LEVEL ENCODER ONLY as of 2026-09-17: ``mcp_server.safety_set_tc_type()``
    no longer calls this -- it writes the identical underlying
    ``config_store_record_t.tc_type`` field over GET/POST
    /api/safety/commissioning instead (config_params.c id 0x0105), which
    independently re-reads and confirms the write rather than trusting a
    wire command with no reply. This raw wire encoder is kept for
    ``SafetyClient.set_config()``/anything that needs the bare nibble sent
    directly, but a caller wanting tc_type commissioned with feedback should
    use ``safety_set_tc_type()``, not this.

    `tc_type` is the raw MAX31856 CR1 TC[3:0] wire value (0-0x0F).
    """
    return struct.pack(
        "<BB", SAFETY_CMD_SET_CONFIG, _check_range(tc_type, 0, 0x0F, "tc_type")
    )


def safety_request_rollback() -> bytes:
    """0x17 ROLLBACK: revert the safety processor to its previous bootloader
    slot, right now.

    tools/PcTools/TODO.md's `ota_rollback(processor)` line, Pico half (the
    ESP half is mcp_server.ota_rollback_esp()). No args, fire-and-forget like
    CLEAR_TRIP/SET_CONFIG -- there is no reply on the wire. Refused entirely
    on SaftyFW's own say-so: the relay is currently ARMED, or (the property
    that matters most) the OTHER bootloader slot is not currently VALID or
    PENDING_VERIFY, so a rollback can never strand the board with zero
    bootable slots. A successful rollback reboots the Pico -- the outcome is
    observed as the link dropping and recovering with a new boot_id on the
    next GET_STATUS poll, not from anything returned here.
    """
    return struct.pack("<B", SAFETY_CMD_ROLLBACK)


@dataclass(frozen=True)
class SafetyStatus:
    """Decoded GET_STATUS reply -- the ESP's cache of the last good poll."""

    flags: SafetyFlag
    temperature_c: float
    cold_junction_c: float
    fault_status: ThermoFault
    current_a: "tuple[float, float, float]"
    age_ms: int
    #: uart_owner's TX-ring drop count on the Pico, saturating (254 =
    #: "254 or more"), as of the last status frame the Pico sent -- None
    #: means "unknown", not "zero": either the Pico has only ever sent a
    #: V1 (23-byte) status frame this ESP boot, or none at all yet. Added
    #: 2026-08-23 (the DIAG-frame-went-dark investigation) specifically to
    #: be readable on a channel proven to still arrive when GET_DIAG's own
    #: tx_frames_dropped cannot -- do not collapse None to 0 anywhere this
    #: value is displayed or logged; that is exactly the ambiguity this
    #: field exists to remove.
    tx_dropped_sat: "int | None"

    @property
    def link_up(self) -> bool:
        return bool(self.flags & SafetyFlag.LINK_UP)

    @property
    def fault_asserted(self) -> bool:
        return bool(self.flags & SafetyFlag.FAULT)

    @property
    def estop(self) -> bool:
        return bool(self.flags & SafetyFlag.ESTOP)

    @property
    def relay_energized(self) -> bool:
        return bool(self.flags & SafetyFlag.RELAY)

    @property
    def enabled(self) -> bool:
        return bool(self.flags & SafetyFlag.ENABLED)

    @property
    def temp_valid(self) -> bool:
        return bool(self.flags & SafetyFlag.TEMP_VALID)

    @property
    def never_received(self) -> bool:
        """True when no status has ever arrived from the Pico.

        Today this is the normal state: the RP2040 firmware isn't written yet.
        """
        return self.age_ms == SAFETY_AGE_NEVER

    @property
    def flag_labels(self) -> "list[str]":
        return [text for bit, text in SAFETY_FLAG_LABELS.items() if self.flags & bit]

    @property
    def fault_labels(self) -> "list[str]":
        return thermo_fault_labels(int(self.fault_status))

    def describe(
        self,
        ct_fitted: "tuple[bool, bool, bool] | None" = None,
        ct_summed_attrib_zone: "int | None" = None,
    ) -> str:
        """Render the status. ``current_a`` on the wire never carries an
        "is this channel physically fitted" bit -- amps_valid does not cross
        this frame (CT_COMMISSIONING_PLAN.md step 4) -- so that has to come
        from the caller, normally GET /api/status's ``ct_fitted`` (preferred:
        read straight off the ESP's committed safety-cfg cache) or, as a
        fallback when that HTTP call is unavailable, the commissioning
        param's ``ct_topology``. ``ct_fitted=None`` (the default) means
        "unknown -- assume per_zone", the old behaviour: every channel prints
        its raw amps. Where an entry is explicitly False (summed-CT
        topology, channels 0/1 -- only GPIO28/channel 2 is wired there) this
        prints "not fitted" rather than a fabricated ``0.00 A``.
        ``ct_summed_attrib_zone`` (0-based zone index, or None for "no single
        zone currently attributable") is only surfaced when ``ct_fitted`` was
        supplied -- it has no meaning in per_zone topology or when the
        caller has no topology information at all.
        """
        if self.never_received:
            age = "never received"
        else:
            age = f"{self.age_ms} ms old"
        set_flags = self.flag_labels or ["no flags set"]
        if self.temp_valid:
            temp = f"{self.temperature_c:.2f} C (CJ {self.cold_junction_c:.2f} C)"
        else:
            # fault_status is transmitted independently of temp_valid
            # (SaftyFW link_task.c's link_task_send_status(): fault_bits is
            # read unconditionally, only tc_c/cj_c are NaN'd when invalid),
            # so it is available here even on a fully invalid reading -- but
            # it does NOT distinguish a real SPI/part fault from a CR1-verify
            # (tc_type mismatch) or DRDY-silence downgrade, both of which
            # zero fault_bits on the wire (thermo_task.c: those paths set
            # valid=false without ever touching fault_bits, or set it to 0
            # outright). "no MAX31856 fault bits set" here means "not a
            # datasheet-visible fault", not "the sensor is fine" -- see
            # thermo_task.c's max31856_tc_type_verified()/plausibility-band
            # downgrades for the other ways this can read invalid.
            labels = self.fault_labels
            reason = "; ".join(labels) if labels else "no MAX31856 fault bits set"
            temp = f"safety TC invalid ({reason})"
        if ct_fitted is None:
            currents = ", ".join(f"{a:.2f} A" for a in self.current_a)
        else:
            currents = ", ".join(
                f"{a:.2f} A" if fitted else "not fitted"
                for a, fitted in zip(self.current_a, ct_fitted)
            )
            ct_zone = f"zone {ct_summed_attrib_zone}" if ct_summed_attrib_zone is not None else "-"
            currents += f" | ct zone: {ct_zone}"
        if self.tx_dropped_sat is None:
            tx_dropped = "tx_dropped unknown (peer sent no V2 status frame yet)"
        elif self.tx_dropped_sat >= 254:
            tx_dropped = "tx_dropped 254+"
        else:
            tx_dropped = f"tx_dropped {self.tx_dropped_sat}"
        return f"{'; '.join(set_flags)} | {temp} | currents {currents} | {age} | {tx_dropped}"


@dataclass(frozen=True)
class SafetyLinkStats:
    """Decoded GET_LINK_STATS reply -- the ESP's own counters for the isolated
    UART, so these move even while the far side is silent."""

    frames_sent: int
    #: GET_STATUS (Frame A) replies applied, ONLY that frame type, despite
    #: the generic-sounding name -- mirrors safety_link_stats_t::
    #: frames_received's own doc comment (firmware/KilnFW/App/drivers/
    #: safety_link.h). 2026-08-23, the DIAG-frame-went-dark investigation:
    #: this field was read more than once this session as "total frames of
    #: any kind" -- it never was. DIAG/POWER staying dark while this climbs
    #: in lockstep with frames_sent, at exactly the poll rate, is a
    #: measurement artifact of what this counter has always meant, not
    #: evidence those frame types are being dropped. See diag_applied/
    #: power_applied below for the counters that actually answer that.
    frames_received: int
    crc_errors: int
    #: REDEFINED 2026-09-10 (docs/audits/
    #: safety_link_get_status_timeout_counter_2026-09-10.md and its follow-up
    #: opus review). NO LONGER "GET_STATUS exchanges that got no reply" --
    #: GET_STATUS is not a request/reply pair (the Pico never answers it),
    #: so that per-exchange definition either produced a permanent, bimodal
    #: ~0%/~100% artifact, or (a first attempted fix) a value that could
    #: never rise above what the existing ~1500 ms link_up age check already
    #: showed. The ESP now writes this once per ~500 ms poll iteration,
    #: incrementing it when NO new STATUS frame was applied anywhere during
    #: that whole iteration (elapsed-time push-throughput, not a per-request
    #: match) -- see safety_link_stats_t::timeouts's own doc comment
    #: (firmware/KilnFW/App/drivers/safety/safety_link.h). Near zero on a
    #: healthy link; rises with real partial loss (e.g. losing 2 of every 3
    #: pushes) well before the link_up age check would trip.
    timeouts: int
    poll_period_ms: int
    #: BROADCAST frames (GET_STATUS/DIAG/POWER/TRIP_EVENT/FW_VERSION -- any
    #: of it) the ESP's own task-7 inbox had no room for and silently
    #: discarded -- distinct from crc_errors, which only counts a frame that
    #: arrived intact and reached a driver-level length/opcode check;
    #: broadcast_dropped counts one that never got that far at all, so it
    #: can be nonzero while crc_errors reads perfectly clean. Added
    #: 2026-08-23 (the DIAG-frame-went-dark investigation). None means
    #: "unknown", same "do not collapse to 0" discipline SafetyStatus.
    #: tx_dropped_sat documents -- a peer ESP built before this field
    #: existed genuinely might have been dropping broadcasts the whole time
    #: with nothing counting it; reporting 0 in that case would claim a
    #: clean bill of health this tool cannot actually vouch for.
    broadcast_dropped: "int | None"
    #: Real "safety_apply_diag()/_power() succeeded N times" counters --
    #: added 2026-08-23 once tx_dropped_sat==0 and broadcast_dropped==0 both
    #: measured clean on hardware while DIAG stayed permanently dark on the
    #: ESP, which meant the open question became "has DIAG actually been
    #: applied more than once, ever" -- something SafetyDiag's own
    #: ever_received (a one-shot bool) cannot answer, and something the
    #: Pico's own diag_uptime_ms cannot answer reliably (an SWD halt of the
    #: Pico can perturb its clock mid-measurement; it cannot un-increment
    #: this counter). None means "unknown", same discipline as
    #: broadcast_dropped/tx_dropped_sat above.
    diag_applied: "int | None"
    power_applied: "int | None"
    #: Deframer/dispatch-level counters, added 2026-08-23 (final round of the
    #: DIAG-frame-went-dark investigation) once diag_applied/power_applied
    #: pinned at exactly 1 per boot with tx_dropped_sat==0 (Pico TX ring,
    #: confirmed live by an SWD read) and broadcast_dropped==0 (this ESP's
    #: per-task inbox, confirmed live by a real nonzero reading during a
    #: reflash burst) both clean -- meaning frames were vanishing somewhere
    #: between "TX ring accepted it" and "the per-task inbox", a gap none of
    #: the existing counters covered. frames_deframed is the raw "what did
    #: this port actually pull off the wire" number, upstream of every
    #: type-based branch (unlike frames_received, GET_STATUS-only).
    #: frames_routed_nowhere is deframed-but-found-no-destination. The
    #: remaining three are deframer reject counts -- confirmed, by reading
    #: the ESP source first, not to already exist under another name before
    #: adding them (this session already spent several rounds on
    #: frames_received turning out to mean something narrower than its
    #: name suggested; these five do not repeat that). None means
    #: "unknown", same discipline as every other counter above.
    frames_deframed: "int | None"
    frames_routed_nowhere: "int | None"
    frame_length_mismatch: "int | None"
    frame_crc_mismatch: "int | None"
    frame_resync: "int | None"
    #: 2026-08-23, one round further: frames_deframed proved DIAG/POWER
    #: arrive CRC-valid at roughly the expected rate; frames_routed_nowhere
    #: and broadcast_dropped both stayed 0, ruling out an unregistered
    #: dst_task and an inbox backing up. dequeued_total is the count of
    #: successful uart_protocol_receive() returns inside
    #: safety_drain_inbox_ex() -- the ESP's confirmed sole consumer of this
    #: inbox (grepped every uart_protocol_receive() call site; every other
    #: one reads a different task's inbox on a different uart_protocol_t
    #: instance) -- counted before its dispatch switch. Compare against
    #: frames_deframed minus frames_received: a match means the drain IS
    #: pulling DIAG/POWER out and losing them inside the switch; a gap means
    #: something else is emptying the queue. unmatched_cmd_count/
    #: last_unmatched_cmd_byte record hits of that switch's default: branch
    #: -- a dequeued, CRC-valid message whose first payload byte matched no
    #: case -- with the actual byte value, not just a count, so a wrong
    #: first byte is seen directly rather than inferred.
    dequeued_total: "int | None"
    unmatched_cmd_count: "int | None"
    last_unmatched_cmd_byte: "int | None"
    #: 2026-08-23, the measurement that ends the DIAG-frame-went-dark
    #: investigation's inference phase: a per-command dequeue histogram,
    #: one field per safety_drain_inbox_ex() switch case (see
    #: SafetyLinkStats' own module-level doc / safety_link.h's
    #: cmd_status_count doc comment for the exact field list and the "why
    #: now" reasoning). These nine plus unmatched_cmd_count must sum to
    #: exactly dequeued_total -- every dequeued message lands in exactly one
    #: of these ten buckets.
    cmd_status_count: "int | None"
    cmd_fw_version_count: "int | None"
    cmd_update_status_count: "int | None"
    cmd_power_count: "int | None"
    cmd_diag_count: "int | None"
    cmd_trip_event_count: "int | None"
    cmd_ct_cal_count: "int | None"
    cmd_config_page_count: "int | None"
    cmd_commit_config_rejected_count: "int | None"

    def describe(self) -> str:
        def _fmt(value: "int | None") -> str:
            return "unknown" if value is None else str(value)

        return (
            f"sent {self.frames_sent}, received {self.frames_received}, "
            f"crc/framing errors {self.crc_errors}, timeouts {self.timeouts}, "
            f"broadcast dropped {_fmt(self.broadcast_dropped)}, "
            f"diag applied {_fmt(self.diag_applied)}, "
            f"power applied {_fmt(self.power_applied)}, "
            f"frames deframed {_fmt(self.frames_deframed)}, "
            f"routed nowhere {_fmt(self.frames_routed_nowhere)}, "
            f"length mismatch {_fmt(self.frame_length_mismatch)}, "
            f"crc mismatch {_fmt(self.frame_crc_mismatch)}, "
            f"resync {_fmt(self.frame_resync)}, "
            f"dequeued {_fmt(self.dequeued_total)}, "
            f"unmatched cmd count {_fmt(self.unmatched_cmd_count)}, "
            f"last unmatched cmd byte {_fmt(self.last_unmatched_cmd_byte)}, "
            f"cmd histogram [status={_fmt(self.cmd_status_count)} "
            f"fw_version={_fmt(self.cmd_fw_version_count)} "
            f"update_status={_fmt(self.cmd_update_status_count)} "
            f"power={_fmt(self.cmd_power_count)} "
            f"diag={_fmt(self.cmd_diag_count)} "
            f"trip_event={_fmt(self.cmd_trip_event_count)} "
            f"ct_cal={_fmt(self.cmd_ct_cal_count)} "
            f"config_page={_fmt(self.cmd_config_page_count)} "
            f"commit_config_rejected={_fmt(self.cmd_commit_config_rejected_count)}], "
            f"poll period {self.poll_period_ms} ms"
        )


#: SaftyFW's safety_trip_t (firmware/SaftyFW/docs/ARCHITECTURE.md) --
#: SAFETY_TRIP_INEFFECTIVE (S9, "the contactor is welded/bypassed and current
#: is still flowing after the relay opened") is the one value a GUI must
#: never render like an ordinary trip (CommonFW/docs/LINK_PROTOCOL.md sec 7:
#: "Different required action, so it must not look like the others"). Kept
#: here, not just in safety_page.html's own copy, so a PC-side caller can
#: make the same distinction.
SAFETY_TRIP_INEFFECTIVE = 10

#: Full safety_trip_t reason -> (enum name, guard tag) map
#: (firmware/SaftyFW/docs/ARCHITECTURE.md). Kept here so a caller/human never
#: has to compute `1 << (reason - 1)` by hand or guess which guard a reason
#: number names -- CLAUDE.md documented this wrong (bit 6/0x0040 for S6a,
#: actually bit 5/0x0020) for most of 2026-09-09 before being corrected, and
#: that mistake was propagated into other docs the same day. The bit
#: position is NOT the guard number past S3 -- S4 and S10 are WARN-only and
#: leave gaps in the enum, so `reason - 1` diverges from the guard number
#: from S5 onward.
SAFETY_TRIP_NAMES: "dict[int, tuple[str, str]]" = {
    0: ("SAFETY_TRIP_NONE", "-"),
    1: ("SAFETY_TRIP_OVERTEMP", "S1"),
    2: ("SAFETY_TRIP_OVER_SETPOINT", "S2"),
    3: ("SAFETY_TRIP_LOAD_STUCK_ON", "S3"),
    5: ("SAFETY_TRIP_SENSOR_INVALID", "S5"),
    6: ("SAFETY_TRIP_MAIN_FAULT", "S6a"),
    7: ("SAFETY_TRIP_LINK_DEAD", "S6b"),
    8: ("SAFETY_TRIP_ESTOP", "S7"),
    9: ("SAFETY_TRIP_RATE", "S8"),
    10: ("SAFETY_TRIP_INEFFECTIVE", "S9"),
    12: ("SAFETY_TRIP_FROZEN_SENSOR", "S11"),
    13: ("SAFETY_TRIP_ENCLOSURE_TEMP", "S12"),
    14: ("SAFETY_TRIP_BORROWED_STALE", "S13"),
    15: ("SAFETY_TRIP_CONFIG_CORRUPT", "-"),
    16: ("SAFETY_TRIP_SELF_TEST", "-"),
}


def safety_trip_mask_for_reason(reason: int) -> int:
    """Mirrors link_frame_trip_mask_for_reason() (firmware/SaftyFW/src/tasks/
    link_frame.c) exactly: 0 for SAFETY_TRIP_NONE, else 1 << (reason - 1)."""
    if reason == 0:
        return 0
    return 1 << (reason - 1)


def describe_safety_trip_reason(reason: int) -> str:
    """'<name> (<guard tag>)' for a trip_reason byte, or a plain unknown-
    reason label -- never just a bare integer a reader has to look up."""
    name, tag = SAFETY_TRIP_NAMES.get(reason, (f"unknown reason {reason}", "?"))
    if tag == "-":
        return name
    return f"{name} ({tag})"


@dataclass(frozen=True)
class SafetyDiag:
    """Decoded GET_DIAG (0x0C) reply -- the ESP's cache of the Pico's last
    DIAG (Frame B) push. Meaningless (all-zero) unless ``ever_received``."""

    ever_received: bool
    trip_reason: int
    warn_mask: int
    trip_mask: int
    uptime_ms: int
    boot_reason: int
    context_age_100ms: int
    context_frames_ok: int
    context_frames_bad: int
    tx_frames_dropped: int
    state: int
    flags: int
    log_frames_dropped: int

    @property
    def context_never_received(self) -> bool:
        """True when no PUSH_CONTEXT frame has ever reached the Pico -- 255
        (SAFETY_LINK_DIAG_CONTEXT_AGE_NEVER) is the sentinel, not a real age."""
        return self.context_age_100ms == 0xFF

    def describe(self) -> str:
        """Human-readable summary, notably the Pico's last-boot reason
        (kilnlink_diag.h's boot_reason bits) -- the gap this exists to close:
        nothing else on the PC side names *why* the Pico last rebooted.
        boot_reason bits are POWERON=0x01/WATCHDOG=0x02/BROWNOUT=0x04; more
        than one bit set is possible on the wire (e.g. a brownout during a
        watchdog reset) so all set bits are named, not just the first match.
        """
        if not self.ever_received:
            return "never received (no Pico firmware attached, or link never up)"

        boot_bits = []
        if self.boot_reason & 0x01:
            boot_bits.append("power-on")
        if self.boot_reason & 0x02:
            boot_bits.append("watchdog")
        if self.boot_reason & 0x04:
            boot_bits.append("brownout")
        boot_desc = "+".join(boot_bits) if boot_bits else f"unknown (0x{self.boot_reason:02x})"

        state_names = {0: "init", 1: "grace", 2: "armed", 3: "warn", 4: "tripped"}
        state_desc = state_names.get(self.state, f"unknown ({self.state})")

        flag_bits = []
        if self.flags & 0x01:
            flag_bits.append("sim_context_seen")
        if self.flags & 0x02:
            flag_bits.append("calibration_missing")
        if self.flags & 0x04:
            flag_bits.append("estop_unwired_suspect")

        context_age = (
            "never received" if self.context_never_received else f"{self.context_age_100ms * 100} ms"
        )

        reason_desc = describe_safety_trip_reason(self.trip_reason)
        expected_mask = safety_trip_mask_for_reason(self.trip_reason)
        if self.trip_mask == expected_mask:
            mask_desc = f"0x{self.trip_mask:04x}"
        else:
            # Disagreement between the reason byte and the mask field --
            # exactly the ambiguity that led CLAUDE.md to document the wrong
            # constant for S6a on 2026-09-09. Never let a caller silently
            # trust one field without seeing the other contradicts it.
            mask_desc = (
                f"0x{self.trip_mask:04x} (MISMATCH: trip_reason {self.trip_reason} "
                f"implies 0x{expected_mask:04x} = 1 << ({self.trip_reason} - 1))"
            )

        return (
            f"boot reason: {boot_desc} | state {state_desc} | "
            f"trip_reason {self.trip_reason} [{reason_desc}] | "
            f"warn_mask 0x{self.warn_mask:04x} | "
            f"trip_mask {mask_desc} | uptime {self.uptime_ms} ms | "
            f"context age {context_age} | context frames ok {self.context_frames_ok}, "
            f"bad {self.context_frames_bad} | tx frames dropped {self.tx_frames_dropped} | "
            f"log frames dropped {self.log_frames_dropped}"
            + (f" | flags [{', '.join(flag_bits)}]" if flag_bits else "")
        )


@dataclass(frozen=True)
class SafetyTripEvent:
    """Decoded GET_TRIP_EVENT (0x15) reply -- the ESP's cache of the Pico's
    last TRIP_EVENT (Frame D) push, preserved until a newer trip replaces it
    (never cleared by CLEAR_TRIP). Meaningless (all-zero) unless
    ``ever_received``."""

    ever_received: bool
    last_seq: int
    trip_reason: int
    uptime_ms: int
    safety_tc_c: float
    deciding_threshold: float
    current_a: "tuple[float, float, float]"
    relay_recent_mask: int
    context_age_100ms: int
    age_ms: int

    @property
    def ineffective(self) -> bool:
        """True for SAFETY_TRIP_INEFFECTIVE (S9) -- see that constant's
        comment for why this must render differently from every other trip."""
        return self.ever_received and self.trip_reason == SAFETY_TRIP_INEFFECTIVE


@dataclass(frozen=True)
class SafetyFwVersion:
    """Decoded GET_FW_VERSION (0x0B) reply -- the ESP's cache of the Pico's
    own FW_VERSION (Frame C) push: build identity and active config CRC, the
    other half of the "mirror it on the PC-link SAFETY task" ask alongside
    :class:`SafetyDiag`/:class:`SafetyTripEvent` (CommonFW/docs/LINK_PROTOCOL.md
    sec 7).

    ``protocol_version``/``min_compatible`` are parsed *first*, deliberately --
    LINK_PROTOCOL.md sec 6: "read bytes 1-4 first and decide compatibility
    before parsing anything after them," so a version-incompatible peer's
    frame can still be read far enough to learn why it is incompatible.

    ``commit``/``built`` are empty strings when the Pico's build has no known
    identity (``commit_len``/``datetime_len`` == 0 on the wire) -- never a
    placeholder standing in for "unknown". ``dirty`` is the wire's own
    dirty-or-unknown bit: LINK_PROTOCOL.md sec 6 requires "unknown" and
    "dirty" to map to the same value, so this field alone cannot distinguish
    them; an empty ``commit`` is the signal that the identity itself is
    unknown, not just that the tree was dirty.
    """

    protocol_version: int
    min_compatible: int
    dirty: bool
    commit: str
    built: str
    boot_id: int
    config_version: int
    config_crc: int

    @property
    def commissioned(self) -> bool:
        """False when config_crc == 0 -- LINK_PROTOCOL.md sec 6: "a config CRC
        of zero means running on compiled-in defaults that were never
        commissioned."""
        return self.config_crc != 0

    @property
    def known(self) -> bool:
        """False when the Pico has never reported a build identity -- an
        empty ``commit`` (see the class docstring: this, not ``dirty``, is
        the "unknown" signal)."""
        return bool(self.commit)

    def describe(self) -> str:
        if not self.known:
            identity = "unknown (Pico has not reported a build identity)"
        else:
            dirty = " (dirty)" if self.dirty else ""
            identity = f"{self.commit}{dirty} built {self.built or 'unknown time'}"
        commissioned = "commissioned" if self.commissioned else "NOT commissioned (default config)"
        return (
            f"Pico build: {identity}, boot_id={self.boot_id}, "
            f"config_version={self.config_version}, "
            f"config_crc=0x{self.config_crc:04X} ({commissioned}), "
            f"protocol v{self.protocol_version} (min compatible v{self.min_compatible})"
        )


@dataclass(frozen=True)
class SafetyCtCalChannel:
    """One channel's calibration, as SaftyFW's config_store.c holds it."""

    calibrated: bool
    gain: float
    offset: float


@dataclass(frozen=True)
class SafetyCtCal:
    """Decoded CT_CAL (0x1A) reply -- the three current-sense channels'
    stored CT amps calibration, read live from the Pico (never cached on the
    ESP; see :func:`safety_get_ct_cal`). ``channels`` is always exactly
    :data:`~kilnctrl.protocol.SAFETY_CT_CAL_NUM_CHANNELS` (3) long, index 0-2.

    A channel with ``calibrated`` False has meaningless gain/offset --
    current_sense.c never reads them for such a channel -- so a caller must
    check ``calibrated`` before displaying or trusting a channel's numbers as
    a real correction.
    """

    channels: "tuple[SafetyCtCalChannel, ...]"


def parse_safety_response(
    payload: bytes,
) -> "tuple[int, SafetyStatus | SafetyLinkStats | SafetyDiag | SafetyTripEvent | SafetyFwVersion | SafetyCtCal | OkReason]":
    """Decode a SAFETY query reply into ``(subcommand, value)``.

    Layouts (uart_task_ids.h)::

        GET_STATUS:      byte0=0x01, flags u8, tc f32, cj f32, SR u8,
                         current1..3 f32, age u16 LE,
                         [tx_dropped_sat u8, extra_flags u8 (bit0
                          tx_dropped_known)]                (25 or 27 bytes --
                         the last two bytes are V2, added 2026-08-23; a peer
                         ESP built before that change sends 25 and
                         tx_dropped_sat decodes as None/unknown, never 0)
        GET_LINK_STATS:  byte0=0x04, sent u32, received u32 (GET_STATUS
                         replies ONLY, despite the name -- see
                         SafetyLinkStats.frames_received's own doc comment),
                         crc u32, timeouts u32, poll period u16 LE,
                         [broadcast_dropped u32 LE,
                          [diag_applied u32 LE, power_applied u32 LE,
                           [frames_deframed u32 LE, frames_routed_nowhere
                            u32 LE, frame_length_mismatch u32 LE,
                            frame_crc_mismatch u32 LE, frame_resync u32 LE,
                            [dequeued_total u32 LE, unmatched_cmd_count
                             u32 LE, last_unmatched_cmd_byte u8,
                             [nine u32 LE per-command dequeue counts --
                              status/fw_version/update_status/power/diag/
                              trip_event/ct_cal/config_page/
                              commit_config_rejected, in that order]]]]]
                         (19, 23, 31, 51, 60, or 96 bytes -- V2/V3/V4/V5/V6,
                         all added 2026-08-23; a peer ESP built before any
                         given change decodes that change's field(s) as
                         None/unknown, never 0)
        GET_DIAG:        byte0=0x0C, flags u8 (bit0 ever_received),
                         trip_reason u8, warn_mask u16 LE, trip_mask u16 LE,
                         uptime_ms u32 LE, boot_reason u8,
                         context_age_100ms u8, context_frames_ok u32 LE,
                         context_frames_bad u32 LE, tx_frames_dropped u32 LE,
                         state u8, diag_flags u8, log_frames_dropped u32 LE
                                                                     (31 bytes)
        GET_TRIP_EVENT:  byte0=0x15, flags u8 (bit0 ever_received),
                         last_seq u8, trip_reason u8, uptime_ms u32 LE,
                         safety_tc_c f32 LE, deciding_threshold f32 LE,
                         current1..3 f32 LE, relay_recent_mask u8,
                         context_age_100ms u8, age_ms u32 LE         (34 bytes)
        GET_FW_VERSION:  byte0=0x0B, protocol_version u16 LE, min_compatible
                         u16 LE, dirty u8, commit_len u8, commit (N1 ASCII),
                         datetime_len u8, datetime (N2 ASCII), boot_id u8,
                         config_version u8, config_crc u16 LE  (variable, see
                         CommonFW/docs/LINK_PROTOCOL.md sec 6 Frame C)
        GET_CT_CAL:      byte0=0x1A (SAFETY_CMD_CT_CAL, the reply's OWN id --
                         DIFFERENT from the 0x22 request id since firmware
                         protocol version 7), then 3 channels of
                         [calibrated u8, gain f32 LE, offset f32 LE]
                         (28 bytes total -- kilnlink_ct_cal.h). A reply
                         carrying the 0x22 request id instead is a
                         driver-error refusal, decoded via _decode_ok_reason
                         like SET_CT_CAL/SET_CONFIG's own refusal replies
                         below, not this success shape.
    """
    if len(payload) < 1:
        raise SafetyResponseError("SAFETY response is empty")
    subcommand = payload[0]

    if subcommand == SAFETY_CMD_GET_STATUS:
        # V1 (25 bytes, pre-2026-08-23) and V2 (27, + tx_dropped_sat/
        # extra_flags) both accepted -- never just one: an ESP flashed before
        # this change and a pc_tools build flashed after it (or vice versa)
        # must not simply stop talking to each other over one extra field,
        # the same reasoning Frame A's own V1/V2 split documents on the
        # SaftyFW<->ESP hop this field originates from.
        if len(payload) not in (25, 27):
            raise SafetyResponseError(
                f"GET_STATUS response must be 25 or 27 bytes, got {len(payload)}"
            )
        (
            flags,
            temperature,
            cold_junction,
            fault_status,
            current1,
            current2,
            current3,
            age_ms,
        ) = struct.unpack_from("<BffBfffH", payload, 1)
        tx_dropped_sat: "int | None" = None
        if len(payload) == 27:
            tx_dropped_raw, extra_flags = struct.unpack_from("<BB", payload, 25)
            if extra_flags & 0x01:  # SAFETY_LINK_STATUS_EXTRA_FLAG_TX_DROPPED_KNOWN
                tx_dropped_sat = tx_dropped_raw
        # NaN is how "no valid reading" arrives here (the Pico's own TC can be
        # invalid); an infinity is not a value this protocol can carry, and a
        # bogus current reading is exactly the kind of thing that must not
        # reach a safety readout looking legitimate.
        try:
            temperature = _decoded_float(temperature, "safety temperature", allow_nan=True)
            cold_junction = _decoded_float(
                cold_junction, "safety cold junction", allow_nan=True
            )
            current1 = _decoded_float(current1, "current sense 1", allow_nan=True)
            current2 = _decoded_float(current2, "current sense 2", allow_nan=True)
            current3 = _decoded_float(current3, "current sense 3", allow_nan=True)
        except ValueError as exc:
            raise SafetyResponseError(str(exc)) from exc
        return subcommand, SafetyStatus(
            flags=SafetyFlag(flags & 0x3F),
            temperature_c=temperature,
            cold_junction_c=cold_junction,
            fault_status=ThermoFault(fault_status),
            current_a=(current1, current2, current3),
            age_ms=age_ms,
            tx_dropped_sat=tx_dropped_sat,
        )

    if subcommand == SAFETY_CMD_GET_LINK_STATS:
        # V1 (19 bytes, pre-2026-08-23), V2 (23, + broadcast_dropped u32),
        # V3 (31, + diag_applied u32 + power_applied u32), V4 (51, +
        # frames_deframed/frames_routed_nowhere/frame_length_mismatch/
        # frame_crc_mismatch/frame_resync, all u32), V5 (60, +
        # dequeued_total u32 + unmatched_cmd_count u32 +
        # last_unmatched_cmd_byte u8), and V6 (96, + nine u32 per-command
        # dequeue counts) all accepted, same reasoning as GET_STATUS's own
        # V1/V2 split above: whichever of ESP/pc_tools gets rebuilt first
        # must not stop talking to the other over an extra counter.
        if len(payload) not in (19, 23, 31, 51, 60, 96):
            raise SafetyResponseError(
                f"GET_LINK_STATS response must be 19, 23, 31, 51, 60, or 96 "
                f"bytes, got {len(payload)}"
            )
        sent, received, crc_errors, timeouts, poll_period = struct.unpack_from(
            "<IIIIH", payload, 1
        )
        broadcast_dropped: "int | None" = None
        if len(payload) >= 23:
            (broadcast_dropped,) = struct.unpack_from("<I", payload, 19)
        diag_applied: "int | None" = None
        power_applied: "int | None" = None
        if len(payload) >= 31:
            diag_applied, power_applied = struct.unpack_from("<II", payload, 23)
        frames_deframed: "int | None" = None
        frames_routed_nowhere: "int | None" = None
        frame_length_mismatch: "int | None" = None
        frame_crc_mismatch: "int | None" = None
        frame_resync: "int | None" = None
        if len(payload) >= 51:
            (
                frames_deframed,
                frames_routed_nowhere,
                frame_length_mismatch,
                frame_crc_mismatch,
                frame_resync,
            ) = struct.unpack_from("<IIIII", payload, 31)
        dequeued_total: "int | None" = None
        unmatched_cmd_count: "int | None" = None
        last_unmatched_cmd_byte: "int | None" = None
        if len(payload) >= 60:
            dequeued_total, unmatched_cmd_count = struct.unpack_from("<II", payload, 51)
            (last_unmatched_cmd_byte,) = struct.unpack_from("<B", payload, 59)
        cmd_status_count: "int | None" = None
        cmd_fw_version_count: "int | None" = None
        cmd_update_status_count: "int | None" = None
        cmd_power_count: "int | None" = None
        cmd_diag_count: "int | None" = None
        cmd_trip_event_count: "int | None" = None
        cmd_ct_cal_count: "int | None" = None
        cmd_config_page_count: "int | None" = None
        cmd_commit_config_rejected_count: "int | None" = None
        if len(payload) == 96:
            (
                cmd_status_count,
                cmd_fw_version_count,
                cmd_update_status_count,
                cmd_power_count,
                cmd_diag_count,
                cmd_trip_event_count,
                cmd_ct_cal_count,
                cmd_config_page_count,
                cmd_commit_config_rejected_count,
            ) = struct.unpack_from("<IIIIIIIII", payload, 60)
        return subcommand, SafetyLinkStats(
            frames_sent=sent,
            frames_received=received,
            crc_errors=crc_errors,
            timeouts=timeouts,
            poll_period_ms=poll_period,
            broadcast_dropped=broadcast_dropped,
            diag_applied=diag_applied,
            power_applied=power_applied,
            frames_deframed=frames_deframed,
            frames_routed_nowhere=frames_routed_nowhere,
            frame_length_mismatch=frame_length_mismatch,
            frame_crc_mismatch=frame_crc_mismatch,
            frame_resync=frame_resync,
            dequeued_total=dequeued_total,
            unmatched_cmd_count=unmatched_cmd_count,
            last_unmatched_cmd_byte=last_unmatched_cmd_byte,
            cmd_status_count=cmd_status_count,
            cmd_fw_version_count=cmd_fw_version_count,
            cmd_update_status_count=cmd_update_status_count,
            cmd_power_count=cmd_power_count,
            cmd_diag_count=cmd_diag_count,
            cmd_trip_event_count=cmd_trip_event_count,
            cmd_ct_cal_count=cmd_ct_cal_count,
            cmd_config_page_count=cmd_config_page_count,
            cmd_commit_config_rejected_count=cmd_commit_config_rejected_count,
        )

    if subcommand == SAFETY_CMD_GET_DIAG:
        if len(payload) != 31:
            raise SafetyResponseError(
                f"GET_DIAG response must be 31 bytes, got {len(payload)}"
            )
        (
            flags,
            trip_reason,
            warn_mask,
            trip_mask,
            uptime_ms,
            boot_reason,
            context_age_100ms,
            context_frames_ok,
            context_frames_bad,
            tx_frames_dropped,
            state,
            diag_flags,
            log_frames_dropped,
        ) = struct.unpack_from("<BBHHIBBIIIBBI", payload, 1)
        return subcommand, SafetyDiag(
            ever_received=bool(flags & 0x01),
            trip_reason=trip_reason,
            warn_mask=warn_mask,
            trip_mask=trip_mask,
            uptime_ms=uptime_ms,
            boot_reason=boot_reason,
            context_age_100ms=context_age_100ms,
            context_frames_ok=context_frames_ok,
            context_frames_bad=context_frames_bad,
            tx_frames_dropped=tx_frames_dropped,
            state=state,
            flags=diag_flags,
            log_frames_dropped=log_frames_dropped,
        )

    if subcommand == SAFETY_CMD_GET_TRIP_EVENT:
        if len(payload) != 34:
            raise SafetyResponseError(
                f"GET_TRIP_EVENT response must be 34 bytes, got {len(payload)}"
            )
        (
            flags,
            last_seq,
            trip_reason,
            uptime_ms,
            safety_tc_c,
            deciding_threshold,
            current1,
            current2,
            current3,
            relay_recent_mask,
            context_age_100ms,
            age_ms,
        ) = struct.unpack_from("<BBBIfffffBBI", payload, 1)
        return subcommand, SafetyTripEvent(
            ever_received=bool(flags & 0x01),
            last_seq=last_seq,
            trip_reason=trip_reason,
            uptime_ms=uptime_ms,
            safety_tc_c=safety_tc_c,
            deciding_threshold=deciding_threshold,
            current_a=(current1, current2, current3),
            relay_recent_mask=relay_recent_mask,
            context_age_100ms=context_age_100ms,
            age_ms=age_ms,
        )

    if subcommand == SAFETY_CMD_GET_FW_VERSION:
        # Variable-length: LINK_PROTOCOL.md sec 6 Frame C. Parse strictly in
        # wire order and bounds-check before every read -- this is untrusted
        # input from another processor relayed through the ESP (CommonFW/
        # README.md rule 6), and a byte-offset mistake here would produce
        # confident garbage on a safety diagnostics surface.
        #
        #   offset 1..2  protocol_version u16 LE   <- read + gate first
        #   offset 3..4  min_compatible   u16 LE   <- (sec 6: "read bytes
        #                                              1-4 first")
        #   offset 5     dirty            u8
        #   offset 6     commit_len (N1)  u8
        #   offset 7..            N1 * u8  commit, ASCII, not null-terminated
        #   next 1                u8       datetime_len (N2)
        #   next N2                N2 * u8  datetime, ASCII
        #   next 1                u8       boot_id
        #   next 1                u8       config_version
        #   next 2                u16 LE   config_crc
        if len(payload) < 7:
            raise SafetyResponseError(
                f"GET_FW_VERSION response too short: {len(payload)} bytes "
                "(need >= 7 to reach commit_len)"
            )
        protocol_version, min_compatible, dirty, commit_len = struct.unpack_from(
            "<HHBB", payload, 1
        )
        commit_start = 7
        commit_end = commit_start + commit_len
        if len(payload) < commit_end + 1:
            raise SafetyResponseError(
                f"GET_FW_VERSION response too short: {len(payload)} bytes, "
                f"commit_len={commit_len} implies at least {commit_end + 1} "
                "bytes (need 1 more for datetime_len)"
            )
        commit = payload[commit_start:commit_end].decode("ascii", errors="replace")
        datetime_len = payload[commit_end]
        datetime_start = commit_end + 1
        datetime_end = datetime_start + datetime_len
        suffix_end = datetime_end + 4  # boot_id(1) + config_version(1) + config_crc(2)
        if len(payload) != suffix_end:
            raise SafetyResponseError(
                f"GET_FW_VERSION response length mismatch: commit_len={commit_len}, "
                f"datetime_len={datetime_len} imply exactly {suffix_end} bytes, "
                f"got {len(payload)}"
            )
        built = payload[datetime_start:datetime_end].decode("ascii", errors="replace")
        boot_id, config_version, config_crc = struct.unpack_from(
            "<BBH", payload, datetime_end
        )
        return subcommand, SafetyFwVersion(
            protocol_version=protocol_version,
            min_compatible=min_compatible,
            dirty=bool(dirty),
            commit=commit,
            built=built,
            boot_id=boot_id,
            config_version=config_version,
            config_crc=config_crc,
        )

    if subcommand == SAFETY_CMD_CT_CAL:
        # The reply's OWN id (0x1A) -- different from SAFETY_CMD_GET_CT_CAL
        # (0x22, the request's id) since firmware protocol version 7. Always
        # the 28-byte success payload; a driver-error refusal arrives under
        # the request's id instead (handled just below), never this one.
        expected_len = 1 + SAFETY_CT_CAL_NUM_CHANNELS * 9  # calibrated(1)+gain f32(4)+offset f32(4)
        if len(payload) != expected_len:
            raise SafetyResponseError(
                f"GET_CT_CAL response must be {expected_len} bytes, got {len(payload)}"
            )
        channels = []
        for ch in range(SAFETY_CT_CAL_NUM_CHANNELS):
            offset = 1 + ch * 9
            calibrated, gain, cal_offset = struct.unpack_from("<Bff", payload, offset)
            channels.append(
                SafetyCtCalChannel(calibrated=bool(calibrated), gain=gain, offset=cal_offset)
            )
        return subcommand, SafetyCtCal(channels=tuple(channels))

    if subcommand == SAFETY_CMD_GET_CT_CAL:
        # This is the REQUEST's id echoed back -- only ever seen when
        # uart_bridge.c's safety_bridge_task() refused the command outright
        # (bridge_reply_reject(), "driver error" -- e.g. the live round trip
        # to the Pico timed out). A successful reply always carries
        # SAFETY_CMD_CT_CAL's id instead, handled above. Splitting the two
        # ids (version 7) is exactly what makes this refusal distinguishable
        # from a truncated/malformed success reply in the first place -- see
        # protocol.py's SAFETY_CMD_GET_CT_CAL doc comment.
        return subcommand, _decode_ok_reason(payload, SafetyResponseError, "GET_CT_CAL")

    if subcommand in (
        SAFETY_CMD_REQUEST_ENABLE,
        SAFETY_CMD_SET_POLL_PERIOD,
        SAFETY_CMD_SET_FAULT_OUT,
        SAFETY_CMD_SET_CT_CAL,
        SAFETY_CMD_SET_CONFIG,
    ):
        # safety_bridge_task() replies nothing at all when one of these is
        # accepted (they are fire-and-forget broadcasts to the Pico, or a
        # local state change) -- a reply only ever means bridge_reply_reject()
        # fired for a "truncated"/"out of range" argument caught before
        # anything was sent (uart_bridge.c, safety_bridge_task()). Decoded
        # the same way CONTROL's SET_ZONE_*/PROFILES' DELETE etc. are, so any
        # reason text survives instead of being an unreachable code path.
        return subcommand, _decode_ok_reason(
            payload, SafetyResponseError, "REQUEST_ENABLE/SET_POLL_PERIOD/SET_FAULT_OUT/SET_CT_CAL/SET_CONFIG"
        )

    raise SafetyResponseError(f"unknown SAFETY response subcommand 0x{subcommand:02X}")


