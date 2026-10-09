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

2026-10-02 (ROADMAP A3): the handler now hands the erase to http_async_job so
httpd_worker is not stalled ~3.4 s by it; the reply (same 200 ``{"ok":true}``
/ 500 bodies as before) is sent by the job task once the erase is done. A
second job already running answers 503 (CrashReportClearBusy). A 202 reply
(accepted, not yet done) is also understood and returned with
``accepted: true``; a socket timeout raises CrashReportClearTimeout -- in
both cases the caller polls GET /api/crash_report (its present:false reply
carries ``clear_in_progress``) and /api/coredump/info for completion.

POST /api/crash_report/clear takes no request body and no form fields --
crash_report_clear_post_handler() reads nothing off the request. It
answers:
  * ``{"ok":true}`` (200) -- the COREDUMP erase succeeded. Note this does
    NOT promise the NVS crash record was erased: crash_report_clear()
    (crash_report.c) only ESP_LOGWs a failed hal_kv_erase_key()/commit()
    and still returns ESP_OK, so a caller must confirm by re-reading
    GET /api/crash_report rather than trusting this body -- which is what
    mcp_server_info.crash_report_clear()'s read-back is for.
  * ``{"ok":false,"error":"<esp_err_to_name(...)>"}`` (500) -- the
    hal_sysinfo_coredump_erase() call failed; that is the ONLY failure
    crash_report_clear() propagates. The route has no "nothing was present
    to clear" status of its own the way the /ack route's 409 does --
    crash_report_clear() unconditionally attempts the erase.
The 500 (like any other non-2xx) raises CrashReportClearHttpError (via
urllib's own HTTPError-on-non-2xx behaviour) with `.status`/`.detail`
carrying the board's own JSON body.
"""
from __future__ import annotations

import json
import socket
import urllib.error
import urllib.request

from . import http_auth
from typing import Optional

# The board replies only once the coredump erase is done (about 3.4 s on the
# bench, 2026-10-02), on the http_async_job task -- httpd_worker itself stays
# free, but THIS request still waits for that reply, so the old 8 s budget
# was too tight for a slow flash; 20 s leaves margin without being unbounded.
CRASH_REPORT_CLEAR_HTTP_TIMEOUT_S = 20.0

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


class CrashReportClearBusy(CrashReportClearHttpError):
    """The board answered 503: another http_async_job is already running
    (ct_auto_zero, bench_preset, a previous clear). Nothing was started --
    safe to retry."""


class CrashReportClearTimeout(CrashReportClearHttpError):
    """The POST's own socket timed out waiting for the reply. The board may
    well still be erasing; the caller must poll the read-back, not assume
    failure or success."""


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
            status_code = getattr(resp, "status", 200)
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        if status == 503:
            raise CrashReportClearBusy(f"POST {_API_PATH} busy: HTTP 503: {detail}", status, detail) from exc
        raise CrashReportClearHttpError(f"POST {_API_PATH} refused: HTTP {status}: {detail}",
                                         status, detail) from exc
    except (socket.timeout, TimeoutError) as exc:
        raise CrashReportClearTimeout(f"POST {_API_PATH} timed out after {timeout}s") from exc
    except urllib.error.URLError as exc:
        if isinstance(exc.reason, (socket.timeout, TimeoutError)):
            raise CrashReportClearTimeout(f"POST {_API_PATH} timed out after {timeout}s") from exc
        _, detail = _http_error_detail(exc)
        raise CrashReportClearHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        body = json.loads(text)
    except Exception as exc:
        raise CrashReportClearHttpError(
            f"POST {_API_PATH} returned {status_code} but the body was not JSON: {text!r}") from exc
    if status_code == 202 and isinstance(body, dict):
        body.setdefault("accepted", True)  # async-accepted: caller must poll the read-back
    return body
