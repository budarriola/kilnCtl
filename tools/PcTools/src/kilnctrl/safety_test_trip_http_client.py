#!/usr/bin/env python3
"""safety_test_trip_http_client.py -- pure HTTP client for
POST /api/safety/test_trip (dashboard_exec_http.c's
safety_test_trip_post_handler(), ROUTE_TIER_ADMIN), docs/TEST_TRIP_PLAN.md.

The route asks the Pico to latch SAFETY_TRIP_TEST (reason 4). It can only
CAUSE a trip. The body must carry ``confirm=1``.

Unlike the other write clients, every answer that carries a JSON body is
returned as ``(status, dict)`` -- the 4xx/5xx bodies ARE the outcome
(``rate_limited``, ``already_tripped``, ``pico_protocol_too_old``,
``no_reply`` ...), so they are parsed, not raised. Only a transport failure
or a non-JSON body raises SafetyTestTripHttpError.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Optional

from . import http_auth

SAFETY_TEST_TRIP_HTTP_TIMEOUT_S = 12.0

_API_PATH = "/api/safety/test_trip"


class SafetyTestTripHttpError(Exception):
    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str) -> str:
    return f"http://{host}{_API_PATH}"


def _parse(text: str, status: int) -> "tuple[int, dict]":
    try:
        body = json.loads(text)
    except Exception as exc:
        raise SafetyTestTripHttpError(
            f"POST {_API_PATH} returned HTTP {status} with a non-JSON body: {text!r}", status, text) from exc
    if not isinstance(body, dict):
        raise SafetyTestTripHttpError(
            f"POST {_API_PATH} returned HTTP {status} with a non-object body: {text!r}", status, text)
    return status, body


def post_test_trip(host: str, timeout: float = SAFETY_TEST_TRIP_HTTP_TIMEOUT_S) -> "tuple[int, dict]":
    """POST confirm=1. Returns (http_status, json_body) for 2xx, 4xx and 5xx."""
    req = urllib.request.Request(
        _url(host), data=b"confirm=1", method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
            status = int(getattr(resp, "status", 200))
    except urllib.error.HTTPError as exc:
        try:
            text = exc.read().decode("utf-8", errors="replace")
        except Exception:
            text = ""
        return _parse(text, exc.code)
    except urllib.error.URLError as exc:
        raise SafetyTestTripHttpError(f"POST {_API_PATH} unreachable: {exc.reason}") from exc
    return _parse(text, status)
