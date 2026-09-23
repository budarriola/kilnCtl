#!/usr/bin/env python3
"""crash_report_clear_http_client.py -- pure HTTP client for
POST /api/crash_report/clear
(firmware/KilnFW/App/drivers/http/diagnostics_http.c's
crash_report_clear_post_handler(), ROUTE_TIER_ADMIN per
route_tier_table.h), which acknowledges the board's last-crash record AND
erases the coredump image plus this module's own NVS record
(crash_report_clear() -> crash_report_acknowledge() then
hal_sysinfo_coredump_erase()), freeing the `coredump` partition slot for
the next crash -- unlike ``/api/crash_report/ack``
(crash_report_ack_http_client.py), which only marks the record reviewed
and leaves the coredump image in place.

Same "stdlib urllib.request, no framework" convention as
crash_report_ack_http_client.py, in its own module for the same reason:
kept apart from dashboard_http_client.get_crash_report() (the read side)
so the read path keeps its "no function here can change board state"
guarantee.

POST /api/crash_report/clear takes no request body and no form fields --
crash_report_clear_post_handler() reads nothing off the request. It
answers:
  * ``{"ok":true}`` (200) -- acknowledged and erased.
  * ``{"ok":false,"error":"<esp_err_to_name(...)>"}`` (500) -- either the
    NVS erase of the crash record or the coredump erase failed (the
    handler does not distinguish which, and does not distinguish "nothing
    was present to clear" as a separate case the way the /ack route's 409
    does -- crash_report_clear() unconditionally attempts the erase).
Both non-2xx cases raise CrashReportClearHttpError (via urllib's own
HTTPError-on-non-2xx behaviour) with `.status`/`.detail` carrying the
board's own JSON body.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request

from . import http_auth
from typing import Optional

CRASH_REPORT_CLEAR_HTTP_TIMEOUT_S = 8.0

_API_PATH = "/api/crash_report/clear"


class CrashReportClearHttpError(Exception):
    """Any transport or protocol failure talking to
    POST /api/crash_report/clear -- unreachable host, non-2xx (including
    the handler's own 500 "erase failed" body), or a response shape this
    client does not understand."""

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


def post_crash_report_clear(host: str, timeout: float = CRASH_REPORT_CLEAR_HTTP_TIMEOUT_S) -> dict:
    """POST /api/crash_report/clear. No request body.

    Returns the decoded JSON body for a 200 response (always ``{"ok":true}``
    per the handler). Raises CrashReportClearHttpError for every non-2xx
    answer -- including the handler's own 500 "erase failed" body, which
    urllib raises as HTTPError before this function ever sees a parsed
    dict -- so a caller must inspect the raised error's
    `.status`/`.detail`. Goes through :mod:`http_auth`
    (KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD), same as every other write
    tool that talks to an ADMIN-tier route."""
    req = urllib.request.Request(_url(host), data=b"", method="POST")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise CrashReportClearHttpError(f"POST {_API_PATH} refused: HTTP {status}: {detail}",
                                         status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise CrashReportClearHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise CrashReportClearHttpError(
            f"POST {_API_PATH} returned 200 but the body was not JSON: {text!r}") from exc
