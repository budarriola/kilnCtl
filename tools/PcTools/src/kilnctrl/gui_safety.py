"""SAFETY popup (task 7).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
SafetyMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class SafetyMixin:
    # ======================================================================
    # SAFETY page (task 7)
    # ======================================================================
    def _build_safety_popup(self, top: tk.Toplevel) -> None:
        """Status of the isolated RP2040 link, plus the controls this side owns.

        The banner at the top is load-bearing, not decoration: the Pico
        firmware does not exist yet, so a down link is the *expected* state and
        must not be painted as a fault -- otherwise every future user of this
        page starts by debugging a non-problem.
        """
        banner = tk.Label(
            top,
            text=(
                "The RP2040 safety-processor firmware does not exist yet.\n"
                "\"Link: down\" with age \"never received\" is the EXPECTED state today -- "
                "it is not a fault to chase.\n"
                "Everything below except the Fault line is the Pico's view, relayed "
                "through the ESP's cache of its last good poll."
            ),
            background="#fff4d6",
            foreground="#6b4e00",
            justify="left",
            anchor="w",
            padx=10,
            pady=8,
            relief="groove",
        )
        banner.pack(fill="x", padx=8, pady=(8, 4))

        status = ttk.LabelFrame(top, text="Status (cached by the ESP, refreshed every 2 s)")
        status.pack(fill="x", padx=8, pady=4)

        self._safety_vars: dict[str, tk.StringVar] = {}
        self._safety_labels: dict[str, ttk.Label] = {}
        rows = (
            ("link", "Isolated link"),
            ("age", "Data age"),
            ("fault_out", "Fault line (we drive it)"),
            ("estop", "E-stop"),
            ("relay", "Safety relay K4"),
            ("enabled", "Heating enable"),
            ("temp", "Safety thermocouple"),
            ("faults", "Safety TC faults"),
            ("current1", "Current sense 1 (J13)"),
            ("current2", "Current sense 2 (J15)"),
            ("current3", "Current sense 3 (J17)"),
        )
        for row, (key, label) in enumerate(rows):
            var = tk.StringVar(value="---")
            self._safety_vars[key] = var
            ttk.Label(status, text=label, width=24, anchor="w").grid(
                row=row, column=0, padx=(8, 4), pady=2, sticky="w"
            )
            value_label = ttk.Label(status, textvariable=var, anchor="w")
            value_label.grid(row=row, column=1, padx=4, sticky="w")
            self._safety_labels[key] = value_label
        status.columnconfigure(1, weight=1)

        stats = ttk.LabelFrame(top, text="Isolated UART counters (the ESP's own, they move regardless)")
        stats.pack(fill="x", padx=8, pady=4)
        self.safety_stats_var = tk.StringVar(value="not read yet")
        ttk.Label(stats, textvariable=self.safety_stats_var, foreground=_MUTED_COLOR).pack(
            side="left", padx=8, pady=6
        )
        ttk.Button(stats, text="Read Stats", command=self.safety_read_stats_async).pack(
            side="right", padx=8, pady=6
        )

        controls = ttk.LabelFrame(top, text="Controls")
        controls.pack(fill="x", padx=8, pady=(4, 8))
        ttk.Button(controls, text="Refresh Now", command=self.safety_refresh_async).grid(
            row=0, column=0, padx=(8, 4), pady=6
        )
        ttk.Button(
            controls,
            text="Ping",
            command=lambda: self.send_async(
                "Safety ping", UART_TASK_ID_SAFETY, devices.safety_ping
            ),
        ).grid(row=0, column=1, padx=4)
        ttk.Button(
            controls,
            text="Request Enable",
            command=lambda: self._safety_mutating_async(
                "Safety request enable", lambda: self.safety.request_enable(True)
            ),
        ).grid(row=0, column=2, padx=(12, 4))
        ttk.Button(
            controls,
            text="Drop Enable",
            command=lambda: self._safety_mutating_async(
                "Safety drop enable", lambda: self.safety.request_enable(False)
            ),
        ).grid(row=0, column=3, padx=4)

        self.safety_poll_period = tk.IntVar(value=500)
        ttk.Label(controls, text="Poll period (ms)").grid(row=0, column=4, padx=(16, 2))
        ttk.Spinbox(
            controls, from_=0, to=60000, increment=100, textvariable=self.safety_poll_period, width=7
        ).grid(row=0, column=5, padx=2)
        ttk.Button(
            controls,
            text="Apply",
            command=lambda: self._safety_mutating_async(
                f"Safety poll period {self.safety_poll_period.get()} ms",
                lambda: self.safety.set_poll_period(self.safety_poll_period.get()),
            ),
        ).grid(row=0, column=6, padx=(2, 8))

        fault = ttk.Frame(controls)
        fault.grid(row=1, column=0, columnspan=7, padx=8, pady=(0, 6), sticky="w")
        ttk.Label(
            fault,
            text="Fault line (ESP GPIO6 -> U1 -> the Pico's mainFault): an OUTPUT we assert. "
            "The firmware already asserts it on PC-link loss, a thermocouple fault or a "
            "watchdog trip -- these are a manual override.",
            foreground=_MUTED_COLOR,
            justify="left",
            wraplength=620,
        ).pack(anchor="w", pady=(0, 4))
        ttk.Button(
            fault,
            text="Assert Fault",
            command=lambda: self._safety_mutating_async(
                "Safety assert fault", lambda: self.safety.set_fault_out(True)
            ),
        ).pack(side="left")
        ttk.Button(
            fault,
            text="Clear Fault",
            command=lambda: self._safety_mutating_async(
                "Safety clear fault", lambda: self.safety.set_fault_out(False)
            ),
        ).pack(side="left", padx=6)

    def _safety_mutating_async(self, description: str, action: Callable[[], OkReason]) -> None:
        """Run a SAFETY mutating command and surface a REJECTED reply (with
        reason, when the firmware gave one) instead of letting it land
        silently in SafetyClient's own consumer thread -- see
        :meth:`_thermo_mutating_async`'s docstring for why :meth:`send_async`
        is the wrong tool for these.
        """
        def apply(result: OkReason) -> None:
            if result.ok:
                self.set_status(f"{description}: ok.")
            else:
                detail = f" ({result.reason})" if result.reason else ""
                self.set_status(f"{description}: REJECTED{detail}.", error=True)

        self.query_async(description, action, apply, error_types=(SafetyQueryError,))

    def _safety_schedule_poll(self) -> None:
        """SAFETY has no auto-report subcommand, so this page polls -- but it
        polls the ESP's *cache*, so it never waits on the isolated link."""
        self._safety_stop_poll()
        if not self._is_open("safety"):
            return
        self._safety_poll_id = self.root.after(_SAFETY_POLL_MS, self._safety_poll_tick)

    def _safety_poll_tick(self) -> None:
        self._safety_poll_id = None
        if not self._is_open("safety"):
            return
        if self.link.is_connected and self.info.compatible is True:
            self.safety_refresh_async(quiet=True)
        self._safety_schedule_poll()

    def _safety_stop_poll(self) -> None:
        if self._safety_poll_id is not None:
            self.root.after_cancel(self._safety_poll_id)
            self._safety_poll_id = None

    def safety_refresh_async(self, quiet: bool = False) -> None:
        """Re-read the cached safety status.

        ``quiet`` suppresses the status-bar chatter for the background poll --
        a page refreshing every two seconds should not be overwriting the
        record of the last thing the user actually did.
        """
        if quiet and (not self.link.is_connected or self.info.compatible is not True):
            return
        self.query_async(
            "Safety status",
            lambda: self.safety.get_status(),
            self._apply_safety_status,
            error_types=(SafetyQueryError,),
        )

    def safety_read_stats_async(self) -> None:
        def apply(stats) -> None:
            if self._is_open("safety"):
                self.safety_stats_var.set(stats.describe())

        self.query_async(
            "Safety link stats",
            lambda: self.safety.get_link_stats(),
            apply,
            error_types=(SafetyQueryError,),
        )

    def _apply_safety_status(self, status: SafetyStatus) -> None:
        if not self._is_open("safety"):
            return

        def show(key: str, text: str, color: str) -> None:
            self._safety_vars[key].set(text)
            self._safety_labels[key].config(foreground=color)

        # A down link is amber, not red: with no Pico firmware in existence it
        # is the expected state, and colouring it as a fault would train the
        # user to ignore the one colour that should mean something.
        if status.link_up:
            show("link", "up", _OK_COLOR)
        else:
            show("link", "down (expected -- no RP2040 firmware yet)", _EXPECTED_COLOR)

        if status.never_received:
            show("age", "never received (expected today)", _EXPECTED_COLOR)
        else:
            show("age", f"{status.age_ms} ms", _MUTED_COLOR)

        # This one IS ours, so it is a real red/green: we drive GPIO6.
        show(
            "fault_out",
            "ASSERTED (we are signalling a fault to the Pico)" if status.fault_asserted else "not asserted",
            _BAD_COLOR if status.fault_asserted else _OK_COLOR,
        )
        show(
            "estop",
            "ASSERTED" if status.estop else "clear",
            _BAD_COLOR if status.estop else _OK_COLOR,
        )
        show("relay", "energized" if status.relay_energized else "open", _MUTED_COLOR)
        show(
            "enabled",
            "granted" if status.enabled else "not granted",
            _OK_COLOR if status.enabled else _MUTED_COLOR,
        )

        if status.temp_valid:
            show(
                "temp",
                f"{status.temperature_c:.2f} C  (CJ {status.cold_junction_c:.1f} C)",
                "",
            )
        else:
            show("temp", "invalid / no reading", _MUTED_COLOR)

        faults = status.fault_labels
        show(
            "faults",
            "; ".join(faults) if faults else "none",
            _BAD_COLOR if faults else _OK_COLOR,
        )
        for index, amps in enumerate(status.current_a, start=1):
            show(f"current{index}", f"{amps:.2f} A", _MUTED_COLOR)

