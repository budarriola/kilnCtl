"""Wi-Fi Settings and Firing Status popups (HTTP to the board), plus the shared generic-popup dispatch helpers used by every popup opener in the app.

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
WifiFiringMixin is mixed into KilnCtrlApp (see gui.py) so every method below
still runs against the single ``self`` the app constructs -- no ``_gui.``
indirection needed for per-instance state; only the rare module-level
mutable global gets that treatment (there are none of those referenced
here). See gui.py's module docstring for the overall map.
"""
from __future__ import annotations

import json
import queue
import re
import subprocess
import sys
import threading
import tkinter as tk
import urllib.error
import urllib.parse
import urllib.request

from . import http_auth
import webbrowser
from collections import deque
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, simpledialog, ttk
from typing import Callable, Optional

from . import devices, pin_overlay, pinout_reference, settings, wifi_credentials
from .autotune import AutotuneClient, AutotuneQueryError
from .control import ControlClient, ControlQueryError
from .device_log import LogClient
from .devices import (
    FirmwareVersion,
    IoState,
    LogLine,
    OkReason,
    PinConfigEntry,
    ProfileSegment,
    SafetyStatus,
    ThermoReading,
    WifiStatus,
)
from .display import BlitError, DisplayClient, DisplayQueryError
from .info import InfoClient, InfoQueryError
from .io_expander import IoClient, IoQueryError
from .link_hub import get_shared_link
from .profiles import ProfilesClient, ProfilesQueryError
from .protocol import (
    DISPLAY_NATIVE_HEIGHT,
    DISPLAY_NATIVE_WIDTH,
    IO_DIGITAL_COUNT,
    IO_RELAY_COUNT,
    PROFILES_SAVE_ID_NEW,
    profile_id_is_builtin,
    THERMO_CHANNEL_COUNT,
    UART_TASK_ID_AUTOTUNE,
    UART_TASK_ID_CONTROL,
    UART_TASK_ID_PROFILES,
    UART_TASK_ID_SAFETY,
    UART_TASK_ID_SYSTEM,
    UART_TASK_ID_WIFI,
    AUTOTUNE_METHOD_RELAY,
    AUTOTUNE_METHOD_STEP,
    AUTOTUNE_RULE_TL,
    AUTOTUNE_RULE_ZN,
    AvgMode,
    LogLevel,
    TcType,
    WIFI_MODE_AP,
    WIFI_MODE_HOME,
)
from .safety import SafetyClient, SafetyQueryError
from .system import SystemClient, SystemQueryError
from .serial_link import PortInfo, list_ports, recommend_port
from .session_log import MAX_KEEP_LOGS, MIN_KEEP_LOGS, SessionLogger
from .thermo import ThermoClient, ThermoQueryError
from .wifi_uart import WifiUartClient, WifiUartQueryError

_NO_PORTS = "<no serial ports found>"
_PINOUT_IMAGE = Path(__file__).resolve().with_name("assets") / "pinout.png"
_PINOUT_SCALE = 1.0
_LOG_HISTORY_LINES = 2000
_DEVICE_LOG_HISTORY_LINES = 2000
_THERMO_REPORT_PERIOD_MS = 1000
_IO_REPORT_PERIOD_MS = 500
_SAFETY_POLL_MS = 2000
_FIRING_POLL_MS = 3000
_DEVICE_LOG_COLORS: dict[LogLevel, str] = {
    LogLevel.ERROR: "#a11",
    LogLevel.WARN: "#b8860b",
    LogLevel.INFO: "#000000",
    LogLevel.DEBUG: "#555555",
    LogLevel.VERBOSE: "#888888",
}
_DEVICE_LOG_SESSION_METHOD_NAME: dict[LogLevel, str] = {
    LogLevel.ERROR: "error",
    LogLevel.WARN: "warning",
}
_WIFI_AP_DEFAULT_HOST = "192.168.4.1"
_WIFI_MDNS_HOST = "kilnctl.local"
_WIFI_HTTP_TIMEOUT_S = 8.0
_OK_COLOR = "#0a7d28"
_BAD_COLOR = "#a11"
_MUTED_COLOR = "#555555"
_EXPECTED_COLOR = "#b8860b"


class WifiFiringMixin:
    # -- Wi-Fi settings popup ------------------------------------------------
    # Talks straight HTTP to wifi_provision_http.c's routes (/status, /scan,
    # /provision) -- the same API the captive-portal page a phone sees uses.
    # Deliberately not a UART command: wifi_prov's join/scan/local-only
    # controls were never exposed over the UART protocol (only read-only
    # GET_WIFI_STATUS was, for the dashboard button above), and there's no
    # need to invent a new task_id/subcommand set when the board already
    # serves this over HTTP. The PC must be able to route to the target host
    # -- joined to the board's fallback AP for first-time setup, or already
    # on the same home network once it's provisioned.
    def open_wifi_settings_popup(self) -> None:
        self._popup("wifi_settings", "Wi-Fi Settings", self._build_wifi_settings_popup)
        # Default to UART whenever we have no evidence of a live Wi-Fi link --
        # the HTTP path has nothing to talk to before the board has ever
        # joined a network (or if it's dropped off one), so start on the
        # link that always works instead of making the user discover the
        # checkbox after a failed query.
        wifi_reachable = bool(self.wifi_status and self.wifi_status.connected)
        self.wifi_use_uart_var.set(not wifi_reachable)
        self.wifi_settings_refresh_status_async()

    # -- Firing Status popup -------------------------------------------------
    # Same "straight HTTP to the board" approach as the Wi-Fi popup above,
    # against dashboard_http.c's /api/profile_exec and /api/autotune (TODO.md
    # section 6/6A) -- read-only here, deliberately: starting/stopping a
    # firing or an autotune run stays a web-dashboard action for now, this is
    # a status view for whoever's driving the board from this GUI's Wi-Fi/
    # UART side and wants the PID/thermal-guard/autotune picture without
    # switching to a browser. Polls on a timer (unlike the Wi-Fi popup, which
    # only refreshes on demand) because that's the point of a status page --
    # see _FIRING_POLL_MS's comment for why this is safe to do at 3s.
    def open_firing_status_popup(self) -> None:
        self._popup(
            "firing_status", "Firing Status (PID/Autotune)", self._build_firing_status_popup,
            on_close=self._firing_stop_poll,
        )
        self.firing_status_refresh_async()
        self._firing_schedule_poll()

    def _wifi_settings_is_open(self) -> bool:
        return self._is_open("wifi_settings")

    def _wifi_default_host(self) -> str:
        if self.wifi_status and self.wifi_status.connected and self.wifi_status.ip:
            return self.wifi_status.ip
        return _WIFI_AP_DEFAULT_HOST

    def _build_wifi_settings_popup(self, top: tk.Toplevel) -> None:
        host_row = ttk.Frame(top)
        host_row.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Label(host_row, text="Device host/IP:").pack(side="left")
        self.wifi_host_var = tk.StringVar(value=self._wifi_default_host())
        ttk.Entry(host_row, textvariable=self.wifi_host_var, width=16).pack(
            side="left", padx=(6, 6)
        )
        ttk.Button(
            host_row, text="Refresh Status", command=self.wifi_settings_refresh_status_async
        ).pack(side="left")

        # Also reachable at kilnctl.local -- the firmware advertises this over
        # mDNS unconditionally at boot (both AP-fallback and station mode),
        # so it works even when the IP above is stale or unknown. Shown as a
        # copy-pasteable hint, not wired as the host field's default: mDNS
        # resolution isn't guaranteed on every OS (needs Bonjour/avahi).
        mdns_row = ttk.Frame(top)
        mdns_row.pack(fill="x", padx=8, pady=(0, 4))
        ttk.Label(
            mdns_row, text=f"Also reachable at: {_WIFI_MDNS_HOST}", foreground="#555555",
        ).pack(side="left")

        # UART toggle: the whole point of task WIFI (11) is provisioning
        # without needing an existing HTTP/network path in the first place --
        # useful before the board has ever joined a network, when the host/IP
        # field above has nothing valid to point at. When checked, every
        # action in this popup goes over the UART link (self.wifi_uart_client)
        # instead of urllib HTTP against the host field; the host field itself
        # is then unused.
        self.wifi_use_uart_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            host_row, text="Use UART (no network needed)", variable=self.wifi_use_uart_var,
        ).pack(side="left", padx=(12, 0))

        self.wifi_settings_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            top, textvariable=self.wifi_settings_status_var, anchor="w", justify="left"
        ).pack(fill="x", padx=8, pady=(0, 8))

        scan_frame = ttk.LabelFrame(top, text="Nearby networks")
        scan_frame.pack(fill="both", expand=True, padx=8, pady=4)
        self.wifi_scan_list = tk.Listbox(scan_frame, height=8, exportselection=False)
        self.wifi_scan_list.pack(side="left", fill="both", expand=True, padx=(6, 0), pady=6)
        self.wifi_scan_list.bind("<<ListboxSelect>>", self._on_wifi_scan_select)
        scan_scroll = ttk.Scrollbar(
            scan_frame, orient="vertical", command=self.wifi_scan_list.yview
        )
        self.wifi_scan_list.configure(yscrollcommand=scan_scroll.set)
        scan_scroll.pack(side="left", fill="y", pady=6)
        ttk.Button(scan_frame, text="Scan", command=self.wifi_scan_async).pack(
            side="left", padx=6, pady=6, anchor="n"
        )
        # (ssid, secure) for each row currently listed, in display order --
        # lets a click fill the SSID field without re-parsing label text.
        self._wifi_scan_entries: list[tuple[str, bool]] = []

        join_frame = ttk.LabelFrame(top, text="Join a network")
        join_frame.pack(fill="x", padx=8, pady=4)
        self.wifi_ssid_var = tk.StringVar()
        self.wifi_password_var = tk.StringVar()
        ttk.Label(join_frame, text="SSID:").grid(row=0, column=0, padx=6, pady=6, sticky="e")
        ttk.Entry(join_frame, textvariable=self.wifi_ssid_var, width=28).grid(
            row=0, column=1, padx=(0, 6), pady=6, sticky="we"
        )
        ttk.Label(join_frame, text="Password:").grid(
            row=1, column=0, padx=6, pady=(0, 6), sticky="e"
        )
        ttk.Entry(join_frame, textvariable=self.wifi_password_var, width=28, show="*").grid(
            row=1, column=1, padx=(0, 6), pady=(0, 6), sticky="we"
        )
        ttk.Button(join_frame, text="Connect", command=self.wifi_connect_async).grid(
            row=0, column=2, rowspan=2, padx=6, pady=6
        )
        join_frame.columnconfigure(1, weight=1)

        mode_frame = ttk.LabelFrame(top, text="Wi-Fi mode")
        mode_frame.pack(fill="x", padx=8, pady=(4, 8))
        self.wifi_mode_var = tk.StringVar(value="home")
        ttk.Radiobutton(
            mode_frame,
            text="Home Wi-Fi (join the network above)",
            variable=self.wifi_mode_var,
            value="home",
        ).pack(anchor="w", padx=6, pady=(6, 0))
        ttk.Radiobutton(
            mode_frame,
            text="Access Point (AP always on, never joins a network)",
            variable=self.wifi_mode_var,
            value="ap",
        ).pack(anchor="w", padx=6, pady=(0, 4))
        ttk.Button(mode_frame, text="Apply", command=self.wifi_set_mode_async).pack(
            anchor="w", padx=6, pady=(0, 6)
        )

        ap_frame = ttk.LabelFrame(top, text="Access point identity")
        ap_frame.pack(fill="x", padx=8, pady=(4, 8))
        ttk.Label(
            ap_frame,
            text="This board's own network -- used for first-time setup and while in "
            "Access Point mode. Not the home network it joins.",
            foreground=_MUTED_COLOR,
            wraplength=420,
            justify="left",
        ).grid(row=0, column=0, columnspan=3, padx=6, pady=(6, 4), sticky="w")
        ttk.Label(ap_frame, text="SSID:").grid(row=1, column=0, padx=6, pady=(0, 6), sticky="e")
        self.wifi_ap_ssid_var = tk.StringVar()
        ttk.Entry(ap_frame, textvariable=self.wifi_ap_ssid_var, width=24).grid(
            row=1, column=1, padx=(0, 6), pady=(0, 6), sticky="w"
        )
        ttk.Label(ap_frame, text="Password:").grid(row=2, column=0, padx=6, pady=(0, 6), sticky="e")
        self.wifi_ap_password_var = tk.StringVar()
        ttk.Entry(ap_frame, textvariable=self.wifi_ap_password_var, width=24, show="*").grid(
            row=2, column=1, padx=(0, 6), pady=(0, 6), sticky="w"
        )
        ttk.Label(ap_frame, text="(empty = open network)", foreground=_MUTED_COLOR).grid(
            row=2, column=2, padx=(0, 6), pady=(0, 6), sticky="w"
        )
        ttk.Button(
            ap_frame, text="Save AP Identity", command=self.wifi_set_ap_identity_async
        ).grid(row=3, column=0, columnspan=3, padx=6, pady=(0, 6), sticky="w")
        ap_frame.columnconfigure(1, weight=1)

    def _on_wifi_scan_select(self, _event: object) -> None:
        selection = self.wifi_scan_list.curselection()
        if not selection:
            return
        ssid, _secure = self._wifi_scan_entries[selection[0]]
        self.wifi_ssid_var.set(ssid)

    def _wifi_host(self) -> str:
        return self.wifi_host_var.get().strip() or _WIFI_AP_DEFAULT_HOST

    def _wifi_http_get(self, path: str) -> object:
        url = f"http://{self._wifi_host()}{path}"
        with http_auth.urlopen(url, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def _wifi_http_post_form(self, path: str, fields: dict[str, str]) -> None:
        url = f"http://{self._wifi_host()}{path}"
        body = urllib.parse.urlencode(fields).encode("ascii")
        req = urllib.request.Request(
            url, data=body, headers={"Content-Type": "application/x-www-form-urlencoded"}
        )
        with http_auth.urlopen(req, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
            resp.read()

    @staticmethod
    def _wifi_http_error_text(exc: Exception) -> str:
        if isinstance(exc, urllib.error.HTTPError):
            detail = exc.read().decode("utf-8", errors="replace").strip()
            return f"HTTP {exc.code}{': ' + detail if detail else ''}"
        if isinstance(exc, urllib.error.URLError):
            return f"unreachable: {exc.reason}"
        return str(exc)

    def _wifi_use_uart(self) -> bool:
        return bool(getattr(self, "wifi_use_uart_var", None) and self.wifi_use_uart_var.get())

    def wifi_settings_refresh_status_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        if self._wifi_use_uart():
            self.wifi_settings_status_var.set("Querying over UART...")

            def apply(status) -> None:
                if not self._wifi_settings_is_open():
                    return
                self.wifi_mode_var.set(status.mode_name)
                if status.ap_ssid and not self.wifi_ap_ssid_var.get():
                    self.wifi_ap_ssid_var.set(status.ap_ssid)
                text = (
                    f"Mode: {'Access Point' if status.mode_name == 'ap' else 'Home Wi-Fi'}  |  "
                    f"Saved SSID: {status.ssid}  |  "
                    f"Station: {'connected (' + status.sta_ip + ')' if status.sta_connected else 'not connected'}"
                )
                self.wifi_settings_status_var.set(text)
                self.session_log.info("wifi (UART) settings status: %s", text)

            self.query_async(
                "Wi-Fi status (UART)", lambda: self.wifi_uart_client.get_status(), apply,
                error_types=(WifiUartQueryError,),
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(f"Querying {host}...")

        def worker() -> None:
            try:
                data = self._wifi_http_get("/status")
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning(
                    "Wi-Fi settings HTTP status query failed (%s), falling back to UART", text
                )
                # HTTP is unreachable -- fall back to UART automatically
                # rather than leaving the user staring at "Query failed".
                # Flip the checkbox too so every later action in this popup
                # (scan/connect/mode/AP identity) follows the same path
                # instead of retrying the dead HTTP link.
                self.post(lambda: self.wifi_use_uart_var.set(True))
                self.post(self.wifi_settings_refresh_status_async)
                return
            self.post(lambda: self._apply_wifi_settings_status(data))

        threading.Thread(target=worker, name="wifi-http-status", daemon=True).start()

    def _apply_wifi_settings_status(self, data: dict) -> None:
        if not self._wifi_settings_is_open():
            return
        state = data.get("state", "unknown")
        mode = data.get("mode", "home")
        ssid = data.get("ssid", "")
        sta_connected = bool(data.get("sta_connected", False))
        sta_ip = data.get("sta_ip", "")
        ap_ssid = data.get("ap_ssid", "")
        self.wifi_mode_var.set(mode if mode in ("home", "ap") else "home")
        if ap_ssid and not self.wifi_ap_ssid_var.get():
            # Only prefill if the user hasn't already started typing a
            # replacement -- a refresh mid-edit shouldn't clobber it.
            self.wifi_ap_ssid_var.set(ap_ssid)
        text = f"State: {state}"
        if ssid:
            text += f"  |  Saved SSID: {ssid}"
        text += f"  |  Mode: {'Access Point' if mode == 'ap' else 'Home Wi-Fi'}"
        text += f"  |  Station: {'connected (' + sta_ip + ')' if sta_connected else 'not connected'}"
        self.wifi_settings_status_var.set(text)
        self.session_log.info("wifi settings status: %s", text)

    def wifi_scan_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        if self._wifi_use_uart():
            self.wifi_settings_status_var.set("Scanning over UART...")

            def apply(result) -> None:
                if not self._wifi_settings_is_open():
                    return
                entries, truncated = result
                self.wifi_scan_list.delete(0, "end")
                self._wifi_scan_entries = []
                for e in entries:
                    lock = "" if e.secure else " (open)"
                    self.wifi_scan_list.insert("end", f"{e.ssid}  [{e.rssi} dBm]{lock}")
                    self._wifi_scan_entries.append((e.ssid, e.secure))
                suffix = " (truncated)" if truncated else ""
                self.wifi_settings_status_var.set(f"{len(entries)} network(s) found{suffix}.")

            self.query_async(
                "Wi-Fi scan (UART)", lambda: self.wifi_uart_client.scan(), apply,
                error_types=(WifiUartQueryError,),
            )
            return
        self.wifi_settings_status_var.set(f"Scanning from {self._wifi_host()}...")

        def worker() -> None:
            try:
                data = self._wifi_http_get("/scan")
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi scan failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"Scan failed: {text}"))
                return
            self.post(lambda: self._apply_wifi_scan(data))

        threading.Thread(target=worker, name="wifi-http-scan", daemon=True).start()

    def _apply_wifi_scan(self, results: list) -> None:
        if not self._wifi_settings_is_open():
            return
        self.wifi_scan_list.delete(0, "end")
        self._wifi_scan_entries = []
        for entry in results:
            ssid = str(entry.get("ssid", ""))
            rssi = entry.get("rssi", 0)
            secure = bool(entry.get("secure", True))
            lock = "" if secure else " (open)"
            self.wifi_scan_list.insert("end", f"{ssid}  [{rssi} dBm]{lock}")
            self._wifi_scan_entries.append((ssid, secure))
        self.wifi_settings_status_var.set(f"{len(results)} network(s) found.")
        self.session_log.info("wifi scan: %d network(s)", len(results))

    def wifi_connect_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        ssid = self.wifi_ssid_var.get().strip()
        if not ssid:
            self.wifi_settings_status_var.set("Connect: SSID is required.")
            return
        password = self.wifi_password_var.get()
        wifi_credentials.save(ssid, password)
        if self._wifi_use_uart():
            self.wifi_settings_status_var.set(f"Sending credentials for {ssid!r} over UART...")
            self.session_log.info("wifi (UART) settings: add network %r", ssid)

            def apply(result: OkReason) -> None:
                if not self._wifi_settings_is_open():
                    return
                if result.ok:
                    self.wifi_settings_status_var.set(
                        f"Credentials sent for {ssid!r}. Joining in the background -- Refresh "
                        "Status to check progress."
                    )
                    self.wifi_settings_refresh_status_async()
                else:
                    detail = f" ({result.reason})" if result.reason else ""
                    self.wifi_settings_status_var.set(f"Add network {ssid!r}: REJECTED{detail}.")

            self.query_async(
                "Wi-Fi add network (UART)",
                lambda: self.wifi_uart_client.add_network(ssid, password),
                apply,
                error_types=(WifiUartQueryError,),
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(f"Sending credentials for {ssid!r} to {host}...")
        self.session_log.info("wifi settings: connect to %r via %s", ssid, host)

        def worker() -> None:
            try:
                self._wifi_http_post_form("/provision", {"ssid": ssid, "password": password})
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi connect failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"Connect failed: {text}"))
                return
            self.post(lambda: self._wifi_settings_status_var.set(
                f"Credentials sent for {ssid!r}. Joining in the background -- Refresh Status "
                "to check progress."
            ))
            self.post(self.wifi_settings_refresh_status_async)

        threading.Thread(target=worker, name="wifi-http-connect", daemon=True).start()

    def wifi_set_mode_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        want_mode = self.wifi_mode_var.get()
        if self._wifi_use_uart():
            mode_byte = WIFI_MODE_AP if want_mode == "ap" else WIFI_MODE_HOME
            self.wifi_settings_status_var.set(
                f"Switching to {'Access Point' if want_mode == 'ap' else 'Home Wi-Fi'} mode over UART..."
            )

            def apply(result: OkReason) -> None:
                if self._wifi_settings_is_open():
                    if result.ok:
                        self.wifi_settings_status_var.set("Mode change: ok.")
                        self.wifi_settings_refresh_status_async()
                    else:
                        detail = f" ({result.reason})" if result.reason else ""
                        self.wifi_settings_status_var.set(f"Mode change: REJECTED{detail}.")

            self.query_async(
                "Wi-Fi set mode (UART)", lambda: self.wifi_uart_client.set_mode(mode_byte), apply,
                error_types=(WifiUartQueryError,),
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(
            f"Switching to {'Access Point' if want_mode == 'ap' else 'Home Wi-Fi'} mode on {host}..."
        )

        def worker() -> None:
            try:
                self._wifi_http_post_form("/provision", {"mode": want_mode})
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi mode change failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"Apply failed: {text}"))
                return
            self.post(self.wifi_settings_refresh_status_async)

        threading.Thread(target=worker, name="wifi-http-mode", daemon=True).start()

    def wifi_set_ap_identity_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        ap_ssid = self.wifi_ap_ssid_var.get().strip()
        ap_password = self.wifi_ap_password_var.get()
        if not ap_ssid:
            self.wifi_settings_status_var.set("AP SSID is required.")
            return
        if ap_password and len(ap_password) < 8:
            self.wifi_settings_status_var.set(
                "AP password must be empty (open network) or at least 8 characters (WPA2)."
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(f"Setting AP identity on {host}...")

        def worker() -> None:
            try:
                self._wifi_http_post_form(
                    "/provision", {"ap_ssid": ap_ssid, "ap_password": ap_password}
                )
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi AP identity change failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"AP identity change failed: {text}"))
                return
            self.session_log.info("wifi settings: AP identity changed")
            self.post(lambda: self.wifi_settings_status_var.set(
                "AP identity changed. If this PC is joined to the board's AP, it will be "
                "disconnected and must rejoin with the new SSID/password."
            ))

        threading.Thread(target=worker, name="wifi-http-ap-identity", daemon=True).start()

    def _build_firing_status_popup(self, top: tk.Toplevel) -> None:
        host_row = ttk.Frame(top)
        host_row.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Label(host_row, text="Device host/IP:").pack(side="left")
        self.firing_host_var = tk.StringVar(value=self._wifi_default_host())
        ttk.Entry(host_row, textvariable=self.firing_host_var, width=16).pack(
            side="left", padx=(6, 6)
        )
        ttk.Button(
            host_row, text="Refresh Now", command=self.firing_status_refresh_async
        ).pack(side="left")

        profile_frame = ttk.LabelFrame(top, text="Profile executor")
        profile_frame.pack(fill="x", padx=8, pady=4)
        self.firing_profile_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            profile_frame, textvariable=self.firing_profile_var, anchor="w", justify="left",
            wraplength=440,
        ).pack(fill="x", padx=6, pady=6)

        autotune_frame = ttk.LabelFrame(top, text="Autotune")
        autotune_frame.pack(fill="x", padx=8, pady=(4, 8))
        self.firing_autotune_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            autotune_frame, textvariable=self.firing_autotune_var, anchor="w", justify="left",
            wraplength=440,
        ).pack(fill="x", padx=6, pady=6)

        ttk.Label(
            top,
            text="Read-only: start/stop a firing or an autotune run from the web dashboard "
            "(Open Web Dashboard on the main window) or the Zones/Profiles pages there.",
            foreground=_MUTED_COLOR, wraplength=440, justify="left",
        ).pack(fill="x", padx=8, pady=(0, 8))

    def _firing_host(self) -> str:
        var = getattr(self, "firing_host_var", None)
        if var is not None:
            host = var.get().strip()
            if host:
                return host
        return self._wifi_default_host()

    def _firing_status_is_open(self) -> bool:
        return self._is_open("firing_status")

    def _firing_schedule_poll(self) -> None:
        self._firing_stop_poll()
        if not self._firing_status_is_open():
            return
        self._firing_poll_id = self.root.after(_FIRING_POLL_MS, self._firing_poll_tick)

    def _firing_poll_tick(self) -> None:
        self._firing_poll_id = None
        if not self._firing_status_is_open():
            return
        self.firing_status_refresh_async(quiet=True)
        self._firing_schedule_poll()

    def _firing_stop_poll(self) -> None:
        if self._firing_poll_id is not None:
            self.root.after_cancel(self._firing_poll_id)
            self._firing_poll_id = None

    def firing_status_refresh_async(self, quiet: bool = False) -> None:
        if not self._firing_status_is_open():
            return
        host = self._firing_host()

        def worker() -> None:
            try:
                profile = self._wifi_http_get_from("/api/profile_exec", host)
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                if not quiet:
                    self.session_log.warning("Firing status query failed: %s", text)
                self.post(lambda: self._apply_firing_profile_error(text))
                return
            try:
                autotune = self._wifi_http_get_from("/api/autotune", host)
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.post(lambda: self._apply_firing_profile(profile))
                self.post(lambda: self._apply_firing_autotune_error(text))
                return
            self.post(lambda: self._apply_firing_profile(profile))
            self.post(lambda: self._apply_firing_autotune(autotune))

        threading.Thread(target=worker, name="firing-status-http", daemon=True).start()

    def _apply_firing_profile_error(self, text: str) -> None:
        if self._firing_status_is_open():
            self.firing_profile_var.set(f"Query failed: {text}")
            self.firing_autotune_var.set(f"Query failed: {text}")

    def _apply_firing_autotune_error(self, text: str) -> None:
        if self._firing_status_is_open():
            self.firing_autotune_var.set(f"Query failed: {text}")

    def _apply_firing_profile(self, data: dict) -> None:
        """TODO.md 6A.5: a profile can now drive more than one zone at once
        off a single shared ramp/dwell schedule (ramp-lock), so the status
        payload grew a `zones` array instead of one flat zone/actual/duty
        set -- see dashboard_http.c's append_zone_status_json()."""
        if not self._firing_status_is_open():
            return
        state = data.get("state", "unknown")
        if state == "idle":
            self.firing_profile_var.set("Idle -- no profile running.")
            return
        # Mirrors zone_control_mode_t (firmware/KilnFW/App/drivers/http/zones_http.h)
        # -- no shared Python source exists for this enum (devices_control.py
        # only carries the raw int; the web dashboard's MODE_NAMES lives in
        # zones_page.html, a different language). Kept as a local dict rather
        # than adding a fourth hand-maintained copy in a new module.
        mode_names = {0: "OFF", 1: "BANGBANG", 2: "PID", 3: "PID_FUZZY"}
        lines = [
            f"State: {state}   Zones: {data.get('zone_mask', 0):#04x}   "
            f"Profile: {data.get('profile_name', '')!r} (id {data.get('profile_id', '?')})",
            f"Segment {data.get('segment_index', 0) + 1}/{data.get('segment_count', 0)}"
            f"{'  (dwelling)' if data.get('dwelling') else '  (ramping)'}"
            f"   Shared target: {data.get('target_c', 0):.1f}C",
        ]
        if data.get("ramp_lock_held"):
            lines.append(f"Ramp-lock held -- waiting on zone mask {data.get('ramp_lock_lagging_mask', 0):#04x}")
        for z in data.get("zones", []):
            actual = f"{z.get('actual_c', 0):.1f}C" if z.get("actual_valid") else "no reading"
            mode = mode_names.get(z.get("control_mode", 0), "?")
            status = f"FAULT(guard {z.get('fault_guard', '?')})" if z.get("faulted") else (
                "ON" if z.get("relay_on") else "off"
            )
            lines.append(f"  Zone {z.get('zone', '?')}: {actual}   {mode}   {status}   duty={z.get('duty', 0):.2f}")
            if z.get("faulted") and z.get("fault_reason"):
                lines.append(f"    -> {z.get('fault_reason')}")
        if state == "faulted" and data.get("fault_reason"):
            lines.append(f"FAULT (guard {data.get('fault_guard', '?')}): {data.get('fault_reason', '')}")
        self.firing_profile_var.set("\n".join(lines))

    def _apply_firing_autotune(self, data: dict) -> None:
        if not self._firing_status_is_open():
            return
        state = data.get("state", "unknown")
        if state == "idle":
            self.firing_autotune_var.set("Idle -- no autotune run active.")
            return
        lines = [
            f"State: {state}   Zone: {data.get('zone', '?')}   "
            f"Elapsed: {data.get('elapsed_s', 0)}s   Samples: {data.get('sample_count', 0)}",
        ]
        if data.get("actual_valid"):
            lines.append(f"Actual: {data.get('actual_c', 0):.1f}C   Duty: {data.get('duty', 0):.2f}")
        if state == "aborted" and data.get("abort_reason"):
            lines.append(f"Aborted: {data.get('abort_reason')}")
        if state == "done" and data.get("model_valid"):
            lines.append(
                f"Fitted K={data.get('k_gain_c_per_duty', 0):.1f}C/duty  "
                f"tau={data.get('tau_s', 0):.0f}s  L={data.get('dead_time_s', 0):.0f}s"
            )
            lines.append(
                f"Proposed: Kp={data.get('proposed_kp', 0):.4f}  Ki={data.get('proposed_ki', 0):.5f}  "
                f"Kd={data.get('proposed_kd', 0):.4f}"
            )
            lines.append(
                f"Predicted max ramp: ~{data.get('predicted_max_ramp_c_per_hr', 0):.0f} C/hr   "
                "(accept/download trace from the web dashboard's Zones page)"
            )
        self.firing_autotune_var.set("\n".join(lines))

    def _wifi_http_get_from(self, path: str, host: str) -> object:
        """Like _wifi_http_get, but against an explicit host -- the Firing
        Status popup has its own host field so it can point at a device
        without opening the Wi-Fi Settings popup too."""
        url = f"http://{host}{path}"
        with http_auth.urlopen(url, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
            return json.loads(resp.read().decode("utf-8"))

    # -- popups ------------------------------------------------------------
    def _popup(self, key: str, title: str, builder: Callable[[tk.Toplevel], None],
               on_close: Optional[Callable[[], None]] = None) -> bool:
        """Open (or raise) a Toplevel. Returns True if it was newly built."""
        existing = self.popups.get(key)
        if existing is not None and existing.winfo_exists():
            existing.deiconify()
            existing.lift()
            existing.focus_force()
            return False

        top = tk.Toplevel(self.root)
        top.title(title)
        top.transient(self.root)

        def close() -> None:
            self.popups.pop(key, None)
            if on_close is not None:
                on_close()
            top.destroy()

        top.protocol("WM_DELETE_WINDOW", close)
        self.popups[key] = top
        builder(top)
        return True

    def _is_open(self, key: str) -> bool:
        top = self.popups.get(key)
        return top is not None and top.winfo_exists()

    def open_thermo_popup(self) -> None:
        self._popup(
            "thermo",
            "Thermocouples (3x MAX31856) - manual control",
            self._build_thermo_popup,
            on_close=self._thermo_stop_reporting,
        )
        # Reopening an already-open window skips the builder, so (re)arm the
        # live feed here: opening the page should always land on live data.
        self._thermo_start_reporting()

    def open_io_popup(self) -> None:
        self._popup(
            "io",
            "I/O & Relays (SX1509) - manual control",
            self._build_io_popup,
            on_close=self._io_stop_reporting,
        )
        self._io_start_reporting()

    def open_display_popup(self) -> None:
        self._popup("display", "Display (ILI9488) - manual control", self._build_display_popup)

    def open_safety_popup(self) -> None:
        # No on_close poll-stop here: the status-bar safety summary (see
        # gui.py's _build_status_bar) keeps polling after the popup closes,
        # for as long as the link stays connected -- only toggle_connect's
        # disconnect path calls _safety_stop_poll now.
        self._popup(
            "safety",
            "Safety processor (RP2040, isolated) - status",
            self._build_safety_popup,
        )
        self.safety_refresh_async()
        if self._safety_poll_id is None:
            self._safety_schedule_poll()

    def open_about_popup(self) -> None:
        self._popup("about", "About - pin configuration", self._build_about_popup)
        # Opening an already-open window skips the builder, so refresh here:
        # the point of the window is live data.
        self.refresh_pin_config()

    def open_pinout_reference_popup(self) -> None:
        """Static schematic-derived reference -- the opposite of the "Pin
        Configuration" popup above: no device, no UART, no live data. See
        pinout_reference.py's module docstring for why this is a separate
        page rather than folded into that one."""
        self._popup(
            "pinout_reference", "Full Board Pinout (reference)", self._build_pinout_reference_popup
        )

    def open_log_popup(self) -> None:
        self._popup("log", "Session log", self._build_log_popup)

    def open_device_log_popup(self) -> None:
        self._popup("device_log", "Device Console", self._build_device_log_popup)

