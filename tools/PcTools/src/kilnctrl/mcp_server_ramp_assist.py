"""Ramp assist MCP tools -- HTTP, not the UART link.

Exposes ramp_assist_cfg.h's kiln-wide on/off flag (GET/POST /api/ramp_assist,
firmware/KilnFW/App/drivers/http/diagnostics_http.c) through the sanctioned tool
facade -- this repo's standing rule is that AI/tooling must be able to work
with and around a feature like this, not be left to raw HTTP against the
board (see CLAUDE.md). Same "own module, plumbed through mcp_server.py's
star-import list" pattern mcp_server_adaptive_tune.py/mcp_server_ota.py use.
Host discovery mirrors those modules' own resolver exactly (prefer the
board's current station IP via the UART-side WIFI query, fall back to the
fallback-AP address, an explicit ``host`` argument always wins) -- not
imported from either (both helpers are module-private), so it is duplicated
here rather than made public just for a third caller; if a fourth HTTP-tool
module needs it, that is the point to promote it to a shared helper instead
of a third copy.

WHY A GETTER **AND** A SETTER, AND WHY THE SETTER IS CONFIRM-GATED: this flag
decides whether the executor is allowed to silently rewrite a firing's ramp/
dwell shape. A PID tuning run or an A/B controller comparison needs to know
FOR CERTAIN which state the board is in before trusting its own tracking-
error numbers (get), and an automated experiment needs to be able to PIN the
flag to a known state rather than inherit whatever a previous session left
behind (set) -- see run_queue.py's preset-apply code, which does exactly
that. The setter follows the same confirm=True idiom
adaptive_tune_set_enabled()/debug_program() use for a live-board write that
changes what a firing does: no default that quietly turns into a write."""
from __future__ import annotations

from typing import Optional

from . import ramp_assist_http_client as ra_http
from .wifi_uart import WifiUartQueryError

from . import mcp_server_core as _core


def _ramp_assist_resolve_host(host: Optional[str]) -> str:
    """Same resolution order as mcp_server_adaptive_tune.py's
    ``_adaptive_tune_resolve_host()``/mcp_server_ota.py's
    ``_ota_resolve_host()``: explicit ``host`` always wins; otherwise prefer
    the board's current Wi-Fi station IP, else the fallback-AP address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return ra_http.RAMP_ASSIST_AP_DEFAULT_HOST


@_core._tool()
def ramp_assist_get_enabled(host: Optional[str] = None) -> str:
    """GET /api/ramp_assist -- the board's ACTUAL current state of the
    kiln-wide ramp-assist flag (never an assumed default). Read-only, safe
    to call at any time, including during a live firing.

    THIS FLAG IS THE ON/OFF SWITCH ONLY: ramp assist's actual ramp-stretch/
    dwell-credit behaviour is a separate, not-yet-shipped feature that will
    read this flag at decision time. A PID tuning run or an A/B controller
    comparison should check this BEFORE trusting its own tracking-error
    numbers -- an unassisted run silently becoming assisted invalidates every
    measurement it produces.

    `host`: board IP or hostname. Defaults to the board's current station IP
    (via the UART-side wifi_get_status() query) if connected, else the
    fallback-AP address 192.168.4.1.
    """
    resolved = _ramp_assist_resolve_host(host)
    try:
        enabled = ra_http.get_enabled(resolved)
    except ra_http.RampAssistHttpError as exc:
        return f"error: {exc} (host={resolved})"
    return f"ramp_assist enabled={enabled} (host={resolved})"


@_core._tool()
def ramp_assist_set_enabled(enabled: bool, confirm: bool = False, host: Optional[str] = None) -> str:
    """POST /api/ramp_assist -- set the kiln-wide ramp-assist flag. WRITES
    BOARD CONFIG that changes whether the executor may stretch a ramp or
    shorten a dwell during a firing.

    USE THIS TO PIN THE FLAG FOR AN EXPERIMENT, not just to toggle it by
    hand: a PID tuning run or A/B controller comparison must PIN this to a
    known state (normally False/off) rather than inherit whatever the board
    happens to have left over from a previous session -- see run_queue.py's
    preset-apply code, which calls this on every experiment's setup rather
    than assuming the board's current value.

    Refused unless `confirm=True` is passed explicitly -- same idiom
    adaptive_tune_set_enabled()/debug_program() use for a live-board write of
    this consequence; there is no default that becomes a write.

    `enabled`: True to allow the executor to stretch ramps/shorten dwells,
    False (the shipped default) to keep the raw, unassisted schedule.
    `host`: board IP or hostname, same resolution as
    ramp_assist_get_enabled().

    Returns the board's own answer: "ok" on a clean write, or an "error"
    line if the write itself failed. Note that ramp_assist_cfg_set_enabled()
    applies the value live before attempting to persist it -- an ``ok:false``
    response can still mean the flag took effect for the rest of this boot,
    only that it will not survive a reboot; call ramp_assist_get_enabled()
    afterward if that distinction matters to the caller.
    """
    if confirm is not True:
        return (
            "error: ramp_assist enable/disable refused without confirm=True -- this writes board "
            "config that changes whether the executor may stretch ramps/shorten dwells during a "
            "firing (see this tool's own docstring)"
        )
    resolved = _ramp_assist_resolve_host(host)
    try:
        result = ra_http.set_enabled(resolved, enabled)
    except ra_http.RampAssistHttpError as exc:
        return f"error: {exc} (host={resolved})"
    if not result.get("ok"):
        return f"error: board reported failure: {result} (host={resolved})"
    state = "enabled" if enabled else "disabled"
    return f"ok - ramp assist {state} (POST accepted by the board, not read back) (host={resolved})"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
