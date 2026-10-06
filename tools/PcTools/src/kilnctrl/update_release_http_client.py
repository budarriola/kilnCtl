#!/usr/bin/env python3
"""update_release_http_client.py -- pure HTTP client for the application's
GitHub-release fetch routes (firmware/KilnFW/App/drivers/update/update_fetch.c
and update_settings_http.c, WP8/WP9 of docs/GITHUB_RELEASE_UPDATE_PLAN.md),
all ROUTE_TIER_ADMIN:

  GET  /api/update/settings        {"ok","repo","default_repo","is_default"}
  POST /api/update/settings        form body ``repo=owner/name`` ('' resets)
  POST /api/update/check           202 {"ok":true,"started":true}, async
  POST /api/update/download        202, async; query allow_downgrade /
                                   confirm_downgrade / allow_prerelease / force
  GET  /api/update/fetch           job status: state idle|checking|downloading|
                                   done|failed, error, http_status, verdict, ...
  POST /api/update/fetch/cancel

Written for the OT-G bench cases (bench_test/cases_ota.py); the MCP update
tools (mcp_server_update.py) do not wrap these routes yet. Same convention as
update_http_client.py, whose request helper and UpdateHttpError it reuses:
http_auth.urlopen(), no socket in unit tests, credentials never echoed.
"""
from __future__ import annotations

import urllib.parse
import urllib.request
from typing import Optional

from . import update_http_client as _u
from .update_http_client import UpdateHttpError, error_name  # noqa: F401  (re-exported)

SETTINGS_PATH = "/api/update/settings"
CHECK_PATH = "/api/update/check"
DOWNLOAD_PATH = "/api/update/download"
FETCH_PATH = "/api/update/fetch"
FETCH_CANCEL_PATH = "/api/update/fetch/cancel"
FETCH_TIMEOUT_S = 15.0


def get_settings(host: str, timeout: float = FETCH_TIMEOUT_S) -> dict:
    req = urllib.request.Request(_u._url(host, SETTINGS_PATH), method="GET")
    return _u._request(req, SETTINGS_PATH, timeout)


def set_repo(host: str, repo: str, timeout: float = FETCH_TIMEOUT_S) -> dict:
    """POST /api/update/settings repo=<repo>; an empty string resets to the default."""
    body = urllib.parse.urlencode({"repo": repo}).encode("ascii")
    req = urllib.request.Request(_u._url(host, SETTINGS_PATH), data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    return _u._request(req, SETTINGS_PATH, timeout)


def start_check(host: str, timeout: float = FETCH_TIMEOUT_S) -> dict:
    req = urllib.request.Request(_u._url(host, CHECK_PATH), data=b"", method="POST")
    return _u._request(req, CHECK_PATH, timeout)


def start_download(host: str, allow_downgrade: bool = False, confirm_downgrade: str = "",
                   allow_prerelease: bool = False, force: bool = False,
                   timeout: float = FETCH_TIMEOUT_S) -> dict:
    q = {}
    if allow_downgrade:
        q["allow_downgrade"] = "1"
    if confirm_downgrade:
        q["confirm_downgrade"] = confirm_downgrade
    if allow_prerelease:
        q["allow_prerelease"] = "1"
    if force:
        q["force"] = "1"
    path = DOWNLOAD_PATH + ("?" + urllib.parse.urlencode(q) if q else "")
    req = urllib.request.Request(_u._url(host, path), data=b"", method="POST")
    return _u._request(req, DOWNLOAD_PATH, timeout)


def get_fetch_status(host: str, timeout: float = FETCH_TIMEOUT_S) -> dict:
    req = urllib.request.Request(_u._url(host, FETCH_PATH), method="GET")
    data = _u._request(req, FETCH_PATH, timeout)
    if "state" not in data:
        raise UpdateHttpError(f"GET {FETCH_PATH} response lacks state: {data!r}")
    return data


def upload_stage_truncated(host: str, image: bytes, send_fraction: float = 0.6,
                           timeout: float = 60.0) -> "tuple[Optional[int], str]":
    """POST /api/update/stage declaring the FULL Content-Length but sending
    only ``send_fraction`` of the bytes, then half-closing the socket: a real
    cut-off upload (urllib cannot do this, it always sends what it declares).
    Returns (HTTP status, body) if the board answers before closing, else
    (None, "<ExcName>: ...") for a transport-level close/timeout. Uses the
    ADMIN session from http_auth. Bench-untested (written for OT-G02)."""
    import http.client
    import socket
    from . import http_auth
    err = _u.validate_upload_args(image)
    if err:
        raise UpdateHttpError(f"refusing to upload: {err}")
    cookie = http_auth.login(f"http://{host}", timeout)
    cut = max(1, min(len(image) - 1, int(len(image) * send_fraction)))
    conn = http.client.HTTPConnection(host, timeout=timeout)
    try:
        conn.putrequest("POST", _u.STAGE_PATH)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(image)))
        conn.putheader("Cookie", f"kiln_sid={cookie}")
        conn.endheaders()
        conn.send(image[:cut])
        try:
            conn.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        resp = conn.getresponse()
        return resp.status, resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        return None, f"{type(exc).__name__}: {exc}"
    finally:
        conn.close()
