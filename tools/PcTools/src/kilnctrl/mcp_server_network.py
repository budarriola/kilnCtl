"""NETWORK tools -- the board's STA IP configuration (static IP / DHCP) over
HTTP: ``network_get_ip_config`` (read-only) and ``network_set_ip_config``
(POST /ip_config, ROUTE_TIER_ADMIN). Part of the mcp_server.py split -- see
that module's docstring for the overall map.

Request/response handling lives in wifi_prov_http_client.py (wire contract in
the comment block above ``post_ip_config`` there). The one thing a caller must
know: the POST never reliably returns its "ok" body, because applying the
change forces a Wi-Fi disconnect, so success is decided ONLY by polling
GET /status afterwards.

CREDENTIAL SAFETY. This module sends the administrator web credential only to
an address it has independently established is the board: the station IP the
board itself reports over the UART link. The address a static change MOVES the
board to is therefore never contacted with credentials until the UART reports
the board there -- a printer or NAS that already owns that address must never
see the admin login. Never prints a credential.
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


def _uart_sta_ip() -> Optional[str]:
    """The station IP the board reports over the UART link, or None when the
    link is down or the station is not connected. This is the identity anchor:
    an HTTP host is the board only if it equals this value."""
    from .wifi_uart import WifiUartQueryError

    try:
        status = _srv._wifi.get_status()
    except WifiUartQueryError:
        return None
    except Exception:  # noqa: BLE001 -- any UART failure means identity unknown
        return None
    if status.sta_connected and status.sta_ip:
        return status.sta_ip
    return None


def _diagnose(before: dict, old_host: str) -> str:
    """After an unconfirmed change: where is the board, and did it change?"""
    uart_ip = _uart_sta_ip()
    if uart_ip is None:
        return (f"board not located: the UART reports no connected station address and the verification "
                f"never confirmed it at the requested address (previously at {old_host})")
    try:
        st = wph.get_status_admin(uart_ip, timeout=4.0, trusted=True)
    except Exception as exc:  # noqa: BLE001
        return f"UART reports the board at {uart_ip}, but GET /status there failed ({type(exc).__name__})"
    unchanged = all(st.get(k) == before.get(k) for k in
                    ("ip_mode", "static_ip", "static_netmask", "static_gateway"))
    if uart_ip == old_host and unchanged:
        return f"board UNCHANGED, still at {old_host} with the same ip configuration"
    return f"board is at {uart_ip} now ({_describe_status(st)}); previously {old_host}"


@_srv._tool()
def network_get_ip_config(host: Optional[str] = None) -> str:
    """READ-ONLY. Report the board's STA IP configuration from GET /status:
    ``ip_mode`` (dhcp/static), the configured static ip/netmask/gateway, the
    current station IP and whether the station is connected.

    ``static_ip``/``static_netmask``/``static_gateway`` are redacted to JSON
    null by the firmware unless the caller holds an admin session (or web
    auth is off). The tool logs in from ``KILNCTL_WEB_USERNAME``/
    ``KILNCTL_WEB_PASSWORD`` only against the address the UART link reports as
    the board's station IP; an explicit ``host`` that differs is read
    WITHOUT any login (its static fields then stay null). Never prints a
    credential.
    """
    from .mcp_server_ota import _ota_resolve_host  # local import: avoids a circular import

    resolved = _ota_resolve_host(host)
    trusted = resolved == _uart_sta_ip()
    try:
        st = wph.get_status_admin(resolved, trusted=trusted)
    except wph.WifiProvHttpError as exc:
        return f"error: GET /status failed (host={resolved}): {exc}"
    note = "" if trusted else " [no login: host is not confirmed as the board by the UART link]"
    return f"{_describe_status(st)} (host={resolved}){note}"


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
    loses it.

    REFUSES unless ``confirm is True`` exactly (not merely truthy), validates
    every field PC-side before any I/O (strict dotted quads, contiguous
    netmask, ip outside the setup AP's 192.168.4.0/24, gateway inside the
    subnet), refuses while a profile or autotune is running (the firmware has
    no run gate on this route; the refusal is this tool's own, because a link
    drop mid-firing blinds the host), and refuses unless the UART link reports
    a connected station address -- that address is the board's identity, and
    an explicit ``host`` that differs from it is refused (the run gate and the
    verification both read the UART-attached board). For a static ip that
    differs from the current address it also refuses if something already
    answers at that address (an address conflict), probed with no credentials.

    CREDENTIAL SAFETY: the admin credential goes only to an address the UART
    link reports as the board's station IP. The new static address is not
    contacted at all until the UART reports the board there.

    The POST never reliably returns its "ok" body (a reset or truncated read
    after the request is the normal outcome of a successful change, and a read
    timeout after the connection was made is ambiguous, not a failure), so the
    result is decided only by polling GET /status -- at the new address for
    static; at the old address and the UART-reported station IP for dhcp,
    re-resolved every attempt -- for up to ``verify_timeout_s`` seconds. It
    FAILS LOUD unless ``ip_mode`` and (for static) the three static fields read
    back exactly as requested, and on failure says where the board actually is
    (unchanged at the old address, or at a new one per the UART). A 400/500
    from the board, or a failure to connect (nothing sent), is reported as a
    failure with the board's message.

    Credentials come only from the environment (``KILNCTL_WEB_USERNAME``/
    ``KILNCTL_WEB_PASSWORD``), never a parameter, never echoed.
    """
    from .mcp_server_control import _profile_or_autotune_running_reason

    reason = wph.validate_ip_config(mode, ip, netmask, gateway)
    if reason is not None:
        return f"refused: {reason}"
    if confirm is not True:
        return ("refused: pass confirm=True (exactly True) to change the board's IP configuration -- "
                "this disconnects Wi-Fi and may move the board to a different address")
    if isinstance(verify_timeout_s, bool) or not isinstance(verify_timeout_s, (int, float)) \
            or not (5 <= verify_timeout_s <= 600):
        return f"refused: verify_timeout_s={verify_timeout_s!r} must be a number in [5, 600]"

    uart_ip = _uart_sta_ip()
    if uart_ip is None:
        return ("refused: the UART link reports no connected station address, so the board's identity at "
                "any HTTP address cannot be confirmed -- no credential is sent to an unconfirmed host")
    if host is not None and host != uart_ip:
        return (f"refused: host={host!r} differs from the UART-reported station IP {uart_ip} -- the run gate "
                f"and the verification both read the UART-attached board; pass host={uart_ip} or omit it")
    resolved = uart_ip

    running_reason = _profile_or_autotune_running_reason()
    if running_reason is not None:
        return (f"refused: {running_reason} -- the IP configuration is not changed mid-run, a Wi-Fi drop "
                f"would blind the host (host={resolved})")

    if mode == "static" and ip != resolved:
        taken, detail = wph.probe_host_answers(ip)
        if taken:
            return (f"refused: something already answers at the requested static ip {ip} ({detail}) -- "
                    f"an address conflict would strand the board; pick a free address")

    try:
        before = wph.get_status_admin(resolved, trusted=True)
    except wph.WifiProvHttpError as exc:
        return f"error: GET /status failed before the change (host={resolved}): {exc}"
    before_line = f"before: {_describe_status(before)}"

    if mode == "static":
        if wph._matches(before, "static", ip, netmask, gateway)[0]:
            return f"ok: already configured, nothing sent -- {before_line} (host={resolved})"
        expected = (f"static ip {ip} differs from the previous address {resolved}: the board MOVED to {ip}"
                    if ip != resolved else f"static ip equals the previous address {resolved}: no move")
    else:
        if before.get("ip_mode") == "dhcp":
            return f"ok: already DHCP, nothing sent -- {before_line} (host={resolved})"
        expected = f"board re-leased an address from DHCP (previously {resolved})"

    try:
        outcome = wph.post_ip_config(resolved, mode, ip, netmask, gateway)
    except wph.WifiProvHttpError as exc:
        return f"failed: {exc}\n{before_line}"

    def resolve_hosts():
        if mode == "static":
            return [ip]
        hosts = [resolved]
        now = _uart_sta_ip()
        if now is not None and now not in hosts:
            hosts.append(now)
        return hosts

    ok, why = wph.verify_ip_config(resolve_hosts, mode, ip, netmask, gateway,
                                   is_trusted=lambda h: h == _uart_sta_ip(),
                                   timeout_s=float(verify_timeout_s))
    if ok:
        post_line = {"ok": "POST answered ok",
                     "dropped": "POST connection reset/truncated after sending (expected: Wi-Fi was forced to rejoin)",
                     "timeout": "POST response timed out after sending (outcome decided by verification)"
                     }.get(outcome, f"POST answered an unexpected body ({outcome})")
        return f"ok: ip configuration verified -- {why}\n{expected}\n{post_line}\n{before_line}"
    post_line = {"ok": "POST answered ok", "dropped": "POST connection dropped after sending (outcome unconfirmed)",
                 "timeout": "POST response timed out after sending (outcome unconfirmed)"
                 }.get(outcome, f"POST answered an unexpected body ({outcome})")
    where = _diagnose(before, resolved)
    requested = f"requested: {mode}" + (f" ip={ip} netmask={netmask} gateway={gateway}" if mode == "static" else "")
    return (f"FAILED verification: ip configuration NOT confirmed -- {why}\n{requested}\n{where}\n"
            f"{post_line}\n{before_line}")
