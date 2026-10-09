"""LOG frame checks.

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


def log_checks() -> None:
    """LOG task payload parsing, then a live push over the virtual link."""
    from kilnctrl.device_log import LogClient
    from kilnctrl.protocol import UART_TASK_ID_LOG, LogLevel
    from kilnctrl.serial_link import SendResult

    print("\n== LOG payload parsing (uart_task_ids.h) ==")
    check("task id LOG == 5", UART_TASK_ID_LOG, 5)

    line = devices.parse_log_frame(bytes([0]) + b"thermo: SPI read failed on ch1")
    check("level byte 0 -> ERROR", line.level, LogLevel.ERROR)
    check("text decoded", line.text, "thermo: SPI read failed on ch1")
    check("letter tag", line.letter, "E")

    line = devices.parse_log_frame(bytes([2]) + b"hello")
    check("level byte 2 -> INFO", line.level, LogLevel.INFO)

    line = devices.parse_log_frame(bytes([0xFF]) + b"unknown level")
    check("unknown level byte falls back to INFO", line.level, LogLevel.INFO)

    try:
        devices.parse_log_frame(b"")
        check("empty log frame rejected", False, True)
    except ValueError:
        check("empty log frame rejected", True, True)

    print("\n== LOG: unsolicited push over the virtual link ==")
    _a, _b, host, esp, _ = _make_pair()

    received: list[devices.LogLine] = []
    client = LogClient(host, on_line=received.append)
    try:
        # Firmware side never expects a reply -- same fire-and-forget shape
        # as uart_log_bridge_task() in uart_log_bridge.c.
        res = esp.send(
            dst_task=UART_TASK_ID_LOG,
            src_task=UART_TASK_ID_LOG,
            payload=bytes([LogLevel.WARN]) + b"sx1509: i2c write failed",
            dst_device=Device.HOST,
        )
        check("unsolicited LOG frame delivered", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not received and time.time() < deadline:
            time.sleep(0.02)
        check("LogClient received one line", len(received), 1)
        if received:
            check("received line level", received[0].level, LogLevel.WARN)
            check("received line text", received[0].text, "sx1509: i2c write failed")
    finally:
        client.close()
        host.disconnect()
        esp.disconnect()


