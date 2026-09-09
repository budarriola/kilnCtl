#!/usr/bin/env python3
"""Tests for serial_link.py's main-board / fixture port disambiguation.

Two ESP32-S3 boards are now permanently on the bench (2026-09-05): the main
board and the UnitTestFixture. Identification is by USB SERIAL NUMBER
wherever the unit reports one, and by VID:PID only for the one unit that
does not (the main board's CH340K bridge).

Bench identities, CORRECTED 2026-09-09 -- the two boards' UART bridges were
recorded backwards from 2026-09-05 until then, so recommend_port() excluded
the MAIN board's own bridge and returned None on a bench where it was the
only bridge attached. Settled by a live get_fw_version round trip over the
CH340K (COM14), which answered as KilnFW. See serial_link.py's
board-identity block for the full evidence.

  main board  303A:1001 SER=1C:DB:D4:92:F4:7C   (native JTAG, = its STA MAC)
  main board  1A86:7522 no serial               (CH340K UART bridge, COM14)
  fixture     303A:1001 SER=68:B6:B3:29:D0:B8   (native JTAG)
  fixture     1A86:55D3 SER=552E006806          (CH343 UART bridge)

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


#: Bench enumeration, all six ports present at once (both boards' JTAG +
#: UART, both CMSIS-DAP debug probes), with each UART bridge assigned to the
#: board that actually owns it (see the module docstring).
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
            self.assertEqual(serial_link.recommend_port(), "COM14")

    def test_fixture_absent_picks_the_main_boards_own_bridge(self) -> None:
        """Only the main board attached (no fixture bridge on the bench) --
        the live 2026-09-09 bench state, enumerated verbatim. This is the
        regression the backwards UART identities caused: recommend_port()
        returned None here, and every kilnctrl call failed with "no serial
        port open"."""
        ports = [
            _FakeComPort("COM3", "USB Serial Device (COM3)", "Microsoft",
                         "USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C"),
            _FakeComPort("COM14", "USB-SERIAL CH340K (COM14)", "wch.cn",
                         "USB VID:PID=1A86:7522 LOCATION=1-2.4.4.4"),
        ]
        with _patch_comports(ports):
            self.assertEqual(serial_link.recommend_port(), "COM14")

    def test_only_fixture_attached_finds_nothing(self) -> None:
        """Fixture alone on the bench (main board unplugged): the main
        board's picker must never return the fixture's port -- there is
        nothing for it to recommend."""
        ports = [
            _FakeComPort("COM7", "USB-SERIAL-JTAG", "Espressif",
                         "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8"),
            _FakeComPort("COM6", "USB-SERIAL CH343", "wch.cn",
                         "USB VID:PID=1A86:55D3 SER=552E006806"),
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
        picker had nothing to exclude the fixture's bridge with and returned
        its port as if it were the main board's."""
        ports = [
            _FakeComPort("COM7", "USB-SERIAL-JTAG", "Espressif",
                         "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8"),
            _FakeComPort("COM6", "USB-SERIAL CH343", "wch.cn",
                         "USB VID:PID=1A86:55D3 SER=552E006806"),
        ]
        pre_fix_vid_hints = tuple(
            h for h in serial_link._VID_HINTS if h[0] != "1A86:55D3"
        )
        with mock.patch.object(serial_link, "is_fixture_port", return_value=False), \
             mock.patch.object(serial_link, "_VID_HINTS", pre_fix_vid_hints):
            with _patch_comports(ports):
                self.assertEqual(serial_link.recommend_port(), "COM6")


class BoardIdentityHelpersTest(unittest.TestCase):
    def test_is_main_board_port_matches_jtag_and_uart(self) -> None:
        jtag = PortInfo(device="COM3", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                         hwid="USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C", score=0)
        uart = PortInfo(device="COM14", description="USB-SERIAL CH340K", manufacturer="wch.cn",
                         hwid="USB VID:PID=1A86:7522 LOCATION=1-2.4.4.4", score=0)
        self.assertTrue(serial_link.is_main_board_port(jtag))
        self.assertTrue(serial_link.is_main_board_port(uart))

    def test_is_fixture_port_matches_jtag_and_uart(self) -> None:
        jtag = PortInfo(device="COM7", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                         hwid="USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8", score=0)
        uart = PortInfo(device="COM6", description="USB-SERIAL CH343", manufacturer="wch.cn",
                         hwid="USB VID:PID=1A86:55D3 SER=552E006806", score=0)
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


#: The probe's CDC interface as Windows actually enumerates it (bench,
#: 2026-09-09): a bland "USB Serial Device", with nothing in the description
#: naming a probe -- which is exactly why it scored +25 on the generic
#: "usb-serial" text hint and beat the real bridge.
_PROBE_A = _FakeComPort("COM10", "USB Serial Device (COM10)", "Microsoft",
                        "USB VID:PID=2E8A:000C SER=E66540F0A36C6E21 LOCATION=1-2.4.2:x.1")
_PROBE_B = _FakeComPort("COM11", "USB Serial Device (COM11)", "Microsoft",
                        "USB VID:PID=2E8A:000C SER=DEADBEEF00000000 LOCATION=1-2.4.3:x.1")
_MAIN_BRIDGE = _FakeComPort("COM14", "USB-SERIAL CH340K (COM14)", "wch.cn",
                            "USB VID:PID=1A86:7522 LOCATION=1-2.4.4.4")
_INERT = _FakeComPort("COM1", "Communications Port (COM1)", "", r"ACPI\PNP0501\0")


class DebugProbeExclusionTest(unittest.TestCase):
    """Confirmed bug, 2026-09-09: autodiscovery picked COM10 -- the Pico
    CMSIS-DAP debug probe's CDC interface (2E8A:000C, serial
    E66540F0A36C6E21, the same unit debug_probe.py pins for
    debug_program(peer="pico")) -- scoring it 25 and labelling it
    "(recommended)". Every kilnctrl call afterwards failed with "no serial
    port open - connect first". Excluded by VID:PID, so it covers BOTH
    identical probes on this bench, not one serial number."""

    def test_probe_only_selects_nothing(self) -> None:
        with _patch_comports([_PROBE_A, _PROBE_B, _INERT]):
            self.assertIsNone(serial_link.recommend_port())

    def test_bridge_only_selects_the_bridge(self) -> None:
        with _patch_comports([_MAIN_BRIDGE, _INERT]):
            self.assertEqual(serial_link.recommend_port(), "COM14")

    def test_both_present_selects_the_bridge(self) -> None:
        with _patch_comports([_PROBE_A, _PROBE_B, _MAIN_BRIDGE, _INERT]):
            self.assertEqual(serial_link.recommend_port(), "COM14")

    def test_probe_never_recommended_in_listing(self) -> None:
        with _patch_comports([_PROBE_A, _PROBE_B, _MAIN_BRIDGE]):
            for info in serial_link.list_ports():
                if serial_link.is_debug_probe_port(info):
                    self.assertFalse(info.recommended, info.device)

    def test_is_debug_probe_port_covers_both_units_and_legacy_pid(self) -> None:
        for hwid in ("USB VID:PID=2E8A:000C SER=E66540F0A36C6E21",
                     "USB VID:PID=2e8a:000c SER=DEADBEEF00000000",
                     "USB VID:PID=2E8A:0004 SER=000000000000"):
            info = PortInfo(device="COMx", description="USB Serial Device",
                            manufacturer="", hwid=hwid, score=0)
            self.assertTrue(serial_link.is_debug_probe_port(info), hwid)
        self.assertFalse(serial_link.is_debug_probe_port(
            PortInfo(device="COM6", description="USB-SERIAL CH343", manufacturer="wch.cn",
                     hwid="USB VID:PID=1A86:55D3 SER=552E006806", score=0)))


class SerialFirstIdentificationTest(unittest.TestCase):
    """Serial number is the source of truth; VID:PID is a fallback used only
    where the unit reports no serial. Regression cover for the 2026-09-05
    mix-up, which came from treating a chip family (CH340K vs CH343) as a
    board identity."""

    @staticmethod
    def _info(device, description, hwid):
        return PortInfo(device=device, description=description,
                        manufacturer="wch.cn", hwid=hwid, score=0)

    def test_identify_by_serial_main_board_jtag(self) -> None:
        self.assertEqual(serial_link.identify_port(self._info(
            "COM3", "USB Serial Device",
            "USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C")), "main_board")

    def test_identify_by_serial_fixture_uart_bridge(self) -> None:
        """The fixture's bridge DOES report a serial, so the serial decides
        -- not its 1A86:55D3 chip family."""
        self.assertEqual(serial_link.identify_port(self._info(
            "COM6", "USB-SERIAL CH343",
            "USB VID:PID=1A86:55D3 SER=552E006806")), "fixture")

    def test_identify_by_serial_fixture_jtag(self) -> None:
        self.assertEqual(serial_link.identify_port(self._info(
            "COM7", "USB Serial Device",
            "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8")), "fixture")

    def test_serial_wins_over_vid_pid_for_a_third_unit(self) -> None:
        """A DIFFERENT CH343 (same VID:PID, unknown serial) must come back
        as unidentified, never adopted as the fixture on chip family alone.
        Same for a third ESP32-S3 on the shared 303A:1001."""
        self.assertIsNone(serial_link.identify_port(self._info(
            "COM20", "USB-SERIAL CH343",
            "USB VID:PID=1A86:55D3 SER=AABBCCDDEE")))
        self.assertIsNone(serial_link.identify_port(self._info(
            "COM21", "USB Serial Device",
            "USB VID:PID=303A:1001 SER=00:00:00:00:00:01")))

    def test_vid_pid_fallback_only_when_no_serial(self) -> None:
        """The main board's CH340K reports no serial at all (verbatim bench
        hwid, 2026-09-09) -- VID:PID is the only handle it has, and this is
        the sole case where the fallback fires."""
        info = self._info("COM14", "USB-SERIAL CH340K (COM14)",
                          "USB VID:PID=1A86:7522 LOCATION=1-2.4.4.4")
        self.assertIsNone(serial_link.hwid_serial(info.hwid))
        self.assertEqual(serial_link.identify_port(info), "main_board")
        self.assertTrue(serial_link.is_main_board_port(info))
        self.assertFalse(serial_link.is_fixture_port(info))

    def test_hwid_serial_absent_vs_present(self) -> None:
        self.assertIsNone(serial_link.hwid_serial("USB VID:PID=1A86:7522"))
        self.assertIsNone(serial_link.hwid_serial(""))
        self.assertEqual(
            serial_link.hwid_serial("USB VID:PID=1A86:55D3 SER=552E006806"),
            "552E006806")

    def test_unknown_device_identifies_as_neither(self) -> None:
        self.assertIsNone(serial_link.identify_port(self._info(
            "COM1", "Communications Port (COM1)", r"ACPI\PNP0501\0")))


if __name__ == "__main__":
    unittest.main()
