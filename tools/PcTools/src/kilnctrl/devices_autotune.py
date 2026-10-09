"""AUTOTUNE wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403
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
# AUTOTUNE -- PID autotune (step/relay methods)
# (task_id = UART_TASK_ID_AUTOTUNE)
# ---------------------------------------------------------------------------
class AutotuneResponseError(ValueError):
    """Raised when an AUTOTUNE response payload does not match its wire layout."""


@dataclass(frozen=True)
class AutotuneModel:
    k_gain_c_per_duty: float
    tau_s: float
    dead_time_s: float


@dataclass(frozen=True)
class AutotuneGains:
    kp: float
    ki: float
    kd: float
    rule: int


@dataclass(frozen=True)
class AutotuneRelayResult:
    ku: float
    tu_s: float
    amplitude_c: float


@dataclass(frozen=True)
class AutotuneStatus:
    state: int
    method: int
    zone: int
    elapsed_s: int
    sample_count: int
    actual_c: float
    actual_valid: bool
    duty: float
    model_valid: bool
    model_settled: bool
    model_extrapolation_converged: bool
    model_tau_consistent: bool
    model: AutotuneModel
    proposed_gains: AutotuneGains
    predicted_max_ramp_c_per_hr: float
    relay_valid: bool
    relay: AutotuneRelayResult
    abort_reason: str

    STATE_NAMES = {
        0: "idle", 1: "settling", 2: "stepping", 3: "relay_approach",
        4: "relay_cycling", 5: "done", 6: "aborted",
    }

    @property
    def state_name(self) -> str:
        return self.STATE_NAMES.get(self.state, f"unknown({self.state})")


def autotune_get_status() -> bytes:
    """0x01 GET_STATUS request (query): no args."""
    return struct.pack("<B", AUTOTUNE_CMD_GET_STATUS)


def autotune_start(
    zone: int,
    method: int,
    step_duty_or_setpoint_c: float,
    relay_d: float = -1.0,
    relay_h_c: float = -1.0,
    rule: int = AUTOTUNE_RULE_TL,
) -> bytes:
    """0x02 START: zone, method(0=step,1=relay), setpoint/duty, relay_d,
    relay_h_c, rule -- always sends all 16 argument bytes (fields the
    selected method doesn't use are ignored, same as the HTTP form's
    server-side defaulting). ``relay_d``/``relay_h_c`` <=0 means "engine
    default". Replies ok/fail (+ error text on failure).
    """
    if method not in (AUTOTUNE_METHOD_STEP, AUTOTUNE_METHOD_RELAY):
        raise ValueError(f"method must be 0 (step) or 1 (relay), got {method}")
    if rule not in (AUTOTUNE_RULE_TL, AUTOTUNE_RULE_ZN):
        raise ValueError(f"rule must be 0 (tyreus-luyben) or 1 (ziegler-nichols), got {rule}")
    return struct.pack(
        "<BBBfffB",
        AUTOTUNE_CMD_START,
        _check_u8(zone, "zone"),
        method,
        _check_finite(step_duty_or_setpoint_c, "step_duty_or_setpoint_c"),
        _check_finite(relay_d, "relay_d"),
        _check_finite(relay_h_c, "relay_h_c"),
        rule,
    )


def autotune_abort() -> bytes:
    """0x03 ABORT: no args. Always replies ok."""
    return struct.pack("<B", AUTOTUNE_CMD_ABORT)


def autotune_accept(ack_unsettled: bool = False) -> bytes:
    """0x04 ACCEPT: optional ack_unsettled byte. Replies ok/fail.

    A STEP-method result whose ``AutotuneStatus.model_settled`` is False
    (the fit ended via the 4h max-duration backstop, not genuine settling --
    see ``fopdt_model_t::settled``'s doc comment on the firmware side) is
    refused by the firmware UNLESS this byte is nonzero -- see
    ``autotune_engine_accept()``'s own comment (autotune_engine.h). Callers
    should read ``model_settled`` off the last GET_STATUS response and only
    pass True here after an explicit, deliberate operator choice to accept a
    low-confidence fit -- see ``docs/MCP_SERVERS.md`` / the CLI's own
    confirmation prompt for how that choice is surfaced.
    """
    return struct.pack("<BB", AUTOTUNE_CMD_ACCEPT, 1 if ack_unsettled else 0)


def parse_autotune_response(payload: bytes) -> "tuple[int, object]":
    """Decode an AUTOTUNE reply into ``(subcmd, value)``."""
    if len(payload) < 1:
        raise AutotuneResponseError("AUTOTUNE response is empty")
    subcommand = payload[0]

    if subcommand == AUTOTUNE_CMD_GET_STATUS:
        if len(payload) < 63:
            raise AutotuneResponseError(
                f"GET_STATUS response must be >= 63 bytes, got {len(payload)}"
            )
        state, method, zone = payload[1], payload[2], payload[3]
        elapsed_s = struct.unpack_from("<I", payload, 4)[0]
        sample_count = struct.unpack_from("<H", payload, 8)[0]
        actual_c = struct.unpack_from("<f", payload, 10)[0]
        actual_valid = bool(payload[14])
        duty = struct.unpack_from("<f", payload, 15)[0]
        model_valid = bool(payload[19])
        k_gain, tau_s, dead_time_s = struct.unpack_from("<fff", payload, 20)
        kp, ki, kd = struct.unpack_from("<fff", payload, 32)
        rule = payload[44]
        predicted_max_ramp = struct.unpack_from("<f", payload, 45)[0]
        relay_valid = bool(payload[49])
        ku, tu_s, amplitude_c = struct.unpack_from("<fff", payload, 50)
        abort_len = payload[62]
        abort_reason = ""
        # abort_reason is length-prefixed (bx_put_lstring on the firmware
        # side), so nothing after it can be at a fixed offset -- its own
        # length varies frame to frame. model_settled (added 2026-09-01,
        # UART_PROTOCOL_VERSION 8->9) is genuinely APPENDED after this
        # string, so its offset is computed from abort_len, never
        # hardcoded. A firmware build older than this protocol bump simply
        # doesn't send the trailing byte at all -- defaults to False rather
        # than raising, since a mid-transition build talking a stale
        # firmware is refused earlier by the protocol-version handshake
        # (UART_PROTOCOL_VERSION equality check), not here.
        model_settled_offset = 63 + abort_len
        if abort_len:
            if len(payload) < model_settled_offset:
                raise AutotuneResponseError("GET_STATUS abort_reason truncated")
            abort_reason = payload[63:model_settled_offset].decode("ascii", errors="replace")
        model_settled = (
            bool(payload[model_settled_offset])
            if len(payload) > model_settled_offset
            else False
        )
        # extrapolation_converged / tau_consistent_with_gain (added
        # 2026-09-02, UART_PROTOCOL_VERSION 9->10) -- two MORE bytes
        # genuinely appended after model_settled, same "compute from the
        # previous field's own offset, never hardcode" discipline that
        # field's own comment establishes. Both fold into autotune_engine_
        # accept()'s ack_unsettled gate alongside model_settled (see
        # uart_task_ids.h's Version 10 history and autotune_engine.h's own
        # comment on that function) and are surfaced HERE distinctly so a
        # caller can tell an operator WHICH of the three is unmet, not just
        # that one is. Each independently defaults False if the firmware
        # build predates it (same reasoning as model_settled's own
        # default -- a genuinely mixed-version link is refused earlier by
        # the protocol-version handshake, not here).
        extrapolation_converged_offset = model_settled_offset + 1
        tau_consistent_offset = extrapolation_converged_offset + 1
        model_extrapolation_converged = (
            bool(payload[extrapolation_converged_offset])
            if len(payload) > extrapolation_converged_offset
            else False
        )
        model_tau_consistent = (
            bool(payload[tau_consistent_offset])
            if len(payload) > tau_consistent_offset
            else False
        )
        return subcommand, AutotuneStatus(
            state=state,
            method=method,
            zone=zone,
            elapsed_s=elapsed_s,
            sample_count=sample_count,
            actual_c=actual_c,
            actual_valid=actual_valid,
            duty=duty,
            model_valid=model_valid,
            model_settled=model_settled,
            model_extrapolation_converged=model_extrapolation_converged,
            model_tau_consistent=model_tau_consistent,
            model=AutotuneModel(k_gain_c_per_duty=k_gain, tau_s=tau_s, dead_time_s=dead_time_s),
            proposed_gains=AutotuneGains(kp=kp, ki=ki, kd=kd, rule=rule),
            predicted_max_ramp_c_per_hr=predicted_max_ramp,
            relay_valid=relay_valid,
            relay=AutotuneRelayResult(ku=ku, tu_s=tu_s, amplitude_c=amplitude_c),
            abort_reason=abort_reason,
        )

    if subcommand == AUTOTUNE_CMD_START:
        if len(payload) < 2:
            raise AutotuneResponseError("START response is missing its ok byte")
        ok = payload[1]
        if ok:
            return subcommand, (True, "")
        err_len = payload[2] if len(payload) > 2 else 0
        error = ""
        if err_len:
            error = payload[3 : 3 + err_len].decode("ascii", errors="replace")
        return subcommand, (False, error)

    if subcommand in (AUTOTUNE_CMD_ABORT, AUTOTUNE_CMD_ACCEPT):
        # Used to reduce this to bool(payload[1]) and throw away everything
        # past it -- the same decode-then-discard bug CONTROL's
        # SET_ZONE_PID/SET_ZONE_MODEL and PROFILES' DELETE/PAUSE/RESUME/
        # ACK_LAST_RUN/STOP had (see their fixes above). ACCEPT's refusal
        # carries a real reason ("no completed autotune result to accept",
        # autotune_handle_message()'s ACCEPT case) that this was silently
        # dropping.
        return subcommand, _decode_ok_reason(payload, AutotuneResponseError, "ABORT/ACCEPT")

    raise AutotuneResponseError(f"unknown AUTOTUNE response subcommand 0x{subcommand:02X}")


