"""IO task checks.

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


def io_checks() -> None:
    """IO (SX1509) payload layouts, query flow, and the auto-report push."""
    from kilnctrl.io_expander import IoClient, IoQueryError
    from kilnctrl.protocol import UART_TASK_ID_IO

    print("\n== IO payload layouts (uart_task_ids.h) ==")
    check("task id IO == 2", UART_TASK_ID_IO, 2)
    check("set_relay bytes", devices.io_set_relay(1, True), b"\x01\x01\x01")
    check("set_relay_mask bytes", devices.io_set_relay_mask(0x0F, 0x05), b"\x02\x0f\x05")
    check("set_io bytes", devices.io_set_io(7, False), b"\x03\x07\x00")
    check("set_io_dir bytes", devices.io_set_io_dir(3, True, True), b"\x04\x03\x01\x01")
    check("read request bytes", devices.io_read(), b"\x05")
    check(
        "set_auto_report bytes (u16 LE)",
        devices.io_set_auto_report(500),
        b"\x06" + struct.pack("<H", 500),
    )
    check("all_relays_off bytes", devices.io_all_relays_off(), b"\x07")
    check("sx_write_reg bytes", devices.sx_write_reg(0x0F, 0xA5), b"\x10\x0f\xa5")
    check("sx_read_reg bytes", devices.sx_read_reg(0x10, 2), b"\x11\x10\x02")
    check("sx_set_dir bytes", devices.sx_set_dir(0xFF00), b"\x12\x00\xff")
    check("sx_set_pullup bytes", devices.sx_set_pullup(0x0010), b"\x13\x10\x00")
    check("sx_set_opendrain bytes", devices.sx_set_opendrain(0x0020), b"\x14\x20\x00")
    check(
        "sx_set_debounce bytes (u16 mask then config)",
        devices.sx_set_debounce(0x00F0, 3),
        b"\x15" + struct.pack("<H", 0x00F0) + b"\x03",
    )
    check(
        "sx_set_int_mask bytes (mask u16 then sense u16)",
        devices.sx_set_int_mask(0x00FF, 0x5555),
        b"\x16" + struct.pack("<HH", 0x00FF, 0x5555),
    )
    check("sx_led_driver bytes", devices.sx_led_driver(4, True, 128), b"\x17\x04\x01\x80")
    check("sx_reset bytes", devices.sx_reset(True), b"\x18\x01")
    check("sx_scan bytes", devices.sx_scan(), b"\x19")

    for label, fn in (
        ("relay 0 rejected", lambda: devices.io_set_relay(0, True)),
        ("relay 5 rejected", lambda: devices.io_set_relay(5, True)),
        ("io 8 rejected", lambda: devices.io_set_io(8, True)),
        ("relay mask 0x10 rejected", lambda: devices.io_set_relay_mask(0x10, 0)),
        ("debounce config 8 rejected", lambda: devices.sx_set_debounce(0, 8)),
        ("led pin 16 rejected", lambda: devices.sx_led_driver(16, True)),
        ("sx_read_reg len 17 rejected", lambda: devices.sx_read_reg(0, 17)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # The relay numbering is the single most dangerous thing on this board to
    # get wrong, so pin the mapping down in a test rather than only in a doc.
    check("Relay1 is K3 on J8", devices.relay_label(1), "Relay1 (K3 -> J8)")
    check("Relay2 is K1 on J3", devices.relay_label(2), "Relay2 (K1 -> J3)")
    check("Relay3 is K2 on J4", devices.relay_label(3), "Relay3 (K2 -> J4)")
    check("Relay4 is K5 on J11", devices.relay_label(4), "Relay4 (K5 -> J11)")
    check("expander pin 8 is DRDY0", devices.expander_pin_name(8), "thermoDrdy_0")
    check("expander pin 15 is LCD_Reset", devices.expander_pin_name(15).startswith("LCD_Reset"), True)

    # READ reply as io_bridge_task() emits it: relays 1+3 on, IO1/IO2 high,
    # DRDY on channel 1, ~INT asserted.
    read_reply = bytes([0x05]) + struct.pack(
        "<HHBBBB", 0x0105, 0xFF00, 0b0101, 0b0000011, 0b010, 0x01
    )
    check("READ reply is 9 bytes", len(read_reply), 9)
    subcommand, state = devices.parse_io_response(read_reply)
    check("classify READ reply", subcommand, 0x05)
    check("relay 1 on", state.relay(1), True)
    check("relay 2 off", state.relay(2), False)
    check("relay 3 on", state.relay(3), True)
    check("io 1 high", state.io_level(1), True)
    check("io 3 low", state.io_level(3), False)
    check("io 1 is an input (dir bit 4)", state.io_is_input(1), False)
    check("io 5 is an input (dir bit 11)", state.io_is_input(5), True)
    check("channel 1 DRDY asserted", state.drdy_asserted(1), True)
    check("channel 0 DRDY idle", state.drdy_asserted(0), False)
    check("~INT asserted", state.int_asserted, True)
    check("I2C did not fail", state.i2c_failed, False)

    reg_reply = bytes([0x11, 0x10, 2, 0xDE, 0xAD])
    subcommand, registers = devices.parse_io_response(reg_reply)
    check("classify SX_READ_REG reply", subcommand, 0x11)
    check("expander register bytes", registers.data, b"\xde\xad")

    scan_reply = bytes([0x19, 1, 0x3E])
    subcommand, found = devices.parse_io_response(scan_reply)
    check("classify SX_SCAN reply", subcommand, 0x19)
    check("scan found the board's expander", found, [0x3E])

    for label, bad in (
        ("empty io reply rejected", b""),
        ("short READ reply rejected", bytes([0x05, 0, 0])),
        ("SX_READ_REG len mismatch rejected", bytes([0x11, 0, 4, 1])),
        ("SX_SCAN count mismatch rejected", bytes([0x19, 3, 0x3E])),
        ("unknown io subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_io_response(bad)
            check(label, False, True)
        except devices.IoResponseError:
            check(label, True, True)

    print("\n== IO query flow + auto-report push over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_IO,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        if payload[0] == 0x05:
            return read_reply
        if payload[0] == 0x11:
            return reg_reply
        if payload[0] == 0x19:
            return scan_reply
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_IO, answer, seen=writes)

    reports: list = []
    client = IoClient(host, on_report=reports.append)
    try:
        state = client.read(timeout=3.0)
        check("live read query", (state.relay(1), state.drdy_asserted(1)), (True, True))
        check("query answer is not mistaken for an auto-report", reports, [])
        check("live scan query", client.scan(timeout=3.0), [0x3E])
        check("live read_reg query", client.read_reg(0x10, 2, timeout=3.0).data, b"\xde\xad")

        from kilnctrl.serial_link import SendResult

        res = client.send(devices.io_set_relay(2, True))
        check("write-style subcommand ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("write reached the bridge", writes[:1], [b"\x01\x02\x01"])

        esp.send(
            dst_task=UART_TASK_ID_IO,
            src_task=UART_TASK_ID_IO,
            payload=read_reply,
            dst_device=Device.HOST,
        )
        deadline = time.time() + 2.0
        while not reports and time.time() < deadline:
            time.sleep(0.02)
        check("unsolicited READ detected as an auto-report", len(reports), 1)

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read(timeout=0.3)
            check("undelivered query raises", False, True)
        except IoQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


