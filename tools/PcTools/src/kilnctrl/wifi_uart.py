"""Client for the firmware's WIFI task (task 11): status/scan/provision/
forget over UART, mirroring ``wifi_provision_http.c``'s ``/status``,
``/scan``, ``/provision``, ``/networks``, ``/forget``.

Exists specifically so Wi-Fi can be provisioned from a link that works even
when Wi-Fi itself is down or unconfigured -- the existing Wi-Fi Settings
popup (gui.py) talks straight HTTP to the same endpoints; this client is the
UART-only alternative, useful before the board has ever joined a network.
Never touches kiln_io/relay_authority/safety_link.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import UartWifiScanEntry, UartWifiSavedNetwork, UartWifiStatus, WifiUartResponseError
from .protocol import (
    UART_TASK_ID_WIFI,
    WIFI_CMD_ADD_NETWORK,
    WIFI_CMD_FORGET,
    WIFI_CMD_GET_NETWORKS,
    WIFI_CMD_GET_STATUS,
    WIFI_CMD_SCAN,
    WIFI_CMD_SET_AP_IDENTITY,
    WIFI_CMD_SET_MODE,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: SCAN probes nearby APs, which can take a few seconds; the rest are fast.
DEFAULT_REPLY_TIMEOUT_S = 3.0
SCAN_REPLY_TIMEOUT_S = 8.0


class WifiUartQueryError(RuntimeError):
    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class WifiUartClient:
    """Owns task :data:`UART_TASK_ID_WIFI` on the PC side of a link."""

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_WIFI) -> None:
        self.link = link
        self.task_id = task_id
        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-wifi-rx", daemon=True
        )
        self._consumer.start()

    def close(self) -> None:
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- requests ------------------------------------------------------------
    def get_status(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> UartWifiStatus:
        return self._query(WIFI_CMD_GET_STATUS, devices.wifi_uart_get_status(), timeout)  # type: ignore[return-value]

    def scan(self, timeout: float = SCAN_REPLY_TIMEOUT_S) -> "tuple[list[UartWifiScanEntry], bool]":
        return self._query(WIFI_CMD_SCAN, devices.wifi_uart_scan(), timeout)  # type: ignore[return-value]

    def add_network(
        self, ssid: str, password: str = "", timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> "devices.OkReason":
        """Add a saved network, and learn *why* if refused (e.g. "ssid too
        long", "saved network list is full")."""
        return self._query(
            WIFI_CMD_ADD_NETWORK, devices.wifi_uart_add_network(ssid, password), timeout
        )  # type: ignore[return-value]

    def set_mode(self, mode: int, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "devices.OkReason":
        """Switch AP/home mode, and learn *why* if refused."""
        return self._query(WIFI_CMD_SET_MODE, devices.wifi_uart_set_mode(mode), timeout)  # type: ignore[return-value]

    def set_ap_identity(
        self, ap_ssid: "Optional[str]" = None, ap_password: "Optional[str]" = None,
        timeout: float = DEFAULT_REPLY_TIMEOUT_S,
    ) -> "devices.OkReason":
        """Set the AP-mode SSID/password, and learn *why* if refused (e.g.
        "ap_password must be empty or 8-63 characters")."""
        return self._query(
            WIFI_CMD_SET_AP_IDENTITY,
            devices.wifi_uart_set_ap_identity(ap_ssid, ap_password),
            timeout,
        )  # type: ignore[return-value]

    def get_networks(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "tuple[list[UartWifiSavedNetwork], bool]":
        return self._query(WIFI_CMD_GET_NETWORKS, devices.wifi_uart_get_networks(), timeout)  # type: ignore[return-value]

    def forget(self, ssid: str, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "devices.OkReason":
        """Forget a saved network, and learn *why* if refused."""
        return self._query(WIFI_CMD_FORGET, devices.wifi_uart_forget(ssid), timeout)  # type: ignore[return-value]

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> object:
        with self._query_lock:
            pending = _Pending(subcommand)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self.link.send(
                    dst_task=self.task_id, src_task=self.task_id, payload=payload,
                    dst_device=Device.ESP,
                )
                if not result.ok:
                    raise WifiUartQueryError(
                        f"WIFI request 0x{subcommand:02X} not delivered: {result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise WifiUartQueryError(
                        f"WIFI request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s"
                    )
                return pending.value
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling WIFI frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_wifi_uart_response(frame.payload)
        except WifiUartResponseError as exc:
            log.warning("dropping malformed WIFI response: %s", exc)
            return
        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return
        log.debug("ignoring unsolicited WIFI response 0x%02X", subcommand)


__all__ = ["WifiUartClient", "WifiUartQueryError"]
