"""THERMO popup (task 1).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
ThermoMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class ThermoMixin:
    # ======================================================================
    # THERMO page (task 1)
    # ======================================================================
    def _build_thermo_popup(self, top: tk.Toplevel) -> None:
        """Live readout per channel (fed by the firmware's auto-report push,
        not by polling), plus configuration, thresholds and raw registers."""
        self._thermo_temp_vars: list[tk.StringVar] = []
        self._thermo_cj_vars: list[tk.StringVar] = []
        self._thermo_fault_vars: list[tk.StringVar] = []
        self._thermo_temp_labels: list[ttk.Label] = []
        self._thermo_fault_labels: list[ttk.Label] = []

        # -- live readout ---------------------------------------------------
        live = ttk.LabelFrame(top, text="Live readings (pushed by the device, not polled)")
        live.pack(fill="x", padx=8, pady=(8, 4))
        for channel in range(THERMO_CHANNEL_COUNT):
            temp_var = tk.StringVar(value="--- C")
            cj_var = tk.StringVar(value="CJ ---")
            fault_var = tk.StringVar(value="no data yet")
            self._thermo_temp_vars.append(temp_var)
            self._thermo_cj_vars.append(cj_var)
            self._thermo_fault_vars.append(fault_var)

            ttk.Label(live, text=f"CH{channel}", font=("Segoe UI", 10, "bold")).grid(
                row=channel, column=0, padx=(8, 4), pady=4, sticky="w"
            )
            temp_label = ttk.Label(
                live, textvariable=temp_var, font=("Segoe UI", 14, "bold"), width=12, anchor="e"
            )
            temp_label.grid(row=channel, column=1, padx=4, pady=4)
            self._thermo_temp_labels.append(temp_label)
            ttk.Label(live, textvariable=cj_var, width=14, anchor="e", foreground=_MUTED_COLOR).grid(
                row=channel, column=2, padx=4
            )
            # Faults as readable text, never as a hex byte: "open circuit (no
            # thermocouple?)" is actionable, "SR=0x01" is homework.
            fault_label = ttk.Label(live, textvariable=fault_var, foreground=_MUTED_COLOR, anchor="w")
            fault_label.grid(row=channel, column=3, padx=(10, 8), sticky="w")
            self._thermo_fault_labels.append(fault_label)
            ttk.Button(
                live,
                text="One Shot",
                width=9,
                command=lambda c=channel: self._thermo_mutating_async(
                    f"Thermo CH{c} one-shot", lambda: self.thermo.one_shot(c)
                ),
            ).grid(row=channel, column=4, padx=2)
            ttk.Button(
                live,
                text="Clear Faults",
                width=11,
                command=lambda c=channel: self._thermo_mutating_async(
                    f"Thermo CH{c} clear faults", lambda: self.thermo.clear_faults(c)
                ),
            ).grid(row=channel, column=5, padx=(2, 8))
        live.columnconfigure(3, weight=1)

        report_row = ttk.Frame(top)
        report_row.pack(fill="x", padx=8, pady=(0, 4))
        self.thermo_period_var = tk.IntVar(value=_THERMO_REPORT_PERIOD_MS)
        ttk.Label(report_row, text="Auto-report period (ms):").pack(side="left")
        ttk.Spinbox(
            report_row, from_=0, to=60000, increment=100, textvariable=self.thermo_period_var, width=8
        ).pack(side="left", padx=(4, 6))
        ttk.Button(report_row, text="Apply", command=self._thermo_start_reporting).pack(side="left")
        ttk.Button(report_row, text="Stop", command=self._thermo_stop_reporting).pack(
            side="left", padx=6
        )
        ttk.Button(report_row, text="Read Now", command=self.thermo_read_async).pack(side="left")
        ttk.Button(report_row, text="Read Faults", command=self.thermo_read_faults_async).pack(
            side="left", padx=6
        )

        # -- configuration --------------------------------------------------
        config = ttk.LabelFrame(top, text="Channel configuration")
        config.pack(fill="x", padx=8, pady=4)
        self.thermo_cfg_channel = tk.IntVar(value=0)
        self.thermo_tc_type = tk.StringVar(value=devices.TC_TYPE_LABELS[TcType.K])
        self.thermo_avg = tk.StringVar(value=devices.AVG_MODE_LABELS[AvgMode.AVG_1])
        self.thermo_filter_50 = tk.BooleanVar(value=False)
        self.thermo_auto_convert = tk.BooleanVar(value=True)

        ttk.Label(config, text="Channel").grid(row=0, column=0, padx=(8, 2), pady=6)
        ttk.Spinbox(
            config,
            from_=0,
            to=THERMO_CHANNEL_COUNT - 1,
            textvariable=self.thermo_cfg_channel,
            width=4,
        ).grid(row=0, column=1, padx=2)
        ttk.Label(config, text="Type").grid(row=0, column=2, padx=(10, 2))
        ttk.Combobox(
            config,
            textvariable=self.thermo_tc_type,
            state="readonly",
            width=18,
            values=[devices.TC_TYPE_LABELS[t] for t in TcType],
        ).grid(row=0, column=3, padx=2)
        ttk.Label(config, text="Averaging").grid(row=0, column=4, padx=(10, 2))
        ttk.Combobox(
            config,
            textvariable=self.thermo_avg,
            state="readonly",
            width=11,
            values=[devices.AVG_MODE_LABELS[a] for a in AvgMode],
        ).grid(row=0, column=5, padx=2)
        ttk.Checkbutton(config, text="50 Hz filter", variable=self.thermo_filter_50).grid(
            row=0, column=6, padx=(10, 2)
        )
        ttk.Checkbutton(
            config, text="Auto convert", variable=self.thermo_auto_convert
        ).grid(row=0, column=7, padx=2)
        ttk.Button(config, text="Apply Config", command=self._thermo_apply_config).grid(
            row=0, column=8, padx=(10, 8)
        )

        # -- thresholds / cold junction -------------------------------------
        limits = ttk.LabelFrame(top, text="Thresholds and cold-junction offset")
        limits.pack(fill="x", padx=8, pady=4)
        self.thermo_tc_high = tk.StringVar(value="1300.0")
        self.thermo_tc_low = tk.StringVar(value="-20.0")
        self.thermo_cj_high = tk.IntVar(value=85)
        self.thermo_cj_low = tk.IntVar(value=-20)
        self.thermo_cj_offset = tk.StringVar(value="0.0")

        for column, (text, var, width) in enumerate(
            (
                ("TC high (C)", self.thermo_tc_high, 9),
                ("TC low (C)", self.thermo_tc_low, 9),
            )
        ):
            ttk.Label(limits, text=text).grid(row=0, column=column * 2, padx=(8, 2), pady=6)
            ttk.Entry(limits, textvariable=var, width=width).grid(
                row=0, column=column * 2 + 1, padx=2
            )
        ttk.Label(limits, text="CJ high (C)").grid(row=0, column=4, padx=(10, 2))
        ttk.Spinbox(limits, from_=-128, to=127, textvariable=self.thermo_cj_high, width=6).grid(
            row=0, column=5, padx=2
        )
        ttk.Label(limits, text="CJ low (C)").grid(row=0, column=6, padx=(10, 2))
        ttk.Spinbox(limits, from_=-128, to=127, textvariable=self.thermo_cj_low, width=6).grid(
            row=0, column=7, padx=2
        )
        ttk.Button(limits, text="Set Thresholds", command=self._thermo_apply_thresholds).grid(
            row=0, column=8, padx=(10, 8)
        )
        ttk.Label(limits, text="CJ offset (C, -8..+8)").grid(
            row=1, column=0, columnspan=2, padx=(8, 2), pady=(0, 6), sticky="w"
        )
        ttk.Entry(limits, textvariable=self.thermo_cj_offset, width=9).grid(
            row=1, column=2, padx=2, pady=(0, 6)
        )
        ttk.Button(limits, text="Set CJ Offset", command=self._thermo_apply_cj_offset).grid(
            row=1, column=3, padx=(6, 8), pady=(0, 6), sticky="w"
        )

        # -- raw registers ---------------------------------------------------
        raw = ttk.LabelFrame(top, text="Raw registers (debug)")
        raw.pack(fill="x", padx=8, pady=(4, 8))
        self.thermo_reg_addr = tk.StringVar(value="0x00")
        self.thermo_reg_len = tk.IntVar(value=1)
        self.thermo_reg_value = tk.StringVar(value="0x00")
        self.thermo_reg_result = tk.StringVar(value="")

        ttk.Label(raw, text="Reg").grid(row=0, column=0, padx=(8, 2), pady=6)
        ttk.Entry(raw, textvariable=self.thermo_reg_addr, width=7).grid(row=0, column=1, padx=2)
        ttk.Label(raw, text="Len").grid(row=0, column=2, padx=(8, 2))
        ttk.Spinbox(raw, from_=1, to=16, textvariable=self.thermo_reg_len, width=4).grid(
            row=0, column=3, padx=2
        )
        ttk.Button(raw, text="Read", command=self.thermo_read_reg_async).grid(
            row=0, column=4, padx=(8, 2)
        )
        ttk.Label(raw, text="Value").grid(row=0, column=5, padx=(12, 2))
        ttk.Entry(raw, textvariable=self.thermo_reg_value, width=7).grid(row=0, column=6, padx=2)
        ttk.Button(raw, text="Write", command=self._thermo_write_reg).grid(
            row=0, column=7, padx=(8, 8)
        )
        ttk.Label(raw, textvariable=self.thermo_reg_result, foreground=_MUTED_COLOR).grid(
            row=1, column=0, columnspan=8, padx=8, pady=(0, 6), sticky="w"
        )

    def _thermo_cfg_channel(self) -> int:
        return int(self.thermo_cfg_channel.get())

    def _thermo_apply_config(self) -> None:
        # The combobox shows the human label, so map back to the enum value
        # rather than parsing the text -- the labels are display strings and
        # are allowed to change without breaking the wire encoding.
        tc_type = next(
            (t for t in TcType if devices.TC_TYPE_LABELS[t] == self.thermo_tc_type.get()),
            TcType.K,
        )
        avg = next(
            (a for a in AvgMode if devices.AVG_MODE_LABELS[a] == self.thermo_avg.get()),
            AvgMode.AVG_1,
        )
        channel = self._thermo_cfg_channel()
        self._thermo_mutating_async(
            f"Thermo CH{channel} config",
            lambda: self.thermo.config_channel(
                channel,
                tc_type,
                avg,
                self.thermo_filter_50.get(),
                self.thermo_auto_convert.get(),
            ),
        )

    def _thermo_apply_thresholds(self) -> None:
        from .gui import _as_float  # local import: avoids a circular import with gui.py

        channel = self._thermo_cfg_channel()

        def action() -> OkReason:
            return self.thermo.set_thresholds(
                channel,
                _as_float(self.thermo_tc_high, "TC high"),
                _as_float(self.thermo_tc_low, "TC low"),
                self.thermo_cj_high.get(),
                self.thermo_cj_low.get(),
            )

        self._thermo_mutating_async(f"Thermo CH{channel} thresholds", action)

    def _thermo_apply_cj_offset(self) -> None:
        from .gui import _as_float  # local import: avoids a circular import with gui.py

        channel = self._thermo_cfg_channel()
        self._thermo_mutating_async(
            f"Thermo CH{channel} CJ offset",
            lambda: self.thermo.set_cj_offset(
                channel, _as_float(self.thermo_cj_offset, "CJ offset")
            ),
        )

    def _thermo_write_reg(self) -> None:
        from .gui import _as_int  # local import: avoids a circular import with gui.py

        channel = self._thermo_cfg_channel()
        self._thermo_mutating_async(
            f"Thermo CH{channel} write reg",
            lambda: self.thermo.write_reg(
                channel,
                _as_int(self.thermo_reg_addr, "register"),
                _as_int(self.thermo_reg_value, "value"),
            ),
        )

    def _thermo_start_reporting(self) -> None:
        """Turn on (or re-apply) the firmware's auto-report for all channels.

        This is what feeds the live readout. Deliberately a push rather than a
        GUI-side polling loop: at one frame per period the link stays free for
        commands, and the firmware batches all three channels into one frame.
        """
        if not self._is_open("thermo"):
            return
        period = max(0, int(self.thermo_period_var.get()))
        mask = (1 << THERMO_CHANNEL_COUNT) - 1
        self._thermo_mutating_async(
            f"Thermo auto-report {period} ms",
            lambda: self.thermo.set_auto_report(mask, period),
        )

    def _thermo_stop_reporting(self) -> None:
        """Turn auto-reporting off -- on window close too, so a closed page
        doesn't leave the firmware pushing frames nobody is rendering."""
        if not self.link.is_connected or self.info.compatible is not True:
            return
        self._thermo_mutating_async(
            "Thermo auto-report off", lambda: self.thermo.set_auto_report(0, 0)
        )

    def _thermo_mutating_async(self, description: str, action: Callable[[], OkReason]) -> None:
        """Run a THERMO mutating command and surface a REJECTED reply (with
        reason, when the firmware gave one) instead of letting it land
        silently in ThermoClient's own consumer thread -- same "wait a short
        window for the optional refusal" shape :meth:`thermo.ThermoClient`'s
        helpers use, just wired into the GUI's async/status-bar convention
        instead of :meth:`send_async` (which never waits for that reply).
        """
        def apply(result: OkReason) -> None:
            if result.ok:
                self.set_status(f"{description}: ok.")
            else:
                detail = f" ({result.reason})" if result.reason else ""
                self.set_status(f"{description}: REJECTED{detail}.", error=True)

        self.query_async(description, action, apply, error_types=(ThermoQueryError,))

    def thermo_read_async(self) -> None:
        self.query_async(
            "Thermo read",
            lambda: self.thermo.read(),
            self._apply_thermo_readings,
            error_types=(ThermoQueryError,),
        )

    def thermo_read_faults_async(self) -> None:
        def apply(faults) -> None:
            text = "; ".join(f.describe() for f in faults) or "no channels reported"
            self.set_status(f"Thermo faults: {text}")
            self.session_log.info("thermo faults: %s", text)

        self.query_async(
            "Thermo read faults",
            lambda: self.thermo.read_faults(),
            apply,
            error_types=(ThermoQueryError,),
        )

    def thermo_read_reg_async(self) -> None:
        from .gui import _as_int  # local import: avoids a circular import with gui.py

        channel = self._thermo_cfg_channel()
        try:
            reg = _as_int(self.thermo_reg_addr, "register")
            length = int(self.thermo_reg_len.get())
        except ValueError as exc:
            self.thermo_reg_result.set(str(exc))
            return

        def apply(registers) -> None:
            if self._is_open("thermo"):
                self.thermo_reg_result.set(registers.describe())

        self.query_async(
            "Thermo read reg",
            lambda: self.thermo.read_reg(channel, reg, length),
            apply,
            error_types=(ThermoQueryError,),
        )

    def _drain_thermo_reports(self) -> None:
        """Render auto-report pushes on the Tk thread.

        Only the newest batch is drawn: if the GUI fell behind (a modal dialog,
        a slow redraw), painting every buffered frame in turn would just be an
        animation of stale temperatures on the way to the current one.
        """
        latest: Optional[list[ThermoReading]] = None
        try:
            while True:
                latest = self.thermo_reports.get_nowait()
        except queue.Empty:
            pass
        if latest is not None:
            self._apply_thermo_readings(latest)

    def _apply_thermo_readings(self, readings: "list[ThermoReading]") -> None:
        if not self._is_open("thermo"):
            return
        for reading in readings:
            channel = reading.channel
            if not 0 <= channel < THERMO_CHANNEL_COUNT:
                continue  # a channel we have no row for; ignore rather than crash
            if reading.valid:
                self._thermo_temp_vars[channel].set(f"{reading.temperature_c:.2f} C")
                self._thermo_cj_vars[channel].set(f"CJ {reading.cold_junction_c:.1f} C")
            else:
                self._thermo_temp_vars[channel].set("--- C")
                self._thermo_cj_vars[channel].set("CJ ---")

            notes = reading.fault_labels + reading.flag_labels
            if notes:
                self._thermo_fault_vars[channel].set("; ".join(notes))
                self._thermo_fault_labels[channel].config(foreground=_BAD_COLOR)
                self._thermo_temp_labels[channel].config(foreground=_BAD_COLOR)
            else:
                self._thermo_fault_vars[channel].set("ok")
                self._thermo_fault_labels[channel].config(foreground=_OK_COLOR)
                self._thermo_temp_labels[channel].config(foreground="")

