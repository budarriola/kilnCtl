"""Tkinter manual-control GUI for the ESP32-S3 UART link.

Layout:
  * a port bar (combobox + refresh + connect/disconnect + status light);
  * a ``manualCtrl`` menu with one entry per device, each opening a Toplevel;
  * an ``About`` menu with the live pinout diagram;
  * a ``Logs`` menu (in-app log view + retention setting);
  * a status bar showing the last command's result and the device FW version.

Threading: a send can block for up to ``max_retries * ack_timeout`` (~4 s with
the defaults), so every control action is dispatched to a worker thread. Worker
results come back through a :class:`queue.Queue` drained by a ``root.after``
poll on the Tk mainloop thread -- Tk widgets are only ever touched there. INFO
queries (pin config / FW version) block for even longer, since they wait for a
reply frame *after* the ACK, and use the same pattern.

Reminder (see devices.py): a result of "OK" means the command reached the ESP
task's inbox. The firmware bridge does not report device-level success back --
except on task INFO, which is a genuine query channel (see info.py).
"""

from __future__ import annotations

import queue
import subprocess
import sys
import threading
import tkinter as tk
from collections import deque
from pathlib import Path
from tkinter import messagebox, scrolledtext, simpledialog, ttk
from typing import Callable, Optional

from . import devices, pin_overlay, settings
from .device_log import LogClient
from .devices import FirmwareVersion, LogLine, PinConfigEntry
from .info import InfoClient, InfoQueryError
from .link_hub import get_shared_link
from .protocol import (
    UART_TASK_ID_AD9833,
    UART_TASK_ID_DAC,
    UART_TASK_ID_OLED,
    UART_TASK_ID_SYSTEM,
    LogLevel,
    Waveform,
)
from .serial_link import PortInfo, list_ports, recommend_port
from .session_log import MAX_KEEP_LOGS, MIN_KEEP_LOGS, SessionLogger

_NO_PORTS = "<no serial ports found>"

#: The pinout diagram shipped alongside the code (800x800 PNG; tk.PhotoImage
#: reads PNG natively on Tk 8.6+, so no Pillow dependency).
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

#: Text-widget tag -> color for each firmware log level, applied in
#: _build_device_log_popup / _append_device_log_lines.
_DEVICE_LOG_COLORS: dict[LogLevel, str] = {
    LogLevel.ERROR: "#a11",
    LogLevel.WARN: "#b8860b",
    LogLevel.INFO: "#000000",
    LogLevel.DEBUG: "#555555",
    LogLevel.VERBOSE: "#888888",
}


class UartControlApp:
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

        # Task INFO is registered here, at construction -- not lazily when the
        # About window opens -- so the firmware's once-per-boot version push
        # (info_boot_push_task) is never missed if the board reboots while the
        # GUI is already connected.
        self.info = InfoClient(self.link, on_boot_push=self._on_boot_push)
        self.fw_version: Optional[FirmwareVersion] = None
        self.pin_config: Optional[list[PinConfigEntry]] = None
        #: Show the incompatibility dialog at most once per connection --
        #: reset in toggle_connect() on each fresh connect.
        self._incompatibility_warned = False

        # Device console: task LOG is registered here for the same reason as
        # task INFO above -- so a line logged the instant the board boots
        # (before any Device Console window is ever opened) isn't missed.
        # LogClient's callback runs on its own consumer thread, so it only
        # ever queues; _drain_device_log_lines (Tk thread, via
        # _drain_results) is what actually touches widgets/history.
        self.device_log_lines: "queue.Queue[LogLine]" = queue.Queue()
        self.device_log_history: "deque[LogLine]" = deque(maxlen=_DEVICE_LOG_HISTORY_LINES)
        self.device_log = LogClient(self.link, on_line=self.device_log_lines.put)

        root.title("ESP32-S3 UART Manual Control")
        root.geometry("620x200")
        root.minsize(560, 180)
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
        manual.add_command(label="MCP4728 DAC...", command=self.open_dac_popup)
        manual.add_command(label="AD9833 Generator...", command=self.open_ad9833_popup)
        manual.add_command(label="SSD1306 OLED...", command=self.open_oled_popup)
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
        menubar.add_cascade(label="About", menu=about)

        self.root.config(menu=menubar)

    def _build_port_bar(self) -> None:
        frame = ttk.LabelFrame(self.root, text="Serial port")
        frame.pack(fill="x", padx=8, pady=(8, 4))

        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(
            frame, textvariable=self.port_var, state="readonly", width=52
        )
        self.port_combo.grid(row=0, column=0, columnspan=3, padx=6, pady=(6, 2), sticky="we")

        self.recommend_label = ttk.Label(frame, text="", foreground="#0a7d28")
        self.recommend_label.grid(row=1, column=0, padx=6, sticky="w")

        ttk.Button(frame, text="Refresh", command=self.refresh_ports).grid(
            row=1, column=1, padx=4, pady=(0, 6)
        )
        self.connect_button = ttk.Button(frame, text="Connect", command=self.toggle_connect)
        self.connect_button.grid(row=1, column=2, padx=(4, 6), pady=(0, 6))
        ttk.Button(frame, text="Restart UART", command=self.restart_uart).grid(
            row=1, column=3, padx=(0, 6), pady=(0, 6)
        )

        frame.columnconfigure(0, weight=1)

    def restart_uart(self) -> None:
        """On-demand recovery lever for a stuck/desynced link -- see
        SYSTEM_CMD_RESTART_UART in uart_task_ids.h. Flushes the ESP's UART
        RX ring buffer; does not disconnect or reopen the local port."""
        self.send_async(
            "Restart UART", UART_TASK_ID_SYSTEM, devices.system_restart_uart
        )

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
        self.conn_label = ttk.Label(bar, textvariable=self.conn_var, foreground="#a11")
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
            index = next(
                (i for i, p in enumerate(self.ports) if p.device == target), 0
            )
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

    def _set_connection_state(self) -> None:
        if self.link.is_connected:
            self.conn_var.set(f"Connected: {self.link.port}")
            self.conn_label.config(foreground="#0a7d28")
            self.connect_button.config(text="Disconnect")
            self.port_combo.config(state="disabled")
        else:
            self.conn_var.set("Disconnected")
            self.conn_label.config(foreground="#a11")
            self.connect_button.config(text="Connect")
            self.port_combo.config(state="readonly")

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
        except ValueError as exc:
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

        # The reader thread can die if the cable is yanked; keep the UI honest.
        if not self.link.is_connected and self.connect_button["text"] == "Disconnect":
            self._set_connection_state()
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
        self.fw_label.config(foreground="#a11" if not version.compatible else "")
        self.session_log.info("device firmware: %s", version.describe())
        self.set_status(
            f"Device rebooted: {version.describe()}"
            if booted
            else f"FW version: {version.describe()}"
        )
        self._refresh_about_version()

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
                "layouts may not match. DAC/AD9833/OLED commands are refused "
                "until this is resolved.\n\n"
                "Flash matching firmware, or update pc_tools to match.",
                parent=self.root,
            )

    # -- popups ------------------------------------------------------------
    def _popup(self, key: str, title: str, builder: Callable[[tk.Toplevel], None]) -> None:
        existing = self.popups.get(key)
        if existing is not None and existing.winfo_exists():
            existing.deiconify()
            existing.lift()
            existing.focus_force()
            return
        top = tk.Toplevel(self.root)
        top.title(title)
        top.transient(self.root)
        top.protocol("WM_DELETE_WINDOW", lambda: (self.popups.pop(key, None), top.destroy()))
        self.popups[key] = top
        builder(top)

    def open_dac_popup(self) -> None:
        self._popup("dac", "MCP4728 DAC - manual control", self._build_dac_popup)

    def open_ad9833_popup(self) -> None:
        self._popup("ad9833", "AD9833 Generator - manual control", self._build_ad9833_popup)

    def open_oled_popup(self) -> None:
        self._popup("oled", "SSD1306 OLED - manual control", self._build_oled_popup)

    def open_about_popup(self) -> None:
        self._popup("about", "About - pin configuration", self._build_about_popup)
        # Opening an already-open window skips the builder, so refresh here:
        # the point of the window is live data.
        self.refresh_pin_config()

    def open_log_popup(self) -> None:
        self._popup("log", "Session log", self._build_log_popup)

    def open_device_log_popup(self) -> None:
        self._popup("device_log", "Device Console", self._build_device_log_popup)

    # -- About popup -------------------------------------------------------
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
        yscroll = ttk.Scrollbar(
            canvas_frame, orient="vertical", command=self.about_canvas.yview
        )
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
                fill="#a11",
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
        self.about_tree.column("description", width=300, anchor="w")
        self.about_tree.pack(fill="both", expand=True, padx=6, pady=6)

        ttk.Label(
            legend,
            text=(
                "Highlights are drawn from the device's live reply to\n"
                "INFO_CMD_GET_PIN_CONFIG - not from a static table."
            ),
            foreground="#555",
            justify="left",
        ).pack(anchor="w", padx=6, pady=(0, 6))

    def _about_is_open(self) -> bool:
        top = self.popups.get("about")
        return top is not None and top.winfo_exists()

    def _fw_text(self) -> str:
        return self.fw_version.describe() if self.fw_version else "FW version: unknown"

    def _refresh_about_version(self) -> None:
        if self._about_is_open() and hasattr(self, "about_fw_var"):
            self.about_fw_var.set(self._fw_text())

    def refresh_pin_config(self) -> None:
        """(Re)query the device's pin configuration and redraw the About view."""
        if not self._about_is_open():
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
        if not self._about_is_open():
            return
        self.pin_config = entries or None

        self.about_canvas.delete("overlay")
        unplaced = pin_overlay.draw_overlay(
            self.about_canvas, entries, scale=_PINOUT_SCALE
        )

        self.about_tree.delete(*self.about_tree.get_children())
        for entry in entries:
            # A GPIO with no measured badge position can't be drawn on the
            # image, but it must still be visible somewhere.
            suffix = "  (not on diagram)" if entry in unplaced else ""
            self.about_tree.insert(
                "",
                "end",
                values=(entry.abbrev, entry.gpio, f"{entry.label}{suffix}"),
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

    # -- log popup ---------------------------------------------------------
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

    # -- device console popup -----------------------------------------------
    def _build_device_log_popup(self, top: tk.Toplevel) -> None:
        """Live view of the firmware's ESP_LOGx output (task LOG), color-coded
        by level -- see uart_log_bridge.c for how these lines get here instead
        of the USB-Serial-JTAG console."""
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
        if self._device_log_is_open():
            self.device_log_text.configure(state="normal")
            self.device_log_text.delete("1.0", "end")
            self.device_log_text.configure(state="disabled")

    def _device_log_is_open(self) -> bool:
        top = self.popups.get("device_log")
        return top is not None and top.winfo_exists()

    def _append_device_log_lines(self, lines) -> None:
        """Insert already-decoded lines into the (already unlocked-by-caller
        or freshly built) device console text widget, one tagged line each."""
        for line in lines:
            self.device_log_text.insert("end", f"{line.text}\n", line.level.name)

    def _drain_device_log_lines(self) -> None:
        """Move incoming firmware log lines into history and the open
        console view, mirroring _drain_log_lines' pattern for the session
        log."""
        new: list[LogLine] = []
        try:
            while True:
                new.append(self.device_log_lines.get_nowait())
        except queue.Empty:
            pass
        if not new:
            return
        self.device_log_history.extend(new)

        if not self._device_log_is_open():
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

        top = self.popups.get("log")
        if top is None or not top.winfo_exists():
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

    # -- DAC popup ---------------------------------------------------------
    def _build_dac_popup(self, top: tk.Toplevel) -> None:
        percent_vars: list[tk.DoubleVar] = []
        mode_vars: list[tk.IntVar] = []

        frame = ttk.LabelFrame(top, text="Channel outputs (% of full scale)")
        frame.pack(fill="both", expand=True, padx=8, pady=8)

        for ch in range(devices.DAC_CHANNELS):
            percent = tk.DoubleVar(value=0.0)
            mode = tk.IntVar(value=0)
            percent_vars.append(percent)
            mode_vars.append(mode)

            ttk.Label(frame, text=f"CH{ch}").grid(row=ch, column=0, padx=(6, 4), pady=4)
            ttk.Scale(
                frame,
                from_=0.0,
                to=100.0,
                orient="horizontal",
                variable=percent,
                length=180,
                command=lambda v, p=percent: p.set(round(float(v), 1)),
            ).grid(row=ch, column=1, padx=4, pady=4, sticky="we")
            ttk.Spinbox(
                frame, from_=0.0, to=100.0, increment=0.1, textvariable=percent, width=7
            ).grid(row=ch, column=2, padx=4, pady=4)
            ttk.Button(
                frame,
                text="Set",
                width=6,
                command=lambda c=ch, p=percent: self.send_async(
                    f"DAC CH{c} set {p.get():.1f}%",
                    UART_TASK_ID_DAC,
                    lambda c=c, p=p: devices.dac_set_channel_percent(c, p.get()),
                ),
            ).grid(row=ch, column=3, padx=4, pady=4)
            ttk.Label(frame, text="PD mode").grid(row=ch, column=4, padx=(10, 2))
            ttk.Spinbox(frame, from_=0, to=3, textvariable=mode, width=3).grid(
                row=ch, column=5, padx=2
            )
            ttk.Button(
                frame,
                text="Power Down",
                command=lambda c=ch, m=mode: self.send_async(
                    f"DAC CH{c} power down (mode {m.get()})",
                    UART_TASK_ID_DAC,
                    lambda c=c, m=m: devices.dac_power_down(c, m.get()),
                ),
            ).grid(row=ch, column=6, padx=(4, 6), pady=4)

        frame.columnconfigure(1, weight=1)

        actions = ttk.Frame(top)
        actions.pack(fill="x", padx=8, pady=(0, 8))
        ttk.Button(
            actions,
            text="Set All",
            command=lambda: self.send_async(
                "DAC set all",
                UART_TASK_ID_DAC,
                lambda: devices.dac_set_all_percent([v.get() for v in percent_vars]),
            ),
        ).pack(side="right")
        ttk.Button(
            actions,
            text="Zero All",
            command=lambda: [v.set(0.0) for v in percent_vars],
        ).pack(side="right", padx=6)

    # -- AD9833 popup ------------------------------------------------------
    def _build_ad9833_popup(self, top: tk.Toplevel) -> None:
        freq_var = tk.StringVar(value="1000.0")
        freq_reg = tk.IntVar(value=0)
        phase_var = tk.StringVar(value="0.0")
        phase_reg = tk.IntVar(value=0)
        wave_var = tk.IntVar(value=int(Waveform.SINE))
        sleep_dac = tk.BooleanVar(value=False)
        sleep_mclk = tk.BooleanVar(value=False)

        def as_float(var: tk.StringVar, name: str) -> float:
            try:
                return float(var.get())
            except ValueError:
                raise ValueError(f"{name} must be a number, got {var.get()!r}") from None

        # Frequency
        fr = ttk.LabelFrame(top, text="Frequency")
        fr.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Entry(fr, textvariable=freq_var, width=14).grid(row=0, column=0, padx=6, pady=6)
        ttk.Label(fr, text="Hz").grid(row=0, column=1)
        ttk.Radiobutton(fr, text="FREQ0", variable=freq_reg, value=0).grid(row=0, column=2, padx=6)
        ttk.Radiobutton(fr, text="FREQ1", variable=freq_reg, value=1).grid(row=0, column=3)
        ttk.Button(
            fr,
            text="Set Frequency",
            command=lambda: self.send_async(
                f"AD9833 FREQ{freq_reg.get()} = {freq_var.get()} Hz",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_set_frequency(
                    freq_reg.get(), as_float(freq_var, "frequency")
                ),
            ),
        ).grid(row=0, column=4, padx=6)
        ttk.Button(
            fr,
            text="Select Active Freq Reg",
            command=lambda: self.send_async(
                f"AD9833 select FREQ{freq_reg.get()}",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_select_freq_reg(freq_reg.get()),
            ),
        ).grid(row=0, column=5, padx=(4, 6))

        # Phase
        ph = ttk.LabelFrame(top, text="Phase")
        ph.pack(fill="x", padx=8, pady=4)
        ttk.Entry(ph, textvariable=phase_var, width=14).grid(row=0, column=0, padx=6, pady=6)
        ttk.Label(ph, text="deg").grid(row=0, column=1)
        ttk.Radiobutton(ph, text="PHASE0", variable=phase_reg, value=0).grid(row=0, column=2, padx=6)
        ttk.Radiobutton(ph, text="PHASE1", variable=phase_reg, value=1).grid(row=0, column=3)
        ttk.Button(
            ph,
            text="Set Phase",
            command=lambda: self.send_async(
                f"AD9833 PHASE{phase_reg.get()} = {phase_var.get()} deg",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_set_phase(
                    phase_reg.get(), as_float(phase_var, "phase")
                ),
            ),
        ).grid(row=0, column=4, padx=6)
        ttk.Button(
            ph,
            text="Select Active Phase Reg",
            command=lambda: self.send_async(
                f"AD9833 select PHASE{phase_reg.get()}",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_select_phase_reg(phase_reg.get()),
            ),
        ).grid(row=0, column=5, padx=(4, 6))

        # Waveform
        wf = ttk.LabelFrame(top, text="Waveform")
        wf.pack(fill="x", padx=8, pady=4)
        for col, wave in enumerate(Waveform):
            ttk.Radiobutton(
                wf, text=devices.WAVEFORM_LABELS[wave], variable=wave_var, value=int(wave)
            ).grid(row=0, column=col, padx=6, pady=6)
        ttk.Button(
            wf,
            text="Apply Waveform",
            command=lambda: self.send_async(
                f"AD9833 waveform {devices.WAVEFORM_LABELS[Waveform(wave_var.get())]}",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_set_waveform(wave_var.get()),
            ),
        ).grid(row=0, column=len(Waveform), padx=(12, 6))

        # Reset / sleep
        misc = ttk.LabelFrame(top, text="Reset / Sleep")
        misc.pack(fill="x", padx=8, pady=(4, 8))
        ttk.Button(
            misc,
            text="Reset (hold)",
            command=lambda: self.send_async(
                "AD9833 reset hold",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_reset(True),
            ),
        ).grid(row=0, column=0, padx=6, pady=6)
        ttk.Button(
            misc,
            text="Reset (release)",
            command=lambda: self.send_async(
                "AD9833 reset release",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_reset(False),
            ),
        ).grid(row=0, column=1, padx=6)
        ttk.Checkbutton(misc, text="DAC power-down", variable=sleep_dac).grid(
            row=0, column=2, padx=(16, 4)
        )
        ttk.Checkbutton(misc, text="MCLK power-down", variable=sleep_mclk).grid(
            row=0, column=3, padx=4
        )
        ttk.Button(
            misc,
            text="Apply Sleep",
            command=lambda: self.send_async(
                f"AD9833 sleep (dac={sleep_dac.get()}, mclk={sleep_mclk.get()})",
                UART_TASK_ID_AD9833,
                lambda: devices.ad9833_sleep(sleep_dac.get(), sleep_mclk.get()),
            ),
        ).grid(row=0, column=4, padx=(12, 6))

    # -- OLED popup ----------------------------------------------------------
    def _build_oled_popup(self, top: tk.Toplevel) -> None:
        text_var = tk.StringVar(value="kilnCtl")
        col_var = tk.IntVar(value=0)
        row_var = tk.IntVar(value=0)
        contrast_var = tk.IntVar(value=0xCF)
        invert_var = tk.BooleanVar(value=False)

        # Text
        tf = ttk.LabelFrame(top, text="Text")
        tf.pack(fill="x", padx=8, pady=(8, 4))
        ttk.Entry(tf, textvariable=text_var, width=32).grid(
            row=0, column=0, columnspan=4, padx=6, pady=6, sticky="we"
        )
        ttk.Button(
            tf,
            text="Print",
            command=lambda: self.send_async(
                f"OLED print {text_var.get()!r}",
                UART_TASK_ID_OLED,
                lambda: devices.oled_print(text_var.get()),
            ),
        ).grid(row=0, column=4, padx=(4, 6), pady=6)
        ttk.Button(
            tf,
            text="Write && Show",
            command=lambda: self._oled_write_and_show(text_var.get(), col_var.get(), row_var.get()),
        ).grid(row=0, column=5, padx=(0, 6), pady=6)
        tf.columnconfigure(0, weight=1)

        # Cursor
        cf = ttk.LabelFrame(top, text="Cursor (text-cell coordinates)")
        cf.pack(fill="x", padx=8, pady=4)
        ttk.Label(cf, text="Col").grid(row=0, column=0, padx=(6, 2))
        ttk.Spinbox(cf, from_=0, to=255, textvariable=col_var, width=5).grid(row=0, column=1, padx=2)
        ttk.Label(cf, text="Row").grid(row=0, column=2, padx=(10, 2))
        ttk.Spinbox(cf, from_=0, to=255, textvariable=row_var, width=5).grid(row=0, column=3, padx=2)
        ttk.Button(
            cf,
            text="Set Cursor",
            command=lambda: self.send_async(
                f"OLED cursor ({col_var.get()},{row_var.get()})",
                UART_TASK_ID_OLED,
                lambda: devices.oled_set_cursor(col_var.get(), row_var.get()),
            ),
        ).grid(row=0, column=4, padx=(10, 6), pady=6)

        # Framebuffer / panel
        ff = ttk.LabelFrame(top, text="Framebuffer / panel")
        ff.pack(fill="x", padx=8, pady=4)
        ttk.Button(
            ff,
            text="Clear",
            command=lambda: self.send_async("OLED clear", UART_TASK_ID_OLED, devices.oled_clear),
        ).grid(row=0, column=0, padx=6, pady=6)
        ttk.Button(
            ff,
            text="Display (flush to panel)",
            command=lambda: self.send_async("OLED display", UART_TASK_ID_OLED, devices.oled_display),
        ).grid(row=0, column=1, padx=6)

        # Contrast / invert / power
        mf = ttk.LabelFrame(top, text="Contrast / invert / power")
        mf.pack(fill="x", padx=8, pady=(4, 8))
        ttk.Scale(
            mf,
            from_=0,
            to=255,
            orient="horizontal",
            variable=contrast_var,
            length=140,
            command=lambda v: contrast_var.set(round(float(v))),
        ).grid(row=0, column=0, padx=(6, 4), pady=6)
        ttk.Spinbox(mf, from_=0, to=255, textvariable=contrast_var, width=5).grid(row=0, column=1, padx=2)
        ttk.Button(
            mf,
            text="Set Contrast",
            command=lambda: self.send_async(
                f"OLED contrast {contrast_var.get()}",
                UART_TASK_ID_OLED,
                lambda: devices.oled_set_contrast(contrast_var.get()),
            ),
        ).grid(row=0, column=2, padx=(4, 10))
        ttk.Checkbutton(mf, text="Invert", variable=invert_var).grid(row=0, column=3, padx=4)
        ttk.Button(
            mf,
            text="Apply Invert",
            command=lambda: self.send_async(
                f"OLED invert {invert_var.get()}",
                UART_TASK_ID_OLED,
                lambda: devices.oled_set_invert(invert_var.get()),
            ),
        ).grid(row=0, column=4, padx=(4, 10))
        ttk.Button(
            mf,
            text="Power On",
            command=lambda: self.send_async(
                "OLED power on", UART_TASK_ID_OLED, lambda: devices.oled_set_power(True)
            ),
        ).grid(row=0, column=5, padx=4)
        ttk.Button(
            mf,
            text="Power Off",
            command=lambda: self.send_async(
                "OLED power off", UART_TASK_ID_OLED, lambda: devices.oled_set_power(False)
            ),
        ).grid(row=0, column=6, padx=(4, 6))

    def _oled_write_and_show(self, text: str, col: int, row: int) -> None:
        """Convenience: clear -> set cursor -> print -> display, one call.

        Each step is its own protocol send (same as every other button here),
        run sequentially on one worker thread rather than via send_async
        (which only knows how to fire a single send), stopping at the first
        one that doesn't come back OK.
        """
        if not self.link.is_connected:
            self.set_status("OLED write & show: not connected.", error=True)
            return
        if self.info.compatible is not True:
            self.set_status("OLED write & show: refused (protocol version not confirmed).", error=True)
            return

        description = f"OLED write & show {text!r} @ ({col},{row})"
        self.set_status(f"{description}: sending...")
        self.session_log.info("send: %s", description)

        def worker() -> None:
            steps = (
                ("clear", devices.oled_clear()),
                ("set cursor", devices.oled_set_cursor(col, row)),
                ("print", devices.oled_print(text)),
                ("display", devices.oled_display()),
            )
            for step_name, payload in steps:
                try:
                    result = self.link.send(
                        dst_task=UART_TASK_ID_OLED, src_task=UART_TASK_ID_OLED, payload=payload
                    )
                except Exception as exc:  # pragma: no cover - defensive
                    self.session_log.error("send: %s (%s) -> error: %s", description, step_name, exc)
                    self.results.put((description, f"error at {step_name}: {exc}", True))
                    return
                self.session_log.info("send: %s (%s) -> %s", description, step_name, result.value)
                if not result.ok:
                    self.results.put((description, f"{step_name}: {result.describe()}", True))
                    return
            self.results.put((description, "OK", False))

        threading.Thread(target=worker, name="uart-send-oled", daemon=True).start()

    # -- shutdown ----------------------------------------------------------
    def on_close(self) -> None:
        try:
            self.info.close()
            self.device_log.close()
            # close(), not disconnect(): this link may be shared with another
            # process (see link_hub.py) -- closing the window shouldn't yank
            # the physical port out from under it. An explicit Disconnect
            # click (toggle_connect) is the only thing that should do that.
            self.link.close()
            self.session_log.close()
        finally:
            self.root.destroy()


def main() -> int:
    """Entry point for the ``uart-control-gui`` console script."""
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    UartControlApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
