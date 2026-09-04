"""INFO task checks.

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



#: A GET_PIN_CONFIG reply as build_pin_config_reply() emits it for this
#: board's s_pin_config[] -- the nineteen ESP32-S3 GPIOs docs/HARDWARE.md
#: lists. Expander pins (relays, DRDY, LCD_IORQ/LCD_Reset) are deliberately
#: absent: they are not GPIOs and are reported by the IO task instead.
_PIN_CONFIG_ENTRIES = (
    (8, 0x01),   # SDA
    (9, 0x02),   # SCL
    (43, 0x03),  # UART0 TX (this link)
    (44, 0x04),  # UART0 RX
    (12, 0x05),  # SPI CLK
    (11, 0x06),  # SPI MOSI
    (13, 0x09),  # SPI MISO
    (14, 0x07),  # CS0 thermocouple 0
    (17, 0x07),  # CS1 thermocouple 1
    (18, 0x07),  # CS2 thermocouple 2
    (21, 0x07),  # CS3 display
    (38, 0x0A),  # thermoFault_0
    (47, 0x0A),  # thermoFault_1
    (48, 0x0A),  # thermoFault_2
    (7, 0x0B),   # IO_Expander_IRQ
    (10, 0x0C),  # IO_Expander_RST
    (5, 0x0D),   # DataToSafty   -- the ESP's TX (drives U2's LED via R12)
    (4, 0x0E),   # DataFromSafty -- the ESP's RX (U3's collector, R15 pull-up)
    (6, 0x0F),   # Fault, an ESP OUTPUT to the safety processor
)
_PIN_CONFIG_REPLY = bytes([len(_PIN_CONFIG_ENTRIES)]) + b"".join(
    bytes(entry) for entry in _PIN_CONFIG_ENTRIES
)

#: A GET_FW_VERSION reply as build_fw_version_reply() emits it: protocol
#: version (u16 LE) first at a fixed offset, then dirty/commit/datetime.
#: Built from devices.UART_PROTOCOL_VERSION itself, not a hand-copied
#: literal -- a hardcoded byte pair here (previously bytes([5, 0]), stale
#: against a UART_PROTOCOL_VERSION that had already moved to 10) is exactly
#: what silently turned this "compatible" fixture into an "INCOMPATIBLE"
#: one and cascaded failures through every check downstream of it.
_FW_VERSION_REPLY = (
    struct.pack("<H", devices.UART_PROTOCOL_VERSION)
    + bytes([1, 7])
    + b"a1b2c3d"
    + bytes([20])
    + b"2026-08-09 12:34:56Z"
)


def info_checks() -> None:
    """INFO request/response layouts, then the full query flow over the wire."""
    from kilnctrl.info import InfoClient, InfoQueryError
    from kilnctrl.protocol import (
        INFO_CMD_GET_FW_VERSION,
        INFO_CMD_GET_PIN_CONFIG,
        UART_TASK_ID_INFO,
    )

    print("\n== INFO payload layouts (uart_task_ids.h) ==")
    check("task id INFO == 3", UART_TASK_ID_INFO, 3)
    check("GET_PIN_CONFIG request bytes", devices.info_get_pin_config(), b"\x01")
    check("GET_FW_VERSION request bytes", devices.info_get_fw_version(), b"\x02")
    check(
        "pin config reply is 1 + 2N bytes",
        len(_PIN_CONFIG_REPLY),
        1 + len(_PIN_CONFIG_ENTRIES) * 2,
    )

    entries = devices.parse_pin_config_response(_PIN_CONFIG_REPLY)
    check("pin config entry count", len(entries), len(_PIN_CONFIG_ENTRIES))
    check(
        "pin config gpios",
        [e.gpio for e in entries],
        [gpio for gpio, _ in _PIN_CONFIG_ENTRIES],
    )
    check(
        "pin config labels resolved",
        entries[0].label,
        "I2C SDA (SX1509 expander, and J2/J6 pass-through)",
    )
    # The five function ids added for this board must all resolve to text --
    # an unlabelled id would show up in the About window as "unknown function
    # 0x09", which is exactly the regression this catches.
    for gpio, expected in ((13, "MISO"), (38, "TCFLT"), (7, "IOIRQ"), (10, "IORST"), (6, "SFFLT")):
        entry = next(e for e in entries if e.gpio == gpio)
        check(f"GPIO{gpio} abbrev", entry.abbrev, expected)
        check(f"GPIO{gpio} label is known", entry.label.startswith("unknown"), False)
    check("every reported gpio is on the diagram", pin_overlay.unmapped_gpios(entries), [])

    version = devices.parse_fw_version_response(_FW_VERSION_REPLY)
    check(
        "fw version protocol_version",
        version.protocol_version,
        devices.UART_PROTOCOL_VERSION,
    )
    check("fw version compatible", version.compatible, True)
    check("fw version dirty flag", version.dirty, True)
    check("fw version commit", version.commit, "a1b2c3d")
    check("fw version build timestamp", version.built, "2026-08-09 12:34:56Z")
    check(
        "fw version status-bar text",
        version.describe(),
        "FW a1b2c3d (dirty) built 2026-08-09 12:34:56Z",
    )
    clean = devices.parse_fw_version_response(
        _FW_VERSION_REPLY[:2] + b"\x00" + _FW_VERSION_REPLY[3:]
    )
    check("clean tree reads 'clean'", "(clean)" in clean.describe(), True)

    # v1 is the unit-test fixture, where task 1 is an MCP4728 DAC rather than
    # three thermocouples -- the single most important thing the version gate
    # has to catch on this board.
    fixture = devices.parse_fw_version_response(bytes([1, 0]) + _FW_VERSION_REPLY[2:])
    check("v1 (fixture) firmware parsed", fixture.protocol_version, 1)
    check("v1 (fixture) firmware is incompatible", fixture.compatible, False)
    check("incompatible describe() flags it", "INCOMPATIBLE" in fixture.describe(), True)

    # Replies carry no subcommand byte, so classification is structural.
    check(
        "classify pin config reply",
        devices.parse_info_response(_PIN_CONFIG_REPLY)[0],
        INFO_CMD_GET_PIN_CONFIG,
    )
    check(
        "classify fw version reply",
        devices.parse_info_response(_FW_VERSION_REPLY)[0],
        INFO_CMD_GET_FW_VERSION,
    )
    for label, bad in (
        ("empty payload rejected", b""),
        ("truncated pin config rejected", b"\x08\x04"),
        ("overrunning commit_len rejected", b"\x01\x40AB"),
    ):
        try:
            devices.parse_info_response(bad)
            check(label, False, True)
        except devices.InfoResponseError:
            check(label, True, True)

    print("\n== INFO query flow over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_INFO,))
    stop = threading.Event()

    def answer(payload: bytes):
        if payload[0] == INFO_CMD_GET_PIN_CONFIG:
            return _PIN_CONFIG_REPLY
        if payload[0] == INFO_CMD_GET_FW_VERSION:
            return _FW_VERSION_REPLY
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_INFO, answer)

    boot_pushes: list[devices.FirmwareVersion] = []
    client = InfoClient(host, on_boot_push=boot_pushes.append)
    try:
        got = client.get_pin_config(timeout=3.0)
        check(
            "live pin config query",
            [e.gpio for e in got][:4],
            [8, 9, 43, 44],
        )
        got_version = client.get_fw_version(timeout=3.0)
        check("live fw version query", got_version.commit, "a1b2c3d")
        check("query reply is not mistaken for a boot push", boot_pushes, [])

        # Unsolicited push: info_boot_push_task() sends this once at boot, to
        # (HOST, task INFO), with nobody having asked.
        esp.send(
            dst_task=UART_TASK_ID_INFO,
            src_task=UART_TASK_ID_INFO,
            payload=_FW_VERSION_REPLY,
            dst_device=Device.HOST,
        )
        deadline = time.time() + 2.0
        while not boot_pushes and time.time() < deadline:
            time.sleep(0.02)
        check("unsolicited push detected as reboot signal", len(boot_pushes), 1)

        # Deaf peer: the request itself never gets through.
        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.get_fw_version(timeout=0.3)
            check("undelivered query raises", False, True)
        except InfoQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


