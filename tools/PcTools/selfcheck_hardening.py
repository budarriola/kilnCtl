"""Hardening / malformed-input checks.

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
from selfcheck_info import _FW_VERSION_REPLY


def _expect_raises(label: str, exc_type, fn) -> None:
    """Assert ``fn()`` raises ``exc_type`` -- and specifically that, not just
    "something went wrong": an IndexError or a struct.error escaping a parser
    is exactly the failure these checks exist to catch."""
    try:
        fn()
    except exc_type:
        check(label, True, True)
    except BaseException as exc:  # noqa: BLE001 - the wrong exception is a failure
        check(f"{label} [raised {type(exc).__name__}: {exc}]", False, True)
    else:
        check(f"{label} [did not raise]", False, True)


def hardening_checks() -> None:
    """Negative paths: malformed bytes off the wire, absurd arguments, and the
    link failing underneath a caller.

    Everything here is something a *non*-firmware peer can do -- line noise, a
    half-flashed board, a v1 fixture, a process that isn't pc_tools bound to
    the hub port -- so each case must produce a specific, catchable exception
    or a clean error result, never an IndexError/struct.error escaping into
    the GUI event loop or an MCP tool response.
    """
    from kilnctrl.display import (
        MAX_BLIT_DIMENSION,
        DisplayClient,
        DisplayQueryError,
        image_to_rgb565,
        test_pattern_rgb565,
    )
    from kilnctrl.protocol import (
        HEADER_LEN,
        UART_PROTO_MAX_PAYLOAD,
        UART_TASK_ID_DISPLAY,
    )
    from kilnctrl.serial_link import SendResult

    print("\n== hardening: malformed frames ==")
    # A LENGTH byte larger than the protocol's maximum payload must be a
    # FrameError. Frame's own constructor raises a plain ValueError for the
    # same thing, and the RX loop does not catch that -- it would kill the
    # reader thread rather than dropping one frame.
    over = bytearray([int(MsgType.DATA), 0, 1, 0, 1, 1, 1, UART_PROTO_MAX_PAYLOAD + 1])
    over += bytes(UART_PROTO_MAX_PAYLOAD + 1)
    over += struct.pack(">H", crc16_ccitt_false(bytes(over)))
    _expect_raises("over-long LENGTH rejected as FrameError", FrameError, lambda: Frame.from_raw(bytes(over)))
    _expect_raises(
        "header-only frame rejected",
        FrameError,
        lambda: Frame.from_raw(bytes(HEADER_LEN)),
    )
    _expect_raises(
        "unknown msg type rejected",
        FrameError,
        lambda: Frame.from_raw(bytes([0x55, 0, 1, 0, 1, 1, 1, 0, 0, 0])),
    )
    # Pure noise must never yield a frame, and must never grow the decoder's
    # buffer without bound. NOTE: bytes(range(256)) is NOT valid noise for
    # this -- it contains FRAME_DELIM (0x7E) and FRAME_ESC (0x7D) exactly
    # once each per 256-byte cycle, so the decoder correctly treats those as
    # real frame boundaries and returns the (garbage) bytes between them.
    # That is FrameDecoder doing its job, not a bug -- CRC/structure
    # validation happens one layer up, in Frame.from_raw(). Excluding those
    # two byte values keeps this a real no-delimiter-ever-seen test.
    dec = FrameDecoder()
    noise = bytes(b for b in range(256) if b not in (FRAME_DELIM, FRAME_ESC)) * 8
    got = dec.feed(noise)
    check("noise stream yields no frames", got, [])
    check("decoder buffer stays bounded", len(dec._buf) <= 138, True)

    print("\n== hardening: THERMO reply parsing ==")
    for label, bad in (
        ("READ count 4 (more than the board has) rejected", bytes([0x05, 4]) + bytes(4 * 12)),
        (
            "READ entry with channel 7 rejected",
            bytes([0x05, 1]) + struct.pack("<BffBB", 7, 25.0, 24.0, 0, 0) + b"\x00",
        ),
        (
            "READ entry with infinite temperature rejected",
            bytes([0x05, 1]) + struct.pack("<BffBB", 0, float("inf"), 24.0, 0, 0) + b"\x00",
        ),
        (
            "READ entry with infinite cold junction rejected",
            bytes([0x05, 1]) + struct.pack("<BffBB", 0, 25.0, float("-inf"), 0, 0) + b"\x00",
        ),
        ("READ truncated mid-entry rejected", bytes([0x05, 1]) + bytes(6)),
        ("READ_FAULTS count 9 rejected", bytes([0x06, 9]) + bytes(27)),
        ("READ_FAULTS channel 5 rejected", bytes([0x06, 1, 5, 0x01, 0xFF])),
        ("READ_REG channel 9 rejected", bytes([0x09, 9, 0x02, 1, 0xAA])),
        ("READ_REG len 17 rejected", bytes([0x09, 0, 0x02, 17]) + bytes(17)),
        ("READ_REG header truncated rejected", bytes([0x09, 0, 0x02])),
        ("subcommand-only READ payload rejected", bytes([0x05])),
    ):
        _expect_raises(label, devices.ThermoResponseError, lambda b=bad: devices.parse_thermo_response(b))

    # READ_REG len=0 is NOT a malformed reply as of uart_bridge.c's
    # THERMO_CMD_READ_REG case (2026-08-20): the firmware now replies with a
    # 0-length body instead of dropping the reply when the channel's SPI read
    # failed. parse_thermo_response() must accept it structurally; only
    # ThermoClient.read_reg() -- which knows the PC never legitimately asks
    # for 0 bytes -- turns it into an error for the caller.
    zero_len_reg = bytes([0x09, 0, 0x02, 0])
    subcommand, zero_regs = devices.parse_thermo_response(zero_len_reg)
    check("READ_REG len 0 accepted as a failure marker", subcommand, 0x09)
    check("READ_REG len 0 carries no data bytes", zero_regs.data, b"")

    # NaN stays legal: it is how the firmware reports a channel whose SPI read
    # failed, and dropping it would hide a real fault.
    nan_entry = bytes([0x05, 1]) + struct.pack(
        "<BffBB", 2, float("nan"), float("nan"), 0, 0x02
    ) + b"\x00"
    _sub, nan_readings = devices.parse_thermo_response(nan_entry)
    check("NaN temperature still accepted (SPI-failed channel)", math.isnan(nan_readings[0].temperature_c), True)
    check("SPI-failed channel reads as invalid", nan_readings[0].valid, False)

    print("\n== hardening: IO / DISPLAY / SAFETY reply parsing ==")
    for label, bad in (
        ("SX_READ_REG len 17 rejected", bytes([0x11, 0x10, 17]) + bytes(17)),
        ("SX_READ_REG len 0 rejected", bytes([0x11, 0x10, 0])),
        ("SX_SCAN with a non-7-bit address rejected", bytes([0x19, 1, 0x80])),
        ("READ 8 bytes (one short) rejected", bytes([0x05]) + bytes(7)),
        ("READ 10 bytes (one long) rejected", bytes([0x05]) + bytes(9)),
    ):
        _expect_raises(label, devices.IoResponseError, lambda b=bad: devices.parse_io_response(b))

    _expect_raises(
        "READ_ID claiming ok with 0x0 geometry rejected",
        devices.DisplayResponseError,
        lambda: devices.parse_display_response(
            bytes([0x0F, 1, 0x54, 0x80, 0x66]) + struct.pack("<HH", 0, 0)
        ),
    )
    _expect_raises(
        "READ_ID truncated mid-geometry rejected",
        devices.DisplayResponseError,
        lambda: devices.parse_display_response(bytes([0x0F, 1, 0, 0, 0, 0, 0, 0])),
    )

    for label, bad in (
        (
            "GET_STATUS with an infinite current rejected",
            bytes([0x01, 0x01])
            + struct.pack("<ffBfffH", 20.0, 20.0, 0, float("inf"), 0.0, 0.0, 10),
        ),
        (
            "GET_STATUS with an infinite temperature rejected",
            bytes([0x01, 0x01])
            + struct.pack("<ffBfffH", float("inf"), 20.0, 0, 0.0, 0.0, 0.0, 10),
        ),
        ("GET_LINK_STATS one byte short rejected", bytes([0x04]) + bytes(17)),
    ):
        _expect_raises(label, devices.SafetyResponseError, lambda b=bad: devices.parse_safety_response(b))

    print("\n== hardening: INFO reply parsing ==")
    # Non-ASCII in a text field is replaced, not raised on: a mangled commit
    # string is worth showing, unlike a mangled numeric field.
    weird = bytes([2, 0, 0, 3]) + b"\xff\xfe\xfd" + bytes([1]) + b"\x80"
    version = devices.parse_fw_version_response(weird)
    # Compared as a length/codepoint rather than printed: this selfcheck's
    # own console is cp1252 on Windows and cannot render U+FFFD.
    check(
        "non-ASCII commit is replaced, not fatal",
        (len(version.commit), set(version.commit) == {"�"}),
        (3, True),
    )
    for label, bad in (
        ("fw version 1 byte (can't even read the version) rejected", bytes([2])),
        ("fw version with trailing junk rejected", _FW_VERSION_REPLY + b"\x00"),
        ("fw version datetime_len overrun rejected", bytes([2, 0, 0, 1, 65, 40]) + b"x"),
        ("fw version dirty flag 2 rejected", bytes([2, 0, 2, 0, 0])),
    ):
        _expect_raises(
            label,
            devices.InfoResponseError,
            lambda b=bad: devices.parse_fw_version_response(b),
        )
    _expect_raises(
        "pin config claiming 200 entries rejected",
        devices.InfoResponseError,
        lambda: devices.parse_pin_config_response(bytes([200, 1, 1])),
    )
    # A v1 device's version reply must still parse far enough to *report* the
    # mismatch -- the version field is at a fixed offset precisely so this
    # works across an incompatible peer.
    v1 = devices.parse_fw_version_response(bytes([1, 0]) + _FW_VERSION_REPLY[2:])
    check("v1 peer's version is still readable", v1.protocol_version, 1)
    check("v1 peer is refused", v1.compatible, False)

    print("\n== hardening: argument validation at the API surface ==")
    for label, fn in (
        ("NaN threshold rejected", lambda: devices.thermo_set_thresholds(0, float("nan"), 0, 0, 0)),
        ("infinite threshold rejected", lambda: devices.thermo_set_thresholds(0, float("inf"), 0, 0, 0)),
        ("NaN cj offset rejected", lambda: devices.thermo_set_cj_offset(0, float("nan"))),
        ("negative fill_rect width rejected", lambda: devices.display_fill_rect(0, 0, -5, 10, 0)),
        ("negative coordinate rejected", lambda: devices.display_draw_line(-1, 0, 10, 10, 0)),
        ("color above 0xFFFF rejected", lambda: devices.display_clear(0x1FFFF)),
        ("non-string print text rejected", lambda: devices.display_print(None)),
        ("blit window 0 wide rejected", lambda: devices.display_blit_begin(0, 0, 0, 10)),
        ("absurd test-pattern size rejected", lambda: test_pattern_rgb565(60000, 60000)),
        (
            f"test pattern over {MAX_BLIT_DIMENSION}px per side rejected",
            lambda: test_pattern_rgb565(MAX_BLIT_DIMENSION + 1, 1),
        ),
        ("negative test-pattern size rejected", lambda: test_pattern_rgb565(-1, 10)),
        ("image target size 0 rejected", lambda: image_to_rgb565(__file__, 0, 10)),
        ("absurd image target size rejected", lambda: image_to_rgb565(__file__, 60000, 60000)),
    ):
        _expect_raises(label, ValueError, fn)

    # A file that is not an image must be an ordinary error, not a traceback:
    # this selfcheck's own source is a convenient non-image.
    _expect_raises(
        "non-image file rejected by the blit loader",
        (ValueError, OSError),
        lambda: image_to_rgb565(__file__, 16, 16),
    )
    _expect_raises(
        "missing image file rejected by the blit loader",
        (ValueError, OSError),
        lambda: image_to_rgb565("no-such-file-here.png", 16, 16),
    )

    print("\n== hardening: link failure paths ==")
    a, _b, host, esp, _ = _make_pair((UART_TASK_ID_DISPLAY,))
    try:
        # Port yanked mid-session: the sender must say "no port", not burn
        # every retry instantly and report a peer timeout.
        a.is_open = False
        check(
            "send with the port gone -> NOT_CONNECTED",
            host.send(UART_TASK_ID_DISPLAY, UART_TASK_ID_DISPLAY, b"\x0f"),
            SendResult.NOT_CONNECTED,
        )
    finally:
        host.disconnect()
        esp.disconnect()

    # A reply carrying the wrong subcommand must not satisfy the outstanding
    # query -- otherwise a late/stray frame would be handed to the next
    # caller as if it answered them.
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_DISPLAY,))
    stop = threading.Event()
    _responder(
        stop, esp_inbox, esp, UART_TASK_ID_DISPLAY,
        # Answers a READ_ID request with a *THERMO-shaped* payload: the
        # subcommand byte does not match what was asked.
        lambda p: bytes([0x05, 0]) if p[0] == 0x0F else None,
    )
    client = DisplayClient(host)
    try:
        _expect_raises(
            "reply with a mismatched subcommand does not satisfy the query",
            DisplayQueryError,
            lambda: client.read_id(timeout=0.6),
        )
        check("no waiter left dangling after a timeout", client._pending, None)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()

    print("\n== hardening: link hub survives a hostile client ==")
    import socket as _socket

    from kilnctrl.link_hub import LinkHub, RemoteUartLink

    listen = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    listen.bind(("127.0.0.1", 0))
    listen.listen(8)
    hub_port = listen.getsockname()[1]
    hub = LinkHub(listen, own_device=Device.HOST)
    hub.start()

    raw = _socket.create_connection(("127.0.0.1", hub_port), timeout=2.0)
    try:
        # Valid JSON of the wrong shape, then an unparseable line, then a
        # real request: the first two must cost one message each, not the
        # connection (req.get() on a non-dict used to kill the handler
        # thread, and the client just saw its socket die).
        raw.sendall(b'5\n["not", "an", "object"]\n{oops\n{"id": 1, "op": "status"}\n')
        reply = raw.makefile("r", encoding="utf-8").readline()
        payload = __import__("json").loads(reply)
        check("hub answers after malformed request lines", payload.get("ok"), True)
        check("hub answered the right request id", payload.get("id"), 1)
    finally:
        raw.close()

    client = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    try:
        check("client reached the hub", client.status()["connected"], False)
        # An unknown op is an error *reply*, not a dropped connection.
        _expect_raises(
            "unknown op reported as an error",
            RuntimeError,
            lambda: client._request("no-such-op"),
        )
        check("client still usable after an error reply", client.status()["connected"], False)
    finally:
        client.close()
    # close() ends the reader, which must release every in-flight waiter
    # rather than leaving it to block for the full RPC timeout.
    check("no pending RPC waiters left after close", client._pending, {})

    # And the waiter must actually be *woken*, not merely forgotten: a GUI
    # worker blocked in _request when the hub process exits would otherwise
    # sit there for the whole 5-8 s budget with the answer already known.
    doomed = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    waiter = threading.Event()
    box: dict = {}
    with doomed._pending_lock:
        doomed._pending[999] = (waiter, box)
    doomed.close()
    check("in-flight RPC waiter released when the hub goes away", waiter.wait(2.0), True)
    check("released waiter carries a failure, not a result", box.get("msg", {}).get("ok"), False)
    listen.close()


