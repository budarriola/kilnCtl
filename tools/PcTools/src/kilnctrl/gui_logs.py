"""In-app session log and device console popups, plus app shutdown.

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
LogsMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class LogsMixin:
    # ======================================================================
    # log popups
    # ======================================================================
    def _build_log_popup(self, top: tk.Toplevel) -> None:
        header = ttk.Frame(top)
        header.pack(fill="x", padx=8, pady=(8, 4))
        self.log_path_var = tk.StringVar(value=self._log_path_text())
        ttk.Label(header, textvariable=self.log_path_var).pack(side="left")
        ttk.Button(header, text="Keep Logs...", command=self.ask_keep_logs).pack(side="right")

        self.log_text = scrolledtext.ScrolledText(top, width=110, height=24, wrap="none")
        self.log_text.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.log_text.insert("end", "\n".join(self.log_history))
        if self.log_history:
            self.log_text.insert("end", "\n")
        self.log_text.configure(state="disabled")
        self.log_text.see("end")

    def _log_path_text(self) -> str:
        path = self.session_log.path
        return f"Log file: {path}" if path else "Log file: none yet (connect to start one)"

    def _build_device_log_popup(self, top: tk.Toplevel) -> None:
        """Live view of the firmware's ESP_LOGx output (task LOG), color-coded
        by level -- see uart_log_bridge.c for how these lines get here instead
        of the USB-Serial-JTAG console. This is also the only place a failed
        SPI/I2C transfer on the device ever becomes visible to the PC."""
        header = ttk.Frame(top)
        header.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Label(
            header,
            text="Firmware log output, forwarded over the reliable UART link "
            "(no debugger/second cable required).",
        ).pack(side="left")
        ttk.Button(header, text="Clear", command=self._clear_device_log).pack(side="right")

        self.device_log_text = scrolledtext.ScrolledText(top, width=110, height=24, wrap="none")
        self.device_log_text.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        for level, color in _DEVICE_LOG_COLORS.items():
            self.device_log_text.tag_configure(level.name, foreground=color)
        self._append_device_log_lines(self.device_log_history)
        self.device_log_text.configure(state="disabled")
        self.device_log_text.see("end")

    def _clear_device_log(self) -> None:
        self.device_log_history.clear()
        if self._is_open("device_log"):
            self.device_log_text.configure(state="normal")
            self.device_log_text.delete("1.0", "end")
            self.device_log_text.configure(state="disabled")

    def _on_device_log_line(self, line: LogLine) -> None:
        """LogClient's consumer thread: queue for the Tk-side Device Console
        view, and also mirror ERROR/WARN lines into the persistent session log
        file -- otherwise anything the firmware logged right before a reset
        (e.g. an SPI/I2C driver error) is lost the moment the deque wraps."""
        self.device_log_lines.put(line)
        method_name = _DEVICE_LOG_SESSION_METHOD_NAME.get(line.level)
        if method_name is not None:
            getattr(self.session_log, method_name)("device: %s", line.text)

    def _append_device_log_lines(self, lines) -> None:
        """Insert already-decoded lines into the device console text widget."""
        for line in lines:
            self.device_log_text.insert("end", f"{line.text}\n", line.level.name)

    def _drain_device_log_lines(self) -> None:
        new: list[LogLine] = []
        try:
            while True:
                new.append(self.device_log_lines.get_nowait())
        except queue.Empty:
            pass
        if not new:
            return
        self.device_log_history.extend(new)

        if not self._is_open("device_log"):
            return
        self.device_log_text.configure(state="normal")
        self._append_device_log_lines(new)
        self.device_log_text.configure(state="disabled")
        self.device_log_text.see("end")

    def _drain_log_lines(self) -> None:
        """Move formatted log lines into the history and the open log view."""
        new: list[str] = []
        try:
            while True:
                new.append(self.log_lines.get_nowait())
        except queue.Empty:
            pass
        if not new:
            return
        self.log_history.extend(new)

        if not self._is_open("log"):
            return
        self.log_text.configure(state="normal")
        self.log_text.insert("end", "\n".join(new) + "\n")
        self.log_text.configure(state="disabled")
        self.log_text.see("end")
        self.log_path_var.set(self._log_path_text())

    def ask_keep_logs(self) -> None:
        """Set (and persist) how many session log files to keep."""
        value = simpledialog.askinteger(
            "Keep Logs",
            "Number of session log files to keep:",
            parent=self.root,
            initialvalue=self.session_log.keep,
            minvalue=MIN_KEEP_LOGS,
            maxvalue=MAX_KEEP_LOGS,
        )
        if value is None:
            return
        keep = self.session_log.set_keep(value)  # persists + prunes immediately
        self.set_status(f"Keeping the {keep} most recent log file(s).")

    def open_log_folder(self) -> None:
        """Reveal the log directory in the OS file manager."""
        directory = self.session_log.log_dir
        try:
            directory.mkdir(parents=True, exist_ok=True)
            if sys.platform == "win32":
                subprocess.Popen(["explorer", str(directory)])
            elif sys.platform == "darwin":  # pragma: no cover - not this project's target
                subprocess.Popen(["open", str(directory)])
            else:  # pragma: no cover
                subprocess.Popen(["xdg-open", str(directory)])
        except OSError as exc:
            self.set_status(f"Could not open {directory}: {exc}", error=True)

    # -- shutdown ----------------------------------------------------------
    def on_close(self) -> None:
        try:
            self._safety_stop_poll()
            self._firing_stop_poll()
            for client in (
                self.info,
                self.system,
                self.device_log,
                self.thermo,
                self.io,
                self.display,
                self.safety,
                self.control,
                self.profiles_client,
                self.autotune_client,
                self.wifi_uart_client,
            ):
                client.close()
            # close(), not disconnect(): this link may be shared with another
            # process (see link_hub.py) -- closing the window shouldn't yank
            # the physical port out from under it. An explicit Disconnect
            # click (toggle_connect) is the only thing that should do that.
            self.link.close()
            self.session_log.close()
        finally:
            self.root.destroy()


