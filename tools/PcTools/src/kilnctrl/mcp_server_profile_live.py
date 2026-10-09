"""Live profile edit tools -- wraps profiles_live_http.c's five-route HTTP
surface (docs/LIVE_PROFILE_EDIT.md section 10) over
profile_live_http_client.py. See that client module's docstring for the full
wire contract; this module is a thin MCP layer over it: host resolution,
confirm-gating on writes, and echoing the board's own response text.

NOT YET VERIFIED AGAINST REAL HARDWARE -- request construction/response
parsing are unit-tested with mocked HTTP only (see
tools/PcTools/tests/test_profile_live_http_client.py).
"""
from __future__ import annotations

from typing import Optional

from . import profile_live_http_client as profile_live_http
from .wifi_uart import WifiUartQueryError

from . import mcp_server_core as _core


def _profile_live_resolve_host(host: Optional[str]) -> str:
    """Same convention as _ota_resolve_host()/_control_resolve_host(): an
    explicit host always wins, else the board's current station IP (via the
    UART-side WIFI query), else its fallback-AP address."""
    if host:
        return host
    try:
        status = _srv._wifi.get_status()
        if status.sta_connected and status.sta_ip:
            return status.sta_ip
    except WifiUartQueryError:
        pass
    return "192.168.4.1"


@_core._tool()
def profile_live_get(content: bool = False, host: Optional[str] = None) -> str:
    """GET /api/profile/live -- the live-edit status object, or (with
    content=True) GET /api/profile/live?content=1 -- the working copy's
    full profile body (name/zone_mask/segments).

    Status fields: active, origin_id, origin_is_builtin, working_id (-1 if
    no working copy exists yet), editable_from_segment, pending_decision,
    and last_refusal (null, or {"generation","result","message"} -- the
    board's own live-edit-generation-tagged refusal record; there is no
    other generation counter exposed on this surface).

    `host`: board IP/hostname; defaults to the board's current station IP,
    else its fallback-AP address 192.168.4.1.
    """
    resolved = _profile_live_resolve_host(host)
    try:
        if content:
            obj = profile_live_http.get_live_content(resolved)
        else:
            obj = profile_live_http.get_live_status(resolved)
    except profile_live_http.ProfileLiveHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        detail_bit = f" -- {exc.detail}" if exc.detail else ""
        return f"error: {exc}{status_bit}{detail_bit} (host={resolved})"
    return f"ok - {obj} (host={resolved})"


@_core._tool()
def profile_live_fork(confirm: bool = False, host: Optional[str] = None) -> str:
    """POST /api/profile/live/fork -- forks whatever profile is currently
    running into a new working copy that can then be edited in place
    without touching the stored original.

    Refused (409) if nothing is currently running, or if the fork itself
    fails (e.g. no free profile slot).

    Requires confirm=True: on confirm=False, no request is sent to the
    board at all.

    `host`: board IP/hostname; defaults to the board's current station IP,
    else its fallback-AP address 192.168.4.1.
    """
    if not confirm:
        return "error: refused -- confirm=True is required. This forks the running profile into a new working copy."
    resolved = _profile_live_resolve_host(host)
    try:
        obj = profile_live_http.fork_live(resolved)
    except profile_live_http.ProfileLiveHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        detail_bit = f" -- {exc.detail}" if exc.detail else ""
        return f"error: {exc}{status_bit}{detail_bit} (host={resolved})"
    return f"ok - {obj} (host={resolved})"


@_core._tool()
def profile_live_edit(name: str, zone_mask: int, segments: list, confirm: bool = False,
                       host: Optional[str] = None) -> str:
    """POST /api/profile/live -- saves a candidate profile body into the
    working slot (validated HARD, then window-checked against the running
    profile before being accepted).

    `segments`: a list of dicts, one per segment, in execution order. Each
    dict accepts EXACTLY: kind (optional, default 0 = zone ramp/dwell);
    target or target_c, ramp or ramp_c_per_hr, dwell or dwell_min (zone-ramp
    segments); io_target, io_state, io_blocking, io_leave_on or
    io_leave_on_at_end (I/O segments). Both spellings per field are accepted
    so a get_live_content()-shaped dict (which uses target_c/ramp_c_per_hr/
    dwell_min/io_leave_on_at_end) can be read, mutated, and passed straight
    back in without renaming keys. Any other key raises ValueError -- a
    typo or an unrecognized key is never silently dropped.

    NOTE: this route never sends rule%u_* fields, so every live edit sets
    on_off_rule_count=0 on the working copy and erases any existing ON_OFF
    zone rules -- pre-existing firmware/page behaviour, not something this
    tool can route around.

    Refused (400) on a bound violation -- the board's message names the
    offending segment/value/limit -- or on "fork before editing"/"no active
    firing". Refused (409) on a window violation: the candidate would change
    a segment that has already run or is currently running
    (editable_from_segment, from profile_live_get(), names the first
    segment still open to edits).

    Requires confirm=True: on confirm=False, no request is sent to the
    board at all.

    `host`: board IP/hostname; defaults to the board's current station IP,
    else its fallback-AP address 192.168.4.1.
    """
    if not confirm:
        return "error: refused -- confirm=True is required. This overwrites the working copy's content."
    resolved = _profile_live_resolve_host(host)
    try:
        obj = profile_live_http.edit_live(resolved, name, zone_mask, segments)
    except ValueError as exc:
        return f"error: {exc}"
    except profile_live_http.ProfileLiveHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        detail_bit = f" -- {exc.detail}" if exc.detail else ""
        return f"error: {exc}{status_bit}{detail_bit} (host={resolved})"
    return f"ok - {obj} (host={resolved})"


@_core._tool()
def profile_live_decide(action: str, name: Optional[str] = None, confirm: bool = False,
                         host: Optional[str] = None) -> str:
    """POST /api/profile/live/decide -- resolves a pending working copy.

    `action`: one of
      "discard"   -- drop the working copy, changing nothing stored.
      "save_as"   -- save the working copy as a NEW profile slot (requires
                     `name`); returns {"ok":true,"id":<new slot>}.
      "overwrite" -- overwrite the ORIGIN slot in place with the working
                     copy's content; refused (403) if the origin is a
                     builtin profile (builtins are never overwritable).

    Refused (409, any action) if there is no pending working copy at all.

    Requires confirm=True: on confirm=False, no request is sent to the
    board at all -- this is true for EVERY action here, including
    "discard", since discard is still a destructive, irreversible decision
    on the working copy.

    `host`: board IP/hostname; defaults to the board's current station IP,
    else its fallback-AP address 192.168.4.1.
    """
    if not confirm:
        return (f"error: refused -- confirm=True is required for action={action!r}. "
                 "This resolves (and may discard) the pending working copy.")
    if action not in ("discard", "save_as", "overwrite"):
        return f"error: unknown action {action!r} -- must be 'discard', 'save_as', or 'overwrite'"
    if action == "save_as" and not name:
        return "error: action='save_as' requires `name`"
    resolved = _profile_live_resolve_host(host)
    try:
        if action == "discard":
            obj = profile_live_http.decide_live_discard(resolved)
        elif action == "save_as":
            obj = profile_live_http.decide_live_save_as(resolved, name)
        else:
            obj = profile_live_http.decide_live_overwrite(resolved)
    except profile_live_http.ProfileLiveHttpError as exc:
        status_bit = f" (HTTP {exc.status})" if exc.status else ""
        detail_bit = f" -- {exc.detail}" if exc.detail else ""
        return f"error: {exc}{status_bit}{detail_bit} (host={resolved})"
    return f"ok - {obj} (host={resolved})"

# Bound last, on purpose: tool bodies read `_srv` only at call time, and importing the
# aggregate any earlier would let it star-import this module half-initialised
# when this module is imported first (see mcp_server_core.py).
from . import mcp_server as _srv  # noqa: E402
