"""PROFILES tools (task 9).

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
import subprocess
import sys
import threading
import time
from collections import deque
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
# PROFILES (task 9) -- fire profile CRUD + execution control, mirrors
# profiles_http.c / dashboard_http.c's /api/profile_exec*
#
# start()/stop()/pause()/resume() are the tools that actually begin, end or
# hold a firing. They carry no extra gate here beyond what the firmware
# itself enforces (relay_authority_on_blocked(), the safety-link liveness
# rule once M6 lands) -- this is not a second, weaker control path, it is the
# same one the GUI and the HTTP dashboard already use.
# ---------------------------------------------------------------------------
@_core._tool()
def profiles_list() -> str:
    """List every profile on the board: id, name, zone_mask, segment count.

    Covers both id spaces -- the 100 writable user slots (ids 0-99) and the
    read-only firing schedules that ship in flash (ids 128+). The listing is
    paged over the link because the full catalogue does not fit in one frame;
    that is handled here. Schedules the user has hidden are not listed.
    """
    try:
        summaries = _srv._profiles.list_all()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if not summaries:
        return "no saved profiles"
    user = [s for s in summaries if not s.builtin]
    builtin = [s for s in summaries if s.builtin]

    def fmt(s) -> str:
        tag = " [built-in, read-only]" if s.builtin else ""
        return (
            f"#{s.id} {s.name!r} zone_mask=0x{s.zone_mask:X} "
            f"segments={s.segment_count}{tag}"
        )

    lines = [fmt(s) for s in user] or ["(no user profiles saved)"]
    if builtin:
        lines.append(f"-- {len(builtin)} built-in schedule(s) --")
        lines.extend(fmt(s) for s in builtin)
    return "\n".join(lines)


@_core._tool()
def profiles_get(profile_id: int) -> str:
    """Read one profile's full segment list (target_c, ramp_c_per_hr, dwell_min per segment).

    ``profile_id`` is either a writable user slot (0-99) or one of the
    read-only firing schedules shipped in flash (ids 128 and up -- see
    profiles_list). A built-in reports the zone mask it would run with, taken
    from the configured zones.
    """
    try:
        detail = _srv._profiles.get(profile_id)
    except (ProfilesQueryError, ValueError) as exc:
        return f"error: {exc}"
    if detail is None:
        return f"no such profile #{profile_id}"
    tag = " [built-in, read-only]" if detail.builtin else ""
    lines = [f"#{detail.id} {detail.name!r} zone_mask=0x{detail.zone_mask:X}{tag}"]
    for i, seg in enumerate(detail.segments):
        lines.append(
            f"  segment {i}: target={seg.target_c:.1f}C ramp={seg.ramp_c_per_hr:.1f}C/hr "
            f"dwell={seg.dwell_min}min"
        )
    return "\n".join(lines)


@_core._tool()
def profiles_save(
    profile_id: int, name: str, zone_mask: int, segments_json: str,
    host: Optional[str] = None, allow_strip: bool = False,
) -> str:
    """Create (profile_id=-1) or overwrite a user profile (slots 0-99).

    ``segments_json`` is a JSON array of
    ``{"target_c": .., "ramp_c_per_hr": .., "dwell_min": ..}`` objects, one per
    segment, in firing order.

    Passing a built-in id (128+) does NOT overwrite the shipped schedule --
    those are read-only flash -- it saves the submitted profile as a copy into
    the first free user slot. The reply names the slot it landed in.

    This UART path carries only plain segments: overwriting a slot that holds
    aux/on-off rules or relay-IO segments would silently STRIP them (the
    save rebuilds the profile from the submitted segments alone). The existing
    slot is read over HTTP first and the save is REFUSED if it holds either,
    or if it cannot be read (fail closed); allow_strip=True (exactly True)
    overrides. After a successful save the slot is read back over UART and the
    name and segment count compared.

    Not confirm-gated by design: saving a profile is a routine, recoverable edit; the board validates and locks it.
    """
    try:
        raw_segments = json.loads(segments_json)
        if not isinstance(raw_segments, list):
            return "error: segments_json must be a JSON array"
        segments = [
            devices.ProfileSegment(
                target_c=float(s["target_c"]),
                ramp_c_per_hr=float(s["ramp_c_per_hr"]),
                dwell_min=int(s["dwell_min"]),
            )
            for s in raw_segments
        ]
    except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
        return f"error: could not parse segments_json: {exc}"
    pid = PROFILES_SAVE_ID_NEW if profile_id < 0 else profile_id
    if pid != PROFILES_SAVE_ID_NEW and not profile_id_is_builtin(pid) and allow_strip is not True:
        refusal = _profile_slot_strip_refusal(pid, host)
        if refusal is not None:
            return refusal
    try:
        result = _srv._profiles.save(pid, name, zone_mask, segments)
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if not result.ok:
        return f"refused: {result.error}"
    suffix = f", {result.warning_count} warning(s)" if result.warning_count else ""
    try:
        got = _srv._profiles.get(result.id)
    except Exception:  # noqa: BLE001
        got = None
    if got is None:
        return (f"FAILED - board reported ok for #{result.id} but the read-back failed; "
                "slot content UNVERIFIED")
    got_mask = getattr(got, "zone_mask", None)
    def _seg_differs(a, b) -> bool:
        return (abs(a.target_c - b.target_c) > 1.0 or abs(a.ramp_c_per_hr - b.ramp_c_per_hr) > 1.0
                or int(a.dwell_min) != int(b.dwell_min))

    if got.name != name or len(got.segments) != len(segments) or (
            got_mask is not None and got_mask != zone_mask) or any(
            _seg_differs(a, b) for a, b in zip(got.segments, segments)):
        return (f"FAILED - board reported ok for #{result.id} but read-back differs "
                f"(name {got.name!r}, {len(got.segments)} segment(s); wanted {name!r}, {len(segments)})")
    return f"ok - saved as #{result.id}{suffix}, read back verified"


def _profile_slot_strip_refusal(pid: int, host: Optional[str]) -> Optional[str]:
    """Refusal text if overwriting user slot ``pid`` over UART would strip aux
    rules / relay-IO segments (or the slot cannot be inspected), else None."""
    from . import aux_http_client as ahc  # local: keeps module import light
    from .mcp_server_ota import _ota_resolve_host  # local: circular, same convention as mcp_server_aux
    try:
        resolved = _ota_resolve_host(host)
        detail = ahc._get_json(resolved, f"/api/profile?id={pid}", ahc.AUX_HTTP_TIMEOUT_S)
    except ahc.AuxHttpError as exc:
        if exc.status == 404:
            return None  # empty slot: nothing to strip
        return (f"refused: could not read existing slot #{pid} over HTTP ({exc}) to check for aux rules / "
                "relay-IO segments that this UART save would strip; pass allow_strip=True to save anyway")
    except Exception as exc:  # noqa: BLE001 - host unresolved etc: fail closed
        return (f"refused: could not inspect slot #{pid} ({type(exc).__name__}: {exc}); "
                "pass allow_strip=True to save anyway")
    if not isinstance(detail, dict):
        return f"refused: slot #{pid} read-back was not an object; pass allow_strip=True to save anyway"
    rules = detail.get("on_off_rules")
    segs = detail.get("segments")
    relay_io = [i + 1 for i, sg in enumerate(segs if isinstance(segs, list) else [])
                if isinstance(sg, dict) and sg.get("seg_kind") == 1]
    if (isinstance(rules, list) and rules) or relay_io:
        return (f"refused: slot #{pid} holds {len(rules) if isinstance(rules, list) else 0} aux/on-off rule(s) "
                f"and relay-IO segment(s) {relay_io}; a UART save rebuilds the profile from plain segments and "
                "would STRIP them. Edit via the web UI / POST /api/profile, or pass allow_strip=True.")
    return None


@_core._tool()
def profiles_delete(profile_id: int) -> str:
    """Delete a saved user profile (slots 0-99). Refused if it is the one currently running.

    Built-in schedules (ids 128+) cannot be deleted -- they are const data in
    flash. Hide one instead, via the web UI / POST /api/profile/builtin/hide,
    which is reversible.

    Not confirm-gated by design: deleting a user profile slot is a routine edit; builtins cannot be deleted.
    """
    if profile_id_is_builtin(profile_id):
        return (
            f"refused - #{profile_id} is a built-in read-only schedule and cannot be "
            f"deleted; hide it instead (web UI / POST /api/profile/builtin/hide)"
        )
    try:
        result = _srv._profiles.delete(profile_id)
    except (ProfilesQueryError, ValueError) as exc:
        return f"error: {exc}"
    if result.ok:
        return f"ok - deleted #{profile_id}"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not delete #{profile_id}{detail}"


@_core._tool()
def profiles_get_exec_status(host: Optional[str] = None) -> str:
    """Read the current (or last) run's state: which profile, which segment,
    dwell/ramp state, per-zone actuals and any fault. While running/paused it
    also reads pause_reason over HTTP (`host` overrides the board address)."""
    try:
        st = _srv._profiles.get_exec_status()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    lines = [
        f"state={st.state} profile=#{st.profile_id} {st.name!r} "
        f"segment={st.segment_index}/{st.segment_count} dwelling={st.dwelling} "
        f"target={st.target_c:.1f}C elapsed={st.segment_elapsed_s}s "
        f"dwell_remaining={st.dwell_remaining_s}s ramp_lock={st.ramp_lock_held} "
        f"fault_guard={st.fault_guard}"
    ]
    for z in st.zones:
        lines.append(
            f"  zone {z.zone}: mode={z.control_mode} "
            f"actual={z.actual_c:.1f}C {'(valid)' if z.actual_valid else '(invalid)'} "
            f"duty={z.duty:.2f} relay={'on' if z.relay_commanded_on else 'off'} "
            f"faulted={z.faulted}"
        )
    if st.state in (1, 2):  # running / paused: pause_reason exists only in the HTTP JSON
        lines.append(_pause_reason_line(host))
    return "\n".join(lines)


def _pause_reason_line(host: Optional[str]) -> str:
    """Best-effort pause_reason from GET /api/profile_exec (never raises, never blocks long)."""
    from . import run_queue
    from .pause_reason import pause_reason_text
    try:
        resolved = host
        if not resolved:
            status = _srv._wifi.get_status()
            resolved = status.sta_ip if (status.sta_connected and status.sta_ip) else "192.168.4.1"
        data = run_queue.get_exec(resolved, 3.0)
    except Exception as exc:  # noqa: BLE001
        return f"pause_reason: unavailable ({type(exc).__name__})"
    text = pause_reason_text(data.get("pause_reason"))
    return f"pause_reason: {data.get('pause_reason')} -- {text}" if text else "pause_reason: none"


@_core._tool()
def profiles_start(profile_id: int) -> str:
    """Start firing a profile. This is the tool that turns on heat --
    same interlocks as the GUI/HTTP start button, nothing weaker.

    ``profile_id`` is a user slot (0-99) or one of the read-only schedules
    shipped in flash (ids 128+); both run the same way. A built-in fires every
    configured zone, since the catalogue itself is zone-agnostic.

    Not confirm-gated by design: starting a firing is the routine purpose of this tool and the operator-visible start path; the board gates it on readiness and the safety processor.
    """
    try:
        result = _srv._profiles.start(profile_id)
    except (ProfilesQueryError, ValueError) as exc:
        return f"error: {exc}"
    if not result.ok:
        return f"refused: {result.error}"
    return f"ok - firing #{profile_id}"


@_core._tool()
def profiles_stop() -> str:
    """Stop the current firing. Relays off."""
    try:
        result = _srv._profiles.stop()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - stopped"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not stop{detail or ' (board gave no reason; it may have nothing running)'}"


@_core._tool()
def profiles_pause() -> str:
    """Pause the current firing (holds state; does not turn off heat outright)."""
    try:
        result = _srv._profiles.pause()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - paused"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not pause{detail or ' (board gave no reason; it may have nothing running)'}"


@_core._tool()
def profiles_resume() -> str:
    """Resume a paused firing."""
    try:
        result = _srv._profiles.resume()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - resumed"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - could not resume{detail or ' (board gave no reason; it may have nothing paused)'}"


@_core._tool()
def profiles_ack_last_run() -> str:
    """Acknowledge the last completed/faulted run, clearing it so a new one can start."""
    try:
        result = _srv._profiles.ack_last_run()
    except ProfilesQueryError as exc:
        return f"error: {exc}"
    if result.ok:
        return "ok - acknowledged"
    detail = f": {result.reason}" if result.reason else ""
    return f"refused - nothing to acknowledge{detail}"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
