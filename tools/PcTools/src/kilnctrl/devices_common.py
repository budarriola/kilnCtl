"""Shared validation helpers and OkReason decode.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map.
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass


# ---------------------------------------------------------------------------
# Secret-field redaction -- 2026-09-21 fix: get_board_state() was passing
# UartWifiStatus (devices_wifi_uart.py) through dataclasses.asdict() straight
# into its JSON snapshot, which includes the board's own AP Wi-Fi password
# (ap_password) in plaintext. The UART GET_STATUS reply legitimately carries
# that value in the clear (uart_bridge_ext_wifi.c's GET_STATUS handler has no
# auth concept at all -- the UART link is physical-access-gated by design,
# same reasoning as the HTTP /status route's on_ap disclosure), but nothing
# downstream of that decode should ever repeat it into a tool's rendered
# output. This is the single choke point: any dict key that looks like a
# password/psk/passphrase field gets replaced with a "[set]"/"[unset]"
# boolean-shaped string instead of its value, recursively, so a future field
# with the same shape is covered without another audit pass.
# ---------------------------------------------------------------------------
_SECRET_KEY_RE = re.compile(r"(password|psk|passphrase)", re.IGNORECASE)


def redact_secret_fields(value):
    """Recursively replace any dict value whose key looks like a
    password/psk/passphrase field with "[set]" (truthy) or "[unset]"
    (falsy/empty), leaving every other field untouched. Safe to call on
    dicts, lists/tuples of them, or any other JSON-shaped value."""
    if isinstance(value, dict):
        redacted = {}
        for key, val in value.items():
            if isinstance(key, str) and _SECRET_KEY_RE.search(key):
                redacted[key] = "[set]" if val else "[unset]"
            else:
                redacted[key] = redact_secret_fields(val)
        return redacted
    if isinstance(value, (list, tuple)):
        return [redact_secret_fields(v) for v in value]
    return value


def _check_bool_byte(value: object) -> int:
    return 1 if bool(value) else 0


def _check_u8(value: int, name: str) -> int:
    value = int(value)
    if not 0 <= value <= 0xFF:
        raise ValueError(f"{name} must be 0..255, got {value}")
    return value


def _check_u16(value: int, name: str) -> int:
    value = int(value)
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"{name} must be a 16-bit value 0..0xFFFF, got {value}")
    return value


def _check_i8(value: int, name: str) -> int:
    value = int(value)
    if not -128 <= value <= 127:
        raise ValueError(f"{name} must be -128..127, got {value}")
    return value


def _check_range(value: int, low: int, high: int, name: str) -> int:
    value = int(value)
    if not low <= value <= high:
        raise ValueError(f"{name} must be {low}..{high}, got {value}")
    return value


def _check_finite(value: float, name: str) -> float:
    """Reject NaN/inf before they reach the wire.

    struct.pack("<f", float("inf")) succeeds and the firmware memcpy's it
    straight into a threshold register, so a stray inf becomes a silently
    dead trip point rather than an error anyone sees.
    """
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{name} must be a finite number, got {value}")
    return value


def _decoded_float(value: float, name: str, allow_nan: bool = False) -> float:
    """Validate a float that came *off* the wire.

    ``allow_nan`` is set only where the protocol defines NaN as meaningful
    (a thermocouple channel whose SPI read failed). An infinity is never
    meaningful in any of these fields: it is line noise that would otherwise
    propagate into a chart axis or a control loop as a real reading.
    """
    if math.isnan(value):
        if allow_nan:
            return value
        raise ValueError(f"{name} is NaN")
    if math.isinf(value):
        raise ValueError(f"{name} is infinite")
    return value


# ---------------------------------------------------------------------------
# Shared {subcmd, ok, [reason]} reply shape.
#
# ROADMAP.md "Future work -- KilnFW PC-link command acknowledgement": the
# transport ACK a DATA frame gets the instant it lands in a bridge task's
# inbox only ever proved *delivery*, never that the subcommand switch did
# anything. uart_bridge.c's bridge_reply_reject() and uart_bridge_ext.c's
# bx_reply_ok_err() both answer a *rejected* mutating command with the same
# wire shape: byte0 = subcmd echoed back, byte1 = ok (always 0 from
# bridge_reply_reject; either from bx_reply_ok_err), then, only when refused
# and a reason was given, a length-prefixed ASCII string. This is that shape,
# decoded once so every task-specific parser below shares one bug surface
# instead of reimplementing the length-prefix walk.
#
# The reason text is free-form per call site (uart_bridge.c uses a fixed set
# for IO's relay refusals -- see RelayRefusal below -- uart_bridge_ext.c uses
# whatever string the PROFILES/CONTROL handler already had, e.g. "name too
# long"). Callers that want to branch on *which* reason should classify the
# text themselves (RelayRefusal.from_wire() is the IO-specific example);
# this layer only guarantees the text survives the trip instead of being
# decoded and discarded, which is the bug this whole change exists to fix.
@dataclass(frozen=True)
class OkReason:
    """Decoded ``{subcmd, ok, [len, reason]}`` tail of a mutating-command reply."""

    ok: bool
    #: None on success, and on a rejection that carried no reason text (the
    #: plain 2-byte {subcmd, 0} shape every caller could already see before
    #: this change -- e.g. the unrecognized-subcommand default: case).
    reason: "str | None" = None

    def __bool__(self) -> bool:
        return self.ok

    def describe(self) -> str:
        if self.ok:
            return "ok"
        return f"refused: {self.reason}" if self.reason else "refused"


def _decode_ok_reason(payload: bytes, error_cls: type, who: str, offset: int = 1) -> OkReason:
    """Decode the ``ok, [len, reason]`` tail starting at ``offset`` (byte0 is
    always the echoed subcmd, already consumed by the caller). Raises
    ``error_cls(...)`` -- the task's own ``*ResponseError`` -- on a payload
    too short to hold even the ok byte."""
    if len(payload) <= offset:
        raise error_cls(f"{who} response is missing its ok byte")
    ok = bool(payload[offset])
    reason = None
    if not ok and len(payload) > offset + 1:
        reason_len = payload[offset + 1]
        start = offset + 2
        reason = payload[start : start + reason_len].decode("ascii", errors="replace")
    return OkReason(ok=ok, reason=reason)


