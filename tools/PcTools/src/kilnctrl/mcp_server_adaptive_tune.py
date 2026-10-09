"""Adaptive-tune (Phase 7d) MCP tools -- HTTP, not the UART link.

Closes a tooling gap: adaptive_tune_http.c's GET /api/adaptive_tune / POST
/api/adaptive_tune/enable were reachable only by raw HTTP against the board,
bypassing the sanctioned tool facade entirely (this repo's standing rule is
to fix a tooling gap like that, not work around it -- see CLAUDE.md).

Same "own module, plumbed through mcp_server.py's star-import list" pattern
mcp_server_ota.py/mcp_server_control.py use. Host discovery mirrors
mcp_server_ota.py's ``_ota_resolve_host()`` exactly (prefer the board's
current station IP via the UART-side WIFI query, fall back to the
fallback-AP address, an explicit ``host`` argument always wins) -- not
imported from there because that helper is module-private
(leading-underscore names are not carried by ``from .mcp_server_ota import
*``), so it is duplicated here rather than made public just for one more
caller; if a third HTTP-tool module needs it too, that is the point to
promote it to a shared helper instead of a third copy.

Built against firmware/KilnFW/App/drivers/http/adaptive_tune_http.c/.h and
adaptive_tune.h as they stood 2026-09-01. That surface is actively changing
under a different agent's work (a revert endpoint is being added, and the
opt-in flag is being moved off this module's own NVS namespace into the
zone config blob) -- see adaptive_tune_http_client.py's own module docstring
for how this client's GET parsing tolerates that.

SAFETY GATING for the one write this module exposes
(``adaptive_tune_set_enabled``): adaptive_tune_run_end() only ever applies a
learned gain change at a firing's run-end boundary, after the zone's relays
are already off -- never mid-firing -- so flipping the opt-in flag cannot
itself bump a live control loop. It DOES decide what happens the NEXT time a
run ends, though, including a run already in progress right now. That is
exactly the "flip it on by accident during a firing" hazard the task brief
calls out, so this tool follows the same idiom
``debug_program()``/``debug_write_memory()`` (mcp_server_debug.py) already
use for a live-board write: refused unless ``confirm=True`` is passed
explicitly, no default that quietly turns into True.
"""
from __future__ import annotations

from typing import Optional

from . import adaptive_tune_http_client as at_http
from .wifi_uart import WifiUartQueryError

from . import mcp_server_core as _core


def _adaptive_tune_resolve_host(host: Optional[str]) -> str:
    """Same resolution order as mcp_server_ota.py's ``_ota_resolve_host()``:
    explicit ``host`` always wins; otherwise prefer the board's current
    Wi-Fi station IP (works even with the isolated UART link's own picture
    of the world being all this process has), else the fallback-AP
    address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return at_http.ADAPTIVE_TUNE_AP_DEFAULT_HOST


def _format_zone_status(z: "at_http.AdaptiveTuneZoneStatus") -> str:
    lines = [
        f"zone {z.zone}: enabled={z.enabled} observations={z.observation_count} "
        f"(lifetime={z.observations_lifetime})",
    ]
    if z.has_applied:
        lines.append(
            f"  last applied: k_dc {z.prior_k_dc:.4f} -> {z.applied_k_dc:.4f} "
            f"({z.delta_pct:+.2f}%), profile={z.last_applied_profile_id}, "
            f"unix_s={z.last_applied_unix_s}"
        )
    else:
        lines.append("  no refinement applied yet this boot")
    if z.refusal:
        lines.append(f"  last refusal: {z.refusal}")
    lines.append(
        f"  coupled: joint_observations={z.joint_observations} "
        f"attempted={z.coupled_attempted} applied={z.coupled_applied} "
        f"cells_changed={z.coupled_cells_changed}"
    )
    if z.coupled_refusal:
        lines.append(f"    coupled refusal: {z.coupled_refusal}")
    lines.append(
        f"  ki: verdict={z.ki_verdict_name} correction_pct={z.ki_correction_pct:+.2f} "
        f"applied={z.ki_applied}"
    )
    if z.ki_refusal:
        lines.append(f"    ki refusal: {z.ki_refusal}")
    lines.append(f"  revert available: {z.revert_available}")
    return "\n".join(lines)


@_core._tool()
def adaptive_tune_get_status(host: Optional[str] = None) -> str:
    """GET /api/adaptive_tune -- every zone's Phase 7d continuous/adaptive
    PID-tuning status: per-zone opt-in (enabled), how many settled-dwell
    observations are held, whether a refinement has ever been applied this
    boot and what it changed (prior/applied K_dc, delta_pct), the most
    recent refusal reason if the last run-end attempt did not apply, plus
    the fuller coupled-identification (joint_observations,
    coupled_attempted/applied/cells_changed, coupled_refusal) and integral
    (Ki) diagnosis (ki_verdict, ki_correction_pct, ki_applied, ki_refusal)
    fields PID_EXPANSION_PLAN.md 3.3 layer 2 added.

    Read-only -- safe to call at any time, including during a live firing.

    `host`: board IP or hostname. Defaults to the board's current station IP
    (via the UART-side wifi_get_status() query) if connected, else the
    fallback-AP address 192.168.4.1.
    """
    resolved = _adaptive_tune_resolve_host(host)
    try:
        zones = at_http.get_status(resolved)
    except at_http.AdaptiveTuneHttpError as exc:
        return f"error: {exc} (host={resolved})"
    if not zones:
        return f"ok - no zones reported (host={resolved})"
    return f"host={resolved}\n" + "\n".join(_format_zone_status(z) for z in zones)


@_core._tool()
def adaptive_tune_set_enabled(zone: int, enabled: bool, confirm: bool = False,
                               host: Optional[str] = None) -> str:
    """POST /api/adaptive_tune/enable -- opt a zone in or out of Phase 7d's
    continuous/adaptive PID tuning. WRITES BOARD CONFIG.

    DANGER: adaptive_tune_run_end() applies any learned gain change at the
    END of a firing, after that zone's relays are already off -- so this
    flag can never disturb a running control loop the instant it is set.
    But it DOES decide whether the run-end that is coming next (including
    one already underway right now) quietly rewrites this zone's PID gains.
    Flipping this on during an unattended or in-progress firing is exactly
    the accidental-trigger hazard this repo's danger-zone convention exists
    to prevent (see debug_program()/debug_write_memory() in
    mcp_server_debug.py for the same idiom) -- so this call is REFUSED
    unless `confirm=True` is passed explicitly. There is no default that
    becomes a write; a caller (human or agent) must make that choice on
    every call.

    `zone`: 0-based zone index. `enabled`: True to opt in, False to opt out.
    `host`: board IP or hostname, same resolution as
    adaptive_tune_get_status().

    Returns the board's own answer: "ok" on a clean write, or a "warning"
    line if the flag took effect live but its NVS save failed (same
    "applied live, logged if the save failed" convention
    adaptive_tune_set_enabled() (firmware) documents) -- the live value is
    already in effect either way; only persistence across a reboot is in
    question when that warning appears.
    """
    if not confirm:
        return (
            "error: adaptive-tune enable/disable refused without confirm=True -- "
            "this writes board config that changes what a firing's run-end does "
            "to this zone's PID gains (see this tool's own docstring)"
        )
    resolved = _adaptive_tune_resolve_host(host)
    try:
        result = at_http.set_enabled(resolved, zone, enabled)
    except at_http.AdaptiveTuneHttpError as exc:
        return f"error: {exc} (host={resolved})"
    if not result.get("ok"):
        return f"error: board reported failure: {result} (host={resolved})"
    warning = result.get("warning")
    state = "enabled" if enabled else "disabled"
    if warning:
        return f"ok - zone {zone} adaptive tuning {state} (host={resolved}) -- WARNING: {warning}"
    return f"ok - zone {zone} adaptive tuning {state}, confirmed by the board (host={resolved})"


@_core._tool()
def adaptive_tune_revert(zone: int, confirm: bool = False, host: Optional[str] = None) -> str:
    """POST /api/adaptive_tune/revert -- U1's one-click revert: undoes this
    zone's last APPLIED adaptive-tune refinement (the change
    adaptive_tune_get_status()'s prior_k_dc/applied_k_dc/delta_pct
    describe), writing the PID gains back through the same
    zones_config_set_*() path the refinement itself used. WRITES BOARD
    CONFIG -- same class of action as adaptive_tune_set_enabled() above,
    just in the opposite direction, and gated the same way: refused unless
    `confirm=True` is passed explicitly, for the identical reason (a gain
    rewrite an operator did not deliberately ask for, this time undoing one
    instead of enabling future ones).

    Only meaningful when adaptive_tune_get_status() reports
    `revert_available=True` for this zone; calling it otherwise is answered
    by the board's own refusal reason (e.g. nothing to revert), not a client
    -side guess here.

    `zone`: 0-based zone index. `host`: board IP or hostname, same
    resolution as adaptive_tune_get_status()/adaptive_tune_set_enabled().
    """
    if not confirm:
        return (
            "error: adaptive-tune revert refused without confirm=True -- "
            "this writes board config, rewriting this zone's PID gains back "
            "to their pre-refinement values (see this tool's own docstring)"
        )
    resolved = _adaptive_tune_resolve_host(host)
    try:
        result = at_http.revert(resolved, zone)
    except at_http.AdaptiveTuneHttpError as exc:
        return f"error: {exc} (host={resolved})"
    if not result.get("ok"):
        return f"error: board reported failure: {result.get('reason', result)} (host={resolved})"
    return f"ok - zone {zone} adaptive-tune refinement reverted (host={resolved})"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
