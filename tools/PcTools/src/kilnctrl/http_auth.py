#!/usr/bin/env python3
"""http_auth.py -- the one seam every PcTools HTTP client issues its
requests through, so that web authentication can stay enabled on the board
without blinding the tooling.

WHY THIS EXISTS. KilnFW's route tier table
(firmware/KilnFW/App/drivers/http/route_tier_table.h) classifies a large
share of the routes this package depends on -- zones, setup progress, ramp
assist, watchdog config, sim, control, profiles, current sweep status, OTA
status -- as ROUTE_TIER_USER or ROUTE_TIER_ADMIN. Every client here used to
issue bare, unauthenticated urllib requests, so turning web authentication
on made ota_status() and its neighbours fail with "authentication
required", and the only way to keep the bench usable was to turn a real
security feature back off. That is this repository's documented "redaction
blinded the tooling" failure class (fixed once before, for a different
gate, in commit f0b73c30): a gate lands on the firmware side and the PC
side is never taught to pass it.

WHAT IT DOES. :func:`urlopen` is a drop-in replacement for
``urllib.request.urlopen`` as this package used it. It is deliberately
passive:

  - It makes the caller's request exactly as before. When authentication is
    disabled on the board -- and nothing has ever logged in this process --
    the bytes on the wire are identical to the previous behaviour: no extra
    header, no probe, no login request.
  - Only when the board answers 401 does it log in once via
    POST /api/auth/login (form-urlencoded ``username``/``password``, which
    is exactly what web_auth_login_http.c's login_post_handler parses),
    remember the session cookie for that host, and retry the original
    request exactly once. Any other failure is re-raised untouched, and a
    retry is never retried.
  - It does not weaken, bypass, or widen any route tier. It only presents
    the credential the board already asks for.

CREDENTIALS come from the environment -- ``KILNCTL_WEB_USERNAME`` and
``KILNCTL_WEB_PASSWORD`` -- and from nowhere else. The environment was
chosen over a local credentials file because there is then no path at all,
inside or outside the repository, that a password can be written to by
accident: this module never opens, creates, or writes a file, so no
credential of the owner's can end up in a commit, a test fixture, a log, or
a report. Its absence is reported as a clear, actionable error naming both
variables, never as a silent skip and never as a crash.
"""

from __future__ import annotations

import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional, Tuple

from . import host_resolve

#: Environment variables holding the board's web credential. Nothing else in
#: this package reads a password from anywhere, and nothing writes one.
USERNAME_ENV = "KILNCTL_WEB_USERNAME"
PASSWORD_ENV = "KILNCTL_WEB_PASSWORD"

#: POST target and cookie name, mirrored from
#: firmware/KilnFW/App/drivers/http/web_auth_login_http.c (route
#: "/api/auth/login", ROUTE_TIER_OPEN) and http_session_iface.h
#: (HTTP_SESSION_COOKIE_NAME). Read-only mirrors -- never edited to work
#: around a gate.
LOGIN_PATH = "/api/auth/login"
SESSION_COOKIE_NAME = "kiln_sid"

#: Mirrored from the same file's POST /api/auth/logout (ROUTE_TIER_USER --
#: any authenticated session, so this module only ever calls it after a
#: successful urlopen() login has actually happened).
LOGOUT_PATH = "/api/auth/logout"

#: Seconds allowed for the login round trip itself when the caller's own
#: request carried no timeout.
LOGIN_TIMEOUT_S = 10.0


class HttpAuthError(RuntimeError):
    """Raised when a 401 could not be answered: no credential in the
    environment, or a login that itself failed. Deliberately distinct from
    every client's own ``*HttpError`` so a caller can tell "the board
    refused me" from "the board is unreachable"."""


#: host (scheme://netloc) -> session cookie value. Process-lifetime only:
#: never persisted, never logged. Empty until a 401 is actually seen, which
#: is what keeps the auth-disabled path byte-for-byte unchanged.
_SESSIONS: "dict[str, str]" = {}


def clear_sessions() -> None:
    """Forget every remembered session. Used by tests, and available to any
    caller that knows the board rebooted (its session table did not
    survive)."""
    _SESSIONS.clear()


def credentials() -> Tuple[str, str]:
    """Return ``(username, password)`` from the environment.

    Raises :class:`HttpAuthError` naming both variables if either is missing
    or empty -- the actionable form, rather than sending a blank password
    and reporting the board's rejection instead of the real cause.
    """
    username = os.environ.get(USERNAME_ENV) or ""
    password = os.environ.get(PASSWORD_ENV) or ""
    if not username or not password:
        missing = [name for name, value in ((USERNAME_ENV, username), (PASSWORD_ENV, password))
                   if not value]
        raise HttpAuthError(
            "the board answered 401 (web authentication is enabled) but no credential is "
            f"available: {' and '.join(missing)} {'is' if len(missing) == 1 else 'are'} not set "
            f"in the environment. Set {USERNAME_ENV} and {PASSWORD_ENV} in the shell that "
            "launches the MCP server (or in your own environment when calling these clients "
            "directly). Credentials are never read from, or written to, any file in this "
            "repository.")
    return username, password


def _origin(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    return f"{parts.scheme}://{parts.netloc}"


def _as_request(req) -> urllib.request.Request:
    if isinstance(req, urllib.request.Request):
        return req
    return urllib.request.Request(req)


def _copy_request(req: urllib.request.Request) -> urllib.request.Request:
    """A fresh Request with the same url, body, method and headers.

    The caller's own Request object is never mutated by this module -- a
    cookie added in place would outlive this call and silently attach
    itself to whatever the caller did with that object next.
    """
    return urllib.request.Request(
        req.full_url, data=req.data, method=req.get_method(),
        headers=dict(req.header_items()),
    )


def _set_cookie_values(resp) -> "list[str]":
    headers = getattr(resp, "headers", None)
    if headers is None and hasattr(resp, "info"):
        headers = resp.info()
    if headers is None:
        return []
    get_all = getattr(headers, "get_all", None)
    if get_all is not None:
        return list(get_all("Set-Cookie") or [])
    value = headers.get("Set-Cookie")
    return [value] if value else []


def _login(origin: str, timeout: Optional[float]) -> str:
    """Log in at ``origin`` and return the session cookie value.

    Raises :class:`HttpAuthError` for a missing credential, a refused login,
    an unreachable board, or a 200 that carried no session cookie. Never
    retries: one attempt, then a clear error.
    """
    username, password = credentials()
    body = urllib.parse.urlencode({"username": username, "password": password}).encode("ascii")
    req = urllib.request.Request(
        origin + LOGIN_PATH, data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout or LOGIN_TIMEOUT_S) as resp:
            cookies = _set_cookie_values(resp)
            resp.read()
    except urllib.error.HTTPError as exc:
        # The password itself is never echoed into this message.
        raise HttpAuthError(
            f"POST {origin}{LOGIN_PATH} was refused: HTTP {exc.code}. Check the credential in "
            f"{USERNAME_ENV}/{PASSWORD_ENV} against the board's configured web login.") from exc
    except Exception as exc:  # noqa: BLE001
        raise HttpAuthError(f"POST {origin}{LOGIN_PATH} failed: {exc}") from exc

    for raw in cookies:
        name, _, rest = raw.partition("=")
        if name.strip() == SESSION_COOKIE_NAME:
            value = rest.split(";", 1)[0].strip()
            if value:
                _SESSIONS[origin] = value
                return value
    raise HttpAuthError(
        f"POST {origin}{LOGIN_PATH} succeeded but returned no {SESSION_COOKIE_NAME} cookie")


def logout(origin: str, timeout: Optional[float] = None) -> bool:
    """Ends this process's own remembered session at ``origin``, if any.

    Calls POST /api/auth/logout with whatever cookie :func:`urlopen` last
    remembered for ``origin`` (never a bare unauthenticated request -- the
    board's route is ROUTE_TIER_USER and would just answer 401), then
    forgets that session regardless of the board's response: this side of
    the seam is done with the credential either way, matching the route's
    own idempotent, best-effort logout stance (see http_session_iface.h's
    http_auth_session_logout() comment).

    Returns ``True`` if a remembered session existed and the POST was sent
    (regardless of the board's status code), ``False`` if this process had
    no remembered session for ``origin`` to begin with -- there is then
    nothing to revoke, and no request is made.
    """
    cookie = _SESSIONS.pop(origin, None)
    if not cookie:
        return False
    req = urllib.request.Request(
        origin + LOGOUT_PATH, data=b"", method="POST",
        headers={"Cookie": f"{SESSION_COOKIE_NAME}={cookie}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout or LOGIN_TIMEOUT_S) as resp:
            resp.read()
    except urllib.error.HTTPError:
        # The board refused or errored -- this process has already forgotten
        # the cookie above, so the next request through urlopen() will log in
        # fresh rather than keep presenting a credential we no longer trust.
        pass
    except Exception:  # noqa: BLE001
        pass
    return True


def urlopen(req, timeout=None):
    """Drop-in for ``urllib.request.urlopen`` that answers a 401 once.

    Behaviour when the board is not gating (today's state, and any route of
    ROUTE_TIER_OPEN): a single plain request, no Cookie header, no login --
    identical to calling ``urllib.request.urlopen`` directly.

    Behaviour on 401: log in once, attach the session cookie, and reissue
    the same request exactly once. The second response is returned, or its
    error raised, as-is -- there is no second retry and no loop.
    """
    original = _as_request(req)
    origin = _origin(original.full_url)

    cookie = _SESSIONS.get(origin)
    if cookie and not original.has_header("Cookie"):
        request = _copy_request(original)
        request.add_unredirected_header("Cookie", f"{SESSION_COOKIE_NAME}={cookie}")
    else:
        # Nothing to attach: hand urllib the caller's OWN argument,
        # untouched -- the same object, a bare string still a bare string.
        # With authentication disabled (and before any 401 in this process)
        # this call is indistinguishable from the urllib.request.urlopen
        # call it replaced.
        request = req

    try:
        resp = urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as exc:
        if exc.code != 401:
            raise
    else:
        # No exception means urllib got a genuine 2xx (or a redirect it
        # already followed) -- a non-2xx, non-401 status raises HTTPError
        # above and is re-raised, never reaching here. A confirmed 2xx
        # response is worth remembering as the default for the next call
        # that doesn't name a host.
        host_resolve.record_host_seen(origin)
        return resp
    # Exactly one login, exactly one retry. A 401 on the retry propagates to
    # the caller unchanged rather than starting another round.
    cookie = _login(origin, timeout)
    retry = _copy_request(original)
    retry.add_unredirected_header("Cookie", f"{SESSION_COOKIE_NAME}={cookie}")
    resp = urllib.request.urlopen(retry, timeout=timeout)
    host_resolve.record_host_seen(origin)
    return resp
