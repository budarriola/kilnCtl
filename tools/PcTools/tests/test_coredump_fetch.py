#!/usr/bin/env python3
"""Unit tests for coredump_fetch.py -- fetching the ESP's coredump partition
over HTTP and symbolizing it, with particular focus on the failure paths the
2026-09-14 task requires: no coredump present, a truncated transfer, and
(the important one) an ELF that does not match the coredump. All against a
fake HTTP server / mocked subprocess -- no real board, no OpenOCD, no
espcoredump install required.

Run with: python -m pytest tools/PcTools/tests/test_coredump_fetch.py -q
"""
from __future__ import annotations

import http.server
import json
import os
import sys
import tempfile
import threading
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import coredump_fetch  # noqa: E402


class _FakeCoredumpServer(http.server.BaseHTTPRequestHandler):
    """Minimal stand-in for diagnostics_http.c's two coredump endpoints.
    Class-level state (set by each test) drives what /api/coredump/info and
    /api/coredump/chunk return -- this is a real TCP server (not a mocked
    urllib call) so fetch_coredump_over_http() is exercised over its actual
    HTTP path, chunk loop included."""

    present = True
    data = b""
    partition_size = 0x100000
    chunk_size = 64
    fail_after_bytes = None  # if set, chunk requests past this offset 500

    def log_message(self, *args):  # silence
        pass

    def do_GET(self):
        if self.path.startswith("/api/coredump/info"):
            body = json.dumps({
                "ok": True,
                "present": self.present,
                "data_len": len(self.data) if self.present else coredump_fetch.COREDUMP_BLANK_LEN,
                "partition_size": self.partition_size,
                "chunk_size": self.chunk_size,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/api/coredump/chunk"):
            from urllib.parse import urlparse, parse_qs
            qs = parse_qs(urlparse(self.path).query)
            offset = int(qs["offset"][0])
            length = int(qs["len"][0])
            if self.fail_after_bytes is not None and offset >= self.fail_after_bytes:
                self.send_response(500)
                self.end_headers()
                return
            chunk = self.data[offset:offset + length]
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(chunk)))
            self.end_headers()
            self.wfile.write(chunk)
            return
        self.send_response(404)
        self.end_headers()


class FetchOverHttpTests(unittest.TestCase):
    def setUp(self):
        _FakeCoredumpServer.present = True
        _FakeCoredumpServer.data = os.urandom(1000)
        _FakeCoredumpServer.chunk_size = 64
        _FakeCoredumpServer.fail_after_bytes = None
        self.server = http.server.HTTPServer(("127.0.0.1", 0), _FakeCoredumpServer)
        self.host = f"127.0.0.1:{self.server.server_address[1]}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
        self.server.shutdown()
        self.thread.join(timeout=5)

    def test_fetch_full_transfer_matches_source(self):
        out_path = os.path.join(self.tmpdir, "dump.bin")
        n = coredump_fetch.fetch_coredump_over_http(self.host, out_path)
        self.assertEqual(n, len(_FakeCoredumpServer.data))
        with open(out_path, "rb") as f:
            self.assertEqual(f.read(), _FakeCoredumpServer.data)
        # no leftover .part file
        self.assertFalse(os.path.exists(out_path + ".part"))

    def test_no_coredump_present_fails_loud(self):
        _FakeCoredumpServer.present = False
        out_path = os.path.join(self.tmpdir, "dump.bin")
        with self.assertRaises(coredump_fetch.CoredumpFetchError):
            coredump_fetch.fetch_coredump_over_http(self.host, out_path)
        self.assertFalse(os.path.exists(out_path))

    def test_mid_transfer_http_failure_leaves_no_partial_file(self):
        _FakeCoredumpServer.fail_after_bytes = 500
        out_path = os.path.join(self.tmpdir, "dump.bin")
        with self.assertRaises(coredump_fetch.CoredumpFetchError):
            coredump_fetch.fetch_coredump_over_http(self.host, out_path)
        self.assertFalse(os.path.exists(out_path))
        self.assertFalse(os.path.exists(out_path + ".part"))

    def test_info_self_inconsistent_data_len_refused(self):
        # data_len is derived from len(data) in the fake server, so force an
        # inconsistency directly against get_coredump_info's caller contract:
        # partition_size smaller than data_len must be refused, not clamped.
        _FakeCoredumpServer.partition_size = 10  # data is 1000 bytes
        out_path = os.path.join(self.tmpdir, "dump.bin")
        with self.assertRaises(coredump_fetch.CoredumpFetchError):
            coredump_fetch.fetch_coredump_over_http(self.host, out_path)


class SymbolizeMismatchTests(unittest.TestCase):
    """The negative test the task explicitly asks for: an ELF that does not
    match the coredump must fail loudly, never produce a plausible-looking
    result. espcoredump/esp_coredump is not assumed installed here -- the
    subprocess call itself is mocked, since what this module is responsible
    for is correctly PROPAGATING that refusal, not re-implementing
    esp_coredump's own SHA256 check."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.coredump_path = os.path.join(self.tmpdir, "dump.bin")
        with open(self.coredump_path, "wb") as f:
            f.write(b"\x00" * 64)
        self.elf_path = os.path.join(self.tmpdir, "KilnCtrl-deadbeef.elf")
        with open(self.elf_path, "wb") as f:
            f.write(b"\x7fELF fake")
        self.idf_dir = os.path.join(self.tmpdir, "idf")
        os.makedirs(os.path.join(self.idf_dir, "components", "espcoredump"), exist_ok=True)
        with open(os.path.join(self.idf_dir, "components", "espcoredump", "espcoredump.py"), "w") as f:
            f.write("# stub, never actually executed -- subprocess.run is mocked below\n")

    def test_mismatched_elf_raises_with_names(self):
        mismatch_result = unittest.mock.Mock(
            returncode=1,
            stdout="",
            stderr="Error: SHA256 of ELF file does not match the one stored in the core dump!",
        )
        with unittest.mock.patch("subprocess.run", return_value=mismatch_result) as run_mock:
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                coredump_fetch.symbolize_coredump(
                    self.coredump_path, self.elf_path,
                    fw_build="Sep 10 2026 12:00:00",
                    idf_path=self.idf_dir,
                )
            run_mock.assert_called_once()
        msg = str(ctx.exception)
        # the failure must name what was tried, not just say "failed" --
        # compare against repr() since the message embeds the path via !r
        self.assertIn(repr(self.elf_path), msg)
        self.assertIn("Sep 10 2026 12:00:00", msg)
        self.assertIn("SHA256", msg)

    def test_matching_elf_returns_stdout(self):
        ok_result = unittest.mock.Mock(returncode=0, stdout="Crashed task: profile_executor\n", stderr="")
        with unittest.mock.patch("subprocess.run", return_value=ok_result):
            out = coredump_fetch.symbolize_coredump(
                self.coredump_path, self.elf_path, idf_path=self.idf_dir,
            )
        self.assertIn("profile_executor", out)

    def test_missing_elf_refused_before_subprocess(self):
        with unittest.mock.patch("subprocess.run") as run_mock:
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError):
                coredump_fetch.symbolize_coredump(
                    self.coredump_path, os.path.join(self.tmpdir, "does_not_exist.elf"),
                    idf_path=self.idf_dir,
                )
            run_mock.assert_not_called()

    def test_missing_idf_path_refused_before_subprocess(self):
        with unittest.mock.patch("subprocess.run") as run_mock:
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError):
                coredump_fetch.symbolize_coredump(
                    self.coredump_path, self.elf_path, idf_path=os.path.join(self.tmpdir, "no-such-idf"),
                )
            run_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
