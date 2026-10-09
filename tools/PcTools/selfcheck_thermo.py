"""THERMO task checks.

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


def _thermo_read_reply(entries) -> bytes:
    """Build a READ reply the way the firmware does: count, then 12 bytes each."""
    payload = bytearray([0x05, len(entries)])  # THERMO_CMD_READ
    for channel, temperature, cold_junction, status, flags in entries:
        payload += struct.pack("<BffBB", channel, temperature, cold_junction, status, flags)
        payload += b"\x00"  # byte[11] reserved
    return bytes(payload)


def thermo_checks() -> None:
    """THERMO payload layouts, the query flow, and the auto-report push."""
    from kilnctrl.protocol import THERMO_CHANNEL_ALL, UART_TASK_ID_THERMO, ThermoFault
    from kilnctrl.thermo import ThermoClient, ThermoQueryError

    print("\n== THERMO payload layouts (uart_task_ids.h) ==")
    check("task id THERMO == 1", UART_TASK_ID_THERMO, 1)
    check(
        "config_channel bytes",
        devices.thermo_config_channel(1, 3, 2, True, False),
        b"\x01\x01\x03\x02\x01\x00",
    )
    check(
        "set_thresholds bytes (f32 LE x2 then i8 x2)",
        devices.thermo_set_thresholds(0, 1300.0, -20.0, 85, -20),
        b"\x02\x00" + struct.pack("<ff", 1300.0, -20.0) + struct.pack("<bb", 85, -20),
    )
    check("set_thresholds len", len(devices.thermo_set_thresholds(0, 0, 0, 0, 0)), 12)
    check(
        "set_cj_offset bytes",
        devices.thermo_set_cj_offset(2, 1.5),
        b"\x03\x02" + struct.pack("<f", 1.5),
    )
    check("one_shot bytes", devices.thermo_one_shot(1), b"\x04\x01")
    check("read (all) bytes", devices.thermo_read(), b"\x05\xff")
    check("read (one) bytes", devices.thermo_read(2), b"\x05\x02")
    check("read_faults bytes", devices.thermo_read_faults(0), b"\x06\x00")
    check("clear_faults bytes", devices.thermo_clear_faults(1), b"\x07\x01")
    check(
        "set_auto_report bytes (u16 LE period)",
        devices.thermo_set_auto_report(0x07, 1000),
        b"\x08\x07" + struct.pack("<H", 1000),
    )
    check("read_reg bytes", devices.thermo_read_reg(0, 0x02, 4), b"\x09\x00\x02\x04")
    check("write_reg bytes", devices.thermo_write_reg(0, 0x02, 0xC3), b"\x0a\x00\x02\xc3")

    for label, fn in (
        ("channel 3 rejected", lambda: devices.thermo_one_shot(3)),
        ("channel 0xFF rejected where not allowed", lambda: devices.thermo_one_shot(0xFF)),
        ("bad tc_type rejected", lambda: devices.thermo_config_channel(0, 0x0B)),
        ("bad avg_mode rejected", lambda: devices.thermo_config_channel(0, 3, 9)),
        ("cj offset out of range rejected", lambda: devices.thermo_set_cj_offset(0, 9.0)),
        ("channel mask 0x08 rejected", lambda: devices.thermo_set_auto_report(0x08, 100)),
        ("read_reg len 17 rejected", lambda: devices.thermo_read_reg(0, 0, 17)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # Replies as thermo_bridge_task() emits them.
    read_reply = _thermo_read_reply(
        (
            (0, 25.5, 24.0, 0x00, 0x00),
            (1, 1201.25, 26.5, 0x08, 0x01),  # TCHIGH + ~FAULT pin asserted
            (2, float("nan"), float("nan"), 0x00, 0x02),  # SPI read failed
        )
    )
    check("READ reply is 2 + 12N bytes", len(read_reply), 2 + 3 * 12)
    subcommand, readings = devices.parse_thermo_response(read_reply)
    check("classify READ reply", subcommand, 0x05)
    check("read count", len(readings), 3)
    check("channel 0 temperature", round(readings[0].temperature_c, 2), 25.5)
    check("channel 0 cold junction", round(readings[0].cold_junction_c, 2), 24.0)
    check("channel 0 has no faults", readings[0].fault_labels, [])
    check("channel 0 valid", readings[0].valid, True)
    check(
        "channel 1 fault decoded to text",
        readings[1].fault_labels,
        ["TC above high threshold"],
    )
    check("channel 1 flag decoded to text", readings[1].flag_labels, ["~FAULT pin asserted"])
    check("channel 2 SPI failure flagged", readings[2].flag_labels, ["SPI read failed"])
    check("channel 2 not valid", readings[2].valid, False)
    check("channel 2 temperature is NaN", math.isnan(readings[2].temperature_c), True)
    check(
        "fault labels decode every bit",
        len(devices.thermo_fault_labels(0xFF)),
        8,
    )
    check("ThermoFault flag values", int(ThermoFault.OPEN | ThermoFault.CJRANGE), 0x81)

    faults_reply = bytes([0x06, 2, 0, 0x01, 0xFF, 1, 0x00, 0xFF])
    subcommand, faults = devices.parse_thermo_response(faults_reply)
    check("classify READ_FAULTS reply", subcommand, 0x06)
    check("fault entry count", len(faults), 2)
    check("fault entry 0 decoded", faults[0].fault_labels, ["open circuit (no thermocouple?)"])
    check("fault entry 1 has no faults", faults[1].fault_labels, [])

    reg_reply = bytes([0x09, 1, 0x02, 3, 0xAA, 0xBB, 0xCC])
    subcommand, registers = devices.parse_thermo_response(reg_reply)
    check("classify READ_REG reply", subcommand, 0x09)
    check("register bytes", registers.data, b"\xaa\xbb\xcc")

    for label, bad in (
        ("empty thermo reply rejected", b""),
        ("READ count mismatch rejected", bytes([0x05, 2, 0, 0])),
        ("READ_FAULTS count mismatch rejected", bytes([0x06, 3, 0, 0, 0])),
        ("READ_REG len mismatch rejected", bytes([0x09, 0, 0, 4, 1])),
        ("unknown thermo subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_thermo_response(bad)
            check(label, False, True)
        except devices.ThermoResponseError:
            check(label, True, True)

    print("\n== THERMO query flow + auto-report push over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_THERMO,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        if payload[0] == 0x05:
            return read_reply
        if payload[0] == 0x06:
            return faults_reply
        if payload[0] == 0x09:
            if payload[1] == 2:
                # Zero-length body: firmware's "this channel's SPI read
                # failed" marker (echoes channel/reg, len=0, no data bytes).
                return bytes([0x09, payload[1], payload[2], 0])
            return reg_reply
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_THERMO, answer, seen=writes)

    reports: list[list] = []
    client = ThermoClient(host, on_report=reports.append)
    try:
        got = client.read(THERMO_CHANNEL_ALL, timeout=3.0)
        check("live read query", [r.channel for r in got], [0, 1, 2])
        check("query answer is not mistaken for an auto-report", reports, [])
        check("last_readings cached", len(client.last_readings), 3)
        check("live read_faults query", len(client.read_faults(timeout=3.0)), 2)
        check("live read_reg query", client.read_reg(1, 0x02, 3, timeout=3.0).data, b"\xaa\xbb\xcc")

        # 2026-08-20: a 0-length READ_REG reply is the firmware's documented
        # marker for "this channel's SPI read failed" (uart_bridge.c). Channel
        # 2 is wired in `answer()` below to send that zero-length reply so we
        # can prove read_reg() actually raises on it -- not just that the
        # parse layer accepts the wire shape (that's covered separately by
        # "classify READ_REG reply" above). We also re-check the success path
        # through the same client/mechanism right after, so this proves the
        # *difference*, not merely that something threw.
        try:
            client.read_reg(2, 0x02, 3, timeout=3.0)
            check("read_reg raises on 0-length firmware reply", False, True)
        except ThermoQueryError as exc:
            check("read_reg raises on 0-length firmware reply", True, True)
            check(
                "0-length read_reg error names the channel",
                "ch2" in str(exc),
                True,
            )
        check(
            "read_reg still succeeds after a 0-length reply on another channel",
            client.read_reg(1, 0x02, 3, timeout=3.0).data,
            b"\xaa\xbb\xcc",
        )

        from kilnctrl.serial_link import SendResult

        res = client.send(devices.thermo_set_auto_report(0x07, 500))
        check("write-style subcommand ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("write reached the bridge", writes[:1], [b"\x08\x07" + struct.pack("<H", 500)])

        # The push: byte-identical to a READ answer, but nobody asked. This is
        # the inference the whole client design rests on.
        esp.send(
            dst_task=UART_TASK_ID_THERMO,
            src_task=UART_TASK_ID_THERMO,
            payload=read_reply,
            dst_device=Device.HOST,
        )
        deadline = time.time() + 2.0
        while not reports and time.time() < deadline:
            time.sleep(0.02)
        check("unsolicited READ detected as an auto-report", len(reports), 1)
        if reports:
            check("auto-report carries all channels", len(reports[0]), 3)

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read(timeout=0.3)
            check("undelivered query raises", False, True)
        except ThermoQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


