"""Autotune popup (task 10, AUTOTUNE).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
AutotuneMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class AutotuneMixin:
    # ======================================================================
    # Autotune panel (task 10, AUTOTUNE)
    # ======================================================================
    def open_autotune_popup(self) -> None:
        self._popup("autotune", "Autotune (UART)", self._build_autotune_popup)
        self.autotune_refresh_async()

    def _build_autotune_popup(self, top: tk.Toplevel) -> None:
        top.geometry("480x420")
        status_frame = ttk.LabelFrame(top, text="Status")
        status_frame.pack(fill="x", padx=8, pady=(8, 4))
        self.autotune_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            status_frame, textvariable=self.autotune_status_var, anchor="w", justify="left",
            wraplength=440,
        ).pack(fill="x", padx=6, pady=6)
        ttk.Button(status_frame, text="Refresh", command=self.autotune_refresh_async).pack(
            anchor="w", padx=6, pady=(0, 6)
        )

        start_frame = ttk.LabelFrame(top, text="Start")
        start_frame.pack(fill="x", padx=8, pady=4)
        row = ttk.Frame(start_frame)
        row.pack(fill="x", padx=6, pady=6)
        ttk.Label(row, text="Zone:").pack(side="left")
        self.autotune_zone_var = tk.StringVar(value="0")
        ttk.Entry(row, textvariable=self.autotune_zone_var, width=4).pack(side="left", padx=(2, 8))
        ttk.Label(row, text="Method:").pack(side="left")
        self.autotune_method_var = tk.StringVar(value="step")
        ttk.Combobox(
            row, textvariable=self.autotune_method_var, values=("step", "relay"),
            state="readonly", width=8,
        ).pack(side="left", padx=(2, 8))
        row2 = ttk.Frame(start_frame)
        row2.pack(fill="x", padx=6, pady=(0, 6))
        ttk.Label(row2, text="Step duty / setpoint C:").pack(side="left")
        self.autotune_value_var = tk.StringVar(value="0.5")
        ttk.Entry(row2, textvariable=self.autotune_value_var, width=8).pack(side="left", padx=(2, 8))
        row3 = ttk.Frame(start_frame)
        row3.pack(fill="x", padx=6, pady=(0, 6))
        ttk.Label(row3, text="Relay d (<=0=default):").pack(side="left")
        self.autotune_relay_d_var = tk.StringVar(value="-1")
        ttk.Entry(row3, textvariable=self.autotune_relay_d_var, width=6).pack(side="left", padx=(2, 8))
        ttk.Label(row3, text="Relay h C (<=0=default):").pack(side="left")
        self.autotune_relay_h_var = tk.StringVar(value="-1")
        ttk.Entry(row3, textvariable=self.autotune_relay_h_var, width=6).pack(side="left", padx=(2, 8))
        ttk.Label(row3, text="Rule:").pack(side="left")
        self.autotune_rule_var = tk.StringVar(value="tl")
        ttk.Combobox(
            row3, textvariable=self.autotune_rule_var, values=("tl", "zn"), state="readonly", width=4,
        ).pack(side="left", padx=(2, 8))
        ttk.Button(start_frame, text="Start", command=self.autotune_start_async).pack(
            anchor="w", padx=6, pady=(0, 6)
        )

        action_frame = ttk.Frame(top)
        action_frame.pack(fill="x", padx=8, pady=(4, 8))
        ttk.Button(action_frame, text="Abort", command=self.autotune_abort_async).pack(side="left")
        ttk.Button(action_frame, text="Accept", command=self.autotune_accept_async).pack(
            side="left", padx=6
        )
        ttk.Label(
            top,
            text="Cross-zone coupling matrix and trace/history CSV are HTTP-only "
            "(Open Web Dashboard) -- not mirrored over UART.",
            foreground=_MUTED_COLOR, wraplength=440, justify="left",
        ).pack(fill="x", padx=8, pady=(0, 8))

    def autotune_refresh_async(self) -> None:
        if not self._is_open("autotune"):
            return
        self.query_async(
            "Autotune status", lambda: self.autotune_client.get_status(), self._apply_autotune_status,
            error_types=(AutotuneQueryError,),
        )

    def _apply_autotune_status(self, status) -> None:
        if not self._is_open("autotune"):
            return
        lines = [
            f"State: {status.state_name}   Method: {'relay' if status.method else 'step'}   "
            f"Zone: {status.zone}   Elapsed: {status.elapsed_s}s   Samples: {status.sample_count}",
        ]
        if status.actual_valid:
            lines.append(f"Actual: {status.actual_c:.1f}C   Duty: {status.duty:.2f}")
        if status.state == 6 and status.abort_reason:
            lines.append(f"Aborted: {status.abort_reason}")
        if status.state == 5 and status.model_valid:
            m = status.model
            g = status.proposed_gains
            lines.append(f"Fitted K={m.k_gain_c_per_duty:.2f}  tau={m.tau_s:.0f}s  L={m.dead_time_s:.0f}s")
            lines.append(f"Proposed Kp={g.kp:.4f} Ki={g.ki:.5f} Kd={g.kd:.4f}")
            lines.append(f"Predicted max ramp: ~{status.predicted_max_ramp_c_per_hr:.0f} C/hr")
        if status.state == 5 and status.relay_valid:
            r = status.relay
            lines.append(f"Relay: Ku={r.ku:.3f}  Tu={r.tu_s:.0f}s  amplitude={r.amplitude_c:.1f}C")
        self.autotune_status_var.set("\n".join(lines))

    def autotune_start_async(self) -> None:
        try:
            zone = int(self.autotune_zone_var.get())
            method = AUTOTUNE_METHOD_RELAY if self.autotune_method_var.get() == "relay" else AUTOTUNE_METHOD_STEP
            value = float(self.autotune_value_var.get())
            relay_d = float(self.autotune_relay_d_var.get())
            relay_h = float(self.autotune_relay_h_var.get())
            rule = AUTOTUNE_RULE_ZN if self.autotune_rule_var.get() == "zn" else AUTOTUNE_RULE_TL
        except ValueError as exc:
            self.autotune_status_var.set(f"Start: {exc}")
            return

        def apply(result) -> None:
            if not self._is_open("autotune"):
                return
            ok, error = result
            self.autotune_status_var.set(f"Start: ok" if ok else f"Start REJECTED: {error}")
            self.autotune_refresh_async()

        self.query_async(
            "Start autotune",
            lambda: self.autotune_client.start(zone, method, value, relay_d, relay_h, rule),
            apply,
            error_types=(AutotuneQueryError,),
        )

    def autotune_abort_async(self) -> None:
        def apply(ok: bool) -> None:
            if self._is_open("autotune"):
                self.autotune_status_var.set(f"Abort: {'ok' if ok else 'failed'}.")
                self.autotune_refresh_async()

        self.query_async("Abort autotune", lambda: self.autotune_client.abort(), apply,
                          error_types=(AutotuneQueryError,))

    def autotune_accept_async(self) -> None:
        def apply(ok: bool) -> None:
            if self._is_open("autotune"):
                self.autotune_status_var.set(f"Accept: {'ok' if ok else 'REJECTED'}.")
                self.autotune_refresh_async()

        self.query_async("Accept autotune", lambda: self.autotune_client.accept(), apply,
                          error_types=(AutotuneQueryError,))

