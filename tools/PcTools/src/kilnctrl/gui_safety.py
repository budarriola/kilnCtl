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

from . import dashboard_http_client, devices, host_resolve, pin_overlay, pinout_reference, settings, wifi_credentials
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
#: Above this, the ESP's cached Pico status is old enough to call out in the
#: status-bar summary rather than silently keep showing the last good text --
#: comfortably more than one poll period's worth of Pico->ESP reporting so an
#: ordinary jitter tick doesn't flap the label.
_SAFETY_STALE_AGE_MS = 5000
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
        polls the ESP's *cache*, so it never waits on the isolated link.

        Runs whenever connected, independent of whether the Safety Processor
        popup is open: the status-bar summary column (see gui.py's
        _build_status_bar) needs live data too. Stopped explicitly on
        disconnect (toggle_connect) rather than gated on popup state."""
        self._safety_stop_poll()
        if not self.link.is_connected:
            return
        self._safety_poll_id = self.root.after(_SAFETY_POLL_MS, self._safety_poll_tick)

    def _safety_poll_tick(self) -> None:
        self._safety_poll_id = None
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
            on_error=self._on_safety_query_error,
        )
        # The borrowed-sensor check below is HTTP (GET /api/status), a
        # completely separate transport from the UART query above -- and
        # deliberately NOT run on the quiet 2 s tick (_safety_schedule_poll),
        # only on an explicit refresh (button click, or popup open via
        # gui.py's toggle_popup). Firing it every 2 s would add a new HTTP
        # round trip to a hot polling loop, which is exactly what this
        # feature must not do.
        if not quiet:
            self._safety_refresh_tc_separate_async()

    def _safety_refresh_tc_separate_async(self) -> None:
        """One-shot fetch of GET /api/status's `safety_tc_is_separate_sensor`
        (dashboard_status_http.c) -- the ESP's own confirmed-borrowed
        predicate (safety_tc_is_separate_physical_sensor() in
        firmware/KilnFW/App/drivers/safety/safety_link.h), same field the
        KilnFW LCD/web displays already gate on (see b90fcb3).

        Same fail-to-shown semantics as the firmware: a missing field
        (older firmware) or a failed fetch (board unreachable over HTTP,
        e.g. serial-only bench setup) leaves `_safety_tc_separate` at
        whatever it last was -- `None` at startup, which `_apply_safety_status`
        treats as "shown", never as "confirmed borrowed"."""
        def apply(data: dict) -> None:
            if isinstance(data, dict) and "safety_tc_is_separate_sensor" in data:
                self._safety_tc_separate = bool(data["safety_tc_is_separate_sensor"])

        self.query_async(
            "Safety TC borrowed-sensor check",
            lambda: dashboard_http_client.get_status(self._wifi_host()),
            apply,
            error_types=(dashboard_http_client.DashboardHttpError,),
            on_error=lambda exc: None,
        )

    def _on_safety_query_error(self, exc: BaseException) -> None:
        """A failed Safety status read must not leave the last good summary
        text on screen -- that would read as "still fine" when it's actually
        "we don't know" (see TODO.md's paired-state class: two things that
        looked consistent only because the failing side never updated)."""
        self.safety_summary_var.set("Safety: no reply")
        self.safety_summary_label.config(foreground=_BAD_COLOR)

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
        # Status-bar summary column: updated unconditionally, whether or not
        # the Safety Processor popup is open (see gui.py's _build_status_bar).
        if status.estop:
            self.safety_summary_var.set("Safety: E-STOP")
            self.safety_summary_label.config(foreground=_BAD_COLOR)
        elif status.fault_asserted:
            # The Pico's own fault output, not a locally-clean thermocouple
            # reading -- a fault-asserted board must never read OK just
            # because *our* TC happens to be fine.
            self.safety_summary_var.set("Safety: FAULT asserted")
            self.safety_summary_label.config(foreground=_BAD_COLOR)
        elif not status.link_up:
            self.safety_summary_var.set("Safety: link down (expected, no RP2040 FW)")
            self.safety_summary_label.config(foreground=_EXPECTED_COLOR)
        elif not status.never_received and status.age_ms > _SAFETY_STALE_AGE_MS:
            self.safety_summary_var.set(f"Safety: stale ({status.age_ms // 1000}s)")
            self.safety_summary_label.config(foreground=_EXPECTED_COLOR)
        elif status.fault_labels:
            self.safety_summary_var.set("Safety: TC fault")
            self.safety_summary_label.config(foreground=_BAD_COLOR)
        else:
            self.safety_summary_var.set(
                "Safety: OK" + (" (enabled)" if status.enabled else "")
            )
            self.safety_summary_label.config(foreground=_OK_COLOR)

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

        # Fail-to-shown: only an explicit False (a fresh GET /api/status
        # confirmed safety_tc_is_separate_sensor == false) hides the
        # temperature. `None` (never fetched yet, older firmware, or the
        # last fetch failed) is treated the same as True -- shown -- exactly
        # the firmware's own "unknown must fail to shown" rule (see
        # safety_tc_is_separate_physical_sensor()'s doc comment).
        tc_separate = getattr(self, "_safety_tc_separate", None)
        if tc_separate is False:
            show(
                "temp",
                "borrowed from a zone probe (same probe, not a second sensor)",
                _MUTED_COLOR,
            )
        elif status.temp_valid:
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

