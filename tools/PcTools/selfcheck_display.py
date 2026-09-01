"""DISPLAY task checks.

Part of the selfcheck.py split (pure refactor) -- moved verbatim, no logic
changes. See selfcheck.py's module docstring for the overall map.
"""
from __future__ import annotations

import json
import math
import pathlib
import struct
import sys
import threading
import time

from kilnctrl import devices, pin_overlay
from kilnctrl.protocol import (
    FRAME_DELIM,
    FRAME_ESC,
    Device,
    Frame,
    FrameDecoder,
    FrameError,
    MsgType,
    crc16_ccitt_false,
    stuff,
    unstuff,
)
from kilnctrl.serial_link import list_ports, recommend_port

from selfcheck_common import check, _make_pair, _responder


def display_checks() -> None:
    """DISPLAY payload layouts, the READ_ID query, and the blit stream."""
    from kilnctrl.display import DisplayClient, DisplayQueryError, test_pattern_rgb565
    from kilnctrl.protocol import UART_PROTO_MAX_PAYLOAD, UART_TASK_ID_DISPLAY

    print("\n== DISPLAY payload layouts (uart_task_ids.h) ==")
    check("task id DISPLAY == 4", UART_TASK_ID_DISPLAY, 4)
    check("reset bytes", devices.display_reset(True), b"\x01\x01")
    check("set_power bytes", devices.display_set_power(False), b"\x02\x00")
    check("set_rotation bytes", devices.display_set_rotation(3), b"\x03\x03")
    check("set_invert bytes", devices.display_set_invert(True), b"\x04\x01")
    check("clear bytes (u16 LE color)", devices.display_clear(0xF800), b"\x05\x00\xf8")
    check(
        "fill_rect bytes (5 x u16 LE)",
        devices.display_fill_rect(1, 2, 3, 4, 0x07E0),
        b"\x06" + struct.pack("<HHHHH", 1, 2, 3, 4, 0x07E0),
    )
    check(
        "draw_rect bytes",
        devices.display_draw_rect(1, 2, 3, 4, 0x001F),
        b"\x07" + struct.pack("<HHHHH", 1, 2, 3, 4, 0x001F),
    )
    check(
        "draw_line bytes",
        devices.display_draw_line(0, 0, 479, 319, 0xFFFF),
        b"\x08" + struct.pack("<HHHHH", 0, 0, 479, 319, 0xFFFF),
    )
    check(
        "set_text_cursor bytes",
        devices.display_set_text_cursor(10, 20),
        b"\x09" + struct.pack("<HH", 10, 20),
    )
    check(
        "set_text_style bytes",
        devices.display_set_text_style(0xFFFF, 0x0000, 2, True),
        b"\x0a" + struct.pack("<HHBB", 0xFFFF, 0x0000, 2, 1),
    )
    check("print bytes", devices.display_print("Kiln"), b"\x0bKiln")
    check(
        "blit_begin bytes",
        devices.display_blit_begin(0, 0, 480, 320),
        b"\x0c" + struct.pack("<HHHH", 0, 0, 480, 320),
    )
    check("blit_data bytes", devices.display_blit_data(b"\x01\x02"), b"\x0d\x01\x02")
    check("blit_end bytes", devices.display_blit_end(), b"\x0e")
    check("read_id bytes", devices.display_read_id(), b"\x0f")

    check("rgb565 white", hex(devices.rgb565(255, 255, 255)), hex(0xFFFF))
    check("rgb565 red", hex(devices.rgb565(255, 0, 0)), hex(0xF800))
    check("rgb565 green", hex(devices.rgb565(0, 255, 0)), hex(0x07E0))
    check("rgb565 blue", hex(devices.rgb565(0, 0, 255)), hex(0x001F))
    check("rgb565 round trip (black)", devices.rgb565_to_rgb(0x0000), (0, 0, 0))
    check("rgb565 round trip (white)", devices.rgb565_to_rgb(0xFFFF), (255, 255, 255))

    for label, fn in (
        ("rotation 4 rejected", lambda: devices.display_set_rotation(4)),
        ("text size 9 rejected", lambda: devices.display_set_text_style(0, 0, 9)),
        ("odd blit chunk rejected", lambda: devices.display_blit_data(b"\x01")),
        ("empty blit chunk rejected", lambda: devices.display_blit_data(b"")),
        # limit is UART_PROTO_MAX_PAYLOAD - 1 = 252 bytes; use 254 (still even)
        # so this stays oversize regardless of future payload-size changes as
        # long as they don't also grow past 254.
        ("oversize blit chunk rejected", lambda: devices.display_blit_data(b"\x00" * 254)),
        ("zero-size blit window rejected", lambda: devices.display_blit_begin(0, 0, 0, 10)),
        # limit is UART_PROTO_MAX_PAYLOAD - 1 = 252 bytes.
        ("text over 252 bytes rejected", lambda: devices.display_print("x" * 253)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # Chunking: every frame must fit the payload limit and hold whole pixels,
    # because an odd byte count is a firmware-side error and a short frame
    # would silently shift every pixel after it.
    # UART_PROTO_MAX_PAYLOAD is 253 as of uart_task_ids.h version 3
    # (2026-08-11), so a blit chunk holds (253-1)//2 = 126 pixels, not the
    # old 128-payload figure of 63.
    check("blit chunk size is 126 pixels", devices.DISPLAY_BLIT_CHUNK_PIXELS, 126)
    pixels = bytes(range(256)) * 2  # 512 bytes = 256 pixels
    chunks = list(devices.iter_blit_chunks(pixels))
    check("chunk count for 256 pixels", len(chunks), 3)  # 126*2 + 4
    check("every chunk fits the payload limit", all(len(c) <= UART_PROTO_MAX_PAYLOAD for c in chunks), True)
    check("every chunk holds whole pixels", all((len(c) - 1) % 2 == 0 for c in chunks), True)
    check(
        "chunks reassemble to the original pixels",
        b"".join(c[1:] for c in chunks),
        pixels,
    )

    pattern = test_pattern_rgb565(16, 8)
    check("test pattern is 2 bytes per pixel", len(pattern), 16 * 8 * 2)
    check("test pattern needs no Pillow", isinstance(pattern, bytes), True)

    id_reply = bytes([0x0F, 1, 0x54, 0x80, 0x66]) + struct.pack("<HH", 480, 320)
    check("READ_ID reply is 9 bytes", len(id_reply), 9)
    subcommand, ident = devices.parse_display_response(id_reply)
    check("classify READ_ID reply", subcommand, 0x0F)
    check("panel id bytes", ident.id_bytes, b"\x54\x80\x66")
    check("panel geometry", (ident.width, ident.height), (480, 320))
    check("panel ok flag", ident.ok, True)

    for label, bad in (
        ("empty display reply rejected", b""),
        ("short READ_ID reply rejected", bytes([0x0F, 1, 0, 0])),
        ("bad ok flag rejected", bytes([0x0F, 2, 0, 0, 0, 0, 0, 0, 0])),
        ("unknown display subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_display_response(bad)
            check(label, False, True)
        except devices.DisplayResponseError:
            check(label, True, True)

    print("\n== DISPLAY query + blit stream over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_DISPLAY,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        return id_reply if payload[0] == 0x0F else None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_DISPLAY, answer, seen=writes)

    client = DisplayClient(host)
    try:
        ident = client.read_id(timeout=3.0)
        check("live read_id query", ident.id_bytes, b"\x54\x80\x66")

        progress: list[tuple[int, int]] = []
        frames = client.blit(0, 0, 8, 4, test_pattern_rgb565(8, 4), progress=lambda s, t: progress.append((s, t)))
        check("blit of 32 pixels is one data frame", frames, 1)
        check("progress reported once per chunk", len(progress), 1)
        deadline = time.time() + 2.0
        while len(writes) < 3 and time.time() < deadline:
            time.sleep(0.02)
        # BLIT_BEGIN, one BLIT_DATA, BLIT_END -- in that order, with nothing
        # else interleaved, which is what the firmware requires.
        check("blit sent BEGIN/DATA/END", [w[0] for w in writes[:3]], [0x0C, 0x0D, 0x0E])

        # A mismatched buffer must be caught here, not halfway through the
        # stream with a window already open on the panel.
        try:
            client.blit(0, 0, 10, 10, b"\x00\x00")
            check("wrong-size blit buffer rejected", False, True)
        except ValueError:
            check("wrong-size blit buffer rejected", True, True)

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read_id(timeout=0.3)
            check("undelivered query raises", False, True)
        except DisplayQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


