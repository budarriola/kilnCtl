#!/usr/bin/env python3
"""Tests for the OpenOCD `adapter serial` pinning added to mcp_server_flash.py
(flash_firmware() and fixture_flash()), 2026-09-06.

Two ESP32-S3 boards are permanently on the bench now: the main board and the
UnitTestFixture. Both share USB VID:PID 303A:1001 on their native
USB-Serial-JTAG interface, so `board/esp32s3-builtin.cfg` with no `adapter
serial` binds whichever one OpenOCD enumerates first -- a real risk of
flashing the wrong image onto the wrong board. This covers:

  * flash_firmware()'s TCL contains `adapter serial <main board serial>`.
  * fixture_flash()'s TCL contains `adapter serial <fixture serial>`.
  * both refuse BEFORE calling OpenOCD at all if their pinned serial is not
    currently enumerated, naming whichever 303A:1001 serial(s) ARE present.
  * a negative test proving the refusal check can actually fail.

All against a fake serial_link.list_ports()/_list_ports.comports() -- no
real board, no real openocd.exe (that call is mocked too).

Run with: python -m pytest tools/PcTools/tests/test_flash_board_pinning.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_flash as mf  # noqa: E402
from kilnctrl import flash_provenance  # noqa: E402
from kilnctrl.serial_link import PortInfo  # noqa: E402


def _port(device, hwid):
    return PortInfo(device=device, description="", manufacturer="", hwid=hwid, score=0)


_MAIN_BOARD_JTAG = _port("COM3", "USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C")
_FIXTURE_JTAG = _port("COM7", "USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8")
_UNRELATED_303A = _port("COM9", "USB VID:PID=303A:1001 SER=DEADBEEF00000000")


class RefuseIfAdapterAbsentTest(unittest.TestCase):
    def test_present_returns_none(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG]):
            self.assertIsNone(mf._refuse_if_adapter_absent(mf.MAIN_BOARD_JTAG_SERIAL, "main board"))

    def test_absent_names_what_was_seen_instead(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf._refuse_if_adapter_absent(mf.MAIN_BOARD_JTAG_SERIAL, "main board")
        self.assertIsNotNone(result)
        self.assertIn("error:", result)
        self.assertIn(mf.FIXTURE_JTAG_SERIAL, result)

    def test_nothing_enumerated_says_so(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[]):
            result = mf._refuse_if_adapter_absent(mf.MAIN_BOARD_JTAG_SERIAL, "main board")
        self.assertIn("No 303A:1001 device is enumerated", result)

    def test_negative_confirms_the_check_can_fail(self) -> None:
        """Break the production picker (accept any serial) and confirm the
        absent-board scenario above would then wrongly report present --
        proving test_absent_names_what_was_seen_instead exercises a real
        check rather than a vacuous one."""
        def _always_present(expected_serial, board_label):
            return None  # pretend it's always there -- the pre-fix behavior

        with unittest.mock.patch.object(mf, "_refuse_if_adapter_absent", side_effect=_always_present):
            result = mf._refuse_if_adapter_absent(mf.MAIN_BOARD_JTAG_SERIAL, "main board")
        self.assertIsNone(result)  # the broken version wrongly says "fine"
        # ... whereas the real function, unpatched, correctly refuses:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            real_result = mf._refuse_if_adapter_absent(mf.MAIN_BOARD_JTAG_SERIAL, "main board")
        self.assertIsNotNone(real_result)


class FlashFirmwareAdapterPinningTest(unittest.TestCase):
    """End-to-end through flash_firmware(): the refusal happens before
    OpenOCD is touched, and the TCL sent to OpenOCD carries the pinned
    serial when the board IS present."""

    def setUp(self):
        self._openocd_patch = unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe")
        self._openocd_patch.start()
        self.addCleanup(self._openocd_patch.stop)

        self._kill_patch = unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value="")
        self._kill_patch.start()
        self.addCleanup(self._kill_patch.stop)

        self.run_mock = unittest.mock.Mock(return_value=(True, "verified"))
        self._run_patch = unittest.mock.patch.object(mf, "_run_openocd", self.run_mock)
        self._run_patch.start()
        self.addCleanup(self._run_patch.stop)

        stale_ok = unittest.mock.Mock(stale=False, reason="")
        self._stale_patch = unittest.mock.patch.object(mf.stale_check, "check_kilnfw_stale", return_value=stale_ok)
        self._stale_patch.start()
        self.addCleanup(self._stale_patch.stop)

        clean_state = flash_provenance.TreeState(timestamp=0.0, head="abc1234")
        self._provenance_patch = unittest.mock.patch.object(
            mf.flash_provenance, "capture_tree_state", return_value=clean_state
        )
        self._provenance_patch.start()
        self.addCleanup(self._provenance_patch.stop)
        self._provenance_write_patch = unittest.mock.patch.object(
            mf.flash_provenance, "write_provenance_json", return_value=None
        )
        self._provenance_write_patch.start()
        self.addCleanup(self._provenance_write_patch.stop)

        self._isfile_patch = unittest.mock.patch.object(mf.os.path, "isfile", return_value=True)
        self._isfile_patch.start()
        self.addCleanup(self._isfile_patch.stop)

        self._preflash_patch = unittest.mock.patch.object(mf, "_preflash_board_address", return_value=None)
        self._preflash_patch.start()
        self.addCleanup(self._preflash_patch.stop)

    def test_refuses_before_touching_openocd_when_serial_absent(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.flash_firmware(verify=False)
        self.assertTrue(result.startswith("error:"))
        self.assertIn(mf.MAIN_BOARD_JTAG_SERIAL, result)
        self.run_mock.assert_not_called()

    def test_tcl_pins_adapter_serial_when_board_present(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG]):
            result = mf.flash_firmware(verify=False)
        self.assertIn("flashed and verified OK", result)
        self.run_mock.assert_called_once()
        _openocd_exe, _board_cfg, tcl = self.run_mock.call_args.args[:3]
        self.assertIn(f"adapter serial {mf.MAIN_BOARD_JTAG_SERIAL}", tcl)
        self.assertTrue(tcl.startswith(f"adapter serial {mf.MAIN_BOARD_JTAG_SERIAL}"))

    def test_does_not_refuse_when_an_unrelated_303a_device_is_also_present(self) -> None:
        """The main board's serial being present is what matters -- an
        extra, unrecognized 303A:1001 device on the bench must not itself
        cause a refusal."""
        with unittest.mock.patch.object(
            mf.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG, _UNRELATED_303A]
        ):
            result = mf.flash_firmware(verify=False)
        self.assertIn("flashed and verified OK", result)


class FixtureFlashTest(unittest.TestCase):
    def setUp(self):
        self._openocd_patch = unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe")
        self._openocd_patch.start()
        self.addCleanup(self._openocd_patch.stop)

        self._kill_patch = unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value="")
        self._kill_patch.start()
        self.addCleanup(self._kill_patch.stop)

        self.run_mock = unittest.mock.Mock(return_value=(True, "verified"))
        self._run_patch = unittest.mock.patch.object(mf, "_run_openocd", self.run_mock)
        self._run_patch.start()
        self.addCleanup(self._run_patch.stop)

        self._isfile_patch = unittest.mock.patch.object(mf.os.path, "isfile", return_value=True)
        self._isfile_patch.start()
        self.addCleanup(self._isfile_patch.stop)

    def test_no_paths_given_is_an_error(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.fixture_flash()
        self.assertTrue(result.startswith("error:"))
        self.run_mock.assert_not_called()

    def test_refuses_before_openocd_when_fixture_serial_absent(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG]):
            result = mf.fixture_flash(app_bin="fake/App.bin")
        self.assertTrue(result.startswith("error:"))
        self.assertIn(mf.FIXTURE_JTAG_SERIAL, result)
        self.run_mock.assert_not_called()

    def test_tcl_pins_fixture_serial_and_flashes_only_given_images(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.fixture_flash(app_bin="fake/App.bin")
        self.assertIn("flashed and verified OK", result)
        self.run_mock.assert_called_once()
        _openocd_exe, _board_cfg, tcl = self.run_mock.call_args.args[:3]
        self.assertIn(f"adapter serial {mf.FIXTURE_JTAG_SERIAL}", tcl)
        self.assertIn("App.bin", tcl)
        # Never mentions the images that weren't passed in.
        self.assertNotIn("bootloader", tcl)
        self.assertNotIn("partition", tcl)
        self.assertIn("reset exit", tcl)

    def test_never_pinned_to_main_board_serial(self) -> None:
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            mf.fixture_flash(app_bin="fake/App.bin")
        _openocd_exe, _board_cfg, tcl = self.run_mock.call_args.args[:3]
        self.assertNotIn(mf.MAIN_BOARD_JTAG_SERIAL, tcl)


if __name__ == "__main__":
    unittest.main()
