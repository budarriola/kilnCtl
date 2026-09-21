#!/usr/bin/env python3
"""readiness_http_client.py -- pure HTTP client for GET /api/readiness
(firmware/KilnFW/App/drivers/http/readiness_http.c's
api_readiness_get_handler()), the commissioning checklist the readiness
page renders (`firmware/KilnFW/App/drivers/http/readiness_page.html`).

Same "stdlib urllib.request, no framework" convention as
dashboard_http_client.py/safety_cfg_http_client.py, in its own module so
this READ-ONLY path stays unit-tested against mocked HTTP responses
(tools/PcTools/tests/test_readiness_http_client.py), no real socket and no
live board. This module never writes anything -- there is no POST
/api/readiness on the board side.

Response shape, per append_item() in readiness_http.c:
``{"items":[{"key","label","status","detail","fix_url"}, ...]}`` where
``status`` is one of readiness_status_t's four names: "ok", "not_done",
"cannot_yet", "deliberately_off" (and "unknown" for an unrecognised value,
per status_name()'s default branch -- kept here rather than raised, since a
forward-compatible client should still be able to render an item whose
status name this client predates)."""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Optional

from . import http_auth

READINESS_HTTP_TIMEOUT_S = 8.0

_API_PATH = "/api/readiness"


class ReadinessHttpError(Exception):
    """Any transport or protocol failure talking to GET /api/readiness --
    unreachable host, non-2xx, or a response shape this client does not
    understand."""

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


def get_readiness(host: str, timeout: float = READINESS_HTTP_TIMEOUT_S) -> dict:
    """GET /api/readiness. Returns the decoded JSON body:
    ``{"items":[{"key","label","status","detail","fix_url"}, ...]}``.

    Raises ReadinessHttpError for a transport failure, non-2xx response, a
    non-JSON body, or a body missing the top-level ``items`` list (readiness_
    http.c always emits one, even when the item loop dropped entries -- a
    missing list means this client is talking to something else)."""
    req = urllib.request.Request(_url(host), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise ReadinessHttpError(f"GET {_API_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise ReadinessHttpError(
            f"GET {_API_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or not isinstance(data.get("items"), list):
        raise ReadinessHttpError(f"GET {_API_PATH} response has no items list: {body_text!r}")
    return data
