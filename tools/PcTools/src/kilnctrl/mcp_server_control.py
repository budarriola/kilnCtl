"""CONTROL tools (task 8).

Part of the mcp_server.py split (pure refactor) -- moved verbatim, no
logic changes. See mcp_server.py's module docstring for the overall map.
"""
from __future__ import annotations

import asyncio
import copy
import dataclasses
import functools
import glob
import json
import math
import logging
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
from collections import deque
from pathlib import Path
from typing import Any, Callable, Optional

from . import actions, config_presets, debug_probe, devices, mcp_facade, openocd_util, pico_gpio_probe, safety_cfg_http_client, settings, stale_check, ui_test_runner, wifi_credentials, zones_http_client
from .autotune import AutotuneClient, AutotuneQueryError
from .control import ControlClient, ControlQueryError
from .device_log import LogClient
from .devices import LogLine
from .display import BlitError, DisplayClient, DisplayQueryError
from .touch import TouchClient, TouchQueryError
from .ui_test_client import UiTestClient, UiTestQueryError
from .web_ui_client import WebUiClient
from .info import InfoClient, InfoQueryError
from .system import SystemClient, SystemQueryError
from .io_expander import IoClient, IoQueryError
from .link_hub import get_shared_link
from .profiles import ProfilesClient, ProfilesQueryError
from .protocol import (
    FACTORY_RESET_SCOPE_KILN,
    PROFILES_SAVE_ID_NEW,
    profile_id_is_builtin,
    THERMO_CHANNEL_ALL,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    WIFI_MODE_AP,
    WIFI_MODE_HOME,
    Device,
    Frame,
    FrameError,
    LogLevel,
    MsgType,
    unstuff,
)
from . import ota_http_client as ota_http
from . import probe
from .probe import ProbeClient, ProbeQueryError
from .wifi_uart import WifiUartClient, WifiUartQueryError
from .safety import SafetyClient, SafetyQueryError
from .serial_link import list_ports, recommend_port
from .session_log import SessionLogger
from .thermo import ThermoClient, ThermoQueryError

from . import mcp_server_core as _core


# ---------------------------------------------------------------------------
# CONTROL (task 8) -- zone PID/model config, mirrors zones_http.c
#
# Manual relay control is NOT here -- see io_set_relay/io_set_relay_mask.
# ---------------------------------------------------------------------------
def _control_resolve_host(host: Optional[str]) -> str:
    """Same discovery convention as ota_http.py's ``_ota_resolve_host()``:
    an explicit `host` always wins; otherwise prefer the board's current
    Wi-Fi station IP (read over the UART link, which works even with Wi-Fi
    down), falling back to the fixed fallback-AP address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return ota_http.OTA_AP_DEFAULT_HOST


_REPO_ROOT = Path(__file__).resolve().parents[4]
_COUPLING_SOLVE_C_PATH = (
    _REPO_ROOT
    / "firmware"
    / "KilnFW"
    / "App"
    / "drivers"
    / "control"
    / "zone_coupling_solve.c"
)
#: Matches the body of `zone_coupling_use_measured_diag_k_dc()` well enough to
#: pull out its `return true;`/`return false;` -- tolerant of whitespace so a
#: reformat doesn't silently stop matching (in which case
#: `_read_coupling_use_measured_diag_k_dc_compiled_value()` returns None and
#: callers fall back to "unknown", never to a stale guess).
_COUPLING_USE_MEASURED_DIAG_RE = re.compile(
    r"zone_coupling_use_measured_diag_k_dc\s*\(\s*void\s*\)\s*\{\s*return\s+(true|false)\s*;",
    re.DOTALL,
)


def _read_coupling_use_measured_diag_k_dc_compiled_value() -> Optional[bool]:
    """Derive the CURRENT compiled value of
    `zone_coupling_use_measured_diag_k_dc()` by reading it straight out of
    `zone_coupling_solve.c` at call time, rather than hardcoding a copy of a
    value that lives in firmware source and has already gone stale here once
    (see docs/audits/coupling_measured_diag_flag_audit_2026-09-11.md). This
    reads whatever source tree this PC tool checkout has on disk -- not
    necessarily what a given board was actually flashed with -- so the
    caller should describe it as "compiled in this source tree", not "on the
    board". Returns None if the file is missing or the function's shape no
    longer matches (never guesses)."""
    try:
        text = _COUPLING_SOLVE_C_PATH.read_text(encoding="utf-8")
    except OSError:
        return None
    m = _COUPLING_USE_MEASURED_DIAG_RE.search(text)
    if not m:
        return None
    return m.group(1) == "true"


def _describe_coupling_matrix(zones_json: dict) -> str:
    """Render the coupling matrix from a GET /api/zones JSON body.

    Orientation is c[i][j] = how much zone i's (the AFFECTED zone's)
    temperature moves per unit of zone j's (the STEPPED zone's) actuation --
    zones_http.c emits it that way, and it is the same orientation
    coupling_workflow.py/coupled_ident.py assume. The diagonal is always 0
    (the firmware force-ranges the diagonal cell to exactly 0 -- see
    zones_http_client.py's _ZONE_COUPLING_CELL_RE comment). This orientation
    is easy to get backwards -- test_zones_http_client.py carries
    test_TRANSPOSED_mapping_is_caught_by_this_test because it happened once
    already -- so the row/col meaning is spelled out here rather than left
    implicit."""
    zones = zones_json.get("zones", [])
    n = len(zones)
    lines = [
        "coupling matrix (row i = AFFECTED zone, column j = STEPPED zone; "
        "c[i][j] = how much zone i's temperature moves per unit of zone j's "
        "actuation; diagonal is always 0):"
    ]
    for i, z in enumerate(zones):
        cells = [z.get(f"coupling_c{j}") for j in range(n)]
        cells_str = ", ".join(
            "0" if c == 0 else (f"{c:.2f}" if isinstance(c, (int, float)) else "?")
            for c in cells
        )
        lines.append(f"  z{i}: [{cells_str}]")
    use_measured_diag = _read_coupling_use_measured_diag_k_dc_compiled_value()
    if use_measured_diag is None:
        flag_desc = (
            "zone_coupling_use_measured_diag_k_dc() compiled value unknown -- "
            f"could not read/parse {_COUPLING_SOLVE_C_PATH}"
        )
    else:
        flag_desc = (
            "zone_coupling_use_measured_diag_k_dc() is compiled "
            f"{'true' if use_measured_diag else 'false'} in this source tree "
            "(read live from zone_coupling_solve.c, not hardcoded here)"
        )
    diag_bits = []
    for i, z in enumerate(zones):
        k_dc = z.get("coupling_diag_k_dc")
        if k_dc == 0.0:
            diag_bits.append(f"z{i}=0.0 (never identified on hardware)")
        elif use_measured_diag:
            diag_bits.append(
                f"z{i}={k_dc:.4f} (measured; used as the coupled matrix's "
                f"diagonal when {flag_desc} and provenance passes)"
            )
        elif use_measured_diag is False:
            diag_bits.append(
                f"z{i}={k_dc:.4f} (measured, but {flag_desc} -- "
                "not currently used even though present)"
            )
        else:
            diag_bits.append(f"z{i}={k_dc:.4f} (measured; usage gated by {flag_desc})")
    lines.append("coupling_diag_k_dc: " + "  ".join(diag_bits))
    return "\n".join(lines)


#: Per-zone fields that, like the coupling matrix above, only come back over
#: GET /api/zones (not the UART CONTROL wire) -- these three are the fields
#: a 2026-09-04 bench snapshot claimed were unreadable from the live board at
#: all. They ARE in the board's raw HTTP response (confirmed against a live
#: board that day); the gap was that this tool fetched zones_json for the
#: coupling matrix only and silently dropped everything else in it. Adding a
#: field here fixes a readback verification gap the same way missing
#: control_mode did (see project_safety_calls_logging_unchecked_success-style
#: incidents): a tool that omits fields makes a campaign look verified when
#: it wasn't actually checked.
HTTP_ONLY_ZONE_FIELDS = (
    "zone_type",
    "fuzzy_strength_pct",
    "ease_off_window_mult",
    "approach_rate_cap_c_per_hr",
)


def _describe_http_only_zone_fields(zones_json: dict) -> str:
    """Render the HTTP_ONLY_ZONE_FIELDS for every zone in a GET /api/zones
    JSON body, one line per zone, so a readback through this tool is
    complete rather than silently partial."""
    zones = zones_json.get("zones", [])
    lines = ["http-only fields (not on the UART CONTROL wire):"]
    for i, z in enumerate(zones):
        parts = [f"{name}={z.get(name)!r}" for name in HTTP_ONLY_ZONE_FIELDS]
        lines.append(f"  z{i}: " + ", ".join(parts))
    return "\n".join(lines)


#: Sentinel for zone_cfg_t::model_fit_temp_c/model_fit_ambient_c
#: (firmware/KilnFW/App/drivers/persist/zones_config_accessors.h,
#: ZONE_MODEL_FIT_TEMP_UNKNOWN, ZONES_CFG_VERSION 23->24): -273.15 (absolute
#: zero) is physically unreachable on a kiln, so it is used as "never
#: recorded" rather than a real temperature. Read live off that header at
#: call time (same discipline as
#: _read_coupling_use_measured_diag_k_dc_compiled_value() above) so this
#: never drifts from the firmware's own definition; falls back to the known
#: literal only if the header can't be read/parsed, and says so.
_ZONE_MODEL_FIT_TEMP_UNKNOWN_HEADER_PATH = (
    _REPO_ROOT
    / "firmware"
    / "KilnFW"
    / "App"
    / "drivers"
    / "persist"
    / "zones_config_accessors.h"
)
_ZONE_MODEL_FIT_TEMP_UNKNOWN_RE = re.compile(
    r"#define\s+ZONE_MODEL_FIT_TEMP_UNKNOWN\s+\(?\s*(-?[0-9.]+)f?\s*\)?"
)


def _read_zone_model_fit_temp_unknown_sentinel() -> float:
    """Return the live ZONE_MODEL_FIT_TEMP_UNKNOWN sentinel value, read out
    of firmware source rather than hardcoded here. Falls back to the known
    literal (-273.15) only if the header is missing or its shape no longer
    matches -- this fallback is a last resort, not a silent substitute for
    reading the real thing, so a caller comparing against it should not
    assume the firmware still agrees if this ever falls back."""
    try:
        text = _ZONE_MODEL_FIT_TEMP_UNKNOWN_HEADER_PATH.read_text(encoding="utf-8")
        m = _ZONE_MODEL_FIT_TEMP_UNKNOWN_RE.search(text)
        if m:
            return float(m.group(1))
    except OSError:
        pass
    return -273.15


def _describe_model_fields(zones_json: dict, diag_error: Optional[str] = None) -> str:
    """Render the identified-plant-model fields from a GET /api/zones JSON
    body: model_k_dc/model_tau_s/model_dead_time_s, tuning_valid,
    model_fit_temp_c/model_fit_ambient_c, and autotune_baseline_k_dc.

    None of these are on the UART CONTROL wire (control_get_zones()'s
    ZoneConfig has no model fields at all) -- this tool used to advertise
    "PID/model config" in its docstring while actually never rendering the
    model half of that claim (docs/audits/mcp_zone_model_fields_2026-09-13.md).

    Every sentinel here is rendered as a labeled sentinel, never as a plain
    number: model_fit_temp_c/model_fit_ambient_c's -273.15
    (ZONE_MODEL_FIT_TEMP_UNKNOWN, read live from firmware source -- see
    _read_zone_model_fit_temp_unknown_sentinel()) prints as "UNKNOWN (never
    recorded)", and model_k_dc/model_tau_s/model_dead_time_s's 0.0 "no
    model" sentinel prints as "no model identified" instead of "0.0000C".
    autotune_baseline_k_dc's presence is derived from the actual response
    passed in, per zone, rather than asserted from a hardcoded claim about
    firmware source that would go stale the same way the coupling-diag flag
    string did (docs/audits/coupling_measured_diag_flag_audit_2026-09-11.md)
    and the way THIS field's own docstring text did (it used to say "NOT
    exposed by GET /api/zones as of 2026-09-13" -- true when written, false
    since zones_http_get.c started emitting it in 0dbd7c6d; see
    docs/audits/stale_mcp_server_window_recheck_2026-09-14.md). If the key is
    absent from a zone's JSON object this prints "not present in this
    response"; if present, its live value is rendered like the other model
    fields (0.0 is a legal, common value here -- not a sentinel -- so it is
    printed as a plain number, not specially flagged).

    `diag_error`, when set, is the reason the caller's SECOND request (GET
    /api/zones_diag, which owns model_fit_temp_c/model_fit_ambient_c since
    docs/audits/zones_diag_endpoint_split_2026-09-14.md) failed. It is
    rendered explicitly, because "the board does not emit this field" and
    "the second fetch failed" must NOT read identically -- the first is a
    fact about the firmware, the second is a fact about this tool call, and
    an engineer reading "missing" would otherwise have no way to tell which
    happened. Adversarial review 2026-09-14 (see that audit doc's Review
    section): before this parameter existed, pointing the client at a host
    that 404s /api/zones_diag rendered exactly `fit_at=missing
    (ambient=missing)`, indistinguishable from a genuine firmware gap."""
    unknown_temp = _read_zone_model_fit_temp_unknown_sentinel()
    zones = zones_json.get("zones", [])
    lines = ["plant model (identified via autotune; feeds feedforward/fuzzy bands):"]
    for i, z in enumerate(zones):
        k_dc = z.get("model_k_dc")
        tau_s = z.get("model_tau_s")
        dead_time_s = z.get("model_dead_time_s")
        tuning_valid = z.get("tuning_valid")
        fit_temp_c = z.get("model_fit_temp_c")
        fit_ambient_c = z.get("model_fit_ambient_c")

        if k_dc is None or tau_s is None or dead_time_s is None:
            model_desc = "unavailable (field(s) missing from GET /api/zones response)"
        elif k_dc == 0.0 and tau_s == 0.0 and dead_time_s == 0.0:
            model_desc = "no model identified (all-zero sentinel)"
        else:
            model_desc = (
                f"K_dc={k_dc:.4f} C/duty  tau={tau_s:.1f}s  "
                f"dead_time={dead_time_s:.1f}s"
            )

        if isinstance(fit_temp_c, (int, float)) and fit_temp_c == unknown_temp:
            fit_temp_desc = "UNKNOWN (never recorded)"
        elif isinstance(fit_temp_c, (int, float)):
            fit_temp_desc = f"{fit_temp_c:.2f}C"
        else:
            fit_temp_desc = "missing" if diag_error is None else "UNAVAILABLE(diag-fetch-failed)"

        if isinstance(fit_ambient_c, (int, float)) and fit_ambient_c == unknown_temp:
            fit_ambient_desc = "UNKNOWN (never recorded)"
        elif isinstance(fit_ambient_c, (int, float)):
            fit_ambient_desc = f"{fit_ambient_c:.2f}C"
        else:
            fit_ambient_desc = "missing" if diag_error is None else "UNAVAILABLE(diag-fetch-failed)"

        valid_desc = (
            "missing" if tuning_valid is None else ("yes" if tuning_valid else "no")
        )

        if "autotune_baseline_k_dc" not in z:
            baseline_desc = "not present in this response"
        else:
            baseline_k_dc = z.get("autotune_baseline_k_dc")
            baseline_desc = (
                f"{baseline_k_dc:.4f}"
                if isinstance(baseline_k_dc, (int, float))
                else f"{baseline_k_dc!r} (unexpected type)"
            )

        lines.append(
            f"  z{i}: {model_desc}  fit_at={fit_temp_desc} (ambient={fit_ambient_desc})  "
            f"tuning_valid={valid_desc}  autotune_baseline_k_dc={baseline_desc}"
        )
    if diag_error is not None:
        lines.append(
            "  model_fit_temp_c/model_fit_ambient_c: NOT READ this call -- the "
            f"second request (GET /api/zones_diag) failed: {diag_error}. The "
            "values above are unknown, NOT confirmed absent; retry before "
            "drawing any conclusion about this board's recorded fit operating "
            "points (docs/audits/zones_diag_endpoint_split_2026-09-14.md)."
        )
    return "\n".join(lines)


@_core._tool()
def control_get_zones(host: Optional[str] = None) -> str:
    """Read every zone's current PID config, calibration offset and
    temperature limits, plus the thermocouple and relay counts, over the
    UART CONTROL wire.

    Also fetches, over HTTP GET /api/zones (none of these are on the UART
    CONTROL wire): the identified plant model (model_k_dc/model_tau_s/
    model_dead_time_s, tuning_valid -- see _describe_model_fields()'s
    docstring for why this section was added 2026-09-13 and how its
    sentinels are rendered), the per-zone coupling matrix (coupling_c0.., a
    first-class control parameter -- which matrix is live measurably changes
    tracking IAE), coupling_diag_k_dc, and the HTTP-only fields
    fuzzy_strength_pct, ease_off_window_mult and approach_rate_cap_c_per_hr
    (these ARE present in the board's raw HTTP response -- this tool used to
    fetch that response and then silently drop everything but the coupling
    matrix from it, and separately never rendered the model fields at all
    despite this docstring's old text claiming "PID/model config" -- see
    docs/audits/mcp_zone_model_fields_2026-09-13.md). model_fit_temp_c/
    model_fit_ambient_c are ALSO fetched, over a second request to HTTP GET
    /api/zones_diag and merged back in by index (docs/audits/
    zones_diag_endpoint_split_2026-09-14.md -- these two moved off GET
    /api/zones once that endpoint ran low on response-buffer headroom); a
    diag-fetch failure degrades those two fields to an explicitly labeled
    "UNAVAILABLE(diag-fetch-failed)" plus a line naming the error -- NOT to
    the plain "missing" a genuinely absent field renders (adversarial review
    2026-09-14) -- without failing the rest of this tool. Host is auto-resolved the same way the
    OTA tools do (board's Wi-Fi station IP, falling back to the fallback-AP
    address); pass `host` explicitly for kilnctl.local or a board reachable
    only from a different network than this link. If the HTTP fetch fails
    the PID section above is still returned, with the model/coupling-
    matrix/HTTP-only-fields sections noting why they're missing."""
    try:
        thermo_count, relay_count, zones = _srv._control.get_zones()
    except ControlQueryError as exc:
        return f"error: {exc}"
    header = f"{thermo_count} thermocouple(s), {relay_count} relay(s)"
    body = header
    if zones:
        body += "\n" + "\n".join(z.describe() for z in zones)

    resolved_host = _control_resolve_host(host)
    try:
        zones_json = zones_http_client.get_zones(resolved_host)
    except zones_http_client.ZonesHttpError as exc:
        return body + f"\nplant model / coupling matrix: unavailable ({exc}, host={resolved_host})"
    # docs/audits/zones_diag_endpoint_split_2026-09-14.md: model_fit_temp_c/
    # model_fit_ambient_c (rendered by _describe_model_fields() below) moved
    # off GET /api/zones onto GET /api/zones_diag once the main endpoint ran
    # low on json_cap headroom. Fetched as a SECOND request and merged back
    # onto zones_json's zone dicts by index (merge_zones_diag()) so
    # _describe_model_fields()/_describe_coupling_matrix() below don't need
    # to know two endpoints exist -- they still just read zones_json["zones"].
    # A diag fetch failure degrades gracefully: the model-fit fields simply
    # read back "missing" (their existing "field(s) missing" rendering path,
    # unchanged) rather than failing this whole tool call, since the PID/
    # coupling-matrix/HTTP-only-fields sections below have nothing to do
    # with this second endpoint and must still render.
    diag_error: Optional[str] = None
    try:
        diag_json = zones_http_client.get_zones_diag(resolved_host)
        zones_json = zones_http_client.merge_zones_diag(zones_json, diag_json)
    except zones_http_client.ZonesHttpError as exc:
        # Adversarial review 2026-09-14: still degrades gracefully (the PID/
        # coupling/http-only sections have nothing to do with this endpoint
        # and must still render), but the reason is now CARRIED, not
        # swallowed -- a swallowed failure rendered "missing", which is also
        # exactly what a firmware that never emitted the field renders.
        diag_error = f"{exc} (host={resolved_host})"
    return (
        body
        + "\n" + _describe_model_fields(zones_json, diag_error)
        + "\n" + _describe_coupling_matrix(zones_json)
        + "\n" + _describe_http_only_zone_fields(zones_json)
        + "\n" + _describe_relay_types(zones_json)
    )


@_core._tool()
def control_set_zone_pid(zone: int, kp: float, ki: float, kd: float) -> str:
    """Set a zone's PID gains."""
    try:
        result = _srv._control.set_zone_pid(zone, kp, ki, kd)
    except ControlQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - zone {zone} PID set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set zone {zone} PID{detail}"


@_core._tool()
def control_set_zone_model(zone: int, k_dc: float, tau_s: float, dead_time_s: float) -> str:
    """Set a zone's feedforward thermal model (steady-state gain, time
    constant, dead time), used for model feedforward and autotune seeding."""
    try:
        result = _srv._control.set_zone_model(zone, k_dc, tau_s, dead_time_s)
    except ControlQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - zone {zone} model set"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not set zone {zone} model{detail}"


# ---------------------------------------------------------------------------
# control_set_zone_limits -- the gap this fills: the only pre-existing zones
# writer, config_presets.load_config_preset() (config_presets.py -- applies
# a whole bench_fixture.json-shaped preset via zones_http_client.
# build_post_body()), overwrites PID gains and control_mode for EVERY zone
# along with whatever limit it also carries, which would regress a bench's
# already-tuned gains just to fix one zone's max_temp_c/min_temp_c. This
# tool touches ONLY those two fields, reusing the same GET-merge-POST path
# (zones_http_client.build_post_body()) cases_heat.py's
# _restore_zone_limit()/_post_zones_restore() already use to restore a
# zone's max_temp_c after HP-07 lowers it -- see that module for the sibling
# use of the same pattern.
# ---------------------------------------------------------------------------
def _profile_or_autotune_running_reason() -> Optional[str]:
    """None if neither a profile firing nor an autotune run is currently in
    progress; otherwise a human-readable reason naming which one. Shared
    gate for any write tool that must not touch persistent zone config
    mid-run -- the same rule cases_heat.py's bench cases already respect for
    the hidden profile slot (profiles_get_exec_status()'s state/AutotuneClient.
    get_status()'s state, not merely "not idle": profile state 2 is 'paused',
    which is still a live run with heat history riding on the current
    config, not a safe window to change zone limits in)."""
    try:
        st = _srv._profiles.get_exec_status()
    except ProfilesQueryError as exc:
        return f"could not read profile exec status ({exc}) -- refusing to guess"
    # Allow-list, fail closed: anything but idle/done/faulted (running,
    # paused, or an unknown(N) state from newer firmware) refuses.
    if st.state_name not in ("idle", "done", "faulted"):
        return f"a profile is currently {st.state_name} (#{st.profile_id} {st.name!r})"
    try:
        at = _srv._autotune.get_status()
    except AutotuneQueryError as exc:
        return f"could not read autotune status ({exc}) -- refusing to guess"
    # AutotuneStatus.STATE_NAMES: 0=idle, 5=done, 6=aborted are the only
    # non-running states; everything else (settling/stepping/relay_approach/
    # relay_cycling) is a live run.
    if at.state not in (0, 5, 6):
        return f"autotune is currently {at.state_name!r} on zone {at.zone}"
    return None


# Top-level GET /api/zones keys that are firmware-derived telemetry, not
# config this tool could have posted: they may legitimately change between
# the before/after snapshots (generation bumps on every commit; safety_ceiling
# is recomputed from the new zone maxima BY DESIGN when max_temp_c changes;
# safety_tc_type(_known) is a Pico readback that can land at any moment), so
# they are excluded from the collateral diff rather than producing a false
# FAILED. Everything else at top level -- thermo_count, relay_count,
# timing_profiles, relay_names, relay_types, pc_link_abort_silence_ms, ... --
# rides the same whole-page POST and IS compared.
_ZONE_LIMITS_TOP_TELEMETRY_KEYS = frozenset({
    "generation", "safety_ceiling", "safety_tc_type", "safety_tc_type_known",
    "ct_warn_mask", "safety_wiring", "relay_zone_owned_mask",
    "on_off_hyst_c_default", "on_off_min_on_off_s_default",
})
# Per-zone measured telemetry with no POST field (a CT sweep can update it
# independently of this write).
_ZONE_LIMITS_ZONE_TELEMETRY_KEYS = frozenset({"normal_current_measured", "normal_current_a"})


def _ceiling_target_from_zones(snapshot: dict) -> "Optional[float]":
    """The Pico abs_max_temp_c target the firmware derives from a zones
    snapshot -- safety_ceiling_policy_target_c(): max over zones with index <
    thermo_count of max_temp_c, ignoring any <= 0. None when no zone
    qualifies ("no ceiling opinion": firmware leaves the Pico untouched)."""
    count = snapshot.get("thermo_count")
    best: "Optional[float]" = None
    for z in snapshot.get("zones") or []:
        idx = z.get("index")
        if isinstance(count, int) and isinstance(idx, int) and idx >= count:
            continue
        v = z.get("max_temp_c")
        if isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0 and (best is None or v > best):
            best = float(v)
    return best


def _describe_safety_ceiling(snapshot: dict) -> "tuple[str, Optional[str]]":
    """(description, mismatch_warning_or_None) for GET /api/zones' own
    read-only safety_ceiling block ({target_c, pico_known, pico_current_c}).
    On this firmware the Pico's abs_max_temp_c is NOT an independent limit:
    POST /api/zones derives it from the zone maxima and raises the Pico FIRST
    (409 if that raise can't be confirmed, e.g. Pico ARMED) or lowers it
    best-effort AFTER commit. The owner rule is that the two must be EQUAL;
    a persistent gap is what readiness item safety_ceiling_match blocks
    firing on. This tool cannot close that gap itself, so it reports it."""
    sc = snapshot.get("safety_ceiling")
    if not isinstance(sc, dict):
        return "safety_ceiling not reported by GET /api/zones", None
    target = sc.get("target_c")
    known = bool(sc.get("pico_known"))
    pico = sc.get("pico_current_c")
    if not known:
        return (f"safety ceiling target={target!r}C, Pico ceiling not yet confirmed this boot",
                "the Pico's abs_max_temp_c has not been confirmed this boot, so equality with the "
                "zone-derived target cannot be verified")
    desc = f"safety ceiling target={target!r}C, Pico abs_max_temp_c={pico!r}C"
    if (isinstance(target, (int, float)) and isinstance(pico, (int, float)) and target > 0
            and abs(float(target) - float(pico)) > 0.05):
        return desc, (f"Pico abs_max_temp_c ({pico:g}C) != zone-derived target ({target:g}C) -- "
                      f"the firmware's ceiling sync did not land (typically the Pico was ARMED); "
                      f"readiness item safety_ceiling_match blocks firing until it is reconciled")
    return desc, None


def _zone_by_index(zones: "list[dict]", index: int) -> "Optional[dict]":
    for z in zones:
        if z.get("index") == index:
            return z
    return None


def _zone_collateral_diff(
    before: dict, after: dict, zone: int, changed: "set[str]",
) -> "list[str]":
    """Compare the before/after GET /api/zones snapshots and report any
    field that changed other than the ones this tool deliberately changed on
    `zone`: every field of every zone (PID gains, control_mode, relay_mask,
    ramp rate, coupling, ...) and every top-level config field the same
    whole-page POST carries (thermo_count, relay_count, timing_profiles,
    relay_names, ...). Firmware-derived telemetry
    (_ZONE_LIMITS_TOP_TELEMETRY_KEYS/_ZONE_LIMITS_ZONE_TELEMETRY_KEYS) is
    skipped -- see those sets for why. A zone present in one snapshot and
    missing from the other is itself reported rather than silently skipped."""
    diffs: "list[str]" = []
    for key in sorted(set(before) | set(after)):
        if key == "zones" or key in _ZONE_LIMITS_TOP_TELEMETRY_KEYS:
            continue
        if before.get(key) != after.get(key):
            diffs.append(f"top-level {key}: {before.get(key)!r} -> {after.get(key)!r}")
    before_by_idx = {z.get("index"): z for z in before.get("zones") or []}
    after_by_idx = {z.get("index"): z for z in after.get("zones") or []}
    for idx in sorted(set(before_by_idx) | set(after_by_idx), key=lambda v: (v is None, v)):
        bz = before_by_idx.get(idx)
        az = after_by_idx.get(idx)
        if bz is None or az is None:
            diffs.append(f"zone {idx}: present in only one snapshot (before={bz is not None}, after={az is not None})")
            continue
        for key in sorted(set(bz) | set(az)):
            if idx == zone and key in changed:
                continue
            if key in _ZONE_LIMITS_ZONE_TELEMETRY_KEYS:
                continue
            if bz.get(key) != az.get(key):
                diffs.append(f"zone {idx}.{key}: {bz.get(key)!r} -> {az.get(key)!r}")
    return diffs


@_core._tool()
def control_set_zone_limits(
    zone: int,
    max_temp_c: Optional[float] = None,
    min_temp_c: Optional[float] = None,
    confirm: bool = False,
    host: Optional[str] = None,
) -> str:
    """Set a zone's persistent max_temp_c (ceiling) and/or min_temp_c
    (floor), touching ONLY those two fields over the GET-merge-POST
    /api/zones path (zones_http_client.build_post_body()) -- every other
    field the board reports (PID gains, control_mode, relay_mask, ramp rate,
    coupling matrix, timing profiles, ...) is echoed back exactly as read,
    never overwritten. This is the narrow tool the facade was missing: the
    only pre-existing zones writer, load_config_preset() (config_presets.py),
    overwrites PID gains and control_mode for every zone along with whatever
    limit a preset also carries.

    SAFETY CEILING: on this firmware the Pico's abs_max_temp_c (S1) is NOT
    an independent number -- POST /api/zones derives it as the max of the
    positive zone max_temp_c values (safety_ceiling_policy.h) and keeps the
    two equal itself: a RAISE of that max writes and confirms the Pico FIRST
    and refuses the whole POST (HTTP 409 safety_ceiling_raise_failed) if it
    can't -- the common case while the Pico is ARMED; a LOWER commits the
    zone first and lowers the Pico best-effort afterward. This tool
    therefore does not second-guess the raise client-side; it reports the
    ceiling the change will imply (dry run), surfaces a 409 verbatim, and
    after a write reports GET /api/zones' own safety_ceiling block, with a
    WARNING when the Pico does not equal the derived target (readiness item
    safety_ceiling_match then blocks firing until reconciled).

    Pass at least one of `max_temp_c`/`min_temp_c`; the other is left at its
    current value. Refused:
      - if neither is given, or either is not a finite number, or
        `max_temp_c` <= 0 (0 is the firmware's "no ceiling configured"
        state -- this tool never disarms a zone ceiling);
      - unless `confirm is True` exactly (a dry run otherwise -- no POST);
      - unless the profile executor reads idle/done/faulted and autotune
        reads idle/done/aborted (anything else, including an unreadable
        state, refuses);
      - if `zone` is not one of the indices GET /api/zones reports;
      - if the resulting min_temp_c >= max_temp_c.

    After a confirmed write, re-fetches GET /api/zones and FAILS LOUD if the
    newly-set field(s) don't read back within 0.05 C, or if ANY other config
    field (any zone, or top-level) differs between the before and after
    snapshots. Firmware-derived telemetry that legitimately moves on this
    write (generation, safety_ceiling, measured currents) is excluded.

    Uses the http_auth ADMIN-session seam via zones_http_client -- never
    prints, logs, or echoes a credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as load_config_preset()

    if max_temp_c is None and min_temp_c is None:
        return "error: pass at least one of max_temp_c/min_temp_c"
    for name, val in (("max_temp_c", max_temp_c), ("min_temp_c", min_temp_c)):
        if val is None:
            continue
        if isinstance(val, bool) or not isinstance(val, (int, float)) or not math.isfinite(val):
            return f"refused: {name}={val!r} is not a finite number"
    if max_temp_c is not None and max_temp_c <= 0:
        return (f"refused: max_temp_c={max_temp_c:g} -- <= 0 is the firmware's 'no ceiling configured' "
                f"state; this tool never disarms a zone ceiling")

    resolved = _ota_resolve_host(host)

    running_reason = _profile_or_autotune_running_reason()
    if running_reason is not None:
        return f"refused: {running_reason} -- zone limits are not changed mid-run (host={resolved})"

    try:
        before = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return f"error: GET /api/zones failed (host={resolved}): {exc}"

    zones = before.get("zones") or []
    valid_indices = sorted(z.get("index") for z in zones if "index" in z)
    if zone not in valid_indices:
        return f"refused: zone {zone} is out of range -- board reports zones {valid_indices} (host={resolved})"

    current = _zone_by_index(zones, zone) or {}
    effective_max = max_temp_c if max_temp_c is not None else current.get("max_temp_c")
    effective_min = min_temp_c if min_temp_c is not None else current.get("min_temp_c")
    if (isinstance(effective_max, (int, float)) and isinstance(effective_min, (int, float))
            and effective_min >= effective_max):
        return (f"refused: min_temp_c ({effective_min:g}) must be strictly less than "
                f"max_temp_c ({effective_max:g}) (host={resolved})")

    changed_fields: "set[str]" = set()
    zone_override: "dict[str, Any]" = {"index": zone}
    if max_temp_c is not None:
        zone_override["max_temp_c"] = max_temp_c
        changed_fields.add("max_temp_c")
    if min_temp_c is not None:
        zone_override["min_temp_c"] = min_temp_c
        changed_fields.add("min_temp_c")

    ceiling_before_desc, _ = _describe_safety_ceiling(before)
    proposed = {**before, "zones": [
        ({**z, **{k: v for k, v in zone_override.items() if k != "index"}} if z.get("index") == zone else z)
        for z in zones
    ]}
    target_before = _ceiling_target_from_zones(before)
    target_after = _ceiling_target_from_zones(proposed)
    if target_after is not None and (target_before is None or target_after > target_before + 0.01):
        ceiling_effect = (f"derived Pico ceiling target {target_before!r} -> {target_after:g}C: the firmware "
                          f"will RAISE the Pico first and refuse the POST (409) if it cannot confirm that "
                          f"(expected while the Pico is ARMED)")
    elif target_after is not None and target_before is not None and target_after < target_before - 0.01:
        ceiling_effect = (f"derived Pico ceiling target {target_before:g} -> {target_after:g}C: the firmware "
                          f"lowers the Pico best-effort after committing (may stay wider if ARMED)")
    else:
        ceiling_effect = f"derived Pico ceiling target unchanged ({target_after!r})"

    if confirm is not True:
        wanted_desc = ", ".join(f"{k}={v:g}" for k, v in zone_override.items() if k != "index")
        return (
            f"DRY RUN (pass confirm=True, exactly, to actually write) -- would set zone {zone}: "
            f"{wanted_desc} (current: max_temp_c={current.get('max_temp_c')!r}, "
            f"min_temp_c={current.get('min_temp_c')!r}; {ceiling_effect}; now: {ceiling_before_desc}; "
            f"host={resolved})"
        )

    try:
        body = zones_http_client.build_post_body(before, {"zones": [zone_override]})
    except zones_http_client.ZonesHttpError as exc:
        return f"error: could not build POST body from the GET snapshot: {exc}"

    try:
        post_result = zones_http_client.post_zones(resolved, body)
    except zones_http_client.ZonesHttpError as exc:
        if exc.status == 409 and "safety_ceiling_raise_failed" in (exc.detail or ""):
            return (f"refused: the firmware could not raise/confirm the Pico's abs_max_temp_c to "
                    f"{target_after!r}C first, so it left the zone config UNCHANGED (HTTP 409): "
                    f"{exc.detail} -- the Pico refuses config writes while ARMED; see "
                    f"safety_ceiling_policy.h (host={resolved})")
        if exc.status == 409 and zones_http_client.is_system_mode_gate_refusal(exc.detail):
            return (f"refused: system_mode_gate refused this write (HTTP 409): {exc.detail} -- "
                    f"a firing or autotune run started after this tool's own precheck; distinct "
                    f"from the safety-ceiling-raise 409 above and from OTA's 428 interlock "
                    f"(host={resolved})")
        return f"error: POST /api/zones failed (host={resolved}): {exc}"
    if post_result != "ok":
        return f"refused: POST /api/zones refused: {post_result} (host={resolved})"

    try:
        after = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return (f"error: POST /api/zones returned ok, but the confirming re-fetch of GET "
                f"/api/zones failed (host={resolved}): {exc} -- state UNKNOWN, re-check before "
                f"trusting this")

    after_zones = after.get("zones") or []
    after_zone = _zone_by_index(after_zones, zone)
    if after_zone is None:
        return f"FAILED: zone {zone} missing from the re-fetched GET /api/zones response (host={resolved})"

    mismatches = []
    for key in sorted(changed_fields):
        wanted = zone_override[key]
        got = after_zone.get(key)
        if not isinstance(got, (int, float)) or abs(float(got) - float(wanted)) > 0.05:
            mismatches.append(f"{key}: wanted {wanted:g}, board now reports {got!r}")
    if mismatches:
        return (f"FAILED: POST /api/zones returned ok, but read-back does not confirm it "
                f"landed -- {'; '.join(mismatches)} (host={resolved}). Do not trust this as applied.")

    collateral = _zone_collateral_diff(before, after, zone, changed_fields)
    if collateral:
        return (f"FAILED: zone {zone}'s {', '.join(sorted(changed_fields))} landed correctly, but "
                f"other field(s) changed unexpectedly -- {'; '.join(collateral)} (host={resolved}). "
                f"This tool must touch only the named fields; investigate before trusting this "
                f"board's config.")

    ceiling_after_desc, ceiling_warning = _describe_safety_ceiling(after)
    applied_desc = ", ".join(f"{k}={after_zone.get(k):g}" for k in sorted(changed_fields))
    result = f"ok - zone {zone}: {applied_desc} (confirmed by read-back; {ceiling_after_desc}; host={resolved})"
    if ceiling_warning:
        result += f"\nWARNING: {ceiling_warning}"
    return result


# ---------------------------------------------------------------------------
# control_set_zone_type -- narrow writer for one zone's zone_type field only,
# modeled directly on control_set_zone_limits() above (same GET-merge-POST
# /api/zones path, zones_http_client.build_post_body(), same confirm gate,
# same mode-gate/collateral read-back discipline). The only pre-existing
# zone_type writer is config_presets.load_config_preset(), which is a whole
# bench_fixture.json-shaped preset POST that also overwrites PID gains and
# control_mode for every zone -- forbidden for a single-field fix.
#
# Why this exists: bench zone 2 was left zone_type=ZONE_TYPE_ON_OFF (1) by an
# unverified preset restore, which silently made a 3-zone firing never close
# zone 2's relay (HP-02) -- ZONE_TYPE_ON_OFF zones are driven by hysteresis/
# on-off rules, not the PID loop a firing profile assumes. zone_type is a
# uint8_t enum (zones_config_accessors.h's zone_type_t): ZONE_TYPE_HEATER = 0
# (PID-controlled), ZONE_TYPE_ON_OFF = 1 (hysteresis-controlled). Validated
# range-checked firmware-side too (zones_config_json.c: "zone zone_type out
# of range" if > ZONE_TYPE_ON_OFF).
# ---------------------------------------------------------------------------
ZONE_TYPE_HEATER = 0
ZONE_TYPE_ON_OFF = 1
#: zones_config_accessors.h's ZONE_COUPLING_COEFF_MAX -- the firmware's own
#: range ceiling for an off-diagonal coupling_coeff cell (the diagonal is
#: forced to exactly 0). Mirrored here, not imported, the same way this
#: module already mirrors ZONE_TYPE_HEATER/ZONE_TYPE_ON_OFF from the C enum.
ZONE_COUPLING_COEFF_MAX = 100.0
_ZONE_TYPE_NAMES = {ZONE_TYPE_HEATER: "heater/PID", ZONE_TYPE_ON_OFF: "on/off"}
# What else the firmware does differently once a zone has this type -- none
# of it is a live relay/integral side effect (the write is refused mid-run),
# but each changes what the NEXT firing or profile save does with the zone.
_ZONE_TYPE_CONSEQUENCE = {
    ZONE_TYPE_HEATER: (
        "note: a heater zone is PID-driven, needs a max_temp_c ceiling (zone_needs_ceiling()), "
        "and profile on/off rules aimed at it are refused on profile save (profiles_http.c)"),
    ZONE_TYPE_ON_OFF: (
        "note: an on/off zone is never PID-driven; its coupling row/column read as 0 at use "
        "(zones_config_get_coupling(), stored cells kept), and a custom profile including it "
        "with no on/off rule is refused at start (HP-02, profile_executor_run.c)"),
}


@_core._tool()
def control_set_zone_type(
    zone: int,
    zone_type: int,
    confirm: bool = False,
    host: Optional[str] = None,
) -> str:
    """Set a zone's persistent zone_type (0 = ZONE_TYPE_HEATER/PID-controlled,
    1 = ZONE_TYPE_ON_OFF/hysteresis-controlled), touching ONLY that one field
    over the GET-merge-POST /api/zones path (zones_http_client.
    build_post_body()) -- every other field the board reports (PID gains,
    control_mode, relay_mask, limits, ramp rate, coupling matrix, timing
    profiles, ...) is echoed back exactly as read, never overwritten. Modeled
    directly on control_set_zone_limits(); see that tool's docstring for the
    shared GET-merge-POST/collateral-diff discipline.

    The only pre-existing zone_type writer is load_config_preset()
    (config_presets.py), which overwrites PID gains and control_mode for
    every zone along with whatever zone_type a preset also carries -- forbidden
    for a single-field fix. This tool exists because an unverified preset
    restore once left a bench zone at zone_type=1 (on/off), which silently
    made a 3-zone firing never close that zone's relay (a hysteresis-
    controlled zone is not driven by the PID loop a firing profile assumes).

    Refused:
      - if `zone_type` is not 0 or 1 (the firmware's own valid range --
        zones_config_json.c refuses anything > ZONE_TYPE_ON_OFF);
      - unless `confirm is True` exactly (a dry run otherwise -- no POST);
      - unless the profile executor reads idle/done/faulted and autotune
        reads idle/done/aborted (anything else, including an unreadable
        state, refuses) -- the same system_mode_gate window
        control_set_zone_limits() respects, since changing a zone's control
        strategy mid-run is exactly as unsafe as changing its limits;
      - if `zone` is not one of the indices GET /api/zones reports.

    After a confirmed write, re-fetches GET /api/zones and FAILS LOUD if
    zone_type doesn't read back exactly as posted, or if ANY other config
    field (any zone, or top-level) differs between the before and after
    snapshots -- reusing control_set_zone_limits()'s _zone_collateral_diff()
    so the same firmware-derived-telemetry exclusions (generation,
    safety_ceiling, measured currents) apply and nothing else is silently
    permitted to drift.

    Uses the http_auth ADMIN-session seam via zones_http_client -- never
    prints, logs, or echoes a credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as control_set_zone_limits()

    if isinstance(zone_type, bool) or not isinstance(zone_type, int) or zone_type not in (ZONE_TYPE_HEATER, ZONE_TYPE_ON_OFF):
        return (f"refused: zone_type={zone_type!r} is not a valid zone_type -- must be "
                f"{ZONE_TYPE_HEATER} (heater/PID) or {ZONE_TYPE_ON_OFF} (on/off)")

    resolved = _ota_resolve_host(host)

    running_reason = _profile_or_autotune_running_reason()
    if running_reason is not None:
        return f"refused: {running_reason} -- zone_type is not changed mid-run (host={resolved})"

    try:
        before = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return f"error: GET /api/zones failed (host={resolved}): {exc}"

    zones = before.get("zones") or []
    valid_indices = sorted(z.get("index") for z in zones if "index" in z)
    if zone not in valid_indices:
        return f"refused: zone {zone} is out of range -- board reports zones {valid_indices} (host={resolved})"

    current = _zone_by_index(zones, zone) or {}
    current_type = current.get("zone_type")

    changed_fields = {"zone_type"}
    zone_override: "dict[str, Any]" = {"index": zone, "zone_type": zone_type}

    if confirm is not True:
        return (
            f"DRY RUN (pass confirm=True, exactly, to actually write) -- would set zone {zone}: "
            f"zone_type={zone_type} ({_ZONE_TYPE_NAMES.get(zone_type, '?')}) "
            f"(current: zone_type={current_type!r} ({_ZONE_TYPE_NAMES.get(current_type, '?')}); "
            f"host={resolved})\n{_ZONE_TYPE_CONSEQUENCE[zone_type]}"
        )

    try:
        body = zones_http_client.build_post_body(before, {"zones": [zone_override]})
    except zones_http_client.ZonesHttpError as exc:
        return f"error: could not build POST body from the GET snapshot: {exc}"

    try:
        post_result = zones_http_client.post_zones(resolved, body)
    except zones_http_client.ZonesHttpError as exc:
        if exc.status == 409 and zones_http_client.is_system_mode_gate_refusal(exc.detail):
            return (f"refused: system_mode_gate refused this write (HTTP 409): {exc.detail} -- "
                    f"a firing or autotune run started after this tool's own precheck "
                    f"(host={resolved})")
        return f"error: POST /api/zones failed (host={resolved}): {exc}"
    if post_result != "ok":
        return f"refused: POST /api/zones refused: {post_result} (host={resolved})"

    try:
        after = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return (f"error: POST /api/zones returned ok, but the confirming re-fetch of GET "
                f"/api/zones failed (host={resolved}): {exc} -- state UNKNOWN, re-check before "
                f"trusting this")

    after_zones = after.get("zones") or []
    after_zone = _zone_by_index(after_zones, zone)
    if after_zone is None:
        return f"FAILED: zone {zone} missing from the re-fetched GET /api/zones response (host={resolved})"

    got = after_zone.get("zone_type")
    if got != zone_type:
        return (f"FAILED: POST /api/zones returned ok, but read-back does not confirm it "
                f"landed -- zone_type: wanted {zone_type}, board now reports {got!r} "
                f"(host={resolved}). Do not trust this as applied.")

    collateral = _zone_collateral_diff(before, after, zone, changed_fields)
    if collateral:
        return (f"FAILED: zone {zone}'s zone_type landed correctly, but other field(s) changed "
                f"unexpectedly -- {'; '.join(collateral)} (host={resolved}). This tool must touch "
                f"only zone_type; investigate before trusting this board's config.")

    return (f"ok - zone {zone}: zone_type={got} ({_ZONE_TYPE_NAMES.get(got, '?')}) "
            f"(confirmed by read-back; host={resolved})\n{_ZONE_TYPE_CONSEQUENCE[zone_type]}")


# ---------------------------------------------------------------------------
# control_set_zone_coupling -- narrow writer for ONE coupling-matrix cell,
# modeled directly on control_set_zone_type()/control_set_zone_limits() above
# (same GET-merge-POST /api/zones path, zones_http_client.build_post_body(),
# same confirm gate, same mode-gate/collateral read-back discipline). No PID,
# control_mode, limits, zone_type or model (k_dc/tau_s/dead_time_s) collateral.
#
# Why this exists: backup_import.c's coupling-cell import bug (fixed in
# fee835fa/b4bad2c7 -- an on/off-typed zone's coupling row/column was zeroed on
# import instead of preserved) left bench zone 2's cross-terms at 0. The
# only pre-existing coupling writer is load_config_preset() (config_presets.py),
# a whole bench_fixture.json-shaped preset POST that also overwrites PID
# gains and control_mode for every zone -- forbidden for a single-cell fix,
# same reasoning as control_set_zone_type()'s own HP-02 incident.
#
# Wire format (read from zones_http_post_parse.c/zones_http_get.c directly,
# not assumed): GET /api/zones reports coupling as MAX31856_CHANNEL_COUNT
# indexed keys PER ZONE, "coupling_c0".."coupling_c{N-1}" (zones_http_get.c:
# APPEND("\"coupling_c%u\":%.9g,", j, ...) as of 2026-09-28 -- was %.4f, see
# "GET-merge-POST rounding" below), where zone i's coupling_c{j} is
# zone j's (the STEPPED zone) measured/authored effect on zone i (the
# AFFECTED zone) -- row i = affected, column j = stepped, per this module's
# own _describe_coupling_matrix(). The matching POST field is
# "z{i}_coupling_c{j}" (zones_http_post_parse.c: `snprintf(key, ...,
# "z%u_coupling_c%u", i, j)`), parsed with zones_config_json_parse_float_field()
# ranged to [0, ZONE_COUPLING_COEFF_MAX] (100.0f) for j != i, and to exactly
# [0, 0] (i.e. only 0 is accepted) for the diagonal j == i -- the firmware
# refuses a nonzero diagonal outright ("zone coupling_coeff out of range"),
# it does not silently zero it, so this tool refuses the diagonal client-side
# too rather than relying on that 400.
#
# Each z{i}_coupling_c{j} field is OPTIONAL and, unlike most per-zone fields
# on this whole-page-submit handler (which default to 0 if omitted, since
# `tmp` starts zeroed), an OMITTED coupling cell PRESERVES the currently-
# stored value (zones_http_post_parse.c's per-cell loop: `if (...field_present...)
# ... else { z->coupling_coeff[j] = current_z->coupling_coeff[j]; }` -- the
# same convention as z%u_fuzzy_strength/coupling_diag_k_dc, added 2026-08-30
# specifically so a whole-page save from an operator's browser -- which only
# ever renders/submits ALL cells together -- doesn't accidentally delete a
# measurement other code paths (autotune's own coupling-cell setter) wrote.
# The REQUIRED per-zone fields (PID gains, relay_mask, control_mode, ...)
# still have to be posted, so the body is still built by the GET-merge-POST
# path (zones_http_client.build_post_body(), which echoes every field), and
# then the omit-preserved measured fields are stripped back out -- see
# "GET-merge-POST rounding" below.
#
# zones_http_client.build_post_body()'s only per-zone list-shaped override
# key is "coupling_coeff" (_PRESET_ZONE_COUPLING_FIELD) -- a bare
# "coupling_c{j}" override key is recognized only as a REFERENCE field
# (silently ignored, never applied; see build_post_body()'s own
# _ZONE_COUPLING_CELL_RE branch) to support a preset authored by copying a
# live GET/backup-export response verbatim. This tool therefore builds the
# override as a full-width "coupling_coeff" list seeded from the zone's
# CURRENT GET-reported cells (so every other cell in the row round-trips
# unchanged, not zeroed) with only index `from_zone` replaced -- never a
# bare "coupling_cN" key.
#
# GET-merge-POST rounding, and what this tool strips to avoid it: GET
# /api/zones prints most floats at %.4f (model_tau_s/model_dead_time_s at
# %.1f), so a GET-merge-POST re-posts every field at GET's rounded precision.
# 2026-09-28: pid_kp/ki/kd, model_k_dc, coupling_diag_k_dc and coupling_c%u
# moved to %.9g (zones_http_get.c/backup_export.c) specifically because this
# rounding was silently zeroing a tuned Ki like 0.000034 on every narrow
# GET-merge-POST writer -- %.9g is lossless for a float32, so those fields no
# longer accumulate residual on a round trip. cal_offset_c/max_ramp_c_per_hr/
# and the rest of the REQUIRED fields (relay_mask, control_mode, ...; omitted
# means 400 or 0) are still printed at their prior, coarser precision (%.3f/
# %.2f/%.1f/%.0f) and still carry the same residual
# control_set_zone_type()/control_set_zone_limits() always have: well below
# parse_zone_fields()'s own 0.0001 tuning_valid-invalidation tolerance, and
# exactly zero on a value that already went through one such round trip (a
# prior page save or a backup import, which also exports at that same
# precision). For the MEASURED fields parse_zone_fields() omit-PRESERVES from
# the live struct (z%u_k/z%u_tau/z%u_deadtime, z%u_coupling_diag_k_dc and
# every z%u_coupling_c%u), this tool drops them from the body
# (_strip_omit_preserved_zone_fields()) except the one target cell, so the
# plant model, the coupling diagonal gain and every OTHER coupling cell of
# every zone are preserved bit-exact by the firmware rather than re-posted
# rounded. coupling_tau_c%u/coupling_dead_time_c%u have no POST field at all
# and are memcpy()'d from the live struct unconditionally. The read-back's
# collateral diff (_zone_collateral_diff(), exact equality on the printed
# values) then catches anything else that moved.
# ---------------------------------------------------------------------------
#: POST keys parse_zone_fields() (zones_http_post_parse.c) treats as
#: omit-PRESERVES-current -- verified per key against that file: z%u_k/
#: z%u_tau/z%u_deadtime (`http_form_find_field(...) > 0` else current_z),
#: z%u_coupling_diag_k_dc and z%u_coupling_c%u
#: (`zones_config_json_field_present()` else current_z). Nothing else is
#: stripped: a required field left out of the body would 400 or zero.
_ZONE_OMIT_PRESERVED_KEY_RE = re.compile(r"^z\d+_(?:k|tau|deadtime|coupling_diag_k_dc|coupling_c\d+)$")


def _strip_omit_preserved_zone_fields(body: str, keep_key: "Optional[str]") -> str:
    """Drop every _ZONE_OMIT_PRESERVED_KEY_RE key from a build_post_body()
    form body except `keep_key` (None keeps none: control_set_relay_type()
    posts no zone field at all). Raises ZonesHttpError if a non-None
    `keep_key` is not in the body (the write would silently be a no-op)."""
    pairs = urllib.parse.parse_qsl(body, keep_blank_values=True)
    if keep_key is not None and not any(k == keep_key for k, _ in pairs):
        raise zones_http_client.ZonesHttpError(
            f"built POST body carries no {keep_key!r} field -- refusing to post a no-op")
    kept = [(k, v) for k, v in pairs if k == keep_key or not _ZONE_OMIT_PRESERVED_KEY_RE.match(k)]
    return urllib.parse.urlencode(kept)


_ZONE_COUPLING_READBACK_TOLERANCE = 0.0005  # generous vs. GET's %.9g coupling_c%u
                                             # print (2026-09-28; was %.4f) --
                                             # kept unchanged since it was
                                             # already comfortably above any
                                             # float32 rounding noise


@_core._tool()
def control_set_zone_coupling(
    zone: int,
    from_zone: int,
    coeff: float,
    confirm: bool = False,
    host: Optional[str] = None,
) -> str:
    """Set ONE cell of the coupling matrix -- zone `zone`'s (the AFFECTED
    zone) measured/authored response to zone `from_zone`'s (the STEPPED
    zone) heater, i.e. GET /api/zones' zones[zone].coupling_c{from_zone} --
    touching ONLY that one field over the GET-merge-POST /api/zones path
    (zones_http_client.build_post_body()) -- every other field the board
    reports is either echoed back as GET printed it (the fields the firmware
    requires: PID gains, control_mode, relay_mask, limits, zone_type, timing
    profiles, ...) or left out of the body so the firmware preserves it
    bit-exact (model k_dc/tau_s/dead_time_s, coupling_diag_k_dc, every OTHER
    coupling cell of every zone). Modeled directly on
    control_set_zone_type()/control_set_zone_limits(); see this module's
    section comment just above for the wire-format detail (field names,
    units, bounds, the diagonal refusal, and which fields are stripped).

    The only pre-existing coupling writer is load_config_preset()
    (config_presets.py), which overwrites PID gains and control_mode for
    every zone along with whatever coupling matrix a preset also carries --
    forbidden for a single-cell fix. This tool exists because
    backup_import.c's coupling-cell import bug (fixed in fee835fa/b4bad2c7) left
    bench zone 2's coupling cross-terms zeroed with no narrow way to restore
    them short of a whole-page preset.

    Refused:
      - if `zone == from_zone` (the diagonal; the firmware forces it to
        exactly 0 and refuses any other value -- zones_config_json.c/
        zones_http_post_parse.c);
      - if `zone` or `from_zone` is not one of the indices GET /api/zones
        reports;
      - if `coeff` is not a finite number, or is outside [0, 100.0]
        (ZONE_COUPLING_COEFF_MAX -- the firmware's own range for an
        off-diagonal cell; negative coupling is not a representable value
        here);
      - unless `confirm is True` exactly (a dry run otherwise -- no POST);
      - unless the profile executor reads idle/done/faulted and autotune
        reads idle/done/aborted (anything else, including an unreadable
        state, refuses) -- the same system_mode_gate window
        control_set_zone_limits()/control_set_zone_type() respect, since
        POST /api/zones refuses EVERY zone/relay/guard config write while a
        firing or autotune run is active (owner decision Q2, 2026-09-25,
        SYSTEM_MODE_GATE_PLAN.md).

    After a confirmed write, re-fetches GET /api/zones and FAILS LOUD unless
    the target cell reads back within 0.0005 (comfortably above GET's own
    %.9g print precision, 2026-09-28; was %.4f) of `coeff`, or if ANY other config field (any zone, or
    top-level -- including every OTHER coupling cell) differs between the
    before and after snapshots -- reusing control_set_zone_limits()'s
    _zone_collateral_diff() so the same firmware-derived-telemetry
    exclusions (generation, safety_ceiling, measured currents) apply.

    Uses the http_auth ADMIN-session seam via zones_http_client -- never
    prints, logs, or echoes a credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as control_set_zone_limits()

    if zone == from_zone:
        return f"refused: zone == from_zone ({zone}) -- the coupling diagonal is always 0, never writable"

    if isinstance(coeff, bool) or not isinstance(coeff, (int, float)) or not math.isfinite(coeff):
        return f"refused: coeff={coeff!r} is not a finite number"
    if coeff < 0.0 or coeff > ZONE_COUPLING_COEFF_MAX:
        return (f"refused: coeff={coeff!r} is out of range -- must be within "
                f"[0, {ZONE_COUPLING_COEFF_MAX:g}] (ZONE_COUPLING_COEFF_MAX)")

    resolved = _ota_resolve_host(host)

    running_reason = _profile_or_autotune_running_reason()
    if running_reason is not None:
        return f"refused: {running_reason} -- zone coupling is not changed mid-run (host={resolved})"

    try:
        before = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return f"error: GET /api/zones failed (host={resolved}): {exc}"

    zones = before.get("zones") or []
    valid_indices = sorted(z.get("index") for z in zones if "index" in z)
    if zone not in valid_indices:
        return f"refused: zone {zone} is out of range -- board reports zones {valid_indices} (host={resolved})"
    if from_zone not in valid_indices:
        return f"refused: from_zone {from_zone} is out of range -- board reports zones {valid_indices} (host={resolved})"

    current = _zone_by_index(zones, zone) or {}
    field_name = f"coupling_c{from_zone}"
    current_cell = current.get(field_name)

    # Full-width overlay, seeded from what GET reported for every cell of this
    # zone's row -- build_post_body()'s "coupling_coeff" override key applies
    # this whole list (skipping the diagonal itself), so a bare "coupling_cN"
    # override key would be silently ignored (see the section comment above)
    # and every OTHER cell must round-trip through this list unchanged, not
    # zeroed by an absent index.
    n = len(valid_indices)
    coeffs = [current.get(f"coupling_c{j}") for j in range(n)]
    coeffs[from_zone] = coeff
    changed_fields = {field_name}
    zone_override: "dict[str, Any]" = {"index": zone, "coupling_coeff": coeffs}

    if confirm is not True:
        return (
            f"DRY RUN (pass confirm=True, exactly, to actually write) -- would set zone {zone}'s "
            f"{field_name}={coeff:g} (current: {field_name}={current_cell!r}; host={resolved})"
        )

    try:
        body = _strip_omit_preserved_zone_fields(
            zones_http_client.build_post_body(before, {"zones": [zone_override]}),
            f"z{zone}_{field_name}")
    except zones_http_client.ZonesHttpError as exc:
        return f"error: could not build POST body from the GET snapshot: {exc}"

    try:
        post_result = zones_http_client.post_zones(resolved, body)
    except zones_http_client.ZonesHttpError as exc:
        if exc.status == 409 and zones_http_client.is_system_mode_gate_refusal(exc.detail):
            return (f"refused: system_mode_gate refused this write (HTTP 409): {exc.detail} -- "
                    f"a firing or autotune run started after this tool's own precheck "
                    f"(host={resolved})")
        return f"error: POST /api/zones failed (host={resolved}): {exc}"
    if post_result != "ok":
        return f"refused: POST /api/zones refused: {post_result} (host={resolved})"

    try:
        after = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return (f"error: POST /api/zones returned ok, but the confirming re-fetch of GET "
                f"/api/zones failed (host={resolved}): {exc} -- state UNKNOWN, re-check before "
                f"trusting this")

    after_zones = after.get("zones") or []
    after_zone = _zone_by_index(after_zones, zone)
    if after_zone is None:
        return f"FAILED: zone {zone} missing from the re-fetched GET /api/zones response (host={resolved})"

    got = after_zone.get(field_name)
    if not isinstance(got, (int, float)) or abs(float(got) - float(coeff)) > _ZONE_COUPLING_READBACK_TOLERANCE:
        return (f"FAILED: POST /api/zones returned ok, but read-back does not confirm it "
                f"landed -- {field_name}: wanted {coeff!r}, board now reports {got!r} "
                f"(host={resolved}). Do not trust this as applied.")

    collateral = _zone_collateral_diff(before, after, zone, changed_fields)
    if collateral:
        return (f"FAILED: zone {zone}'s {field_name} landed correctly, but other field(s) changed "
                f"unexpectedly -- {'; '.join(collateral)} (host={resolved}). This tool must touch "
                f"only {field_name}; investigate before trusting this board's config.")

    return (f"ok - zone {zone}: {field_name}={got:g} (zone {from_zone}'s effect on zone {zone}) "
            f"(confirmed by read-back; host={resolved})")


# ---------------------------------------------------------------------------
# control_set_relay_type -- narrow writer for ONE relay's device type (what the
# relay physically drives), modeled on control_set_zone_type()/
# control_set_zone_coupling() above (same GET-merge-POST /api/zones path, same
# confirm gate, mode-gate precheck + 409, collateral read-back).
#
# Why this exists: docs/ZONE_GRAPHIC_PLAN.md M17's bench round trip needs to
# change a relay's device type, and the only POST path was a hand-built form.
#
# Wire format (read from the firmware, not the plan doc -- the plan says
# `relay_type_N=`, the code says otherwise): GET /api/zones carries a TOP-LEVEL
# "relay_types":[t0..t3], one small integer per relay, index r = relay r+1
# (zones_http_get.c). The POST key is "relay<N>_type" with N = 1-based relay
# number (zones_http_post.c: snprintf(tkey, ..., "relay%u_type", r), r in
# 1..KILN_IO_RELAY_COUNT), a decimal integer in [0, RELAY_DEVICE_TYPE_COUNT)
# (zones_config_accessors.h's relay_device_type_t: 0 unset, 1 damper, 2 outlet,
# 3 valve, 4 fan, 5 light, 6 other); out of range or non-numeric is a 400. NOT
# to be confused with the per-ZONE "z%u_relaytype" (SSR/contactor/mercury,
# zones_http_post_parse.c), which is a different field.
#
# The field is omit-PRESERVES (relay_names_cfg_t, a separate struct), and
# build_post_body() never echoes relay_types/relay_names, so the body carries
# exactly one relay-keyed field: the one this tool appends. Every zone field
# the firmware omit-preserves (plant model, coupling cells, coupling_diag_k_dc)
# is stripped from the body so it stays bit-exact, as in
# control_set_zone_coupling(); required fields are re-posted at GET's print
# precision, the same residual the other narrow writers carry.
# ---------------------------------------------------------------------------
#: zones_config_accessors.h's relay_device_type_t, mirrored (a unit test
#: parses the header and fails on drift).
_RELAY_DEVICE_TYPE_NAMES = {0: "unset", 1: "damper", 2: "outlet", 3: "valve",
                            4: "fan", 5: "light", 6: "other"}
_RELAY_DEVICE_TYPE_BY_NAME = {v: k for k, v in _RELAY_DEVICE_TYPE_NAMES.items()}


def _relay_device_type_name(value: Any) -> str:
    if isinstance(value, int) and not isinstance(value, bool) and value in _RELAY_DEVICE_TYPE_NAMES:
        return _RELAY_DEVICE_TYPE_NAMES[value]
    return "?"


def _parse_relay_device_type(device_type: Any) -> "Optional[int]":
    """Number or name (case-insensitive) -> enum value, or None if invalid.
    bool is refused (True would otherwise read as 1)."""
    if isinstance(device_type, bool):
        return None
    if isinstance(device_type, int):
        return device_type if device_type in _RELAY_DEVICE_TYPE_NAMES else None
    if isinstance(device_type, str):
        s = device_type.strip().lower()
        if s in _RELAY_DEVICE_TYPE_BY_NAME:
            return _RELAY_DEVICE_TYPE_BY_NAME[s]
        if s.isdigit():
            return int(s) if int(s) in _RELAY_DEVICE_TYPE_NAMES else None
    return None


def _describe_relay_types(zones_json: dict) -> str:
    """One line: every relay's device type (GET /api/zones' top-level
    relay_types), 1-based like the POST key, each with its name."""
    types = zones_json.get("relay_types")
    if not isinstance(types, list):
        return "relay device types: not present in this response (firmware predates relay_types)"
    bits = [f"relay{n}={t!r} ({_relay_device_type_name(t)})" for n, t in enumerate(types, start=1)]
    return "relay device types (what each relay drives; 1-based like relay<N>_type): " + "  ".join(bits)


@_core._tool()
def control_set_relay_type(
    relay: int,
    device_type: "int | str",
    confirm: bool = False,
    host: Optional[str] = None,
) -> str:
    """Set ONE relay's persistent device type (what it physically drives):
    0/unset, 1/damper, 2/outlet, 3/valve, 4/fan, 5/light, 6/other -- a number
    or a case-insensitive name. `relay` is 1-based, the same numbering as the
    POST key relay<N>_type and relay_mask bit N-1 (GET /api/zones
    relay_types[relay-1]). Not the per-zone SSR/contactor/mercury field.

    Touches ONLY that one field over the GET-merge-POST /api/zones path
    (zones_http_client.build_post_body()); every zone field the firmware
    omit-preserves (plant model, coupling cells, coupling_diag_k_dc) is
    stripped from the body so it stays bit-exact. See this module's section
    comment above for the wire format and why it differs from the plan doc.

    Refused: a `relay` outside 1..len(relay_types) or a bad `device_type`
    (before any board access beyond the GET that learns the relay count);
    unless `confirm is True` exactly (dry run otherwise, no POST); unless
    the profile executor reads idle/done/faulted and autotune idle/done/
    aborted (precheck), and again if POST answers the system_mode_gate 409.

    After a confirmed write, re-fetches GET /api/zones and FAILS LOUD unless
    relay_types[relay-1] reads back as posted, or if ANY other config field
    (any zone, or top-level, including every other relay's type) differs,
    via _zone_collateral_diff().

    Uses the http_auth ADMIN-session seam via zones_http_client -- never
    prints, logs, or echoes a credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import, same convention as control_set_zone_limits()

    if isinstance(relay, bool) or not isinstance(relay, int):
        return f"refused: relay={relay!r} is not an integer relay number (1-based)"
    wanted = _parse_relay_device_type(device_type)
    if wanted is None:
        return (f"refused: device_type={device_type!r} is not valid -- use a number or name from "
                f"{', '.join(f'{k}={v}' for k, v in _RELAY_DEVICE_TYPE_NAMES.items())}")

    resolved = _ota_resolve_host(host)

    running_reason = _profile_or_autotune_running_reason()
    if running_reason is not None:
        return f"refused: {running_reason} -- a relay's device type is not changed mid-run (host={resolved})"

    try:
        before = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return f"error: GET /api/zones failed (host={resolved}): {exc}"

    types = before.get("relay_types")
    if not isinstance(types, list) or not types:
        return (f"refused: GET /api/zones reports no relay_types (host={resolved}) -- "
                f"firmware predates the field; nothing to write")
    if relay < 1 or relay > len(types):
        return f"refused: relay {relay} is out of range -- board has relays 1..{len(types)} (host={resolved})"

    current = types[relay - 1]
    field_name = f"relay{relay}_type"

    if confirm is not True:
        return (
            f"DRY RUN (pass confirm=True, exactly, to actually write) -- would set {field_name}="
            f"{wanted} ({_relay_device_type_name(wanted)}) (current: {current!r} "
            f"({_relay_device_type_name(current)}); host={resolved})"
        )

    try:
        body = _strip_omit_preserved_zone_fields(
            zones_http_client.build_post_body(before, {"zones": []}), None)
    except zones_http_client.ZonesHttpError as exc:
        return f"error: could not build POST body from the GET snapshot: {exc}"
    body = body + ("&" if body else "") + urllib.parse.urlencode({field_name: str(wanted)})

    try:
        post_result = zones_http_client.post_zones(resolved, body)
    except zones_http_client.ZonesHttpError as exc:
        if exc.status == 409 and zones_http_client.is_system_mode_gate_refusal(exc.detail):
            return (f"refused: system_mode_gate refused this write (HTTP 409): {exc.detail} -- "
                    f"a firing or autotune run started after this tool's own precheck "
                    f"(host={resolved})")
        return f"error: POST /api/zones failed (host={resolved}): {exc}"
    if post_result != "ok":
        return f"refused: POST /api/zones refused: {post_result} (host={resolved})"

    try:
        after = zones_http_client.get_zones(resolved)
    except zones_http_client.ZonesHttpError as exc:
        return (f"error: POST /api/zones returned ok, but the confirming re-fetch of GET "
                f"/api/zones failed (host={resolved}): {exc} -- state UNKNOWN, re-check before "
                f"trusting this")

    after_types = after.get("relay_types")
    got = after_types[relay - 1] if isinstance(after_types, list) and len(after_types) >= relay else None
    if got != wanted:
        return (f"FAILED: POST /api/zones returned ok, but read-back does not confirm it "
                f"landed -- {field_name}: wanted {wanted}, board now reports {got!r} "
                f"(host={resolved}). Do not trust this as applied.")

    # The target relay's type is the one expected top-level change: compare
    # against a before-snapshot with it already applied, so every OTHER
    # relay_types cell (and everything else) must be unchanged.
    expected = copy.deepcopy(before)
    expected["relay_types"][relay - 1] = wanted
    collateral = _zone_collateral_diff(expected, after, -1, set())
    if collateral:
        return (f"FAILED: {field_name} landed correctly, but other field(s) changed "
                f"unexpectedly -- {'; '.join(collateral)} (host={resolved}). This tool must touch "
                f"only {field_name}; investigate before trusting this board's config.")

    return (f"ok - {field_name}={got} ({_relay_device_type_name(got)}) "
            f"(was {current!r} ({_relay_device_type_name(current)}); confirmed by read-back; host={resolved})")

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
