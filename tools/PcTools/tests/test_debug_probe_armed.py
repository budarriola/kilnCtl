#!/usr/bin/env python3
"""Unit tests for kilnctrl.debug_probe's Pico ARMED-state read
(resolve_symbol()/pico_armed_state()) -- see tools/PcTools/TODO.md line 478
and debug_probe.py's module docstring for the design this backs.

All tests mock subprocess (arm-none-eabi-nm) and debug_probe.read_memory()
directly -- no real OpenOCD session, no live board is used or required.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe  # noqa: E402


def _nm_dash_S_output(lines: "list[str]") -> str:
    return "\n".join(lines) + "\n"


class ResolveSymbolTests(unittest.TestCase):
    def _run(self, stdout: str, returncode: int = 0):
        proc = unittest.mock.Mock(stdout=stdout, returncode=returncode)
        return unittest.mock.patch("kilnctrl.debug_probe.subprocess.run", return_value=proc)

    def test_unique_symbol_resolves(self):
        stdout = _nm_dash_S_output([
            "2000ba33 00000001 b s_state",
            "20009000 00000004 b s_other",
        ])
        with self._run(stdout), unittest.mock.patch("os.path.isfile", return_value=True):
            result = debug_probe.resolve_symbol("fake.elf", "s_state", nm_exe="fake-nm")
        self.assertEqual(result, (0x2000BA33, 1))

    def test_missing_symbol_returns_none(self):
        stdout = _nm_dash_S_output(["2000ba33 00000001 b s_other"])
        with self._run(stdout), unittest.mock.patch("os.path.isfile", return_value=True):
            result = debug_probe.resolve_symbol("fake.elf", "s_state", nm_exe="fake-nm")
        self.assertIsNone(result)

    def test_ambiguous_symbol_fails_closed(self):
        # Same name resolved twice (e.g. a hypothetical future collision) --
        # must refuse rather than guess which one is the real state.
        stdout = _nm_dash_S_output([
            "2000ba33 00000001 b s_state",
            "20009000 00000001 b s_state",
        ])
        with self._run(stdout), unittest.mock.patch("os.path.isfile", return_value=True):
            result = debug_probe.resolve_symbol("fake.elf", "s_state", nm_exe="fake-nm")
        self.assertIsNone(result)

    def test_nm_error_returns_none(self):
        with self._run("", returncode=1), unittest.mock.patch("os.path.isfile", return_value=True):
            result = debug_probe.resolve_symbol("fake.elf", "s_state", nm_exe="fake-nm")
        self.assertIsNone(result)

    def test_missing_elf_returns_none_without_running_nm(self):
        with unittest.mock.patch("os.path.isfile", return_value=False):
            with unittest.mock.patch("kilnctrl.debug_probe.subprocess.run") as run:
                result = debug_probe.resolve_symbol("missing.elf", "s_state", nm_exe="fake-nm")
                run.assert_not_called()
        self.assertIsNone(result)

    def test_no_nm_exe_returns_none(self):
        with unittest.mock.patch("kilnctrl.debug_probe._find_arm_nm_exe", return_value=None):
            result = debug_probe.resolve_symbol("fake.elf", "s_state", nm_exe=None)
        self.assertIsNone(result)


class PicoArmedStateTests(unittest.TestCase):
    def _patch_resolve(self, resolved):
        return unittest.mock.patch("kilnctrl.debug_probe.resolve_symbol", return_value=resolved)

    def _patch_read(self, ok: bool, output: str):
        return unittest.mock.patch("kilnctrl.debug_probe.read_memory", return_value=(ok, output))

    def test_armed_value_reports_true(self):
        with self._patch_resolve((0x2000BA33, 1)), self._patch_read(
            True, "MEMRD 0x2000ba33 0x02\n"
        ):
            armed, detail = debug_probe.pico_armed_state()
        self.assertTrue(armed)
        self.assertIn("ARMED", detail)

    def test_grace_value_reports_not_armed(self):
        with self._patch_resolve((0x2000BA33, 1)), self._patch_read(
            True, "MEMRD 0x2000ba33 0x01\n"
        ):
            armed, detail = debug_probe.pico_armed_state()
        self.assertFalse(armed)

    def test_tripped_value_reports_not_armed(self):
        with self._patch_resolve((0x2000BA33, 1)), self._patch_read(
            True, "MEMRD 0x2000ba33 0x03\n"
        ):
            armed, detail = debug_probe.pico_armed_state()
        self.assertFalse(armed)

    def test_unresolvable_symbol_fails_closed(self):
        with self._patch_resolve(None):
            armed, detail = debug_probe.pico_armed_state()
        self.assertIsNone(armed)

    def test_wrong_size_fails_closed(self):
        with self._patch_resolve((0x2000BA33, 4)):
            armed, detail = debug_probe.pico_armed_state()
        self.assertIsNone(armed)
        self.assertIn("byte", detail)

    def test_failed_swd_read_fails_closed(self):
        with self._patch_resolve((0x2000BA33, 1)), self._patch_read(False, "Error: no target"):
            armed, detail = debug_probe.pico_armed_state()
        self.assertIsNone(armed)

    def test_unparseable_output_fails_closed(self):
        with self._patch_resolve((0x2000BA33, 1)), self._patch_read(True, "garbage, no MEMRD line"):
            armed, detail = debug_probe.pico_armed_state()
        self.assertIsNone(armed)

    def test_out_of_range_value_fails_closed(self):
        # Real hardware finding this session: a live read against a board not
        # currently running the resolved ELF returned 0xb7 -- must refuse,
        # not silently report "not armed".
        with self._patch_resolve((0x2000BA33, 1)), self._patch_read(
            True, "MEMRD 0x2000ba33 0xb7\n"
        ):
            armed, detail = debug_probe.pico_armed_state()
        self.assertIsNone(armed)
        self.assertIn("0xb7", detail)


if __name__ == "__main__":
    unittest.main()
