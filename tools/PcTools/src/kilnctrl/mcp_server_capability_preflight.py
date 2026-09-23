"""Capability preflight MCP tool -- exposes capability_preflight.py through
the sanctioned tool facade, same "own module, plumbed through mcp_server.py's
star-import list" pattern mcp_server_ramp_assist.py/mcp_server_config_presets.py
use. See capability_preflight.py's module docstring for WHY this exists (the
906d026 ramp_assist_enabled incident) and how fatal/benign is decided.

This is the automation-reachable half of constraint 5: a scripted run
(run_queue.py, or any future orchestrator) can call this exact tool -- not
just an interactive operator -- before committing to a long unattended
firing."""
from __future__ import annotations

from typing import Optional

from . import capability_preflight, config_presets

from . import mcp_server as _srv


def _preflight_resolve_host(host: Optional[str]) -> str:
    """Same resolution order as mcp_server_ramp_assist.py's
    ``_ramp_assist_resolve_host()``/mcp_server_ota.py's
    ``_ota_resolve_host()``: explicit ``host`` always wins; otherwise prefer
    the board's current Wi-Fi station IP, else the fallback-AP address."""
    from .wifi_uart import WifiUartQueryError

    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return capability_preflight.PREFLIGHT_AP_DEFAULT_HOST


def _preflight_task_liveness():
    """Compute a task_liveness.TaskLivenessReport off the LINK (not HTTP --
    capability_preflight.py itself has no link access), same
    get_stack_margin() path as the standalone check_task_liveness tool.
    Returns None (not checked) if the link/UART query fails, e.g. no board
    connected on this host, rather than treating "could not check" as
    "confirmed absent" -- an operator running preflight for a board
    reachable only over Wi-Fi HTTP without a live serial link should not be
    refused over a check this preflight cannot perform."""
    import os

    from . import task_liveness
    from .info import InfoQueryError

    try:
        entries = _srv._info.get_stack_margin()
    except InfoQueryError:
        return None
    repo_root = os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
    )
    script_path = task_liveness.default_check_script_path(repo_root)
    try:
        expected = task_liveness.load_required_task_names(script_path)
    except (task_liveness.TaskLivenessParseError, OSError):
        return None
    return task_liveness.check_task_liveness(entries, expected)


@_srv._tool()
def capability_preflight_check(name: str, host: Optional[str] = None,
                                zones_host: Optional[str] = None,
                                safety_host: Optional[str] = None,
                                allow_missing_tasks: bool = False) -> str:
    """Preflight a config preset against the LIVE board's actual firmware
    capabilities, BEFORE applying it -- read-only, safe to call at any time,
    never writes anything.

    Reports, for every HTTP-gated capability this preset's apply path would
    reach: present/missing, and for a missing one, whether it is FATAL (the
    preset pins a state this firmware cannot provide -- e.g.
    ramp_assist_enabled=true on firmware built before /api/ramp_assist
    existed) or BENIGN (the pinned state is what firmware lacking the
    feature already does by default -- e.g. ramp_assist_enabled=false).

    USE THIS BEFORE `load_config_preset` on any board whose firmware
    provenance is not certain -- this is exactly the check that would have
    caught the 906d026 incident (a required preset field whose apply path
    needed an endpoint the flashed firmware predated) before a multi-hour
    unattended campaign started instead of mid-run.

    `name`: preset name, same as `load_config_preset`.
    `host`: board IP/hostname for GET /api/status (firmware identity) and
    for capability probes not overridden by `zones_host`/`safety_host`
    below. Same resolution as `ramp_assist_get_enabled`: explicit host wins,
    else the board's current station IP, else the fallback-AP address.
    `zones_host`/`safety_host`: pass these to match whatever you plan to
    pass to `load_config_preset` -- a capability gated on `zones_host` (like
    ramp_assist) is only reported as required when a zones host is given
    here too, mirroring apply_preset()'s own gating exactly.

    Also refuses (same as an unacknowledged crash report) if the live
    `check_task_liveness` cross-check finds a required task dead (task
    creation failed this boot) or absent (never registered) -- unless
    `allow_missing_tasks=True`. That check runs over the direct UART link,
    not `host`; if no link/board answers it, task liveness is simply not
    checked (never treated as a failure), so a preflight against a board
    reachable only by Wi-Fi HTTP still works.
    """
    try:
        preset = config_presets.load_preset_data(name)
    except config_presets.ConfigPresetError as exc:
        return f"error: {exc}"
    resolved = _preflight_resolve_host(host)
    task_liveness_report = _preflight_task_liveness()
    # Mirrors load_config_preset()'s own default: zones_host/safety_host
    # omitted means that write path is not attempted, so a capability
    # gated on it (like ramp_assist) is correctly reported as not required
    # -- resolving it to `resolved` regardless of whether it was passed
    # would over-report requirements the actual apply call would not reach.
    resolved_zones = _preflight_resolve_host(zones_host) if zones_host else None
    resolved_safety = _preflight_resolve_host(safety_host) if safety_host else None
    report = capability_preflight.run_preflight(
        preset, resolved, zones_host=resolved_zones, safety_host=resolved_safety,
        preset_name=name,
        task_liveness=task_liveness_report,
        allow_missing_tasks=allow_missing_tasks,
    )
    return report.describe()
