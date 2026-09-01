"""Fire Profiles popup (task 9, PROFILES).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
ProfilesMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class ProfilesMixin:
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

