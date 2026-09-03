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


@_srv._tool()
def capability_preflight_check(name: str, host: Optional[str] = None,
                                zones_host: Optional[str] = None,
                                safety_host: Optional[str] = None) -> str:
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
    """
    try:
        preset = config_presets.load_preset_data(name)
    except config_presets.ConfigPresetError as exc:
        return f"error: {exc}"
    resolved = _preflight_resolve_host(host)
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
    )
    return report.describe()
