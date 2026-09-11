"""SAFETY task checks.

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


def safety_checks() -> None:
    """SAFETY payload layouts and the query flow, including a down link."""
    from kilnctrl.protocol import SAFETY_AGE_NEVER, UART_TASK_ID_SAFETY
    from kilnctrl.safety import SafetyClient, SafetyQueryError

    print("\n== SAFETY payload layouts (uart_task_ids.h) ==")
    check("task id SAFETY == 7", UART_TASK_ID_SAFETY, 7)
    check("get_status bytes", devices.safety_get_status(), b"\x01")
    check("request_enable bytes", devices.safety_request_enable(True), b"\x02\x01")
    check("ping bytes", devices.safety_ping(), b"\x03")
    check("get_link_stats bytes", devices.safety_get_link_stats(), b"\x04")
    check(
        "set_poll_period bytes (u16 LE)",
        devices.safety_set_poll_period(500),
        b"\x05" + struct.pack("<H", 500),
    )
    check("set_fault_out bytes", devices.safety_set_fault_out(True), b"\x06\x01")
    check("clear_fault_out bytes", devices.safety_set_fault_out(False), b"\x06\x00")

    # The state this board is actually in today: no Pico firmware exists, so
    # the ESP has never had a valid reply. This must parse cleanly and read as
    # "expected", not as a malformed frame or an error.
    down_reply = bytes([0x01, 0x00]) + struct.pack(
        "<ffBfffH", 0.0, 0.0, 0x00, 0.0, 0.0, 0.0, SAFETY_AGE_NEVER
    )
    check("GET_STATUS reply is 25 bytes", len(down_reply), 25)
    subcommand, status = devices.parse_safety_response(down_reply)
    check("classify GET_STATUS reply", subcommand, 0x01)
    check("link reported down", status.link_up, False)
    check("age is 'never received'", status.never_received, True)
    check("no flags set", status.flag_labels, [])
    check("down link is not an exception", isinstance(status, devices.SafetyStatus), True)

    # A live link, for when the Pico firmware exists.
    up_reply = bytes([0x01, 0x01 | 0x08 | 0x10 | 0x20]) + struct.pack(
        "<ffBfffH", 1150.5, 27.25, 0x00, 12.5, 0.0, 3.25, 120
    )
    _subcommand, live = devices.parse_safety_response(up_reply)
    check("link reported up", live.link_up, True)
    check("relay energized", live.relay_energized, True)
    check("heating enable granted", live.enabled, True)
    check("safety temperature", round(live.temperature_c, 2), 1150.5)
    check("current sense channels", [round(a, 2) for a in live.current_a], [12.5, 0.0, 3.25])
    check("age in ms", live.age_ms, 120)
    check("fault line not asserted by us", live.fault_asserted, False)

    # The fault flag is ours: GPIO6 is an ESP output.
    _subcommand, faulting = devices.parse_safety_response(
        bytes([0x01, 0x02]) + down_reply[2:]
    )
    check("fault line asserted by this firmware", faulting.fault_asserted, True)
    check(
        "fault flag has a readable label",
        faulting.flag_labels,
        ["fault line asserted (by us)"],
    )

    stats_reply = bytes([0x04]) + struct.pack("<IIIIH", 1000, 0, 0, 1000, 500)
    check("GET_LINK_STATS reply is 19 bytes", len(stats_reply), 19)
    subcommand, stats = devices.parse_safety_response(stats_reply)
    check("classify GET_LINK_STATS reply", subcommand, 0x04)
    check("frames sent", stats.frames_sent, 1000)
    # Sent climbing with received stuck at zero is exactly the signature of
    # the missing Pico firmware. This is a hand-built wire fixture, not a
    # live board -- it just proves the field decodes; the 1000 value no
    # longer means "1000 GET_STATUS exchanges got no reply" on real firmware
    # (that per-exchange definition was retired 2026-09-10, docs/audits/
    # safety_link_get_status_timeout_counter_2026-09-10.md) -- it is now a
    # count of ~500 ms poll iterations that saw zero new STATUS frames
    # applied anywhere, which a link with NOTHING ever received (this
    # fixture) would trivially max out on every single iteration too.
    check("nothing ever received", stats.frames_received, 0)
    check("every poll iteration saw a push gap", stats.timeouts, 1000)
    check("poll period", stats.poll_period_ms, 500)

    for label, bad in (
        ("empty safety reply rejected", b""),
        ("short GET_STATUS reply rejected", bytes([0x01, 0, 0])),
        ("short GET_LINK_STATS reply rejected", bytes([0x04, 0, 0])),
        ("unknown safety subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_safety_response(bad)
            check(label, False, True)
        except devices.SafetyResponseError:
            check(label, True, True)

    print("\n== SAFETY query flow over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_SAFETY,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        if payload[0] == 0x01:
            return down_reply
        if payload[0] == 0x04:
            return stats_reply
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_SAFETY, answer, seen=writes)

    client = SafetyClient(host)
    try:
        got = client.get_status(timeout=3.0)
        check("live get_status query (link down)", got.never_received, True)
        check("status cached", client.last_status is not None, True)
        check("live get_link_stats query", client.get_link_stats(timeout=3.0).timeouts, 1000)

        from kilnctrl.serial_link import SendResult

        res = client.send(devices.safety_set_fault_out(True))
        check("set_fault_out ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("fault-out write reached the bridge", writes[:1], [b"\x06\x01"])

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.get_status(timeout=0.3)
            check("undelivered query raises", False, True)
        except SafetyQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


