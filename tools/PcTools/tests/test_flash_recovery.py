#!/usr/bin/env python3
"""Tests for flash_recovery() (mcp_server_flash.py) and recovery_flash.py.

No board, no real OpenOCD: `_run_openocd` is a recording fake, the adapter
and board-state probes are patched, and the provenance/archive paths are
redirected into a temp tree (elf_archive._repo_root).

Run with: python -m pytest tools/PcTools/tests/test_flash_recovery.py -q
"""
from __future__ import annotations

import json
import os
import re
import struct
import sys
import tempfile
import types
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import elf_archive, esp_app_desc, flash_provenance, recovery_flash  # noqa: E402
from kilnctrl import mcp_server_flash as mf  # noqa: E402

# Distinct, non-default offsets so a hardcoded value or a mixed-up row cannot pass.
_CSV = """\
# Name, Type, SubType, Offset, Size
nvs,       data, nvs,     0x9000,   0x6000,
otadata,   data, ota,     0x200000, 0x2000,
app,       app,  ota_0,   0x210000, 0x800000,
recovery,  app,  factory, 0xA10000, 0x1000,
coredump,  data, coredump,0xBF0000, 0x100000,
"""
_RECOVERY = (0xA10000, 0x1000)
_APP = (0x210000, 0x800000)


def _image(project: str = "recovery", magic: int = 0xE9, chip_id: int = 0x0009,
           desc_magic: int = esp_app_desc.ESP_APP_DESC_MAGIC_WORD, size: int = 0x400) -> bytes:
    data = bytearray(b"\x00" * size)
    data[0] = magic
    struct.pack_into("<H", data, 12, chip_id)
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, desc_magic)
    desc[16:16 + 5] = b"v0.1\x00"[:5]
    desc[48:48 + len(project)] = project.encode()
    desc[80:80 + 8] = b"20:13:41"
    desc[96:96 + 11] = b"Sep  3 2026"
    data[0x20:0x20 + 256] = desc
    return bytes(data)


def _tree_state():
    return flash_provenance.TreeState(
        timestamp="t", head="deadbeef", dirty_files=["a.c"], sensitive_files=[], git_available=True)


class RecoveryFlashBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="recflash_test_")
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        self.fw_root = os.path.join(self.tmp, "firmware", "KilnFW")
        os.makedirs(self.fw_root)
        with open(os.path.join(self.fw_root, "partitions.csv"), "w") as f:
            f.write(_CSV)
        self.build = os.path.join(self.tmp, "firmware", "KilnFW_recovery", "build")
        os.makedirs(self.build)
        self.bin = os.path.join(self.build, "recovery.bin")
        self.write_bin(_image())
        self.tcl_calls: "list[str]" = []
        self.board_refusals: "list[str]" = []

        def fake_run(exe, cfg, tcl, cwd, timeout_s):
            self.tcl_calls.append(tcl)
            return True, "ok"

        self.fake_run = fake_run
        for p in (
            unittest.mock.patch.object(mf, "_find_openocd_exe", return_value="fake-openocd.exe"),
            unittest.mock.patch.object(mf, "kill_openocd_sessions", return_value=""),
            unittest.mock.patch.object(mf, "_refuse_if_adapter_absent", return_value=None),
            unittest.mock.patch.object(mf, "_run_openocd", side_effect=lambda *a, **k: self.fake_run(*a, **k)),
            unittest.mock.patch.object(mf, "_recovery_board_state_refusals",
                                       side_effect=lambda host: list(self.board_refusals)),
            unittest.mock.patch.object(flash_provenance, "capture_tree_state", return_value=_tree_state()),
            unittest.mock.patch.object(elf_archive, "_repo_root", return_value=self.tmp),
        ):
            p.start()
            self.addCleanup(p.stop)

    def write_bin(self, data: bytes) -> None:
        with open(self.bin, "wb") as f:
            f.write(data)

    def call(self, **kw) -> str:
        kw.setdefault("kiln_fw_root", self.fw_root)
        kw.setdefault("confirm", True)
        return mf.flash_recovery(**kw)


class ResolutionTest(RecoveryFlashBase):
    def test_offset_and_size_come_from_the_recovery_row(self):
        t = recovery_flash.resolve_recovery_target(self.fw_root)
        self.assertEqual((t.offset, t.size), _RECOVERY)

    def test_default_bin_is_sibling_recovery_tree(self):
        self.assertEqual(os.path.normpath(recovery_flash.default_recovery_bin(self.fw_root)),
                         os.path.normpath(self.bin))

    def test_missing_recovery_row_refuses(self):
        with open(os.path.join(self.fw_root, "partitions.csv"), "w") as f:
            f.write(_CSV.replace("recovery,", "other,   "))
        with self.assertRaisesRegex(recovery_flash.RecoveryFlashRefusal, "no partition named 'recovery'"):
            recovery_flash.resolve_recovery_target(self.fw_root)

    def test_real_repo_table_resolves(self):
        real = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW"))
        if not os.path.isfile(os.path.join(real, "partitions.csv")):
            self.skipTest("repo partitions.csv not present")
        t = recovery_flash.resolve_recovery_target(real)
        self.assertEqual(t.name, "recovery")
        self.assertGreater(t.size, 0)


class RefusalTest(RecoveryFlashBase):
    def assertRefused(self, out: str, needle: str) -> None:
        self.assertTrue(out.startswith("error:"), out)
        self.assertIn(needle, out)
        self.assertEqual(self.tcl_calls, [], "OpenOCD must not be reached on a refusal")

    def test_confirm_not_true(self):
        for bad in (False, 1, "yes", None):
            self.assertRefused(self.call(confirm=bad), "confirm=True")

    def test_missing_file(self):
        os.remove(self.bin)
        self.assertRefused(self.call(), "not found")

    def test_empty_file(self):
        self.write_bin(b"")
        self.assertRefused(self.call(), "empty")

    def test_too_large(self):
        self.write_bin(_image(size=_RECOVERY[1] + 1))
        self.assertRefused(self.call(), "does not fit")

    def test_exactly_partition_size_is_accepted(self):
        self.write_bin(_image(size=_RECOVERY[1]))
        self.assertIn("WRITTEN", self.call())

    def test_bad_magic(self):
        self.write_bin(_image(magic=0xAA))
        self.assertRefused(self.call(), "magic")

    def test_wrong_chip_id(self):
        self.write_bin(_image(chip_id=0x0000))
        self.assertRefused(self.call(), "chip id")

    def test_missing_app_desc(self):
        self.write_bin(_image(desc_magic=0))
        self.assertRefused(self.call(), "esp_app_desc_t")

    def test_wrong_project_name(self):
        self.write_bin(_image(project="KilnCtrl"))
        self.assertRefused(self.call(), "not the recovery project")

    def test_adapter_absent(self):
        with unittest.mock.patch.object(mf, "_refuse_if_adapter_absent", return_value="error: adapter gone"):
            self.assertRefused(self.call(), "adapter gone")

    def test_board_state_refusal(self):
        self.board_refusals = ["a profile is running or paused"]
        self.assertRefused(self.call(), "a profile is running")

    def test_skip_board_state_check(self):
        self.board_refusals = ["a profile is running or paused"]
        self.assertIn("WRITTEN", self.call(skip_board_state_check=True))

    def test_relative_root(self):
        self.assertIn("absolute", mf.flash_recovery(kiln_fw_root="rel", confirm=True))


class BoardStateTest(unittest.TestCase):
    def _pf(self, armed, running, name="idle"):
        return types.SimpleNamespace(safety_armed=armed, profile_running_or_paused=running,
                                     profile_state_name=name)

    def test_profile_running_refuses(self):
        r = recovery_flash.board_state_refusals(self._pf(False, True, "running"), lambda: [])
        self.assertEqual(len(r), 1)

    def test_unreadable_refuses(self):
        self.assertTrue(recovery_flash.board_state_refusals(self._pf(None, None), lambda: []))

    def test_armed_idle_is_ok(self):
        self.assertEqual(recovery_flash.board_state_refusals(self._pf(True, False), lambda: []), [])

    def test_armed_with_firing_evidence_refuses(self):
        r = recovery_flash.board_state_refusals(self._pf(True, False), lambda: ["relay output(s) energized: [1]"])
        self.assertIn("ARMED", r[0])

    def test_armed_conditions_unreadable_refuses(self):
        def boom():
            raise RuntimeError("link down")
        self.assertTrue(recovery_flash.board_state_refusals(self._pf(True, False), boom))


class DryRunTest(RecoveryFlashBase):
    def test_dry_run_needs_no_board_and_no_confirm(self):
        with unittest.mock.patch.object(mf, "_find_openocd_exe", side_effect=AssertionError("board path")), \
             unittest.mock.patch.object(mf, "_refuse_if_adapter_absent", side_effect=AssertionError("board path")), \
             unittest.mock.patch.object(mf, "_recovery_board_state_refusals", side_effect=AssertionError("board path")), \
             unittest.mock.patch.object(mf, "kill_openocd_sessions", side_effect=AssertionError("board path")):
            out = mf.flash_recovery(kiln_fw_root=self.fw_root, dry_run=True)
        self.assertIn("dry run", out)
        self.assertIn("0xa10000", out)
        self.assertEqual(self.tcl_calls, [])
        self.assertFalse(os.path.exists(recovery_flash.recovery_provenance_path(self.tmp)))

    def test_dry_run_still_refuses_a_bad_image(self):
        self.write_bin(_image(project="KilnCtrl"))
        out = mf.flash_recovery(kiln_fw_root=self.fw_root, dry_run=True)
        self.assertTrue(out.startswith("error:"))


class WriteScopeTest(RecoveryFlashBase):
    def test_only_the_recovery_range_is_written(self):
        out = self.call()
        self.assertEqual(len(self.tcl_calls), 1)
        tcl = self.tcl_calls[0]
        writes = re.findall(r'program_esp\s+"[^"]+"\s+0x([0-9a-f]+)', tcl)
        self.assertEqual([int(w, 16) for w in writes], [_RECOVERY[0]])
        self.assertEqual(tcl.count("program_esp"), 1)
        for forbidden in ("erase", "otadata", "bootloader", "partition-table", "KilnCtrl"):
            self.assertNotIn(forbidden, tcl)
        # the image fits wholly inside the recovery range
        self.assertLessEqual(os.path.getsize(self.bin), _RECOVERY[1])
        self.assertIn(mf.MAIN_BOARD_JTAG_SERIAL, tcl)
        self.assertIn("verify", tcl)
        self.assertIn("NOT BOOTED", out)
        self.assertIn("READ-BACK-VERIFIED", out)

    def test_write_offset_follows_a_changed_csv(self):
        with open(os.path.join(self.fw_root, "partitions.csv"), "w") as f:
            f.write(_CSV.replace("0xA10000", "0xB00000"))
        self.call()
        self.assertIn("0xb00000", self.tcl_calls[0])

    def test_retry_once_then_failure_recorded(self):
        calls = []

        def failing(exe, cfg, tcl, cwd, timeout_s):
            calls.append(tcl)
            return False, "Verify Failed"
        self.fake_run = failing
        out = self.call()
        self.assertTrue(out.startswith("error: recovery flash failed twice"))
        self.assertEqual(len(calls), 2)
        prov = json.load(open(recovery_flash.recovery_provenance_path(self.tmp)))
        self.assertEqual(prov["outcome"], "flash_failed")


class ProvenanceTest(RecoveryFlashBase):
    def test_provenance_and_elf_archive(self):
        with open(os.path.join(self.build, "recovery.elf"), "wb") as f:
            f.write(b"ELFDATA" * 10)
        out = self.call()
        prov = json.load(open(recovery_flash.recovery_provenance_path(self.tmp)))
        self.assertEqual(prov["outcome"], "flashed_ok")
        self.assertEqual(prov["image_size"], os.path.getsize(self.bin))
        self.assertEqual(len(prov["image_sha256"]), 64)
        self.assertEqual(prov["app_desc_project_name"], "recovery")
        self.assertEqual(prov["app_desc_build_timestamp"], "Sep  3 2026 20:13:41")
        self.assertEqual(prov["head"], "deadbeef")
        self.assertEqual(prov["dirty_files"], ["a.c"])
        self.assertFalse(prov["booted_and_verified"])
        self.assertEqual(prov["partition"]["offset"], _RECOVERY[0])
        archived = prov["elf_archived"]
        self.assertTrue(archived and os.path.isfile(archived))
        self.assertIn("elf archived", out)
        manifest = json.load(open(os.path.join(recovery_flash.recovery_archive_dir(self.tmp), "manifest.json")))
        self.assertEqual(list(manifest.values())[0]["source"], "flash_recovery")

    def test_no_elf_is_reported_not_fatal(self):
        out = self.call()
        self.assertIn("nothing archived", out)
        self.assertIn("WRITTEN", out)


if __name__ == "__main__":
    unittest.main()
