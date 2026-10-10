#!/usr/bin/env python3
"""find_crash_elf / find_crash_elf_for_coredump must also search the recovery
ELF archive (firmware/KilnFW/recovery_elf_archive). symbolize_coredump is
faked: it "matches" iff the ELF's SHA equals the SHA in the coredump header."""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import coredump_fetch, elf_archive, mcp_server_flash  # noqa: E402


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _fake_symbolize(coredump_path, elf_path, **_kw):
    with open(coredump_path, "rb") as f:
        want = f.read().decode().strip()
    with open(elf_path, "rb") as f:
        got = _sha(f.read())
    if want != got:
        raise coredump_fetch.CoredumpSymbolizeError("SHA256 mismatch")
    return "bt"


class RecoveryArchiveTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rec = os.path.join(self.tmp.name, "recovery_elf_archive")
        os.makedirs(self.rec)
        self.kiln = os.path.join(self.tmp.name, "elf_archive")
        os.makedirs(self.kiln)
        self.addCleanup(self.tmp.cleanup)
        for target, val in (("recovery_archive_dir", self.rec), ("kiln_archive_dir", self.kiln)):
            p = unittest.mock.patch.object(elf_archive, target, return_value=val)
            p.start()
            self.addCleanup(p.stop)
        p = unittest.mock.patch.object(elf_archive, "list_all_kiln_elf_paths", return_value=[])
        p.start()
        self.addCleanup(p.stop)
        p = unittest.mock.patch.object(coredump_fetch, "symbolize_coredump", _fake_symbolize)
        p.start()
        self.addCleanup(p.stop)

    def _put_elf(self, data: bytes) -> str:
        path = os.path.join(self.rec, f"recovery-{_sha(data)[:12]}.elf")
        with open(path, "wb") as f:
            f.write(data)
        return path

    def _dump(self, sha: str) -> str:
        path = os.path.join(self.tmp.name, "core.bin")
        with open(path, "wb") as f:
            f.write(sha.encode())
        return path

    def test_matching_recovery_elf_found(self):
        elf = self._put_elf(b"recovery-elf-A")
        out = mcp_server_flash.find_crash_elf_for_coredump(self._dump(_sha(b"recovery-elf-A")))
        self.assertIn(f"elf={elf}", out)

    def test_non_matching_recovery_elf_not_returned(self):
        self._put_elf(b"recovery-elf-A")
        out = mcp_server_flash.find_crash_elf_for_coredump(self._dump(_sha(b"something-else")))
        self.assertNotIn("elf=", out)
        self.assertIn("UNSYMBOLIZABLE", out)

    def test_find_crash_elf_by_build_uses_recovery_manifest(self):
        elf = self._put_elf(b"recovery-elf-B")
        key = _sha(b"recovery-elf-B")[:12]
        build = "Oct  9 2026 10:00:00"
        with open(os.path.join(self.rec, "manifest.json"), "w") as f:
            json.dump({elf_archive.normalize_build_timestamp(build): {"elf_key": key}}, f)
        out = mcp_server_flash.find_crash_elf(fw_build=build)
        self.assertIn(elf, out)
        self.assertFalse(out.startswith("error"))

    def test_find_crash_elf_by_dump_sha_prefix(self):
        elf = self._put_elf(b"recovery-elf-C")
        self._put_elf(b"recovery-elf-D")
        prefix = _sha(b"recovery-elf-C")[:9]
        out = mcp_server_flash.find_crash_elf(dump_elf_sha=prefix)
        self.assertIn(elf, out)
        out = mcp_server_flash.find_crash_elf(dump_elf_sha="ffffffff1")
        self.assertTrue(out.startswith("error"))
        self.assertIn("NOT a substitute", out)

    def test_find_crash_elf_prefers_stale_dump_sha_from_board(self):
        elf = self._put_elf(b"recovery-elf-E")
        rec = {"present": True, "stale_image": True, "dump_elf_sha": _sha(b"recovery-elf-E")[:9]}
        with unittest.mock.patch.object(mcp_server_flash.dashboard_http_client, "get_crash_report", return_value=rec),                 unittest.mock.patch("kilnctrl.mcp_server_ota._ota_resolve_host", return_value="h"):
            out = mcp_server_flash.find_crash_elf()
        self.assertIn(elf, out)


if __name__ == "__main__":
    unittest.main()
