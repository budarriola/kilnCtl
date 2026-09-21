#!/usr/bin/env python3
"""kiln_configs_quarantine_http_client.py -- pure HTTP client for
POST /api/kiln_configs/quarantine_clear
(firmware/KilnFW/App/drivers/http/kiln_cfg_http.c's
quarantine_clear_post_handler(), ROUTE_TIER_ADMIN per route_tier_table.h),
which is the one way out of a quarantined kiln_configs store
(kiln_cfg_store.c's set_quarantine()/kiln_cfg_store_is_quarantined()) short
of a full ``factory_reset(scope=KILN)`` that erases the whole kiln_nvs
partition. The store quarantines itself on boot if the persisted blob is
the wrong size for any known schema version (kiln_cfg_store.c around
line 489); every save/clone/rename/delete is refused while quarantined, but
the live zones config on the board is unaffected.

Same "stdlib urllib.request, no framework" convention as
crash_report_ack_http_client.py/safety_log_level_http_client.py, in its own
module for the same reason.

The route's own ordering (kiln_cfg_http.c's quarantine_clear_post_handler())
checks the store's quarantine state BEFORE the confirm gate, which is what
lets this module offer a non-mutating status probe: a POST with a non-empty
but deliberately false ``confirm=0`` field (the body must be non-empty or
the handler's read_small_body() rejects it with a 400 before the quarantine
check ever runs) still answers 409 "not quarantined" if the store is
healthy, or 400 "confirm=1 required" if it actually is quarantined --
either way, no state changes. ``get_quarantine_status()`` below is exactly
that probe. The mutating call, ``post_quarantine_clear()``, sends
``confirm=1`` and actually discards the quarantined bytes.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request

from . import http_auth
from typing import Optional

KILN_CONFIGS_QUARANTINE_HTTP_TIMEOUT_S = 8.0

_API_PATH = "/api/kiln_configs/quarantine_clear"
_LIST_PATH = "/api/kiln_configs"


class KilnConfigsQuarantineHttpError(Exception):
    """Any transport or protocol failure talking to
    POST /api/kiln_configs/quarantine_clear or GET /api/kiln_configs --
    unreachable host, non-2xx, or a response shape this client does not
    understand. `.status`/`.detail` carry the board's own reported status
    code and body, same shape as CrashReportAckHttpError."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


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


def get_kiln_configs_list(host: str, timeout: float = KILN_CONFIGS_QUARANTINE_HTTP_TIMEOUT_S) -> dict:
    """GET /api/kiln_configs -- the ordinary list/active-id read (list_get_
    handler() in kiln_cfg_http.c). Used here only as a pre-flight sanity
    read (does the board answer at all, and what does it currently see) --
    this route does NOT report quarantine state itself; see
    get_quarantine_status() for that."""
    req = urllib.request.Request(_url(host, _LIST_PATH), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise KilnConfigsQuarantineHttpError(
            f"GET {_LIST_PATH} refused: HTTP {status}: {detail}", status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise KilnConfigsQuarantineHttpError(f"GET {_LIST_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise KilnConfigsQuarantineHttpError(
            f"GET {_LIST_PATH} returned 200 but the body was not JSON: {text!r}") from exc


def _post_quarantine_clear_raw(host: str, confirm: bool, timeout: float) -> "tuple[Optional[int], str]":
    # The handler's first statement (read_small_body(), kiln_cfg_http.c:645)
    # rejects an empty body with a 400 BEFORE the quarantine check ever runs
    # (kiln_cfg_http.c:73) -- an empty-body probe therefore always looks
    # quarantined, on a healthy board too. Send a non-empty, deliberately
    # false confirm field ("confirm=0") so the body is non-empty and the
    # handler actually reaches the quarantine check; confirm=0 is still not
    # "1" so the confirm gate below it never fires either.
    body = urllib.parse.urlencode({"confirm": "1" if confirm else "0"}).encode("ascii")
    req = urllib.request.Request(_url(host, _API_PATH), data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        return status, detail
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise KilnConfigsQuarantineHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc


def get_quarantine_status(host: str, timeout: float = KILN_CONFIGS_QUARANTINE_HTTP_TIMEOUT_S) -> "tuple[bool, str]":
    """Non-mutating status probe: POSTs with no ``confirm`` field, which the
    route answers with (409, "not quarantined") or (400, "confirm=1
    required..."), never changing state either way -- see this module's
    docstring for why the route's ordering makes that safe. Returns
    ``(is_quarantined, detail_text)``. Any OTHER status is an error this
    function does not understand and raises for."""
    status, detail = _post_quarantine_clear_raw(host, confirm=False, timeout=timeout)
    if status == 409:
        return False, detail
    if status == 400:
        return True, detail
    raise KilnConfigsQuarantineHttpError(
        f"POST {_API_PATH} (status probe) returned unexpected HTTP {status}: {detail}", status, detail)


def post_quarantine_clear(host: str, timeout: float = KILN_CONFIGS_QUARANTINE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/kiln_configs/quarantine_clear with confirm=1 -- actually
    discards the quarantined store and starts a fresh, empty one. Raises
    KilnConfigsQuarantineHttpError for any non-2xx (including a 409 racing
    against something else that cleared it first). Goes through
    :mod:`http_auth` (KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD), same as
    every other ADMIN-tier write tool in this package."""
    body = urllib.parse.urlencode({"confirm": "1"}).encode("ascii")
    req = urllib.request.Request(_url(host, _API_PATH), data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise KilnConfigsQuarantineHttpError(
            f"POST {_API_PATH} refused: HTTP {status}: {detail}", status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise KilnConfigsQuarantineHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise KilnConfigsQuarantineHttpError(
            f"POST {_API_PATH} returned 200 but the body was not JSON: {text!r}") from exc
