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

from kilnctrl import elf_archive, esp_app_desc, flash_provenance, partition_http_client, partition_table, recovery_flash  # noqa: E402
from kilnctrl import mcp_server_flash as mf  # noqa: E402

# Captured BEFORE any patching so tests can exercise the real functions.
_REAL_BOARD_STATE = mf._recovery_board_state_refusals
_REAL_PROBE = mf._probe_board_partitions
_REAL_OBSERVE = mf._observe_boot_after_recovery_flash

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
        self.hazards: "list[str]" = []
        self.unreadable: "list[str]" = []
        # The chip's live table = the CSV, board running `app`.
        self.chip_entries = partition_table.parse_partitions_csv(os.path.join(self.fw_root, "partitions.csv"))
        self.probe_result = ("192.168.1.50", self.chip_entries, "app")
        self.boot_report = "app -- the board answered at 192.168.1.50 running 'app'"

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
                                       side_effect=lambda host: (list(self.hazards), list(self.unreadable))),
            unittest.mock.patch.object(mf, "_probe_board_partitions",
                                       side_effect=lambda host: self.probe_result),
            unittest.mock.patch.object(mf, "_observe_boot_after_recovery_flash",
                                       side_effect=lambda host, pre: self.boot_report),
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

    def test_recovery_row_not_factory_subtype_refuses(self):
        with open(os.path.join(self.fw_root, "partitions.csv"), "w") as f:
            f.write(_CSV.replace("recovery,  app,  factory", "recovery,  app,  ota_1  "))
        with self.assertRaisesRegex(recovery_flash.RecoveryFlashRefusal, "not an app/factory"):
            recovery_flash.resolve_recovery_target(self.fw_root)

    def test_duplicate_recovery_row_refuses(self):
        with open(os.path.join(self.fw_root, "partitions.csv"), "a") as f:
            f.write("recovery,  app,  factory, 0xC00000, 0x1000,\n")
        with self.assertRaisesRegex(recovery_flash.RecoveryFlashRefusal, "2 partitions named"):
            recovery_flash.resolve_recovery_target(self.fw_root)
        self.assertTrue(self.call().startswith("error:"))
        self.assertEqual(self.tcl_calls, [])

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

    def test_observed_hazard_refuses_whatever_flags_are_passed(self):
        self.hazards = ["a profile is running or paused"]
        for kw in ({}, {"allow_unreadable_board_state": True},
                   {"allow_unconfirmed_partition_table": True, "allow_reset_into_recovery": True,
                    "allow_unreadable_board_state": True, "allow_stale": True}):
            out = self.call(**kw)
            self.assertRefused(out, "a profile is running")
            self.assertIn("no flag overrides", out)

    def test_hazard_plus_unreadable_still_refuses_with_waiver(self):
        self.hazards = ["safety is ARMED and relay output(s) energized: [1]"]
        self.unreadable = ["profile state could not be read"]
        self.assertRefused(self.call(allow_unreadable_board_state=True), "relay output")

    def test_unreadable_refuses_by_default(self):
        self.unreadable = ["safety link state could not be confirmed"]
        self.assertRefused(self.call(), "could not be read")

    def test_unreadable_is_waivable_only_by_its_own_flag(self):
        self.unreadable = ["safety link state could not be confirmed"]
        for bad in (1, "yes"):
            self.assertRefused(self.call(allow_unreadable_board_state=bad), "could not be read")
        for other in ("allow_unconfirmed_partition_table", "allow_reset_into_recovery", "allow_stale"):
            self.assertRefused(self.call(**{other: True}), "could not be read")
        self.assertIn("WRITTEN", self.call(allow_unreadable_board_state=True))

    def test_skip_board_state_check_is_gone(self):
        self.hazards = ["a profile is running or paused"]
        out = self.call(skip_board_state_check=True)
        self.assertIn("unexpected keyword argument", out)
        self.assertEqual(self.tcl_calls, [])

    def test_tcl_unsafe_path_refuses(self):
        for ch in '[]${}"`':
            out = self.call(recovery_bin=os.path.join(self.build, f"a{ch}b", "recovery.bin"))
            self.assertRefused(out, "unsafe inside")

    def test_backslash_path_is_normalized_not_refused(self):
        recovery_flash.check_tcl_safe_path("C:\\wt\\x\\recovery.bin")
        self.assertNotIn("\\", recovery_flash.build_tcl("S", "C:\\wt\\x\\recovery.bin",
                                                         recovery_flash.resolve_recovery_target(self.fw_root)))

    def test_build_tcl_itself_refuses_unsafe_path(self):
        t = recovery_flash.resolve_recovery_target(self.fw_root)
        with self.assertRaises(recovery_flash.RecoveryFlashRefusal):
            recovery_flash.build_tcl("S", "x[exec calc].bin", t)

    def test_relative_root(self):
        self.assertIn("absolute", mf.flash_recovery(kiln_fw_root="rel", confirm=True))


class BoardStateTest(unittest.TestCase):
    def _pf(self, armed, running, name="idle", interlock=True, link=True):
        return types.SimpleNamespace(safety_armed=armed, profile_running_or_paused=running,
                                     profile_state_name=name, ota_interlock_ok=interlock,
                                     ota_interlock_reason="ok" if interlock else "busy", link_up=link)

    def test_profile_running_is_a_hazard_not_unreadable(self):
        h, u = recovery_flash.board_state_refusals(self._pf(False, True, "running"), lambda: [])
        self.assertEqual((len(h), u), (1, []))

    def test_unreadable_profile_and_armed_are_unreadable_not_hazards(self):
        h, u = recovery_flash.board_state_refusals(self._pf(None, None), lambda: [])
        self.assertEqual(h, [])
        self.assertEqual(len(u), 2)

    def test_armed_idle_is_ok(self):
        self.assertEqual(recovery_flash.board_state_refusals(self._pf(True, False), lambda: []), ([], []))

    def test_armed_with_firing_evidence_is_a_hazard(self):
        h, u = recovery_flash.board_state_refusals(self._pf(True, False), lambda: ["relay output(s) energized: [1]"])
        self.assertIn("ARMED", h[0])
        self.assertEqual(u, [])

    def test_armed_conditions_unreadable_is_unreadable(self):
        def boom():
            raise RuntimeError("link down")
        h, u = recovery_flash.board_state_refusals(self._pf(True, False), boom)
        self.assertEqual(h, [])
        self.assertIn("link down", u[0])

    def test_interlock_busy_is_hazard_unknown_is_unreadable(self):
        h, u = recovery_flash.board_state_refusals(self._pf(False, False, interlock=False), lambda: [])
        self.assertEqual((len(h), u), (1, []))
        h, u = recovery_flash.board_state_refusals(self._pf(False, False, interlock=None), lambda: [])
        self.assertEqual((h, len(u)), ([], 1))

    def test_link_down_is_hazard_unknown_is_unreadable(self):
        h, u = recovery_flash.board_state_refusals(self._pf(False, False, link=False), lambda: [])
        self.assertEqual((len(h), u), (1, []))
        h, u = recovery_flash.board_state_refusals(self._pf(False, False, link=None), lambda: [])
        self.assertEqual((h, len(u)), ([], 1))


class RecoveryBoardStateUnmockedTest(unittest.TestCase):
    """_recovery_board_state_refusals() with fakes only at ITS dependencies."""

    def _run(self, pf=None, pf_exc=None, armed=None, armed_exc=None):
        from kilnctrl import mcp_server_coordinated_gpio_test as gt
        from kilnctrl import mcp_server_ota_matrix as om

        def fake_pf(h):
            if pf_exc:
                raise pf_exc
            return pf

        def fake_armed():
            if armed_exc:
                raise armed_exc
            return armed or []

        with unittest.mock.patch.object(gt, "_gpio_test_preflight", side_effect=fake_pf), \
             unittest.mock.patch.object(gt, "_gpio_test_resolve_host", return_value="h"), \
             unittest.mock.patch.object(om, "_read_armed_latch_conditions", side_effect=fake_armed):
            return _REAL_BOARD_STATE("h")

    def _pf(self, **kw):
        d = dict(safety_armed=False, profile_running_or_paused=False, profile_state_name="idle",
                 ota_interlock_ok=True, ota_interlock_reason="ok", link_up=True)
        d.update(kw)
        return types.SimpleNamespace(**d)

    def test_clean(self):
        self.assertEqual(self._run(self._pf()), ([], []))

    def test_preflight_exception_is_unreadable(self):
        h, u = self._run(pf_exc=RuntimeError("boom"))
        self.assertEqual(h, [])
        self.assertIn("boom", u[0])

    def test_armed_conditions_flow_through_as_hazards(self):
        h, u = self._run(self._pf(safety_armed=True), armed=["a trip is latched"])
        self.assertTrue(h and "a trip is latched" in h[0])

    def test_armed_conditions_exception_is_unreadable(self):
        h, u = self._run(self._pf(safety_armed=True), armed_exc=RuntimeError("uart"))
        self.assertEqual(h, [])
        self.assertIn("uart", u[0])

    def test_running_profile_is_hazard(self):
        h, u = self._run(self._pf(profile_running_or_paused=True))
        self.assertEqual(len(h), 1)


class LiveTableGateTest(RecoveryFlashBase):
    def assertRefused(self, out: str, needle: str) -> None:
        self.assertTrue(out.startswith("error:"), out)
        self.assertIn(needle, out)
        self.assertEqual(self.tcl_calls, [])

    def _modified(self, **changes):
        out = []
        for e in self.chip_entries:
            if e.name == "recovery":
                e = partition_table.PartitionEntry(**{**e.__dict__, **changes})
            out.append(e)
        return out

    def test_matching_table_proceeds(self):
        self.assertIn("WRITTEN", self.call())

    def test_each_disagreeing_field_refuses_with_no_override(self):
        for change in ({"offset": 0xB00000}, {"size": 0x2000}, {"type": 1}, {"subtype": 0x10}):
            self.probe_result = ("h", self._modified(**change), "app")
            out = self.call(allow_unconfirmed_partition_table=True, allow_reset_into_recovery=True,
                            allow_unreadable_board_state=True)
            self.assertRefused(out, "disagrees with partitions.csv")

    def test_chip_without_recovery_row_refuses(self):
        self.probe_result = ("h", [e for e in self.chip_entries if e.name != "recovery"], "app")
        self.assertRefused(self.call(), "no partition named")

    def test_chip_duplicate_recovery_row_refuses(self):
        rec = [e for e in self.chip_entries if e.name == "recovery"][0]
        self.probe_result = ("h", self.chip_entries + [rec], "app")
        self.assertRefused(self.call(), "2 partitions named")

    def test_unreachable_board_refuses_unless_allowed(self):
        self.probe_result = (None, None, None)
        self.assertRefused(self.call(), "did not answer")
        # table waiver alone is not enough: running partition is unknown too
        self.assertRefused(self.call(allow_unconfirmed_partition_table=True), "RUNNING partition")
        self.assertIn("WRITTEN", self.call(allow_unconfirmed_partition_table=True,
                                           allow_reset_into_recovery=True))

    def test_recovery_shaped_answer_refuses_unless_allowed(self):
        self.probe_result = ("h", None, "recovery")
        self.assertRefused(self.call(), "cannot be confirmed")
        self.assertRefused(self.call(allow_unconfirmed_partition_table=True), "already 'recovery'")
        self.assertIn("WRITTEN", self.call(allow_unconfirmed_partition_table=True,
                                           allow_reset_into_recovery=True))

    def test_running_recovery_with_good_table_needs_reset_flag(self):
        self.probe_result = ("h", self.chip_entries, "recovery")
        out = self.call()
        self.assertRefused(out, "otadata")
        self.assertIn("factory-subtype", out)
        self.assertRefused(self.call(allow_unconfirmed_partition_table=True), "otadata")
        self.assertIn("WRITTEN", self.call(allow_reset_into_recovery=True))

    def test_running_unreadable_needs_reset_flag(self):
        self.probe_result = ("h", self.chip_entries, None)
        self.assertRefused(self.call(), "could not be read")

    def test_flags_must_be_exactly_true(self):
        self.probe_result = (None, None, None)
        self.assertRefused(self.call(allow_unconfirmed_partition_table=1, allow_reset_into_recovery=1),
                           "did not answer")


class ProbeAndObserveTest(unittest.TestCase):
    """The real _probe_board_partitions/_observe_boot_after_recovery_flash
    against a fake get_partitions."""

    def _patch(self, fn):
        return (unittest.mock.patch.object(partition_http_client, "get_partitions", side_effect=fn),
                unittest.mock.patch.object(mf, "_resolve_verify_hosts", return_value=["h1", "h2"]),
                unittest.mock.patch.object(mf.time, "sleep"))

    def _enter(self, fn):
        for p in self._patch(fn):
            p.start()
            self.addCleanup(p.stop)

    def test_probe_reads_table_and_running(self):
        body = {"running": "app", "partitions": [
            {"label": "recovery", "type": 0, "subtype": 0, "offset": 0xA10000, "size": 0x1000, "encrypted": False}]}
        self._enter(lambda h, timeout=None: body)
        addr, entries, running = _REAL_PROBE(None)
        self.assertEqual((addr, running, entries[0].name, entries[0].offset), ("h1", "app", "recovery", 0xA10000))

    def test_probe_recovery_shape(self):
        def fn(h, timeout=None):
            raise partition_http_client.RecoveryImageResponse("recovery", "0x9000", "app")
        self._enter(fn)
        self.assertEqual(_REAL_PROBE(None), ("h1", None, "recovery"))

    def test_probe_unreachable(self):
        def fn(h, timeout=None):
            raise partition_http_client.PartitionHttpError("down")
        self._enter(fn)
        self.assertEqual(_REAL_PROBE(None), (None, None, None))

    def test_probe_falls_through_to_second_host(self):
        def fn(h, timeout=None):
            if h == "h1":
                raise partition_http_client.PartitionHttpError("down")
            return {"running": "app", "partitions": []}
        self._enter(fn)
        self.assertEqual(_REAL_PROBE(None)[0], "h2")

    def test_observe_app(self):
        self._enter(lambda h, timeout=None: {"running": "app", "partitions": []})
        self.assertTrue(_REAL_OBSERVE(None, None).startswith("app"))

    def test_observe_recovery_table_shape(self):
        self._enter(lambda h, timeout=None: {"running": "recovery", "partitions": []})
        self.assertTrue(_REAL_OBSERVE(None, None).startswith("recovery"))

    def test_observe_recovery_image_shape(self):
        def fn(h, timeout=None):
            raise partition_http_client.RecoveryImageResponse("recovery", "0x9000", "app")
        self._enter(fn)
        self.assertTrue(_REAL_OBSERVE(None, None).startswith("recovery"))

    def test_observe_other_partition(self):
        self._enter(lambda h, timeout=None: {"running": "ota_9", "partitions": []})
        self.assertTrue(_REAL_OBSERVE(None, None).startswith("other"))

    def test_observe_unreachable(self):
        def fn(h, timeout=None):
            raise partition_http_client.PartitionHttpError("down")
        self._enter(fn)
        out = _REAL_OBSERVE(None, None)
        self.assertTrue(out.startswith("unreachable"))
        self.assertIn("UNKNOWN", out)


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
        self.assertIn("READ-BACK-VERIFIED", out)
        self.assertIn("post-reset observation", out)
        self.assertIn(self.boot_report, out)
        self.assertNotIn("no HTTP verification is possible", out)
        self.assertNotIn("is running `app`", out)

    def test_exact_openocd_command(self):
        self.call()
        expected = (f'adapter serial {mf.MAIN_BOARD_JTAG_SERIAL}; '
                    f'program_esp "{self.bin.replace(chr(92), "/")}" 0xa10000 verify reset exit')
        self.assertEqual(self.tcl_calls, [expected])

    def test_boot_outcome_unreachable_and_recovery_are_reported(self):
        for rep_ in ("unreachable -- no answer", "recovery -- the board answered"):
            self.boot_report = rep_
            self.assertIn(rep_, self.call())

    def test_s6a_note(self):
        out = self.call()
        self.assertIn("trip_reason 6", out)
        self.assertIn("0x0020", out)
        self.assertIn("0x0040", out)

    def test_write_offset_follows_a_changed_csv(self):
        with open(os.path.join(self.fw_root, "partitions.csv"), "w") as f:
            f.write(_CSV.replace("0xA10000", "0xB00000"))
        self.probe_result = ("h", partition_table.parse_partitions_csv(
            os.path.join(self.fw_root, "partitions.csv")), "app")
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
    def _elf(self, image_bytes: bytes) -> None:
        with open(os.path.join(self.build, "recovery.elf"), "wb") as f:
            f.write(b"\x7fELF" + b"\x00" * 40 + image_bytes[0x20:0x20 + 256] + b"tail")

    def test_provenance_and_elf_archive(self):
        self._elf(_image())
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

    def test_mismatched_elf_is_warned_and_not_archived(self):
        # ELF carries a descriptor with a different build time.
        other = bytearray(_image())
        other[0x20 + 80:0x20 + 88] = b"01:02:03"
        self._elf(bytes(other))
        out = self.call()
        self.assertIn("WRITTEN", out)
        self.assertIn("does not match the flashed image", out)
        self.assertIn("NOT archived", out)
        self.assertFalse(os.path.exists(recovery_flash.recovery_archive_dir(self.tmp)))
        prov = json.load(open(recovery_flash.recovery_provenance_path(self.tmp)))
        self.assertIsNone(prov["elf_archived"])

    def test_elf_with_wrong_version_is_not_archived(self):
        other = bytearray(_image())
        other[0x20 + 16:0x20 + 21] = b"v9.9\x00"
        self._elf(bytes(other))
        self.assertIn("NOT archived", self.call())

    def test_elf_without_descriptor_is_not_archived(self):
        with open(os.path.join(self.build, "recovery.elf"), "wb") as f:
            f.write(b"ELFDATA" * 10)
        out = self.call()
        self.assertIn("none found", out)
        self.assertFalse(os.path.exists(recovery_flash.recovery_archive_dir(self.tmp)))

    def test_no_elf_is_reported_not_fatal(self):
        out = self.call()
        self.assertIn("nothing archived", out)
        self.assertIn("WRITTEN", out)


class StalenessTest(RecoveryFlashBase):
    def setUp(self):
        super().setUp()
        self.src = os.path.join(self.tmp, "firmware", "KilnFW_recovery", "main")
        os.makedirs(self.src)
        self.src_file = os.path.join(self.src, "recovery_http.c")
        with open(self.src_file, "w") as f:
            f.write("x")
        now = os.path.getmtime(self.bin)
        self.now = now

    def _touch(self, path, mtime):
        os.utime(path, (mtime, mtime))

    def test_image_older_than_source_refuses(self):
        self._touch(self.bin, self.now - 100)
        self._touch(self.src_file, self.now)
        out = self.call()
        self.assertTrue(out.startswith("error:"), out)
        self.assertIn("stale", out)
        self.assertIn("recovery_http.c", out)
        self.assertEqual(self.tcl_calls, [])
        self.assertTrue(mf.flash_recovery(kiln_fw_root=self.fw_root, dry_run=True).startswith("error:"))

    def test_allow_stale_proceeds_with_warning(self):
        self._touch(self.bin, self.now - 100)
        self._touch(self.src_file, self.now)
        out = self.call(allow_stale=True)
        self.assertIn("WRITTEN", out)
        self.assertIn("WARNING", out)
        self.assertIn("OLDER", out)

    def test_allow_stale_must_be_exactly_true(self):
        self._touch(self.bin, self.now - 100)
        self._touch(self.src_file, self.now)
        self.assertTrue(self.call(allow_stale=1).startswith("error:"))

    def test_fresh_image_and_build_dir_files_are_ignored(self):
        self._touch(self.src_file, self.now - 100)
        newer_build = os.path.join(self.build, "other.o")
        with open(newer_build, "w") as f:
            f.write("o")
        self._touch(newer_build, self.now + 500)
        out = self.call()
        self.assertIn("WRITTEN", out)
        self.assertIn("image age:", out)

    def test_age_is_always_printed(self):
        self._touch(self.src_file, self.now - 100)
        self.assertIn("image age:", mf.flash_recovery(kiln_fw_root=self.fw_root, dry_run=True))


if __name__ == "__main__":
    unittest.main()
