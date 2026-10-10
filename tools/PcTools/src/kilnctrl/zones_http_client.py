#!/usr/bin/env python3
"""zones_http_client.py -- pure HTTP client for GET/POST /api/zones
(firmware/KilnFW/App/drivers/http/zones_http.c), the ONE real write path for the
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
  * z%u_tctype / pc_link_abort_silence_ms: OMITTED MEANS KEEP THE CURRENT
    VALUE (falls back to the live s_zones.cfg value in zones_http.c). Sent
    explicitly here anyway (echoed from the GET), same as every other
    field, so this module's behavior does not depend on memorizing which
    fields are safe to omit.
  * safety_tc_type: DEPRECATED as a write path, 2026-09-15 (owner decision,
    "the commissioning page owns the type" -- Opus review F3). ANY
    submitted value is now ignored by zones_http_post.c -- the live value
    always wins, submitted or omitted alike. This module treats it as
    read-only (see _TOP_READONLY_OR_STRUCTURAL_KEYS below), never sends it
    in a POST body, and only ever surfaces the value GET reports.
  * relay<N>_name (relay_names_cfg_t, a SEPARATE struct from zones_cfg_t):
    the OPPOSITE convention on purpose -- omitted means "keep the current
    name," specifically so that /settings/safety's whole-page save (which
    has no relay-name inputs at all) can never wipe every relay name just by
    posting to this same endpoint. This module deliberately never sends
    relay<N>_name at all: omitting it is what PRESERVES the operator's relay
    names, the same protection zones_page.html itself relies on for fields
    it doesn't render either.
  * z%u_fuzzy_strength / z%u_coupling_c%u / z%u_settings_source: OMITTED
    MEANS KEEP THE CURRENT VALUE, same convention as z%u_tctype above --
    these are measured/operator-set quantities, not safe-to-zero. Sent
    explicitly here anyway, same as every other field. The coupling
    diagonal (j == the zone's own index) is additionally special: the
    firmware force-ranges it to exactly 0 and _encode_zone() refuses to
    post it nonzero even if a caller's merged dict somehow carried one in.
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
import re
import urllib.error
import urllib.parse
import urllib.request

from . import http_auth
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


class ZonesHttpGenerationMismatchError(ZonesHttpError):
    """Raised by merge_zones_diag() (docs/audits/
    zones_field_sourcing_and_generation_2026-09-14.md) when /api/zones and
    /api/zones_diag report different zones_config_generation() values -- a
    config write landed between the two GETs, so the merge would pair a
    gain from one config generation with an operating point from another.
    Not a subclass distinction callers need to special-case for transport
    reasons; it exists so "the merge is internally inconsistent" is a
    distinct, catchable failure mode from "a request failed"."""


#: Substring of system_mode_gate_check()'s own reason text
#: (firmware/KilnFW/App/drivers/safety/system_mode_gate.c, SYS_ACTION_WRITE_
#: ZONES_CONFIG/SYS_ACTION_FACTORY_RESET/SYS_ACTION_CFGFS_FORMAT) common to
#: every action it gates, used below to tell a mode-gate 409 apart from any
#: other 409 an admin-tier write route can answer with (e.g. this module's
#: own safety_ceiling_raise_failed, or kiln_cfg_http.c's own conflicts).
#: Never matched against OTA's 428 -- that status is structurally distinct
#: (system_mode_gate_http.c's own header comment, owner decision Q4) and
#: this text does not appear in a 428 body.
_SYSTEM_MODE_GATE_REFUSAL_MARKER = "firing or autotune run is active"


def is_system_mode_gate_refusal(detail: Optional[str]) -> bool:
    """True if an HTTP 409's plain-text body was sent by system_mode_gate_
    http_send_refusal() (system_mode_gate_http.c) -- i.e. this write was
    refused because a firing or autotune run is active, not for some other
    409 reason (this module's own safety_ceiling_raise_failed, a different
    route's own conflict, etc). A caller that only checks `status == 409`
    without this distinction will misreport which of several possible 409
    reasons actually happened."""
    return bool(detail) and _SYSTEM_MODE_GATE_REFUSAL_MARKER in detail


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
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise ZonesHttpError(f"GET /api/zones failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise ZonesHttpError(f"GET /api/zones response was not valid JSON: {body_text!r}") from exc


def get_zones_diag(host: str, timeout: float = ZONES_HTTP_TIMEOUT_S) -> dict:
    """GET /api/zones_diag (docs/audits/zones_diag_endpoint_split_2026-09-14.md,
    implementing docs/audits/zones_json_headroom_plan_2026-09-14.md sec 3a):
    the read-only, tool/diagnostics-oriented per-zone fields split OFF
    GET /api/zones once that endpoint ran low on json_cap headroom --
    coupling_tau_c%u/coupling_dead_time_c%u and model_fit_temp_c/
    model_fit_ambient_c. NOT included here: tuning_* -- that record stayed on
    GET /api/zones because zones_page.html's renderTuningQuality() actually
    renders it from the SAME /api/zones fetch (re-verified directly against
    the page source before this split; the original headroom plan's claim
    that tuning_* was diagnostics-only was wrong).

    Response shape: {"zones": [{"index": N, "coupling_tau_c0": ..., "coupling_
    dead_time_c0": ..., "model_fit_temp_c": ..., "model_fit_ambient_c": ...},
    ...]}, indexed the same way GET /api/zones' own "zones" array is, so a
    caller that wants the pre-split combined shape can zip the two by
    "index" -- see merge_zones_diag() below for exactly that."""
    req = urllib.request.Request(_url(host, "/api/zones_diag"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise ZonesHttpError(f"GET /api/zones_diag failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise ZonesHttpError(f"GET /api/zones_diag response was not valid JSON: {body_text!r}") from exc


#: The fields GET /api/zones_diag emits per zone, alongside "index" -- kept
#: as one place so merge_zones_diag() and any future consumer agree on
#: exactly what moved. Mirrors _ZONE_MODEL_FIT_READONLY_KEYS plus the
#: coupling_tau/dead_time cell pattern (_ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE)
#: -- both of those sets/regexes are UNCHANGED by the split (they still
#: describe what these keys mean and how they're excluded from POST bodies),
#: this is just "which keys does the diag response carry".
_ZONES_DIAG_MODEL_FIT_KEYS = {"model_fit_temp_c", "model_fit_ambient_c"}


def merge_zones_diag(zones_json: dict, diag_json: dict) -> dict:
    """Merge a GET /api/zones_diag response's per-zone fields back into a
    GET /api/zones response's zone dicts, by "index", so a caller that wants
    the pre-2026-09-14-split combined shape (e.g. a renderer written before
    the split, or a display tool with no reason to track which endpoint owns
    which field) can get it back without duplicating the merge logic.
    Returns a NEW dict (zones_json is not mutated) -- shallow-copies the
    top-level dict and the zones list, but each zone dict is itself copied
    before its diag fields are added, so callers holding a reference to the
    original zones_json's zone dicts never see them mutated out from under
    them.

    Does nothing destructive if diag_json has no matching index (leaves that
    zone as GET /api/zones reported it, un-merged) -- a partial/short
    zones_diag response degrades to "diag fields missing for that zone", not
    a crash.

    docs/audits/zones_field_sourcing_and_generation_2026-09-14.md (opus
    review, defect 2): the two GETs are separate HTTP requests with no
    shared snapshot, and autotune_engine.c's finalize_fit() writes model_
    k_dc/model_tau_s on /api/zones AND model_fit_temp_c/coupling_tau_c%u/
    coupling_dead_time_c%u on /api/zones_diag -- a finalize landing between
    the two requests can pair a NEW gain with an OLD fit operating point,
    exactly the misattribution model_fit_temp_c exists to prevent. Both
    responses now carry "generation" (zones_config_generation(), bumped by
    every firmware config setter). RAISES ZonesHttpGenerationMismatchError
    if the two disagree, rather than silently returning a merged view that
    never existed on the board -- a caller that wants the old best-effort
    behaviour can catch that specific exception and use zones_json
    unmerged (it still carries every /api/zones field on its own). A
    missing "generation" key on EITHER side (older, pre-2026-09-14
    firmware) is NOT treated as a mismatch -- there is nothing to compare,
    and refusing to merge against boards that predate this field would
    regress every existing caller for no safety gain."""
    zones_gen = zones_json.get("generation")
    diag_gen = diag_json.get("generation")
    if zones_gen is not None and diag_gen is not None and zones_gen != diag_gen:
        raise ZonesHttpGenerationMismatchError(
            "merge_zones_diag() refused: /api/zones generation "
            f"{zones_gen} != /api/zones_diag generation {diag_gen} -- a config "
            "write (e.g. an autotune finalize) landed between the two GETs, "
            "so merging would pair a gain from one config generation with an "
            "operating point from another. Re-fetch both, or handle "
            "ZonesHttpGenerationMismatchError explicitly if a stale-but-"
            "internally-consistent view of /api/zones alone is acceptable."
        )
    diag_by_index = {}
    for dz in diag_json.get("zones", []):
        idx = dz.get("index")
        if idx is not None:
            diag_by_index[idx] = dz
    merged = dict(zones_json)
    merged_zones = []
    for z in zones_json.get("zones", []):
        z2 = dict(z)
        dz = diag_by_index.get(z.get("index"))
        if dz is not None:
            for k, v in dz.items():
                if k == "index":
                    continue
                z2[k] = v
        merged_zones.append(z2)
    merged["zones"] = merged_zones
    return merged


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
        with http_auth.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        status_code, detail = _http_error_detail(exc)
        raise ZonesHttpError(f"POST /api/zones refused: HTTP {status_code}: {detail}",
                              status_code, detail) from exc
    except urllib.error.URLError as exc:
        _, detail = _http_error_detail(exc)
        if isinstance(getattr(exc, "reason", None), TimeoutError):
            raise ZonesHttpError(f"POST /api/zones timed out after {timeout:g}s: the board may have "
                                 f"APPLIED it -- state UNKNOWN, read GET /api/zones back before retrying") from exc
        raise ZonesHttpError(f"POST /api/zones unreachable: {detail}") from exc
    except OSError as exc:  # TimeoutError / ConnectionResetError mid-response: the POST may have landed
        raise ZonesHttpError(f"POST /api/zones got no complete reply ({type(exc).__name__}: {exc}): the board "
                             f"may have APPLIED it -- state UNKNOWN, read GET /api/zones back before retrying") from exc


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
    # PID_EXPANSION_PLAN.md Phase 2/4 (2026-08-30). zones_http.c line ~4834:
    # snprintf(key, sizeof(key), "z%u_fuzzy_strength", i) -- NOT
    # "z%u_fuzzy_strength_pct"; the JSON key and the form-key suffix differ.
    # Omitted-on-POST means PRESERVE the current value (parse_zone_fields()),
    # not zero -- irrelevant to this module since every GET field is always
    # echoed back explicitly.
    "fuzzy_strength_pct": "fuzzy_strength",
    # settings_source: zones_http.c line ~4877, snprintf(key, ..., "z%u_settings_source", i).
    # 0xFF (ZONE_SETTINGS_SOURCE_CUSTOM) or another zone's index; self-reference
    # and cycle-forming chains are refused by the firmware itself.
    "settings_source": "settings_source",
    # settings_source_groups (docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs, ZONES_CFG_VERSION
    # 20->21) is handled separately in _encode_zone() below -- it is a nested
    # dict of {group_name: value}, not a scalar, so it cannot go through
    # _format_scalar() the way every other entry in this table does. See
    # _SRC_GROUP_NAMES/_SRC_GROUP_FORM_SUFFIX below.
    # ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md section 3.2 follow-up,
    # 2026-09-02): coupling_diag_k_dc -- the coupling identification's own
    # diagonal cell, a SEPARATE measured DC gain from model_k_dc/ff_k_dc (see
    # zone_cfg_t::coupling_diag_k_dc's own doc comment in zones_config_json.h
    # for why). zones_http_handlers.c: snprintf(key, ..., "z%u_coupling_diag_k_dc", i)
    # -- unlike coupling_tau_c%u/coupling_dead_time_c%u below, this DOES have a
    # real POST wire field (parse_zone_fields() accepts it, omitted preserves
    # the current value -- same convention as fuzzy_strength_pct/coupling_c%u
    # above). Same JSON key and form-key suffix, no name translation needed.
    "coupling_diag_k_dc": "coupling_diag_k_dc",
    # ZONES_CFG_VERSION 16->17 (PID_EXPANSION_PLAN.md sec 3.6d, 2026-09-04):
    # the terminal ease-off taper window multiplier, moved PER-ZONE -- was a
    # single top-level "ease_off_window_mult" scalar (see the now-removed
    # _TOP_FIELD_FORM_KEY entry's own history) applied board-wide, which
    # could not give z0 alone a wider window without moving z1/z2's too.
    # zones_http_post_parse.c: snprintf(key, ..., "z%u_easeoffmult", i) --
    # same JSON key on the GET side, different (shorter) form-key suffix on
    # POST, same as fuzzy_strength_pct's own name split above. Omitted-on-
    # POST preserves the current per-zone value (same convention as
    # coupling_diag_k_dc just above).
    "ease_off_window_mult": "easeoffmult",
    # ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_
    # TARGET_DESIGN_STUDY.md option (b), 2026-09-04): the per-zone approach-
    # rate cap -- a per-zone ceiling on how fast this zone's own commanded
    # setpoint may approach the shared segment target, which can only ever
    # TIGHTEN the segment's own ramp_c_per_hr, never loosen it. 0.0 = "this
    # zone reports its own actual commanded rate as a ceiling to no one" --
    # unlike ease_off_window_mult's 0 (substitutes a firmware DEFAULT), this
    # 0 means uncapped outright, with no other value substituted (see
    # zone_cfg_t::approach_rate_cap_c_per_hr's own doc comment,
    # zones_config_json.h). zones_http_post_parse.c:
    # snprintf(key, ..., "z%u_approachratecap", i) -- same JSON key on the
    # GET side (zones_http_get.c), a shorter form-key suffix on POST, same
    # split as ease_off_window_mult's own "easeoffmult" just above (both
    # comfortably inside char key[24]: "z0_approachratecap" is 18 chars + a
    # NUL). Omitted-on-POST preserves the current per-zone value, same
    # convention as ease_off_window_mult/coupling_diag_k_dc above.
    "approach_rate_cap_c_per_hr": "approachratecap",
    # ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g, 2026-09-04):
    # pid_fuzzy.c's two triangular-membership half-widths, promoted from
    # compile-time constants to per-zone config -- logs/coupling/
    # fuzzy_bands_envelope_20260904e_report.md found this rig has never once
    # left the centre rule cell at the shipped 20.0/0.5 widths (peak error
    # 29% of band, peak rate 33% of band), which makes trying a rescaled,
    # relabeled, or dropped fuzzy layer an owner decision -- this mapping is
    # what lets that decision be tried as a config POST instead of a
    # reflash. 0.0 = "use the firmware default" (20.0 degC / 0.5 degC/s),
    # same sentinel convention as ease_off_window_mult's own 0 above (NOT
    # approach_rate_cap_c_per_hr's "0 = off" convention just above it).
    # zones_http_post_parse.c: snprintf(key, ..., "z%u_errorband"/
    # "z%u_rateband", i) -- same JSON key on the GET side (zones_http_get.c),
    # a shorter form-key suffix on POST, same split as ease_off_window_mult/
    # approach_rate_cap_c_per_hr's own name splits above (both comfortably
    # inside char key[24]: "z0_errorband"/"z0_rateband" are 12/11 chars + a
    # NUL). Omitted-on-POST preserves the current per-zone value, same
    # convention as every other field in this dict.
    "error_band_c": "errorband",
    "rate_band_c_per_s": "rateband",
    # ZONES_CFG_VERSION 21->22 (Opus review of 992f3954, 2026-09-06): the
    # per-zone dwell-progress band width. zones_http_post_parse.c:
    # snprintf(key, ..., "z%u_progressband", i) -- same JSON key on the GET
    # side (zones_http_get.c's "progress_band_c"), a shorter form-key suffix
    # on POST, same split as error_band_c/rate_band_c_per_s above. 0.0 =
    # "use the firmware default" (see zones_config_get_progress_band_c()'s
    # own comment). Omitted-on-POST preserves the current per-zone value,
    # same convention as every other field in this dict.
    "progress_band_c": "progressband",
    # RELAY_LIFE_BUDGET.md (ZONES_CFG_VERSION 19->20): which contact-life
    # budget this zone's relay_mask relays are rated for (0=SSR/1=Contactor/
    # 2=Mercury). Found missing from this table by
    # zones_per_zone_field_table_checks() below -- the same silent-drop
    # shape as every other entry added 2026-09-08, just never hit on
    # hardware yet since no test or preset had exercised a whole-page
    # round-trip against a board with a non-default relay_type set.
    # zones_http_post_parse.c: snprintf(key, ..., "z%u_relaytype", i).
    "relay_type": "relaytype",
    # ZONES_CFG_VERSION 22->23 (docs/ON_OFF_ZONE.md step 6, 2026-09-08):
    # zone_type/failsafe_state/hyst_c/min_on_s/min_off_s, added alongside the
    # on/off zone UI pass. zones_http_post_parse.c: snprintf(key, ...,
    # "z%u_zonetype"/"z%u_failsafe"/"z%u_hystc"/"z%u_minons"/"z%u_minoffs",
    # i) -- read from that file directly, not guessed (the earlier
    # progress_band_c fix found the real key was "progressband", not the
    # obvious guess -- same caution applied here). Omitted-on-POST preserves
    # the current value for every one of these five, same convention as
    # every other field in this dict. failsafe_state is emitted by
    # zones_http_get.c as a JSON bool ("true"/"false", not 0/1) -- see its
    # own bool handling in _format_scalar() below, same treatment as
    # continue_on_zone_trip.
    "zone_type": "zonetype",
    "failsafe_state": "failsafe",
    "hyst_c": "hystc",
    "min_on_s": "minons",
    "min_off_s": "minoffs",
    # ZONES_CFG_VERSION 24->25 (owner request: per-coil nameplate wattage
    # override). 0 = not overridden, use an equal share of the safety
    # page's whole-kiln sum nameplate -- see zones_config_json.h's
    # coil_power_w comment. Same "optional, falls back to current_z on
    # omit" POST handling as hyst_c/min_on_s/min_off_s above.
    "coil_power_w": "coilpower",
}
#: docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs (ZONES_CFG_VERSION 20->21, Opus review of
#: 5672719 item 4): the five independent settings_source groups, in the
#: exact order/spelling zones_http_post_parse.c's SRC_GROUP_NAMES array uses
#: -- GET /api/zones emits "settings_source_groups":{"limits":N,
#: "relaytiming":N,"control":N,"guards":N,"tc":N} (zones_http_get.c) and
#: parse_zone_fields() accepts each back as z%u_settings_source_<name>. Kept
#: as a plain tuple, not a dict, since it is only ever iterated, never
#: looked up by name.
_SRC_GROUP_NAMES = ("limits", "relaytiming", "control", "guards", "tc")
#: Integer-valued zone fields -- posted as a plain int string (parse_u8_field()
#: on the firmware side), never a float repr like "2.0".
_ZONE_INT_FIELDS = {
    "relay_mask", "thermo_mask", "control_mode", "tc_type", "ct_mask", "timing_profile",
    "settings_source",
    # zone_type: enum (0=heater, 1=on/off device), posted with parse_u8_field()
    # on the firmware side, same as tc_type/ct_mask above.
    "zone_type",
    # relay_type: enum (0=SSR/1=Contactor/2=Mercury), same treatment.
    "relay_type",
    # min_on_s/min_off_s: uint16_t seconds. zones_http_get.c emits them as
    # plain integers ("%u", not "%.3f"), and zones_http_post_parse.c parses
    # them via the float helper only because it exceeds
    # parse_u8_field()'s 0-255 range (see that file's own comment) -- the
    # value itself is still a whole-second count on both sides, so encode
    # as int here too rather than "30.0".
    "min_on_s", "min_off_s",
}
#: zones_http.c emits the coupling row as MAX31856_CHANNEL_COUNT separate
#: JSON keys per zone -- "coupling_c0".."coupling_c{N-1}" -- rather than an
#: array, and parse_zone_fields() expects the identical name back as the
#: z%u_ suffix (z%u_coupling_c%u). Matched by pattern in _encode_zone()
#: below rather than listed individually here, since N is a firmware
#: constant this module does not hardcode. Omitted-on-POST means PRESERVE
#: the current cell (same convention as fuzzy_strength_pct above); the
#: diagonal cell (j == the zone's own index) is force-ranged to [0,0] by
#: parse_zone_fields() (`(j == i) ? 0.0f : ZONE_COUPLING_COEFF_MAX`) and
#: MUST NEVER be posted nonzero -- _encode_zone() asserts this explicitly
#: rather than trusting the GET payload always has 0 there.
_ZONE_COUPLING_CELL_RE = re.compile(r"^coupling_c(\d+)$")
#: ZONES_CFG_VERSION 11->12 (2026-08-31): coupling_tau_c%u/
#: coupling_dead_time_c%u -- MOVED 2026-09-14 (docs/audits/
#: zones_diag_endpoint_split_2026-09-14.md) from GET /api/zones to GET
#: /api/zones_diag (zones_diag_get_handler(), get_zones_diag() above; same
#: orientation as coupling_c%u, [affected][stepped]), but parse_zone_fields() has NO
#: z%u_coupling_tau_c%u / z%u_coupling_dead_time_c%u wire field at all: it
#: unconditionally memcpy()s coupling_tau_s[]/coupling_dead_time_s[] from
#: current_z regardless of what a POST body contains (see zones_http.c's own
#: comment on that memcpy -- these two arrays are only ever written by
#: autotune_engine.c's finalize_fit() persist path, never through this POST
#: parser). Matched here and treated like _ZONE_READONLY_KEYS -- never
#: encoded into the POST body -- for two reasons: sending them would be
#: silently ignored on the wire (nothing to gain), and if this module instead
#: fell through to "unknown field" it would raise ZonesHttpUnknownFieldError
#: on every single zones round-trip against any board running this firmware,
#: since GET always emits these keys unconditionally.
_ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE = re.compile(r"^coupling_(?:tau|dead_time)_c\d+$")
#: Read-only telemetry zones_get_handler() emits per zone that has NO POST
#: counterpart at all (measured data, or the array index itself) -- excluded
#: from the POST body on purpose, not by omission-means-preserve, since these
#: are never accepted as input in the first place.
#: ZONES_CFG_VERSION 12->13 (2026-08-31): the tuning-quality record (set 1).
#: zones_http_handlers.c's GET handler emits all 11 keys unconditionally
#: (see its own comment ~line 347), but parse_zone_fields() has NO z%u_
#: POST key for ANY of them -- the POST side unconditionally copies every
#: one of these 11 fields from current_z (lines ~668-678 of
#: zones_http_handlers.c), invalidating tuning_valid only when the PID
#: gains actually changed (line ~681), never from a POST field. Genuinely
#: read-only/derived (autotune's fitted result), same class as
#: normal_current_a below -- listed here, not in _ZONE_FIELD_FORM_KEY,
#: so build_post_body() leaves them out of the POST rather than raising
#: ZonesHttpUnknownFieldError on every zones round-trip against this
#: firmware.
_ZONE_TUNING_READONLY_KEYS = {
    "tuning_valid", "tuning_method", "tuning_rule", "tuning_settled",
    "tuning_extrapolation_converged", "tuning_tau_consistent",
    "tuning_baseline_c", "tuning_step_ambient_c", "tuning_raw_rise_c",
    "tuning_rise_inf_c", "tuning_seq",
}
#: ZONES_CFG_VERSION 23->24 (2026-09-09): the operating point a zone's plant
#: model was fitted at. Same read-only class as the tuning_* record above,
#: but MOVED 2026-09-14 (docs/audits/zones_diag_endpoint_split_2026-09-14.md)
#: from GET /api/zones to GET /api/zones_diag -- zones_page.html never
#: rendered either key (grepped directly, zero hits), unlike tuning_*, which
#: renderTuningQuality() does render from the same /api/zones fetch and so
#: stayed put. Still listed here (and still excluded from POST bodies below)
#: because a caller that has merge_zones_diag()'d a zones_diag response back
#: onto its zones dict still must not try to repost these -- -273.15 is the
#: "no operating point recorded" sentinel (ZONE_MODEL_FIT_TEMP_UNKNOWN); read
#: it as None-equivalent, never as a real 273-below fit.
_ZONE_MODEL_FIT_READONLY_KEYS = {"model_fit_temp_c", "model_fit_ambient_c"}
#: ZONES_CFG_VERSION 25->26 (docs/audits/zones_get_autotune_baseline_exposure_
#: 2026-09-13.md): the K_dc adaptive_tune_refine_zone_locked() anchors its
#: plausibility-ratio test and blend target to. zones_http_get.c now emits
#: it unconditionally (previously write-only from the API's perspective),
#: but zones_http_post_parse.c has no z%u_ key for it -- adaptive_tune.c is
#: its only writer, and a whole-page POST must preserve whatever is stored.
#: 0 means "no baseline recorded yet" (same sentinel convention as
#: model_k_dc/hyst_c/coil_power_w) and is emitted raw. Same read-only class
#: as the tuning_*/model_fit_* records above.
_ZONE_AUTOTUNE_BASELINE_READONLY_KEYS = {"autotune_baseline_k_dc"}
#: GAP 2, docs/audits/observability_gaps_closed_2026-09-14.md: whether THIS
#: zone's fuzzy layer can actually run right now, independent of whether it
#: is configured to (fuzzy_strength_pct > 0) -- derived server-side
#: (pid_fuzzy_derive_bands()'s own model_valid predicate against this zone's
#: model_k_dc/model_tau_s, the same check the live control tick makes) and
#: emitted unconditionally, same always-emit convention as every field
#: around it. Read-only: there is no z%u_ POST key for it, and there never
#: should be -- it is not stored, it is computed fresh on every GET from
#: fields that ARE already round-tripped (model_k_dc/model_tau_s).
_ZONE_FUZZY_MODEL_VALID_READONLY_KEYS = {"fuzzy_model_valid"}
_ZONE_READONLY_KEYS = ({"index", "normal_current_measured", "normal_current_a"}
                       | _ZONE_TUNING_READONLY_KEYS | _ZONE_MODEL_FIT_READONLY_KEYS
                       | _ZONE_AUTOTUNE_BASELINE_READONLY_KEYS
                       | _ZONE_FUZZY_MODEL_VALID_READONLY_KEYS)

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
    "pc_link_abort_silence_ms": "pc_link_abort_silence_ms",
    # ease_off_window_mult was HERE (ZONES_CFG_VERSION 15->16) -- REMOVED at
    # 16->17 (2026-09-04): the field moved per-zone (see _ZONE_FIELD_FORM_KEY's
    # own entry). A GET no longer emits a top-level "ease_off_window_mult" at
    # all, so this module never sees one in `current`; see
    # _LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY below for the backward-compat path
    # that still lets an OLD preset using this now-removed top-level scalar
    # apply -- to every zone, not silently dropped.
}
_TOP_INT_FIELDS = {"thermo_count", "relay_count", "max_simultaneous_relays"}
#: Top-level keys GET emits that this module deliberately never echoes back:
#: relay_names/timing_profiles/zones are handled by their own dedicated
#: logic below (not this scalar map), and the rest are read-only telemetry
#: (safety_wiring, ct_warn_mask, relay_zone_owned_mask) with no POST form
#: field at all.
_TOP_READONLY_OR_STRUCTURAL_KEYS = {
    "relay_zone_owned_mask", "safety_wiring", "ct_warn_mask",
    # safety_tc_type (owner decision 2026-09-15, "the commissioning page owns
    # the type" -- Opus review F3): moved OUT of _TOP_FIELD_FORM_KEY here.
    # zones_http_post.c no longer reads an operator-submitted safety_tc_type
    # at all -- it always echoes the current live value regardless of what a
    # form submission carries (see that file's own comment) -- so this
    # module must not send it as a POST override either: doing so would
    # look like a working control that silently never reaches the Pico. GET
    # still reports it (read back from the Pico, see zones_http_get.c) and
    # this module still surfaces that read-only value the same way
    # safety_ceiling below does.
    "safety_tc_type",
    # safety_tc_type_known (2026-09-15, Opus re-review N5): whether the
    # safety_tc_type value above has actually been fetched from the Pico
    # this boot, as opposed to zones_http_get.c falling back to the ESP's
    # own possibly-stale zones.cfg.safety_tc_type before the first Pico
    # readback lands -- see that file's own comment. Read-only telemetry,
    # same as safety_tc_type itself: no POST field.
    "safety_tc_type_known",
    # safety_ceiling (owner request 2026-09-10): read-only telemetry --
    # {target_c, pico_known, pico_current_c} showing what the Pico's
    # abs_max_temp_c ceiling should be (from the zone maxima,
    # safety_ceiling_policy.h's formula) and what it last confirmed. There
    # is no POST field for it: the Pico's ceiling is written by the
    # firmware's own zones_post_handler (safety_ceiling_sync.c), triggered
    # by max_temp_c changes, never posted directly by a client.
    "safety_ceiling",
    "relay_names", "timing_profiles", "zones",
    # relay_types (docs/ZONE_GRAPHIC_PLAN.md stage 2, 2026-09-18): one small
    # integer per relay saying what that relay physically drives (damper,
    # outlet, valve, fan, light, other, or 0 = nobody has said yet), indexed
    # exactly like relay_names above. Structural, same as relay_names: this
    # module's scalar map never echoes it back. The POST side does accept it,
    # but per-relay, under a key built dynamically as "relay<N>_type" -- never
    # a top-level literal -- so it correctly does not appear in the POST
    # field table either.
    "relay_types",
    # docs/ON_OFF_ZONE.md step 6 (ZONES_CFG_VERSION 22->23, 2026-09-08):
    # the resolved on/off hysteresis/min-on-off-seconds DEFAULTS, emitted
    # top-level purely so zones_page.html's placeholder text can't drift
    # from the firmware default (see zones_http_get.c's own comment on the
    # APPEND() call that emits these two). Read-only telemetry -- there is
    # no top-level POST field for either; the per-zone hyst_c/min_on_s/
    # min_off_s fields (see _ZONE_FIELD_FORM_KEY) are what actually get
    # posted.
    "on_off_hyst_c_default", "on_off_min_on_off_s_default",
    # docs/audits/zones_field_sourcing_and_generation_2026-09-14.md (opus
    # review of the zones_diag split, defect 2): zones_config_generation()
    # (firmware, bumped by every config setter) is now emitted on BOTH
    # /api/zones and /api/zones_diag so merge_zones_diag() can detect a
    # config write landing between the two GETs instead of silently pairing
    # a new gain with an old fit operating point. Read-only telemetry, no
    # POST field.
    "generation",
}


def _format_scalar(key: str, value: Any, int_fields: "set[str]") -> str:
    if value is None:
        # A preset carrying a JSON null for a field otherwise headed for
        # int(value)/float(value) would raise a raw TypeError that escapes
        # every `except ZonesHttpError` handler this module's callers use --
        # refuse it the same loud, catchable way as every other malformed
        # value instead.
        raise ZonesHttpError(f"field {key!r} is null -- refusing to encode a missing value")
    if key in ("continue_on_zone_trip", "failsafe_state"):
        # zones_http_get.c emits failsafe_state as a JSON bool ("true"/
        # "false"), same as continue_on_zone_trip's top-level field; the
        # firmware's POST parser (zones_http_post_parse.c) expects
        # z%u_failsafe as "0"/"1" via zones_config_json_parse_u8_field(),
        # not "true"/"false".
        return "1" if value else "0"
    if key in int_fields:
        return str(int(value))
    if isinstance(value, bool):
        # No other bool-valued field is expected here; refuse rather than
        # silently coercing True/False to 1.0/0.0.
        raise ZonesHttpError(f"field {key!r} is a bool but has no known bool encoding")
    if isinstance(value, str):
        if value == "":
            # Firmware treats a blank as omit-preserve; refuse here so a preset never relies on it.
            raise ZonesHttpError(f"field {key!r} is an empty string -- refusing to send a blank value")
        return value
    return repr(float(value))


def _encode_zone(index: int, zone: dict) -> "dict[str, str]":
    fields: "dict[str, str]" = {}
    for key, value in zone.items():
        if key in _ZONE_READONLY_KEYS:
            continue
        if key == "settings_source_groups":
            # Item 4 (Opus review of 5672719): before this, this nested dict
            # had no entry in _ZONE_FIELD_FORM_KEY at all, so a whole-page
            # save either raised ZonesHttpUnknownFieldError outright or (an
            # earlier version of this module) silently dropped it -- either
            # way the five independent groups never round-tripped and a
            # save collapsed every zone back to whatever "settings_source"
            # (the LIMITS value) happened to be. Expand it into one
            # z{index}_settings_source_<group> key per _SRC_GROUP_NAMES
            # entry instead, matching parse_zone_fields()'s own per-group
            # wire format exactly.
            if not isinstance(value, dict):
                raise ZonesHttpUnknownFieldError(
                    f"zone {index}: settings_source_groups is {value!r}, not a dict -- "
                    "GET /api/zones payload shape has changed")
            for group_name in _SRC_GROUP_NAMES:
                if group_name not in value:
                    raise ZonesHttpUnknownFieldError(
                        f"zone {index}: settings_source_groups is missing group {group_name!r} -- "
                        "GET /api/zones payload shape has changed")
                fields[f"z{index}_settings_source_{group_name}"] = _format_scalar(
                    "settings_source", value[group_name], _ZONE_INT_FIELDS)
            continue
        if _ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE.match(key):
            continue
        cell_match = _ZONE_COUPLING_CELL_RE.match(key)
        if cell_match:
            j = int(cell_match.group(1))
            if j == index and float(value) != 0.0:
                # zones_http.c's parse_zone_fields() ranges the diagonal
                # cell to exactly [0,0] (`(j == i) ? 0.0f : ...`) -- a
                # nonzero value here means either this module's caller
                # corrupted the GET payload, or the firmware itself no
                # longer forces the diagonal to 0. Either way, posting it
                # would either be refused by the firmware or -- worse, if
                # the firmware's own guard ever regressed -- silently
                # accepted. Refuse rather than post it.
                raise ZonesHttpError(
                    f"zone {index}: coupling diagonal cell coupling_c{j} is {value!r}, "
                    "not 0 -- refusing to post a nonzero diagonal")
            fields[f"z{index}_{key}"] = repr(float(value))
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
    # 2026-08-29: heater_window_ms, for the same reason and now with a
    # constraint that TIES it to heater_min_on_ms -- the window must be at
    # least 3x the effective minimum on-time or no fractional duty can be
    # rendered in it at all (zones_http.h's ZONE_HEATER_WINDOW_MIN_MULTIPLE).
    # This bench carried a 2000 ms window against the 10 s minimum, which
    # made zone 0 a bang-bang output that reported itself as a PID one: an
    # autotune step at duty 0.4 commanded heat for 40 minutes and never
    # closed the relay. A preset that pins a bench's relay timing has to be
    # able to carry the window as well as the minimum, or the pair it is
    # trying to pin is only half pinned. Not in _REQUIRED_ZONE_FIELDS, same
    # reasoning as the two below it.
    "heater_window_ms",
    # 2026-08-29: sanity_rate_c_per_min, thermal_guard.c guard 1's minimum
    # rise rate (the dead-element check). Same reasoning as heater_min_on_ms
    # above: it is a per-bench physical property -- how fast THIS jig can
    # actually heat -- so a preset that pins a bench has to carry it, and
    # this bench's 5.0 C/min (a real kiln's figure) killed every firing at
    # t=62s. Also deliberately not in _REQUIRED_ZONE_FIELDS: a preset that
    # omits it still echoes the board's value back unchanged.
    "sanity_rate_c_per_min",
    # 2026-08-31: cross_zone_max_delta_c, thermal_guard.c guard 8's
    # cross-zone divergence limit. Found missing from this set on hardware:
    # zones 1 and 2 ran all night with guard 8 disabled because a preset
    # that named cross_zone_max_delta_c had the value silently dropped here
    # -- build_post_body() still echoed the BOARD's old value back (never
    # zeroed it, since _ZONE_FIELD_FORM_KEY's "xzone" mapping has always
    # existed), so the POST looked identical to "success" while the field
    # never actually changed. ZERO IS A MEANINGFUL VALUE for this field --
    # 0.0 means the guard is DISABLED, not "no limit given" -- so it must be
    # transmittable and must never be treated as falsy/absent by anything
    # downstream of this set (see _PRESET_ZONE_KNOWN_IGNORED_FIELDS below
    # for how a truly-omitted preset key is still distinguished from an
    # explicit 0).
    "cross_zone_max_delta_c",
    # 2026-08-31: name/thermo_mask/tc_type/ct_mask/timing_profile/
    # heater_min_off_ms/fuzzy_strength_pct and the whole guard_* family
    # below were round-trippable through _ZONE_FIELD_FORM_KEY and
    # firmware-accepted but NOT overridable by a preset -- the same silent-
    # drop shape as cross_zone_max_delta_c above, just never hit on
    # hardware yet. name/thermo_mask/tc_type/ct_mask are the same class as
    # relay_mask (already overridable): per-bench hardware assignment a
    # preset legitimately pins. heater_min_off_ms completes the
    # heater_min_on_ms/heater_window_ms timing trio above -- same
    # reasoning. fuzzy_strength_pct is a PID tuning knob, same class as
    # pid_kp/ki/kd. timing_profile selects which already-echoed named
    # profile (tp%u_*) a zone uses -- a preset naming it is picking a
    # profile, not writing profile contents (those stay verbatim-echoed,
    # see build_post_body()'s timing-profile loop).
    "name", "thermo_mask", "tc_type", "ct_mask", "timing_profile", "heater_min_off_ms",
    "fuzzy_strength_pct",
    # ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md section 3.2 follow-up,
    # 2026-09-02): coupling_diag_k_dc, the coupling identification's own
    # diagonal cell -- a scalar GET/POST field, same class as
    # fuzzy_strength_pct just above, not the whole-row coupling_coeff
    # handling below (this is a single measured DC gain per zone, not a
    # matrix row).
    "coupling_diag_k_dc",
    # guard_* family: thermal_guard.c's per-zone guard thresholds
    # (guard 1's dead-element rate lives in sanity_rate_c_per_min above;
    # these are guards 2-7ish -- wrong-direction, off-settle, runaway,
    # drift, sensor-fault debounce, frozen-window). SAME INCIDENT CLASS as
    # cross_zone_max_delta_c/guard 8: a preset naming one of these had it
    # silently dropped, which on a bench with elements connected is a
    # disabled safety guard exactly like the guard-8 overnight incident.
    "guard_wrong_dir_window_s", "guard_wrong_dir_rate_c_per_min", "guard_off_settle_s",
    "guard_runaway_rate_c_per_min", "guard_runaway_margin_c", "guard_drift_period_s",
    "guard_sensor_fault_debounce_ticks", "guard_frozen_window_s",
    # ZONES_CFG_VERSION 16->17 (PID_EXPANSION_PLAN.md sec 3.6d, 2026-09-04):
    # ease_off_window_mult, now per-zone -- same scalar-override class as
    # coupling_diag_k_dc above. A preset zone entry naming this sets THAT
    # zone's own A/B arm (e.g. a z0-only 3.5x override); see
    # _apply_legacy_top_level_ease_off_mult() below for the separate,
    # backward-compat path that lets an OLD preset's now-removed top-level
    # "ease_off_window_mult" scalar still apply too (to every zone that does
    # not name its own override here).
    "ease_off_window_mult",
    # ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d, 2026-09-04):
    # approach_rate_cap_c_per_hr, same scalar-override class as
    # ease_off_window_mult just above -- a preset zone entry naming this
    # sets THAT zone's own rate cap for an A/B arm (e.g. a z0-only cap while
    # z1/z2 stay uncapped at 0). No legacy top-level equivalent exists for
    # this field (unlike ease_off_window_mult's now-removed board-wide
    # scalar) -- it was per-zone from the day it was introduced, so there is
    # no _apply_legacy_top_level_*() counterpart needed here.
    "approach_rate_cap_c_per_hr",
    # ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g, 2026-09-04):
    # error_band_c/rate_band_c_per_s, same scalar-override class as
    # ease_off_window_mult/approach_rate_cap_c_per_hr just above -- a preset
    # zone entry naming either sets THAT zone's own fuzzy-PID membership
    # band for an A/B arm (e.g. a z0-only rescale to the measured envelope
    # while z1/z2 stay at the 20.0/0.5 firmware default). No legacy
    # top-level equivalent exists for either field -- both were per-zone
    # from the day they were introduced, so no _apply_legacy_top_level_*()
    # counterpart is needed here, same as approach_rate_cap_c_per_hr's own
    # note just above.
    "error_band_c", "rate_band_c_per_s",
    # ZONES_CFG_VERSION 22->23 (docs/ON_OFF_ZONE.md step 6, 2026-09-08):
    # zone_type/failsafe_state/hyst_c/min_on_s/min_off_s. Found missing from
    # this set by bench_test's HP-03 (an on/off-zone preset naming zone_type
    # raised ZonesHttpUnknownFieldError) -- the same silent-drop-turned-
    # explicit-raise class as cross_zone_max_delta_c/relay_type above, just
    # never hit on hardware until a preset actually tried to reconfigure a
    # zone as on/off. All five are legitimate per-bench overrides a preset
    # picking an on/off zone must be able to carry (same class as
    # relay_mask/control_mode above): zone_type picks heater-vs-on/off,
    # failsafe_state/hyst_c/min_on_s/min_off_s are that on/off zone's own
    # hysteresis/timing, none of which a preset should be forced to leave at
    # whatever the board happened to have.
    "zone_type", "failsafe_state", "hyst_c", "min_on_s", "min_off_s",
}

#: coupling_coeff is handled OUTSIDE _PRESET_ZONE_OVERRIDE_FIELDS on purpose:
#: every other entry in that set is a single scalar GET/POST field
#: (zone[key] -> z%u_<suffix>), but coupling_coeff is a whole ROW of the
#: [affected][stepped] coupling matrix -- zone i's list entry j is "how much
#: zone i (the AFFECTED zone) rises per the firmware's units when zone j (the
#: STEPPED zone) is driven", exactly zones_http_handlers.c's
#: `"coupling_c%u":%.9g` per-zone-i emission (2026-09-28; was %.4f, line ~332) and the matching
#: z%u_coupling_c%u POST field (line ~956, parse_zone_fields()). A preset
#: names it "coupling_coeff": [c0, c1, ..., c{N-1}] -- SAME length and
#: ORDERING as GET /api/zones' own coupling_c0..c{N-1} keys for that zone, not
#: a flat scalar -- so it is expanded into the per-cell coupling_c%u keys of
#: `merged` below rather than assigned like a scalar field. The diagonal
#: cell (j == the zone's own index) is SKIPPED here rather than copied
#: through: zones_http.c/parse_zone_fields() force-ranges it to exactly
#: [0,0] regardless of what is posted, and _encode_zone() below asserts a
#: nonzero diagonal cell reaching the merged dict is a bug -- so this
#: expansion must never place a value there, even if the preset's row (as
#: this matrix's rows do) already carries 0 in that slot.
_PRESET_ZONE_COUPLING_FIELD = "coupling_coeff"

#: Preset zone keys that are legitimately NOT overlaid by build_post_body()
#: -- "index" locates the target zone rather than being posted as a field.
#:
#: k_dc/tau_s/dead_time_s (the UART/preset spelling) AND
#: model_k_dc/model_tau_s/model_dead_time_s (the GET /api/zones /
#: backup-export spelling of the SAME three fields -- zones_get_handler()
#: emits them as "model_k_dc" etc, see _ZONE_FIELD_FORM_KEY's "k"/"tau"/
#: "deadtime" mapping) are BOTH listed here on purpose: config_presets.py's
#: apply_preset() (lines 341-344) already writes these three over the UART
#: CONTROL link before ever calling apply_zone_preset(), so they are not
#: lost -- do not "fix" this by moving them to _PRESET_ZONE_OVERRIDE_FIELDS.
#: A preset hand-authored against the UART-facing schema uses k_dc/tau_s/
#: dead_time_s; a preset built by copying a board GET or a backup export
#: (the most natural way to author one) carries model_k_dc/model_tau_s/
#: model_dead_time_s instead -- both spellings must be tolerated or the
#: latter raises ZonesHttpUnknownFieldError on the operator's own preset.
#:
#: settings_source is a zone-to-zone settings-inheritance pointer (0xFF ==
#: custom, or another zone's index) rather than a tunable value -- letting a
#: preset set it would silently redirect one zone onto another zone's LIVE
#: config instead of the values the preset itself names, which is a worse
#: surprise than the field being merely absent. Deliberately excluded from
#: _PRESET_ZONE_OVERRIDE_FIELDS; a preset that needs to change it should go
#: through a tool that makes that redirection explicit, not this overlay.
#:
#: Any OTHER preset zone key is not "known to be out of scope" -- it is a
#: field this module has never heard of, and the old behavior (silently
#: falling through the `if key in _PRESET_ZONE_OVERRIDE_FIELDS` check) is
#: exactly the "consumer without producer" bug this set exists to catch:
#: build_post_body() now raises ZonesHttpUnknownFieldError for anything that
#: lands in neither set, instead of quietly discarding it. See
#: build_post_body()'s zone-merge loop.
#: normal_current_measured/normal_current_a (Task 1, 2026-08-27+2) and the
#: eleven tuning_* fields (ZONES_CFG_VERSION 12->13, the tuning-quality
#: record) are read-only telemetry on THIS endpoint -- zones_http_handlers.c
#: always emits them (same always-emit convention as model_k_dc/etc, see
#: that handler's own comments) but parse_zone_fields() has no POST wire key
#: for any of them; the encode side already treats them as read-only via
#: _ZONE_READONLY_KEYS/_ZONE_TUNING_READONLY_KEYS. A preset built by copying
#: a live GET or a backup export -- the module docstring's own "most natural
#: way to author one" -- carries these keys too, and until now they landed
#: in neither preset set, so that natural authoring path tripped exactly the
#: cross_zone_max_delta_c incident's ZonesHttpUnknownFieldError class the
#: moment a captured preset was re-applied. Reference-only: ignored, never
#: overlaid, same as model_k_dc above.
_PRESET_ZONE_KNOWN_IGNORED_FIELDS = {
    "index",
    "k_dc", "tau_s", "dead_time_s",
    "model_k_dc", "model_tau_s", "model_dead_time_s",
    "settings_source", "settings_source_groups",
    "normal_current_measured", "normal_current_a",
    "tuning_valid", "tuning_method", "tuning_rule", "tuning_settled",
    "tuning_extrapolation_converged", "tuning_tau_consistent",
    "tuning_baseline_c", "tuning_step_ambient_c", "tuning_raw_rise_c",
    "tuning_rise_inf_c", "tuning_seq",
    "model_fit_temp_c", "model_fit_ambient_c",
}


#: ZONES_CFG_VERSION 16->17 backward compatibility (PID_EXPANSION_PLAN.md
#: sec 3.6d): the top-level "ease_off_window_mult" key config_presets.json
#: presets carried before this pass (e.g. easeoff_ab_2p0_20260903.json/
#: easeoff_ab_3p0_20260903.json) named a board-wide scalar that no longer has
#: anywhere to land -- GET /api/zones stopped emitting it, and the firmware's
#: POST handler stopped reading it as a top-level field the same day this
#: field moved onto zone_cfg_t. Without this function, an old preset still
#: naming it would simply have the key ignored (it is not in
#: _TOP_FIELD_FORM_KEY any more) -- exactly the "preset value never reaches
#: the wire" failure mode this module's own build_post_body() docstring
#: warns about for an unmapped field, except silent instead of a raised
#: ZonesHttpUnknownFieldError, because the key is still a perfectly
#: recognized (just legacy) preset field name.
LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY = "ease_off_window_mult"


def _apply_legacy_top_level_ease_off_mult(current: dict, preset: dict) -> dict:
    """Returns a shallow-ish copy of `preset` with a legacy top-level
    "ease_off_window_mult" scalar (if present) expanded into a per-zone
    "ease_off_window_mult" override on EVERY zone `current` reports (not
    just the zones `preset` already happens to mention -- a preset that
    names this legacy scalar with an empty/partial "zones" list, the most
    common real shape: a pure "just change the board-wide ease-off arm"
    preset, must still reach every zone) that does not already carry its
    own per-zone value -- i.e. the old "one number for every zone" board-
    wide behaviour, reproduced through the new per-zone mechanism, so an
    old preset still applies with EXACTLY its old effect. A zone that
    already names its own "ease_off_window_mult" (the new per-zone preset
    form) is left alone -- the more specific, explicitly-authored override
    wins over the legacy board-wide one, never the other way around.

    `current` is only consulted for its zone INDEX set (current["zones"][*]
    ["index"]) -- never for any other field -- so this needs no more than a
    fresh GET /api/zones response, the same object every caller already has
    in hand at this point in the GET-merge-POST cycle.

    A no-op (returns `preset` unchanged) when the legacy key is absent --
    the overwhelmingly common case for every preset written after this
    pass.
    """
    if LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY not in preset:
        return preset
    legacy_value = preset[LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY]
    new_preset = dict(preset)
    del new_preset[LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY]
    zones_by_index = {z["index"]: dict(z) for z in preset.get("zones", [])}
    for cz in current.get("zones", []):
        idx = cz["index"]
        zone = zones_by_index.setdefault(idx, {"index": idx})
        if LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY not in zone:
            zone[LEGACY_TOP_LEVEL_EASE_OFF_MULT_KEY] = legacy_value
    new_preset["zones"] = list(zones_by_index.values())
    return new_preset


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
    preset = _apply_legacy_top_level_ease_off_mult(current, preset)
    fields: "dict[str, str]" = {}

    # ---- top-level scalars: echo current, let the preset override ANY
    # top-level field it names (not just thermo_count/relay_count -- that
    # was the bug: a preset naming e.g. ease_off_window_mult validated,
    # had a _TOP_FIELD_FORM_KEY mapping, and was still silently dropped
    # here because top_overrides was hand-built for exactly two keys, so
    # the GET-echoed current value went out unchanged and the preset value
    # never reached the wire). Any key in _TOP_FIELD_FORM_KEY that the
    # preset names is an override candidate; unknown preset keys are left
    # alone here and handled by the "unknown field" checks below/elsewhere. ----
    top_overrides: "dict[str, Any]" = {
        key: preset[key] for key in _TOP_FIELD_FORM_KEY if key in preset
    }

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
                if key == _PRESET_ZONE_COUPLING_FIELD:
                    if not isinstance(value, list):
                        raise ZonesHttpError(
                            f"zone {idx}: preset field {_PRESET_ZONE_COUPLING_FIELD!r} must be a "
                            f"list of per-cell values (coupling_c0..c{{N-1}}), got {value!r}")
                    for j, cell in enumerate(value):
                        if j == idx:
                            # Diagonal stays whatever `current` already has
                            # (0, forced by the firmware) -- never overlaid,
                            # see this field's docstring above.
                            continue
                        merged[f"coupling_c{j}"] = cell
                elif key in _PRESET_ZONE_OVERRIDE_FIELDS:
                    merged[key] = value
                elif key in _PRESET_ZONE_KNOWN_IGNORED_FIELDS:
                    continue
                elif (_ZONE_COUPLING_CELL_RE.match(key)
                      or _ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE.match(key)):
                    # A preset built by copying a live GET or a backup export
                    # (the module docstring's "most natural way to author
                    # one") carries the individual coupling_c%u/
                    # coupling_tau_c%u/coupling_dead_time_c%u keys GET emits,
                    # not the coupling_coeff list a hand-authored preset uses.
                    # The row-level override path is coupling_coeff (handled
                    # above) -- coupling_tau_c%u/dead_time_c%u have NO POST
                    # path at all (see _ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE's
                    # own comment), so any of these three key shapes reaching
                    # here are reference-only: ignored, never overlaid.
                    continue
                else:
                    raise ZonesHttpUnknownFieldError(
                        f"zone {idx}: preset field {key!r} is not in "
                        "_PRESET_ZONE_OVERRIDE_FIELDS or _PRESET_ZONE_KNOWN_IGNORED_FIELDS "
                        "-- refusing to silently drop it from the POST (see this module's "
                        "docstring and the cross_zone_max_delta_c incident noted on "
                        "_PRESET_ZONE_OVERRIDE_FIELDS)")
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
    for key in _TOP_FIELD_FORM_KEY:
        if key not in preset:
            continue
        expected = preset[key]
        actual = after.get(key)
        if isinstance(expected, float):
            ok = actual is not None and _float_close(expected, actual)
        else:
            ok = actual == expected
        if not ok:
            mismatches.append(f"{key}: expected {expected!r}, board reports {actual!r}")

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


#: POST keys parse_zone_fields() (zones_http_post_parse.c) treats as
#: omit-PRESERVES-current (`field_present ? parse : current_z->...`): the
#: measured plant model, the coupling cells and diagonal gain, the fuzzy/
#: easeoff/approach/band floats, hystc and coilpower. Re-posting them at
#: GET's coarser print (%.1f/%.2f/%.3f/%.4f) would round them; leaving them
#: out of the body makes the firmware keep them bit-exact. Nothing else is
#: strippable: a required field left out would 400 or zero.
ZONE_OMIT_PRESERVED_KEY_RE = re.compile(
    r"^z\d+_(?:k|tau|deadtime|coupling_diag_k_dc|coupling_c\d+|fuzzy_strength|easeoffmult"
    r"|approachratecap|errorband|rateband|progressband|hystc|coilpower)$")


def strip_omit_preserved(body: str, keep_keys: "frozenset[str] | set[str]" = frozenset()) -> str:
    """Drop every ZONE_OMIT_PRESERVED_KEY_RE key from a build_post_body()
    form body except those in `keep_keys` (fields the caller deliberately
    writes)."""
    pairs = urllib.parse.parse_qsl(body, keep_blank_values=True)
    kept = [(k, v) for k, v in pairs if k in keep_keys or not ZONE_OMIT_PRESERVED_KEY_RE.match(k)]
    return urllib.parse.urlencode(kept)


def _preset_named_omit_preserved_keys(preset: dict) -> "set[str]":
    """Omit-preserved POST keys a preset explicitly names (any scalar field
    in _ZONE_FIELD_FORM_KEY, coupling rows/cells), so apply_zone_preset()'s
    strip keeps what the caller asked to change and drops only what it
    merely echoed from GET."""
    keep = set()
    for z in preset.get("zones", []):
        i = z.get("index")
        if i is None:
            continue
        for key in z:
            # Keep a key only when build_post_body() really takes its VALUE
            # from the preset (_PRESET_ZONE_OVERRIDE_FIELDS). The GET/backup
            # spellings (model_k_dc, coupling_cN, ...) are reference-only
            # there: build_post_body() echoes GET's rounded print for them,
            # so keeping them would re-post rounded values (review LOW-6).
            if key not in _PRESET_ZONE_OVERRIDE_FIELDS:
                continue
            suffix = _ZONE_FIELD_FORM_KEY.get(key)
            if suffix is not None:
                keep.add(f"z{i}_{suffix}")
        if _PRESET_ZONE_COUPLING_FIELD in z:
            for j in range(len(z[_PRESET_ZONE_COUPLING_FIELD])):
                if j != i:  # the diagonal is never overlaid
                    keep.add(f"z{i}_coupling_c{j}")
    return keep


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
    # Expand a legacy top-level "ease_off_window_mult" scalar (a preset
    # written before ZONES_CFG_VERSION 16->17) into per-zone overrides ONCE,
    # here -- build_post_body() does its own equivalent expansion internally
    # on ITS OWN copy of `preset` (a no-op the second time, since the legacy
    # key is already gone), but _verify_against_preset() below needs to see
    # the SAME expanded, per-zone-keyed preset, or the read-back
    # verification this function exists to provide would simply never look
    # at the field a legacy preset asked to change (it is not in
    # _TOP_FIELD_FORM_KEY any more) and silently report ok=True regardless
    # of whether the value actually landed.
    preset = _apply_legacy_top_level_ease_off_mult(current, preset)
    body = strip_omit_preserved(build_post_body(current, preset), _preset_named_omit_preserved_keys(preset))
    response_text = post_zones(host, body, timeout)
    if response_text.strip() != "ok":
        raise ZonesHttpError(f"POST /api/zones returned 200 but body was {response_text!r}, not 'ok'")

    if not verify:
        return ZonesApplyResult(ok=True, post_response=response_text)

    after = get_zones(host, timeout)
    mismatches = _verify_against_preset(after, preset)
    return ZonesApplyResult(ok=not mismatches, mismatches=mismatches, post_response=response_text)
