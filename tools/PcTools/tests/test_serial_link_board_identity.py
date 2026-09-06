#!/usr/bin/env python3
"""Tests for serial_link.py's main-board / fixture port disambiguation.

Two ESP32-S3 boards are now permanently on the bench (2026-09-05): the main
board and the UnitTestFixture. Confirmed bug this covers: recommend_port()
(the MAIN board's picker) used to pick the fixture's CH340K UART bridge
(COM14) over the main board's own CH343 bridge (COM6), because both boards'
generic "ch340"/"ch343" text hints score +50 and the tie-break on COM-port
name text ("COM14" < "COM6" lexicographically) favoured the fixture.

Run with: python -m pytest tools/PcTools/tests/test_serial_link_board_identity.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import serial_link  # noqa: E402
from kilnctrl.serial_link import PortInfo  # noqa: E402


class _FakeComPort:
    """Stands in for serial.tools.list_ports_common.ListPortInfo, the object
    serial_link._score_port()/list_ports() actually iterate over."""

    def __init__(self, device, description, manufacturer, hwid):
        self.device = device
        self.description = description
        self.manufacturer = manufacturer
        self.product = ""
        self.hwid = hwid


#: Bench enumeration, 2026-09-05, all six ports present at once (both
#: boards' JTAG + UART, both CMSIS-DAP debug probes).
_SIX_PORT_BENCH = [
    _FakeComPort("COM7", "USB-SERIAL-JTAG", "Espressif",
                 "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8"),
    _FakeComPort("COM14", "USB-SERIAL CH340K", "wch.cn",
                 "USB VID:PID=1A86:7522"),
    _FakeComPort("COM3", "USB-SERIAL-JTAG", "Espressif",
                 "USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C"),
    _FakeComPort("COM6", "USB-SERIAL CH343", "wch.cn",
                 "USB VID:PID=1A86:55D3 SER=552E006806"),
    _FakeComPort("COM10", "CMSIS-DAP", "Raspberry Pi",
                 "USB VID:PID=2E8A:000C SER=E66540F0A36C6E21"),
    _FakeComPort("COM11", "CMSIS-DAP", "Raspberry Pi",
                 "USB VID:PID=2E8A:000C SER=DEADBEEF00000000"),
]


def _patch_comports(ports):
    return mock.patch.object(serial_link._list_ports, "comports", return_value=ports)


class RecommendPortTest(unittest.TestCase):
    """recommend_port() is the MAIN board's picker (used by connect(),
    profiles, OTA, settings -- everything that isn't fixture.py)."""

    def test_six_port_bench_picks_main_board_not_fixture(self) -> None:
        with _patch_comports(_SIX_PORT_BENCH):
            self.assertEqual(serial_link.recommend_port(), "COM6")

    def test_fixture_absent_behavior_unchanged(self) -> None:
        """Only the main board attached (no fixture on the bench at all):
        must still pick the main board's CH343 exactly as before this
        change."""
        ports = [
            _FakeComPort("COM3", "USB-SERIAL-JTAG", "Espressif",
                         "USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C"),
            _FakeComPort("COM6", "USB-SERIAL CH343", "wch.cn",
                         "USB VID:PID=1A86:55D3 SER=552E006806"),
        ]
        with _patch_comports(ports):
            self.assertEqual(serial_link.recommend_port(), "COM6")

    def test_only_fixture_attached_finds_nothing(self) -> None:
        """Fixture alone on the bench (main board unplugged): the main
        board's picker must never return the fixture's port -- there is
        nothing for it to recommend."""
        ports = [
            _FakeComPort("COM7", "USB-SERIAL-JTAG", "Espressif",
                         "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8"),
            _FakeComPort("COM14", "USB-SERIAL CH340K", "wch.cn",
                         "USB VID:PID=1A86:7522"),
        ]
        with _patch_comports(ports):
            self.assertIsNone(serial_link.recommend_port())

    def test_negative_confirms_the_test_can_fail(self) -> None:
        """Negative test: break the fix (is_fixture_port() never excludes
        anything, matching the pre-fix code's behavior of picking whatever
        scored highest with no fixture-awareness at all) and confirm the
        fixture-only scenario above would then fail -- proving
        test_only_fixture_attached_finds_nothing actually exercises the fix
        rather than passing vacuously. This is also the real confirmed bug:
        with only the fixture attached (main board unplugged), the pre-fix
        picker had nothing to exclude the fixture's CH340K with and
        returned its port as if it were the main board's."""
        ports = [
            _FakeComPort("COM7", "USB-SERIAL-JTAG", "Espressif",
                         "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8"),
            _FakeComPort("COM14", "USB-SERIAL CH340K", "wch.cn",
                         "USB VID:PID=1A86:7522"),
        ]
        pre_fix_vid_hints = tuple(
            h for h in serial_link._VID_HINTS if h[0] != "1A86:7522"
        )
        with mock.patch.object(serial_link, "is_fixture_port", return_value=False), \
             mock.patch.object(serial_link, "_VID_HINTS", pre_fix_vid_hints):
            with _patch_comports(ports):
                self.assertEqual(serial_link.recommend_port(), "COM14")


class BoardIdentityHelpersTest(unittest.TestCase):
    def test_is_main_board_port_matches_jtag_and_uart(self) -> None:
        jtag = PortInfo(device="COM3", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                         hwid="USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C", score=0)
        uart = PortInfo(device="COM6", description="USB-SERIAL CH343", manufacturer="wch.cn",
                         hwid="USB VID:PID=1A86:55D3 SER=552E006806", score=0)
        self.assertTrue(serial_link.is_main_board_port(jtag))
        self.assertTrue(serial_link.is_main_board_port(uart))

    def test_is_fixture_port_matches_jtag_and_uart(self) -> None:
        jtag = PortInfo(device="COM7", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                         hwid="USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8", score=0)
        uart = PortInfo(device="COM14", description="USB-SERIAL CH340K", manufacturer="wch.cn",
                         hwid="USB VID:PID=1A86:7522", score=0)
        self.assertTrue(serial_link.is_fixture_port(jtag))
        self.assertTrue(serial_link.is_fixture_port(uart))

    def test_identities_never_cross_match(self) -> None:
        for port in _SIX_PORT_BENCH:
            info = PortInfo(device=port.device, description=port.description,
                             manufacturer=port.manufacturer, hwid=port.hwid, score=0)
            if serial_link.is_main_board_port(info):
                self.assertFalse(serial_link.is_fixture_port(info), port.device)
            if serial_link.is_fixture_port(info):
                self.assertFalse(serial_link.is_main_board_port(info), port.device)


if __name__ == "__main__":
    unittest.main()
