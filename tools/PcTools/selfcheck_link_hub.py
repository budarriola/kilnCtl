"""link_hub.py checks.

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


def link_hub_checks() -> None:
    """Two independent RemoteUartLink clients sharing one LinkHub -- proves
    the fan-out/refcounted-subscription design in link_hub.py without
    touching the well-known HUB_PORT (so this doesn't collide with a real
    GUI/MCP server hub that might already be running on this machine) or
    real hardware (no connect() call; the hub's own UartLink just never
    opens a port)."""
    import socket as _socket

    from kilnctrl.link_hub import LinkHub, RemoteUartLink

    print("\n== link hub: multiple clients share one link (no hardware) ==")
    listen = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    listen.bind(("127.0.0.1", 0))
    listen.listen(8)
    hub_port = listen.getsockname()[1]
    hub = LinkHub(listen, own_device=Device.HOST)
    hub.start()

    client_a = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    client_b = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    try:
        status = client_a.status()
        check("hub status reachable via RPC", status["connected"], False)

        # Regression guard: create_connection's connect timeout must not be
        # left on the socket. If it is, an idle link (the normal resting
        # state between commands) makes the reader thread raise TimeoutError
        # -- an OSError subclass, so _read_loop swallows it -- and the client
        # silently goes "disconnected" that many seconds after the last
        # traffic, with healthy hardware.
        check(
            "client socket is blocking (no lingering connect timeout)",
            client_a._sock.gettimeout(),
            None,
        )

        task_id = devices.UART_TASK_ID_LOG
        inbox_a = client_a.register_task(task_id)
        inbox_b = client_b.register_task(task_id)
        check("task registered once on the real (shared) link", task_id in hub.link._tasks, True)

        # Simulate an inbound DATA frame "from the ESP" by injecting straight
        # into the hub's real UartLink inbox -- both subscribers should see
        # their own independent copy.
        frame = Frame(
            msg_type=MsgType.DATA,
            msg_index=1,
            src_device=Device.ESP,
            src_task=task_id,
            dst_device=Device.HOST,
            dst_task=task_id,
            payload=bytes([2]) + b"hello",
        )
        hub.link._tasks[task_id].inbox.put(frame)

        got_a = inbox_a.get(timeout=2.0)
        got_b = inbox_b.get(timeout=2.0)
        check("client A saw the fanned-out frame", got_a.payload, frame.payload)
        check("client B saw the same fanned-out frame", got_b.payload, frame.payload)

        client_a.unregister_task(task_id)
        time.sleep(0.1)
        check(
            "task stays registered while another subscriber remains",
            task_id in hub.link._tasks,
            True,
        )
        client_b.unregister_task(task_id)
        time.sleep(0.1)
        check(
            "task unregistered from the real link once the last subscriber leaves",
            task_id in hub.link._tasks,
            False,
        )
    finally:
        client_a.close()
        client_b.close()


