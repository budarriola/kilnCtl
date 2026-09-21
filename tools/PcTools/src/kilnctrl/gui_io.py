"""IO popup (task 2).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
IoMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class IoMixin:
    # ======================================================================
    # IO page (task 2)
    # ======================================================================
    def _build_io_popup(self, top: tk.Toplevel) -> None:
        """Relays (with their K designators and terminal blocks), digital I/O
        with direction control, live DRDY indicators, and raw registers."""
        from .gui import _as_int  # local import: avoids a circular import with gui.py

        self._io_relay_vars: list[tk.BooleanVar] = []
        self._io_level_vars: list[tk.StringVar] = []
        self._io_dir_vars: list[tk.BooleanVar] = []
        self._io_pullup_vars: list[tk.BooleanVar] = []
        self._io_drdy_vars: list[tk.StringVar] = []
        self._io_drdy_labels: list[ttk.Label] = []

        # -- relays -----------------------------------------------------------
        # The schematic's relay numbers are the expander's bit order and do NOT
        # match the K designators (Relay1 is K3, not K1). Showing all three
        # names on every row is the whole point: this is a kiln, and switching
        # the wrong contactor because the numbering looked obvious is exactly
        # the mistake worth designing out of the UI.
        relays = ttk.LabelFrame(
            top, text="Relays  --  note: relay number != K designator (see each row)"
        )
        relays.pack(fill="x", padx=8, pady=(8, 4))
        for relay in range(1, IO_RELAY_COUNT + 1):
            designator, block = devices.RELAY_DESIGNATORS[relay]
            var = tk.BooleanVar(value=False)
            self._io_relay_vars.append(var)
            row = relay - 1
            ttk.Label(relays, text=f"Relay{relay}", font=("Segoe UI", 9, "bold")).grid(
                row=row, column=0, padx=(8, 4), pady=3, sticky="w"
            )
            ttk.Label(relays, text=f"{designator}  ->  {block}", width=14, foreground=_MUTED_COLOR).grid(
                row=row, column=1, padx=4, sticky="w"
            )
            # command= (not a trace) so a programmatic .set() from a read-back
            # can't fire a write back at the device.
            ttk.Checkbutton(
                relays,
                text="energized",
                variable=var,
                command=lambda r=relay: self._io_write_relay(r),
            ).grid(row=row, column=2, padx=4)
        ttk.Button(
            relays,
            text="ALL RELAYS OFF",
            command=lambda: self._io_mutating_async(
                "All relays off", lambda: self.io.all_relays_off()
            ),
        ).grid(row=0, column=3, rowspan=2, padx=(20, 8), pady=4, sticky="ns")
        ttk.Label(
            relays,
            text="12 V coils, low-side switched: checked = coil energized.\n"
            "J3/J4/J8/J11 are 3-pin terminal blocks (NC/COM/NO).",
            foreground=_MUTED_COLOR,
            justify="left",
        ).grid(row=2, column=3, rowspan=2, padx=(20, 8), sticky="w")

        # -- digital I/O -------------------------------------------------------
        digital = ttk.LabelFrame(top, text="Digital I/O")
        digital.pack(fill="x", padx=8, pady=4)
        for io in range(1, IO_DIGITAL_COUNT + 1):
            level_var = tk.StringVar(value="?")
            dir_var = tk.BooleanVar(value=True)
            pullup_var = tk.BooleanVar(value=False)
            self._io_level_vars.append(level_var)
            self._io_dir_vars.append(dir_var)
            self._io_pullup_vars.append(pullup_var)
            row = io - 1

            ttk.Label(digital, text=f"IO {io}", font=("Segoe UI", 9, "bold")).grid(
                row=row, column=0, padx=(8, 4), pady=2, sticky="w"
            )
            ttk.Label(
                digital, text=devices.DIGITAL_IO_LABELS[io], width=34, foreground=_MUTED_COLOR
            ).grid(row=row, column=1, padx=4, sticky="w")
            ttk.Label(digital, text="reads").grid(row=row, column=2, padx=(8, 2))
            ttk.Label(digital, textvariable=level_var, width=3, anchor="center").grid(
                row=row, column=3, padx=2
            )
            ttk.Checkbutton(
                digital, text="input", variable=dir_var, command=lambda i=io: self._io_write_dir(i)
            ).grid(row=row, column=4, padx=(10, 2))
            ttk.Checkbutton(
                digital, text="pull-up", variable=pullup_var, command=lambda i=io: self._io_write_dir(i)
            ).grid(row=row, column=5, padx=2)
            ttk.Button(
                digital,
                text="Drive High",
                width=10,
                command=lambda i=io: self._io_write_level(i, True),
            ).grid(row=row, column=6, padx=(10, 2))
            ttk.Button(
                digital,
                text="Drive Low",
                width=10,
                command=lambda i=io: self._io_write_level(i, False),
            ).grid(row=row, column=7, padx=(2, 8))

        # -- DRDY + link state -------------------------------------------------
        status = ttk.LabelFrame(
            top, text="Thermocouple ~DRDY (these reach the expander, not the ESP32)"
        )
        status.pack(fill="x", padx=8, pady=4)
        for channel in range(THERMO_CHANNEL_COUNT):
            var = tk.StringVar(value="?")
            self._io_drdy_vars.append(var)
            ttk.Label(status, text=f"CH{channel} DRDY").grid(
                row=0, column=channel * 2, padx=(8, 2), pady=6
            )
            label = ttk.Label(status, textvariable=var, width=8, anchor="w")
            label.grid(row=0, column=channel * 2 + 1, padx=(0, 8))
            self._io_drdy_labels.append(label)

        self.io_status_var = tk.StringVar(value="no data yet")
        ttk.Label(status, textvariable=self.io_status_var, foreground=_MUTED_COLOR).grid(
            row=1, column=0, columnspan=6, padx=8, pady=(0, 6), sticky="w"
        )

        report_row = ttk.Frame(top)
        report_row.pack(fill="x", padx=8, pady=(0, 4))
        self.io_period_var = tk.IntVar(value=_IO_REPORT_PERIOD_MS)
        ttk.Label(report_row, text="Auto-report period (ms):").pack(side="left")
        ttk.Spinbox(
            report_row, from_=0, to=60000, increment=100, textvariable=self.io_period_var, width=8
        ).pack(side="left", padx=(4, 6))
        ttk.Button(report_row, text="Apply", command=self._io_start_reporting).pack(side="left")
        ttk.Button(report_row, text="Stop", command=self._io_stop_reporting).pack(side="left", padx=6)
        ttk.Button(report_row, text="Read Now", command=self.io_read_async).pack(side="left")
        ttk.Button(report_row, text="Scan I2C", command=self.io_scan_async).pack(side="left", padx=6)
        ttk.Label(
            report_row,
            text="(the device also pushes immediately on every ~INT edge)",
            foreground=_MUTED_COLOR,
        ).pack(side="left", padx=6)

        # -- raw registers ------------------------------------------------------
        raw = ttk.LabelFrame(top, text="Raw SX1509 registers and masks (debug)")
        raw.pack(fill="x", padx=8, pady=(4, 8))
        self.io_reg_addr = tk.StringVar(value="0x00")
        self.io_reg_len = tk.IntVar(value=1)
        self.io_reg_value = tk.StringVar(value="0x00")
        self.io_mask_var = tk.StringVar(value="0x0000")
        self.io_mask_extra = tk.StringVar(value="0")
        self.io_reg_result = tk.StringVar(value="")

        ttk.Label(raw, text="Reg").grid(row=0, column=0, padx=(8, 2), pady=6)
        ttk.Entry(raw, textvariable=self.io_reg_addr, width=7).grid(row=0, column=1, padx=2)
        ttk.Label(raw, text="Len").grid(row=0, column=2, padx=(8, 2))
        ttk.Spinbox(raw, from_=1, to=16, textvariable=self.io_reg_len, width=4).grid(
            row=0, column=3, padx=2
        )
        ttk.Button(raw, text="Read", command=self.io_read_reg_async).grid(row=0, column=4, padx=(8, 2))
        ttk.Label(raw, text="Value").grid(row=0, column=5, padx=(12, 2))
        ttk.Entry(raw, textvariable=self.io_reg_value, width=7).grid(row=0, column=6, padx=2)
        ttk.Button(
            raw,
            text="Write",
            command=lambda: self._io_mutating_async(
                "Expander write reg",
                lambda: self.io.sx_write_reg(
                    _as_int(self.io_reg_addr, "register"), _as_int(self.io_reg_value, "value")
                ),
            ),
        ).grid(row=0, column=7, padx=(8, 8))

        ttk.Label(raw, text="Mask (u16)").grid(row=1, column=0, padx=(8, 2), pady=(0, 6))
        ttk.Entry(raw, textvariable=self.io_mask_var, width=9).grid(row=1, column=1, padx=2, pady=(0, 6))
        ttk.Label(raw, text="config/sense").grid(row=1, column=2, padx=(8, 2), pady=(0, 6))
        ttk.Entry(raw, textvariable=self.io_mask_extra, width=7).grid(
            row=1, column=3, padx=2, pady=(0, 6)
        )
        for column, (label, method) in enumerate(
            (
                ("Dir", lambda io, m, _e: io.sx_set_dir(m)),
                ("Pull-up", lambda io, m, _e: io.sx_set_pullup(m)),
                ("Open-drain", lambda io, m, _e: io.sx_set_opendrain(m)),
                ("Debounce", lambda io, m, e: io.sx_set_debounce(m, e)),
                ("Int mask", lambda io, m, e: io.sx_set_int_mask(m, e)),
            ),
            start=4,
        ):
            ttk.Button(
                raw,
                text=label,
                command=lambda lbl=label, m=method: self._io_mutating_async(
                    f"Expander {lbl} mask",
                    lambda m=m: m(
                        self.io,
                        _as_int(self.io_mask_var, "mask"),
                        _as_int(self.io_mask_extra, "config/sense"),
                    ),
                ),
            ).grid(row=1, column=column, padx=2, pady=(0, 6))

        led = ttk.Frame(raw)
        led.grid(row=2, column=0, columnspan=8, padx=8, pady=(0, 6), sticky="w")
        self.io_led_pin = tk.IntVar(value=0)
        self.io_led_intensity = tk.IntVar(value=0)
        ttk.Label(led, text="LED driver: pin").pack(side="left")
        ttk.Spinbox(led, from_=0, to=15, textvariable=self.io_led_pin, width=4).pack(
            side="left", padx=(4, 6)
        )
        ttk.Label(led, text="intensity (0 = full on)").pack(side="left")
        ttk.Spinbox(led, from_=0, to=255, textvariable=self.io_led_intensity, width=5).pack(
            side="left", padx=(4, 6)
        )
        ttk.Button(
            led,
            text="Enable",
            command=lambda: self._io_mutating_async(
                "Expander LED driver on",
                lambda: self.io.sx_led_driver(
                    self.io_led_pin.get(), True, self.io_led_intensity.get()
                ),
            ),
        ).pack(side="left")
        ttk.Button(
            led,
            text="Disable",
            command=lambda: self._io_mutating_async(
                "Expander LED driver off",
                lambda: self.io.sx_led_driver(self.io_led_pin.get(), False, 0),
            ),
        ).pack(side="left", padx=6)
        ttk.Button(
            led,
            text="Soft Reset",
            command=lambda: self._io_mutating_async(
                "Expander soft reset", lambda: self.io.sx_reset(False)
            ),
        ).pack(side="left", padx=(20, 4))
        ttk.Button(
            led,
            text="Hard Reset (~RESET pin)",
            command=lambda: self._io_mutating_async(
                "Expander hard reset", lambda: self.io.sx_reset(True)
            ),
        ).pack(side="left")

        ttk.Label(raw, textvariable=self.io_reg_result, foreground=_MUTED_COLOR).grid(
            row=3, column=0, columnspan=8, padx=8, pady=(0, 6), sticky="w"
        )

    def _io_write_relay(self, relay: int) -> None:
        on = self._io_relay_vars[relay - 1].get()
        label = f"{devices.relay_label(relay)} {'ON' if on else 'off'}"

        def apply(result) -> None:
            if result.ok:
                self.set_status(f"{label}: ok.")
            else:
                self.set_status(f"{label}: refused ({result.refusal.value}).", error=True)
                # The requested state didn't take -- put the checkbox back to
                # what the relay shadow actually is rather than lying to the
                # operator about the board's state.
                self._io_relay_vars[relay - 1].set(not on)

        self.query_async(label, lambda: self.io.set_relay(relay, on), apply, error_types=(IoQueryError,))

    def _io_mutating_async(self, description: str, action: "Callable[[], OkReason]") -> None:
        """Run an IO write and surface a refusal (with reason, when the
        firmware gave one) instead of letting it land silently in
        IoClient's own consumer thread -- same shape as
        :meth:`_thermo_mutating_async`, wired to :class:`IoClient`'s
        ``_write_style``-based methods instead of the fire-and-forget
        ``send_async`` every one of these used to go through.
        """
        def apply(result: OkReason) -> None:
            if result.ok:
                self.set_status(f"{description}: ok.")
            else:
                detail = f" ({result.reason})" if result.reason else ""
                self.set_status(f"{description}: REJECTED{detail}.", error=True)

        self.query_async(description, action, apply, error_types=(IoQueryError,))

    def _io_write_level(self, io: int, level: bool) -> None:
        self._io_mutating_async(f"IO {io} = {int(level)}", lambda: self.io.set_io(io, level))

    def _io_write_dir(self, io: int) -> None:
        is_input = self._io_dir_vars[io - 1].get()
        pullup = self._io_pullup_vars[io - 1].get()
        self._io_mutating_async(
            f"IO {io} dir = {'input' if is_input else 'output'}",
            lambda: self.io.set_io_dir(io, is_input, pullup),
        )

    def _io_start_reporting(self) -> None:
        if not self._is_open("io"):
            return
        period = max(0, int(self.io_period_var.get()))
        self._io_mutating_async(
            f"Expander auto-report {period} ms",
            lambda: self.io.set_auto_report(period),
        )

    def _io_stop_reporting(self) -> None:
        if not self.link.is_connected or self.info.compatible is not True:
            return
        self._io_mutating_async(
            "Expander auto-report off", lambda: self.io.set_auto_report(0)
        )

    def io_read_async(self) -> None:
        self.query_async(
            "Expander read",
            lambda: self.io.read(),
            self._apply_io_state,
            error_types=(IoQueryError,),
        )

    def io_scan_async(self) -> None:
        def apply(found) -> None:
            text = (
                ", ".join(f"0x{a:02X}" for a in found)
                if found
                else "nothing answered (expected 0x3E)"
            )
            self.set_status(f"Expander scan: {text}")
            if self._is_open("io"):
                self.io_status_var.set(f"I2C scan: {text}")

        self.query_async(
            "Expander scan", lambda: self.io.scan(), apply, error_types=(IoQueryError,)
        )

    def io_read_reg_async(self) -> None:
        from .gui import _as_int  # local import: avoids a circular import with gui.py

        try:
            reg = _as_int(self.io_reg_addr, "register")
            length = int(self.io_reg_len.get())
        except ValueError as exc:
            self.io_reg_result.set(str(exc))
            return

        def apply(registers) -> None:
            if self._is_open("io"):
                self.io_reg_result.set(registers.describe())

        self.query_async(
            "Expander read reg",
            lambda: self.io.read_reg(reg, length),
            apply,
            error_types=(IoQueryError,),
        )

    def _drain_io_reports(self) -> None:
        """Render expander pushes on the Tk thread -- newest only, same
        reasoning as _drain_thermo_reports."""
        latest: Optional[IoState] = None
        try:
            while True:
                latest = self.io_reports.get_nowait()
        except queue.Empty:
            pass
        if latest is not None:
            self._apply_io_state(latest)

    def _apply_io_state(self, state: IoState) -> None:
        if not self._is_open("io"):
            return
        for relay in range(1, IO_RELAY_COUNT + 1):
            self._io_relay_vars[relay - 1].set(state.relay(relay))
        for io in range(1, IO_DIGITAL_COUNT + 1):
            self._io_level_vars[io - 1].set("1" if state.io_level(io) else "0")
            self._io_dir_vars[io - 1].set(state.io_is_input(io))
        for channel in range(THERMO_CHANNEL_COUNT):
            ready = state.drdy_asserted(channel)
            self._io_drdy_vars[channel].set("READY" if ready else "idle")
            self._io_drdy_labels[channel].config(
                foreground=_OK_COLOR if ready else _MUTED_COLOR
            )
        self.io_status_var.set(state.describe())

