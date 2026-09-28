#!/usr/bin/env python3
"""backup_export_http_client.py -- pure HTTP client for GET /api/backup/export
(firmware/KilnFW/App/drivers/http/backup_export.c's
backup_export_get_handler(), ROUTE_TIER_ADMIN per route_tier_table.h).

READ-ONLY. The board streams a JSON document
(``{"kind":"kilnctl_backup","version":N,"profiles":[...],"zones":[...],
"kiln_configs":[...]}``) via httpd_resp_send_chunk() -- see backup_export.c's
own header comment for why it streams rather than building one buffer. This
client reads the whole body (bounded by BACKUP_EXPORT_MAX_BYTES, generous
headroom over anything a real board produces) and returns the raw text
alongside the parsed JSON, so a caller can both save the exact bytes the
board sent and inspect its shape without re-serializing.

Wi-Fi credentials and the web admin password are DELIBERATELY never part of
this document (backup_http.h's header comment explains why -- a restored
board should not silently join, or overwrite web auth, on a different
board's say-so). Nothing here assumes that always holds true, though:
``scan_for_sensitive_fields()`` below does a substring scan of the raw text
for "wifi"/"password"/"ssid"/"passphrase" so a caller can report presence as
a bare bool without ever having to print or log the matched text itself.

No POST here -- that is backup_import_http_client.py.

Same "stdlib urllib.request, no framework" convention as the other bench
HTTP clients in this package, going through :mod:`http_auth` for the ADMIN
session seam every other admin-tier tool here uses.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Optional

from . import http_auth

BACKUP_EXPORT_HTTP_TIMEOUT_S = 15.0

# Generous headroom: PROFILES_MAX_COUNT/MAX31856_CHANNEL_COUNT/KILN_CFG_MAX_COUNT
# are all small fixed bounds on the board side, so a real export is at most a
# few hundred KB even with every slot full. A response larger than this is
# treated as a protocol mismatch, not trusted blindly.
BACKUP_EXPORT_MAX_BYTES = 8 * 1024 * 1024

_API_PATH = "/api/backup/export"

#: Case-insensitive substrings that, if found anywhere in the raw export
#: text, indicate a credential-shaped field slipped in -- never expected
#: given backup_http.h's design (Wi-Fi/web-auth are deliberately excluded),
#: but this is a load-bearing safety net for a caller that must never print
#: what it found, only whether it found anything.
_SENSITIVE_MARKERS = ("wifi", "ssid", "password", "passphrase", "psk")


class BackupExportHttpError(Exception):
    """Any transport or protocol failure talking to GET /api/backup/export
    -- unreachable host, non-2xx, a response too large to be a real export,
    or a body that isn't valid JSON. `.status`/`.detail` carry the board's
    own reported status code and body when available."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str) -> str:
    return f"http://{host}{_API_PATH}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace")
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def scan_for_sensitive_fields(raw_text: str) -> bool:
    """True if the raw export text contains any Wi-Fi/password-shaped
    substring. Never returns which one, or where -- callers must report
    this as ``[bool]`` only, per this project's credential-handling rule
    (CLAUDE.md/COMMON.md): a backup export is not expected to carry any of
    these, and finding one is a "stop and look, do not print it" signal,
    not routine data."""
    lower = raw_text.lower()
    return any(marker in lower for marker in _SENSITIVE_MARKERS)


def get_export(host: str, timeout: float = BACKUP_EXPORT_HTTP_TIMEOUT_S) -> "tuple[str, dict]":
    """GET /api/backup/export. Returns ``(raw_text, parsed)`` -- the exact
    body text (for saving to a file byte-for-byte) and its parsed JSON dict
    (for reporting version/section counts). Raises BackupExportHttpError for
    a transport failure, non-2xx, an oversized body, or a body that is not
    valid JSON or is missing the ``kind``/``version`` fields
    backup_export_get_handler() always emits first."""
    req = urllib.request.Request(_url(host), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            raw = resp.read(BACKUP_EXPORT_MAX_BYTES + 1)
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise BackupExportHttpError(f"GET {_API_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise BackupExportHttpError(f"GET {_API_PATH} unreachable: {detail}") from exc
    except (http_auth.HttpAuthError, OSError) as exc:
        raise BackupExportHttpError(f"GET {_API_PATH} failed: {exc}") from exc

    if len(raw) > BACKUP_EXPORT_MAX_BYTES:
        raise BackupExportHttpError(
            f"GET {_API_PATH} body exceeds {BACKUP_EXPORT_MAX_BYTES} bytes -- refusing to trust "
            f"this as a genuine export")

    raw_text = raw.decode("utf-8", errors="replace")
    try:
        parsed = json.loads(raw_text)
    except Exception as exc:
        raise BackupExportHttpError(
            f"GET {_API_PATH} returned 200 but the body was not JSON") from exc
    if not isinstance(parsed, dict) or parsed.get("kind") != "kilnctl_backup" or "version" not in parsed:
        raise BackupExportHttpError(
            f"GET {_API_PATH} response is not a kilnctl_backup document (missing/wrong "
            f"\"kind\"/\"version\")")
    return raw_text, parsed
