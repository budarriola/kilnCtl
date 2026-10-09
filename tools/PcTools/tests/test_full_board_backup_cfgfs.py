#!/usr/bin/env python3
"""Tests for full_board_backup.py's cfg-filesystem coverage (docs/FILESYSTEM.md
"Add filesystem coverage to the backup").

Context: user data is moving from NVS to files on the `cfg` LittleFS
partition. The pre-existing backup script only ever pulled HTTP JSON
endpoints (zones/profiles/status/...) -- none of that captured the
filesystem's own contents, so a backup taken after the dual-write window
closes (NVS copies dropped) would have nothing to restore FROM if the file
copy were ever lost. This pass added:

  * GET /api/cfgfs/file?name=<name> (firmware, diagnostics_http.c) -- raw
    bytes of one named cfg file.
  * POST /api/cfgfs/file?name=<name> (firmware) -- writes raw bytes back.
  * _capture_cfgfs_files() / restore_cfgfs_files() (this script) -- glue
    that lists files via the EXISTING /api/cfgfs status endpoint (no
    parallel listing surface), fetches each one, and can validate-then-write
    them back.

These tests prove, without any live board or real socket (urllib.request is
mocked throughout, same convention as test_run_queue_pending_restore.py):

  1. capture -> archive -> restore round-trips a FILE-backed board's cfg
     filesystem byte-for-byte (the case that matters most per this task).
  2. an unmounted/empty `cfg` partition (today's live-board reality) is
     handled as an expected empty section, not an error.
  3. NEGATIVE TEST: a corrupted/truncated archive field (bad base64, and
     separately a size_bytes mismatch) makes restore_cfgfs_files() REFUSE
     the whole restore and write nothing -- proven by asserting zero POSTs
     were issued, not just by reading the return value.

Run with: uv run pytest tools/PcTools/tests/test_full_board_backup_cfgfs.py
"""
from __future__ import annotations

import base64
import io
import sys
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import full_board_backup as fbb  # noqa: E402


class _FakeResponse:
    """Minimal stand-in for the object urllib.request.urlopen()'s context
    manager yields -- only .read() is used by _get_bytes()/_get_json()."""

    def __init__(self, body: bytes):
        self._body = body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _make_fake_urlopen(files: dict, post_log: list):
    """Builds a urlopen() replacement serving GET /api/cfgfs/file?name=X from
    `files` (name -> bytes) and recording every POST's (name, body) into
    `post_log` before answering 200 OK with an empty body -- close enough to
    the real firmware handler's behavior for this script's purposes (it only
    ever reads the response to drain the socket, never parses the POST
    reply)."""

    def fake_urlopen(req_or_url, timeout=None):
        if isinstance(req_or_url, str):
            url = req_or_url
            method = "GET"
            body = None
        else:
            url = req_or_url.full_url
            method = req_or_url.get_method()
            body = req_or_url.data
        assert "/api/cfgfs/file?name=" in url, f"unexpected URL in test: {url}"
        name = url.split("name=")[1]
        import urllib.parse

        name = urllib.parse.unquote(name)
        if method == "POST":
            post_log.append((name, body))
            return _FakeResponse(b"{\"ok\":true}")
        if name not in files:
            import urllib.error

            raise urllib.error.URLError("404")
        return _FakeResponse(files[name])

    return fake_urlopen


def test_capture_then_restore_round_trips_file_backed_board_byte_for_byte():
    """The case that matters most: a board whose data lives in FILES
    (cfg_fs mounted, files present) is backed up and restored correctly."""
    board_files = {
        "zones_config.json": b'{"pid_kp":12.5,"coupling_c1":0.0731}',
        "unit_pref.dat": bytes([0, 0, 0, 7]) + b"\x01",  # rev=7, Fahrenheit
        "profiles.json": b'{"profiles":[]}',
    }
    cfgfs_status = {
        "mounted": True,
        "files": [{"name": n, "size_bytes": len(b)} for n, b in board_files.items()],
    }

    post_log: list = []
    fake_urlopen = _make_fake_urlopen(board_files, post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        cfgfs_files, errors = fbb._capture_cfgfs_files("10.0.0.5", 5.0, cfgfs_status)
        assert errors == []
        assert set(cfgfs_files) == set(board_files)
        for name, raw in board_files.items():
            assert base64.b64decode(cfgfs_files[name]["data_base64"]) == raw
            assert cfgfs_files[name]["size_bytes"] == len(raw)

        # Restore onto a (simulated) board -- prove every byte round-trips.
        ok, message, decoded = fbb.restore_cfgfs_files("10.0.0.5", cfgfs_files, 5.0, dry_run=False)
        assert ok, message
        assert decoded == board_files
        posted = dict(post_log)
        assert posted == board_files, "POST bodies must be byte-identical to the captured files"


def test_unmounted_cfg_partition_is_empty_not_an_error():
    """Today's live-board reality (docs/FILESYSTEM.md: `cfg` is
    UNFORMATTED) -- must not be reported as a failure."""
    cfgfs_files, errors = fbb._capture_cfgfs_files("10.0.0.5", 5.0, {"mounted": False, "reason": "not mounted"})
    assert cfgfs_files == {}
    assert errors == []

    cfgfs_files, errors = fbb._capture_cfgfs_files("10.0.0.5", 5.0, None)
    assert cfgfs_files == {}
    assert errors == []


def test_restore_refuses_on_corrupted_base64_and_writes_nothing():
    """NEGATIVE TEST 1: truncate/corrupt one entry's data_base64 (invalid
    base64 padding). restore_cfgfs_files() must refuse the WHOLE restore and
    issue ZERO POSTs -- proven by asserting the POST log, not just the
    return value."""
    cfgfs_files = {
        "zones_config.json": {"data_base64": base64.b64encode(b"good file").decode(), "size_bytes": 9},
        "unit_pref.dat": {"data_base64": "not-valid-base64!!!", "size_bytes": 5},
    }
    post_log: list = []
    fake_urlopen = _make_fake_urlopen({}, post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        ok, message, decoded = fbb.restore_cfgfs_files("10.0.0.5", cfgfs_files, 5.0, dry_run=False)

    assert ok is False
    assert "unit_pref.dat" in message and "not valid base64" in message
    assert decoded == {}
    assert post_log == [], "corrupted archive field must refuse before any file is written"


def test_restore_refuses_on_size_mismatch_and_writes_nothing():
    """NEGATIVE TEST 2: size_bytes disagrees with the decoded length (a
    truncated archive field that still happens to be valid base64).
    Same all-or-nothing refusal as the corrupted-base64 case."""
    cfgfs_files = {
        "zones_config.json": {"data_base64": base64.b64encode(b"twelve bytes").decode(), "size_bytes": 999},
    }
    post_log: list = []
    fake_urlopen = _make_fake_urlopen({}, post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        ok, message, decoded = fbb.restore_cfgfs_files("10.0.0.5", cfgfs_files, 5.0, dry_run=False)

    assert ok is False
    assert "size_bytes=999" in message
    assert decoded == {}
    assert post_log == []


def test_restore_dry_run_validates_but_writes_nothing():
    cfgfs_files = {
        "zones_config.json": {"data_base64": base64.b64encode(b"hello").decode(), "size_bytes": 5},
    }
    post_log: list = []
    fake_urlopen = _make_fake_urlopen({}, post_log)

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        ok, message, decoded = fbb.restore_cfgfs_files("10.0.0.5", cfgfs_files, 5.0, dry_run=True)

    assert ok is True
    assert decoded == {"zones_config.json": b"hello"}
    assert post_log == [], "dry run must never write"


if __name__ == "__main__":
    import pytest

    raise SystemExit(pytest.main([__file__, "-v"]))
