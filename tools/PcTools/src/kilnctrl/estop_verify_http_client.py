#!/usr/bin/env python3
"""estop_verify_http_client.py -- pure HTTP client for
POST /api/estop/verify
(firmware/KilnFW/App/drivers/http/diagnostics_http.c's
estop_verify_post_handler(), ROUTE_TIER_ADMIN per route_tier_table.h),
which records that a HUMAN has physically verified the E-stop interlock
(estop_verification_confirm(), see estop_verification.h) -- per
firmware/SaftyFW/README.md's bench verification procedure, "I have verified
the E-stop interlock" (both poles; pole 1, the external line contactor's
coil circuit, is wiring firmware cannot see, which is the whole reason this
flag is never inferred from a GPIO read or a config value).

Same "stdlib urllib.request, no framework" convention as
crash_report_ack_http_client.py, in its own module for the same reason:
this is a write of a human attestation, not a state read, and it is kept
apart from readiness_http_client.get_readiness() (the read side) on
purpose, so the read path keeps its "no function here can change board
state" guarantee.

POST /api/estop/verify takes no request body and no form fields --
estop_verify_post_handler() reads nothing off the request. It answers:
  * ``{"ok":true}`` (200) -- verification recorded.
  * ``{"ok":false,"error":"..."}`` (500) -- estop_verification_confirm()
    failed (e.g. an NVS write failure); not recorded.
Both non-2xx cases raise EstopVerifyHttpError (via urllib's own
HTTPError-on-non-2xx behaviour) with `.status`/`.detail` carrying the
board's own JSON body.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request

from . import http_auth
from typing import Optional

ESTOP_VERIFY_HTTP_TIMEOUT_S = 8.0

_API_PATH = "/api/estop/verify"


class EstopVerifyHttpError(Exception):
    """Any transport or protocol failure talking to POST /api/estop/verify
    -- unreachable host, non-2xx (including the handler's own 500 "failed"
    body), or a response shape this client does not understand."""

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


def post_estop_verify(host: str, timeout: float = ESTOP_VERIFY_HTTP_TIMEOUT_S) -> dict:
    """POST /api/estop/verify. No request body.

    Returns the decoded JSON body for a 200 response (always ``{"ok":true}``
    per the handler). Raises EstopVerifyHttpError for every non-2xx answer
    -- including the handler's own 500 "failed" body, which urllib raises
    as HTTPError before this function ever sees a parsed dict -- so a
    caller must inspect the raised error's `.status`/`.detail`. Goes
    through :mod:`http_auth` (KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD),
    same as every other write tool that talks to an ADMIN-tier route.
    Never logs or echoes a credential."""
    req = urllib.request.Request(_url(host), data=b"", method="POST")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise EstopVerifyHttpError(f"POST {_API_PATH} refused: HTTP {status}: {detail}",
                                    status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise EstopVerifyHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise EstopVerifyHttpError(
            f"POST {_API_PATH} returned 200 but the body was not JSON: {text!r}") from exc
