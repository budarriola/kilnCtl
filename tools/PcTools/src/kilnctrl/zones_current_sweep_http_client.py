#!/usr/bin/env python3
"""zones_current_sweep_http_client.py -- pure HTTP client for the three
current-sweep endpoints in firmware/KilnFW/App/drivers/http/zones_http.c
(sweep_start_post_handler()/sweep_abort_post_handler()/
sweep_status_get_handler(), backed by zones_current_sweep_task.c/
zones_current_sweep_engine.c). Same "stdlib urllib.request, no framework"
convention as ramp_assist_http_client.py/adaptive_tune_http_client.py, unit-
tested against mocked HTTP only (test_zones_current_sweep_http_client.py) --
no real socket, no live board.

WHAT THIS ENDPOINT TRIGGERS: a hardware measurement pass that ENERGIZES
HEATER RELAYS, one zone at a time. POST .../start kicks off
zones_current_sweep_start(), a background task that, for each configured
zone in turn: forces every OTHER zone's relay(s) off, turns this zone's own
relay(s) on for ZONE_SWEEP_SETTLE_MS (1000 ms), then samples the shared CT
for ZONE_SWEEP_SAMPLE_MS (4000 ms, about 8 safety-link polls averaged) before
moving to the next zone -- roughly 5 s/zone, never two zones energized at
once. On success it derives a per-zone baseline current (subtracting a
freshly sampled idle baseline in summed-CT topology) and calls
zone_normals_set() to push i_normal_a[zone] to the safety processor, plus
derives k_ct_v_per_a. See mcp_server_zones_current_sweep.py's tool
docstrings, and this module's own function docstrings, for the operational
detail (especially the noise-floor behaviour) -- this module itself is only
the wire client, with no safety opinion of its own, same convention as
ramp_assist_http_client.py.

This module has NO confirm-gate and makes NO firing/trip/crash-report
precondition checks of its own -- see mcp_server_zones_current_sweep.py's
zone_current_sweep_start() tool for both. The firmware itself independently
refuses (zone_sweep_check_refusal(), zones_current_sweep_engine.c) if a
sweep is already running, hardware is unavailable, the zone config didn't
load cleanly, no zones are configured, a firing profile or autotune is
running, the safety link is down, a trip is latched, a relay is already on,
or CT topology is unknown -- reported back here as
``{"ok": false, "reason": "<one of those strings>"}`` with HTTP 409, not an
exception this client raises (a refusal is a normal, well-formed answer,
not a transport failure)."""
from __future__ import annotations

import json
import logging
import urllib.error
import urllib.request
from typing import Optional

_module_log = logging.getLogger(__name__)

#: Short GET/POST -- no large payload, same class of call as
#: ramp_assist_http_client.RAMP_ASSIST_HTTP_TIMEOUT_S. The start/abort calls
#: themselves return immediately (the sweep itself runs in a background
#: task); the timeout here is for the HTTP round trip, not the sweep.
ZONE_SWEEP_HTTP_TIMEOUT_S = 8.0

_SWEEP_START_PATH = "/api/zones/current_sweep/start"
_SWEEP_ABORT_PATH = "/api/zones/current_sweep/abort"
_SWEEP_STATUS_PATH = "/api/zones/current_sweep/status"

#: Same fallback-AP address every other HTTP client in this package uses.
ZONE_SWEEP_AP_DEFAULT_HOST = "192.168.4.1"

#: zones_current_sweep_engine.c's ZONE_SWEEP_NORMAL_NOISE_FLOOR_A -- a
#: measured per-zone current below this is refused (reported as
#: "unmeasured", never recorded), because it cannot be reliably
#: distinguished from CT/ADC noise. Mirrored here (a literal, not imported --
#: this is a Python client with no C toolchain dependency) purely so a
#: caller/tool docstring can state the number without re-deriving it; this
#: module performs no comparison against it itself, the firmware already
#: does that before ever reporting a value back over HTTP.
ZONE_SWEEP_NORMAL_NOISE_FLOOR_A = 0.045


class ZoneSweepHttpError(Exception):
    """Any transport or protocol failure talking to one of the three
    current-sweep endpoints -- unreachable host, non-2xx status THAT ISN'T
    the firmware's own well-formed 409 refusal body, or a response shape
    this client does not understand. A 409 refusal with a valid
    ``{"ok": false, "reason": "..."}`` body is NOT an error from this
    client's point of view -- see start()'s own docstring."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


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


def start(host: str, timeout: float = ZONE_SWEEP_HTTP_TIMEOUT_S) -> dict:
    """POST /api/zones/current_sweep/start.

    THIS CALL, IF ACCEPTED, ENERGIZES HEATER RELAYS ONE ZONE AT A TIME --
    see this module's own docstring for the sequence/timing. Returns
    immediately; the sweep itself runs to completion in a background task
    on the board (poll status() for progress).

    Returns the decoded JSON body, always: ``{"ok": true, "reason": "ok"}``
    on acceptance, or ``{"ok": false, "reason": "<refusal string>"}`` (HTTP
    409) if the firmware's own atomic gate refused -- e.g. "a firing profile
    is running or paused", "the safety link is down", "a safety trip is
    latched". A 409 refusal is deliberately NOT raised as
    :class:`ZoneSweepHttpError` -- it is the firmware answering the question
    correctly, not a transport failure. :class:`ZoneSweepHttpError` is
    raised only for an unreachable host, a timeout, or a response this
    client cannot parse as JSON at all (including a non-2xx status whose
    body isn't the expected ``{"ok": ...}`` shape).

    This function has no confirm-gate of its own and makes no preflight
    checks -- see mcp_server_zones_current_sweep.py's
    zone_current_sweep_start() tool for both."""
    req = urllib.request.Request(_url(host, _SWEEP_START_PATH), data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        try:
            data = json.loads(detail)
        except Exception:
            data = None
        if isinstance(data, dict) and "ok" in data:
            return data
        raise ZoneSweepHttpError(f"POST {_SWEEP_START_PATH} failed: HTTP {status}: {detail}",
                                  status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise ZoneSweepHttpError(f"POST {_SWEEP_START_PATH} unreachable: {detail}", None, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise ZoneSweepHttpError(
            f"POST {_SWEEP_START_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or "ok" not in data:
        raise ZoneSweepHttpError(f"POST {_SWEEP_START_PATH} response has no 'ok' field: {body_text!r}")
    return data


def abort(host: str, timeout: float = ZONE_SWEEP_HTTP_TIMEOUT_S) -> dict:
    """POST /api/zones/current_sweep/abort -- unconditionally de-energizes
    whatever zone the sweep currently has on and stops it
    (zones_current_sweep_abort()). Safe to call at any time, including when
    no sweep is running (the firmware's own abort handler has no
    "not running" error case -- it is always a clean, idempotent no-op or
    stop). Returns the decoded JSON body, always ``{"ok": true}`` on a
    normal response."""
    req = urllib.request.Request(_url(host, _SWEEP_ABORT_PATH), data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise ZoneSweepHttpError(f"POST {_SWEEP_ABORT_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise ZoneSweepHttpError(
            f"POST {_SWEEP_ABORT_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or "ok" not in data:
        raise ZoneSweepHttpError(f"POST {_SWEEP_ABORT_PATH} response has no 'ok' field: {body_text!r}")
    return data


def status(host: str, timeout: float = ZONE_SWEEP_HTTP_TIMEOUT_S) -> dict:
    """GET /api/zones/current_sweep/status -- read-only, safe to poll
    repeatedly while a sweep runs. Returns the decoded JSON body verbatim
    (sweep_status_get_handler()'s shape):

    ``state``: one of "idle"/"running"/"done"/"aborted"/"failed"
    (zone_sweep_state_str()).
    ``zone_index``/``zones_done``/``zones_total``: progress -- which zone is
    currently (or was last) energized, and how many of the total are done.
    ``reason``: human-readable state/refusal detail.
    ``i_normal_pushed_mask``: bitmask of zones whose measured baseline
    current was actually pushed to the safety processor
    (zone_normals_set()) this run.
    ``summed_unmeasured_mask``: bitmask of zones that, in summed-CT
    topology, could NOT be measured -- see this module's own docstring and
    ZONE_SWEEP_NORMAL_NOISE_FLOOR_A. A zone appearing in this mask on a
    from-scratch or low-current fixture is an EXPECTED result, not a
    failure to retry -- see the MCP tool's own docstring for the concrete
    bench numbers this project has actually measured.
    ``ct_map_derived_mask``/``ct_map_reason``, ``k_ct_derived_mask``/
    ``k_ct_reason``, ``nameplate_mismatch_mask``/``nameplate_reason``:
    independent derived-value/advisory fields, each with its own
    reason string for the same reason the firmware keeps them in separate
    fields (see zones_http.c's sweep_status_get_handler() comments) -- one
    shared string could only ever report one of them."""
    req = urllib.request.Request(_url(host, _SWEEP_STATUS_PATH), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status_code, detail = _http_error_detail(exc)
        raise ZoneSweepHttpError(f"GET {_SWEEP_STATUS_PATH} failed: {detail}", status_code, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise ZoneSweepHttpError(
            f"GET {_SWEEP_STATUS_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or "state" not in data:
        raise ZoneSweepHttpError(f"GET {_SWEEP_STATUS_PATH} response has no 'state' field: {body_text!r}")
    return data
