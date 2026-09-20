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

    def test_resolved_gdb_passed_on_cmdline(self):
        """2026-09-19 fix: a matching ELF must still symbolize even when the
        subprocess's own PATH has no GDB on it -- confirmed live against a
        real archived ELF/coredump pair that esp_coredump accepts fine but
        which failed with a bare non-zero exit ("GDB executable not found")
        until GDB was explicitly resolved and passed via --gdb. This does not
        depend on a real GDB install: _resolve_gdb_path is mocked directly,
        and the test asserts the resolved path lands in the subprocess argv,
        with subprocess.run itself also mocked (matching this file's existing
        no-real-espcoredump-install convention)."""
        ok_result = unittest.mock.Mock(returncode=0, stdout="Crashed task: profile_executor\n", stderr="")
        fake_gdb = os.path.join(self.tmpdir, "xtensa-esp32s3-elf-gdb.exe")
        with open(fake_gdb, "w") as f:
            f.write("# fake gdb, never executed\n")
        with unittest.mock.patch("subprocess.run", return_value=ok_result) as run_mock, \
             unittest.mock.patch.object(coredump_fetch, "_resolve_gdb_path", return_value=fake_gdb):
            out = coredump_fetch.symbolize_coredump(
                self.coredump_path, self.elf_path, idf_path=self.idf_dir,
            )
        self.assertIn("profile_executor", out)
        cmd = run_mock.call_args[0][0]
        self.assertIn("--gdb", cmd)
        self.assertEqual(cmd[cmd.index("--gdb") + 1], fake_gdb)
        # --gdb must land as a FLAG before the subcommand's own positional
        # `prog` argument -- pins the argv shape the real invocation relies
        # on rather than just checking --gdb is present somewhere.
        self.assertGreater(cmd.index("--gdb"), cmd.index("info_corefile"))

    def test_resolve_gdb_path_finds_newest_version_under_idf_tools_path(self):
        """Real (unmocked) _resolve_gdb_path against a fake IDF_TOOLS_PATH
        tree with several planted xtensa-esp32s3-elf-gdb.exe versions --
        confirms both that the glob/lookup shape actually works end to end
        (not just the mocked call sites the other tests use) and that the
        newest version is chosen numerically, not lexicographically (a plain
        string sort would rank "9.0" ahead of "12.1")."""
        fake_tools_root = os.path.join(self.tmpdir, "fake_idf_tools")
        versions = ["9.0_20231005", "12.1_20240403", "12.1_20250301", "13.0_20250101"]
        for version in versions:
            gdb_dir = os.path.join(fake_tools_root, "tools", "xtensa-esp-elf-gdb",
                                    version, "xtensa-esp-elf-gdb", "bin")
            os.makedirs(gdb_dir, exist_ok=True)
            with open(os.path.join(gdb_dir, "xtensa-esp32s3-elf-gdb.exe"), "w") as f:
                f.write("# fake gdb, never executed\n")
        with unittest.mock.patch.dict(os.environ, {"IDF_TOOLS_PATH": fake_tools_root}), \
             unittest.mock.patch("shutil.which", return_value=None):
            found = coredump_fetch._resolve_gdb_path()
        self.assertIsNotNone(found)
        # Newest by version number, "13.0_20250101", not by lexicographic
        # sort (which would incorrectly pick "9.0_20231005").
        self.assertIn(os.path.join("13.0_20250101", "xtensa-esp-elf-gdb", "bin"), found)

    def test_gdb_not_found_is_an_environment_failure_not_a_mismatch(self):
        """The exact failure mode this fix targets: espcoredump exits
        non-zero with 'GDB executable not found' (its own GDB_NOT_FOUND_ERROR
        message) against an ELF that actually DOES match the coredump.
        Before this fix that non-zero exit was indistinguishable from a
        genuine SHA256 mismatch to symbolize_coredump's caller. It must now
        surface as a distinctly-worded environment failure, never a
        content-mismatch verdict."""
        no_gdb_result = unittest.mock.Mock(
            returncode=1, stdout="",
            stderr="GDB executable not found. Please install GDB or set up ESP-IDF to complete the action.",
        )
        with unittest.mock.patch("subprocess.run", return_value=no_gdb_result), \
             unittest.mock.patch.object(coredump_fetch, "_resolve_gdb_path", return_value=None):
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                coredump_fetch.symbolize_coredump(
                    self.coredump_path, self.elf_path, idf_path=self.idf_dir,
                )
        msg = str(ctx.exception)
        self.assertNotIn("SHA256", msg)
        self.assertIn("GDB executable not found", msg)

    def test_find_matching_archived_elf_does_not_report_unsymbolizable_on_missing_gdb(self):
        """find_matching_archived_elf must abort loudly on the FIRST
        candidate's environment failure rather than trying every remaining
        candidate and reporting a false PERMANENTLY UNSYMBOLIZABLE -- the
        2026-09-16 contract this module already has for other environment
        markers, now also covering the GDB one."""
        elf2 = os.path.join(self.tmpdir, "KilnCtrl-second.elf")
        with open(elf2, "wb") as f:
            f.write(b"\x7fELF fake 2")
        no_gdb_result = unittest.mock.Mock(
            returncode=1, stdout="",
            stderr="GDB executable not found. Please install GDB or set up ESP-IDF to complete the action.",
        )
        with unittest.mock.patch("subprocess.run", return_value=no_gdb_result) as run_mock, \
             unittest.mock.patch.object(coredump_fetch, "_resolve_gdb_path", return_value=None):
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                coredump_fetch.find_matching_archived_elf(
                    self.coredump_path, [self.elf_path, elf2], idf_path=self.idf_dir,
                )
        msg = str(ctx.exception)
        self.assertNotIn("PERMANENTLY UNSYMBOLIZABLE", msg)
        self.assertIn("GDB executable not found", msg)
        # Aborted after the first candidate -- never tried the second.
        run_mock.assert_called_once()


class FindMatchingArchivedElfTests(unittest.TestCase):
    """Negative tests for the 2026-09-16 gap: `find_crash_elf()`/
    `read_esp_coredump()`'s old behaviour trusted the board's CURRENTLY
    RUNNING `fw_build` as the identity of whatever produced a given
    coredump. That is wrong for a coredump that outlived a later flash --
    the board's current fw_build names a DIFFERENT build than the one that
    actually wrote the dump. `find_matching_archived_elf()` fixes this by
    verifying against espcoredump's own SHA256 check instead of trusting any
    externally-reported identity.

    Each test proves this against the OLD code path too: `elf_archive.
    find_kiln_elf_for_build()` (unchanged, still the function
    `find_crash_elf()` calls) is exercised directly to show it confidently
    returns an ELF that does NOT match the coredump -- not merely that a
    new API is missing."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.coredump_path = os.path.join(self.tmpdir, "dump.bin")
        with open(self.coredump_path, "wb") as f:
            f.write(b"\x00" * 64)
        self.elf_running = os.path.join(self.tmpdir, "KilnCtrl-running.elf")  # what fw_build names
        self.elf_origin = os.path.join(self.tmpdir, "KilnCtrl-origin.elf")    # what actually produced the dump
        for p in (self.elf_running, self.elf_origin):
            with open(p, "wb") as f:
                f.write(b"\x7fELF fake")
        self.idf_dir = os.path.join(self.tmpdir, "idf")
        os.makedirs(os.path.join(self.idf_dir, "components", "espcoredump"), exist_ok=True)
        with open(os.path.join(self.idf_dir, "components", "espcoredump", "espcoredump.py"), "w") as f:
            f.write("# stub, never actually executed -- subprocess.run is mocked below\n")

    def _mock_subprocess_for(self, matching_elf_path: str):
        """A fake `subprocess.run` standing in for espcoredump: succeeds only
        when called against `matching_elf_path`, mismatches (like a real
        SHA256 refusal) against anything else."""
        def _run(cmd, **kwargs):
            elf_arg = cmd[-1]
            if elf_arg == matching_elf_path:
                return unittest.mock.Mock(returncode=0, stdout="Crashed task: profile_executor\n", stderr="")
            return unittest.mock.Mock(
                returncode=1, stdout="",
                stderr="Error: SHA256 of ELF file does not match the one stored in the core dump!",
            )
        return _run

    def test_old_lookup_by_running_fw_build_returns_an_elf_that_does_not_match(self):
        """Reproduces the real incident named in the task: the board's
        currently-running fw_build names `elf_running`, but the coredump was
        actually produced by `elf_origin` (a build that ran earlier and has
        since been superseded by a flash). The OLD code path -- calling
        `find_kiln_elf_for_build(fw_build)` and trusting its result outright
        -- hands back `elf_running`, which fails espcoredump's own SHA256
        check against this coredump. This is the "confidently wrong ELF"
        failure mode itself, demonstrated with the actual unchanged lookup
        function the old `find_crash_elf()`/`read_esp_coredump()` code used."""
        import kilnctrl.elf_archive as elf_archive
        orig_dir_fn = elf_archive.kiln_archive_dir
        archive_dir = os.path.join(self.tmpdir, "elf_archive")
        elf_archive.kiln_archive_dir = lambda: archive_dir
        try:
            elf_archive.archive_kiln_elf(self.elf_running, "Sep 12 2026 10:00:00", "aaaa1111", "test")
            path, _msg = elf_archive.find_kiln_elf_for_build("Sep 12 2026 10:00:00")
            self.assertIsNotNone(path, "the old lookup must still find AN entry -- it exists")

            # Confirm this OLD result is actually wrong for this coredump:
            # espcoredump itself would refuse it with a SHA256 mismatch.
            with unittest.mock.patch("subprocess.run", side_effect=self._mock_subprocess_for(self.elf_origin)):
                with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                    coredump_fetch.symbolize_coredump(
                        self.coredump_path, path, fw_build="Sep 12 2026 10:00:00", idf_path=self.idf_dir,
                    )
            self.assertIn("SHA256", str(ctx.exception))
        finally:
            elf_archive.kiln_archive_dir = orig_dir_fn

    def test_content_based_search_finds_the_true_origin_after_a_mismatch(self):
        """The fix: given both candidates (the wrongly-favored `elf_running`
        first, matching what a naive fw_build-keyed lookup would try, and
        the true `elf_origin` second), `find_matching_archived_elf` must
        skip the mismatching one and return the one that actually matches --
        never stopping at the first candidate's failure and never silently
        keeping the wrong guess."""
        with unittest.mock.patch("subprocess.run", side_effect=self._mock_subprocess_for(self.elf_origin)):
            elf_path, out = coredump_fetch.find_matching_archived_elf(
                self.coredump_path, [self.elf_running, self.elf_origin], idf_path=self.idf_dir,
            )
        self.assertEqual(elf_path, self.elf_origin)
        self.assertIn("profile_executor", out)

    def test_no_candidate_matches_reports_permanently_unsymbolizable(self):
        """When the true origin build was never archived at all (a
        legitimate, permanent outcome -- explicitly called out by the task:
        one such dump exists on the bench board right now), every candidate
        fails the SHA256 check. This must be reported as a distinctly
        labeled, non-recoverable result -- not a plain error indistinguishable
        from a tooling/environment problem, and never a silent fallback to
        any of the candidates tried."""
        def _always_mismatch(cmd, **kwargs):
            return unittest.mock.Mock(
                returncode=1, stdout="",
                stderr="Error: SHA256 of ELF file does not match the one stored in the core dump!",
            )
        with unittest.mock.patch("subprocess.run", side_effect=_always_mismatch):
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                coredump_fetch.find_matching_archived_elf(
                    self.coredump_path, [self.elf_running, self.elf_origin], idf_path=self.idf_dir,
                )
        msg = str(ctx.exception)
        self.assertIn("PERMANENTLY UNSYMBOLIZABLE", msg)
        self.assertIn(self.elf_running, msg)
        self.assertIn(self.elf_origin, msg)

    def test_environment_failure_aborts_search_instead_of_reporting_no_match(self):
        """A broken toolchain (wrong interpreter, no esp_coredump installed)
        must never be reported as "none of the archived ELFs match" -- that
        conflates an environment problem with a verdict about the data,
        exactly this repo's recurring failure mode (2026-09-16 finding in
        coredump_fetch.py: a ModuleNotFoundError and a genuine SHA256
        mismatch produced visually similar "exit 1" results). The search
        must abort on the FIRST candidate's environment failure rather than
        burning through every candidate and then reporting a misleading
        "no match" verdict."""
        module_not_found = unittest.mock.Mock(
            returncode=1, stdout="",
            stderr="Traceback (most recent call last):\nModuleNotFoundError: No module named 'esp_coredump'",
        )
        with unittest.mock.patch("subprocess.run", return_value=module_not_found) as run_mock:
            with self.assertRaises(coredump_fetch.CoredumpSymbolizeError) as ctx:
                coredump_fetch.find_matching_archived_elf(
                    self.coredump_path, [self.elf_running, self.elf_origin], idf_path=self.idf_dir,
                )
            # Aborted after the first candidate -- did not burn through both.
            run_mock.assert_called_once()
        msg = str(ctx.exception)
        self.assertIn("ModuleNotFoundError", msg)
        self.assertNotIn("PERMANENTLY UNSYMBOLIZABLE", msg)


if __name__ == "__main__":
    unittest.main()
