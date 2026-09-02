"""SYSTEM wire encode/decode primitives.

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
# SYSTEM (task_id = UART_TASK_ID_SYSTEM)
# ---------------------------------------------------------------------------
def system_restart_uart() -> bytes:
    """0x01 RESTART_UART request: byte0 = subcommand, no args."""
    return struct.pack("<B", SYSTEM_CMD_RESTART_UART)


def system_factory_reset(scope: int = FACTORY_RESET_SCOPE_ALL) -> bytes:
    """0x02 FACTORY_RESET: byte1=scope (0=wifi 1=kiln 2=profiles 3=all).

    Mirrors POST /api/factory_reset exactly: same per-partition NVS erase,
    same unconditional reboot ~500ms later. No reply frame either way -- the
    ACK is the only delivery confirmation, and the reboot itself (a fresh
    unsolicited GET_FW_VERSION push) is the real evidence the erase happened.
    An out-of-range scope byte is rejected by the firmware with no erase and
    no reboot, so validate it here too rather than letting a typo silently
    no-op on the device.
    """
    return struct.pack(
        "<BB", SYSTEM_CMD_FACTORY_RESET, _check_range(scope, 0, 3, "scope")
    )


def system_get_watchdog_panic_disabled() -> bytes:
    """0x03 GET_WATCHDOG_PANIC_DISABLED request: byte0 = subcommand, no args.

    Query -- like INFO, the request is ACKed for delivery only and the answer
    arrives as a separate DATA frame; see :func:`parse_system_response`.
    """
    return struct.pack("<B", SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED)


def system_set_watchdog_panic_disabled(disabled: bool) -> bytes:
    """0x04 SET_WATCHDOG_PANIC_DISABLED: byte1 = disabled(0/1).

    Persists AND applies immediately, no reboot needed either direction. No
    reply frame -- the ACK is the only confirmation; poll
    :func:`system_get_watchdog_panic_disabled` afterward to read back the
    applied value.
    """
    return struct.pack("<BB", SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED, 1 if disabled else 0)


def system_get_telemetry_enabled() -> bytes:
    """0x06 GET_TELEMETRY_ENABLED request: byte0 = subcommand, no args.

    Query -- like GET_WATCHDOG_PANIC_DISABLED, ACKed for delivery only, with
    the answer arriving as a separate DATA frame; see
    :func:`parse_system_response`.
    """
    return struct.pack("<B", SYSTEM_CMD_GET_TELEMETRY_ENABLED)


def system_set_telemetry_enabled(enabled: bool) -> bytes:
    """0x05 SET_TELEMETRY_ENABLED: byte1 = enabled(0/1).

    Turns the live debug-UART firing/autotune temperature telemetry feed
    (telemetry_log.c's ESP_LOGI lines, task :data:`UART_TASK_ID_LOG`) on or
    off. Applies within one telemetry-task tick (<=1s), no reboot. No reply
    frame -- the ACK is the only confirmation; poll
    :func:`system_get_telemetry_enabled` afterward to read back the applied
    value.
    """
    return struct.pack("<BB", SYSTEM_CMD_SET_TELEMETRY_ENABLED, 1 if enabled else 0)


class SystemResponseError(ValueError):
    """Raised when a SYSTEM response payload does not match its wire layout."""


#: Subcommands SYSTEM answers with a reply DATA frame (byte0 = echoed
#: subcommand, byte1 = a 0/1 flag). Every entry here is a *query* -- adding a
#: new one only requires listing it, not touching the parser below.
_SYSTEM_QUERY_SUBCOMMANDS = frozenset(
    {SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED, SYSTEM_CMD_GET_TELEMETRY_ENABLED}
)


def parse_system_response(payload: bytes) -> "tuple[int, bool]":
    """Decode a SYSTEM query response payload.

    Unlike INFO's replies, SYSTEM replies are self-describing: byte0 echoes
    the subcommand. Every query subcommand (see
    :data:`_SYSTEM_QUERY_SUBCOMMANDS`) shares the same 2-byte layout: byte0 =
    the echoed subcommand, byte1 = a 0/1 flag.

    Raises :class:`SystemResponseError` if the payload does not match this
    layout, including a byte0 that doesn't echo a known query subcommand.
    """
    if len(payload) != 2:
        raise SystemResponseError(
            f"SYSTEM response length mismatch: got {len(payload)} bytes, want 2"
        )
    subcommand = payload[0]
    if subcommand not in _SYSTEM_QUERY_SUBCOMMANDS:
        raise SystemResponseError(
            f"SYSTEM response byte0=0x{subcommand:02X} does not echo a known SYSTEM query subcommand "
            f"({sorted(f'0x{c:02X}' for c in _SYSTEM_QUERY_SUBCOMMANDS)})"
        )
    flag = payload[1]
    if flag > 1:
        raise SystemResponseError(f"SYSTEM response flag byte must be 0 or 1, got {flag}")
    return subcommand, bool(flag)


