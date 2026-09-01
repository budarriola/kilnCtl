"""Full board pinout reference popup.

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
PinoutMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class PinoutMixin:
    # ======================================================================
    # Full board pinout (static reference) popup
    # ======================================================================
    def _build_pinout_reference_popup(self, top: tk.Toplevel) -> None:
        """All current *and future* interfaces/GPIO per docs/HARDWARE.md, one
        tab per ``##`` section, in document order. Pure static reference:
        parsed once (see pinout_reference.load_sections's cache) and never
        touches the link -- this must render something useful with nothing
        plugged in, unlike the "Pin Configuration" popup above it."""
        top.geometry("820x560")
        top.minsize(620, 380)

        sections = pinout_reference.load_sections()
        if not sections:
            # Missing/unparseable HARDWARE.md: say so plainly rather than
            # leaving a blank window -- same rule as the rest of this file's
            # query paths (see e.g. refresh_pin_config's own "not connected"
            # message, just for a doc read instead of a link failure).
            error = pinout_reference.get_parse_error() or "no sections parsed"
            ttk.Label(
                top,
                text=f"Could not load docs/HARDWARE.md:\n{error}",
                foreground=_BAD_COLOR,
                justify="left",
                wraplength=760,
            ).pack(fill="both", expand=True, padx=12, pady=12)
            return

        ttk.Label(
            top,
            text=(
                "Every interface the schematic defines, present or not -- traced from "
                "docs/HARDWARE.md, which is itself traced from hardware/mainBoard/kiln.kicad_sch "
                "plus the two daughterboard schematics. This is a static reference, not "
                "live data; see \"Pin Configuration...\" for what the connected device "
                "actually reports."
            ),
            justify="left",
            wraplength=780,
        ).pack(fill="x", padx=10, pady=(8, 4))

        notebook = ttk.Notebook(top)
        notebook.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        status_colors = {
            pinout_reference.STATUS_BUILT: None,  # default text color -- nothing to flag
            pinout_reference.STATUS_DESIGNED: _EXPECTED_COLOR,
            pinout_reference.STATUS_FUTURE: _MUTED_COLOR,
        }

        for section in sections:
            tab = ttk.Frame(notebook)
            # Short-ish tab labels: the parenthetical designator suffixes
            # (e.g. "(U5, 0x3E)") are useful in the doc but make for a very
            # wide notebook strip across ten tabs; strip them here only.
            tab_label = re.sub(r"\s*\([^)]*\)\s*$", "", section.title).strip() or section.title
            notebook.add(tab, text=tab_label)

            banner = ttk.Frame(tab)
            banner.pack(fill="x", padx=8, pady=(8, 4))
            color = status_colors.get(section.status)
            banner_label = ttk.Label(
                banner,
                text=pinout_reference.STATUS_LABELS.get(section.status, section.status),
                font=("Segoe UI", 9, "bold"),
            )
            if color:
                banner_label.configure(foreground=color)
            banner_label.pack(anchor="w")
            if section.status_note:
                ttk.Label(
                    banner, text=section.status_note, foreground=_MUTED_COLOR, wraplength=760,
                    justify="left",
                ).pack(anchor="w", pady=(2, 0))

            if section.rows:
                tree_frame = ttk.Frame(tab)
                tree_frame.pack(fill="both", expand=True, padx=8, pady=4)
                columns = [f"c{i}" for i in range(len(section.header))]
                tree = ttk.Treeview(
                    tree_frame, columns=columns, show="headings",
                    height=min(14, max(4, len(section.rows))),
                )
                for col, heading in zip(columns, section.header):
                    tree.heading(col, text=heading or " ")
                    tree.column(col, width=150, anchor="w", stretch=True)
                vscroll = ttk.Scrollbar(tree_frame, orient="vertical", command=tree.yview)
                tree.configure(yscrollcommand=vscroll.set)
                tree.pack(side="left", fill="both", expand=True)
                vscroll.pack(side="left", fill="y")
                for row in section.rows:
                    # A row's cell count can differ from the header's (e.g.
                    # the safety-thermocouple-board table's blank spacer
                    # column) -- pad/truncate rather than let Tk raise on a
                    # length mismatch.
                    values = list(row.cells)[: len(columns)]
                    values += [""] * (len(columns) - len(values))
                    tree.insert("", "end", values=values)

            if section.prose:
                prose_frame = ttk.Frame(tab)
                prose_frame.pack(fill="x", padx=8, pady=(0, 8))
                for paragraph in section.prose:
                    ttk.Label(
                        prose_frame, text=paragraph, justify="left", wraplength=760,
                    ).pack(anchor="w", pady=(0, 6))

