#!/usr/bin/env python3
"""Unit tests for flash_firmware()'s `erase_partitions`/`confirm_erase`
parameters (owner decision 2026-09-21): erasing the DEFAULT `nvs` partition
during a commission reflash, to reset the board's web-auth admin record
(unknown password) -- `web_auth_store.c:18-31`'s `kiln_auth` namespace lives
in that partition, per `partitions.csv`'s `nvs,data,nvs,0x9000,0x6000` row.

No tool previously erased any data partition -- a hand
`flash erase_sector 0 0 last` once wiped the WHOLE chip (see
mcp_server_flash.py's module header comment). This parameter is deliberately
narrow: an explicit allowlist of DATA partitions only (never
app/recovery/otadata/bootloader/partition-table/coredump), resolved fresh
from partitions.csv (the same file `_resolve_app_flash_target()` already
reads), written in the SAME OpenOCD session as the app image, after it and
before the final reset -- never a bare `flash erase_sector`.

All against MOCKED `_run_openocd`/`_find_openocd_exe`/`kill_openocd_sessions`
and a synthetic partitions.csv/build dir -- no real socket, no live board, no
OpenOCD/JTAG involved anywhere in this file.

Run with: python -m pytest tools/PcTools/tests/test_flash_firmware_erase_partitions.py -q
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_flash as mf  # noqa: E402
from kilnctrl import partition_table  # noqa: E402


# The real single-slot table's data partitions (docs/OTA_SINGLE_SLOT.md /
# CLAUDE.md's nvs erase note), trimmed to what these tests need.
_PARTITIONS_CSV = (
    "# Name,        Type, SubType,   Offset,    Size\n"
    "nvs,           data, nvs,       0x9000,    0x6000,\n"
    "phy_init,      data, phy,       0xf000,    0x1000,\n"
    "wifi_nvs,      data, nvs,       0x187000,  0x6000,\n"
    "kiln_nvs,      data, nvs,       0x18D000,  0x10000,\n"
    "profiles_nvs,  data, nvs,       0x19D000,  0x60000,\n"
    "otadata,       data, ota,       0x200000,  0x2000,\n"
    "app,           app,  ota_0,     0x210000,  0x800000,\n"
    "recovery,      app,  factory,   0xA10000,  0x1E0000,\n"
    "coredump,      data, coredump,  0xBF0000,  0x100000,\n"
    "cfg,           data, littlefs,  0xDB0000,  0x250000,\n"
)


class TestResolveEraseTargets(unittest.TestCase):
    """_resolve_erase_targets() -- the allowlist + CSV-resolution guard,
    tested directly, no OpenOCD involved at all."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        with open(os.path.join(self.tmp, "partitions.csv"), "w", encoding="utf-8") as f:
            f.write(_PARTITIONS_CSV)

    def test_resolves_nvs_from_csv(self):
        targets = mf._resolve_erase_targets(self.tmp, ["nvs"])
        self.assertEqual(len(targets), 1)
        self.assertEqual(targets[0].name, "nvs")
        self.assertEqual(targets[0].offset, 0x9000)
        self.assertEqual(targets[0].size, 0x6000)

    def test_resolves_multiple_allowlisted_partitions(self):
        targets = mf._resolve_erase_targets(self.tmp, ["nvs", "cfg"])
        names = [t.name for t in targets]
        self.assertEqual(names, ["nvs", "cfg"])

    def test_all_allowlisted_names_are_individually_resolvable(self):
        for name in sorted(mf.ERASABLE_DATA_PARTITIONS):
            targets = mf._resolve_erase_targets(self.tmp, [name])
            self.assertEqual(targets[0].name, name)

    def test_app_partition_is_refused(self):
        with self.assertRaises(ValueError) as ctx:
            mf._resolve_erase_targets(self.tmp, ["app"])
        self.assertIn("app", str(ctx.exception))
        self.assertIn("allowlist", str(ctx.exception))

    def test_recovery_partition_is_refused(self):
        with self.assertRaises(ValueError):
            mf._resolve_erase_targets(self.tmp, ["recovery"])

    def test_otadata_is_refused(self):
        with self.assertRaises(ValueError):
            mf._resolve_erase_targets(self.tmp, ["otadata"])

    def test_coredump_is_refused(self):
        with self.assertRaises(ValueError):
            mf._resolve_erase_targets(self.tmp, ["coredump"])

    def test_bootloader_and_partition_table_are_refused(self):
        # Neither is even a named row in partitions.csv (they are implicit /
        # below the table offset), but they must be refused at the allowlist
        # stage regardless -- never treated as a resolvable data partition.
        with self.assertRaises(ValueError):
            mf._resolve_erase_targets(self.tmp, ["bootloader"])
        with self.assertRaises(ValueError):
            mf._resolve_erase_targets(self.tmp, ["partition_table"])

    def test_allowlisted_name_absent_from_csv_is_refused(self):
        # "cfg" is allowlisted in general, but not every board's table has
        # it -- must still refuse (naming what IS in the CSV) rather than
        # silently no-op or guess an offset.
        trimmed_csv = (
            "# Name, Type, SubType, Offset, Size\n"
            "nvs,    data, nvs,     0x9000, 0x6000,\n"
        )
        with open(os.path.join(self.tmp, "partitions.csv"), "w", encoding="utf-8") as f:
            f.write(trimmed_csv)
        with self.assertRaises(ValueError) as ctx:
            mf._resolve_erase_targets(self.tmp, ["cfg"])
        self.assertIn("cfg", str(ctx.exception))

    def test_unknown_garbage_name_is_refused(self):
        with self.assertRaises(ValueError):
            mf._resolve_erase_targets(self.tmp, ["not_a_real_partition"])


class TestWriteBlankPartitionFile(unittest.TestCase):
    def test_file_is_exact_size_and_all_0xff(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        entry = partition_table.PartitionEntry(name="nvs", type=0x01, subtype=0x02, offset=0x9000, size=0x6000)
        path = mf._write_blank_partition_file(entry, tmp)
        data = open(path, "rb").read()
        self.assertEqual(len(data), 0x6000)
        self.assertEqual(data, b"\xff" * 0x6000)

    def test_large_partition_is_exact_size(self):
        """cfg is 0x250000 B (2.4 MB) -- bigger than the chunk size the
        writer uses internally, so this exercises the multi-chunk loop."""
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        entry = partition_table.PartitionEntry(name="cfg", type=0x01, subtype=0x83, offset=0xDB0000, size=0x250000)
        path = mf._write_blank_partition_file(entry, tmp)
        self.assertEqual(os.path.getsize(path), 0x250000)


class EraseFlashFirmwareEndToEndTest(unittest.TestCase):
    """Drives flash_firmware(erase_partitions=..., confirm_erase=...)
    end-to-end with a mocked OpenOCD runner, proving: the confirm gate, the
    allowlist refusal happens before OpenOCD, and the TCL command list
    contains the erase program_esp line(s) in the right place (after the app
    image, before `reset exit`)."""

    def setUp(self):
        self.tmp_root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp_root, ignore_errors=True)
        self.kiln_fw_root = os.path.join(self.tmp_root, "firmware", "KilnFW")
        self.build_dir = os.path.join(self.kiln_fw_root, "build")
        os.makedirs(os.path.join(self.build_dir, "bootloader"))
        os.makedirs(os.path.join(self.build_dir, "partition_table"))
        with open(os.path.join(self.build_dir, "bootloader", "bootloader.bin"), "wb") as f:
            f.write(b"\x00")
        with open(os.path.join(self.build_dir, "partition_table", "partition-table.bin"), "wb") as f:
            f.write(b"\x00")
        with open(os.path.join(self.build_dir, "KilnCtrl.bin"), "wb") as f:
            f.write(b"\x00")
        with open(os.path.join(self.kiln_fw_root, "partitions.csv"), "w", encoding="utf-8") as f:
            f.write(_PARTITIONS_CSV)

        self._openocd_patch = unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe")
        self._openocd_patch.start()
        self.addCleanup(self._openocd_patch.stop)

        self._kill_patch = unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value="")
        self._kill_patch.start()
        self.addCleanup(self._kill_patch.stop)

        self._run_patch = unittest.mock.patch.object(mf, "_run_openocd", return_value=(True, "verified"))
        self.run_mock = self._run_patch.start()
        self.addCleanup(self._run_patch.stop)

        stale_ok = unittest.mock.Mock(stale=False, reason="")
        self._stale_patch = unittest.mock.patch.object(mf.stale_check, "check_kilnfw_stale", return_value=stale_ok)
        self._stale_patch.start()
        self.addCleanup(self._stale_patch.stop)

        self._adapter_patch = unittest.mock.patch.object(mf, "_refuse_if_adapter_absent", return_value=None)
        self._adapter_patch.start()
        self.addCleanup(self._adapter_patch.stop)

        self._preflash_patch = unittest.mock.patch.object(mf, "_preflash_board_address", return_value=None)
        self._preflash_patch.start()
        self.addCleanup(self._preflash_patch.stop)

        self._archive_patch = unittest.mock.patch.object(mf.elf_archive, "archive_kiln_elf")
        self._archive_patch.start()
        self.addCleanup(self._archive_patch.stop)

        self.prov_path = os.path.join(self.tmp_root, "flash_provenance.json")
        self._provenance_path_patch = unittest.mock.patch.object(
            mf.elf_archive, "kiln_provenance_path", return_value=self.prov_path
        )
        self._provenance_path_patch.start()
        self.addCleanup(self._provenance_path_patch.stop)

    def test_erase_without_confirm_is_refused_before_openocd(self):
        result = mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False, erase_partitions=["nvs"], confirm_erase=False,
        )
        self.assertTrue(result.startswith("error:"))
        self.assertIn("confirm_erase", result)
        self.assertIn("nvs", result)
        self.run_mock.assert_not_called()

    def test_disallowed_partition_is_refused_before_openocd(self):
        result = mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False, erase_partitions=["app"], confirm_erase=True,
        )
        self.assertTrue(result.startswith("error:"))
        self.assertIn("app", result)
        self.assertIn("allowlist", result)
        self.run_mock.assert_not_called()

    def test_partition_absent_from_csv_is_refused_before_openocd(self):
        result = mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False,
            erase_partitions=["not_a_real_partition"], confirm_erase=True,
        )
        self.assertTrue(result.startswith("error:"))
        self.run_mock.assert_not_called()

    def test_confirmed_nvs_erase_flashes_and_appends_program_esp_in_order(self):
        result = mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False, erase_partitions=["nvs"], confirm_erase=True,
        )
        self.assertIn("flashed and verified OK", result)
        self.run_mock.assert_called_once()
        tcl = self.run_mock.call_args.args[2]
        commands = [c.strip() for c in tcl.split(";")]
        app_idx = next(i for i, c in enumerate(commands) if c.startswith("program_esp build/KilnCtrl.bin"))
        erase_idx = next(i for i, c in enumerate(commands) if "erase_nvs.bin" in c)
        reset_idx = next(i for i, c in enumerate(commands) if c.endswith("reset exit") or "reset exit" in c)
        # erase comes strictly after the app image and the SAME command that
        # ends the session (reset exit) is the erase line itself, since it
        # is the last program_esp in the sequence.
        self.assertLess(app_idx, erase_idx)
        self.assertEqual(erase_idx, reset_idx)
        self.assertIn("0x9000", commands[erase_idx])
        self.assertIn("verify", commands[erase_idx])

    def test_multiple_partitions_all_appear_after_app_before_reset(self):
        mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False,
            erase_partitions=["nvs", "kiln_nvs"], confirm_erase=True,
        )
        tcl = self.run_mock.call_args.args[2]
        commands = [c.strip() for c in tcl.split(";")]
        app_idx = next(i for i, c in enumerate(commands) if c.startswith("program_esp build/KilnCtrl.bin"))
        nvs_idx = next(i for i, c in enumerate(commands) if "erase_nvs.bin" in c)
        kiln_nvs_idx = next(i for i, c in enumerate(commands) if "erase_kiln_nvs.bin" in c)
        self.assertLess(app_idx, nvs_idx)
        self.assertLess(nvs_idx, kiln_nvs_idx)
        # Only the very last program_esp ends the session.
        self.assertTrue(commands[-1].startswith("program_esp") and "reset exit" in commands[-1])
        self.assertNotIn("reset exit", commands[nvs_idx])

    def test_result_names_each_erased_partition(self):
        result = mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False, erase_partitions=["nvs"], confirm_erase=True,
        )
        self.assertIn("nvs", result)
        self.assertIn("0x9000", result)
        self.assertIn("0x6000", result)

    def test_provenance_json_records_erased_partitions(self):
        mf.flash_firmware(
            kiln_fw_root=self.kiln_fw_root, verify=False, erase_partitions=["nvs", "cfg"], confirm_erase=True,
        )
        prov = mf.flash_provenance.read_provenance_json(self.prov_path)
        self.assertIsNotNone(prov)
        self.assertEqual(prov["outcome"], mf.flash_provenance.OUTCOME_FLASHED_OK)
        erased = prov["erased_partitions"]
        self.assertEqual([e["name"] for e in erased], ["nvs", "cfg"])
        self.assertEqual(erased[0]["offset"], 0x9000)
        self.assertEqual(erased[0]["size"], 0x6000)

    def test_no_erase_partitions_omits_field_from_provenance(self):
        mf.flash_firmware(kiln_fw_root=self.kiln_fw_root, verify=False)
        prov = mf.flash_provenance.read_provenance_json(self.prov_path)
        self.assertIsNotNone(prov)
        self.assertIsNone(prov["erased_partitions"])
        self.run_mock.assert_called_once()

    def test_blank_files_are_cleaned_up_after_flash(self):
        """The temp dir holding the 0xFF blank files must not be left behind
        on disk after the flash completes."""
        captured_paths = {}

        real_write = mf._write_blank_partition_file

        def _spy(entry, tmp_dir):
            path = real_write(entry, tmp_dir)
            captured_paths[entry.name] = path
            return path

        with unittest.mock.patch.object(mf, "_write_blank_partition_file", side_effect=_spy):
            mf.flash_firmware(
                kiln_fw_root=self.kiln_fw_root, verify=False, erase_partitions=["nvs"], confirm_erase=True,
            )
        self.assertIn("nvs", captured_paths)
        self.assertFalse(os.path.exists(captured_paths["nvs"]))
        self.assertFalse(os.path.exists(os.path.dirname(captured_paths["nvs"])))

    def test_flash_failure_still_cleans_up_and_does_not_write_erased_partitions_as_ok(self):
        with unittest.mock.patch.object(mf, "_run_openocd", return_value=(False, "Verify Failed")) as fail_mock:
            result = mf.flash_firmware(
                kiln_fw_root=self.kiln_fw_root, verify=False, retry_once=False,
                erase_partitions=["nvs"], confirm_erase=True,
            )
        self.assertTrue(result.startswith("error: flash failed"))
        fail_mock.assert_called_once()
        prov = mf.flash_provenance.read_provenance_json(self.prov_path)
        self.assertEqual(prov["outcome"], mf.flash_provenance.OUTCOME_FLASH_FAILED)
        # Still recorded (an attempted, failed erase is on the record too),
        # not silently dropped just because the flash itself failed.
        self.assertEqual([e["name"] for e in prov["erased_partitions"]], ["nvs"])


if __name__ == "__main__":
    unittest.main()
