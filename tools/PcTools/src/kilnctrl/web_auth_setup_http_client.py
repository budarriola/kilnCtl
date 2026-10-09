#!/usr/bin/env python3
"""web_auth_setup_http_client.py -- pure HTTP client for the three routes
``web_auth_setup()`` (mcp_server_web_auth.py) drives to bootstrap web
authentication on a fresh board, matched exactly against the firmware
handlers rather than reverse-engineered from behaviour:

  * GET  /api/auth/config             -- security_http.c's
    security_config_get_handler(), ROUTE_TIER_ADMIN (route_tier_table.h).
    Read-only status: web_enabled/lcd_enabled, timeouts, and whether an
    admin/user password or LCD PIN is configured.
  * POST /api/auth/bootstrap_password -- security_backend_web_auth.c's
    auth_bootstrap_password_post_handler(), ROUTE_TIER_ADMIN_BOOTSTRAP. Only
    reachable while ``web_auth_admin_bootstrap_needed()`` is true on the
    board (web auth ON, no admin record yet) -- otherwise 409. Takes
    ``username``/``password`` form fields, no session required or possible
    (see http_auth_enforce.c's ROUTE_TIER_ADMIN_BOOTSTRAP branch).
  * POST /api/auth/security           -- security_http.c's
    security_post_handler(), ROUTE_TIER_ADMIN. One action per call via a
    ``cmd`` field (``set_web_password``, ``set_policy``, ...). Per
    http_auth_check() (http_auth_enforce.c), a tier of ROUTE_TIER_ADMIN is
    allowed with NO session at all whenever ``web_enabled`` is currently
    false on the board -- "auth off collapses every tier to full access" is
    checked first and unconditionally -- so setting the very first admin
    password through this route while auth is still off needs no login.

All three go through :mod:`http_auth` (KILNCTL_WEB_USERNAME/
KILNCTL_WEB_PASSWORD from the environment, one login-and-retry on a 401),
same as every other ADMIN-tier write client in this package. A password
value is never included in any exception message, log line, or returned
string this module produces -- only field names and status codes are.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth

WEB_AUTH_SETUP_HTTP_TIMEOUT_S = 10.0

_CONFIG_PATH = "/api/auth/config"
_BOOTSTRAP_PATH = "/api/auth/bootstrap_password"
_SECURITY_PATH = "/api/auth/security"
_LOGIN_PATH = http_auth.LOGIN_PATH


class WebAuthSetupHttpError(Exception):
    """Any transport or protocol failure talking to one of the three
    ``/api/auth/*`` routes this module wraps. ``.status`` carries the HTTP
    status when known (409/500/etc from the board's own handlers), never a
    credential value."""

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


def _decode_json(path: str, text: str) -> dict:
    try:
        return json.loads(text)
    except Exception as exc:
        raise WebAuthSetupHttpError(
            f"{path} returned 200 but the body was not JSON") from exc


def get_auth_config(host: str, timeout: float = WEB_AUTH_SETUP_HTTP_TIMEOUT_S) -> dict:
    """GET /api/auth/config. Raises WebAuthSetupHttpError on any non-2xx or
    unreachable board. Uses :func:`http_auth.urlopen` so this succeeds with
    no credential at all while web auth is off, and logs in once (env
    credentials) if the board is already gating this route."""
    req = urllib.request.Request(_url(host, _CONFIG_PATH), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"GET {_CONFIG_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"GET {_CONFIG_PATH} unreachable: {detail}") from exc
    except http_auth.HttpAuthError as exc:
        raise WebAuthSetupHttpError(f"GET {_CONFIG_PATH}: {exc}") from exc
    return _decode_json(_CONFIG_PATH, text)


def post_bootstrap_password(host: str, username: str, password: str,
                             timeout: float = WEB_AUTH_SETUP_HTTP_TIMEOUT_S) -> dict:
    """POST /api/auth/bootstrap_password. No session is possible for this
    route (ROUTE_TIER_ADMIN_BOOTSTRAP) -- a plain, unauthenticated request,
    never routed through :mod:`http_auth`. Raises WebAuthSetupHttpError,
    with ``.status`` set to 409 if the board reports an admin record
    already exists (bootstrap not needed), or 400 if the password was
    rejected as weak."""
    body = urllib.parse.urlencode({"username": username, "password": password}).encode("ascii")
    req = urllib.request.Request(
        _url(host, _BOOTSTRAP_PATH), data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"POST {_BOOTSTRAP_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"POST {_BOOTSTRAP_PATH} unreachable: {detail}") from exc
    return _decode_json(_BOOTSTRAP_PATH, text)


def post_security(host: str, fields: "dict[str, str]",
                   timeout: float = WEB_AUTH_SETUP_HTTP_TIMEOUT_S) -> dict:
    """POST /api/auth/security with ``fields`` (must include ``cmd``),
    form-urlencoded. Routed through :mod:`http_auth.urlopen` -- with web
    auth currently off this needs no session (ROUTE_TIER_ADMIN collapses to
    ALLOW, http_auth_enforce.c); with it already on, ``http_auth`` logs in
    once from the environment on the first 401.

    The security_http.c handler always answers 200 with a JSON body of
    either ``{"ok":true}`` or ``{"ok":false,"error":"..."}`` -- a non-2xx
    transport status here means something below the handler refused the
    request outright (e.g. no session while auth is on), not a rejected
    command; a decoded ``{"ok":false,...}`` body is returned as-is and it is
    the caller's job to check ``ok``."""
    body = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        _url(host, _SECURITY_PATH), data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"POST {_SECURITY_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"POST {_SECURITY_PATH} unreachable: {detail}") from exc
    except http_auth.HttpAuthError as exc:
        raise WebAuthSetupHttpError(f"POST {_SECURITY_PATH}: {exc}") from exc
    return _decode_json(_SECURITY_PATH, text)


def try_login(host: str, username: str, password: str,
              timeout: float = WEB_AUTH_SETUP_HTTP_TIMEOUT_S) -> bool:
    """POST /api/auth/login (web_auth_login_http.c, ROUTE_TIER_OPEN) once,
    with the given credentials, form-urlencoded, and an explicit
    ``Accept-Encoding: identity`` header -- this route answers JSON, not a
    gzip-only embedded page, but identity is requested explicitly rather
    than relying on urllib's header-omission default, since this codebase
    has one prior class of bug (2026-09-21, web_ui_client.py/
    bench_test/cases_web.py) where an ambiguous encoding negotiation against
    this firmware's HTTP server produced a wrong status.

    Returns True on a successful login (200 with a session cookie). Returns
    False on a 401 (bad credentials). Raises WebAuthSetupHttpError for
    anything else (unreachable board, unexpected status, malformed
    response) -- this function deliberately never retries either outcome;
    a caller that gets False must not loop.
    """
    body = urllib.parse.urlencode({"username": username, "password": password}).encode("ascii")
    req = urllib.request.Request(
        _url(host, _LOGIN_PATH), data=body, method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept-Encoding": "identity",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            resp.read()
            return True
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            return False
        status, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"POST {_LOGIN_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise WebAuthSetupHttpError(f"POST {_LOGIN_PATH} unreachable: {detail}") from exc
