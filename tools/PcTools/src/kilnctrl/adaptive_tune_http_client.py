#!/usr/bin/env python3
"""adaptive_tune_http_client.py -- pure HTTP client for
GET /api/adaptive_tune and POST /api/adaptive_tune/enable
(firmware/KilnFW/App/drivers/http/adaptive_tune_http.c), the only surface for
Phase 7d's continuous/adaptive PID-tuning layer. Same "stdlib
urllib.request, no framework" convention as ota_http_client.py/
safety_cfg_http_client.py/zones_http_client.py, and unit-tested against
mocked HTTP only (tools/PcTools/tests/test_adaptive_tune_http_client.py) --
no real socket, no live board.

Built against the handler as it stood 2026-09-01 in
firmware/KilnFW/App/drivers/http/adaptive_tune_http.c/.h and
firmware/KilnFW/App/drivers/control/adaptive_tune.h's ``adaptive_tune_zone_status_t``.
That surface is actively changing under a different agent (a revert endpoint
is being added, and the opt-in flag is being moved into the zone config
blob) -- this client only reads the fields the GET body actually names below
and never assumes any that aren't documented there. A GET response missing a
field this client expects is a live sign that surface moved; see
_zone_status_from_json()'s tolerant parsing (missing keys default rather
than raising) so a field addition/rename on the firmware side degrades
gracefully instead of breaking every existing caller outright.

WHY THIS IS ITS OWN MODULE, not folded into ota_http_client.py or
zones_http_client.py: different endpoint family (adaptive tuning, not
OTA/zone-config), different write shape (POST /api/adaptive_tune/enable is a
two-field form body, not a whole-page submit or an HMAC-signed binary
upload), and it is read-mostly -- one narrow write (the per-zone opt-in
flag) versus those modules' much larger surfaces.

SAFETY NOTE for the enable/disable write: adaptive_tune_run_end() only ever
applies a gain change at a firing's run-end boundary (adaptive_tune.h's own
top comment), never mid-firing -- so flipping the opt-in flag itself cannot
disturb a live control loop the instant it lands. It DOES change what
happens the next time the CURRENTLY RUNNING firing ends, though, if one is
in progress -- see mcp_server_adaptive_tune.py's set_enabled tool docstring
for the confirm-gating this module's caller applies on top of this client
(this module itself has no gating opinion; it is a thin, honest wire
client)."""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request

from . import host_resolve, http_auth
from dataclasses import dataclass
from typing import Any, Optional

#: A plain GET/POST against the board's own HTTP server -- no long-running
#: operation on the far side (unlike safety commissioning's blocking
#: SET_PARAM/COMMIT_CONFIG round trip to the Pico), so this stays at the
#: same default as ota's simplest reads.
ADAPTIVE_TUNE_HTTP_TIMEOUT_S = 8.0

_STATUS_PATH = "/api/adaptive_tune"
_ENABLE_PATH = "/api/adaptive_tune/enable"
_REVERT_PATH = "/api/adaptive_tune/revert"

#: Same fallback-AP address ota_http_client.py's OTA_AP_DEFAULT_HOST names --
#: the board's own AP-mode IP when no station connection is up.
ADAPTIVE_TUNE_AP_DEFAULT_HOST = host_resolve.resolve_default_host()  # was a hardcoded "192.168.4.1"
class AdaptiveTuneHttpError(Exception):
    """Any transport or protocol failure talking to
    GET /api/adaptive_tune or POST /api/adaptive_tune/enable -- unreachable
    host, non-2xx, or a response shape this client does not understand."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


@dataclass(frozen=True)
class AdaptiveTuneZoneStatus:
    """One zone's row from GET /api/adaptive_tune, mirroring
    ``adaptive_tune_zone_status_t`` (adaptive_tune.h) field-for-field as
    serialized by ``status_get_handler()`` (adaptive_tune_http.c)."""

    zone: int
    enabled: bool
    observation_count: int
    observations_lifetime: int
    has_applied: bool
    prior_k_dc: float
    applied_k_dc: float
    delta_pct: float
    last_profile_id: int
    last_applied_unix_s: int
    refusal: str
    # Full coupled identification (PID_EXPANSION_PLAN.md 3.3, layer 2).
    joint_observations: int
    coupled_attempted: bool
    coupled_applied: bool
    coupled_cells_changed: int
    coupled_refusal: str
    # Integral (Ki) diagnosis from dwells (same section, layer 2).
    ki_verdict: int
    ki_correction_pct: float
    ki_applied: bool
    ki_refusal: str
    # U1: one-click revert (added after this client's first pass -- see
    # module docstring). Defaults False for a firmware build that predates
    # this field, same "older peer, tolerant default" convention every other
    # field here already follows.
    revert_available: bool = False

    #: adaptive_tune_ki_verdict_t (adaptive_tune.h), mirrored here as a name
    #: table so a caller doesn't have to import the firmware header to make
    #: sense of the raw uint8_t the wire carries.
    KI_VERDICT_NAMES = {
        0: "insufficient", 1: "ok", 2: "offset_too_small",
        3: "floored", 4: "oscillating", 5: "limit_cycle",
    }

    @property
    def ki_verdict_name(self) -> str:
        return self.KI_VERDICT_NAMES.get(self.ki_verdict, f"unknown({self.ki_verdict})")


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


def _zone_status_from_json(entry: dict) -> AdaptiveTuneZoneStatus:
    """Tolerant decode of one element of the GET's ``zones`` array -- a
    missing key defaults rather than raising (see module docstring on why:
    this handler's surface is actively being extended by another agent's
    work in flight). An unexpected key is simply ignored."""
    return AdaptiveTuneZoneStatus(
        zone=int(entry.get("zone", 0)),
        enabled=bool(entry.get("enabled", False)),
        observation_count=int(entry.get("observation_count", 0)),
        observations_lifetime=int(entry.get("observations_lifetime", 0)),
        has_applied=bool(entry.get("has_applied", False)),
        prior_k_dc=float(entry.get("prior_k_dc", 0.0)),
        applied_k_dc=float(entry.get("applied_k_dc", 0.0)),
        delta_pct=float(entry.get("delta_pct", 0.0)),
        last_profile_id=int(entry.get("last_profile_id", 0)),
        last_applied_unix_s=int(entry.get("last_applied_unix_s", 0)),
        refusal=str(entry.get("refusal", "")),
        joint_observations=int(entry.get("joint_observations", 0)),
        coupled_attempted=bool(entry.get("coupled_attempted", False)),
        coupled_applied=bool(entry.get("coupled_applied", False)),
        coupled_cells_changed=int(entry.get("coupled_cells_changed", 0)),
        coupled_refusal=str(entry.get("coupled_refusal", "")),
        ki_verdict=int(entry.get("ki_verdict", 0)),
        ki_correction_pct=float(entry.get("ki_correction_pct", 0.0)),
        ki_applied=bool(entry.get("ki_applied", False)),
        ki_refusal=str(entry.get("ki_refusal", "")),
        revert_available=bool(entry.get("revert_available", False)),
    )


def get_status(host: str, timeout: float = ADAPTIVE_TUNE_HTTP_TIMEOUT_S) -> "list[AdaptiveTuneZoneStatus]":
    """GET /api/adaptive_tune -- every zone's adaptive-tune status, as
    ``status_get_handler()`` builds it: ``{"zones":[{...}, ...]}``, one
    entry per MAX31856_CHANNEL_COUNT zone (unconfigured zones still appear,
    reading as their struct-zero defaults -- ``enabled=false``, no
    observations, no applied change)."""
    req = urllib.request.Request(_url(host, _STATUS_PATH), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise AdaptiveTuneHttpError(f"GET {_STATUS_PATH} failed: {detail}", status, detail) from exc
    try:
        data = json.loads(body_text)
    except Exception as exc:
        raise AdaptiveTuneHttpError(
            f"GET {_STATUS_PATH} response was not valid JSON: {body_text!r}") from exc
    zones = data.get("zones") if isinstance(data, dict) else None
    if not isinstance(zones, list):
        raise AdaptiveTuneHttpError(f"GET {_STATUS_PATH} response has no zones list: {body_text!r}")
    return [_zone_status_from_json(z) for z in zones if isinstance(z, dict)]


def set_enabled(host: str, zone: int, enabled: bool,
                 timeout: float = ADAPTIVE_TUNE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/adaptive_tune/enable -- form body ``zone=<n>&enabled=<0|1>``,
    exactly what ``enable_post_handler()`` parses (http_form_find_field(),
    not JSON). Returns the decoded JSON body: ``{"ok":true}`` on a clean
    write, or ``{"ok":true,"warning":"applied live, save failed"}`` if the
    flag took effect in RAM but the NVS write behind it failed (same
    "applied live, logged if the save failed" convention
    adaptive_tune.h's ``adaptive_tune_set_enabled()`` documents) -- both are
    ``ok`` from the wire's point of view; a caller that cares about the
    warning should check for the key.

    This function has NO safety opinion of its own -- see
    mcp_server_adaptive_tune.py's ``adaptive_tune_set_enabled()`` tool for
    the confirm-gate applied above this thin wire client."""
    body = urllib.parse.urlencode({"zone": str(int(zone)), "enabled": "1" if enabled else "0"}).encode("ascii")
    req = urllib.request.Request(
        _url(host, _ENABLE_PATH), data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded",
                 "Content-Length": str(len(body))},
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise AdaptiveTuneHttpError(f"POST {_ENABLE_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise AdaptiveTuneHttpError(f"POST {_ENABLE_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise AdaptiveTuneHttpError(
            f"POST {_ENABLE_PATH} returned 200 but the body was not JSON: {text!r}") from exc


def revert(host: str, zone: int, timeout: float = ADAPTIVE_TUNE_HTTP_TIMEOUT_S) -> dict:
    """POST /api/adaptive_tune/revert -- U1's one-click revert, form body
    ``zone=<n>`` (revert_post_handler(), adaptive_tune_http.c). Undoes the
    zone's last applied adaptive-tune refinement via
    ``adaptive_tune_revert()`` (firmware side). Returns the decoded JSON
    body: ``{"ok":true}`` on success, or ``{"ok":false,"reason":"..."}``
    with the firmware's own account of why (e.g. nothing to revert) --
    both are a real answer from a reachable board, not a transport failure,
    so a caller sees an ``{"ok":false}`` as a failed write, not an
    exception. Only present on a firmware build recent enough to report
    ``revert_available`` in GET /api/adaptive_tune's status -- see this
    module's docstring on the surface still being under active development.

    This function has NO safety opinion of its own -- see
    mcp_server_adaptive_tune.py's ``adaptive_tune_revert()`` tool for the
    confirm-gate applied above this thin wire client."""
    body = urllib.parse.urlencode({"zone": str(int(zone))}).encode("ascii")
    req = urllib.request.Request(
        _url(host, _REVERT_PATH), data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded",
                 "Content-Length": str(len(body))},
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise AdaptiveTuneHttpError(f"POST {_REVERT_PATH} refused: HTTP {status}: {detail}",
                                     status, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise AdaptiveTuneHttpError(f"POST {_REVERT_PATH} unreachable: {detail}") from exc
    try:
        return json.loads(text)
    except Exception as exc:
        raise AdaptiveTuneHttpError(
            f"POST {_REVERT_PATH} returned 200 but the body was not JSON: {text!r}") from exc
