"""CONTROL tools (task 8).

Part of the mcp_server.py split (pure refactor) -- moved verbatim, no
logic changes. See mcp_server.py's module docstring for the overall map.
"""
from __future__ import annotations

import asyncio
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

from . import mcp_server as _srv


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


@_srv._tool()
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
    )


@_srv._tool()
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


@_srv._tool()
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


