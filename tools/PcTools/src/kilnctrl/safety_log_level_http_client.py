#!/usr/bin/env python3
"""safety_log_level_http_client.py -- pure HTTP client for
POST /api/safety/log_level
(firmware/KilnFW/App/drivers/http/dashboard_settings_http.c's
safety_log_level_post_handler()), which forwards a runtime log-level request
to the safety processor over the isolated UART link
(safety_link_send_set_log_level()).

Same "stdlib urllib.request, no framework" convention as
safety_cfg_http_client.py/ota_http_client.py, in its own module for the same
reason: unit-tested against mocked HTTP responses, no real socket and no live
board.

This route was found fully built and registered (ROUTE_TIER_ADMIN) with no
client anywhere -- no HTML page, no JS, no PcTools/MCP tool -- during the
2026-09-20 "no client" audit. It is diagnostic-only: it changes what the
Pico's own log output includes, nothing safety-relevant on the Pico side
(guards, trips, relays are unaffected). A PcTools/MCP surface is the
convention this codebase already uses for direct-to-firmware diagnostic
knobs that are not part of the persisted commissioning record (see
safety_set_poll_period/safety_set_fault_out in mcp_server_safety.py, which go
over the link directly rather than HTTP) -- this one specifically must go
over HTTP since that is the only route the firmware actually registered for
it (POST /api/safety/log_level, not a UART command exposed to the ESP-Q
console).
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request

from . import http_auth
from typing import Optional

#: Matches UART_LOG_LEVEL_VERBOSE (4) in the shared link protocol header --
#: 0=ERROR .. 4=VERBOSE, exactly what dashboard_settings_http.c's handler
#: itself validates (`level < 0 || level > UART_LOG_LEVEL_VERBOSE`).
SAFETY_LOG_LEVEL_MAX = 4
SAFETY_LOG_LEVEL_NAMES = ("ERROR", "WARN", "INFO", "DEBUG", "VERBOSE")

SAFETY_LOG_LEVEL_HTTP_TIMEOUT_S = 8.0

#: The two peers the handler accepts for the optional ``peer`` form field
#: (added by e8a09c0f's per-peer log-level filter). "safety" forwards a
#: SET_LOG_LEVEL frame to the Pico over the isolated link; "relay" is a
#: local-only ESP-side filter on the relayed Pico log stream, no wire
#: traffic. Omitting the field entirely keeps the handler's historical
#: default, which is "safety" -- so ``peer=None`` below sends no field at
#: all rather than guessing on the client side.
SAFETY_LOG_LEVEL_PEERS = ("safety", "relay")

_API_PATH = "/api/safety/log_level"


class SafetyLogLevelHttpError(Exception):
    """Any transport or protocol failure talking to
    POST /api/safety/log_level -- unreachable host, non-2xx, or a response
    shape this client does not understand."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str) -> str:
    return f"http://{host}{_API_PATH}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace").strip()
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def set_safety_log_level(host: str, level: int, peer: Optional[str] = None,
                         timeout: float = SAFETY_LOG_LEVEL_HTTP_TIMEOUT_S) -> dict:
    """POST /api/safety/log_level with ``level`` (0=ERROR..4=VERBOSE).

    ``peer`` is the optional peer selector: "safety" (the Pico, over the
    isolated link) or "relay" (a local ESP-side filter on the relayed Pico
    log stream, no wire traffic). ``None`` sends no ``peer`` field at all,
    which the handler treats as its historical "safety" default -- the
    default is deliberately left to the firmware rather than guessed here.

    Returns the decoded JSON body. The handler answers ``{"ok":true}`` once
    the ESP has queued the SET_LOG_LEVEL frame for the safety link (or, for
    peer="relay", once the local filter is set) -- like every other
    safety-link command, this is fire-and-forget over the isolated UART and
    NOT confirmed on the Pico itself.

    Raises SafetyLogLevelHttpError for every non-2xx answer, including the
    handler's own ``{"ok":false,"error":"send failed"}`` link-down case --
    that body is sent with status 500, so urllib raises before this function
    ever sees it. The raised error carries `.status` and `.detail` (the
    response body), so the distinction is still available to a caller that
    wants it."""
    if not isinstance(level, int) or isinstance(level, bool) or level < 0 or level > SAFETY_LOG_LEVEL_MAX:
        raise SafetyLogLevelHttpError(
            f"level must be an integer 0-{SAFETY_LOG_LEVEL_MAX} (ERROR..VERBOSE), got {level!r}")

    if peer is not None and peer not in SAFETY_LOG_LEVEL_PEERS:
        raise SafetyLogLevelHttpError(
            f"peer must be one of {SAFETY_LOG_LEVEL_PEERS} or None (firmware default), got {peer!r}")

    fields = {"level": str(level)}
    if peer is not None:
        fields["peer"] = peer
    data = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        _url(host), data=data, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded",
                 "Content-Length": str(len(data))},
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise SafetyLogLevelHttpError(f"POST {_API_PATH} refused: HTTP {status}: {detail}",
                                       status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise SafetyLogLevelHttpError(f"POST {_API_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise SafetyLogLevelHttpError(
            f"POST {_API_PATH} returned 200 but the body was not JSON: {text!r}") from exc
