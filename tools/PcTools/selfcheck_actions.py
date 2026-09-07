"""actions.py checks.

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
from selfcheck_info import _FW_VERSION_REPLY, _PIN_CONFIG_REPLY
from selfcheck_thermo import _thermo_read_reply


class _FakeSessionLog:
    """Stand-in for SessionLogger that records instead of touching disk."""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def info(self, msg, *args) -> None:
        self.lines.append(msg % args if args else msg)

    def warning(self, msg, *args) -> None:
        self.lines.append(msg % args if args else msg)

    def error(self, msg, *args) -> None:
        self.lines.append(msg % args if args else msg)


def actions_checks() -> None:
    """Exercise the generic action registry (actions.py) end to end.

    This is what backs the MCP server's press_button/list_buttons tools --
    verifies every registered action is well-formed, that a spot-check of real
    actions across all four device tasks goes over the wire and back, and --
    the safety-critical part -- that the protocol-version compatibility gate
    genuinely blocks device commands both before compatibility is known and
    after an incompatible version is observed.
    """
    from kilnctrl import actions
    from kilnctrl.display import DisplayClient
    from kilnctrl.info import InfoClient
    from kilnctrl.io_expander import IoClient
    from kilnctrl.protocol import (
        INFO_CMD_GET_FW_VERSION,
        INFO_CMD_GET_PIN_CONFIG,
        SYSTEM_CMD_FACTORY_RESET,
        UART_TASK_ID_DISPLAY,
        UART_TASK_ID_INFO,
        UART_TASK_ID_IO,
        UART_TASK_ID_SAFETY,
        UART_TASK_ID_SYSTEM,
        UART_TASK_ID_THERMO,
    )
    from kilnctrl.safety import SafetyClient
    from kilnctrl.thermo import ThermoClient

    print("\n== action registry (actions.py) ==")
    check("registry non-empty", len(actions.ACTIONS) > 0, True)
    for name, action in actions.ACTIONS.items():
        check(f"action {name!r} has a description", bool(action.description), True)
        check(f"action {name!r} is callable", callable(action.run), True)

    _a, _b, host, esp, (info_inbox, thermo_inbox, io_inbox, display_inbox, safety_inbox, system_inbox) = _make_pair(
        (
            UART_TASK_ID_INFO,
            UART_TASK_ID_THERMO,
            UART_TASK_ID_IO,
            UART_TASK_ID_DISPLAY,
            UART_TASK_ID_SAFETY,
            UART_TASK_ID_SYSTEM,
        )
    )

    stop = threading.Event()
    thermo_read_reply = _thermo_read_reply(((0, 25.5, 24.0, 0x00, 0x00),))
    io_read_reply = bytes([0x05]) + struct.pack("<HHBBBB", 0x0000, 0xFF00, 0b0001, 0, 0, 0)
    display_id_reply = bytes([0x0F, 1, 0x54, 0x80, 0x66]) + struct.pack("<HH", 480, 320)
    safety_status_reply = bytes([0x01, 0x00]) + struct.pack(
        "<ffBfffH", 0.0, 0.0, 0x00, 0.0, 0.0, 0.0, 0xFFFF
    )

    def info_answer(payload: bytes):
        if payload[0] == INFO_CMD_GET_PIN_CONFIG:
            return _PIN_CONFIG_REPLY
        if payload[0] == INFO_CMD_GET_FW_VERSION:
            return _FW_VERSION_REPLY
        return None

    _responder(stop, info_inbox, esp, UART_TASK_ID_INFO, info_answer)
    _responder(
        stop, thermo_inbox, esp, UART_TASK_ID_THERMO,
        lambda p: thermo_read_reply if p[0] == 0x05 else None,
    )
    _responder(
        stop, io_inbox, esp, UART_TASK_ID_IO,
        lambda p: io_read_reply if p[0] == 0x05 else None,
    )
    _responder(
        stop, display_inbox, esp, UART_TASK_ID_DISPLAY,
        lambda p: display_id_reply if p[0] == 0x0F else None,
    )
    _responder(
        stop, safety_inbox, esp, UART_TASK_ID_SAFETY,
        lambda p: safety_status_reply if p[0] == 0x01 else None,
    )
    system_seen: list[bytes] = []
    _responder(stop, system_inbox, esp, UART_TASK_ID_SYSTEM, lambda p: None, seen=system_seen)

    info_client = InfoClient(host)
    thermo_client = ThermoClient(host)
    io_client = IoClient(host)
    display_client = DisplayClient(host)
    safety_client = SafetyClient(host)
    ctx = actions.ActionContext(
        link=host,
        info=info_client,
        session_log=_FakeSessionLog(),
        thermo=thermo_client,
        io=io_client,
        display=display_client,
        safety=safety_client,
    )

    try:
        print("\n== action registry: live virtual-link exercise ==")

        result = actions.ACTIONS["IO: All Relays Off"].run(ctx)
        check(
            "device command blocked before compatibility is known",
            result.startswith("error: refused"),
            True,
        )

        version_text = actions.ACTIONS["INFO: Get FW Version"].run(ctx)
        check("INFO: Get FW Version reports compatible", "compatible: yes" in version_text, True)
        check("InfoClient.compatible now True", info_client.compatible, True)

        result = actions.ACTIONS["IO: All Relays Off"].run(ctx)
        check("IO: All Relays Off succeeds once compatible", result.startswith("ok"), True)

        result = actions.ACTIONS["IO: Set Relay"].run(ctx, relay=1, on=True)
        check("IO: Set Relay succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Thermo: One Shot"].run(ctx, channel=0)
        check("Thermo: One Shot succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Thermo: Read All"].run(ctx)
        check("Thermo: Read All returns device data", "CH0: 25.50 C" in result, True)

        result = actions.ACTIONS["IO: Read"].run(ctx)
        check("IO: Read returns device data", "R1=1" in result, True)

        result = actions.ACTIONS["Display: Clear"].run(ctx, color=0)
        check("Display: Clear succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Display: Read ID"].run(ctx)
        check("Display: Read ID returns device data", "480x320" in result, True)

        result = actions.ACTIONS["Safety: Get Status"].run(ctx)
        check("Safety: Get Status returns device data", "never received" in result, True)

        result = actions.ACTIONS["Safety: Set Fault Out"].run(ctx, assert_fault=True)
        check("Safety: Set Fault Out succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Display: Test Pattern"].run(ctx, width=8, height=4)
        check("Display: Test Pattern streams", result.startswith("ok"), True)

        result = actions.ACTIONS["System: Factory Reset"].run(ctx, scope=0)
        check(
            "System: Factory Reset refused without confirm=True",
            result.startswith("error: factory reset refused"),
            True,
        )
        check("System: Factory Reset did not send without confirm", len(system_seen), 0)

        result = actions.ACTIONS["System: Factory Reset"].run(ctx, scope=0, confirm=True)
        check("System: Factory Reset succeeds with confirm=True", result.startswith("ok"), True)
        deadline = time.time() + 2.0
        while not system_seen and time.time() < deadline:
            time.sleep(0.02)
        check(
            "System: Factory Reset sent the right wire payload",
            system_seen[-1:] == [bytes([SYSTEM_CMD_FACTORY_RESET, 0])],
            True,
        )

        result = actions.ACTIONS["System: Set Watchdog Panic Disabled"].run(ctx, disabled=True)
        check(
            "System: Set Watchdog Panic Disabled refused without confirm=True",
            result.startswith("error: watchdog panic change refused"),
            True,
        )

        pin_text = actions.ACTIONS["INFO: Get Pin Config"].run(ctx)
        check("INFO: Get Pin Config returns entries", "GPIO8" in pin_text, True)

        # Bad args must not crash press_button's caller -- TypeError/ValueError
        # from a wrong/missing parameter should be catchable, not a hard raise
        # out of run(). mcp_server.press_button relies on exactly this.
        try:
            actions.ACTIONS["IO: Set Relay"].run(ctx, relay=1)  # missing `on`
            check("missing required arg raises", False, True)
        except TypeError:
            check("missing required arg raises", True, True)

        # A query action with no client registered must report, not raise.
        bare = actions.ActionContext(link=host, info=info_client, session_log=_FakeSessionLog())
        check(
            "query action without its client reports cleanly",
            actions.ACTIONS["Thermo: Read All"].run(bare).startswith("error: no THERMO client"),
            True,
        )

        # Now simulate a firmware speaking a different protocol version and
        # confirm the gate re-engages, proving this isn't only checked once.
        # v1 is the unit-test fixture, where task 1 is a DAC -- the exact case
        # that must never be allowed to receive a thermocouple command.
        fixture_version = devices.parse_fw_version_response(
            bytes([1, 0]) + _FW_VERSION_REPLY[2:]
        )
        info_client.last_fw_version = fixture_version
        result = actions.ACTIONS["Thermo: One Shot"].run(ctx, channel=0)
        check(
            "device command blocked after incompatible (v1 fixture) version observed",
            result.startswith("error: refused"),
            True,
        )
        # INFO queries must still work even while incompatible -- that's the
        # only way compatibility could ever be re-established.
        version_text_again = actions.ACTIONS["INFO: Get FW Version"].run(ctx)
        check(
            "INFO queries stay allowed while incompatible",
            version_text_again.startswith("uart_protocol_version:"),
            True,
        )
    finally:
        stop.set()
        for client in (info_client, thermo_client, io_client, display_client, safety_client):
            client.close()
        host.disconnect()
        esp.disconnect()


