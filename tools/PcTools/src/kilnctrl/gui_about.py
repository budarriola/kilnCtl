"""About popup.

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
AboutMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class AboutMixin:
    # ======================================================================
    # About popup
    # ======================================================================
    def _build_about_popup(self, top: tk.Toplevel) -> None:
        """Pinout image + live highlights + a legend of what each pin does."""
        header = ttk.Frame(top)
        header.pack(fill="x", padx=8, pady=(8, 4))

        self.about_fw_var = tk.StringVar(value=self._fw_text())
        ttk.Label(header, textvariable=self.about_fw_var, font=("Segoe UI", 9, "bold")).pack(
            side="left"
        )
        ttk.Button(header, text="Refresh", command=self.refresh_pin_config).pack(side="right")

        self.about_status_var = tk.StringVar(value="")
        ttk.Label(top, textvariable=self.about_status_var, anchor="w").pack(
            fill="x", padx=8, pady=(0, 4)
        )

        body = ttk.Frame(top)
        body.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        # -- diagram --------------------------------------------------------
        canvas_frame = ttk.Frame(body)
        canvas_frame.pack(side="left", fill="both", expand=False)

        size = int(pin_overlay.IMAGE_SIZE * _PINOUT_SCALE)
        # Leave room for the header/legend on short screens; the canvas
        # scrolls rather than forcing an off-screen window.
        view_height = min(size, max(320, self.root.winfo_screenheight() - 260))

        self.about_canvas = tk.Canvas(
            canvas_frame,
            width=size,
            height=view_height,
            scrollregion=(0, 0, size, size),
            background="#ffffff",
            highlightthickness=0,
        )
        yscroll = ttk.Scrollbar(canvas_frame, orient="vertical", command=self.about_canvas.yview)
        self.about_canvas.configure(yscrollcommand=yscroll.set)
        self.about_canvas.pack(side="left", fill="both", expand=True)
        yscroll.pack(side="left", fill="y")

        # PhotoImage must be kept referenced or Tk garbage-collects the pixels.
        self._pinout_photo: Optional[tk.PhotoImage] = None
        try:
            self._pinout_photo = tk.PhotoImage(file=str(_PINOUT_IMAGE))
        except tk.TclError as exc:
            self.about_canvas.create_text(
                size / 2,
                40,
                text=f"pinout image unavailable:\n{exc}",
                fill=_BAD_COLOR,
                width=size - 40,
            )
        else:
            self.about_canvas.create_image(0, 0, anchor="nw", image=self._pinout_photo)

        # -- legend ---------------------------------------------------------
        legend = ttk.LabelFrame(body, text="Reported pin functions")
        legend.pack(side="left", fill="both", expand=True, padx=(8, 0))

        self.about_tree = ttk.Treeview(
            legend, columns=("abbrev", "gpio", "description"), show="headings", height=12
        )
        self.about_tree.heading("abbrev", text="Label")
        self.about_tree.heading("gpio", text="GPIO")
        self.about_tree.heading("description", text="Function")
        self.about_tree.column("abbrev", width=60, anchor="w", stretch=False)
        self.about_tree.column("gpio", width=50, anchor="center", stretch=False)
        self.about_tree.column("description", width=330, anchor="w")
        self.about_tree.pack(fill="both", expand=True, padx=6, pady=6)

        ttk.Label(
            legend,
            text=(
                "Highlights are drawn from the device's live reply to\n"
                "INFO_CMD_GET_PIN_CONFIG - not from a static table.\n\n"
                "Only real ESP32-S3 GPIOs appear here. The relay drives,\n"
                "the thermocouple DRDY inputs and the display's D/C and\n"
                "~RESET are SX1509 pins - see the I/O & Relays page."
            ),
            foreground=_MUTED_COLOR,
            justify="left",
        ).pack(anchor="w", padx=6, pady=(0, 6))

    def _fw_text(self) -> str:
        return self.fw_version.describe() if self.fw_version else "FW version: unknown"

    def _refresh_about_version(self) -> None:
        if self._is_open("about") and hasattr(self, "about_fw_var"):
            self.about_fw_var.set(self._fw_text())

    def refresh_pin_config(self) -> None:
        """(Re)query the device's pin configuration and redraw the About view."""
        if not self._is_open("about"):
            return
        if not self.link.is_connected:
            self._render_pin_config([], "Not connected - connect to query the device.")
            return

        self.about_status_var.set("Querying device...")

        def worker() -> None:
            try:
                entries = self.info.get_pin_config()
            except InfoQueryError as exc:
                self.session_log.warning("pin config query failed: %s", exc)
                self.post(lambda: self._render_pin_config([], str(exc)))
                return
            except Exception as exc:  # pragma: no cover - defensive
                self.session_log.error("pin config query error: %s", exc)
                self.post(lambda: self._render_pin_config([], f"error: {exc}"))
                return
            self.session_log.info("pin config: %d entries", len(entries))
            self.post(lambda: self._render_pin_config(entries, ""))

        threading.Thread(target=worker, name="uart-pin-config", daemon=True).start()

    def _render_pin_config(self, entries: "list[PinConfigEntry]", message: str) -> None:
        """Draw highlights + fill the legend. Tk thread only."""
        if not self._is_open("about"):
            return
        self.pin_config = entries or None

        self.about_canvas.delete("overlay")
        unplaced = pin_overlay.draw_overlay(self.about_canvas, entries, scale=_PINOUT_SCALE)

        self.about_tree.delete(*self.about_tree.get_children())
        for entry in entries:
            # A GPIO with no measured badge position can't be drawn on the
            # image, but it must still be visible somewhere.
            suffix = "  (not on diagram)" if entry in unplaced else ""
            self.about_tree.insert(
                "", "end", values=(entry.abbrev, entry.gpio, f"{entry.label}{suffix}")
            )

        if message:
            self.about_status_var.set(message)
        elif unplaced:
            self.about_status_var.set(
                f"{len(entries)} pins reported; "
                f"{len(unplaced)} not placeable on the diagram (listed below)."
            )
        else:
            self.about_status_var.set(f"{len(entries)} pins reported by the device.")

