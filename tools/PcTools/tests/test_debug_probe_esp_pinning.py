#!/usr/bin/env python3
"""Tests for PEER_ESP's `adapter serial` pinning in debug_probe.py, added
2026-09-06 after a code review caught PEER_ESP still had `adapter_serial=None`
while a parallel fix pinned mcp_server_flash.py's flash_firmware(). Without
this, debug_reset/halt/resume/step/read_memory/write_memory/read_symbol/
read_registers (all routed through debug_probe._run()) would bind whichever
303A:1001 (ESP32-S3 native USB-Serial-JTAG) unit OpenOCD enumerates first --
two such boards (the main board and the UnitTestFixture) are on the bench,
so `debug_reset(peer="esp")` could reset the wrong one.

All against a fake serial_link.list_ports() and a mocked
openocd_util.run_openocd() -- no real OpenOCD, no live board.

Run with: python -m pytest tools/PcTools/tests/test_debug_probe_esp_pinning.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe  # noqa: E402
from kilnctrl.serial_link import PortInfo  # noqa: E402


def _port(device, hwid):
    return PortInfo(device=device, description="", manufacturer="", hwid=hwid, score=0)


_MAIN_BOARD_JTAG = _port("COM3", "USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C")
_FIXTURE_JTAG = _port("COM7", "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8")


class PeerEspAdapterSerialTest(unittest.TestCase):
    def test_peer_esp_is_pinned_to_the_main_board_serial(self) -> None:
        """The blocker itself: PEER_ESP must carry a non-None adapter_serial
        equal to the shared serial_link constant -- not a second literal."""
        esp_cfg = debug_probe.resolve_peer(debug_probe.PEER_ESP)
        self.assertEqual(esp_cfg.adapter_serial, debug_probe.serial_link.MAIN_BOARD_JTAG_SERIAL)
        self.assertIsNotNone(esp_cfg.adapter_serial)


class RunRefusalTest(unittest.TestCase):
    """Exercises debug_probe._run() -- the single choke point every ESP-peer
    command (program/reset/halt/resume/step/read_memory/write_memory/
    read_symbol/read_registers) goes through."""

    def setUp(self):
        self.run_mock = unittest.mock.Mock(return_value=(True, "ok"))
        self._openocd_run_patch = unittest.mock.patch.object(debug_probe.openocd_util, "run_openocd", self.run_mock)
        self._openocd_run_patch.start()
        self.addCleanup(self._openocd_run_patch.stop)

        self._exe_patch = unittest.mock.patch.object(debug_probe, "_openocd_exe_or_raise", return_value="fake-openocd.exe")
        self._exe_patch.start()
        self.addCleanup(self._exe_patch.stop)

    def test_refuses_before_openocd_when_main_board_absent(self) -> None:
        with unittest.mock.patch.object(debug_probe.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            ok, msg = debug_probe.halt(debug_probe.PEER_ESP)
        self.assertFalse(ok)
        self.assertIn(debug_probe.serial_link.MAIN_BOARD_JTAG_SERIAL, msg)
        self.assertIn(debug_probe.serial_link.FIXTURE_JTAG_SERIAL, msg)
        self.run_mock.assert_not_called()

    def test_command_line_pins_adapter_serial_when_present(self) -> None:
        with unittest.mock.patch.object(debug_probe.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG]):
            ok, _msg = debug_probe.halt(debug_probe.PEER_ESP)
        self.assertTrue(ok)
        self.run_mock.assert_called_once()
        _exe, _cfg_args, tcl_commands = self.run_mock.call_args.args[:3]
        expected = f"adapter serial {debug_probe.serial_link.MAIN_BOARD_JTAG_SERIAL}"
        self.assertIn(expected, tcl_commands)
        self.assertTrue(tcl_commands.startswith(expected))

    def test_pico_peer_is_unaffected_by_esp_serial_check(self) -> None:
        """The refusal is ESP-specific -- PEER_PICO commands must not be
        gated on any 303A:1001 enumeration at all."""
        with unittest.mock.patch.object(debug_probe.serial_link, "list_ports", return_value=[]):
            ok, _msg = debug_probe.halt(debug_probe.PEER_PICO)
        self.assertTrue(ok)
        self.run_mock.assert_called_once()

    def test_negative_confirms_the_refusal_can_fail(self) -> None:
        """Break _refuse_if_esp_adapter_absent() (simulate the pre-fix state:
        PEER_ESP never refuses) and confirm the absent-board scenario above
        would then wrongly proceed to call OpenOCD -- proving
        test_refuses_before_openocd_when_main_board_absent exercises a real
        check rather than a vacuous one."""
        with unittest.mock.patch.object(debug_probe, "_refuse_if_esp_adapter_absent", return_value=None):
            with unittest.mock.patch.object(debug_probe.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
                ok, _msg = debug_probe.halt(debug_probe.PEER_ESP)
        self.assertTrue(ok)  # the broken version wrongly proceeds
        self.run_mock.assert_called_once()


if __name__ == "__main__":
    unittest.main()
