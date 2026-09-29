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


def get_status(host: str, timeout: float = WIFI_PROV_HTTP_TIMEOUT_S) -> dict:
    """GET /status and return the full decoded JSON object, straight from
    status_get_handler() -- mode/state/ssid/sta_connected/sta_ip/ap_ssid/
    ap_password/sta_rssi/ap_clients/ip_mode/static_*/ap_password_known/
    ap_password_set/ap_pending_teardown (the last since be7bcad4; absent on
    older firmware -- callers must check with ``"ap_pending_teardown" in
    data``, never assume the key exists)."""
    req = urllib.request.Request(_url(host, "/status"), method="GET")
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            body_text = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:  # noqa: BLE001
        status, detail = _http_error_detail(exc)
        raise WifiProvHttpError(f"GET /status failed: {detail}", status, detail) from exc
    try:
        return json.loads(body_text)
    except Exception as exc:
        raise WifiProvHttpError(f"GET /status response was not valid JSON: {body_text!r}") from exc
