#!/usr/bin/env python3
"""profile_edit_http_client.py -- pure HTTP client for POST /api/profile
(firmware/KilnFW/App/drivers/http/profiles_edit_http.c's profile_post_handler()),
the ONE write path that can attach an on/off trigger rule
(profile_on_off_rule_t) to a saved profile. Every other PC-side profile save
(devices_profiles.py's profiles_save()/ProfilesClient.save(), the raw UART
PROFILES task 0x03 SAVE command, task_id UART_TASK_ID_PROFILES) goes over the
wire format uart_bridge_ext_control.c's profiles_handle_message() decodes,
and that decoder never reads on/off-rule bytes at all: it memsets a scratch
profile_t to zero (leaving on_off_rule_count == 0) and fills only
name/zone_mask/segments before handing the candidate to profiles_http_save().
Extending that wire format would be a firmware change (new bytes in
PROFILES_CMD_SAVE's payload, parsed by the bridge, on top of the existing
segment loop) -- out of scope for a PC-only fix. The already-existing,
already-ADMIN-tier HTTP form endpoint (profiles_parse_profile_fields()'s
"rule%u_zone"/"rule%u_segment"/... family, ON_OFF_ZONE.md plan step 5)
covers exactly this need without touching firmware, so this module is a thin
wrapper around it -- same "stdlib urllib.request, no framework" convention as
zones_http_client.py/ota_http_client.py.

Response shape (profile_post_handler()): 200 ``{"ok":true,"id":N,
"warnings":[...]}`` on success; 400/500 ``{"ok":false,"error":"..."}`` on
refusal. Unlike zones_http_client's POST /api/zones (a whole-page-submit
endpoint where an omitted field usually means "zero it"), this endpoint's
rule family is a dense 0..N-1 list gated purely on presence of
"rule%u_zone" -- omitting every rule%u_* field is exactly "no on/off rule for
this save", the ordinary case for a segment-only profile.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Optional

from . import http_auth
from .devices_profiles import ProfileSegment

#: Short form POST, no large payload.
PROFILE_EDIT_HTTP_TIMEOUT_S = 8.0


class ProfileEditHttpError(Exception):
    """Raised for any transport or protocol failure talking to
    POST /api/profile -- unreachable host, a non-2xx response, or a response
    body this client does not understand."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


@dataclass(frozen=True)
class OnOffRule:
    """One profile_on_off_rule_t, in the same units profiles_edit_http.c's
    parser expects. ``zone_index``/``segment_index`` are the sparse-array
    keys (profiles_types.h); every other field mirrors
    on_off_trigger_decide.h's on_off_trigger_rule_t. ``phase_mask=0``/
    ``direction_mask=0`` are the documented tautologies ("any phase"/"any
    direction"); ``temp_cmp=0``/``temp_source=0`` mean "no temperature axis"
    (ON_OFF_TEMP_CMP_NONE) and must be paired -- a nonzero temp_cmp with
    temp_source != 1 is silently dropped by profile_resolve_on_off_rule()
    (only temp_source==1, "measured, this zone's own TC", is wired), so
    callers wanting a hysteresis rule must set both."""

    zone_index: int
    segment_index: int
    enable: bool = True
    phase_mask: int = 0
    direction_mask: int = 0
    temp_cmp: int = 0
    temp_source: int = 0
    temp_threshold_c: float = 0.0
    time_start_s: int = 0
    time_stop_s: int = 0
    invert: bool = False


#: on_off_temp_cmp_t (on_off_trigger_decide.h) -- re-exported here so a
#: caller building an OnOffRule does not need to reach into firmware headers
#: or hardcode the encoding.
ON_OFF_TEMP_CMP_NONE = 0
ON_OFF_TEMP_CMP_ABOVE = 1
ON_OFF_TEMP_CMP_BELOW = 2

#: temp_source encoding (profiles_types.h profile_on_off_rule_t.temp_source).
#: Only 0/1 are wired by profile_resolve_on_off_rule() as of
#: ON_OFF_ZONE.md plan step 5.
ON_OFF_TEMP_SOURCE_NONE = 0
ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE = 1


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


def build_post_fields(
    profile_id: int,
    name: str,
    zone_mask: int,
    segments: "list[ProfileSegment]",
    on_off_rules: "Optional[list[OnOffRule]]" = None,
) -> "list[tuple[str, str]]":
    """Builds the ``rule%u_*``/``seg%u_*`` form-field list
    ``profiles_parse_profile_fields()`` expects. ``profile_id`` targets an
    existing user slot (create-or-overwrite, same convention as
    profile_post_handler()'s ``id`` field: 0..7 addresses that exact slot).
    ``on_off_rules`` defaults to none -- an ordinary segment-only save, same
    as every existing caller of this family. A list longer than
    PROFILE_MAX_ON_OFF_RULES (8) is refused by the firmware (validate_
    on_off_rules()), not checked here -- this module does not duplicate
    firmware-side range validation, same convention as zones_http_client.py.
    """
    fields: "list[tuple[str, str]]" = [
        ("id", str(profile_id)),
        ("name", name),
        ("zone_mask", str(zone_mask)),
        ("seg_count", str(len(segments))),
    ]
    for i, seg in enumerate(segments):
        fields.append((f"seg{i}_target", repr(seg.target_c)))
        fields.append((f"seg{i}_ramp", repr(seg.ramp_c_per_hr)))
        fields.append((f"seg{i}_dwell", str(seg.dwell_min)))
    for i, rule in enumerate(on_off_rules or []):
        fields.append((f"rule{i}_zone", str(rule.zone_index)))
        fields.append((f"rule{i}_segment", str(rule.segment_index)))
        fields.append((f"rule{i}_enable", "1" if rule.enable else "0"))
        fields.append((f"rule{i}_phase", str(rule.phase_mask)))
        fields.append((f"rule{i}_direction", str(rule.direction_mask)))
        fields.append((f"rule{i}_temp_cmp", str(rule.temp_cmp)))
        fields.append((f"rule{i}_temp_source", str(rule.temp_source)))
        fields.append((f"rule{i}_temp_c", repr(rule.temp_threshold_c)))
        fields.append((f"rule{i}_time_start_s", str(rule.time_start_s)))
        fields.append((f"rule{i}_time_stop_s", str(rule.time_stop_s)))
        fields.append((f"rule{i}_invert", "1" if rule.invert else "0"))
    return fields


def post_profile(
    host: str,
    profile_id: int,
    name: str,
    zone_mask: int,
    segments: "list[ProfileSegment]",
    on_off_rules: "Optional[list[OnOffRule]]" = None,
    timeout: float = PROFILE_EDIT_HTTP_TIMEOUT_S,
) -> dict:
    """POST /api/profile (application/x-www-form-urlencoded), ADMIN tier.
    Returns the parsed ``{"ok":true,"id":N,"warnings":[...]}`` body on
    success. Raises :class:`ProfileEditHttpError` on any transport failure,
    non-2xx response, or a response body that is not the JSON this handler
    always sends (never partial/plain-text on this route, unlike
    POST /api/zones)."""
    fields = build_post_fields(profile_id, name, zone_mask, segments, on_off_rules)
    data = urllib.parse.urlencode(fields).encode("ascii")
    req = urllib.request.Request(
        _url(host, "/api/profile"),
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Content-Length": str(len(data)),
        },
    )
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise ProfileEditHttpError(
            f"POST /api/profile refused: HTTP {status_code}: {detail}", status_code, detail
        ) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise ProfileEditHttpError(f"POST /api/profile unreachable: {detail}") from exc
    try:
        parsed = json.loads(body_text)
    except Exception as exc:
        raise ProfileEditHttpError(
            f"POST /api/profile response was not valid JSON: {body_text!r}"
        ) from exc
    if not isinstance(parsed, dict) or not parsed.get("ok"):
        error = parsed.get("error") if isinstance(parsed, dict) else None
        raise ProfileEditHttpError(f"POST /api/profile refused: {error or body_text!r}")
    return parsed


__all__ = [
    "ProfileEditHttpError",
    "OnOffRule",
    "ON_OFF_TEMP_CMP_NONE",
    "ON_OFF_TEMP_CMP_ABOVE",
    "ON_OFF_TEMP_CMP_BELOW",
    "ON_OFF_TEMP_SOURCE_NONE",
    "ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE",
    "build_post_fields",
    "post_profile",
]
