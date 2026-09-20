#!/usr/bin/env python3
"""crash_report_ack_http_client.py -- pure HTTP client for
POST /api/crash_report/ack
(firmware/KilnFW/App/drivers/http/diagnostics_http.c's
crash_report_ack_post_handler(), ROUTE_TIER_ADMIN per route_tier_table.h),
which marks the board's last-crash record as reviewed
(crash_report_acknowledge()) without erasing the coredump image (that is
``/api/crash_report/clear``, a different, still-clientless route this
module deliberately does not touch).

Same "stdlib urllib.request, no framework" convention as
safety_log_level_http_client.py/safety_cfg_http_client.py, in its own
module for the same reason dashboard_http_client.py's own module docstring
gives for NOT adding a POST here: "a live crash report must stay exactly as
unacknowledged as the board reports it until a person (or an explicit,
separately-invoked tool) decides otherwise." This module IS that explicit,
separately-invoked tool's HTTP client -- kept apart from
dashboard_http_client.get_crash_report() (the read side) on purpose, so the
read path keeps its "no function here can change board state" guarantee.

POST /api/crash_report/ack takes no request body and no form fields --
crash_report_ack_post_handler() reads nothing off the request. It answers:
  * ``{"ok":true}`` (200) -- acknowledged.
  * ``{"ok":false,"error":"no crash record to acknowledge"}`` (409) -- no
    crash is on record at all.
  * ``{"ok":false,"error":"failed to persist acknowledgement -- record
    still unacknowledged, try again"}`` (500) -- a record exists, but the
    NVS write failed; the record is still unacknowledged.
Both non-2xx cases raise CrashReportAckHttpError (via urllib's own
HTTPError-on-non-2xx behaviour) with `.status`/`.detail` carrying the
board's own JSON body, exactly like safety_log_level_http_client's
SendFailedTest/HttpErrorTest split -- a caller that wants to distinguish
"nothing to acknowledge" from "board failed to persist it" can inspect
`.status` (409 vs 500) without this client collapsing the two.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request

from . import http_auth
from typing import Optional

CRASH_REPORT_ACK_HTTP_TIMEOUT_S = 8.0

_API_PATH = "/api/crash_report/ack"


class CrashReportAckHttpError(Exception):
    """Any transport or protocol failure talking to
    POST /api/crash_report/ack -- unreachable host, non-2xx (including the
    handler's own 409/500 "nothing to ack"/"persist failed" bodies), or a
    response shape this client does not understand."""

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


def post_crash_report_ack(host: str, timeout: float = CRASH_REPORT_ACK_HTTP_TIMEOUT_S) -> dict:
    """POST /api/crash_report/ack. No request body.

    Returns the decoded JSON body for a 200 response (always ``{"ok":true}``
    per the handler). Raises CrashReportAckHttpError for every non-2xx
    answer -- including the handler's own 409 "no crash record to
    acknowledge" and 500 "failed to persist acknowledgement" bodies, which
    urllib raises as HTTPError before this function ever sees a parsed dict
    -- so a caller must inspect the raised error's `.status`/`.detail`
    rather than a returned ``ok`` field to tell those two apart. Goes
    through :mod:`http_auth` (KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD),
    same as every other write tool that talks to an ADMIN-tier route."""
    req = urllib.request.Request(_url(host), data=b"", method="POST")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise CrashReportAckHttpError(f"POST {_API_PATH} refused: HTTP {status}: {detail}",
                                       status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise CrashReportAckHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise CrashReportAckHttpError(
            f"POST {_API_PATH} returned 200 but the body was not JSON: {text!r}") from exc
