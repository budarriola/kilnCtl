"""Zones / PID popup (task 8, CONTROL).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
ZonesMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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
import webbrowser
from collections import deque
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, simpledialog, ttk
from typing import Callable, Optional

from . import devices, host_resolve, pin_overlay, pinout_reference, settings, wifi_credentials
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
_WIFI_AP_DEFAULT_HOST = host_resolve.resolve_default_host()  # was a hardcoded "192.168.4.1"
_WIFI_MDNS_HOST = "kilnctl.local"
_WIFI_HTTP_TIMEOUT_S = 8.0
_OK_COLOR = "#0a7d28"
_BAD_COLOR = "#a11"
_MUTED_COLOR = "#555555"
_EXPECTED_COLOR = "#b8860b"


class ZonesMixin:
    # ======================================================================
    # Zones / PID panel (task 8, CONTROL) -- UART mirror of /api/zones'
    # tuning-focused fields (PID gains + plant model + read-back). The
    # whole-page fields (naming, relay assignment, heater window timing,
    # cross-zone guard) stay HTTP-only -- see docs/UART_PROTOCOL.md.
    # ======================================================================
    def open_zones_popup(self) -> None:
        self._popup("zones", "Zones / PID (UART)", self._build_zones_popup)
        self.zones_refresh_async()

    def _build_zones_popup(self, top: tk.Toplevel) -> None:
        top.geometry("560x420")
        ttk.Button(top, text="Refresh", command=self.zones_refresh_async).pack(
            anchor="w", padx=8, pady=(8, 4)
        )
        self.zones_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(top, textvariable=self.zones_status_var, anchor="w").pack(
            fill="x", padx=8, pady=(0, 4)
        )

        container = ttk.Frame(top)
        container.pack(fill="both", expand=True, padx=8, pady=4)
        canvas = tk.Canvas(container, highlightthickness=0)
        scroll = ttk.Scrollbar(container, orient="vertical", command=canvas.yview)
        self.zones_list_frame = ttk.Frame(canvas)
        self.zones_list_frame.bind(
            "<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )
        canvas.create_window((0, 0), window=self.zones_list_frame, anchor="nw")
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")

        self._zones_widgets: dict = {}

    def zones_refresh_async(self) -> None:
        if not self._is_open("zones"):
            return
        self.zones_status_var.set("Querying zones...")
        self.query_async(
            "Get zones", lambda: self.control.get_zones(), self._apply_zones,
            error_types=(ControlQueryError,),
        )

    def _apply_zones(self, result) -> None:
        if not self._is_open("zones"):
            return
        thermo_count, relay_count, zones = result
        self.zones_status_var.set(
            f"{len(zones)} zone(s) ({thermo_count} thermocouples, {relay_count} relays)"
        )
        for child in self.zones_list_frame.winfo_children():
            child.destroy()
        self._zones_widgets = {}
        for zone in zones:
            frame = ttk.LabelFrame(self.zones_list_frame, text=zone.describe())
            frame.pack(fill="x", padx=4, pady=4)
            kp_var = tk.StringVar(value=f"{zone.pid_kp:.5f}")
            ki_var = tk.StringVar(value=f"{zone.pid_ki:.6f}")
            kd_var = tk.StringVar(value=f"{zone.pid_kd:.5f}")
            row = ttk.Frame(frame)
            row.pack(fill="x", padx=6, pady=4)
            ttk.Label(row, text="Kp:").pack(side="left")
            ttk.Entry(row, textvariable=kp_var, width=10).pack(side="left", padx=(2, 8))
            ttk.Label(row, text="Ki:").pack(side="left")
            ttk.Entry(row, textvariable=ki_var, width=10).pack(side="left", padx=(2, 8))
            ttk.Label(row, text="Kd:").pack(side="left")
            ttk.Entry(row, textvariable=kd_var, width=10).pack(side="left", padx=(2, 8))
            ttk.Button(
                row, text="Set PID",
                command=lambda z=zone.index, a=kp_var, b=ki_var, c=kd_var: self.zones_set_pid_async(z, a, b, c),
            ).pack(side="left", padx=(8, 0))

            k_var = tk.StringVar()
            tau_var = tk.StringVar()
            dead_var = tk.StringVar()
            row2 = ttk.Frame(frame)
            row2.pack(fill="x", padx=6, pady=(0, 6))
            ttk.Label(row2, text="K_dc:").pack(side="left")
            ttk.Entry(row2, textvariable=k_var, width=10).pack(side="left", padx=(2, 8))
            ttk.Label(row2, text="tau_s:").pack(side="left")
            ttk.Entry(row2, textvariable=tau_var, width=10).pack(side="left", padx=(2, 8))
            ttk.Label(row2, text="dead_time_s:").pack(side="left")
            ttk.Entry(row2, textvariable=dead_var, width=10).pack(side="left", padx=(2, 8))
            ttk.Button(
                row2, text="Set Model (0,0,0 clears)",
                command=lambda z=zone.index, a=k_var, b=tau_var, c=dead_var: self.zones_set_model_async(z, a, b, c),
            ).pack(side="left", padx=(8, 0))
            self._zones_widgets[zone.index] = frame

    def zones_set_pid_async(self, zone: int, kp_var, ki_var, kd_var) -> None:
        try:
            kp, ki, kd = float(kp_var.get()), float(ki_var.get()), float(kd_var.get())
        except ValueError as exc:
            self.zones_status_var.set(f"Set PID: {exc}")
            return

        def apply(result) -> None:
            if self._is_open("zones"):
                if result.ok:
                    self.zones_status_var.set(f"Zone {zone} PID set.")
                    self.zones_refresh_async()
                else:
                    detail = f" ({result.reason})" if result.reason else ""
                    self.zones_status_var.set(f"Zone {zone} PID REJECTED{detail}.")

        self.query_async(
            f"Set zone {zone} PID", lambda: self.control.set_zone_pid(zone, kp, ki, kd), apply,
            error_types=(ControlQueryError,),
        )

    def zones_set_model_async(self, zone: int, k_var, tau_var, dead_var) -> None:
        try:
            k_dc, tau_s, dead_time_s = float(k_var.get() or 0), float(tau_var.get() or 0), float(dead_var.get() or 0)
        except ValueError as exc:
            self.zones_status_var.set(f"Set Model: {exc}")
            return

        def apply(result) -> None:
            if self._is_open("zones"):
                if result.ok:
                    self.zones_status_var.set(f"Zone {zone} model set.")
                    self.zones_refresh_async()
                else:
                    detail = f" ({result.reason})" if result.reason else ""
                    self.zones_status_var.set(f"Zone {zone} model REJECTED{detail}.")

        self.query_async(
            f"Set zone {zone} model",
            lambda: self.control.set_zone_model(zone, k_dc, tau_s, dead_time_s),
            apply,
            error_types=(ControlQueryError,),
        )

