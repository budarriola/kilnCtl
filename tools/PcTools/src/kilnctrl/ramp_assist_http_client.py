#!/usr/bin/env python3
"""ramp_assist_http_client.py -- pure HTTP client for GET/POST /api/ramp_assist
(firmware/KilnFW/App/drivers/diagnostics_http.c's ramp_assist_get_handler()/
ramp_assist_post_handler(), backed by ramp_assist_cfg.h/.c). Same "stdlib
urllib.request, no framework" convention as adaptive_tune_http_client.py/
ota_http_client.py/safety_cfg_http_client.py, unit-tested against mocked HTTP
only (tools/PcTools/tests/test_ramp_assist_http_client.py) -- no real socket,
no live board.

WHAT THIS FLAG IS. ramp_assist_cfg.h is the kiln-wide (not per-zone) on/off
switch for a forthcoming feature: during a real firing, if the kiln cannot
keep up with a commanded ramp rate, the executor will warn, automatically
stretch the schedule so every target temperature is still reached, and reduce
dwell time by a heat-work-weighted credit. That behaviour is NOT implemented
yet -- this client (and the diagnostics-page control, and this flag) is the
on/off surface only.

WHY THIS MATTERS FOR TOOLING (the reason this module and its MCP tools exist
at all, not just a raw curl an operator could run by hand): PID tuning runs
and A/B controller comparisons in this project measure tracking error against
a KNOWN, COMMANDED ramp/dwell shape. If ramp assist is enabled and silently
stretches a ramp or shortens a dwell mid-run, the schedule the run THINKS it
executed no longer matches what actually happened, and every number the run
produces is invalid. An experiment that does not explicitly PIN this flag
(rather than inherit whatever the board happens to have from a previous
session, another operator, or a firmware default change) is exactly that
silent-invalidation hazard -- see run_queue.py's preset-apply code and
config_presets/*.json for where every existing preset now pins this OFF."""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Optional

#: Plain GET/POST against the board's own HTTP server, no long-running
#: operation on the far side -- same default as adaptive_tune_http_client.py's
#: simplest reads.
RAMP_ASSIST_HTTP_TIMEOUT_S = 8.0

_RAMP_ASSIST_PATH = "/api/ramp_assist"

#: Same fallback-AP address ota_http_client.py's OTA_AP_DEFAULT_HOST /
#: adaptive_tune_http_client.py's ADAPTIVE_TUNE_AP_DEFAULT_HOST name -- the
#: board's own AP-mode IP when no station connection is up.
RAMP_ASSIST_AP_DEFAULT_HOST = "192.168.4.1"


class RampAssistHttpError(Exception):
    """Any transport or protocol failure talking to GET/POST
    /api/ramp_assist -- unreachable host, non-2xx, or a response shape this
    client does not understand."""

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


def get_enabled(host: str, timeout: float = RAMP_ASSIST_HTTP_TIMEOUT_S) -> bool:
    """GET /api/ramp_assist -- ``{"enabled":true|false}``, the board's
    ACTUAL current value (never an assumed default -- a fresh board that has
    never heard of this key reads back false, ramp_assist_cfg.h's persisted
    safe default)."""
    req = urllib.request.Request(_url(host, _RAMP_ASSIST_PATH), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise RampAssistHttpError(f"GET {_RAMP_ASSIST_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise RampAssistHttpError(
            f"GET {_RAMP_ASSIST_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict) or "enabled" not in data:
        raise RampAssistHttpError(f"GET {_RAMP_ASSIST_PATH} response has no 'enabled' field: {body_text!r}")
    return bool(data["enabled"])


def set_enabled(host: str, enabled: bool, timeout: float = RAMP_ASSIST_HTTP_TIMEOUT_S) -> dict:
    """POST /api/ramp_assist -- form body ``enabled=<0|1>``, exactly what
    ``ramp_assist_post_handler()`` parses (http_form_find_field(), not
    JSON). Returns the decoded JSON body: ``{"ok":true,"enabled":<bool>}`` on
    a clean write, or ``{"ok":false,"error":"..."}`` if the NVS persist
    itself failed (the live in-RAM value still took effect either way --
    ramp_assist_cfg_set_enabled()'s own "in-RAM truth first" contract -- so a
    caller that only cares about the live effect for the rest of this boot
    can treat that case as informational, but should not assume the value
    will survive a reboot).

    This function has NO safety opinion of its own -- see
    mcp_server_ramp_assist.py's ``ramp_assist_set_enabled()`` tool for the
    confirm-gate applied above this thin wire client."""
    body = urllib.parse.urlencode({"enabled": "1" if enabled else "0"}).encode("ascii")
    req = urllib.request.Request(
        _url(host, _RAMP_ASSIST_PATH), data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise RampAssistHttpError(f"POST {_RAMP_ASSIST_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise RampAssistHttpError(
            f"POST {_RAMP_ASSIST_PATH} response was not valid JSON: {body_text!r}") from exc
    if not isinstance(data, dict):
        raise RampAssistHttpError(f"POST {_RAMP_ASSIST_PATH} response was not a JSON object: {body_text!r}")
    return data
