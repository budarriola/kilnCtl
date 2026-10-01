#!/usr/bin/env python3
"""wifi_prov_http_client.py -- thin HTTP client for GET /status
(firmware/KilnFW/App/drivers/http/wifi_provision_http.c's status_get_handler,
ROUTE_TIER_OPEN), the wifi-provisioning status route -- distinct from
dashboard_http_client.py's GET /api/status.

WHY THIS EXISTS. wifi_get_status() (mcp_server_wifi.py) reports Wi-Fi state
over the UART wire protocol (devices_wifi_uart.py's GET_STATUS, task 11) so
it works even with no network path to the board at all -- that is the whole
point of the UART link. But the UART wire format was never extended to carry
`ap_pending_teardown` (be7bcad4, 2026-09-28: true while home Wi-Fi is back up
but the fallback AP is deliberately being kept alive because a session is
logged in) -- only the HTTP /status JSON was. Adding this field to the UART
wire protocol would be a firmware change (wire layout bump, uart_bridge_ext_
wifi.c, devices_wifi_uart.py's decode) well beyond this tool's scope; this
module instead lets wifi_get_status() make an OPTIONAL, best-effort HTTP GET
of the same information when a host is known, alongside its UART read,
same "stdlib urllib.request, no framework" convention as
dashboard_http_client.py/ota_http_client.py/zones_http_client.py.

Only `ap_pending_teardown` is read here. As of be7bcad4, GET /status does
NOT also emit an `ap_fallback_active` field (that flag exists internally in
wifi_prov_internal.h but is not serialized anywhere) -- callers must treat
its absence as "not exposed", not as older firmware.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from typing import Optional

from . import http_auth

WIFI_PROV_HTTP_TIMEOUT_S = 5.0


class WifiProvHttpError(RuntimeError):
    """Raised on transport failure, a non-2xx response, or a response body
    that isn't valid JSON. Mirrors DashboardHttpError/OtaHttpError's shape."""

    def __init__(self, message: str, status: Optional[int] = None, detail: str = ""):
        super().__init__(message)
        self.status = status
        self.detail = detail


def _url(host: str, path: str) -> str:
    return f"http://{host}{path}"


def _http_error_detail(exc: Exception) -> "tuple[Optional[int], str]":
    if isinstance(exc, urllib.error.HTTPError):
        try:
            detail = exc.read().decode("utf-8", errors="replace").strip()
        except Exception:
            detail = ""
        return exc.code, detail
    if isinstance(exc, urllib.error.URLError):
        return None, f"unreachable: {exc.reason}"
    return None, str(exc)


def get_status(host: str, timeout: float = WIFI_PROV_HTTP_TIMEOUT_S, trusted: bool = True) -> dict:
    """GET /status and return the full decoded JSON object, straight from
    status_get_handler() -- mode/state/ssid/sta_connected/sta_ip/ap_ssid/
    ap_password/sta_rssi/ap_clients/ip_mode/static_*/ap_password_known/
    ap_password_set/ap_pending_teardown (the last since be7bcad4; absent on
    older firmware -- callers must check with ``"ap_pending_teardown" in
    data``, never assume the key exists).

    ``trusted=False`` is for a host whose identity is NOT yet established
    (e.g. an address the board is only expected to move to): the request goes
    out with ``no_relogin=True``, so a 401 from whatever answers there can
    never trigger a login and no credential or remembered session cookie is
    ever sent to it."""
    req = urllib.request.Request(_url(host, "/status"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout, no_relogin=not trusted, record=trusted) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise WifiProvHttpError(f"GET /status failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        # Never echo the body: GET /status carries `ap_password` in plaintext
        # to AP-side callers (wifi_provision_http.c's on_ap narrowing).
        raise WifiProvHttpError(f"GET /status response was not valid JSON ({len(body_text)} bytes)") from exc


# ---------------------------------------------------------------------------
# POST /ip_config (static IP / DHCP) -- see mcp_server_network.py.
#
# firmware/KilnFW/App/drivers/http/wifi_provision_http.c's
# ip_config_post_handler(), ROUTE_TIER_ADMIN (route_tier_table.h): form-encoded
# body (max 128 B), field "mode" = "dhcp" | "static"; static additionally needs
# "ip", "netmask", "gateway", each a dotted quad. 400 for a missing/bad field
# or an ip inside 192.168.4.0/24 (the fallback AP's own subnet); 500 if the
# setter fails. No system_mode_gate in the handler. Success body is "ok", BUT
# the change forces a Wi-Fi disconnect/rejoin, so the board usually resets
# the connection before (or while) that body is read -- ConnectionReset on
# static, IncompleteRead on dhcp, seen on the bench 2026-09-30. A caller can
# therefore never treat the POST's own response as the verdict; it must poll
# GET /status (ip_mode + static_* fields, redacted to JSON null unless the
# caller has an admin session or web auth is off) at the address the board
# will have afterwards.
# ---------------------------------------------------------------------------
import http.client  # noqa: E402
import ipaddress  # noqa: E402
import re  # noqa: E402
import time  # noqa: E402
import urllib.parse  # noqa: E402
from typing import Callable, Iterable  # noqa: E402

IP_CONFIG_PATH = "/ip_config"
IP_CONFIG_BODY_MAX = 128  # firmware IP_CONFIG_BODY_MAX
#: The fallback SoftAP's own subnet; the firmware refuses a static ip inside it.
AP_SUBNET = ipaddress.ip_network("192.168.4.0/24")

_QUAD_RE = re.compile(r"^(0|[1-9][0-9]{0,2})(\.(0|[1-9][0-9]{0,2})){3}$")


def _parse_quad(label: str, value) -> "tuple[Optional[ipaddress.IPv4Address], Optional[str]]":
    if not isinstance(value, str) or not _QUAD_RE.match(value):
        return None, f"{label}={value!r} is not a strict dotted-quad IPv4 address"
    try:
        return ipaddress.IPv4Address(value), None
    except ValueError:
        return None, f"{label}={value!r} is not a valid IPv4 address (octet > 255)"


def validate_ip_config(mode, ip=None, netmask=None, gateway=None) -> Optional[str]:
    """PC-side validation, mirroring (and in places stricter than) the
    firmware's own: returns None if acceptable, else a reason string. The
    firmware only checks that each string parses and that ip is outside the
    AP subnet; the extra checks here (contiguous netmask, gateway inside the
    subnet, ip not network/broadcast/gateway) exist because a static config
    that passes the firmware but cannot route strands the board off the LAN."""
    if mode not in ("dhcp", "static"):
        return f"mode={mode!r} must be 'dhcp' or 'static'"
    if mode == "dhcp":
        if any(v is not None for v in (ip, netmask, gateway)):
            return "mode='dhcp' takes no ip/netmask/gateway -- omit them (the board clears its static fields)"
        return None
    if ip is None or netmask is None or gateway is None:
        return "mode='static' requires ip, netmask and gateway"
    ip_a, err = _parse_quad("ip", ip)
    if err:
        return err
    mask_a, err = _parse_quad("netmask", netmask)
    if err:
        return err
    gw_a, err = _parse_quad("gateway", gateway)
    if err:
        return err
    mask_int = int(mask_a)
    inverted = (~mask_int) & 0xFFFFFFFF
    if mask_int == 0 or inverted & (inverted + 1):
        return f"netmask={netmask!r} is not a contiguous, non-zero subnet mask"
    if mask_int == 0xFFFFFFFF:
        return "netmask=255.255.255.255 leaves no room for a gateway"
    net = ipaddress.ip_network(f"{ip}/{netmask}", strict=False)
    if ip_a in AP_SUBNET:
        return f"ip={ip} is inside {AP_SUBNET} (the setup AP's subnet); the firmware refuses it"
    if ip_a.is_unspecified or ip_a.is_loopback or ip_a.is_multicast or ip_a.is_reserved:
        return f"ip={ip} is not a usable unicast address"
    if ip_a == net.network_address or ip_a == net.broadcast_address:
        return f"ip={ip} is the network or broadcast address of {net}"
    if gw_a not in net:
        return f"gateway={gateway} is not inside {net} (ip/netmask)"
    if gw_a == ip_a:
        return "gateway equals ip"
    if gw_a == net.network_address or gw_a == net.broadcast_address:
        return f"gateway={gateway} is the network or broadcast address of {net}"
    return None


def build_ip_config_body(mode: str, ip=None, netmask=None, gateway=None) -> bytes:
    fields = [("mode", mode)]
    if mode == "static":
        fields += [("ip", ip), ("netmask", netmask), ("gateway", gateway)]
    body = urllib.parse.urlencode(fields).encode("ascii")
    if len(body) > IP_CONFIG_BODY_MAX:
        raise WifiProvHttpError(f"ip_config body is {len(body)} B, over the firmware's {IP_CONFIG_BODY_MAX} B cap")
    return body


def _is_expected_drop(exc: BaseException) -> bool:
    """True for the connection teardown a successful /ip_config causes."""
    seen = set()
    cur: Optional[BaseException] = exc
    while cur is not None and id(cur) not in seen:
        seen.add(id(cur))
        if isinstance(cur, (ConnectionResetError, ConnectionAbortedError, BrokenPipeError,
                            http.client.IncompleteRead, http.client.RemoteDisconnected)):
            return True
        if isinstance(cur, urllib.error.URLError) and not isinstance(cur, urllib.error.HTTPError):
            cur = cur.reason if isinstance(cur.reason, BaseException) else cur.__cause__
            continue
        cur = cur.__cause__ or cur.__context__
    return False


def post_ip_config(host: str, mode: str, ip=None, netmask=None, gateway=None,
                   timeout: float = WIFI_PROV_HTTP_TIMEOUT_S) -> str:
    """POST /ip_config. Returns "ok" if the board answered ok, "dropped" if
    the connection was reset/truncated after the request went out (the
    EXPECTED outcome of a successful change -- not proof of one; the caller
    must verify via /status), or "timeout" if the response never arrived
    after the connection was established (ambiguous; verify). Raises WifiProvHttpError for an HTTP error
    status (400/500/401...), for an auth failure, and for any other failure
    (e.g. could not connect: nothing was sent, so nothing changed)."""
    body = build_ip_config_body(mode, ip, netmask, gateway)
    req = urllib.request.Request(
        _url(host, IP_CONFIG_PATH), data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace").strip()
    except urllib.error.HTTPError as exc:
        status, detail = _http_error_detail(exc)
        raise WifiProvHttpError(f"POST {IP_CONFIG_PATH} refused: HTTP {status}: {detail}", status, detail) from exc
    except http_auth.HttpAuthError as exc:
        raise WifiProvHttpError(f"POST {IP_CONFIG_PATH} auth failed: {exc}") from exc
    except Exception as exc:  # noqa: BLE001
        if _is_expected_drop(exc):
            return "dropped"
        # urllib wraps a CONNECT-phase failure in URLError (nothing was sent);
        # a bare TimeoutError means the request went out and the response
        # never came -- the board may well have applied the change and dropped
        # Wi-Fi, so that is AMBIGUOUS (verify decides), not a failure.
        if isinstance(exc, TimeoutError) and not isinstance(exc, urllib.error.URLError):
            return "timeout"
        _, detail = _http_error_detail(exc)
        raise WifiProvHttpError(f"POST {IP_CONFIG_PATH} failed: {detail}") from exc
    return "ok" if text == "ok" else f"unexpected-body:{text[:40]}"


def get_status_admin(host: str, timeout: float = WIFI_PROV_HTTP_TIMEOUT_S,
                     login: Optional[Callable[[str], object]] = None,
                     trusted: bool = True) -> dict:
    """GET /status, and if the static_* fields come back redacted (null)
    because no admin session was presented, log in once (env credentials, via
    :func:`http_auth.login`) and read again. /status is ROUTE_TIER_OPEN, so
    http_auth.urlopen never sees a 401 and would otherwise never log in.
    If no credentials are available or login fails, the first (redacted)
    reading is returned -- callers must treat null static_* as "unknown".

    CREDENTIAL SAFETY: the login only ever happens against a ``trusted`` host
    (caller has established it is the board, e.g. via the UART-reported STA
    IP) whose reply actually looks like this board's /status (carries
    ``ip_mode``) -- a foreign JSON without the keys must not make us send the
    admin credentials to whatever answered. ``trusted=False`` never logs in."""
    data = get_status(host, timeout=timeout, trusted=trusted)
    if data.get("static_ip") is not None or data.get("static_netmask") is not None:
        return data
    if not trusted or "ip_mode" not in data:
        return data
    do_login = login or (lambda origin: http_auth.login(origin, timeout))
    try:
        do_login(f"http://{host}")
    except Exception:  # noqa: BLE001 -- no credential / refused: keep the redacted view
        return data
    try:
        return get_status(host, timeout=timeout, trusted=trusted)
    except WifiProvHttpError:
        return data


def probe_host_answers(host: str, timeout: float = 2.0) -> "tuple[bool, str]":
    """Credential-free check for 'does ANYTHING already answer at host:80?'
    -- used before moving the board onto a static ip to catch an address
    conflict. Goes through http_auth with no_relogin=True, which disables login and
    cookie handling entirely (no credential, no session cookie), and
    record=False so a foreign device that answers is never persisted as the
    default host (host_resolve.record_host_seen).
    Any HTTP response, or a connection REFUSED (a live host with the port
    closed), counts as answering; a timeout or no-route counts as free."""
    req = urllib.request.Request(_url(host, "/status"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout, no_relogin=True, record=False):
            return True, "answered HTTP 200"
    except urllib.error.HTTPError as exc:
        return True, f"answered HTTP {exc.code}"
    except Exception as exc:  # noqa: BLE001
        cur = exc.reason if isinstance(exc, urllib.error.URLError) else exc
        if isinstance(cur, ConnectionRefusedError):
            return True, "connection refused (a host is alive there)"
        return False, f"no answer ({type(cur).__name__})"


def _matches(status: dict, mode: str, ip, netmask, gateway) -> "tuple[bool, str]":
    if status.get("ip_mode") != mode:
        return False, f"ip_mode={status.get('ip_mode')!r}, wanted {mode!r}"
    got = (status.get("static_ip"), status.get("static_netmask"), status.get("static_gateway"))
    if mode == "dhcp":
        # The firmware clears the strings on DHCP; redacted (None) is unverifiable
        # but ip_mode already proved the switch, so accept None or "".
        if any(g not in (None, "") for g in got):
            return False, f"static fields not cleared: {got!r}"
        return True, "ip_mode=dhcp, static fields empty/redacted"
    if any(g is None for g in got):
        return False, "static_* fields are redacted (null) -- no admin session, cannot verify"
    want = (ip, netmask, gateway)
    if got != want:
        return False, f"static fields read {got!r}, wanted {want!r}"
    return True, "ip_mode=static and static_ip/netmask/gateway match"


def verify_ip_config(resolve_hosts: Callable[[], Iterable[str]], mode: str, ip=None, netmask=None,
                     gateway=None, *, is_trusted: Callable[[str], bool],
                     timeout_s: float = 90.0, poll_s: float = 3.0,
                     status_timeout: float = 4.0,
                     sleep: Callable[[float], None] = time.sleep,
                     clock: Callable[[], float] = time.monotonic,
                     get_status_fn: Optional[Callable[..., dict]] = None) -> "tuple[bool, str]":
    """Poll GET /status until ip_mode and static_* read back as requested or
    ``timeout_s`` elapses. ``resolve_hosts`` is called on EVERY attempt (the
    board's address can change between polls -- flash_firmware()'s verify
    re-resolves the same way) and returns the candidate hosts in order.
    Returns (ok, message); a board never reached, or reachable only with
    redacted fields, is a failure, never a pass.

    ``is_trusted(host)`` must say whether the board's identity at that host is
    independently established (the UART-reported STA IP equals it). A host
    that is not trusted is NEVER contacted: no request, no login, no
    credential -- and so can never produce a pass either."""
    fetch = get_status_fn or get_status_admin
    deadline = clock() + timeout_s
    last = "no attempt made"
    while True:
        for host in list(resolve_hosts()):
            try:
                trusted = bool(is_trusted(host))
            except Exception:  # noqa: BLE001 -- UART mid-rejoin: unknown is untrusted
                trusted = False
            if not trusted:
                last = (f"{host}: board identity not confirmed (the UART does not report it as the "
                        f"station address yet); not contacted")
                continue
            try:
                st = fetch(host, timeout=status_timeout)
            except WifiProvHttpError as exc:
                last = f"{host}: {exc}"
                continue
            except Exception as exc:  # noqa: BLE001 -- timeouts/resets mid-rejoin
                last = f"{host}: {type(exc).__name__}"
                continue
            ok, why = _matches(st, mode, ip, netmask, gateway)
            if ok:
                return True, f"verified at {host}: {why}"
            last = f"{host}: {why}"
        if clock() >= deadline:
            return False, f"not verified within {timeout_s:g}s; last: {last}"
        sleep(poll_s)
