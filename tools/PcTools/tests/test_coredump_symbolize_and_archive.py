#!/usr/bin/env python3
"""New behavioural tests for coredump_fetch.py, added 2026-09-16
(RELEASE_HARDENING_PLAN.md blocker 1 item 1). Appended to
test_coredump_fetch.py.

These specifically target three defects found while symbolizing the real
748032-byte coredump sitting on the board:

1. `symbolize_coredump()`'s subprocess command passed the coredump path and
   ELF path as bare positionals ("-t", "raw", coredump_path, elf_path)
   instead of using espcoredump's real `info_corefile` CLI, which takes the
   coredump only via `--core`/`-c` + `--core-format`/`-t`, and a `--chip`
   global option, with a single positional (`prog`, the ELF). The old
   command line failed with an argparse "unrecognized arguments" error
   *before espcoredump ever compared the ELF against the coredump* -- so
   the mismatch-detection code path this module's docstring is proud of had
   never actually been exercised end-to-end. A test that mocks
   subprocess.run cannot see this bug (that is exactly why it shipped) --
   this test drives a REAL child Python process against a fake CLI script
   that mimics espcoredump's actual argparse shape, so a wrong command line
   fails for a real, observable reason.
2. `symbolize_coredump()` used `sys.executable` unconditionally, silently
   trying to import `esp_coredump` from whatever interpreter is running the
   MCP server -- which need not have it installed. The resulting
   ModuleNotFoundError got misreported as "very likely an ELF/coredump
   mismatch", an actively wrong diagnosis.
3. `archive_coredump()` did not exist at all: a fetched coredump had no
   durable, provenance-carrying home and was silently overwritten by the
   next fetch.
"""
from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import coredump_fetch  # noqa: E402


_FAKE_ESPCOREDUMP_ARGPARSE = r'''
import argparse, sys

p = argparse.ArgumentParser()
p.add_argument("--chip", required=True)
sub = p.add_subparsers(dest="cmd", required=True)
info = sub.add_parser("info_corefile")
info.add_argument("--core", "-c", required=True)
info.add_argument("--core-format", "-t", required=True)
info.add_argument("prog")

args = p.parse_args()
if not os.path.isfile(args.core):
    print(f"no such core file: {args.core}", file=sys.stderr)
    sys.exit(1)
if not os.path.isfile(args.prog):
    print(f"no such prog file: {args.prog}", file=sys.stderr)
    sys.exit(1)
print(f"OK: chip={args.chip} core={args.core} fmt={args.core_format} prog={args.prog}")
sys.exit(0)
'''


class RealCliArgumentShapeTests(unittest.TestCase):
    """Drives symbolize_coredump() against a REAL child process running a
    fake espcoredump.py that parses arguments the same way the genuine
    ESP-IDF script does (positional `prog`, `--core`/`--core-format`
    flags, required `--chip`). This is deliberately NOT a mocked
    subprocess.run -- a mock can't see an argparse-shape bug, which is
    exactly how the original wrong command line (positional coredump path,
    no --chip) shipped and passed review."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.idf_dir = os.path.join(self.tmpdir, "idf")
        script_dir = os.path.join(self.idf_dir, "components", "espcoredump")
        os.makedirs(script_dir, exist_ok=True)
        self.script_path = os.path.join(script_dir, "espcoredump.py")
        with open(self.script_path, "w") as f:
            f.write("import os\n" + _FAKE_ESPCOREDUMP_ARGPARSE)

        self.coredump_path = os.path.join(self.tmpdir, "dump.bin")
        with open(self.coredump_path, "wb") as f:
            f.write(b"\x00" * 16)
        self.elf_path = os.path.join(self.tmpdir, "KilnCtrl-deadbeef.elf")
        with open(self.elf_path, "wb") as f:
            f.write(b"\x7fELF fake")

    def test_real_cli_invocation_succeeds_with_correct_flags(self):
        """This is the test that catches the shipped bug: with the old
        command line ([subcommand, "-t", "raw", coredump_path, elf_path],
        no --chip), the fake CLI's real argparse rejects it (missing
        --chip, elf_path is an unrecognized extra positional) and this test
        FAILS against unfixed code. Against the fix it must pass."""
        out = coredump_fetch.symbolize_coredump(
            self.coredump_path, self.elf_path,
            fw_build="Sep 16 2026 11:45:29",
            idf_path=self.idf_dir,
            espcoredump_python=sys.executable,
        )
        self.assertIn("OK:", out)
        self.assertIn(self.coredump_path, out)
        self.assertIn(self.elf_path, out)


class ModuleNotFoundMisdiagnosisTests(unittest.TestCase):
    """A missing esp_coredump install must be reported as an environment
    problem, never conflated with 'this is likely an ELF/coredump
    mismatch' -- confirmed live against the real board coredump 2026-09-16
    that these two failure modes look identical (exit 1) until stderr is
    actually inspected."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.coredump_path = os.path.join(self.tmpdir, "dump.bin")
        with open(self.coredump_path, "wb") as f:
            f.write(b"\x00" * 16)
        self.elf_path = os.path.join(self.tmpdir, "fake.elf")
        with open(self.elf_path, "wb") as f:
            f.write(b"\x7fELF fake")
        self.idf_dir = os.path.join(self.tmpdir, "idf")
        os.makedirs(os.path.join(self.idf_dir, "components", "espcoredump"), exist_ok=True)
        with open(os.path.join(self.idf_dir, "components", "espcoredump", "espcoredump.py"), "w") as f:
            f.write("# stub\n")

    def test_missing_module_is_not_reported_as_mismatch(self):
        result = unittest.mock.Mock(
            returncode=1, stdout="",
            stderr=(
                "Traceback (most recent call last):\n"
                "  File \"espcoredump.py\", line 13, in <module>\n"
                "    from esp_coredump import CoreDump\n"
                "ModuleNotFoundError: No module named 'esp_coredump'\n"
            ),
        )
        with unittest.mock.patch("subprocess.run", return_value=result):
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                coredump_fetch.symbolize_coredump(
                    self.coredump_path, self.elf_path, idf_path=self.idf_dir,
                )
        msg = str(ctx.exception)
        self.assertIn("esp_coredump", msg)
        self.assertIn("does not have", msg)
        self.assertNotIn("very likely an ELF/coredump mismatch", msg)


class ResolveEspcoredumpPythonTests(unittest.TestCase):
    def test_prefers_idf_python_env_path(self):
        tmpdir = tempfile.mkdtemp()
        scripts_dir = os.path.join(tmpdir, "Scripts")
        os.makedirs(scripts_dir, exist_ok=True)
        fake_python = os.path.join(scripts_dir, "python.exe")
        with open(fake_python, "w") as f:
            f.write("")
        with unittest.mock.patch.dict(os.environ, {"IDF_PYTHON_ENV_PATH": tmpdir}, clear=False):
            resolved = coredump_fetch._resolve_espcoredump_python(None)
        self.assertEqual(resolved, fake_python)

    def test_explicit_argument_wins_over_env(self):
        with unittest.mock.patch.dict(os.environ, {"IDF_PYTHON_ENV_PATH": "/nonexistent"}, clear=False):
            resolved = coredump_fetch._resolve_espcoredump_python("/explicit/python")
        self.assertEqual(resolved, "/explicit/python")

    def test_falls_back_to_sys_executable_when_env_unset(self):
        env = dict(os.environ)
        env.pop("IDF_PYTHON_ENV_PATH", None)
        with unittest.mock.patch.dict(os.environ, env, clear=True):
            resolved = coredump_fetch._resolve_espcoredump_python(None)
        self.assertEqual(resolved, sys.executable)


class ArchiveCoredumpTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.archive_dir = os.path.join(self.tmpdir, "archive")
        self.fetched = os.path.join(self.tmpdir, "fetched.bin")
        with open(self.fetched, "wb") as f:
            f.write(os.urandom(2000))

    def test_writes_bin_and_provenance_with_expected_fields(self):
        result = coredump_fetch.archive_coredump(
            self.fetched, host="192.168.1.156", fw_build_reported="Sep 16 2026 11:45:29",
            archive_dir=self.archive_dir,
        )
        self.assertTrue(os.path.isfile(result.path))
        self.assertTrue(os.path.isfile(result.provenance_path))
        with open(result.provenance_path) as f:
            prov = json.load(f)
        self.assertEqual(prov["host"], "192.168.1.156")
        self.assertEqual(prov["fw_build_reported"], "Sep 16 2026 11:45:29")
        self.assertEqual(prov["byte_len"], 2000)
        self.assertIn("fetched_at", prov)
        self.assertEqual(prov["sha256"], result.sha256)

    def test_never_overwrites_existing_archived_entry(self):
        first = coredump_fetch.archive_coredump(
            self.fetched, host="host-a", fw_build_reported="build-a",
            archive_dir=self.archive_dir,
        )
        with open(first.provenance_path) as f:
            first_prov = json.load(f)
        # Re-archiving identical content (even under a different host label)
        # must not overwrite the original provenance record.
        second = coredump_fetch.archive_coredump(
            self.fetched, host="host-b", fw_build_reported="build-b",
            archive_dir=self.archive_dir,
        )
        self.assertEqual(first.path, second.path)
        with open(second.provenance_path) as f:
            second_prov = json.load(f)
        self.assertEqual(first_prov, second_prov)
        self.assertEqual(second_prov["host"], "host-a")

    def test_missing_source_file_raises(self):
        with self.assertRaises(coredump_fetch.CoredumpFetchError):
            coredump_fetch.archive_coredump(
                os.path.join(self.tmpdir, "does_not_exist.bin"),
                host="h", fw_build_reported=None, archive_dir=self.archive_dir,
            )


if __name__ == "__main__":
    unittest.main()
