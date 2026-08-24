"""Tkinter manual-control GUI for the KilnCtrl board's UART link.

Layout:
  * a port bar (combobox + refresh + connect/disconnect + status light);
  * a ``manualCtrl`` menu with one entry per device page (Thermocouples, I/O
    & Relays, Display, Safety), each opening a Toplevel;
  * an ``About`` menu with the live pinout diagram;
  * a ``Logs`` menu (in-app log view + retention setting);
  * a status bar showing the last command's result and the device FW version.

Threading: a send can block for up to ``max_retries * ack_timeout`` (~4 s with
the defaults), so every control action is dispatched to a worker thread. Worker
results come back through a :class:`queue.Queue` drained by a ``root.after``
poll on the Tk mainloop thread -- Tk widgets are only ever touched there.
Queries (INFO, THERMO, IO, DISPLAY, SAFETY) block for even longer, since they
wait for a reply frame *after* the ACK, and use the same pattern.

Live data is *pushed*, not polled: the thermocouple and expander pages turn on
the firmware's auto-report (see SET_AUTO_REPORT in uart_task_ids.h) and render
the unsolicited frames as they arrive. Nothing here sits in a polling loop
hammering the link.

Reminder (see devices.py): a result of "OK" means the command reached the ESP
task's inbox. The firmware bridge does not report device-level success back --
except on the query subcommands, which really do answer with data.
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

#: The pinout diagram shipped alongside the code (800x800 PNG of the
#: ESP32-S3-DevKitC-1, which is the module this board carries; tk.PhotoImage
#: reads PNG natively on Tk 8.6+, so no Pillow dependency for this one).
_PINOUT_IMAGE = Path(__file__).resolve().with_name("assets") / "pinout.png"

#: Scale applied to both the image and pin_overlay's measured coordinates.
#: 1.0 = native; the canvas viewport scrolls if the screen is shorter.
_PINOUT_SCALE = 1.0

#: How many recent log lines the in-app view keeps, so a log window opened
#: late still shows history.
_LOG_HISTORY_LINES = 2000

#: Same idea for the device console (firmware ESP_LOGx output over
#: UART_TASK_ID_LOG) -- kept separate from the session log's history since
#: it's a different stream (raw firmware text, not this app's own events).
_DEVICE_LOG_HISTORY_LINES = 2000

#: Default auto-report periods, used when a device page opens. Slow enough to
#: leave the link usable for commands, fast enough to feel live. The expander
#: also pushes on every ~INT edge regardless of this, so an input change shows
#: up immediately rather than up to half a second later.
_THERMO_REPORT_PERIOD_MS = 1000
_IO_REPORT_PERIOD_MS = 500

#: How often the safety page re-queries. This one *is* a poll, because SAFETY
#: has no auto-report subcommand -- but it queries the ESP's own cache, so it
#: costs one round trip and never waits on the isolated link.
_SAFETY_POLL_MS = 2000

#: How often the Firing Status popup re-polls /api/profile_exec and
#: /api/autotune over HTTP (same "device's own cache, one round trip"
#: reasoning as the safety poll above, just over Wi-Fi instead of UART).
_FIRING_POLL_MS = 3000

#: Text-widget tag -> color for each firmware log level, applied in
#: _build_device_log_popup / _append_device_log_lines.
_DEVICE_LOG_COLORS: dict[LogLevel, str] = {
    LogLevel.ERROR: "#a11",
    LogLevel.WARN: "#b8860b",
    LogLevel.INFO: "#000000",
    LogLevel.DEBUG: "#555555",
    LogLevel.VERBOSE: "#888888",
}

#: Firmware log level -> SessionLogger method, so device-side ESP_LOGx lines
#: (the only channel that ever carries a hint of *why* the board reset --
#: e.g. an I2C/SPI driver error logged right before a watchdog reboot) land
#: in the persistent session log file, not just the in-app Device Console
#: popup's capped in-memory deque.
_DEVICE_LOG_SESSION_METHOD_NAME: dict[LogLevel, str] = {
    LogLevel.ERROR: "error",
    LogLevel.WARN: "warning",
}

#: esp-idf's default softAP address -- where wifi_prov's fallback AP (and
#: this popup's default host) lives before/whenever the board isn't joined
#: to a home network. The PC must itself be joined to that AP for requests
#: here to route, same as a phone doing first-time setup would be.
_WIFI_AP_DEFAULT_HOST = "192.168.4.1"

#: mDNS hostname the firmware advertises unconditionally at boot (see
#: app_main's mdns_hostname_set("kiln") in KilnFW/App/main.c) -- resolves in
#: both AP-fallback and station mode, so it's shown to the user as a
#: network-independent alternative to the raw IP in the host field.
_WIFI_MDNS_HOST = "kilnctl.local"

#: Timeout for the Wi-Fi settings popup's HTTP calls to wifi_provision_http.c
#: -- generous enough for a scan (which blocks the ESP's handler on the
#: radio) without leaving a hung request spinning the UI indefinitely.
_WIFI_HTTP_TIMEOUT_S = 8.0

_OK_COLOR = "#0a7d28"
_BAD_COLOR = "#a11"
_MUTED_COLOR = "#555555"
#: Amber: "not true, but not wrong either" -- used for the safety page's
#: expected-down link, which must not read as a fault (see _build_safety_popup).
_EXPECTED_COLOR = "#b8860b"


class KilnCtrlApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        # Shared with any other pc_tools process (MCP server, another GUI)
        # already running -- see link_hub.py. Whichever process asks first
        # owns the physical port; this one becomes a client of it either way.
        self.link = get_shared_link()
        self.results: "queue.Queue[tuple[str, str, bool]]" = queue.Queue()
        #: Zero-arg callables posted by worker threads, run on the Tk thread by
        #: _drain_results. Same marshaling rule as `results`, but for work that
        #: touches more than the status bar.
        self.events: "queue.Queue[Callable[[], None]]" = queue.Queue()
        self.ports: list[PortInfo] = []
        self.popups: dict[str, tk.Toplevel] = {}

        # Session logging. Nothing is written until the first session starts
        # (a successful connect), but the in-app view is fed from the start.
        self.log_lines: "queue.Queue[str]" = queue.Queue()
        self.log_history: "deque[str]" = deque(maxlen=_LOG_HISTORY_LINES)
        self.session_log = SessionLogger()
        self.session_log.attach_view(self.log_lines)
        # Without this, kilnctrl's own module-level warnings (a serial read
        # failing, the reader thread exiting, the link hub connection dying)
        # go nowhere at all -- the GUI configures no logging -- and the
        # session log records a link going down with no reason.
        self.session_log.capture_package_logs()

        # Task INFO is registered here, at construction -- not lazily when the
        # About window opens -- so the firmware's once-per-boot version push
        # (info_boot_push_task) is never missed if the board reboots while the
        # GUI is already connected.
        self.info = InfoClient(self.link, on_boot_push=self._on_boot_push)
        #: Task SYSTEM (6) -- needed by the Danger Zone popup's watchdog-panic
        #: query/toggle (dev-only bench escape hatch, see watchdog_cfg.h).
        self.system = SystemClient(self.link)
        self.fw_version: Optional[FirmwareVersion] = None
        self.pin_config: Optional[list[PinConfigEntry]] = None
        self.wifi_status: Optional[WifiStatus] = None
        #: Show the incompatibility dialog at most once per connection --
        #: reset in toggle_connect() on each fresh connect.
        self._incompatibility_warned = False

        # Device console: task LOG is registered here for the same reason as
        # task INFO above -- so a line logged the instant the board boots
        # (before any Device Console window is ever opened) isn't missed.
        self.device_log_lines: "queue.Queue[LogLine]" = queue.Queue()
        self.device_log_history: "deque[LogLine]" = deque(maxlen=_DEVICE_LOG_HISTORY_LINES)
        self.device_log = LogClient(self.link, on_line=self._on_device_log_line)

        # Tasks THERMO (1), IO (2), DISPLAY (4) and SAFETY (7). Registered
        # here rather than when their windows open, for the same reason as the
        # three above: the two auto-report streams are unsolicited pushes, and
        # a push arriving on an unregistered task is NACKed and lost. The
        # report callbacks run on the clients' consumer threads, so they only
        # ever queue -- the Tk thread does the rendering.
        self.thermo_reports: "queue.Queue[list[ThermoReading]]" = queue.Queue()
        self.io_reports: "queue.Queue[IoState]" = queue.Queue()
        self.thermo = ThermoClient(self.link, on_report=self.thermo_reports.put)
        self.io = IoClient(self.link, on_report=self.io_reports.put)
        self.display = DisplayClient(self.link)
        self.safety = SafetyClient(self.link)

        # Tasks CONTROL (8), PROFILES (9), AUTOTUNE (10), WIFI (11) -- v4
        # additions so the GUI can drive zone tuning, profile CRUD/execution,
        # autotune and Wi-Fi provisioning without needing HTTP/Wi-Fi at all.
        # Registered here for the same "never miss an unregistered-task NACK"
        # reason as the tasks above, even though none of these four push
        # unsolicited data -- every reply here is to a request this client
        # itself sent.
        self.control = ControlClient(self.link)
        self.profiles_client = ProfilesClient(self.link)
        self.autotune_client = AutotuneClient(self.link)
        self.wifi_uart_client = WifiUartClient(self.link)

        #: Pending `after` id for the safety page's poll loop, so closing the
        #: window (or losing the link) actually stops it.
        self._safety_poll_id: Optional[str] = None

        #: Same idea as _safety_poll_id, for the Firing Status popup's
        #: HTTP poll loop.
        self._firing_poll_id: Optional[str] = None

        #: True once a session is underway, so the very next unexpected drop
        #: (link was up, goes down without the user clicking Disconnect) gets
        #: exactly one log line instead of silent UI-only state flips.
        self._was_connected = False

        root.title("KilnCtrl - manual control")
        root.geometry("640x210")
        root.minsize(580, 190)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self._build_menu()
        self._build_port_bar()
        self._build_status_bar()

        self.refresh_ports()
        self.root.after(100, self._drain_results)

    # -- widgets -----------------------------------------------------------
    def _build_menu(self) -> None:
        menubar = tk.Menu(self.root)
        manual = tk.Menu(menubar, tearoff=0)
        manual.add_command(label="Thermocouples (MAX31856)...", command=self.open_thermo_popup)
        manual.add_command(label="I/O && Relays (SX1509)...", command=self.open_io_popup)
        manual.add_command(label="Display (ILI9488)...", command=self.open_display_popup)
        manual.add_command(label="Safety Processor...", command=self.open_safety_popup)
        manual.add_separator()
        manual.add_command(label="Zones / PID (UART)...", command=self.open_zones_popup)
        manual.add_command(label="Fire Profiles (UART)...", command=self.open_profiles_popup)
        manual.add_command(label="Autotune (UART)...", command=self.open_autotune_popup)
        manual.add_command(label="Relay Rules (HTTP)...", command=self.open_rules_popup)
        manual.add_separator()
        manual.add_command(label="Wi-Fi Settings...", command=self.open_wifi_settings_popup)
        manual.add_command(label="Firing Status (PID/Autotune)...", command=self.open_firing_status_popup)
        manual.add_separator()
        manual.add_command(label="Danger Zone (Factory Reset)...", command=self.open_danger_zone_popup)
        menubar.add_cascade(label="manualCtrl", menu=manual)

        logs = tk.Menu(menubar, tearoff=0)
        logs.add_command(label="Show Log...", command=self.open_log_popup)
        logs.add_command(label="Device Console...", command=self.open_device_log_popup)
        logs.add_command(label="Keep Logs...", command=self.ask_keep_logs)
        logs.add_separator()
        logs.add_command(label="Open Log Folder", command=self.open_log_folder)
        menubar.add_cascade(label="Logs", menu=logs)

        about = tk.Menu(menubar, tearoff=0)
        about.add_command(label="Pin Configuration...", command=self.open_about_popup)
        about.add_command(
            label="Full Board Pinout (reference)...", command=self.open_pinout_reference_popup
        )
        about.add_command(label="About KilnCtrl...", command=self.show_about_box)
        menubar.add_cascade(label="About", menu=about)

        self.root.config(menu=menubar)

    def show_about_box(self) -> None:
        messagebox.showinfo(
            "About KilnCtrl",
            "KilnCtrl pc_tools\n\n"
            "PC-side control for the kilnCtl main board: an ESP32-S3-DevKitC "
            "driving three MAX31856 thermocouple channels, an SX1509 expander "
            "(4 relays + 7 digital I/O), an ILI9488 display, and an "
            "opto-isolated link to an RP2040 safety processor.\n\n"
            f"UART protocol version {devices.UART_PROTOCOL_VERSION}.",
            parent=self.root,
        )

    def _build_port_bar(self) -> None:
        frame = ttk.LabelFrame(self.root, text="Serial port")
        frame.pack(fill="x", padx=8, pady=(8, 4))

        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(
            frame, textvariable=self.port_var, state="readonly", width=52
        )
        self.port_combo.grid(row=0, column=0, columnspan=4, padx=6, pady=(6, 2), sticky="we")

        self.recommend_label = ttk.Label(frame, text="", foreground=_OK_COLOR)
        self.recommend_label.grid(row=1, column=0, padx=6, sticky="w")

        ttk.Button(frame, text="Refresh", command=self.refresh_ports).grid(
            row=1, column=1, padx=4, pady=(0, 6)
        )
        self.connect_button = ttk.Button(frame, text="Connect", command=self.toggle_connect)
        self.connect_button.grid(row=1, column=2, padx=(4, 6), pady=(0, 6))
        ttk.Button(frame, text="Restart UART", command=self.restart_uart).grid(
            row=1, column=3, padx=(0, 6), pady=(0, 6)
        )
        self.dashboard_button = ttk.Button(
            frame,
            text="Open Web Dashboard",
            command=self.open_web_dashboard,
            state="disabled",
        )
        self.dashboard_button.grid(row=1, column=4, padx=(0, 6), pady=(0, 6))

        frame.columnconfigure(0, weight=1)

    def restart_uart(self) -> None:
        """On-demand recovery lever for a stuck/desynced link -- see
        SYSTEM_CMD_RESTART_UART in uart_task_ids.h. Flushes the ESP's UART
        RX ring buffer; does not disconnect or reopen the local port."""
        self.send_async("Restart UART", UART_TASK_ID_SYSTEM, devices.system_restart_uart)

    def _build_status_bar(self) -> None:
        # The FW line sits above the status line so a long build timestamp
        # never squeezes out the last command's result.
        version_bar = ttk.Frame(self.root)
        version_bar.pack(fill="x", side="bottom")
        self.fw_var = tk.StringVar(value="FW version: unknown")
        self.fw_label = ttk.Label(version_bar, textvariable=self.fw_var, anchor="w")
        self.fw_label.pack(side="left", fill="x", expand=True, padx=8, pady=(0, 2))

        bar = ttk.Frame(self.root)
        bar.pack(fill="x", side="bottom")

        self.conn_var = tk.StringVar(value="Disconnected")
        self.conn_label = ttk.Label(bar, textvariable=self.conn_var, foreground=_BAD_COLOR)
        self.conn_label.pack(side="left", padx=8, pady=4)

        self.status_var = tk.StringVar(value="Ready.")
        ttk.Label(bar, textvariable=self.status_var, anchor="w").pack(
            side="left", fill="x", expand=True, padx=8
        )

    # -- ports -------------------------------------------------------------
    def refresh_ports(self) -> None:
        self.ports = list_ports()
        recommended = recommend_port()
        if not self.ports:
            self.port_combo["values"] = [_NO_PORTS]
            self.port_var.set(_NO_PORTS)
            self.recommend_label.config(text="No ports detected - plug in the UART USB-C port")
            return

        labels = [
            f"{p.label}{'  (recommended)' if p.device == recommended else ''}"
            for p in self.ports
        ]
        self.port_combo["values"] = labels
        available = {p.device for p in self.ports}
        current = self.selected_port()
        last_used = settings.get_last_port()
        # Prefer, in order: whatever's already selected (a refresh shouldn't
        # jump the selection around), then the last port we successfully
        # connected to, then the autodiscovered recommendation.
        target = next(
            (c for c in (current, last_used, recommended) if c in available), recommended
        )
        index = 0
        if target is not None:
            index = next((i for i, p in enumerate(self.ports) if p.device == target), 0)
        self.port_var.set(labels[index])
        self.recommend_label.config(
            text=f"Recommended: {recommended}" if recommended else "No recommended port"
        )

    def selected_port(self) -> Optional[str]:
        label = self.port_var.get()
        if not label or label == _NO_PORTS:
            return None
        for port in self.ports:
            if label.startswith(port.device):
                return port.device
        return None

    # -- connection --------------------------------------------------------
    def toggle_connect(self) -> None:
        if self.link.is_connected:
            port = self.link.port
            self.link.disconnect()
            self._set_connection_state()
            self.session_log.info("disconnected from %s", port)
            self.set_status("Disconnected.")
            return

        port = self.selected_port()
        try:
            opened = self.link.connect(port)
        except Exception as exc:
            # No session file exists yet at this point, so this only reaches
            # the in-app view -- which is the honest thing: a failed connect
            # never started a session.
            self.session_log.error("connect to %s failed: %s", port or "<auto>", exc)
            messagebox.showerror("Connect failed", str(exc), parent=self.root)
            self.set_status(f"Connect failed: {exc}", error=True)
            return

        # Trigger (a) for a new log file: a successful connect.
        self.session_log.start_session(f"connected to {opened} @ {self.link.baudrate} baud")
        self._incompatibility_warned = False
        self._set_connection_state()
        self.set_status(f"Connected to {opened} @ {self.link.baudrate} baud.")
        settings.set_last_port(opened)

        # The boot version push almost never lands (the board is usually
        # already up when the GUI connects), so pull it explicitly here. This
        # lives at the call site, not in UartLink.connect() -- the transport
        # layer has no business knowing INFO_CMD semantics.
        self.query_fw_version_async()
        self.query_wifi_status_async()

    def _set_connection_state(self) -> None:
        if self.link.is_connected:
            self.conn_var.set(f"Connected: {self.link.port}")
            self.conn_label.config(foreground=_OK_COLOR)
            self.connect_button.config(text="Disconnect")
            self.port_combo.config(state="disabled")
        else:
            self.conn_var.set("Disconnected")
            self.conn_label.config(foreground=_BAD_COLOR)
            self.connect_button.config(text="Connect")
            self.port_combo.config(state="readonly")
            self.wifi_status = None
            self.dashboard_button.config(state="disabled")

    # -- command dispatch --------------------------------------------------
    def send_async(self, description: str, dst_task: int, payload_builder: Callable[[], bytes]) -> None:
        """Build the payload here (fast, validates), send on a worker thread."""
        if not self.link.is_connected:
            self.set_status(f"{description}: not connected.", error=True)
            return
        if self.info.compatible is not True:
            # Blocks on both False (confirmed mismatch) and None (version not
            # known yet) -- never send a device command until we've actually
            # confirmed the firmware speaks our protocol version.
            reason = (
                "protocol version mismatch"
                if self.info.compatible is False
                else "firmware version not yet confirmed"
            )
            self.set_status(f"{description}: refused ({reason}).", error=True)
            self.session_log.error("refused send: %s (%s)", description, reason)
            return
        try:
            payload = payload_builder()
        except (ValueError, TypeError) as exc:
            # A field left empty or holding text (TypeError from int()/float())
            # is as ordinary a user error as an out-of-range number, and must
            # land in the status bar rather than as a traceback in the Tk
            # callback -- which Tk prints to stderr and the user never sees.
            self.set_status(f"{description}: {exc}", error=True)
            return

        self.set_status(f"{description}: sending...")
        self.session_log.info("send: %s (%d payload bytes)", description, len(payload))

        def worker() -> None:
            try:
                result = self.link.send(dst_task=dst_task, src_task=dst_task, payload=payload)
                self.session_log.info("send: %s -> %s", description, result.value)
                self.results.put((description, result.describe(), not result.ok))
            except Exception as exc:  # pragma: no cover - defensive
                self.session_log.error("send: %s -> error: %s", description, exc)
                self.results.put((description, f"error: {exc}", True))

        threading.Thread(target=worker, name="uart-send", daemon=True).start()

    def query_async(
        self,
        description: str,
        query: Callable[[], object],
        apply: Callable[[object], None],
        error_types: tuple = (Exception,),
    ) -> bool:
        """Run a device *query* on a worker thread and apply its result on the
        Tk thread.

        Same compatibility gate as :meth:`send_async`: unlike the INFO
        queries (which are exempt, being how compatibility is discovered),
        the device tasks' queries are device commands too. Returns False if
        the query was refused before it started.
        """
        if not self.link.is_connected:
            self.set_status(f"{description}: not connected.", error=True)
            return False
        if self.info.compatible is not True:
            reason = (
                "protocol version mismatch"
                if self.info.compatible is False
                else "firmware version not yet confirmed"
            )
            self.set_status(f"{description}: refused ({reason}).", error=True)
            return False

        def worker() -> None:
            try:
                value = query()
            except error_types as exc:
                self.session_log.warning("%s failed: %s", description, exc)
                self.results.put((description, str(exc), True))
                return
            except Exception as exc:  # pragma: no cover - defensive
                self.session_log.error("%s error: %s", description, exc)
                self.results.put((description, f"error: {exc}", True))
                return
            self.post(lambda: apply(value))

        threading.Thread(target=worker, name="uart-query", daemon=True).start()
        return True

    def _drain_results(self) -> None:
        try:
            while True:
                description, text, error = self.results.get_nowait()
                self.set_status(f"{description}: {text}", error=error)
        except queue.Empty:
            pass

        try:
            while True:
                self.events.get_nowait()()
        except queue.Empty:
            pass
        except Exception:  # pragma: no cover - never stop the poll loop
            self.session_log.logger.warning("error handling GUI event", exc_info=True)

        self._drain_log_lines()
        self._drain_device_log_lines()
        self._drain_thermo_reports()
        self._drain_io_reports()

        # The reader thread can die if the cable is yanked; keep the UI honest.
        connected = self.link.is_connected
        if not connected and self.connect_button["text"] == "Disconnect":
            self._set_connection_state()
        if self._was_connected and not connected:
            # Link was up last poll, is down now, and nobody clicked
            # Disconnect -- record it with a timestamp so a reboot/dropout
            # burst is visible in the log file after the fact, not just as a
            # gap in output.
            self.session_log.error("link dropped unexpectedly (was connected to %s)", self.link.port)
        self._was_connected = connected
        self.root.after(100, self._drain_results)

    def post(self, action: Callable[[], None]) -> None:
        """Run ``action`` on the Tk thread. Safe to call from a worker."""
        self.events.put(action)

    def set_status(self, text: str, error: bool = False) -> None:
        self.status_var.set(text)

    # -- firmware version --------------------------------------------------
    def query_fw_version_async(self) -> None:
        """Pull the FW version from the device on a worker thread."""
        if not self.link.is_connected:
            return

        def worker() -> None:
            try:
                version = self.info.get_fw_version()
            except InfoQueryError as exc:
                self.session_log.warning("FW version query failed: %s", exc)
                self.results.put(("FW version", str(exc), True))
                return
            except Exception as exc:  # pragma: no cover - defensive
                self.session_log.error("FW version query error: %s", exc)
                self.results.put(("FW version", f"error: {exc}", True))
                return
            self.post(lambda: self._apply_fw_version(version, booted=False))

        threading.Thread(target=worker, name="uart-fw-version", daemon=True).start()

    def _on_boot_push(self, version: FirmwareVersion) -> None:
        """Unsolicited version push = the device just booted (info.py).

        Called on InfoClient's consumer thread, so it only marshals.
        """
        self.post(lambda: self._apply_fw_version(version, booted=True))

    def _apply_fw_version(self, version: FirmwareVersion, booted: bool) -> None:
        if booted:
            # Trigger (b) for a new log file: the firmware pushes its version
            # exactly once per boot, so this frame *is* the reboot signal.
            self.session_log.info("device reboot detected (unsolicited FW version push)")
            self.session_log.start_session(f"device reboot - {version.describe()}")
        self.fw_version = version
        self.fw_var.set(version.describe())
        self.fw_label.config(foreground=_BAD_COLOR if not version.compatible else "")
        self.session_log.info("device firmware: %s", version.describe())
        self.set_status(
            f"Device rebooted: {version.describe()}"
            if booted
            else f"FW version: {version.describe()}"
        )
        self._refresh_about_version()

        # Pages opened before the version landed had their queries refused,
        # leaving them blank; and a reboot resets the firmware's auto-report
        # settings, so whatever is on screen is stale either way. Re-arm both.
        if version.compatible:
            if self._is_open("thermo"):
                self._thermo_start_reporting()
            if self._is_open("io"):
                self._io_start_reporting()
            if self._is_open("safety"):
                self.safety_refresh_async()

        if not version.compatible and not self._incompatibility_warned:
            # Only nag once per connection (re-armed in toggle_connect on a
            # fresh connect) -- a persistent mismatch would otherwise pop a
            # dialog on every reconnect/reboot push.
            self._incompatibility_warned = True
            self.session_log.error(
                "UART protocol version mismatch: device v%d, pc_tools v%d -- "
                "device commands refused until resolved",
                version.protocol_version,
                devices.UART_PROTOCOL_VERSION,
            )
            messagebox.showerror(
                "Protocol version mismatch",
                f"The connected device speaks UART protocol version "
                f"{version.protocol_version}, but this copy of pc_tools speaks "
                f"version {devices.UART_PROTOCOL_VERSION}.\n\n"
                "These are incompatible -- task IDs, subcommands, or payload "
                "layouts may not match. Version 1 is the unit-test fixture, "
                "where task 1 is a DAC rather than three thermocouples, so a "
                "command sent anyway would do something entirely different "
                "from what it says.\n\n"
                "All device commands are refused until this is resolved. "
                "Flash matching firmware, or update pc_tools to match.",
                parent=self.root,
            )

    # -- wifi dashboard ------------------------------------------------------
    def query_wifi_status_async(self) -> None:
        """Pull the device's Wi-Fi join state/IP so the dashboard button
        knows whether there's anywhere to send the browser."""
        if not self.link.is_connected:
            return

        def worker() -> None:
            try:
                status = self.info.get_wifi_status()
            except InfoQueryError as exc:
                self.session_log.warning("Wi-Fi status query failed: %s", exc)
                self.results.put(("Wi-Fi status", str(exc), True))
                return
            except Exception as exc:  # pragma: no cover - defensive
                self.session_log.error("Wi-Fi status query error: %s", exc)
                self.results.put(("Wi-Fi status", f"error: {exc}", True))
                return
            self.post(lambda: self._apply_wifi_status(status))

        threading.Thread(target=worker, name="uart-wifi-status", daemon=True).start()

    def _apply_wifi_status(self, status: WifiStatus) -> None:
        self.wifi_status = status
        self.dashboard_button.config(state="normal" if status.url else "disabled")
        self.session_log.info(
            "device wifi: %s", f"connected, {status.ip}" if status.connected else "not connected"
        )

    def open_web_dashboard(self) -> None:
        url = self.wifi_status.url if self.wifi_status else None
        if not url:
            self.set_status("Open Web Dashboard: device not on a network.", error=True)
            return
        webbrowser.open(url)
        self.set_status(f"Opened {url} in browser.")

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

    # ======================================================================
    # Fire Profiles panel (task 9, PROFILES) -- CRUD + execution control,
    # UART mirror of profiles_http.c / dashboard_http.c's /api/profile_exec*.
    # ======================================================================
    def open_profiles_popup(self) -> None:
        self._popup("profiles", "Fire Profiles (UART)", self._build_profiles_popup)
        self.profiles_refresh_async()
        self.profiles_refresh_exec_status_async()

    def _build_profiles_popup(self, top: tk.Toplevel) -> None:
        top.geometry("620x520")
        self.profiles_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(top, textvariable=self.profiles_status_var, anchor="w").pack(
            fill="x", padx=8, pady=(8, 4)
        )

        list_frame = ttk.LabelFrame(top, text="Saved profiles")
        list_frame.pack(fill="both", expand=True, padx=8, pady=4)
        self.profiles_listbox = tk.Listbox(list_frame, height=8, exportselection=False)
        self.profiles_listbox.pack(side="left", fill="both", expand=True, padx=(6, 0), pady=6)
        self.profiles_listbox.bind("<<ListboxSelect>>", self._on_profile_select)
        pscroll = ttk.Scrollbar(list_frame, orient="vertical", command=self.profiles_listbox.yview)
        self.profiles_listbox.configure(yscrollcommand=pscroll.set)
        pscroll.pack(side="left", fill="y", pady=6)
        btns = ttk.Frame(list_frame)
        btns.pack(side="left", fill="y", padx=6, pady=6)
        ttk.Button(btns, text="Refresh", command=self.profiles_refresh_async).pack(fill="x")
        ttk.Button(btns, text="Load", command=self.profile_load_async).pack(fill="x", pady=(4, 0))
        ttk.Button(btns, text="New", command=self._profile_new).pack(fill="x", pady=(4, 0))
        ttk.Button(btns, text="Delete", command=self.profile_delete_async).pack(fill="x", pady=(4, 0))
        self._profiles_summaries: list = []

        edit_frame = ttk.LabelFrame(top, text="Edit / Save")
        edit_frame.pack(fill="x", padx=8, pady=4)
        row = ttk.Frame(edit_frame)
        row.pack(fill="x", padx=6, pady=6)
        ttk.Label(row, text="Id (blank=new):").pack(side="left")
        self.profile_id_var = tk.StringVar()
        ttk.Entry(row, textvariable=self.profile_id_var, width=4).pack(side="left", padx=(2, 8))
        ttk.Label(row, text="Name:").pack(side="left")
        self.profile_name_var = tk.StringVar()
        ttk.Entry(row, textvariable=self.profile_name_var, width=16).pack(side="left", padx=(2, 8))
        ttk.Label(row, text="Zone mask:").pack(side="left")
        self.profile_zone_mask_var = tk.StringVar(value="1")
        ttk.Entry(row, textvariable=self.profile_zone_mask_var, width=4).pack(side="left", padx=(2, 8))

        ttk.Label(
            edit_frame, text="Segments (one per line): target_c, ramp_c_per_hr, dwell_min",
            foreground=_MUTED_COLOR,
        ).pack(anchor="w", padx=6)
        self.profile_segments_text = tk.Text(edit_frame, height=6)
        self.profile_segments_text.pack(fill="x", padx=6, pady=(0, 6))
        ttk.Button(edit_frame, text="Save Profile", command=self.profile_save_async).pack(
            anchor="w", padx=6, pady=(0, 6)
        )

        exec_frame = ttk.LabelFrame(top, text="Execution")
        exec_frame.pack(fill="x", padx=8, pady=(4, 8))
        self.profile_exec_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            exec_frame, textvariable=self.profile_exec_status_var, anchor="w", justify="left",
            wraplength=560,
        ).pack(fill="x", padx=6, pady=6)
        exec_btns = ttk.Frame(exec_frame)
        exec_btns.pack(fill="x", padx=6, pady=(0, 6))
        ttk.Button(exec_btns, text="Start selected", command=self.profile_start_async).pack(side="left")
        ttk.Button(exec_btns, text="Stop", command=self.profile_stop_async).pack(side="left", padx=6)
        ttk.Button(exec_btns, text="Pause", command=self.profile_pause_async).pack(side="left")
        ttk.Button(exec_btns, text="Resume", command=self.profile_resume_async).pack(side="left", padx=6)
        ttk.Button(exec_btns, text="Ack Last Run", command=self.profile_ack_last_run_async).pack(
            side="left"
        )
        ttk.Button(exec_btns, text="Refresh Status", command=self.profiles_refresh_exec_status_async).pack(
            side="left", padx=6
        )

    def _profile_new(self) -> None:
        self.profile_id_var.set("")
        self.profile_name_var.set("")
        self.profile_zone_mask_var.set("1")
        self.profile_segments_text.delete("1.0", "end")

    def profiles_refresh_async(self) -> None:
        if not self._is_open("profiles"):
            return
        self.profiles_status_var.set("Querying profile list...")
        self.query_async(
            "List profiles", lambda: self.profiles_client.list_all(), self._apply_profiles_list,
            error_types=(ProfilesQueryError,),
        )

    def _apply_profiles_list(self, summaries) -> None:
        if not self._is_open("profiles"):
            return
        self._profiles_summaries = summaries
        self.profiles_listbox.delete(0, "end")
        for s in summaries:
            tag = "  [built-in]" if s.builtin else ""
            self.profiles_listbox.insert(
                "end",
                f"[{s.id}] {s.name}  zones=0x{s.zone_mask:02X}  segments={s.segment_count}{tag}",
            )
        n_builtin = sum(1 for s in summaries if s.builtin)
        n_user = len(summaries) - n_builtin
        self.profiles_status_var.set(
            f"{n_user} user profile(s), {n_builtin} built-in schedule(s)."
        )

    def _on_profile_select(self, _event: object) -> None:
        pass  # selection is read explicitly by profile_load_async/start_async

    def _selected_profile_id(self) -> "Optional[int]":
        sel = self.profiles_listbox.curselection()
        if not sel or sel[0] >= len(self._profiles_summaries):
            return None
        return self._profiles_summaries[sel[0]].id

    def profile_load_async(self) -> None:
        pid = self._selected_profile_id()
        if pid is None:
            self.profiles_status_var.set("Load: select a profile first.")
            return

        def apply(detail) -> None:
            if not self._is_open("profiles"):
                return
            if detail is None:
                self.profiles_status_var.set(f"Profile {pid}: not found.")
                return
            self.profile_id_var.set(str(detail.id))
            self.profile_name_var.set(detail.name)
            self.profile_zone_mask_var.set(str(detail.zone_mask))
            self.profile_segments_text.delete("1.0", "end")
            for seg in detail.segments:
                self.profile_segments_text.insert(
                    "end", f"{seg.target_c}, {seg.ramp_c_per_hr}, {seg.dwell_min}\n"
                )
            if detail.builtin:
                # The id field now holds a built-in id; saving from here is a
                # save-as-copy into the first free user slot, not an overwrite.
                self.profiles_status_var.set(
                    f"Loaded built-in schedule {pid}: {detail.name!r} -- read-only; "
                    f"Save Profile stores a copy in the first free user slot."
                )
            else:
                self.profiles_status_var.set(f"Loaded profile {pid}: {detail.name!r}")

        self.query_async(
            f"Get profile {pid}", lambda: self.profiles_client.get(pid), apply,
            error_types=(ProfilesQueryError,),
        )

    def _parse_segments(self) -> "list[ProfileSegment]":
        segments = []
        for line in self.profile_segments_text.get("1.0", "end").splitlines():
            line = line.strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) != 3:
                raise ValueError(f"segment line {line!r} must be 'target_c, ramp_c_per_hr, dwell_min'")
            target_c, ramp, dwell = float(parts[0]), float(parts[1]), int(float(parts[2]))
            segments.append(ProfileSegment(target_c=target_c, ramp_c_per_hr=ramp, dwell_min=dwell))
        if not segments:
            raise ValueError("at least one segment is required")
        return segments

    def profile_save_async(self) -> None:
        name = self.profile_name_var.get().strip()
        if not name:
            self.profiles_status_var.set("Save: name is required.")
            return
        id_text = self.profile_id_var.get().strip()
        profile_id = PROFILES_SAVE_ID_NEW if not id_text else int(id_text)
        try:
            zone_mask = int(self.profile_zone_mask_var.get())
            segments = self._parse_segments()
        except ValueError as exc:
            self.profiles_status_var.set(f"Save: {exc}")
            return

        def apply(result) -> None:
            if not self._is_open("profiles"):
                return
            if result.ok:
                self.profile_id_var.set(str(result.id))
                self.profiles_status_var.set(
                    f"Saved as id {result.id}"
                    + (f" ({result.warning_count} warning(s))" if result.warning_count else "")
                )
                self.profiles_refresh_async()
            else:
                self.profiles_status_var.set(f"Save REJECTED: {result.error}")

        self.query_async(
            "Save profile",
            lambda: self.profiles_client.save(profile_id, name, zone_mask, segments),
            apply,
            error_types=(ProfilesQueryError,),
        )

    def profile_delete_async(self) -> None:
        pid = self._selected_profile_id()
        if pid is None:
            self.profiles_status_var.set("Delete: select a profile first.")
            return
        if profile_id_is_builtin(pid):
            # Same refusal the firmware and the web UI give: the catalogue is
            # const data in flash. Hiding is the reversible equivalent.
            self.profiles_status_var.set(
                f"Delete {pid}: REFUSED -- built-in schedules are read-only; "
                f"hide it from the web UI instead."
            )
            return
        if not messagebox.askyesno("Delete profile", f"Delete profile {pid}?", parent=self.root):
            return

        def apply(result) -> None:
            if self._is_open("profiles"):
                detail = f" ({result.reason})" if not result.ok and result.reason else ""
                self.profiles_status_var.set(
                    f"Delete {pid}: {'ok' if result.ok else 'REJECTED'}{detail}."
                )
                if result.ok:
                    self.profiles_refresh_async()

        self.query_async(
            f"Delete profile {pid}", lambda: self.profiles_client.delete(pid), apply,
            error_types=(ProfilesQueryError,),
        )

    def profiles_refresh_exec_status_async(self) -> None:
        if not self._is_open("profiles"):
            return
        self.query_async(
            "Exec status", lambda: self.profiles_client.get_exec_status(),
            self._apply_profile_exec_status, error_types=(ProfilesQueryError,),
        )

    def _apply_profile_exec_status(self, status) -> None:
        if not self._is_open("profiles"):
            return
        lines = [
            f"State: {status.state_name}   Profile: {status.name!r} (id {status.profile_id})   "
            f"Zones: 0x{status.zone_mask:02X}",
            f"Segment {status.segment_index + 1}/{status.segment_count}"
            f"{'  (dwelling)' if status.dwelling else '  (ramping)'}   Target: {status.target_c:.1f}C",
        ]
        if status.ramp_lock_held:
            lines.append(f"Ramp-lock held -- waiting on zone mask 0x{status.ramp_lock_lagging_mask:02X}")
        for z in status.zones:
            actual = f"{z.actual_c:.1f}C" if z.actual_valid else "no reading"
            state = "FAULT" if z.faulted else ("ON" if z.relay_commanded_on else "off")
            lines.append(f"  Zone {z.zone}: {actual}  mode={z.control_mode}  {state}  duty={z.duty:.2f}")
        self.profile_exec_status_var.set("\n".join(lines))

    def profile_start_async(self) -> None:
        pid = self._selected_profile_id()
        if pid is None:
            self.profiles_status_var.set("Start: select a profile first.")
            return

        def apply(result) -> None:
            if not self._is_open("profiles"):
                return
            self.profiles_status_var.set(
                f"Start {pid}: ok" if result.ok else f"Start {pid} REJECTED: {result.error}"
            )
            self.profiles_refresh_exec_status_async()

        self.query_async(
            f"Start profile {pid}", lambda: self.profiles_client.start(pid), apply,
            error_types=(ProfilesQueryError,),
        )

    def _profile_exec_action(self, description: str, action) -> None:
        def apply(result) -> None:
            if self._is_open("profiles"):
                detail = f" ({result.reason})" if not result.ok and result.reason else ""
                self.profiles_status_var.set(
                    f"{description}: {'ok' if result.ok else 'REJECTED'}{detail}."
                )
                self.profiles_refresh_exec_status_async()

        self.query_async(description, action, apply, error_types=(ProfilesQueryError,))

    def profile_stop_async(self) -> None:
        self._profile_exec_action("Stop", lambda: self.profiles_client.stop())

    def profile_pause_async(self) -> None:
        self._profile_exec_action("Pause", lambda: self.profiles_client.pause())

    def profile_resume_async(self) -> None:
        self._profile_exec_action("Resume", lambda: self.profiles_client.resume())

    def profile_ack_last_run_async(self) -> None:
        self._profile_exec_action("Ack Last Run", lambda: self.profiles_client.ack_last_run())

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
            ("kiln", "Kiln config only (zones, PID, relay rules)"),
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

        self.danger_status_var.set(f"Sending factory reset ({scope_name})...")

        self.send_async(
            f"Factory reset ({scope_name})",
            UART_TASK_ID_SYSTEM,
            lambda: devices.system_factory_reset(scope),
        )
        self.session_log.info("factory reset requested: scope=%s", scope_name)
        self.danger_status_var.set(
            f"Factory reset ({scope_name}) sent. Device will erase and reboot "
            "~500ms after the ACK -- watch the Device Console / FW version line."
        )

    # ======================================================================
    # Relay Rules panel (HTTP only -- GET/POST /api/rules; free-form DSL text
    # with no fixed-size encoding, no UART mirror -- see docs/UART_PROTOCOL.md)
    # ======================================================================
    def open_rules_popup(self) -> None:
        self._popup("rules", "Relay Rules (HTTP)", self._build_rules_popup)
        self.rules_refresh_async()

    def _build_rules_popup(self, top: tk.Toplevel) -> None:
        top.geometry("560x480")
        host_row = ttk.Frame(top)
        host_row.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Label(host_row, text="Device host/IP:").pack(side="left")
        self.rules_host_var = tk.StringVar(value=self._wifi_default_host())
        ttk.Entry(host_row, textvariable=self.rules_host_var, width=16).pack(
            side="left", padx=(6, 6)
        )
        ttk.Button(host_row, text="Refresh", command=self.rules_refresh_async).pack(side="left")

        self.rules_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(top, textvariable=self.rules_status_var, anchor="w").pack(
            fill="x", padx=8, pady=(0, 4)
        )

        self.rules_text = scrolledtext.ScrolledText(top, wrap="none")
        self.rules_text.pack(fill="both", expand=True, padx=8, pady=4)

        ttk.Button(top, text="Save (POST /api/rules)", command=self.rules_save_async).pack(
            anchor="w", padx=8, pady=(0, 8)
        )

    def _rules_host(self) -> str:
        var = getattr(self, "rules_host_var", None)
        if var is not None:
            host = var.get().strip()
            if host:
                return host
        return self._wifi_default_host()

    def rules_refresh_async(self) -> None:
        if not self._is_open("rules"):
            return
        host = self._rules_host()
        self.rules_status_var.set(f"Querying {host}...")

        def worker() -> None:
            url = f"http://{host}/api/rules"
            try:
                with urllib.request.urlopen(url, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
                    text = resp.read().decode("utf-8", errors="replace")
            except Exception as exc:  # pragma: no cover - network/device dependent
                err = self._wifi_http_error_text(exc)
                self.post(lambda: self.rules_status_var.set(f"Query failed: {err}"))
                return
            self.post(lambda: self._apply_rules_text(text))

        threading.Thread(target=worker, name="rules-http-get", daemon=True).start()

    def _apply_rules_text(self, text: str) -> None:
        if not self._is_open("rules"):
            return
        self.rules_text.delete("1.0", "end")
        self.rules_text.insert("1.0", text)
        self.rules_status_var.set("Loaded.")

    def rules_save_async(self) -> None:
        if not self._is_open("rules"):
            return
        host = self._rules_host()
        body = self.rules_text.get("1.0", "end")
        self.rules_status_var.set(f"Saving to {host}...")

        def worker() -> None:
            url = f"http://{host}/api/rules"
            req = urllib.request.Request(
                url, data=body.encode("utf-8"), method="POST",
                headers={"Content-Type": "text/plain; charset=utf-8"},
            )
            try:
                with urllib.request.urlopen(req, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
                    resp.read()
            except Exception as exc:  # pragma: no cover - network/device dependent
                err = self._wifi_http_error_text(exc)
                self.post(lambda: self.rules_status_var.set(f"Save failed: {err}"))
                return
            self.post(lambda: self.rules_status_var.set("Saved."))

        threading.Thread(target=worker, name="rules-http-post", daemon=True).start()

    # -- Wi-Fi settings popup ------------------------------------------------
    # Talks straight HTTP to wifi_provision_http.c's routes (/status, /scan,
    # /provision) -- the same API the captive-portal page a phone sees uses.
    # Deliberately not a UART command: wifi_prov's join/scan/local-only
    # controls were never exposed over the UART protocol (only read-only
    # GET_WIFI_STATUS was, for the dashboard button above), and there's no
    # need to invent a new task_id/subcommand set when the board already
    # serves this over HTTP. The PC must be able to route to the target host
    # -- joined to the board's fallback AP for first-time setup, or already
    # on the same home network once it's provisioned.
    def open_wifi_settings_popup(self) -> None:
        self._popup("wifi_settings", "Wi-Fi Settings", self._build_wifi_settings_popup)
        # Default to UART whenever we have no evidence of a live Wi-Fi link --
        # the HTTP path has nothing to talk to before the board has ever
        # joined a network (or if it's dropped off one), so start on the
        # link that always works instead of making the user discover the
        # checkbox after a failed query.
        wifi_reachable = bool(self.wifi_status and self.wifi_status.connected)
        self.wifi_use_uart_var.set(not wifi_reachable)
        self.wifi_settings_refresh_status_async()

    # -- Firing Status popup -------------------------------------------------
    # Same "straight HTTP to the board" approach as the Wi-Fi popup above,
    # against dashboard_http.c's /api/profile_exec and /api/autotune (TODO.md
    # section 6/6A) -- read-only here, deliberately: starting/stopping a
    # firing or an autotune run stays a web-dashboard action for now, this is
    # a status view for whoever's driving the board from this GUI's Wi-Fi/
    # UART side and wants the PID/thermal-guard/autotune picture without
    # switching to a browser. Polls on a timer (unlike the Wi-Fi popup, which
    # only refreshes on demand) because that's the point of a status page --
    # see _FIRING_POLL_MS's comment for why this is safe to do at 3s.
    def open_firing_status_popup(self) -> None:
        self._popup(
            "firing_status", "Firing Status (PID/Autotune)", self._build_firing_status_popup,
            on_close=self._firing_stop_poll,
        )
        self.firing_status_refresh_async()
        self._firing_schedule_poll()

    def _wifi_settings_is_open(self) -> bool:
        return self._is_open("wifi_settings")

    def _wifi_default_host(self) -> str:
        if self.wifi_status and self.wifi_status.connected and self.wifi_status.ip:
            return self.wifi_status.ip
        return _WIFI_AP_DEFAULT_HOST

    def _build_wifi_settings_popup(self, top: tk.Toplevel) -> None:
        host_row = ttk.Frame(top)
        host_row.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Label(host_row, text="Device host/IP:").pack(side="left")
        self.wifi_host_var = tk.StringVar(value=self._wifi_default_host())
        ttk.Entry(host_row, textvariable=self.wifi_host_var, width=16).pack(
            side="left", padx=(6, 6)
        )
        ttk.Button(
            host_row, text="Refresh Status", command=self.wifi_settings_refresh_status_async
        ).pack(side="left")

        # Also reachable at kilnctl.local -- the firmware advertises this over
        # mDNS unconditionally at boot (both AP-fallback and station mode),
        # so it works even when the IP above is stale or unknown. Shown as a
        # copy-pasteable hint, not wired as the host field's default: mDNS
        # resolution isn't guaranteed on every OS (needs Bonjour/avahi).
        mdns_row = ttk.Frame(top)
        mdns_row.pack(fill="x", padx=8, pady=(0, 4))
        ttk.Label(
            mdns_row, text=f"Also reachable at: {_WIFI_MDNS_HOST}", foreground="#555555",
        ).pack(side="left")

        # UART toggle: the whole point of task WIFI (11) is provisioning
        # without needing an existing HTTP/network path in the first place --
        # useful before the board has ever joined a network, when the host/IP
        # field above has nothing valid to point at. When checked, every
        # action in this popup goes over the UART link (self.wifi_uart_client)
        # instead of urllib HTTP against the host field; the host field itself
        # is then unused.
        self.wifi_use_uart_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            host_row, text="Use UART (no network needed)", variable=self.wifi_use_uart_var,
        ).pack(side="left", padx=(12, 0))

        self.wifi_settings_status_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            top, textvariable=self.wifi_settings_status_var, anchor="w", justify="left"
        ).pack(fill="x", padx=8, pady=(0, 8))

        scan_frame = ttk.LabelFrame(top, text="Nearby networks")
        scan_frame.pack(fill="both", expand=True, padx=8, pady=4)
        self.wifi_scan_list = tk.Listbox(scan_frame, height=8, exportselection=False)
        self.wifi_scan_list.pack(side="left", fill="both", expand=True, padx=(6, 0), pady=6)
        self.wifi_scan_list.bind("<<ListboxSelect>>", self._on_wifi_scan_select)
        scan_scroll = ttk.Scrollbar(
            scan_frame, orient="vertical", command=self.wifi_scan_list.yview
        )
        self.wifi_scan_list.configure(yscrollcommand=scan_scroll.set)
        scan_scroll.pack(side="left", fill="y", pady=6)
        ttk.Button(scan_frame, text="Scan", command=self.wifi_scan_async).pack(
            side="left", padx=6, pady=6, anchor="n"
        )
        # (ssid, secure) for each row currently listed, in display order --
        # lets a click fill the SSID field without re-parsing label text.
        self._wifi_scan_entries: list[tuple[str, bool]] = []

        join_frame = ttk.LabelFrame(top, text="Join a network")
        join_frame.pack(fill="x", padx=8, pady=4)
        self.wifi_ssid_var = tk.StringVar()
        self.wifi_password_var = tk.StringVar()
        ttk.Label(join_frame, text="SSID:").grid(row=0, column=0, padx=6, pady=6, sticky="e")
        ttk.Entry(join_frame, textvariable=self.wifi_ssid_var, width=28).grid(
            row=0, column=1, padx=(0, 6), pady=6, sticky="we"
        )
        ttk.Label(join_frame, text="Password:").grid(
            row=1, column=0, padx=6, pady=(0, 6), sticky="e"
        )
        ttk.Entry(join_frame, textvariable=self.wifi_password_var, width=28, show="*").grid(
            row=1, column=1, padx=(0, 6), pady=(0, 6), sticky="we"
        )
        ttk.Button(join_frame, text="Connect", command=self.wifi_connect_async).grid(
            row=0, column=2, rowspan=2, padx=6, pady=6
        )
        join_frame.columnconfigure(1, weight=1)

        mode_frame = ttk.LabelFrame(top, text="Wi-Fi mode")
        mode_frame.pack(fill="x", padx=8, pady=(4, 8))
        self.wifi_mode_var = tk.StringVar(value="home")
        ttk.Radiobutton(
            mode_frame,
            text="Home Wi-Fi (join the network above)",
            variable=self.wifi_mode_var,
            value="home",
        ).pack(anchor="w", padx=6, pady=(6, 0))
        ttk.Radiobutton(
            mode_frame,
            text="Access Point (AP always on, never joins a network)",
            variable=self.wifi_mode_var,
            value="ap",
        ).pack(anchor="w", padx=6, pady=(0, 4))
        ttk.Button(mode_frame, text="Apply", command=self.wifi_set_mode_async).pack(
            anchor="w", padx=6, pady=(0, 6)
        )

        ap_frame = ttk.LabelFrame(top, text="Access point identity")
        ap_frame.pack(fill="x", padx=8, pady=(4, 8))
        ttk.Label(
            ap_frame,
            text="This board's own network -- used for first-time setup and while in "
            "Access Point mode. Not the home network it joins.",
            foreground=_MUTED_COLOR,
            wraplength=420,
            justify="left",
        ).grid(row=0, column=0, columnspan=3, padx=6, pady=(6, 4), sticky="w")
        ttk.Label(ap_frame, text="SSID:").grid(row=1, column=0, padx=6, pady=(0, 6), sticky="e")
        self.wifi_ap_ssid_var = tk.StringVar()
        ttk.Entry(ap_frame, textvariable=self.wifi_ap_ssid_var, width=24).grid(
            row=1, column=1, padx=(0, 6), pady=(0, 6), sticky="w"
        )
        ttk.Label(ap_frame, text="Password:").grid(row=2, column=0, padx=6, pady=(0, 6), sticky="e")
        self.wifi_ap_password_var = tk.StringVar()
        ttk.Entry(ap_frame, textvariable=self.wifi_ap_password_var, width=24, show="*").grid(
            row=2, column=1, padx=(0, 6), pady=(0, 6), sticky="w"
        )
        ttk.Label(ap_frame, text="(empty = open network)", foreground=_MUTED_COLOR).grid(
            row=2, column=2, padx=(0, 6), pady=(0, 6), sticky="w"
        )
        ttk.Button(
            ap_frame, text="Save AP Identity", command=self.wifi_set_ap_identity_async
        ).grid(row=3, column=0, columnspan=3, padx=6, pady=(0, 6), sticky="w")
        ap_frame.columnconfigure(1, weight=1)

    def _on_wifi_scan_select(self, _event: object) -> None:
        selection = self.wifi_scan_list.curselection()
        if not selection:
            return
        ssid, _secure = self._wifi_scan_entries[selection[0]]
        self.wifi_ssid_var.set(ssid)

    def _wifi_host(self) -> str:
        return self.wifi_host_var.get().strip() or _WIFI_AP_DEFAULT_HOST

    def _wifi_http_get(self, path: str) -> object:
        url = f"http://{self._wifi_host()}{path}"
        with urllib.request.urlopen(url, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def _wifi_http_post_form(self, path: str, fields: dict[str, str]) -> None:
        url = f"http://{self._wifi_host()}{path}"
        body = urllib.parse.urlencode(fields).encode("ascii")
        req = urllib.request.Request(
            url, data=body, headers={"Content-Type": "application/x-www-form-urlencoded"}
        )
        with urllib.request.urlopen(req, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
            resp.read()

    @staticmethod
    def _wifi_http_error_text(exc: Exception) -> str:
        if isinstance(exc, urllib.error.HTTPError):
            detail = exc.read().decode("utf-8", errors="replace").strip()
            return f"HTTP {exc.code}{': ' + detail if detail else ''}"
        if isinstance(exc, urllib.error.URLError):
            return f"unreachable: {exc.reason}"
        return str(exc)

    def _wifi_use_uart(self) -> bool:
        return bool(getattr(self, "wifi_use_uart_var", None) and self.wifi_use_uart_var.get())

    def wifi_settings_refresh_status_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        if self._wifi_use_uart():
            self.wifi_settings_status_var.set("Querying over UART...")

            def apply(status) -> None:
                if not self._wifi_settings_is_open():
                    return
                self.wifi_mode_var.set(status.mode_name)
                if status.ap_ssid and not self.wifi_ap_ssid_var.get():
                    self.wifi_ap_ssid_var.set(status.ap_ssid)
                text = (
                    f"Mode: {'Access Point' if status.mode_name == 'ap' else 'Home Wi-Fi'}  |  "
                    f"Saved SSID: {status.ssid}  |  "
                    f"Station: {'connected (' + status.sta_ip + ')' if status.sta_connected else 'not connected'}"
                )
                self.wifi_settings_status_var.set(text)
                self.session_log.info("wifi (UART) settings status: %s", text)

            self.query_async(
                "Wi-Fi status (UART)", lambda: self.wifi_uart_client.get_status(), apply,
                error_types=(WifiUartQueryError,),
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(f"Querying {host}...")

        def worker() -> None:
            try:
                data = self._wifi_http_get("/status")
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning(
                    "Wi-Fi settings HTTP status query failed (%s), falling back to UART", text
                )
                # HTTP is unreachable -- fall back to UART automatically
                # rather than leaving the user staring at "Query failed".
                # Flip the checkbox too so every later action in this popup
                # (scan/connect/mode/AP identity) follows the same path
                # instead of retrying the dead HTTP link.
                self.post(lambda: self.wifi_use_uart_var.set(True))
                self.post(self.wifi_settings_refresh_status_async)
                return
            self.post(lambda: self._apply_wifi_settings_status(data))

        threading.Thread(target=worker, name="wifi-http-status", daemon=True).start()

    def _apply_wifi_settings_status(self, data: dict) -> None:
        if not self._wifi_settings_is_open():
            return
        state = data.get("state", "unknown")
        mode = data.get("mode", "home")
        ssid = data.get("ssid", "")
        sta_connected = bool(data.get("sta_connected", False))
        sta_ip = data.get("sta_ip", "")
        ap_ssid = data.get("ap_ssid", "")
        self.wifi_mode_var.set(mode if mode in ("home", "ap") else "home")
        if ap_ssid and not self.wifi_ap_ssid_var.get():
            # Only prefill if the user hasn't already started typing a
            # replacement -- a refresh mid-edit shouldn't clobber it.
            self.wifi_ap_ssid_var.set(ap_ssid)
        text = f"State: {state}"
        if ssid:
            text += f"  |  Saved SSID: {ssid}"
        text += f"  |  Mode: {'Access Point' if mode == 'ap' else 'Home Wi-Fi'}"
        text += f"  |  Station: {'connected (' + sta_ip + ')' if sta_connected else 'not connected'}"
        self.wifi_settings_status_var.set(text)
        self.session_log.info("wifi settings status: %s", text)

    def wifi_scan_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        if self._wifi_use_uart():
            self.wifi_settings_status_var.set("Scanning over UART...")

            def apply(result) -> None:
                if not self._wifi_settings_is_open():
                    return
                entries, truncated = result
                self.wifi_scan_list.delete(0, "end")
                self._wifi_scan_entries = []
                for e in entries:
                    lock = "" if e.secure else " (open)"
                    self.wifi_scan_list.insert("end", f"{e.ssid}  [{e.rssi} dBm]{lock}")
                    self._wifi_scan_entries.append((e.ssid, e.secure))
                suffix = " (truncated)" if truncated else ""
                self.wifi_settings_status_var.set(f"{len(entries)} network(s) found{suffix}.")

            self.query_async(
                "Wi-Fi scan (UART)", lambda: self.wifi_uart_client.scan(), apply,
                error_types=(WifiUartQueryError,),
            )
            return
        self.wifi_settings_status_var.set(f"Scanning from {self._wifi_host()}...")

        def worker() -> None:
            try:
                data = self._wifi_http_get("/scan")
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi scan failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"Scan failed: {text}"))
                return
            self.post(lambda: self._apply_wifi_scan(data))

        threading.Thread(target=worker, name="wifi-http-scan", daemon=True).start()

    def _apply_wifi_scan(self, results: list) -> None:
        if not self._wifi_settings_is_open():
            return
        self.wifi_scan_list.delete(0, "end")
        self._wifi_scan_entries = []
        for entry in results:
            ssid = str(entry.get("ssid", ""))
            rssi = entry.get("rssi", 0)
            secure = bool(entry.get("secure", True))
            lock = "" if secure else " (open)"
            self.wifi_scan_list.insert("end", f"{ssid}  [{rssi} dBm]{lock}")
            self._wifi_scan_entries.append((ssid, secure))
        self.wifi_settings_status_var.set(f"{len(results)} network(s) found.")
        self.session_log.info("wifi scan: %d network(s)", len(results))

    def wifi_connect_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        ssid = self.wifi_ssid_var.get().strip()
        if not ssid:
            self.wifi_settings_status_var.set("Connect: SSID is required.")
            return
        password = self.wifi_password_var.get()
        wifi_credentials.save(ssid, password)
        if self._wifi_use_uart():
            self.wifi_settings_status_var.set(f"Sending credentials for {ssid!r} over UART...")
            self.session_log.info("wifi (UART) settings: add network %r", ssid)

            def apply(result: OkReason) -> None:
                if not self._wifi_settings_is_open():
                    return
                if result.ok:
                    self.wifi_settings_status_var.set(
                        f"Credentials sent for {ssid!r}. Joining in the background -- Refresh "
                        "Status to check progress."
                    )
                    self.wifi_settings_refresh_status_async()
                else:
                    detail = f" ({result.reason})" if result.reason else ""
                    self.wifi_settings_status_var.set(f"Add network {ssid!r}: REJECTED{detail}.")

            self.query_async(
                "Wi-Fi add network (UART)",
                lambda: self.wifi_uart_client.add_network(ssid, password),
                apply,
                error_types=(WifiUartQueryError,),
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(f"Sending credentials for {ssid!r} to {host}...")
        self.session_log.info("wifi settings: connect to %r via %s", ssid, host)

        def worker() -> None:
            try:
                self._wifi_http_post_form("/provision", {"ssid": ssid, "password": password})
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi connect failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"Connect failed: {text}"))
                return
            self.post(lambda: self._wifi_settings_status_var.set(
                f"Credentials sent for {ssid!r}. Joining in the background -- Refresh Status "
                "to check progress."
            ))
            self.post(self.wifi_settings_refresh_status_async)

        threading.Thread(target=worker, name="wifi-http-connect", daemon=True).start()

    def wifi_set_mode_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        want_mode = self.wifi_mode_var.get()
        if self._wifi_use_uart():
            mode_byte = WIFI_MODE_AP if want_mode == "ap" else WIFI_MODE_HOME
            self.wifi_settings_status_var.set(
                f"Switching to {'Access Point' if want_mode == 'ap' else 'Home Wi-Fi'} mode over UART..."
            )

            def apply(result: OkReason) -> None:
                if self._wifi_settings_is_open():
                    if result.ok:
                        self.wifi_settings_status_var.set("Mode change: ok.")
                        self.wifi_settings_refresh_status_async()
                    else:
                        detail = f" ({result.reason})" if result.reason else ""
                        self.wifi_settings_status_var.set(f"Mode change: REJECTED{detail}.")

            self.query_async(
                "Wi-Fi set mode (UART)", lambda: self.wifi_uart_client.set_mode(mode_byte), apply,
                error_types=(WifiUartQueryError,),
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(
            f"Switching to {'Access Point' if want_mode == 'ap' else 'Home Wi-Fi'} mode on {host}..."
        )

        def worker() -> None:
            try:
                self._wifi_http_post_form("/provision", {"mode": want_mode})
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi mode change failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"Apply failed: {text}"))
                return
            self.post(self.wifi_settings_refresh_status_async)

        threading.Thread(target=worker, name="wifi-http-mode", daemon=True).start()

    def wifi_set_ap_identity_async(self) -> None:
        if not self._wifi_settings_is_open():
            return
        ap_ssid = self.wifi_ap_ssid_var.get().strip()
        ap_password = self.wifi_ap_password_var.get()
        if not ap_ssid:
            self.wifi_settings_status_var.set("AP SSID is required.")
            return
        if ap_password and len(ap_password) < 8:
            self.wifi_settings_status_var.set(
                "AP password must be empty (open network) or at least 8 characters (WPA2)."
            )
            return
        host = self._wifi_host()
        self.wifi_settings_status_var.set(f"Setting AP identity on {host}...")

        def worker() -> None:
            try:
                self._wifi_http_post_form(
                    "/provision", {"ap_ssid": ap_ssid, "ap_password": ap_password}
                )
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.session_log.warning("Wi-Fi AP identity change failed: %s", text)
                self.post(lambda: self._wifi_settings_status_var.set(f"AP identity change failed: {text}"))
                return
            self.session_log.info("wifi settings: AP identity changed")
            self.post(lambda: self.wifi_settings_status_var.set(
                "AP identity changed. If this PC is joined to the board's AP, it will be "
                "disconnected and must rejoin with the new SSID/password."
            ))

        threading.Thread(target=worker, name="wifi-http-ap-identity", daemon=True).start()

    def _build_firing_status_popup(self, top: tk.Toplevel) -> None:
        host_row = ttk.Frame(top)
        host_row.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Label(host_row, text="Device host/IP:").pack(side="left")
        self.firing_host_var = tk.StringVar(value=self._wifi_default_host())
        ttk.Entry(host_row, textvariable=self.firing_host_var, width=16).pack(
            side="left", padx=(6, 6)
        )
        ttk.Button(
            host_row, text="Refresh Now", command=self.firing_status_refresh_async
        ).pack(side="left")

        profile_frame = ttk.LabelFrame(top, text="Profile executor")
        profile_frame.pack(fill="x", padx=8, pady=4)
        self.firing_profile_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            profile_frame, textvariable=self.firing_profile_var, anchor="w", justify="left",
            wraplength=440,
        ).pack(fill="x", padx=6, pady=6)

        autotune_frame = ttk.LabelFrame(top, text="Autotune")
        autotune_frame.pack(fill="x", padx=8, pady=(4, 8))
        self.firing_autotune_var = tk.StringVar(value="Not queried yet.")
        ttk.Label(
            autotune_frame, textvariable=self.firing_autotune_var, anchor="w", justify="left",
            wraplength=440,
        ).pack(fill="x", padx=6, pady=6)

        ttk.Label(
            top,
            text="Read-only: start/stop a firing or an autotune run from the web dashboard "
            "(Open Web Dashboard on the main window) or the Zones/Profiles pages there.",
            foreground=_MUTED_COLOR, wraplength=440, justify="left",
        ).pack(fill="x", padx=8, pady=(0, 8))

    def _firing_host(self) -> str:
        var = getattr(self, "firing_host_var", None)
        if var is not None:
            host = var.get().strip()
            if host:
                return host
        return self._wifi_default_host()

    def _firing_status_is_open(self) -> bool:
        return self._is_open("firing_status")

    def _firing_schedule_poll(self) -> None:
        self._firing_stop_poll()
        if not self._firing_status_is_open():
            return
        self._firing_poll_id = self.root.after(_FIRING_POLL_MS, self._firing_poll_tick)

    def _firing_poll_tick(self) -> None:
        self._firing_poll_id = None
        if not self._firing_status_is_open():
            return
        self.firing_status_refresh_async(quiet=True)
        self._firing_schedule_poll()

    def _firing_stop_poll(self) -> None:
        if self._firing_poll_id is not None:
            self.root.after_cancel(self._firing_poll_id)
            self._firing_poll_id = None

    def firing_status_refresh_async(self, quiet: bool = False) -> None:
        if not self._firing_status_is_open():
            return
        host = self._firing_host()

        def worker() -> None:
            try:
                profile = self._wifi_http_get_from("/api/profile_exec", host)
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                if not quiet:
                    self.session_log.warning("Firing status query failed: %s", text)
                self.post(lambda: self._apply_firing_profile_error(text))
                return
            try:
                autotune = self._wifi_http_get_from("/api/autotune", host)
            except Exception as exc:  # pragma: no cover - network/device dependent
                text = self._wifi_http_error_text(exc)
                self.post(lambda: self._apply_firing_profile(profile))
                self.post(lambda: self._apply_firing_autotune_error(text))
                return
            self.post(lambda: self._apply_firing_profile(profile))
            self.post(lambda: self._apply_firing_autotune(autotune))

        threading.Thread(target=worker, name="firing-status-http", daemon=True).start()

    def _apply_firing_profile_error(self, text: str) -> None:
        if self._firing_status_is_open():
            self.firing_profile_var.set(f"Query failed: {text}")
            self.firing_autotune_var.set(f"Query failed: {text}")

    def _apply_firing_autotune_error(self, text: str) -> None:
        if self._firing_status_is_open():
            self.firing_autotune_var.set(f"Query failed: {text}")

    def _apply_firing_profile(self, data: dict) -> None:
        """TODO.md 6A.5: a profile can now drive more than one zone at once
        off a single shared ramp/dwell schedule (ramp-lock), so the status
        payload grew a `zones` array instead of one flat zone/actual/duty
        set -- see dashboard_http.c's append_zone_status_json()."""
        if not self._firing_status_is_open():
            return
        state = data.get("state", "unknown")
        if state == "idle":
            self.firing_profile_var.set("Idle -- no profile running.")
            return
        mode_names = {0: "OFF", 1: "BANGBANG", 2: "PID"}
        lines = [
            f"State: {state}   Zones: {data.get('zone_mask', 0):#04x}   "
            f"Profile: {data.get('profile_name', '')!r} (id {data.get('profile_id', '?')})",
            f"Segment {data.get('segment_index', 0) + 1}/{data.get('segment_count', 0)}"
            f"{'  (dwelling)' if data.get('dwelling') else '  (ramping)'}"
            f"   Shared target: {data.get('target_c', 0):.1f}C",
        ]
        if data.get("ramp_lock_held"):
            lines.append(f"Ramp-lock held -- waiting on zone mask {data.get('ramp_lock_lagging_mask', 0):#04x}")
        for z in data.get("zones", []):
            actual = f"{z.get('actual_c', 0):.1f}C" if z.get("actual_valid") else "no reading"
            mode = mode_names.get(z.get("control_mode", 0), "?")
            status = f"FAULT(guard {z.get('fault_guard', '?')})" if z.get("faulted") else (
                "ON" if z.get("relay_on") else "off"
            )
            lines.append(f"  Zone {z.get('zone', '?')}: {actual}   {mode}   {status}   duty={z.get('duty', 0):.2f}")
            if z.get("faulted") and z.get("fault_reason"):
                lines.append(f"    -> {z.get('fault_reason')}")
        if state == "faulted" and data.get("fault_reason"):
            lines.append(f"FAULT (guard {data.get('fault_guard', '?')}): {data.get('fault_reason', '')}")
        self.firing_profile_var.set("\n".join(lines))

    def _apply_firing_autotune(self, data: dict) -> None:
        if not self._firing_status_is_open():
            return
        state = data.get("state", "unknown")
        if state == "idle":
            self.firing_autotune_var.set("Idle -- no autotune run active.")
            return
        lines = [
            f"State: {state}   Zone: {data.get('zone', '?')}   "
            f"Elapsed: {data.get('elapsed_s', 0)}s   Samples: {data.get('sample_count', 0)}",
        ]
        if data.get("actual_valid"):
            lines.append(f"Actual: {data.get('actual_c', 0):.1f}C   Duty: {data.get('duty', 0):.2f}")
        if state == "aborted" and data.get("abort_reason"):
            lines.append(f"Aborted: {data.get('abort_reason')}")
        if state == "done" and data.get("model_valid"):
            lines.append(
                f"Fitted K={data.get('k_gain_c_per_duty', 0):.1f}C/duty  "
                f"tau={data.get('tau_s', 0):.0f}s  L={data.get('dead_time_s', 0):.0f}s"
            )
            lines.append(
                f"Proposed: Kp={data.get('proposed_kp', 0):.4f}  Ki={data.get('proposed_ki', 0):.5f}  "
                f"Kd={data.get('proposed_kd', 0):.4f}"
            )
            lines.append(
                f"Predicted max ramp: ~{data.get('predicted_max_ramp_c_per_hr', 0):.0f} C/hr   "
                "(accept/download trace from the web dashboard's Zones page)"
            )
        self.firing_autotune_var.set("\n".join(lines))

    def _wifi_http_get_from(self, path: str, host: str) -> object:
        """Like _wifi_http_get, but against an explicit host -- the Firing
        Status popup has its own host field so it can point at a device
        without opening the Wi-Fi Settings popup too."""
        url = f"http://{host}{path}"
        with urllib.request.urlopen(url, timeout=_WIFI_HTTP_TIMEOUT_S) as resp:
            return json.loads(resp.read().decode("utf-8"))

    # -- popups ------------------------------------------------------------
    def _popup(self, key: str, title: str, builder: Callable[[tk.Toplevel], None],
               on_close: Optional[Callable[[], None]] = None) -> bool:
        """Open (or raise) a Toplevel. Returns True if it was newly built."""
        existing = self.popups.get(key)
        if existing is not None and existing.winfo_exists():
            existing.deiconify()
            existing.lift()
            existing.focus_force()
            return False

        top = tk.Toplevel(self.root)
        top.title(title)
        top.transient(self.root)

        def close() -> None:
            self.popups.pop(key, None)
            if on_close is not None:
                on_close()
            top.destroy()

        top.protocol("WM_DELETE_WINDOW", close)
        self.popups[key] = top
        builder(top)
        return True

    def _is_open(self, key: str) -> bool:
        top = self.popups.get(key)
        return top is not None and top.winfo_exists()

    def open_thermo_popup(self) -> None:
        self._popup(
            "thermo",
            "Thermocouples (3x MAX31856) - manual control",
            self._build_thermo_popup,
            on_close=self._thermo_stop_reporting,
        )
        # Reopening an already-open window skips the builder, so (re)arm the
        # live feed here: opening the page should always land on live data.
        self._thermo_start_reporting()

    def open_io_popup(self) -> None:
        self._popup(
            "io",
            "I/O & Relays (SX1509) - manual control",
            self._build_io_popup,
            on_close=self._io_stop_reporting,
        )
        self._io_start_reporting()

    def open_display_popup(self) -> None:
        self._popup("display", "Display (ILI9488) - manual control", self._build_display_popup)

    def open_safety_popup(self) -> None:
        self._popup(
            "safety",
            "Safety processor (RP2040, isolated) - status",
            self._build_safety_popup,
            on_close=self._safety_stop_poll,
        )
        self.safety_refresh_async()
        self._safety_schedule_poll()

    def open_about_popup(self) -> None:
        self._popup("about", "About - pin configuration", self._build_about_popup)
        # Opening an already-open window skips the builder, so refresh here:
        # the point of the window is live data.
        self.refresh_pin_config()

    def open_pinout_reference_popup(self) -> None:
        """Static schematic-derived reference -- the opposite of the "Pin
        Configuration" popup above: no device, no UART, no live data. See
        pinout_reference.py's module docstring for why this is a separate
        page rather than folded into that one."""
        self._popup(
            "pinout_reference", "Full Board Pinout (reference)", self._build_pinout_reference_popup
        )

    def open_log_popup(self) -> None:
        self._popup("log", "Session log", self._build_log_popup)

    def open_device_log_popup(self) -> None:
        self._popup("device_log", "Device Console", self._build_device_log_popup)

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
        channel = self._thermo_cfg_channel()
        self._thermo_mutating_async(
            f"Thermo CH{channel} CJ offset",
            lambda: self.thermo.set_cj_offset(
                channel, _as_float(self.thermo_cj_offset, "CJ offset")
            ),
        )

    def _thermo_write_reg(self) -> None:
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

    # ======================================================================
    # IO page (task 2)
    # ======================================================================
    def _build_io_popup(self, top: tk.Toplevel) -> None:
        """Relays (with their K designators and terminal blocks), digital I/O
        with direction control, live DRDY indicators, and raw registers."""
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

    # ======================================================================
    # DISPLAY page (task 4)
    # ======================================================================
    def _build_display_popup(self, top: tk.Toplevel) -> None:
        """Panel control, primitives, text, and the image/test-pattern blit."""
        self.display_color_var = tk.StringVar(value="0xFFFF")
        self.display_bg_var = tk.StringVar(value="0x0000")

        # -- panel ------------------------------------------------------------
        panel = ttk.LabelFrame(top, text="Panel")
        panel.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Button(
            panel,
            text="Reset (soft)",
            command=lambda: self._display_mutating_async(
                "Display soft reset", lambda: self.display.reset(False)
            ),
        ).grid(row=0, column=0, padx=(8, 4), pady=6)
        ttk.Button(
            panel,
            text="Reset (~RESET pin)",
            command=lambda: self._display_mutating_async(
                "Display hard reset", lambda: self.display.reset(True)
            ),
        ).grid(row=0, column=1, padx=4)
        ttk.Button(
            panel,
            text="Power On",
            command=lambda: self._display_mutating_async(
                "Display power on", lambda: self.display.set_power(True)
            ),
        ).grid(row=0, column=2, padx=(12, 4))
        ttk.Button(
            panel,
            text="Power Off",
            command=lambda: self._display_mutating_async(
                "Display power off", lambda: self.display.set_power(False)
            ),
        ).grid(row=0, column=3, padx=4)

        self.display_rotation = tk.IntVar(value=1)
        ttk.Label(panel, text="Rotation").grid(row=0, column=4, padx=(12, 2))
        ttk.Spinbox(
            panel,
            from_=0,
            to=3,
            textvariable=self.display_rotation,
            width=3,
            command=lambda: self._display_mutating_async(
                f"Display rotation {self.display_rotation.get()}",
                lambda: self.display.set_rotation(self.display_rotation.get()),
            ),
        ).grid(row=0, column=5, padx=2)
        self.display_invert = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            panel,
            text="Invert",
            variable=self.display_invert,
            command=lambda: self._display_mutating_async(
                f"Display invert {self.display_invert.get()}",
                lambda: self.display.set_invert(self.display_invert.get()),
            ),
        ).grid(row=0, column=6, padx=(12, 4))
        ttk.Button(panel, text="Read ID", command=self.display_read_id_async).grid(
            row=0, column=7, padx=(12, 8)
        )
        self.display_id_var = tk.StringVar(value="panel not identified yet")
        ttk.Label(panel, textvariable=self.display_id_var, foreground=_MUTED_COLOR).grid(
            row=1, column=0, columnspan=8, padx=8, pady=(0, 6), sticky="w"
        )

        # -- colour + primitives ------------------------------------------------
        draw = ttk.LabelFrame(top, text="Drawing (colors are RGB565, e.g. 0xF800 = red)")
        draw.pack(fill="x", padx=8, pady=4)
        ttk.Label(draw, text="Color").grid(row=0, column=0, padx=(8, 2), pady=6)
        ttk.Entry(draw, textvariable=self.display_color_var, width=9).grid(row=0, column=1, padx=2)
        ttk.Button(draw, text="Pick RGB...", command=self._display_pick_color).grid(
            row=0, column=2, padx=4
        )
        ttk.Button(
            draw,
            text="Clear Screen",
            command=lambda: self._display_mutating_async(
                "Display clear",
                lambda: self.display.clear(_as_int(self.display_color_var, "color")),
            ),
        ).grid(row=0, column=3, padx=(12, 8))

        self.display_x = tk.IntVar(value=0)
        self.display_y = tk.IntVar(value=0)
        self.display_w = tk.IntVar(value=100)
        self.display_h = tk.IntVar(value=60)
        for column, (label, var) in enumerate(
            (("x/x0", self.display_x), ("y/y0", self.display_y), ("w/x1", self.display_w), ("h/y1", self.display_h))
        ):
            ttk.Label(draw, text=label).grid(row=1, column=column * 2, padx=(8, 2), pady=(0, 6))
            ttk.Spinbox(draw, from_=0, to=1000, textvariable=var, width=6).grid(
                row=1, column=column * 2 + 1, padx=2, pady=(0, 6)
            )
        for column, (label, method) in enumerate(
            (
                ("Fill Rect", lambda d, *a: d.fill_rect(*a)),
                ("Draw Rect", lambda d, *a: d.draw_rect(*a)),
                ("Draw Line", lambda d, *a: d.draw_line(*a)),
            ),
            start=8,
        ):
            ttk.Button(
                draw,
                text=label,
                command=lambda lbl=label, m=method: self._display_mutating_async(
                    f"Display {lbl.lower()}",
                    lambda m=m: m(
                        self.display,
                        self.display_x.get(),
                        self.display_y.get(),
                        self.display_w.get(),
                        self.display_h.get(),
                        _as_int(self.display_color_var, "color"),
                    ),
                ),
            ).grid(row=1, column=column, padx=2, pady=(0, 6))

        # -- text ---------------------------------------------------------------
        text_frame = ttk.LabelFrame(top, text="Text")
        text_frame.pack(fill="x", padx=8, pady=4)
        self.display_text = tk.StringVar(value="KilnCtrl")
        self.display_text_size = tk.IntVar(value=2)
        self.display_text_opaque = tk.BooleanVar(value=True)
        ttk.Entry(text_frame, textvariable=self.display_text, width=30).grid(
            row=0, column=0, columnspan=2, padx=(8, 4), pady=6, sticky="we"
        )
        ttk.Button(
            text_frame,
            text="Print",
            command=lambda: self._display_mutating_async(
                f"Display print {self.display_text.get()!r}",
                lambda: self.display.print_text(self.display_text.get()),
            ),
        ).grid(row=0, column=2, padx=4)
        ttk.Button(
            text_frame,
            text="Set Cursor (x,y above)",
            command=lambda: self._display_mutating_async(
                "Display text cursor",
                lambda: self.display.set_text_cursor(
                    self.display_x.get(), self.display_y.get()
                ),
            ),
        ).grid(row=0, column=3, padx=4)
        ttk.Label(text_frame, text="bg").grid(row=0, column=4, padx=(12, 2))
        ttk.Entry(text_frame, textvariable=self.display_bg_var, width=9).grid(row=0, column=5, padx=2)
        ttk.Label(text_frame, text="size").grid(row=0, column=6, padx=(8, 2))
        ttk.Spinbox(text_frame, from_=1, to=8, textvariable=self.display_text_size, width=3).grid(
            row=0, column=7, padx=2
        )
        ttk.Checkbutton(
            text_frame, text="opaque bg", variable=self.display_text_opaque
        ).grid(row=0, column=8, padx=4)
        ttk.Button(
            text_frame,
            text="Apply Style",
            command=lambda: self._display_mutating_async(
                "Display text style",
                lambda: self.display.set_text_style(
                    _as_int(self.display_color_var, "color"),
                    _as_int(self.display_bg_var, "background"),
                    self.display_text_size.get(),
                    self.display_text_opaque.get(),
                ),
            ),
        ).grid(row=0, column=9, padx=(8, 8))
        text_frame.columnconfigure(0, weight=1)

        # -- image blit ----------------------------------------------------------
        image = ttk.LabelFrame(top, text="Send an image (streamed with BLIT_BEGIN/DATA/END)")
        image.pack(fill="x", padx=8, pady=(4, 8))
        self.display_blit_fit = tk.BooleanVar(value=True)
        self.display_blit_progress = tk.DoubleVar(value=0.0)
        self.display_blit_status = tk.StringVar(value="")

        ttk.Button(image, text="Open Image...", command=self.display_send_image_async).grid(
            row=0, column=0, padx=(8, 4), pady=6
        )
        ttk.Checkbutton(
            image, text="fit (letterbox, keep aspect)", variable=self.display_blit_fit
        ).grid(row=0, column=1, padx=4)
        ttk.Button(image, text="Test Pattern", command=self.display_test_pattern_async).grid(
            row=0, column=2, padx=(12, 4)
        )
        ttk.Progressbar(
            image, variable=self.display_blit_progress, maximum=100.0, length=200
        ).grid(row=0, column=3, padx=(12, 8))
        ttk.Label(image, textvariable=self.display_blit_status, foreground=_MUTED_COLOR).grid(
            row=1, column=0, columnspan=4, padx=8, pady=(0, 6), sticky="w"
        )
        # Say the slow part out loud rather than letting a user conclude the
        # app has hung: 480x320 RGB565 is ~2440 frames at 115200 baud.
        ttk.Label(
            image,
            text="A full 480x320 image is ~2440 protocol frames -- expect about a "
            "minute, filling top-to-bottom as it arrives.",
            foreground=_MUTED_COLOR,
            justify="left",
        ).grid(row=2, column=0, columnspan=4, padx=8, pady=(0, 6), sticky="w")

    def _display_pick_color(self) -> None:
        """Convert an 8-bit RGB triple into the RGB565 the wire carries."""
        text = simpledialog.askstring(
            "Pick color",
            "Enter R,G,B (0-255 each):",
            parent=self.popups.get("display", self.root),
            initialvalue="255,255,255",
        )
        if not text:
            return
        try:
            r, g, b = (int(part.strip()) for part in text.split(","))
            color = devices.rgb565(r, g, b)
        except ValueError as exc:
            self.set_status(f"Color: {exc}", error=True)
            return
        self.display_color_var.set(f"0x{color:04X}")

    def _display_mutating_async(self, description: str, action: "Callable[[], OkReason]") -> None:
        """Run a DISPLAY one-shot write and surface a driver-error refusal
        (with reason, when the firmware gave one) instead of letting it land
        silently in DisplayClient's own consumer thread -- same shape as
        :meth:`_thermo_mutating_async`, wired to :class:`DisplayClient`'s
        ``_write``-based methods instead of the fire-and-forget
        ``send_async`` every one of these used to go through.
        """
        def apply(result: OkReason) -> None:
            if result.ok:
                self.set_status(f"{description}: ok.")
            else:
                detail = f" ({result.reason})" if result.reason else ""
                self.set_status(f"{description}: REJECTED{detail}.", error=True)

        self.query_async(description, action, apply, error_types=(DisplayQueryError,))

    def display_read_id_async(self) -> None:
        def apply(ident) -> None:
            if self._is_open("display"):
                self.display_id_var.set(ident.describe())
            self.set_status(f"Display: {ident.describe()}")

        self.query_async(
            "Display read ID",
            lambda: self.display.read_id(),
            apply,
            error_types=(DisplayQueryError,),
        )

    def display_send_image_async(self) -> None:
        from .display import PILLOW_MISSING_HINT, image_to_rgb565, pillow_available

        if not pillow_available():
            messagebox.showerror(
                "Pillow not installed",
                PILLOW_MISSING_HINT
                + "\n\nThe Test Pattern button works without it and exercises the "
                "same blit path.",
                parent=self.popups.get("display", self.root),
            )
            return
        path = filedialog.askopenfilename(
            title="Choose an image to send to the panel",
            filetypes=[("Images", "*.png *.jpg *.jpeg *.bmp *.gif"), ("All files", "*.*")],
            parent=self.popups.get("display", self.root),
        )
        if not path:
            return

        width, height = self._display_target_size()
        fit = self.display_blit_fit.get()

        def load() -> "tuple[bytes, int, int]":
            return image_to_rgb565(path, width, height, fit=fit)

        self._display_blit_async(f"image {Path(path).name}", load)

    def display_test_pattern_async(self) -> None:
        from .display import test_pattern_rgb565

        width, height = self._display_target_size()

        def load() -> "tuple[bytes, int, int]":
            return test_pattern_rgb565(width, height), width, height

        self._display_blit_async("test pattern", load)

    def _display_target_size(self) -> "tuple[int, int]":
        """Panel size for the current rotation: 1/3 are landscape, 0/2 portrait."""
        if self.display_rotation.get() in (0, 2):
            return DISPLAY_NATIVE_HEIGHT, DISPLAY_NATIVE_WIDTH
        return DISPLAY_NATIVE_WIDTH, DISPLAY_NATIVE_HEIGHT

    def _display_blit_async(
        self, description: str, load: Callable[[], "tuple[bytes, int, int]"]
    ) -> None:
        """Convert on the worker thread, then stream, reporting progress.

        Both halves are slow -- the RGB565 conversion is per-pixel Python and
        the stream is thousands of frames -- so neither belongs on the Tk
        thread. Progress arrives through the same post() marshaling every
        other worker result uses.
        """
        if not self.link.is_connected:
            self.set_status(f"Display {description}: not connected.", error=True)
            return
        if self.info.compatible is not True:
            self.set_status(
                f"Display {description}: refused (protocol version not confirmed).", error=True
            )
            return

        self.display_blit_progress.set(0.0)
        self.display_blit_status.set(f"preparing {description}...")
        self.set_status(f"Display {description}: preparing...")

        def on_progress(sent: int, total: int) -> None:
            percent = 100.0 * sent / max(1, total)
            # Only touch Tk variables from the Tk thread.
            self.post(lambda: self._display_blit_progress(percent, sent, total))

        def worker() -> None:
            try:
                pixels, width, height = load()
            except (RuntimeError, OSError, ValueError) as exc:
                self.session_log.error("display %s failed to load: %s", description, exc)
                self.results.put((f"Display {description}", f"error: {exc}", True))
                self.post(lambda: self.display_blit_status.set(f"error: {exc}"))
                return
            try:
                frames = self.display.blit(0, 0, width, height, pixels, progress=on_progress)
            except (BlitError, ValueError) as exc:
                self.session_log.error("display %s blit failed: %s", description, exc)
                self.results.put((f"Display {description}", f"error: {exc}", True))
                self.post(lambda: self.display_blit_status.set(f"error: {exc}"))
                return
            self.session_log.info(
                "display %s: %dx%d in %d frames", description, width, height, frames
            )
            self.results.put(
                (f"Display {description}", f"OK - {width}x{height} in {frames} frames", False)
            )
            self.post(
                lambda: self.display_blit_status.set(
                    f"sent {width}x{height} in {frames} frames"
                )
            )

        threading.Thread(target=worker, name="uart-display-blit", daemon=True).start()

    def _display_blit_progress(self, percent: float, sent: int, total: int) -> None:
        if not self._is_open("display"):
            return
        self.display_blit_progress.set(percent)
        self.display_blit_status.set(f"streaming... {sent}/{total} frames ({percent:.0f}%)")

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


# ---------------------------------------------------------------------------
# small entry-parsing helpers
#
# Raise ValueError, which send_async already catches and reports on the status
# bar -- the same path a devices.py validation failure takes, so a bad hex
# string and an out-of-range channel are reported identically.
# ---------------------------------------------------------------------------
def _as_int(var: tk.StringVar, name: str) -> int:
    """Parse an entry accepting 0x hex, 0b binary or plain decimal."""
    text = var.get().strip()
    try:
        return int(text, 0)
    except ValueError:
        raise ValueError(f"{name} must be a number (e.g. 0x1F), got {text!r}") from None


def _as_float(var: tk.StringVar, name: str) -> float:
    text = var.get().strip()
    try:
        return float(text)
    except ValueError:
        raise ValueError(f"{name} must be a number, got {text!r}") from None


def main() -> int:
    """Entry point for the ``kilnctrl-gui`` console script."""
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    KilnCtrlApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
