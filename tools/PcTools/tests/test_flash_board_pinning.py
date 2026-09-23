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

    def test_lowercase_enumerated_serial_still_matches(self) -> None:
        """The hwid SER= value and the pinned constant must compare
        case-insensitively -- Windows' USB stack is not guaranteed to report
        a given adapter's serial with consistent casing, and the pinned
        constants in this codebase use uppercase hex."""
        lowercase_main_board = _port("COM3", "USB VID:PID=303A:1001 SER=1c:db:d4:92:f4:7c")
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[lowercase_main_board]):
            result = mf._refuse_if_adapter_absent(mf.MAIN_BOARD_JTAG_SERIAL, "main board")
        self.assertIsNone(result)

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

        # M1 (2026-09-15 review): flash_provenance.json's path is now
        # elf_archive.kiln_provenance_path(), guarded the same way as the ELF
        # archive dirs -- unpatched under pytest this raises. Point it at a
        # harmless fake path (write_provenance_json above is mocked anyway).
        self._provenance_path_patch = unittest.mock.patch.object(
            mf.elf_archive, "kiln_provenance_path", return_value="FAKE-flash_provenance.json"
        )
        self._provenance_path_patch.start()
        self.addCleanup(self._provenance_path_patch.stop)

        self._isfile_patch = unittest.mock.patch.object(mf.os.path, "isfile", return_value=True)
        self._isfile_patch.start()
        self.addCleanup(self._isfile_patch.stop)

        # No real build/KilnCtrl.bin exists on disk for these tests
        # (isfile is faked above) -- fake a size under the real `app`
        # partition's 0x800000 B so the pre-flight size check is a no-op.
        self._getsize_patch = unittest.mock.patch.object(mf.os.path, "getsize", return_value=1024)
        self._getsize_patch.start()
        self.addCleanup(self._getsize_patch.stop)

        self._preflash_patch = unittest.mock.patch.object(mf, "_preflash_board_address", return_value=None)
        self._preflash_patch.start()
        self.addCleanup(self._preflash_patch.stop)

        # 2026-09-10: this class drives flash_firmware() end-to-end, which
        # (via _archive_flashed_elf) calls elf_archive.archive_kiln_elf()
        # against the CANONICAL (main-tree) archive -- unpatched, this ran
        # for real and overwrote a genuine manifest entry with this test's
        # fabricated "abc1234" commit (see elf_archive.py's own guard,
        # added because of this exact incident). Patch it out explicitly,
        # in addition to that guard.
        self._archive_patch = unittest.mock.patch.object(mf.elf_archive, "archive_kiln_elf")
        self._archive_patch.start()
        self.addCleanup(self._archive_patch.stop)

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


class FlashFirmwareSizePreflightTest(FlashFirmwareAdapterPinningTest):
    """RELEASE_HARDENING_PLAN.md section 6: the size preflight in
    flash_firmware() (mcp_server_flash.py, `if app_bin_size > app_target.size`)
    refuses before OpenOCD is touched when build/KilnCtrl.bin does not fit the
    resolved `app` partition -- the guard that would have caught the
    2026-09-17 incident's overflow into `coredump` before a single byte was
    written, if it had existed then.

    Every OTHER test in this file (and FlashFirmwareAdapterPinningTest's own
    setUp, which this class inherits) deliberately fakes getsize() to return
    1024 bytes specifically so this check is a no-op and does not interfere
    with what they're testing -- which meant, until this class, the refusal
    branch itself was never exercised by anything. Negative-tested manually
    2026-09-17 (see RELEASE_HARDENING_PLAN.md section 6's status line): the
    guard was disabled by commenting out the `if` fast-fail, the test below
    failed as expected (result did not start with "error:", OpenOCD WAS
    called), the file was restored by hand via Edit, `git diff` and
    `git hash-object` were confirmed to match, and the full test-file build
    directory was removed before re-measuring."""

    _APP_PARTITION_SIZE = 0x800000  # firmware/KilnFW/partitions.csv: app,ota_0,0x210000,0x800000

    def test_refuses_before_touching_openocd_when_bin_exceeds_partition(self) -> None:
        oversized = self._APP_PARTITION_SIZE + 1
        with unittest.mock.patch.object(mf.os.path, "getsize", return_value=oversized), \
             unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG]):
            result = mf.flash_firmware(verify=False)
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn(str(oversized), result)
        self.assertIn(str(self._APP_PARTITION_SIZE), result)
        self.run_mock.assert_not_called()

    def test_exactly_at_the_boundary_is_not_refused(self) -> None:
        """size == partition size must be allowed -- only strictly greater
        is a refusal (mirrors the production `>` in mcp_server_flash.py)."""
        with unittest.mock.patch.object(mf.os.path, "getsize", return_value=self._APP_PARTITION_SIZE), \
             unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_MAIN_BOARD_JTAG]):
            result = mf.flash_firmware(verify=False)
        self.assertIn("flashed and verified OK", result)
        self.run_mock.assert_called_once()


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

    def test_refuses_kiln_fw_build_path_by_default(self) -> None:
        """Reviewer finding: fixture_flash() accepted any path, including
        the MAIN board's own KilnCtrl.bin -- both boards are plain
        ESP32-S3s, so that would flash and 'succeed' while silently putting
        the wrong firmware on the fixture."""
        kiln_ctrl_bin = os.path.join(mf._kiln_fw_root(), "build", "KilnCtrl.bin")
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.fixture_flash(app_bin=kiln_ctrl_bin)
        self.assertTrue(result.startswith("error:"))
        self.assertIn("MAIN board", result)
        self.run_mock.assert_not_called()

    def test_refuses_kiln_fw_build_path_even_via_relative_dotdot(self) -> None:
        """Resolved via realpath, so `..` segments can't walk around the
        check."""
        sneaky = os.path.join(
            mf._kiln_fw_root(), "build", "..", "build", "KilnCtrl.bin"
        )
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.fixture_flash(app_bin=sneaky)
        self.assertTrue(result.startswith("error:"))
        self.run_mock.assert_not_called()

    def test_allow_cross_board_path_overrides_the_refusal(self) -> None:
        kiln_ctrl_bin = os.path.join(mf._kiln_fw_root(), "build", "KilnCtrl.bin")
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.fixture_flash(app_bin=kiln_ctrl_bin, allow_cross_board_path=True)
        self.assertIn("flashed and verified OK", result)
        self.run_mock.assert_called_once()

    def test_unit_test_fw_build_path_is_not_refused(self) -> None:
        """A path under the fixture's OWN build directory must never trip
        the KilnFW-path guard."""
        own_bin = os.path.join(mf._unit_test_fixture_fw_root(), "build", "App.bin")
        with unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG]):
            result = mf.fixture_flash(app_bin=own_bin)
        self.assertIn("flashed and verified OK", result)


class FixtureFlashPartitionTableOffsetTest(unittest.TestCase):
    """fixture_flash()'s partition_table_bin offset used to be hardcoded to
    0x8000 regardless of the fixture project's own sdkconfig -- the same
    defect class flash_firmware() had for the main board. Both now go
    through the same _resolve_partition_table_offset() helper so they can't
    drift independently; this covers fixture_flash()'s call site."""

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

        self._ports_patch = unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[_FIXTURE_JTAG])
        self._ports_patch.start()
        self.addCleanup(self._ports_patch.stop)

    def test_uses_resolved_offset_from_helper(self) -> None:
        with unittest.mock.patch.object(mf, "_resolve_partition_table_offset", return_value=(0x10000, "")) as resolve_mock:
            result = mf.fixture_flash(partition_table_bin="fake/partition-table.bin")
        self.assertIn("flashed and verified OK", result)
        resolve_mock.assert_called_once_with(mf._unit_test_fixture_fw_root())
        _openocd_exe, _board_cfg, tcl = self.run_mock.call_args.args[:3]
        self.assertIn('partition-table.bin" 0x10000 verify', tcl)
        self.assertNotIn("0x8000", tcl)

    def test_falls_back_to_default_when_helper_reports_default(self) -> None:
        with unittest.mock.patch.object(
            mf, "_resolve_partition_table_offset",
            return_value=(mf.DEFAULT_PARTITION_TABLE_OFFSET, "note: no sdkconfig"),
        ):
            result = mf.fixture_flash(partition_table_bin="fake/partition-table.bin")
        self.assertIn("flashed and verified OK", result)
        _openocd_exe, _board_cfg, tcl = self.run_mock.call_args.args[:3]
        self.assertIn('partition-table.bin" 0x8000 verify', tcl)

    def test_unparsable_offset_refuses_before_openocd(self) -> None:
        with unittest.mock.patch.object(
            mf, "_resolve_partition_table_offset",
            side_effect=ValueError("bad CONFIG_PARTITION_TABLE_OFFSET"),
        ):
            result = mf.fixture_flash(partition_table_bin="fake/partition-table.bin")
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("bad CONFIG_PARTITION_TABLE_OFFSET", result)
        self.run_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
