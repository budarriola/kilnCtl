"""Virtual-link loopback checks (no hardware).

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


def loopback_checks() -> None:
    from kilnctrl.serial_link import SendResult

    a, b, host, esp, (inbox,) = _make_pair((devices.UART_TASK_ID_THERMO,))

    try:
        payload = devices.thermo_one_shot(0)
        res = host.send(
            dst_task=devices.UART_TASK_ID_THERMO,
            src_task=devices.UART_TASK_ID_THERMO,
            payload=payload,
        )
        check("registered task -> OK (ACK)", res, SendResult.OK)
        got = inbox.get(timeout=1.0)
        check("payload arrived intact", got.payload, payload)
        check("inbox drained", inbox.qsize(), 0)

        # Task IO was never registered on the "ESP" side -> NACK.
        res = host.send(
            dst_task=devices.UART_TASK_ID_IO,
            src_task=devices.UART_TASK_ID_IO,
            payload=devices.io_all_relays_off(),
        )
        check("unregistered task -> UNDELIVERABLE (NACK)", res, SendResult.UNDELIVERABLE)

        # Drop the first two ACKs: the retransmit must be deduped (not
        # re-delivered) yet still re-ACKed, and the send must still succeed.
        b.drop_next_writes = 2
        res = host.send(
            dst_task=devices.UART_TASK_ID_THERMO,
            src_task=devices.UART_TASK_ID_THERMO,
            payload=payload,
        )
        check("lost ACKs recovered by retry -> OK", res, SendResult.OK)
        check("retransmit deduped (delivered once)", inbox.qsize(), 1)
        inbox.get_nowait()

        # Peer completely deaf: every attempt times out.
        a.drop_next_writes = 100
        host.max_retries = 3  # keep the check fast
        res = host.send(
            dst_task=devices.UART_TASK_ID_THERMO,
            src_task=devices.UART_TASK_ID_THERMO,
            payload=payload,
        )
        check("dead link -> TIMEOUT", res, SendResult.TIMEOUT)
    finally:
        host.disconnect()
        esp.disconnect()

    check(
        "send while disconnected -> NOT_CONNECTED",
        host.send(1, 1, b"\x00"),
        SendResult.NOT_CONNECTED,
    )
