"""WIFI_UART wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403
from .devices_common import _decode_ok_reason  # noqa: F401


# ---------------------------------------------------------------------------
# WIFI (UART) -- status/scan/provision/forget, mirrors wifi_provision_http.c
# (task_id = UART_TASK_ID_WIFI)
#
# Exists so Wi-Fi can be provisioned over a link that works even when Wi-Fi
# itself is down or unconfigured -- never touches kiln_io/relay_authority/
# safety_link.
# ---------------------------------------------------------------------------
class WifiUartResponseError(ValueError):
    """Raised when a WIFI (UART) response payload does not match its wire layout."""


@dataclass(frozen=True)
class UartWifiStatus:
    mode: int
    state: int
    sta_connected: bool
    ssid: str
    ap_ssid: str
    ap_password: str
    sta_ip: str
    sta_rssi: int
    ap_clients: int

    @property
    def mode_name(self) -> str:
        return "ap" if self.mode == WIFI_MODE_AP else "home"


@dataclass(frozen=True)
class UartWifiScanEntry:
    ssid: str
    rssi: int
    secure: bool


@dataclass(frozen=True)
class UartWifiSavedNetwork:
    ssid: str
    saved: bool
    in_range: bool
    rssi: int
    secure: bool
    connected: bool


def wifi_uart_get_status() -> bytes:
    """0x01 GET_STATUS request (query): no args."""
    return struct.pack("<B", WIFI_CMD_GET_STATUS)


def wifi_uart_scan() -> bytes:
    """0x02 SCAN request (query): no args. Capped at 6 entries + truncated flag."""
    return struct.pack("<B", WIFI_CMD_SCAN)


def wifi_uart_add_network(ssid: str, password: str = "") -> bytes:
    """0x03 ADD_NETWORK: ssid_len+ssid, password_len+password. Replies ok/fail."""
    return (
        struct.pack("<B", WIFI_CMD_ADD_NETWORK)
        + _pack_str8(ssid, 32, "ssid")
        + _pack_str8(password, 64, "password")
    )


def wifi_uart_set_mode(mode: int) -> bytes:
    """0x04 SET_MODE: mode(0=home,1=ap). Replies ok/fail."""
    if mode not in (WIFI_MODE_HOME, WIFI_MODE_AP):
        raise ValueError(f"mode must be 0 (home) or 1 (ap), got {mode}")
    return struct.pack("<BB", WIFI_CMD_SET_MODE, mode)


def wifi_uart_set_ap_identity(
    ap_ssid: "Optional[str]" = None, ap_password: "Optional[str]" = None
) -> bytes:
    """0x05 SET_AP_IDENTITY: has_ssid[+ssid], has_password[+password].

    Either field may be omitted (None) to leave it unchanged. Replies ok/fail.
    """
    body = struct.pack("<B", WIFI_CMD_SET_AP_IDENTITY)
    if ap_ssid is not None:
        body += struct.pack("<B", 1) + _pack_str8(ap_ssid, 32, "ap_ssid")
    else:
        body += struct.pack("<B", 0)
    if ap_password is not None:
        body += struct.pack("<B", 1) + _pack_str8(ap_password, 63, "ap_password")
    else:
        body += struct.pack("<B", 0)
    return body


def wifi_uart_get_networks() -> bytes:
    """0x06 GET_NETWORKS request (query): no args. Capped at 5 entries."""
    return struct.pack("<B", WIFI_CMD_GET_NETWORKS)


def wifi_uart_forget(ssid: str) -> bytes:
    """0x07 FORGET: ssid_len+ssid. Replies ok/fail."""
    return struct.pack("<B", WIFI_CMD_FORGET) + _pack_str8(ssid, 32, "ssid")


def _unpack_str8(payload: bytes, offset: int, name: str) -> "tuple[str, int]":
    if offset >= len(payload):
        raise WifiUartResponseError(f"{name}: length byte out of range")
    length = payload[offset]
    start = offset + 1
    end = start + length
    if end > len(payload):
        raise WifiUartResponseError(f"{name}: {length}-byte string overruns payload")
    return payload[start:end].decode("ascii", errors="replace"), end


def parse_wifi_uart_response(payload: bytes) -> "tuple[int, object]":
    """Decode a WIFI (UART) reply into ``(subcmd, value)``."""
    if len(payload) < 1:
        raise WifiUartResponseError("WIFI response is empty")
    subcommand = payload[0]

    if subcommand == WIFI_CMD_GET_STATUS:
        if len(payload) < 4:
            raise WifiUartResponseError("GET_STATUS response header is truncated")
        mode, state, sta_connected = payload[1], payload[2], bool(payload[3])
        offset = 4
        ssid, offset = _unpack_str8(payload, offset, "ssid")
        ap_ssid, offset = _unpack_str8(payload, offset, "ap_ssid")
        ap_password, offset = _unpack_str8(payload, offset, "ap_password")
        sta_ip, offset = _unpack_str8(payload, offset, "sta_ip")
        if offset + 2 > len(payload):
            raise WifiUartResponseError("GET_STATUS response tail is truncated")
        sta_rssi = struct.unpack_from("<b", payload, offset)[0]
        ap_clients = payload[offset + 1]
        return subcommand, UartWifiStatus(
            mode=mode,
            state=state,
            sta_connected=sta_connected,
            ssid=ssid,
            ap_ssid=ap_ssid,
            ap_password=ap_password,
            sta_ip=sta_ip,
            sta_rssi=sta_rssi,
            ap_clients=ap_clients,
        )

    if subcommand == WIFI_CMD_SCAN:
        if len(payload) < 3:
            raise WifiUartResponseError("SCAN response header is truncated")
        count, truncated = payload[1], bool(payload[2])
        offset = 3
        entries = []
        for i in range(count):
            ssid, offset = _unpack_str8(payload, offset, f"SCAN entry {i} ssid")
            if offset + 2 > len(payload):
                raise WifiUartResponseError(f"SCAN entry {i} rssi/secure truncated")
            rssi = struct.unpack_from("<b", payload, offset)[0]
            secure = bool(payload[offset + 1])
            offset += 2
            entries.append(UartWifiScanEntry(ssid=ssid, rssi=rssi, secure=secure))
        return subcommand, (entries, truncated)

    if subcommand == WIFI_CMD_GET_NETWORKS:
        if len(payload) < 3:
            raise WifiUartResponseError("GET_NETWORKS response header is truncated")
        count, truncated = payload[1], bool(payload[2])
        offset = 3
        entries = []
        for i in range(count):
            ssid, offset = _unpack_str8(payload, offset, f"network {i} ssid")
            if offset + 4 > len(payload):
                raise WifiUartResponseError(f"network {i} flags truncated")
            saved = bool(payload[offset])
            in_range = bool(payload[offset + 1])
            rssi = struct.unpack_from("<b", payload, offset + 2)[0]
            secure = bool(payload[offset + 3])
            # connected flag was documented as a 6th field; guard for firmware
            # that omits it rather than raising on an otherwise-valid frame.
            if offset + 5 <= len(payload):
                connected = bool(payload[offset + 4])
                offset += 5
            else:
                connected = False
                offset += 4
            entries.append(
                UartWifiSavedNetwork(
                    ssid=ssid, saved=saved, in_range=in_range, rssi=rssi, secure=secure,
                    connected=connected,
                )
            )
        return subcommand, (entries, truncated)

    if subcommand in (
        WIFI_CMD_ADD_NETWORK,
        WIFI_CMD_SET_MODE,
        WIFI_CMD_SET_AP_IDENTITY,
        WIFI_CMD_FORGET,
    ):
        # Used to reduce this to bool(payload[1]) -- the same decode-then-
        # discard bug CONTROL/PROFILES/AUTOTUNE had (see their fixes
        # above). wifi_bridge_task() (uart_bridge_ext.c) gives every one of
        # these a real reason on refusal ("ssid too long", "saved network
        # list is full", "ap_password must be empty or 8-63 characters",
        # "could not forget network", etc) that this was silently dropping.
        return subcommand, _decode_ok_reason(
            payload, WifiUartResponseError, "ADD_NETWORK/SET_MODE/SET_AP_IDENTITY/FORGET"
        )

    raise WifiUartResponseError(f"unknown WIFI response subcommand 0x{subcommand:02X}")


