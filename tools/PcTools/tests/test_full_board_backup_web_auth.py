#!/usr/bin/env python3
"""Tests for full_board_backup.py's web-auth handling.

Context: since d25d5ccf/f3991c09 a large share of the routes this script
reads and posts (zones, kiln_configs, status, ramp_assist,
display_power, cfgfs, ...) are ROUTE_TIER_ADMIN and answer 401 without a
session cookie. Before this change every call in the script went straight
through ``urllib.request.urlopen`` and had no way to log in at all -- the
same "redaction blinded the tooling" class http_auth.py's own module
docstring names. This file proves every call site now goes through
``kilnctrl.http_auth.urlopen`` (log in once on 401, retry once, report a
persisting 401 as a loud failure that is never saved as backup content),
without any live board or real socket -- ``urllib.request.urlopen`` is
mocked throughout, same convention as the other full_board_backup tests.

Run with:
    tools\\PcTools\\.venv\\Scripts\\python -m pytest tools/PcTools/tests/test_full_board_backup_web_auth.py
"""
from __future__ import annotations

import io
import json
import sys
import unittest.mock
import urllib.error
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import full_board_backup as fbb  # noqa: E402
from kilnctrl import http_auth  # noqa: E402


class _FakeResponse:
    def __init__(self, body: bytes, status: int = 200, cookie: str | None = None):
        self._body = body
        self.status = status
        self._cookie = cookie

    def read(self):
        return self._body

    def info(self):
        class _Headers:
            def __init__(self, cookie):
                self._cookie = cookie

            def get_all(self, name):
                if name == "Set-Cookie" and self._cookie:
                    return [f"kiln_sid={self._cookie}; Path=/"]
                return None

        return _Headers(self._cookie)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def setup_function(_fn=None):
    # http_auth's remembered-session table is process-lifetime, module-level
    # state -- clear it before and after every test so one test's login
    # cannot leak a session into the next.
    http_auth.clear_sessions()


def teardown_function(_fn=None):
    http_auth.clear_sessions()


def _set_creds(monkeypatch, username="op", password="secret"):
    monkeypatch.setenv(http_auth.USERNAME_ENV, username)
    monkeypatch.setenv(http_auth.PASSWORD_ENV, password)


def _clear_creds(monkeypatch):
    monkeypatch.delenv(http_auth.USERNAME_ENV, raising=False)
    monkeypatch.delenv(http_auth.PASSWORD_ENV, raising=False)


def test_get_json_logs_in_once_on_401_and_retries():
    """A GET that first answers 401 (ADMIN tier, no session yet) must log in
    through POST /api/auth/login and reissue the SAME GET exactly once,
    succeeding on the retry -- proving _get_json() now goes through
    http_auth.urlopen() rather than a bare urllib call that would just raise
    HTTPError 401 straight back to the caller."""
    import os

    os.environ[http_auth.USERNAME_ENV] = "op"
    os.environ[http_auth.PASSWORD_ENV] = "secret"
    try:
        calls = []

        def fake_urlopen(req_or_url, timeout=None):
            url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
            calls.append(url)
            if url.endswith("/api/auth/login"):
                return _FakeResponse(b"", cookie="sess-abc")
            if url.endswith("/api/zones"):
                req = req_or_url if not isinstance(req_or_url, str) else None
                has_cookie = req is not None and req.has_header("Cookie")
                if not has_cookie:
                    raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, io.BytesIO(b""))
                return _FakeResponse(json.dumps({"zones": []}).encode())
            raise AssertionError(f"unexpected url {url}")

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            data, err = fbb._get_json("http://10.0.0.5/api/zones", 5.0)

        assert err is None, err
        assert data == {"zones": []}
        assert any(c.endswith("/api/auth/login") for c in calls)
    finally:
        del os.environ[http_auth.USERNAME_ENV]
        del os.environ[http_auth.PASSWORD_ENV]


def test_get_json_reports_missing_credentials_as_loud_failure_not_a_crash():
    """No credential in the environment + a 401: http_auth.urlopen() raises
    HttpAuthError. _get_json() must turn that into a (None, error) result --
    reported, not swallowed and not an uncaught exception that would crash
    the whole backup run over one endpoint."""
    import os

    os.environ.pop(http_auth.USERNAME_ENV, None)
    os.environ.pop(http_auth.PASSWORD_ENV, None)

    def fake_urlopen(req_or_url, timeout=None):
        url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
        raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, io.BytesIO(b""))

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        data, err = fbb._get_json("http://10.0.0.5/api/zones", 5.0)

    assert data is None
    assert err is not None
    assert "authentication failed" in err
    assert http_auth.USERNAME_ENV in err and http_auth.PASSWORD_ENV in err


def test_a_persisting_401_is_never_saved_as_backup_content(monkeypatch, tmp_path):
    """End-to-end through main(): every route always answers 401 (login
    itself succeeds, but the retried request is STILL refused -- e.g. an
    admin session that cannot actually pass this route's tier). The archive
    written to disk must show NO endpoint payloads and the error list must
    name the authentication failure; main() must return non-zero because a
    required endpoint failed."""
    _set_creds(monkeypatch)

    def fake_urlopen(req_or_url, timeout=None):
        url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
        if url.endswith("/api/auth/login"):
            return _FakeResponse(b"", cookie="sess-abc")
        raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, io.BytesIO(b""))

    argv = ["full_board_backup.py", "--host", "10.0.0.5", "--out-dir", str(tmp_path)]
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen), \
         unittest.mock.patch.object(sys, "argv", argv):
        rc = fbb.main()

    assert rc == 1, "a required endpoint stuck at 401 must fail the whole backup run"
    archive = json.loads((tmp_path / "board_backup.json").read_text(encoding="utf-8"))
    assert archive["endpoints"] == {}, "a persisting-401 endpoint must never be saved as backup content"
    assert archive["errors"], "the 401 must be recorded, not silently dropped"
    # http_auth.urlopen() logs in once and retries once; a 401 on the RETRY
    # (the "login succeeded but this session still can't pass this route"
    # case) propagates as a plain HTTPError rather than HttpAuthError -- see
    # http_auth.urlopen()'s own "exactly one login, exactly one retry"
    # contract -- so the recorded error names the HTTP status, not the
    # "authentication failed" wording used for a missing/refused credential.
    assert any("401" in e["error"] for e in archive["errors"])


def test_missing_credentials_never_crashes_main_and_is_reported(monkeypatch, tmp_path):
    """No credential set at all: main() must still run to completion (never
    an uncaught HttpAuthError escaping to the top level), report every
    endpoint as failed with the actionable "not set in the environment"
    message, and write an archive with no endpoint content."""
    _clear_creds(monkeypatch)

    def fake_urlopen(req_or_url, timeout=None):
        url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
        raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, io.BytesIO(b""))

    argv = ["full_board_backup.py", "--host", "10.0.0.5", "--out-dir", str(tmp_path)]
    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen), \
         unittest.mock.patch.object(sys, "argv", argv):
        rc = fbb.main()

    assert rc == 1
    archive = json.loads((tmp_path / "board_backup.json").read_text(encoding="utf-8"))
    assert archive["endpoints"] == {}
    assert any("not set in the environment" in e["error"] for e in archive["errors"])
    # No credential value can leak into the archive -- nothing resembling a
    # password/username string should ever be written.
    raw = (tmp_path / "board_backup.json").read_text(encoding="utf-8")
    assert "secret" not in raw


def test_get_bytes_reports_auth_failure(monkeypatch):
    """_get_bytes() (the cfgfs file fetch path) must handle HttpAuthError the
    same way _get_json() does."""
    _clear_creds(monkeypatch)

    def fake_urlopen(req_or_url, timeout=None):
        url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
        raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, io.BytesIO(b""))

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        data, err = fbb._get_bytes("http://10.0.0.5/api/cfgfs/file?name=x", 5.0)

    assert data is None
    assert err is not None and "authentication failed" in err


def test_restore_full_post_reports_auth_failure_as_domain_failure(monkeypatch):
    """restore_full()'s _post() helper (used for /api/backup/import,
    /api/kiln_configs/import, /api/relay_cycles/restore, /api/ramp_assist,
    /api/settings/display_power, /api/unit_pref, /api/settings/tz) must
    record a persisting 401 as a named domain failure, not crash the whole
    restore or silently mark it restored."""
    _clear_creds(monkeypatch)

    def fake_urlopen(req_or_url, timeout=None):
        url = req_or_url if isinstance(req_or_url, str) else req_or_url.full_url
        method = "GET" if isinstance(req_or_url, str) else req_or_url.get_method()
        if url.endswith("/api/profile_exec"):
            return _FakeResponse(json.dumps({"state": "idle"}).encode())
        if url.endswith("/api/autotune"):
            return _FakeResponse(json.dumps({"state": "idle"}).encode())
        if url.endswith("/api/status"):
            return _FakeResponse(json.dumps({
                "io_ready": True,
                "relays": [{"relay": n, "on": False} for n in range(1, 6)],
                "safety_relay_energized": False,
                "relay_life": [],
            }).encode())
        if method == "POST":
            raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, io.BytesIO(b""))
        raise AssertionError(f"unexpected GET {url}")

    archive = {
        "endpoints": {
            "/api/backup/export": {"zones": [], "profiles": []},
            "/api/status": {"temp_unit": "F", "time_tz": "UTC"},
        },
        "kiln_config_exports": {},
        "cfgfs_files": {},
    }

    with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
        result = fbb.restore_full("10.0.0.5", archive, timeout=5.0, dry_run=False)

    assert result["refused"] is None
    entry = next(e for e in result["failed"] if e["domain"] == "zones_config_and_profiles")
    assert "authentication failed" in entry["detail"]


if __name__ == "__main__":
    import pytest

    raise SystemExit(pytest.main([__file__, "-v"]))
