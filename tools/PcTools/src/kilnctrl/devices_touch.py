"""TOUCH wire encode/decode primitives.

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
# TOUCH -- NS2009 touch controller on the same J2 panel as DISPLAY
# (task_id = UART_TASK_ID_TOUCH)
#
# INJECT is the piece that lets this side "send touches as if from the
# screen": the firmware's screen_idle state machine treats an injected press
# exactly like a real NS2009 one -- it resets the auto-blank idle timer and
# wakes the panel if it is currently blanked. x/y are SCREEN PIXEL
# coordinates that are also fed straight into LVGL's input device
# (lvgl_port_inject_touch(), downstream of the NS2009 calibration transform)
# so an injected press really does hit-test buttons/containers/pages exactly
# like a finger would -- see mcp_server.py's touch_inject() docstring for the
# full press/release/drag contract.
# ---------------------------------------------------------------------------


class TouchResponseError(ValueError):
    """Raised when a TOUCH response payload does not match its wire layout."""


def touch_get_state() -> bytes:
    """0x01 GET_STATE request (query): no args."""
    return struct.pack("<B", TOUCH_CMD_GET_STATE)


def touch_inject(x: int, y: int, pressed: bool) -> bytes:
    """0x02 INJECT: fire-and-forget synthetic touch, no reply.

    Feeds the firmware's idle timer and wake logic exactly as a real press
    would. ``pressed=False`` (a release) is accepted but is a no-op on the
    firmware side -- there is no "held" state to end.
    """
    return struct.pack(
        "<BHHB", TOUCH_CMD_INJECT, _check_coord(x, "x"), _check_coord(y, "y"), _check_bool_byte(pressed)
    )


def touch_set_tap_dump(enable: bool) -> bytes:
    """0x03 SET_TAP_DUMP: fire-and-forget, no reply.

    Turns the AUTOMATIC tap-target dump inside the firmware's kiln_ui_show()
    on or off (off by default) -- see kiln_ui_set_auto_tap_dump()'s comment.
    Does not affect touch_log_tap_targets() below, which always dumps.
    """
    return struct.pack("<BB", TOUCH_CMD_SET_TAP_DUMP, _check_bool_byte(enable))


def touch_log_tap_targets() -> bytes:
    """0x04 LOG_TAP_TARGETS: fire-and-forget, no reply.

    Requests an immediate tap-target dump (widget rectangles, centres, and
    labels, including lv_layer_top()/lv_layer_sys() overlays and any open
    lv_keyboard's individual keys) for whatever screen is currently loaded.
    The dump itself arrives as ESP_LOGI lines over the device log, not as a
    reply here -- there is no framebuffer readback on this panel, so this is
    the only way to discover where a widget actually is on screen.
    """
    return struct.pack("<B", TOUCH_CMD_LOG_TAP_TARGETS)


@dataclass(frozen=True)
class TouchState:
    """Decoded GET_STATE reply.

    The diagnostic fields (``input_enabled`` through ``show_exits``) were
    appended 2026-08-21 to give this reply pull-based evidence for the
    on-bench "all touch is dead" investigation -- see uart_bridge.c's
    TOUCH_CMD_GET_STATE case for the wire layout and why appending rather
    than reordering/resizing matters. They are ``None`` when talking to
    older firmware that only ever sent the original 6-byte reply.
    """

    screen_on: bool
    idle_ms: int
    input_enabled: "bool | None" = None
    touch_read_cb_count: "int | None" = None
    injected_delivered_count: "int | None" = None
    show_entries: "int | None" = None
    show_exits: "int | None" = None

    def describe(self) -> str:
        state = "on" if self.screen_on else "blanked"
        base = f"screen {state}, idle {self.idle_ms} ms"
        if self.input_enabled is None:
            return base + " (older firmware: no diag fields)"
        return (
            base
            + f", input_enabled={self.input_enabled}"
            + f", touch_read_cb={self.touch_read_cb_count}"
            + f", injected_delivered={self.injected_delivered_count}"
            + f", show_entries={self.show_entries}"
            + f", show_exits={self.show_exits}"
        )


#: The three fire-and-forget writes on this task (INJECT, SET_TAP_DUMP,
#: LOG_TAP_TARGETS) reply only on the bottom-of-task driver-error refusal
#: (``bridge_reply_reject(..., "driver error")``) -- success is silent. A
#: reply under one of these ids is always that refusal, decoded the same way
#: as DISPLAY's write subcommands below.
_TOUCH_WRITE_SUBCOMMANDS = frozenset(
    {TOUCH_CMD_INJECT, TOUCH_CMD_SET_TAP_DUMP, TOUCH_CMD_LOG_TAP_TARGETS}
)


def parse_touch_response(payload: bytes) -> "tuple[int, object]":
    """Decode a TOUCH reply into ``(subcommand, value)``.

    Layout::

        GET_STATE (original 6 bytes):
            byte0=0x01, byte1=screen_on(0/1), bytes2..5 = idle_ms u32 LE
        GET_STATE (appended diagnostic fields, byte 6 onward -- optional,
        present only from firmware built 2026-08-21 or later):
            byte6 = input_enabled (0/1)
            bytes7..10  = touch_read_cb_count u32 LE
            bytes11..14 = injected_delivered_count u32 LE
            bytes15..18 = kiln_ui_show entries u32 LE
            bytes19..22 = kiln_ui_show completed exits u32 LE
        GET_STATE driver-error refusal ({subcmd=0x01, ok=0, len, reason},
        16 bytes with today's "driver error" text): returned as an
        :class:`OkReason` instead of a :class:`TouchState` -- see the length
        check below.
        INJECT / SET_TAP_DUMP / LOG_TAP_TARGETS: no reply on success; a
        reply under one of these ids is always a ``{subcmd, ok=0, [len,
        reason]}`` driver-error refusal, decoded into an :class:`OkReason`.
        Before this, any of these ids raised "unknown TOUCH response
        subcommand" and the refusal was dropped in
        ``TouchClient._handle_reply``.

    A reply shorter than the full 23 bytes but at least 6 is accepted (older
    firmware, or a firmware built before some later field was added) -- only
    the fields actually present are populated, the rest come back as None.
    (Two more fields, indev_exists and timer_handler_calls, briefly lived at
    bytes 23..27 as a one-off root-cause probe; removed 2026-08-21 once the
    bug they were probing was fixed -- this decoder never read them.)
    """
    if len(payload) < 1:
        raise TouchResponseError("TOUCH response is empty")
    subcommand = payload[0]
    if subcommand in _TOUCH_WRITE_SUBCOMMANDS:
        return subcommand, _decode_ok_reason(payload, TouchResponseError, "TOUCH write")
    if subcommand != TOUCH_CMD_GET_STATE:
        raise TouchResponseError(f"unknown TOUCH response subcommand 0x{subcommand:02X}")
    # Exactly 6 (original fields only) or 23 (+ the 2026-08-21 diagnostic
    # fields) -- uart_bridge.c's TOUCH_CMD_GET_STATE case only ever emits one
    # of those two lengths on success, never anything in between (the old
    # `< 6` check here nominally tolerated any longer length "for a
    # hypothetical in-between firmware build", but no such build ever
    # existed). Tightened to this exact set 2026-08-24, the same day
    # touch_bridge_task() started sending a driver-error refusal ({subcmd,
    # ok=0, len, "driver error"} -- 16 bytes) on a failed
    # screen_idle_get_state(): a loose `>= 6` check would decode that refusal
    # as if it were a real reply (byte1=ok=0 reads as a valid
    # screen_on=False, the reason bytes read as garbage idle_ms) --
    # confidently WRONG data with no exception, worse than the silent drop
    # this refusal reply exists to replace. See CommonFW/docs/LINK_PROTOCOL.md's
    # "Request/reply ids must never be shared" rule -- this is the same
    # length-ambiguity failure mode on a task that has no second id to split
    # onto. Any other length -- 16 bytes with today's "driver error" text,
    # or anything else a future reason string produces -- is decoded as the
    # refusal it is, rather than just excluded, so the caller learns why
    # instead of getting a generic malformed-response drop.
    if len(payload) not in (6, 23):
        return subcommand, _decode_ok_reason(payload, TouchResponseError, "GET_STATE")
    screen_on = payload[1]
    if screen_on > 1:
        raise TouchResponseError(f"GET_STATE screen_on flag must be 0 or 1, got {screen_on}")
    (idle_ms,) = struct.unpack_from("<I", payload, 2)

    input_enabled = None
    touch_read_cb_count = None
    injected_delivered_count = None
    show_entries = None
    show_exits = None
    if len(payload) >= 23:
        input_enabled = bool(payload[6])
        (touch_read_cb_count,) = struct.unpack_from("<I", payload, 7)
        (injected_delivered_count,) = struct.unpack_from("<I", payload, 11)
        (show_entries,) = struct.unpack_from("<I", payload, 15)
        (show_exits,) = struct.unpack_from("<I", payload, 19)

    return subcommand, TouchState(
        screen_on=bool(screen_on),
        idle_ms=idle_ms,
        input_enabled=input_enabled,
        touch_read_cb_count=touch_read_cb_count,
        injected_delivered_count=injected_delivered_count,
        show_entries=show_entries,
        show_exits=show_exits,
    )


