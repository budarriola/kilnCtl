"""DISPLAY wire encode/decode primitives.

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
# DISPLAY -- ILI9488 480x320 on J2 (task_id = UART_TASK_ID_DISPLAY)
#
# D/C and ~RESET hang off the SX1509, so every command/data transition costs
# an I2C transfer. The firmware batches one D/C toggle per command and sends
# that command's whole payload in a single SPI transaction; the corollary
# here is that full-screen work must go through FILL_RECT/BLIT, never
# repeated small writes.
# ---------------------------------------------------------------------------
#: PRINT's payload is [subcmd][text], so the text can't exceed the protocol's
#: max payload minus that one byte.
DISPLAY_MAX_TEXT_LEN = UART_PROTO_MAX_PAYLOAD - 1

#: Pixels per BLIT_DATA frame. 128-byte payload, 1 subcommand byte, 2 bytes
#: per RGB565 pixel, and an odd trailing byte is an error -- so 126 bytes of
#: pixel data, i.e. 63 pixels, is the largest legal chunk.
DISPLAY_BLIT_CHUNK_PIXELS = (UART_PROTO_MAX_PAYLOAD - 1) // 2


class DisplayResponseError(ValueError):
    """Raised when a DISPLAY response payload does not match its wire layout."""


def rgb565(r: int, g: int, b: int) -> int:
    """Pack 8-bit R/G/B into the RGB565 u16 the wire carries."""
    r = _check_u8(r, "r")
    g = _check_u8(g, "g")
    b = _check_u8(b, "b")
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_rgb(color: int) -> "tuple[int, int, int]":
    """Inverse of :func:`rgb565` (lossy -- low bits are replicated, not restored)."""
    color = _check_u16(color, "color")
    r = (color >> 11) & 0x1F
    g = (color >> 5) & 0x3F
    b = color & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _check_coord(value: int, name: str) -> int:
    """Coordinates and sizes are u16 on the wire; the firmware clips to the
    panel, so this only enforces the field width."""
    return _check_u16(value, name)


def display_reset(hard: bool = False) -> bytes:
    """0x01 RESET: 0 = software reset command, 1 = pulse ~RESET via the expander."""
    return struct.pack("<BB", DISPLAY_CMD_RESET, _check_bool_byte(hard))


def display_set_power(on: bool) -> bytes:
    """0x02 SET_POWER: 0 sends display-off + sleep-in."""
    return struct.pack("<BB", DISPLAY_CMD_SET_POWER, _check_bool_byte(on))


def display_set_rotation(rotation: int) -> bytes:
    """0x03 SET_ROTATION: MADCTL 0-3. 0/2 portrait 320x480, 1/3 landscape 480x320."""
    return struct.pack(
        "<BB", DISPLAY_CMD_SET_ROTATION, _check_range(rotation, 0, 3, "rotation")
    )


def display_set_invert(invert: bool) -> bytes:
    """0x04 SET_INVERT: invert 0/1."""
    return struct.pack("<BB", DISPLAY_CMD_SET_INVERT, _check_bool_byte(invert))


def display_clear(color: int = 0x0000) -> bytes:
    """0x05 CLEAR: fill the whole screen with an RGB565 color."""
    return struct.pack("<BH", DISPLAY_CMD_CLEAR, _check_u16(color, "color"))


def display_fill_rect(x: int, y: int, w: int, h: int, color: int) -> bytes:
    """0x06 FILL_RECT: x, y, w, h, color -- all u16 LE."""
    return struct.pack(
        "<BHHHHH",
        DISPLAY_CMD_FILL_RECT,
        _check_coord(x, "x"),
        _check_coord(y, "y"),
        _check_coord(w, "w"),
        _check_coord(h, "h"),
        _check_u16(color, "color"),
    )


def display_draw_rect(x: int, y: int, w: int, h: int, color: int) -> bytes:
    """0x07 DRAW_RECT: same args as FILL_RECT, 1px outline."""
    return struct.pack(
        "<BHHHHH",
        DISPLAY_CMD_DRAW_RECT,
        _check_coord(x, "x"),
        _check_coord(y, "y"),
        _check_coord(w, "w"),
        _check_coord(h, "h"),
        _check_u16(color, "color"),
    )


def display_draw_line(x0: int, y0: int, x1: int, y1: int, color: int) -> bytes:
    """0x08 DRAW_LINE: x0, y0, x1, y1, color -- all u16 LE."""
    return struct.pack(
        "<BHHHHH",
        DISPLAY_CMD_DRAW_LINE,
        _check_coord(x0, "x0"),
        _check_coord(y0, "y0"),
        _check_coord(x1, "x1"),
        _check_coord(y1, "y1"),
        _check_u16(color, "color"),
    )


def display_set_text_cursor(x: int, y: int) -> bytes:
    """0x09 SET_TEXT_CURSOR: pixel coordinates of the glyph's top-left."""
    return struct.pack(
        "<BHH", DISPLAY_CMD_SET_TEXT_CURSOR, _check_coord(x, "x"), _check_coord(y, "y")
    )


def display_set_text_style(
    fg: int, bg: int = 0x0000, size: int = 1, opaque_background: bool = True
) -> bytes:
    """0x0A SET_TEXT_STYLE: fg, bg (u16 LE), size 1-8, opaque background 0/1."""
    return struct.pack(
        "<BHHBB",
        DISPLAY_CMD_SET_TEXT_STYLE,
        _check_u16(fg, "fg"),
        _check_u16(bg, "bg"),
        _check_range(size, 1, 8, "size"),
        _check_bool_byte(opaque_background),
    )


def display_print(text: str) -> bytes:
    """0x0B PRINT: ASCII at the cursor, which advances and wraps at the edge.

    Non-ASCII becomes ``?`` rather than raising -- the panel's font has no
    glyph for it either way -- but a non-string is a caller mistake and is
    named as one instead of surfacing as an AttributeError.
    """
    if not isinstance(text, str):
        raise ValueError(f"text must be a string, got {type(text).__name__}")
    encoded = text.encode("ascii", errors="replace")
    if len(encoded) > DISPLAY_MAX_TEXT_LEN:
        raise ValueError(
            f"text too long: {len(encoded)} bytes > {DISPLAY_MAX_TEXT_LEN}-byte limit"
        )
    return struct.pack("<B", DISPLAY_CMD_PRINT) + encoded


def display_blit_begin(x: int, y: int, w: int, h: int) -> bytes:
    """0x0C BLIT_BEGIN: open a pixel window; BLIT_DATA then streams into it."""
    if int(w) <= 0 or int(h) <= 0:
        raise ValueError(f"blit window must be non-empty, got {w}x{h}")
    return struct.pack(
        "<BHHHH",
        DISPLAY_CMD_BLIT_BEGIN,
        _check_coord(x, "x"),
        _check_coord(y, "y"),
        _check_coord(w, "w"),
        _check_coord(h, "h"),
    )


def display_blit_data(pixels: bytes) -> bytes:
    """0x0D BLIT_DATA: RGB565 pixels, u16 LE, row-major, continuing the window.

    An odd number of bytes is an error on the firmware side (it would split a
    pixel across two frames), so it is rejected here rather than sent.
    """
    pixels = bytes(pixels)
    if not pixels:
        raise ValueError("blit chunk is empty")
    if len(pixels) % 2:
        raise ValueError(f"blit chunk must be a whole number of pixels, got {len(pixels)} bytes")
    if len(pixels) > UART_PROTO_MAX_PAYLOAD - 1:
        raise ValueError(
            f"blit chunk too long: {len(pixels)} bytes > "
            f"{UART_PROTO_MAX_PAYLOAD - 1} (use iter_blit_chunks)"
        )
    return struct.pack("<B", DISPLAY_CMD_BLIT_DATA) + pixels


def display_blit_end() -> bytes:
    """0x0E BLIT_END: close the window.

    Sending anything other than BLIT_DATA/BLIT_END while a blit is open is an
    error and aborts the blit, so callers must not interleave other display
    commands mid-stream.
    """
    return struct.pack("<B", DISPLAY_CMD_BLIT_END)


def display_read_id() -> bytes:
    """0x0F READ_ID request (query): no args."""
    return struct.pack("<B", DISPLAY_CMD_READ_ID)


def iter_blit_chunks(pixels: bytes, chunk_pixels: int = DISPLAY_BLIT_CHUNK_PIXELS):
    """Yield BLIT_DATA payloads covering ``pixels`` (RGB565 u16 LE, row-major).

    Splitting is on pixel boundaries by construction: an odd byte count in any
    frame is a protocol error, and the receiving window just continues where
    the previous chunk stopped, so the split points themselves carry no
    meaning to the firmware.
    """
    pixels = bytes(pixels)
    if len(pixels) % 2:
        raise ValueError(f"pixel buffer must be even-length, got {len(pixels)} bytes")
    chunk_bytes = _check_range(
        chunk_pixels, 1, DISPLAY_BLIT_CHUNK_PIXELS, "chunk_pixels"
    ) * 2
    for offset in range(0, len(pixels), chunk_bytes):
        yield display_blit_data(pixels[offset : offset + chunk_bytes])


@dataclass(frozen=True)
class DisplayId:
    """Decoded READ_ID reply: the panel's RDDID bytes plus its live geometry."""

    ok: bool
    id_bytes: bytes
    width: int
    height: int

    def describe(self) -> str:
        ident = " ".join(f"{b:02X}" for b in self.id_bytes)
        status = "ok" if self.ok else "FAILED (no answer, or all-zero ID)"
        return f"ID {ident} [{status}], {self.width}x{self.height} as rotated"


#: Every DISPLAY write subcommand (RESET..BLIT_END) is fire-and-forget on
#: success -- display_bridge_task() sends nothing back. The only reply that
#: can ever arrive under one of these ids is the bottom-of-task
#: ``bridge_reply_reject(..., "driver error")`` when the driver call itself
#: failed (see uart_bridge.c). READ_ID is excluded: it always replies, with
#: its own 9-byte layout below, and never falls through to that generic
#: reject (its case sets ``err = ESP_OK`` unconditionally).
_DISPLAY_WRITE_SUBCOMMANDS = frozenset(
    {
        DISPLAY_CMD_RESET,
        DISPLAY_CMD_SET_POWER,
        DISPLAY_CMD_SET_ROTATION,
        DISPLAY_CMD_SET_INVERT,
        DISPLAY_CMD_CLEAR,
        DISPLAY_CMD_FILL_RECT,
        DISPLAY_CMD_DRAW_RECT,
        DISPLAY_CMD_DRAW_LINE,
        DISPLAY_CMD_SET_TEXT_CURSOR,
        DISPLAY_CMD_SET_TEXT_STYLE,
        DISPLAY_CMD_PRINT,
        DISPLAY_CMD_BLIT_BEGIN,
        DISPLAY_CMD_BLIT_DATA,
        DISPLAY_CMD_BLIT_END,
    }
)


def parse_display_response(payload: bytes) -> "tuple[int, object]":
    """Decode a DISPLAY reply into ``(subcommand, value)``.

    Two shapes share this one task:

    - READ_ID (the only query): byte0=0x0F, byte1=ok(0/1), bytes2..4 = RDDID
      bytes, bytes5..6 = width u16 LE, bytes7..8 = height u16 LE. Always
      replies, so a caller waiting on it can always tell "answered" from
      "still waiting" -- see the length check below.
    - Every other (write) subcommand: no reply on success. A reply under one
      of those ids is always a refusal -- ``{subcmd, ok=0, [len, reason]}``,
      decoded via :func:`_decode_ok_reason` into an :class:`OkReason` so the
      caller learns *why*, the same shape THERMO/SAFETY/IO/PROFILES/CONTROL
      already use. Before this, any of these ids raised "unknown DISPLAY
      response subcommand" and the refusal was dropped in
      ``DisplayClient._handle_reply`` -- invisible to whoever sent the write.
    """
    if len(payload) < 1:
        raise DisplayResponseError("DISPLAY response is empty")
    subcommand = payload[0]
    if subcommand in _DISPLAY_WRITE_SUBCOMMANDS:
        return subcommand, _decode_ok_reason(payload, DisplayResponseError, "DISPLAY write")
    if subcommand != DISPLAY_CMD_READ_ID:
        raise DisplayResponseError(
            f"unknown DISPLAY response subcommand 0x{subcommand:02X}"
        )
    # Strict equality, not a minimum -- deliberately, since
    # display_bridge_task() can now also send a driver-error refusal under
    # this SAME id ({subcmd, ok=0, len, "driver error"}, 16 bytes). 16 != 9
    # today so that refusal correctly falls into the branch below instead of
    # being misread as a real (if malformed) READ_ID reply; keep this an
    # exact check if the refusal reason text ever changes length, or the two
    # could collide -- see parse_touch_response()'s GET_STATE check for a
    # case where a loose `>=` check on this exact class of collision was a
    # real bug.
    if len(payload) != 9:
        raise DisplayResponseError(
            f"READ_ID response must be 9 bytes, got {len(payload)}"
        )
    ok = payload[1]
    if ok > 1:
        raise DisplayResponseError(f"READ_ID ok flag must be 0 or 1, got {ok}")
    width, height = struct.unpack_from("<HH", payload, 5)
    # A panel that answered (ok=1) must have reported a real geometry; 0x0
    # from a successful read means the payload is not what it claims to be.
    if ok and (width == 0 or height == 0):
        raise DisplayResponseError(
            f"READ_ID reports ok but a {width}x{height} geometry"
        )
    return subcommand, DisplayId(
        ok=bool(ok), id_bytes=bytes(payload[2:5]), width=width, height=height
    )


