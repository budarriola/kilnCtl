"""NETWORK tools -- the board's STA IP configuration (static IP / DHCP) over
HTTP: ``network_get_ip_config`` (read-only) and ``network_set_ip_config``
(POST /ip_config, ROUTE_TIER_ADMIN). Part of the mcp_server.py split -- see
that module's docstring for the overall map.

Request/response handling lives in wifi_prov_http_client.py (wire contract in
the comment block above ``post_ip_config`` there). The one thing a caller must
know: the POST never reliably returns its "ok" body, because applying the
change forces a Wi-Fi disconnect, so success is decided ONLY by polling
GET /status afterwards. Never prints a credential.
"""
from __future__ import annotations

from typing import Optional

from . import mcp_server as _srv
from . import wifi_prov_http_client as wph


def _fmt(v) -> str:
    return "null (redacted: no admin session)" if v is None else repr(v)


def _describe_status(st: dict) -> str:
    mode = st.get("ip_mode")
    parts = [f"ip_mode={mode!r}", f"sta_connected={st.get('sta_connected')!r}",
             f"sta_ip={_fmt(st.get('sta_ip'))}"]
    if mode == "static" or st.get("static_ip") not in (None, ""):
        parts += [f"static_ip={_fmt(st.get('static_ip'))}",
                  f"static_netmask={_fmt(st.get('static_netmask'))}",
                  f"static_gateway={_fmt(st.get('static_gateway'))}"]
    else:
        parts.append("static fields: " + ("redacted (null: no admin session)"
                                          if st.get("static_ip") is None else "empty (DHCP)"))
    return " ".join(parts)


@_srv._tool()
def network_get_ip_config(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the board's STA IP configuration from GET /status:
    ``ip_mode`` (dhcp/static), the configured static ip/netmask/gateway, the
    current station IP and whether the station is connected.

    ``static_ip``/``static_netmask``/``static_gateway`` are redacted to JSON
    null by the firmware unless the caller holds an admin session (or web
    auth is off); this tool tries one login from ``KILNCTL_WEB_USERNAME``/
    ``KILNCTL_WEB_PASSWORD`` when it sees them redacted, and says so plainly
    if they stay null. ``sta_ip`` is likewise withheld from a non-admin caller
    that arrived over the fallback AP. Never prints a credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import

    resolved = _ota_resolve_host(host)
    try:
        st = wph.get_status_admin(resolved)
    except wph.WifiProvHttpError as exc:
        return f"error: GET /status failed (host={resolved}): {exc}"
    return f"{_describe_status(st)} (host={resolved})"


@_srv._tool()
def network_set_ip_config(mode: str, ip: Optional[str] = None, netmask: Optional[str] = None,
                          gateway: Optional[str] = None, confirm: bool = False,
                          host: Optional[str] = None, verify_timeout_s: float = 90.0) -> str:
    """Switch the board's STA interface between DHCP and a static IP --
    POST /ip_config (wifi_provision_http.c, ROUTE_TIER_ADMIN). ``mode`` is
    ``"dhcp"`` (no other fields) or ``"static"`` (``ip``, ``netmask`` and
    ``gateway`` all required).

    DISRUPTIVE: the change forces a Wi-Fi disconnect/rejoin, so the board
    drops this connection and, for a static ip different from its current
    address, MOVES to the new address -- anything else talking to the old one
    loses it. The result says which address the board is expected at.

    REFUSES unless ``confirm is True`` exactly (not merely truthy), validates
    every field PC-side before any I/O (strict dotted quads, contiguous
    netmask, ip outside the setup AP's 192.168.4.0/24, gateway inside the
    subnet), and refuses while a profile or autotune is running. The firmware
    has no run gate on this route; the refusal is this tool's own, because a
    link drop mid-firing blinds the host.

    The POST never reliably returns its "ok" body (ConnectionReset on static,
    IncompleteRead on dhcp are the normal outcome of a successful change), so a
    reset after the request is treated as EXPECTED, not as failure, and the
    result is decided only by polling GET /status -- at the new address for
    static; at the old address and the UART-reported station IP for dhcp,
    re-resolved on every attempt -- for up to ``verify_timeout_s`` seconds. It
    FAILS LOUD unless ``ip_mode`` and (for static) the three static fields read
    back exactly as requested; static fields that are still redacted (no admin
    session) cannot be verified and fail. A 400/500 from the board is reported
    as a failure with the board's message.

    Credentials come only from the environment (``KILNCTL_WEB_USERNAME``/
    ``KILNCTL_WEB_PASSWORD``), never a parameter, never echoed.
    """
    from .mcp_server_control import _profile_or_autotune_running_reason
    from .mcp_server_ota import _ota_resolve_host, _ota_resolve_host_with_source

    reason = wph.validate_ip_config(mode, ip, netmask, gateway)
    if reason is not None:
        return f"refused: {reason}"
    if confirm is not True:
        return ("refused: pass confirm=True (exactly True) to change the board's IP configuration -- "
                "this disconnects Wi-Fi and may move the board to a different address")
    if isinstance(verify_timeout_s, bool) or not isinstance(verify_timeout_s, (int, float)) \
            or not (5 <= verify_timeout_s <= 600):
        return f"refused: verify_timeout_s={verify_timeout_s!r} must be a number in [5, 600]"

    resolved = _ota_resolve_host(host)

    running_reason = _profile_or_autotune_running_reason()
    if running_reason is not None:
        return (f"refused: {running_reason} -- the IP configuration is not changed mid-run, a Wi-Fi drop "
                f"would blind the host (host={resolved})")

    try:
        before = wph.get_status_admin(resolved)
    except wph.WifiProvHttpError as exc:
        return f"error: GET /status failed before the change (host={resolved}): {exc}"
    before_line = f"before: {_describe_status(before)}"

    current_ip = before.get("sta_ip") or resolved
    if mode == "static":
        if wph._matches(before, "static", ip, netmask, gateway)[0]:
            return (f"ok: already configured, nothing sent -- {before_line} (host={resolved})")
        moves = (ip != current_ip)
        expected = (f"WARNING: static ip {ip} differs from the current address {current_ip}; the board "
                    f"will MOVE to {ip}" if moves else f"static ip equals the current address {current_ip}; no move")
    else:
        if before.get("ip_mode") == "dhcp":
            return f"ok: already DHCP, nothing sent -- {before_line} (host={resolved})"
        expected = ("board will re-lease an address from DHCP; it may change from the current "
                    f"{current_ip} and will be looked up via the UART link")

    try:
        outcome = wph.post_ip_config(resolved, mode, ip, netmask, gateway)
    except wph.WifiProvHttpError as exc:
        return f"failed: {exc}\n{before_line}"
    post_line = {"ok": "POST answered ok",
                 "dropped": "POST connection reset/truncated after sending (expected: Wi-Fi was forced to rejoin)"
                 }.get(outcome, f"POST answered an unexpected body ({outcome})")

    def resolve_hosts():
        if mode == "static":
            return [ip]
        hosts = [resolved]
        try:
            h, src = _ota_resolve_host_with_source(None)
            if src == "STA IP" and h not in hosts:
                hosts.append(h)
        except Exception:  # noqa: BLE001 -- UART may be mid-rejoin; keep polling the old host
            pass
        return hosts

    ok, why = wph.verify_ip_config(resolve_hosts, mode, ip, netmask, gateway, timeout_s=float(verify_timeout_s))
    head = "ok: ip configuration verified" if ok else "FAILED verification: ip configuration not confirmed"
    return f"{head} -- {why}\n{expected}\n{post_line}\n{before_line}"
