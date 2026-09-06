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


# ---------------------------------------------------------------------------
# The bulk of this class's methods used to live here inline. It is now split
# into per-popup mixins (pure refactor, zero behavior change) -- moved
# verbatim into their own files, mixed back in below so every method still
# runs against the single ``self`` the app constructs. No call site moves:
# `KilnCtrlApp(...)` still has every method it always had. See each mixin
# module's docstring, and gui.py's own module docstring for the overall map.
# ---------------------------------------------------------------------------
from .gui_zones import ZonesMixin  # noqa: E402
from .gui_profiles import ProfilesMixin  # noqa: E402
from .gui_autotune import AutotuneMixin  # noqa: E402
from .gui_danger_zone import DangerZoneMixin  # noqa: E402
from .gui_wifi_firing import WifiFiringMixin  # noqa: E402
from .gui_thermo import ThermoMixin  # noqa: E402
from .gui_io import IoMixin  # noqa: E402
from .gui_display import DisplayMixin  # noqa: E402
from .gui_safety import SafetyMixin  # noqa: E402
from .gui_about import AboutMixin  # noqa: E402
from .gui_pinout import PinoutMixin  # noqa: E402
from .gui_logs import LogsMixin  # noqa: E402


class KilnCtrlApp(
    ZonesMixin,
    ProfilesMixin,
    AutotuneMixin,
    DangerZoneMixin,
    WifiFiringMixin,
    ThermoMixin,
    IoMixin,
    DisplayMixin,
    SafetyMixin,
    AboutMixin,
    PinoutMixin,
    LogsMixin,
):
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

        # Compact safety column (TODO.md "GUI grows a safety column rather
        # than a second application"): a one-line always-visible summary of
        # the SAFETY task's cached status, independent of whether the full
        # Safety Processor popup is open. Detail still lives in that popup
        # (gui_safety.py) -- this is deliberately just enough to notice a
        # problem without opening it.
        self.safety_summary_var = tk.StringVar(value="Safety: --")
        self.safety_summary_label = ttk.Label(
            bar, textvariable=self.safety_summary_var, anchor="e"
        )
        self.safety_summary_label.pack(side="right", padx=8, pady=4)

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
            self._safety_stop_poll()
            self.safety_summary_var.set("Safety: --")
            self.safety_summary_label.config(foreground="")
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
        # Compact safety-column summary (status bar) runs regardless of
        # whether the Safety Processor popup is open -- see _build_status_bar.
        self.safety_refresh_async()
        self._safety_schedule_poll()

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
