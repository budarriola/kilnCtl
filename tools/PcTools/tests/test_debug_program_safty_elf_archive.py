#!/usr/bin/env python3
"""Unit tests for debug_program(peer="pico")'s post-flash ELF archiving
(mcp_server_debug._archive_flashed_safty_elf / debug_program).

History (2026-09-21): debug_program(peer="pico", elf_path=<worktree>/...)
flashed the Pico from a clean worktree ELF, but the archive helper ignored
the caller's `elf_path` entirely and always archived
debug_probe._safty_fw_elf() (the default main-tree build path) instead --
the correct ELF had to be hand-copied into the archive afterward. Separately,
a failed archive call was swallowed into an empty string, indistinguishable
from a call that never ran.

Both proven fixed here:
  1. The path passed to elf_archive.archive_safty_elf() must equal the
     caller's own `elf_path`, not the default build path, whenever one is
     given.
  2. A failing archive call must leave a loud "WARNING" trace in the tool's
     returned result string, never silently disappear -- while still
     reporting the flash itself as successful (archiving is best-effort and
     must never fail the flash).

All against mocked debug_probe/elf_archive/stale_check -- no real OpenOCD
session, no live board.

Run with:
  tools\\PcTools\\.venv\\Scripts\\python.exe -m pytest tools/PcTools/tests/test_debug_program_safty_elf_archive.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_debug as md  # noqa: E402
from kilnctrl import debug_probe  # noqa: E402
from kilnctrl import elf_archive  # noqa: E402


class DebugProgramSaftyElfArchiveTest(unittest.TestCase):
    def test_archives_caller_supplied_elf_path_not_default(self):
        """The exact `elf_path` passed to debug_program() must be the path
        archived -- not debug_probe._safty_fw_elf()'s default main-tree
        build output."""
        caller_elf = os.path.join("C:\\wt\\elfarch-worktree", "firmware", "SaftyFW",
                                   "build", "SaftyFW.elf")
        program_mock = unittest.mock.Mock(return_value=(True, "ok"))
        archive_result = unittest.mock.Mock(archived_path="/archive/SaftyFW-deadbeef.elf",
                                             identity="deadbeef_date_time")
        archive_mock = unittest.mock.Mock(return_value=archive_result)

        with unittest.mock.patch.object(debug_probe, "program", program_mock), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_root", return_value="/fake/safty"), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_elf",
                                            return_value="/default/main-tree/SaftyFW.elf") as default_elf_mock, \
                unittest.mock.patch.object(elf_archive, "archive_safty_elf", archive_mock):
            result = md.debug_program(peer="pico", elf_path=caller_elf, confirm=True)

        self.assertIn("programmed pico OK", result)
        program_mock.assert_called_once_with("pico", caller_elf)
        archive_mock.assert_called_once()
        archived_elf_arg = archive_mock.call_args[0][0]
        self.assertEqual(archived_elf_arg, caller_elf,
                          "archived the wrong ELF -- must be the caller's elf_path, "
                          "not the default build path")
        self.assertNotEqual(archived_elf_arg, default_elf_mock.return_value)
        self.assertIn(archive_result.archived_path, result)

    def test_default_elf_path_used_only_when_caller_omits_one(self):
        """With no elf_path given, the archived path is the peer's default
        build output (unchanged prior behaviour for the common case)."""
        program_mock = unittest.mock.Mock(return_value=(True, "ok"))
        stale_result = unittest.mock.Mock(stale=False, reason="")
        archive_result = unittest.mock.Mock(archived_path="/archive/SaftyFW-cafef00d.elf",
                                             identity="cafef00d_date_time")
        archive_mock = unittest.mock.Mock(return_value=archive_result)

        with unittest.mock.patch.object(debug_probe, "program", program_mock), \
                unittest.mock.patch.object(md.stale_check, "check_saftyfw_stale", return_value=stale_result), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_root", return_value="/fake/safty"), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_elf",
                                            return_value="/default/main-tree/SaftyFW.elf"), \
                unittest.mock.patch.object(elf_archive, "archive_safty_elf", archive_mock):
            md.debug_program(peer="pico", confirm=True)

        archive_mock.assert_called_once()
        self.assertEqual(archive_mock.call_args[0][0], "/default/main-tree/SaftyFW.elf")

    def test_archive_failure_surfaces_loud_warning_not_silent(self):
        """A raising archive_safty_elf() must never propagate (the flash
        already succeeded) but must leave a "WARNING" trace naming the
        failure in the returned result -- never an empty/silent suffix."""
        program_mock = unittest.mock.Mock(return_value=(True, "ok"))

        with unittest.mock.patch.object(debug_probe, "program", program_mock), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_root", return_value="/fake/safty"), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_elf",
                                            return_value="/default/main-tree/SaftyFW.elf"), \
                unittest.mock.patch.object(elf_archive, "archive_safty_elf",
                                            side_effect=ValueError("boom: no such file")):
            result = md.debug_program(peer="pico", elf_path="/some/worktree/SaftyFW.elf", confirm=True)

        self.assertIn("programmed pico OK", result, "archiving failure must not fail the flash result")
        self.assertIn("WARNING", result)
        self.assertIn("boom: no such file", result)


if __name__ == "__main__":
    unittest.main()
