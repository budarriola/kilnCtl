#!/usr/bin/env python3
"""totp_http_client.py -- pure HTTP client for the TOTP password-reset
routes described in docs/TOTP_PASSWORD_RESET_PLAN.md section 4:
``POST /api/auth/forgot`` and ``POST /api/auth/reset``, plus a read of
``GET /api/auth/totp_status`` for enrollment status (section 2/7 -- WT-A,
not yet implemented in firmware). Modelled on
kiln_configs_apply_http_client.py's shape: stdlib ``urllib.request`` only,
no framework, ``(status, body)`` returned for every response the caller is
expected to branch on, and a dedicated exception only for transport-level
failure or a response shape this client cannot parse at all.

**Route tier note.** All three routes are ROUTE_TIER_OPEN (the whole point
of the forgot/reset flow is that it works without an existing session) --
unlike every ADMIN-tier client in this package, this module does NOT go
through :mod:`http_auth`'s login-on-401 seam for ``forgot``/``reset``
themselves. ``verify_new_password_login()`` below is the one exception: it
deliberately calls :func:`kilnctrl.http_auth.login` (the real login path)
to confirm the new password actually works end to end, which is the whole
point of that verification step.

**Contract decisions recorded here** (docs/TOTP_PASSWORD_RESET_PLAN.md left
these fields unspecified for WT-A to choose; WT-A does not exist yet, so
WT-C is choosing the contract below and recording it in the plan doc in the
same commit as this module -- see that doc's WT-A/WT-C section):

  * ``POST /api/auth/forgot`` request body: ``{"username": str, "code": str}``
    (form-urlencoded, matching ``login_post_handler``'s existing
    convention). Response is **always** HTTP 202 with JSON
    ``{"reset_token": "<opaque string>"}`` -- a token-shaped value is always
    present, whether or not ``username``/``code`` actually verified, so an
    unauthenticated caller cannot distinguish "enrolled and code matched"
    from "not enrolled" or "wrong code" by response shape alone (the plan's
    anti-oracle requirement). Passing that same opaque value to
    ``/api/auth/reset`` only succeeds when it was the REAL token.
  * ``POST /api/auth/reset`` request body:
    ``{"username": str, "reset_token": str, "new_password": str}``
    (form-urlencoded). Success: HTTP 200, JSON ``{"ok": true}``. Any
    failure (expired/wrong/reused token, password policy rejection): HTTP
    400, JSON ``{"ok": false}`` -- deliberately generic per the plan
    ("never distinguishes wrong token from bad password").
  * Both OPEN routes: a rate-limit refusal from the shared
    ``login_ip_scope.c`` ladder is HTTP 429 with that ladder's existing
    PLAIN-TEXT body (``web_auth_login_http.c``'s login 429 site), reused
    as-is, not re-mapped. A board whose clock is not SNTP-synced refuses
    with HTTP 503 (plan section 3's "board clock not synced yet" -- clock
    sync is board-wide, not per-user, so it is no enrollment oracle). A
    malformed body may get httpd's own plain-text 400. None of these
    non-2xx bodies is required to be JSON; this client returns ``{}`` for a
    non-JSON non-2xx body rather than raising, so a caller can still branch
    on the status code.
  * ``GET /api/auth/totp_status``: JSON ``{"enrolled": bool}`` only -- never
    a secret, never a seed, never a QR payload. ROUTE_TIER_ADMIN (unlike the
    two reset routes): whether this administrator has TOTP enrolled is not
    information to hand to an unauthenticated caller, whereas the reset
    routes' whole job is to work for one.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

from . import http_auth

TOTP_HTTP_TIMEOUT_S = 8.0

_FORGOT_PATH = "/api/auth/forgot"
_RESET_PATH = "/api/auth/reset"
_STATUS_PATH = "/api/auth/totp_status"


class TotpHttpError(Exception):
    """Any transport or protocol failure talking to the TOTP HTTP routes --
    unreachable host, or a response shape this client does not understand.
    A non-2xx the caller already expects to branch on (202/400/429) is
    NEVER raised here -- it is returned as ``(status, body)`` -- so this is
    reserved for the cases no caller can meaningfully continue past."""

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


def _post_open(host: str, path: str, fields: dict, timeout: float) -> "tuple[int, str]":
    """POSTs to an OPEN-tier route with no session/credential -- these
    routes are reachable by design without one. Returns ``(status, body)``
    for every response, 2xx or not; only an unreachable host raises."""
    body = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(_url(host, path), data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        return status, detail
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise TotpHttpError(f"POST {path} unreachable: {detail}") from exc
    except OSError as exc:
        # A read timeout surfaces as a bare TimeoutError/OSError, not a
        # URLError -- still a transport failure, never an uncaught raise.
        raise TotpHttpError(f"POST {path} failed: {type(exc).__name__}") from exc


def _parse_json_body(path: str, status: int, body: str) -> dict:
    try:
        parsed = json.loads(body)
    except Exception as exc:
        if not 200 <= status < 300:
            # Non-2xx bodies (the login ladder's 429, httpd's own 400,
            # the unsynced-clock 503) are plain text by design; the status
            # code alone is what the caller branches on.
            return {}
        raise TotpHttpError(
            f"POST {path} returned HTTP {status} but the body was not JSON "
            f"({len(body)} bytes, body not echoed)",
            status, "") from exc
    if not isinstance(parsed, dict):
        if not 200 <= status < 300:
            return {}
        raise TotpHttpError(
            f"{path} returned HTTP {status} with a JSON body that is not an object",
            status, "")
    return parsed


def post_forgot(host: str, username: str, code: str,
                 timeout: float = TOTP_HTTP_TIMEOUT_S) -> "tuple[int, dict]":
    """POST /api/auth/forgot -- ``{"username","code"}``. Per the contract
    recorded in this module's docstring, a well-formed request always comes
    back HTTP 202 with a ``{"reset_token": ...}`` body, whether or not the
    code actually matched; a malformed request, a rate-limit refusal, or an
    unsynced board clock answers differently (400/429/503) and those are
    returned as-is (body ``{}`` when not JSON), never raised. Never logs, prints, or echoes ``code``."""
    status, body = _post_open(host, _FORGOT_PATH, {"username": username, "code": code}, timeout)
    return status, _parse_json_body(_FORGOT_PATH, status, body)


def post_reset(host: str, username: str, reset_token: str, new_password: str,
                timeout: float = TOTP_HTTP_TIMEOUT_S) -> "tuple[int, dict]":
    """POST /api/auth/reset -- ``{"username","reset_token","new_password"}``.
    Returns ``(status, body_dict)`` for 200/400/429/503 alike -- the caller is
    expected to branch on ``status``, per the plan's deliberately generic
    failure shape. Never logs, prints, or echoes ``new_password`` or
    ``reset_token``."""
    status, body = _post_open(
        host, _RESET_PATH,
        {"username": username, "reset_token": reset_token, "new_password": new_password},
        timeout)
    return status, _parse_json_body(_RESET_PATH, status, body)


def get_totp_status(host: str, timeout: float = TOTP_HTTP_TIMEOUT_S) -> dict:
    """GET /api/auth/totp_status -- ``{"enrolled": bool}`` only, never a
    secret. ROUTE_TIER_ADMIN (see this module's docstring for why, unlike
    the two reset routes above) -- goes through :mod:`http_auth`'s
    login-on-401 seam like every other ADMIN-tier read in this package."""
    req = urllib.request.Request(_url(host, _STATUS_PATH), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise TotpHttpError(f"GET {_STATUS_PATH} refused: HTTP {status}: {detail}",
                             status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise TotpHttpError(f"GET {_STATUS_PATH} unreachable: {detail}") from exc
    except (http_auth.HttpAuthError, OSError) as exc:
        raise TotpHttpError(f"GET {_STATUS_PATH} failed: {exc}") from exc
    return _parse_json_body(_STATUS_PATH, 200, text)
