#!/usr/bin/env python3
"""kiln_configs_apply_http_client.py -- pure HTTP client for
POST /api/kiln_configs/apply and GET /api/kiln_configs/apply_status
(firmware/KilnFW/App/drivers/http/kiln_cfg_http.c's apply_post_handler()/
apply_status_get_handler(), ROUTE_TIER_ADMIN per route_tier_table.h).

Before this module, no PcTools client sent the
``X-Kiln-Ack-Hardware-Differs`` header apply_post_handler() checks (kiln_cfg_
http.c around line 361, gated by ``kiln_cfg_store_slot_hardware_differs()``
in kiln_cfg_store.c around line 1466/1480): a stored kiln config whose
hardware shape differs from the board is refused with 428 "Precondition
Required" and the board's own explanation as the body, and the ONLY way past
that refusal is resending the exact same request with that header set to
"1" -- exactly what the kiln_configs page's own "hardware differs" checkbox
does. web_commission_row.py's ``_apply_kiln_config`` drives that same route
through a raw web session for its own W42/commissioning purposes and is not
this module -- this is the general-purpose ADMIN-tier client, following
kiln_configs_quarantine_http_client.py's shape.

Note that a 428 from this route is NOT unambiguously the hardware-differs
refusal. apply_post_handler() runs ota_http_check_interlocks() FIRST, and
ota_http_send_interlock_refusal() (ota_http.c) also answers 428 for
OTA_INTERLOCK_REFUSED_NEEDS_ACK (no safety processor) -- a refusal the
hardware-differs header cannot answer at all; it wants
``X-Ota-Ack-No-Safety``, which this client deliberately never sends. The
only reliable discriminator is the body: the hardware-differs message
apply_hardware_differs() builds (kiln_cfg_store.c) always names the header
it wants, so ``ACK_HARDWARE_DIFFERS_HEADER in body`` is the test --
``is_hardware_differs_body()`` below.

The apply itself is asynchronous: a successful POST returns 202 with the
swap "running", not "done" -- kiln_cfg_swap_apply() is a 60+ round-trip UART
transaction plus a flash write, deliberately dispatched to a worker rather
than run on the httpd stack (see apply_post_handler()'s own comment). The
real outcome is only available from GET /api/kiln_configs/apply_status
afterward (``get_apply_status()`` below); ``poll_apply_status()`` polls it to
a terminal state (``done_ok``/``done_failed``) the way ota_http_client's Pico
status poll does.

Same "stdlib urllib.request, no framework" convention as the other bench
HTTP clients in this package, going through :mod:`http_auth` for the ADMIN
session seam every other admin-tier write tool here uses.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request

from . import http_auth
from typing import Optional

KILN_CONFIGS_APPLY_HTTP_TIMEOUT_S = 8.0

_APPLY_PATH = "/api/kiln_configs/apply"
_APPLY_STATUS_PATH = "/api/kiln_configs/apply_status"

# kiln_cfg_http.c's KILN_CFG_ACK_HW_DIFFERS_HEADER -- deliberately not
# covered by the request HMAC, same reasoning as ota_http.c's
# X-Ota-Ack-No-Safety: this only relaxes a LOCAL policy check, never a
# safety-link/authentication decision.
ACK_HARDWARE_DIFFERS_HEADER = "X-Kiln-Ack-Hardware-Differs"


def is_hardware_differs_body(body: str) -> bool:
    """True when a 428 body is the hardware-shape refusal rather than the
    interlock's own 428 (no safety processor). apply_hardware_differs() in
    kiln_cfg_store.c ends its message with "Resend with
    X-Kiln-Ack-Hardware-Differs: 1."; no interlock reason string names that
    header. Naming the wrong one sends the operator to re-send with a flag
    that cannot possibly help, forever."""
    return ACK_HARDWARE_DIFFERS_HEADER.lower() in (body or "").lower()


class KilnConfigsApplyHttpError(Exception):
    """Any transport or protocol failure talking to POST
    /api/kiln_configs/apply or GET /api/kiln_configs/apply_status --
    unreachable host, non-2xx the caller didn't already expect, or a
    response shape this client does not understand. `.status`/`.detail`
    carry the board's own reported status code and body, same shape as
    KilnConfigsQuarantineHttpError."""

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


def post_apply(
    host: str,
    config_id: int,
    ack_hardware_differs: bool = False,
    timeout: float = KILN_CONFIGS_APPLY_HTTP_TIMEOUT_S,
) -> "tuple[int, str]":
    """POST /api/kiln_configs/apply. Sends the ack header ONLY when
    ``ack_hardware_differs`` is True -- an omitted/false header is what lets
    a genuine hardware-shape mismatch surface as 428 rather than being
    silently masked. Returns ``(status, body_text)`` for EVERY response,
    2xx or not, so a 428 (or a 404/409) reaches the caller with the board's
    own explanation intact rather than being folded into a generic
    exception; only a transport-level failure (unreachable host) raises."""
    body = urllib.parse.urlencode({"id": str(config_id)}).encode("ascii")
    req = urllib.request.Request(_url(host, _APPLY_PATH), data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    if ack_hardware_differs:
        req.add_header(ACK_HARDWARE_DIFFERS_HEADER, "1")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        return status, detail
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise KilnConfigsApplyHttpError(f"POST {_APPLY_PATH} unreachable: {detail}") from exc
    except (http_auth.HttpAuthError, OSError) as exc:
        # http_auth.urlopen() can also raise HttpAuthError (login refused, or
        # no credential in the environment) or a bare OSError/socket.timeout
        # that never got wrapped in URLError -- neither is an HTTPError or a
        # URLError, so without this they escaped past every except here and
        # past the tool's own `except ac.KilnConfigsApplyHttpError` above it.
        raise KilnConfigsApplyHttpError(f"POST {_APPLY_PATH} failed: {exc}") from exc


def get_apply_status(host: str, timeout: float = KILN_CONFIGS_APPLY_HTTP_TIMEOUT_S) -> dict:
    """GET /api/kiln_configs/apply_status -- ``{"state", "id", "diverged",
    "reason"}``, ``state`` one of idle/running/done_ok/done_failed."""
    req = urllib.request.Request(_url(host, _APPLY_STATUS_PATH), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise KilnConfigsApplyHttpError(
            f"GET {_APPLY_STATUS_PATH} refused: HTTP {status}: {detail}", status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise KilnConfigsApplyHttpError(f"GET {_APPLY_STATUS_PATH} unreachable: {detail}") from exc
    except (http_auth.HttpAuthError, OSError) as exc:
        # Same gap as post_apply(): http_auth.urlopen() can raise HttpAuthError
        # or a bare OSError/socket.timeout never wrapped in URLError.
        raise KilnConfigsApplyHttpError(f"GET {_APPLY_STATUS_PATH} failed: {exc}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise KilnConfigsApplyHttpError(
            f"GET {_APPLY_STATUS_PATH} returned 200 but the body was not JSON: {text!r}") from exc


def poll_apply_status(
    host: str,
    deadline_s: float = 60.0,
    now=None,
    sleep=None,
    timeout: float = KILN_CONFIGS_APPLY_HTTP_TIMEOUT_S,
) -> dict:
    """Polls GET /api/kiln_configs/apply_status to a terminal state
    (done_ok/done_failed), same shape as cases_ota.py's `_poll_pico_phase`.
    `now`/`sleep` are injectable for tests. Returns the last status dict
    read, even if the deadline was hit before a terminal state (state will
    then read "running" or "idle").

    A poll that raises :class:`KilnConfigsApplyHttpError` is tolerated and
    retried -- a transient 5xx or dropped connection mid-poll should not
    abort a swap that may still finish. But if EVERY poll in the deadline
    fails this way, the real outcome was never actually read even once, so
    this re-raises the last error rather than returning an empty/default
    dict that would read identically to "polled fine, board just never
    reached a terminal state" to the caller."""
    import time as _time
    now = now or _time.monotonic
    sleep = sleep or _time.sleep

    status: dict = {}
    any_success = False
    last_error: Optional[KilnConfigsApplyHttpError] = None
    deadline = now() + deadline_s
    while now() < deadline:
        try:
            status = get_apply_status(host, timeout=timeout)
        except KilnConfigsApplyHttpError as exc:
            last_error = exc
            status = {}
        else:
            any_success = True
            if status.get("state") in ("done_ok", "done_failed"):
                break
        sleep(2.0)
    if not any_success and last_error is not None:
        raise KilnConfigsApplyHttpError(
            f"{_APPLY_STATUS_PATH}: status unreadable, every poll failed -- last error: {last_error}",
            last_error.status, last_error.detail) from last_error
    return status
