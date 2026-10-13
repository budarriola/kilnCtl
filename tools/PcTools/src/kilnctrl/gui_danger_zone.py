"""Danger Zone popup (factory reset, watchdog panic toggle).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
DangerZoneMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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

from . import devices, factory_reset_guard, host_resolve, pin_overlay, pinout_reference, settings, wifi_credentials
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


class DangerZoneMixin:
    # ======================================================================
    # Danger Zone panel -- factory reset, over UART (SYSTEM_CMD_FACTORY_RESET)
    # so it works with no network. Destructive: double confirmation, scope
    # is explicit, no default action pre-selected as "convenient".
    # ======================================================================
    def open_danger_zone_popup(self) -> None:
        self._popup("danger_zone", "Danger Zone (Factory Reset)", self._build_danger_zone_popup)

    def _build_danger_zone_popup(self, top: tk.Toplevel) -> None:
        tk.Label(
            top,
            text="FACTORY RESET erases NVS-backed configuration and reboots the device.\n"
            "This cannot be undone. Choose the narrowest scope that does what you need.",
            fg=_BAD_COLOR, wraplength=440, justify="left",
        ).pack(fill="x", padx=8, pady=(8, 8))

        self.danger_scope_var = tk.StringVar(value="wifi")
        for value, label in (
            ("wifi", "Wi-Fi only (saved networks, AP identity)"),
            ("kiln", "Kiln config only (zones, PID)"),
            ("profiles", "Fire profiles only"),
            ("all", "ALL of the above"),
        ):
            ttk.Radiobutton(
                top, text=label, variable=self.danger_scope_var, value=value
            ).pack(anchor="w", padx=16, pady=2)

        self.danger_status_var = tk.StringVar(value="")
        ttk.Label(top, textvariable=self.danger_status_var, foreground=_BAD_COLOR).pack(
            fill="x", padx=8, pady=(8, 4)
        )
        ttk.Button(
            top, text="Factory Reset...", command=self.danger_zone_confirm
        ).pack(padx=8, pady=(0, 8))

        ttk.Separator(top, orient="horizontal").pack(fill="x", padx=8, pady=8)

        tk.Label(
            top,
            text="Task-watchdog PANIC (bench debugging only): when disabled, a hung "
            "task no longer reboots the board -- it sits hung with relays in "
            "whatever state they were last commanded. Persists across reboots.",
            fg=_BAD_COLOR, wraplength=440, justify="left",
        ).pack(fill="x", padx=8, pady=(0, 4))
        self.watchdog_panic_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(top, textvariable=self.watchdog_panic_status_var, anchor="w").pack(
            fill="x", padx=8, pady=(0, 4)
        )
        wd_row = ttk.Frame(top)
        wd_row.pack(fill="x", padx=8, pady=(0, 8))
        ttk.Button(
            wd_row, text="Get", command=self.watchdog_panic_get_async
        ).pack(side="left")
        ttk.Button(
            wd_row, text="Disable Panic...", command=lambda: self.watchdog_panic_set_async(True)
        ).pack(side="left", padx=(6, 0))
        ttk.Button(
            wd_row, text="Enable Panic", command=lambda: self.watchdog_panic_set_async(False)
        ).pack(side="left", padx=(6, 0))

    def watchdog_panic_get_async(self) -> None:
        def apply(disabled: bool) -> None:
            self.watchdog_panic_status_var.set(
                f"Watchdog panic disabled: {disabled}"
            )
        self.query_async(
            "Get watchdog panic disabled",
            lambda: self.system.get_watchdog_panic_disabled(),
            apply,
            error_types=(SystemQueryError,),
        )

    def watchdog_panic_set_async(self, disabled: bool) -> None:
        if disabled and not messagebox.askyesno(
            "Confirm Disable Watchdog Panic",
            "Development-only setting. With the panic disabled, a hung task will "
            "NOT reboot the board -- it will sit hung with relays in whatever "
            "state they were last commanded. Never leave this disabled on a "
            "board that will actually fire a kiln. Continue?",
            icon="warning", parent=self.root,
        ):
            return
        self.watchdog_panic_status_var.set(
            f"Sending set watchdog panic disabled={disabled}..."
        )
        self.send_async(
            f"Set watchdog panic disabled={disabled}",
            UART_TASK_ID_SYSTEM,
            lambda: devices.system_set_watchdog_panic_disabled(disabled),
        )
        self.session_log.info("watchdog panic disabled set to %s", disabled)
        self.watchdog_panic_status_var.set(
            f"Set watchdog panic disabled={disabled} sent -- no reply frame; "
            "click Get to confirm the applied value."
        )

    def danger_zone_confirm(self) -> None:
        scope_name = self.danger_scope_var.get()
        scope_values = {"wifi": 0, "kiln": 1, "profiles": 2, "all": 3}
        scope = scope_values[scope_name]
        if not messagebox.askyesno(
            "Confirm Factory Reset",
            f"This will ERASE the {scope_name!r} configuration and REBOOT the device.\n\n"
            "This cannot be undone. Continue?",
            icon="warning", parent=self.root,
        ):
            return
        # Second confirmation via typed text -- a destructive action on real
        # hardware deserves more friction than one dialog click.
        typed = simpledialog.askstring(
            "Type to confirm",
            f"Type RESET to erase {scope_name!r} configuration:",
            parent=self.root,
        )
        if typed != "RESET":
            self.danger_status_var.set("Factory reset cancelled (confirmation text did not match).")
            return

        # Backup first (docs/audits/KILN_NVS_LOSS_2026-10-09.md): refuse if it fails.
        self.danger_status_var.set("Exporting pre-reset backup...")
        self.root.update_idletasks()
        ok, backup = factory_reset_guard.backup_before_reset()
        if not ok:
            self.danger_status_var.set(backup)
            self.session_log.error("factory reset refused: %s", backup)
            return
        self.session_log.info("pre-reset backup saved: %s", backup)

        self.danger_status_var.set(f"Sending factory reset ({scope_name})...")

        self.send_async(
            f"Factory reset ({scope_name})",
            UART_TASK_ID_SYSTEM,
            lambda: devices.system_factory_reset(scope),
        )
        self.session_log.info("factory reset requested: scope=%s", scope_name)
        self.danger_status_var.set(
            f"Factory reset ({scope_name}) sent. Device will erase and reboot "
            "~500ms after the ACK -- watch the Device Console / FW version line. "
            f"Backup: {backup}. Log this reset in docs/BENCH_TEST_LOG.md."
        )

