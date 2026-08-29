#!/usr/bin/env python3
"""zones_http_client.py -- pure HTTP client for GET/POST /api/zones
(firmware/KilnFW/App/drivers/zones_http.c), the ONE real write path for the
zones_cfg_t fields config_presets.py could not write over the UART CONTROL
task: max_temp_c, relay_mask, control_mode, thermo_count, relay_count, and
the rest of zone_cfg_t. This is the hook config_presets.py's module docstring
names ("a zones_http_client.py alongside ota_http_client.py"). Same
"stdlib urllib.request, no framework" convention as ota_http_client.py, kept
in its own module for the identical reason: the merge/encode/decode logic
here is unit-tested with mocked HTTP responses
(tools/PcTools/tests/test_zones_http_client.py), no real socket, no live
board.

THE TRAP THIS MODULE EXISTS TO DEFEND AGAINST: POST /api/zones is a
WHOLE-PAGE-SUBMIT endpoint. For almost every field on a zone (relay_mask,
control_mode, max_temp_c, min_temp_c, cal_offset_c, the PID gains, the ramp
ceiling, ...) an OMITTED form field means "set this to zero" -- see
zones_post_handler()/parse_zone_fields() in zones_http.c: `tmp` (the
candidate config) starts zero-initialized, and most per-zone fields are
REQUIRED on every POST (parse_float_field() returning false on a missing key
rejects the whole request only for the top-level thermo_count/relay_count;
every zone field that ISN'T explicitly probed-for-presence-first is simply
absent from `tmp`, i.e. zero, if the key is missing). A client that posts
only the handful of fields a preset happens to care about would silently
zero every other field on that zone -- including max_temp_c, and
max_temp_c == 0.0 is zones_config_get_temp_limits()'s literal "no ceiling
configured" state (see zones_http.c's own comment on that field). On a bench
with heating elements physically connected, that is a config-loader tool
quietly disarming a temperature limit.

The fix applied throughout this module: GET the live config first, MERGE the
preset's fields onto it (never onto a blank struct), and POST the COMPLETE
field set the firmware's whole-page-submit contract expects -- exactly the
fields zones_page.html itself would have sent for an ordinary "load the page,
tweak a couple of fields, save" round trip. build_post_body() below is what
performs that GET-merge-POST; nothing in this module ever constructs a POST
body from the preset alone.

NOT every field on this endpoint uses the omitted-means-zero convention --
knowing which convention a given field uses matters, because getting it
backwards is its own hazard in either direction:
  * z%u_tctype / safety_tc_type / pc_link_abort_silence_ms: OMITTED MEANS
    KEEP THE CURRENT VALUE (falls back to the live s_zones.cfg value in
    zones_http.c). Sent explicitly here anyway (echoed from the GET), same
    as every other field, so this module's behavior does not depend on
    memorizing which fields are safe to omit.
  * relay<N>_name (relay_names_cfg_t, a SEPARATE struct from zones_cfg_t):
    the OPPOSITE convention on purpose -- omitted means "keep the current
    name," specifically so that /settings/safety's whole-page save (which
    has no relay-name inputs at all) can never wipe every relay name just by
    posting to this same endpoint. This module deliberately never sends
    relay<N>_name at all: omitting it is what PRESERVES the operator's relay
    names, the same protection zones_page.html itself relies on for fields
    it doesn't render either.
  * tp%u_* (zone_timing_profile_t, named safety-timing bundles): every field
    within a named profile is REQUIRED once that profile's tp%u_name is
    present at all (parse_timing_profile_fields() has no per-field presence
    probe), so this module always echoes back every timing profile GET
    reported, verbatim, untouched by any preset -- presets do not carry
    timing-profile data (out of scope for this pass; the per-zone thermal
    guard thresholds live there now, not on zone_cfg_t).

DYNAMIC FIELD DERIVATION, and why it matters right now: this module does NOT
hardcode "the list of zones_cfg_t fields as of today." zones_http.c/
zones_page.html are being actively extended (a current-measurement sweep is
landing fields onto GET /api/zones concurrently with this module being
written) -- a hardcoded field list is exactly the kind of thing that goes
stale the next time somebody adds a field, and silently drops it from the
POST the same way an omitted-preset-field would. Instead, build_post_body()
walks every key GET /api/zones actually returned this call and either (a)
recognizes it as read-only telemetry (normal_current_a, safety_wiring, ...)
and leaves it out of the POST on purpose, (b) recognizes it via
_ZONE_FIELD_FORM_KEY/_TOP_FIELD_FORM_KEY/_TIMING_PROFILE_FIELD_FORM_KEY and
echoes/overrides it, or (c) is genuinely unknown to this module, in which
case build_post_body() RAISES rather than silently dropping it. A field this
module doesn't yet know how to round-trip is a bug to fix here, not a field
to zero on the board -- see ZonesHttpUnknownFieldError.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

#: Short GET/POST -- no large payload like OTA's image upload.
ZONES_HTTP_TIMEOUT_S = 8.0


class ZonesHttpError(Exception):
    """Raised for any transport or protocol failure talking to
    GET/POST /api/zones -- unreachable host, a non-2xx response, or a
    response shape this client does not understand."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


class ZonesHttpUnknownFieldError(ZonesHttpError):
    """Raised by build_post_body() when GET /api/zones reports a field this
    module has no known POST mapping for. This is the module's whole-page-
    submit safeguard: an unrecognized field is refused loudly rather than
    silently left out of the POST (which would zero it on the board, if it
    turns out to follow the omitted-means-zero convention most zone fields
    use). Fix: add the field to _ZONE_FIELD_FORM_KEY / _TOP_FIELD_FORM_KEY /
    _TIMING_PROFILE_FIELD_FORM_KEY (or to the matching *_READONLY_KEYS set,
    if it is read-only telemetry like normal_current_a), after checking
    zones_http.c's zones_post_handler()/parse_zone_fields() for the field's
    real form-key name and omitted-value convention."""


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


def get_zones(host: str, timeout: float = ZONES_HTTP_TIMEOUT_S) -> dict:
    """GET /api/zones -- the full live zones_cfg_t (+ relay_names,
    timing_profiles, and read-only telemetry) as JSON, straight from
    zones_get_handler(). This is always the FIRST call any writer in this
    module makes -- see the module docstring's whole-page-submit trap."""
    req = urllib.request.Request(_url(host, "/api/zones"), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise ZonesHttpError(f"GET /api/zones failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise ZonesHttpError(f"GET /api/zones response was not valid JSON: {body_text!r}") from exc


def post_zones(host: str, body: str, timeout: float = ZONES_HTTP_TIMEOUT_S) -> str:
    """POST /api/zones with `body` (application/x-www-form-urlencoded, built
    by build_post_body()). Returns the response text -- "ok" on success
    (zones_post_handler()'s only 2xx path, httpd_resp_sendstr(req, "ok"),
    NOT JSON). A non-2xx response is PLAIN TEXT (httpd_resp_send_err()),
    e.g. "zone relay_mask references an unconfigured relay" -- surfaced
    verbatim in ZonesHttpError.detail, same convention as ota_http_client.py.
    """
    data = body.encode("ascii")
    req = urllib.request.Request(
        _url(host, "/api/zones"),
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Content-Length": str(len(data)),
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise ZonesHttpError(f"POST /api/zones refused: HTTP {status_code}: {detail}",
                              status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        raise ZonesHttpError(f"POST /api/zones unreachable: {detail}") from exc


# ---------------------------------------------------------------------------
# Field mapping: JSON key (as GET /api/zones emits it) -> POST form-key
# SUFFIX (the "z%u_"/"tp%u_" prefix is added by the caller from the zone/
# profile index). Recovered by reading zones_get_handler()'s APPEND() calls
# against parse_zone_fields()/parse_timing_profile_fields()'s snprintf(key,
# ...) calls side by side in zones_http.c -- not guessed. Every mapped key
# below is a REAL field this module can round-trip; anything GET returns that
# isn't in one of these maps or the matching *_READONLY_KEYS set is refused
# by build_post_body() rather than silently dropped -- see
# ZonesHttpUnknownFieldError.
# ---------------------------------------------------------------------------

_ZONE_FIELD_FORM_KEY = {
    "name": "name",
    "relay_mask": "relay_mask",
    "thermo_mask": "thermo_mask",
    "cal_offset_c": "cal",
    "pid_kp": "kp",
    "pid_ki": "ki",
    "pid_kd": "kd",
    "max_ramp_c_per_hr": "ramp",
    "sanity_rate_c_per_min": "sanity",
    "control_mode": "mode",
    "max_temp_c": "maxtemp",
    "min_temp_c": "mintemp",
    "heater_window_ms": "window",
    "heater_min_on_ms": "minon",
    "heater_min_off_ms": "minoff",
    "guard_wrong_dir_window_s": "wrongdirwindow",
    "guard_wrong_dir_rate_c_per_min": "wrongdirrate",
    "guard_off_settle_s": "offsettle",
    "guard_runaway_rate_c_per_min": "runawayrate",
    "guard_runaway_margin_c": "runawaymargin",
    "guard_drift_period_s": "driftperiod",
    "guard_sensor_fault_debounce_ticks": "debounce",
    "guard_frozen_window_s": "frozenwindow",
    "cross_zone_max_delta_c": "xzone",
    "model_k_dc": "k",
    "model_tau_s": "tau",
    "model_dead_time_s": "deadtime",
    "tc_type": "tctype",
    "ct_mask": "ct_mask",
    "timing_profile": "timingprofile",
}
#: Integer-valued zone fields -- posted as a plain int string (parse_u8_field()
#: on the firmware side), never a float repr like "2.0".
_ZONE_INT_FIELDS = {
    "relay_mask", "thermo_mask", "control_mode", "tc_type", "ct_mask", "timing_profile",
}
#: Read-only telemetry zones_get_handler() emits per zone that has NO POST
#: counterpart at all (measured data, or the array index itself) -- excluded
#: from the POST body on purpose, not by omission-means-preserve, since these
#: are never accepted as input in the first place.
_ZONE_READONLY_KEYS = {"index", "normal_current_measured", "normal_current_a"}

_TIMING_PROFILE_FIELD_FORM_KEY = {
    "name": "name",
    "guard_progress_duty_min": "progressduty",
    "guard_progress_window_s": "progresswindow",
    "guard_drift_hysteresis_c": "drifthyst",
    "guard_frozen_eps_c": "frozeneps",
    "guard_cross_zone_period_s": "xzoneperiod",
    "bangbang_hysteresis_c": "bbhyst",
    "cooling_limited_margin_c": "coolmargin",
    "cooling_limited_hold_s": "coolhold",
    "ramp_lock_band_c": "ramplock",
}
_TIMING_PROFILE_READONLY_KEYS = {"index"}

#: Top-level scalar fields (outside zones[]/timing_profiles[]/relay_names[]).
#: continue_on_zone_trip is a JSON bool, posted as "1"/"0" per the firmware's
#: strcmp(val, "1")/strcmp(val, "0") check.
_TOP_FIELD_FORM_KEY = {
    "thermo_count": "thermo_count",
    "relay_count": "relay_count",
    "max_simultaneous_relays": "max_simultaneous_relays",
    "continue_on_zone_trip": "continue_on_zone_trip",
    "safety_tc_type": "safety_tc_type",
    "pc_link_abort_silence_ms": "pc_link_abort_silence_ms",
}
_TOP_INT_FIELDS = {"thermo_count", "relay_count", "max_simultaneous_relays", "safety_tc_type"}
#: Top-level keys GET emits that this module deliberately never echoes back:
#: relay_names/timing_profiles/zones are handled by their own dedicated
#: logic below (not this scalar map), and the rest are read-only telemetry
#: (safety_wiring, ct_warn_mask, relay_zone_owned_mask) with no POST form
#: field at all.
_TOP_READONLY_OR_STRUCTURAL_KEYS = {
    "relay_zone_owned_mask", "safety_wiring", "ct_warn_mask",
    "relay_names", "timing_profiles", "zones",
}


def _format_scalar(key: str, value: Any, int_fields: "set[str]") -> str:
    if key == "continue_on_zone_trip":
        return "1" if value else "0"
    if key in int_fields:
        return str(int(value))
    if isinstance(value, bool):
        # No other bool-valued field is expected here; refuse rather than
        # silently coercing True/False to 1.0/0.0.
        raise ZonesHttpError(f"field {key!r} is a bool but has no known bool encoding")
    if isinstance(value, str):
        return value
    return repr(float(value))


def _encode_zone(index: int, zone: dict) -> "dict[str, str]":
    fields: "dict[str, str]" = {}
    for key, value in zone.items():
        if key in _ZONE_READONLY_KEYS:
            continue
        if key not in _ZONE_FIELD_FORM_KEY:
            raise ZonesHttpUnknownFieldError(
                f"zone {index}: GET /api/zones field {key!r} has no known POST mapping in "
                "zones_http_client.py -- refusing to build a form body that could silently "
                "drop or zero it (see this module's docstring)")
        suffix = _ZONE_FIELD_FORM_KEY[key]
        fields[f"z{index}_{suffix}"] = _format_scalar(key, value, _ZONE_INT_FIELDS)
    return fields


def _encode_timing_profile(index: int, profile: dict) -> "dict[str, str]":
    fields: "dict[str, str]" = {}
    for key, value in profile.items():
        if key in _TIMING_PROFILE_READONLY_KEYS:
            continue
        if key not in _TIMING_PROFILE_FIELD_FORM_KEY:
            raise ZonesHttpUnknownFieldError(
                f"timing profile {index}: GET /api/zones field {key!r} has no known POST "
                "mapping in zones_http_client.py -- refusing to build a form body that could "
                "silently drop or zero it")
        suffix = _TIMING_PROFILE_FIELD_FORM_KEY[key]
        fields[f"tp{index}_{suffix}"] = _format_scalar(key, value, set())
    return fields


#: Zone fields config_presets.json presets are allowed to override -- exactly
#: _REQUIRED_ZONE_FIELDS from config_presets.py, MINUS "index" (used to
#: locate the target zone, never itself posted as a field) -- see that
#: module for the schema this mirrors. A preset zone dict may carry
#: additional keys (k_dc/tau_s/dead_time_s -- the UART-writable model
#: fields); those are ignored here, not rejected, since config_presets.py's
#: apply_preset() sends them over the CONTROL link separately.
_PRESET_ZONE_OVERRIDE_FIELDS = {
    "relay_mask", "control_mode", "cal_offset_c",
    "pid_kp", "pid_ki", "pid_kd", "max_ramp_c_per_hr",
    "max_temp_c", "min_temp_c",
    # 2026-08-28: heater_min_on_ms became a per-zone hardware-protection
    # setting with a 10 s floor (HEATER_MIN_ON_MS_FLOOR), so a preset that
    # pins a bench's relay timing has to be able to carry it. Every field
    # here has ALWAYS round-tripped through _ZONE_FIELD_FORM_KEY -- this set
    # is only about what a preset may OVERRIDE, and a preset that omits the
    # field still echoes whatever the board reported. It is deliberately NOT
    # in config_presets.py's _REQUIRED_ZONE_FIELDS: making it required would
    # invalidate every existing preset for a value they are all happy to
    # inherit. A preset that DOES set it below 10000 is refused by the board,
    # by design -- see zones_http.h's ZONE_HEATER_MIN_ON_MS_FLOOR.
    "heater_min_on_ms",
}


def build_post_body(current: dict, preset: dict) -> str:
    """GET-merge-POST, per the module docstring: `current` is a freshly
    GET /api/zones'd dict; `preset` is a config_presets.py preset (or any
    dict with the same {"thermo_count"?, "relay_count"?, "zones": [...]}
    shape). Returns an application/x-www-form-urlencoded POST body that
    carries the COMPLETE field set zones_post_handler() expects -- every
    field `current` reported, with the preset's fields overlaid on top --
    never a body built from `preset` alone.

    Raises ZonesHttpUnknownFieldError if `current` contains a field this
    module has no POST mapping for (see that class's docstring) -- refusing
    to build a body is the safe failure here, not silently omitting the
    field (which POST would then treat as "set to zero" for most zone
    fields).
    """
    fields: "dict[str, str]" = {}

    # ---- top-level scalars: echo current, let thermo_count/relay_count be
    # overridden by the preset if it names them. ----
    top_overrides: "dict[str, Any]" = {}
    if "thermo_count" in preset:
        top_overrides["thermo_count"] = preset["thermo_count"]
    if "relay_count" in preset:
        top_overrides["relay_count"] = preset["relay_count"]

    seen_top_keys = set()
    for key, value in current.items():
        if key in _TOP_READONLY_OR_STRUCTURAL_KEYS:
            seen_top_keys.add(key)
            continue
        if key not in _TOP_FIELD_FORM_KEY:
            raise ZonesHttpUnknownFieldError(
                f"GET /api/zones top-level field {key!r} has no known POST mapping in "
                "zones_http_client.py -- refusing to build a form body that could silently "
                "drop or zero it (see this module's docstring)")
        seen_top_keys.add(key)
        merged_value = top_overrides.get(key, value)
        fields[_TOP_FIELD_FORM_KEY[key]] = _format_scalar(key, merged_value, _TOP_INT_FIELDS)
    # A top_overrides key GET never reported at all (shouldn't happen against
    # a real board -- thermo_count/relay_count are always emitted -- but a
    # test fixture could omit one) still needs to reach the wire.
    for key, value in top_overrides.items():
        if key not in seen_top_keys:
            fields[_TOP_FIELD_FORM_KEY[key]] = _format_scalar(key, value, _TOP_INT_FIELDS)

    # ---- timing profiles: echoed back verbatim. Presets carry no timing-
    # profile data (out of scope this pass -- see the module docstring's
    # tp%u_* bullet), and at least tp0 is REQUIRED on every POST. ----
    profiles = current.get("timing_profiles") or []
    if not profiles:
        raise ZonesHttpError(
            "GET /api/zones returned no timing_profiles -- POST /api/zones requires at least "
            "tp0_name; refusing to build a body the board would reject")
    for profile in profiles:
        idx = profile["index"]
        fields.update(_encode_timing_profile(idx, profile))

    # ---- zones: current, with the preset's fields (only the ones
    # config_presets.py's schema actually validates) overlaid per zone. ----
    zones = current.get("zones")
    if not zones:
        raise ZonesHttpError("GET /api/zones returned no zones -- nothing to merge the preset onto")
    preset_zones_by_index = {z["index"]: z for z in preset.get("zones", [])}
    for zone in zones:
        idx = zone["index"]
        merged = dict(zone)
        override = preset_zones_by_index.get(idx)
        if override:
            for key, value in override.items():
                if key in _PRESET_ZONE_OVERRIDE_FIELDS:
                    merged[key] = value
        fields.update(_encode_zone(idx, merged))

    return urllib.parse.urlencode(fields)


@dataclass(frozen=True)
class ZonesApplyResult:
    ok: bool
    #: Human-readable mismatches found on read-back verification -- empty
    #: when `ok` is True. Each entry names the zone/field and the
    #: expected-vs-actual values.
    mismatches: "list[str]" = field(default_factory=list)
    post_response: str = ""

    def describe(self) -> str:
        if self.ok:
            return "zones config written and verified against a fresh GET /api/zones read-back"
        lines = ["zones config write did NOT verify -- read-back disagreed with the preset:"]
        lines.extend(f"  {m}" for m in self.mismatches)
        return "\n".join(lines)


def _float_close(a: float, b: float, tol: float = 1e-3) -> bool:
    return abs(float(a) - float(b)) <= tol


def _verify_against_preset(after: dict, preset: dict) -> "list[str]":
    """Compares a fresh GET /api/zones (`after`) against exactly the fields
    the preset asked to be written -- the read-back-and-assert half of the
    tool's contract (never trust a write that wasn't independently
    re-read). Only checks fields the preset actually names, same set
    build_post_body() overlays -- this is not a full-config diff."""
    mismatches: "list[str]" = []
    if "thermo_count" in preset and after.get("thermo_count") != preset["thermo_count"]:
        mismatches.append(
            f"thermo_count: expected {preset['thermo_count']}, board reports {after.get('thermo_count')}")
    if "relay_count" in preset and after.get("relay_count") != preset["relay_count"]:
        mismatches.append(
            f"relay_count: expected {preset['relay_count']}, board reports {after.get('relay_count')}")

    after_zones_by_index = {z["index"]: z for z in after.get("zones", [])}
    for pz in preset.get("zones", []):
        idx = pz["index"]
        az = after_zones_by_index.get(idx)
        if az is None:
            mismatches.append(f"zone {idx}: not present in read-back at all")
            continue
        for key in _PRESET_ZONE_OVERRIDE_FIELDS:
            if key not in pz:
                continue
            expected = pz[key]
            actual = az.get(key)
            if isinstance(expected, float):
                ok = actual is not None and _float_close(expected, actual)
            else:
                ok = actual == expected
            if not ok:
                mismatches.append(f"zone {idx}.{key}: expected {expected!r}, board reports {actual!r}")
    return mismatches


def apply_zone_preset(host: str, preset: dict, timeout: float = ZONES_HTTP_TIMEOUT_S,
                       verify: bool = True) -> ZonesApplyResult:
    """The whole GET-merge-POST-(verify) cycle for one preset, over
    GET/POST /api/zones. Never touches relay_names, never touches timing
    profiles beyond echoing them back unmodified, never sends thermo_count/
    relay_count/zone fields the preset didn't ask for changed -- see
    build_post_body()'s docstring for exactly what gets merged.

    verify=True (the default) re-reads the config after the POST and
    compares it against every field the preset named, returning ok=False
    with the specific mismatches if the board's read-back disagrees --
    this tool's own "POST returned ok, but did it actually land" contract.
    A POST that raises (rejected, unreachable, ...) always propagates as
    ZonesHttpError; ok=False in the returned result is reserved for a POST
    that the board ACKed but a read-back then contradicted.
    """
    current = get_zones(host, timeout)
    body = build_post_body(current, preset)
    response_text = post_zones(host, body, timeout)
    if response_text.strip() != "ok":
        raise ZonesHttpError(f"POST /api/zones returned 200 but body was {response_text!r}, not 'ok'")

    if not verify:
        return ZonesApplyResult(ok=True, post_response=response_text)

    after = get_zones(host, timeout)
    mismatches = _verify_against_preset(after, preset)
    return ZonesApplyResult(ok=not mismatches, mismatches=mismatches, post_response=response_text)
