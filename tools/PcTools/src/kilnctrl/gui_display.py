"""DISPLAY popup (task 4).

Part of the gui.py split (pure refactor) -- moved verbatim, no logic changes.
DisplayMixin is mixed into KilnCtrlApp (see gui.py) so every method below
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


class DisplayMixin:
    # ======================================================================
    # DISPLAY page (task 4)
    # ======================================================================
    def _build_display_popup(self, top: tk.Toplevel) -> None:
        """Panel control, primitives, text, and the image/test-pattern blit."""
        from .gui import _as_int  # local import: avoids a circular import with gui.py

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

