"""Tkinter GUI skeleton for kilnsim -- ``firmware/SimFW/docs/PLAN.md`` section 6.3.

Panels: zone-temperature strip chart (truth vs reported vs safety-reported
per zone), relay lamp row with edge history, per-TC panel (register hexdump,
shadow truth, fault buttons, MODEL/MANUAL toggle + manual temp entry), CT
panel (per-channel amps slider, distortion combo, live commanded-vs-
calibrated readback), discrete I/O panel (E-stop, DUT power, J20, fault-line
lamp), fault scheduler timeline (armed faults on a sim-time axis), scenario
runner (load YAML, progress, live expectation status, report view).

This pass is the window/panel *structure and layout*, not pixel-perfect
charts -- PLAN.md's own scope for this deliverable ("the point of this pass
is the window/panel structure and layout"). Panels are minimally functional
against :class:`~kilnsim.link.MockSimLink` today; real data wiring is a
follow-on once hardware exists to wire them to.

Session logging to ``tools/PcTools/logs/kilnsim/``, matching the existing
console-capture convention `kilnctrl.session_log` uses (PLAN.md sec 6.3).
"""

from __future__ import annotations

import json
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk
from typing import Optional

from .link import MockSimLink, SerialSimLink, SimLink, SimLinkError
from .protocol import CommandGroup, CtCmd, IoCmd, RelayCmd, TcCmd
from .report import evaluate_expectations
from .scenario import ScenarioError, load_scenario

#: tools/PcTools/logs/kilnsim/  (this file is tools/PcTools/src/kilnsim/gui.py)
_PROJECT_DIR = Path(__file__).resolve().parents[2]
LOG_DIR = _PROJECT_DIR / "logs" / "kilnsim"

_TC_CHANNELS = 4
_CT_CHANNELS = 3
_RELAYS = ("K1", "K2", "K3", "K5", "K4")


class _WorkQueue:
    """Dispatches link calls off the Tk mainloop thread, same pattern as
    kilnctrl.gui: results come back through a queue.Queue drained by a
    root.after poll, since a send can block on a real link."""

    def __init__(self, root: tk.Tk) -> None:
        self._root = root
        self._results: "queue.Queue[tuple]" = queue.Queue()
        root.after(100, self._drain)

    def submit(self, fn, on_done) -> None:
        def worker():
            try:
                result = fn()
            except Exception as exc:  # noqa: BLE001
                self._results.put((on_done, None, exc))
            else:
                self._results.put((on_done, result, None))

        threading.Thread(target=worker, daemon=True).start()

    def _drain(self) -> None:
        try:
            while True:
                on_done, result, exc = self._results.get_nowait()
                on_done(result, exc)
        except queue.Empty:
            pass
        self._root.after(100, self._drain)


class KilnSimGui:
    def __init__(self, root: tk.Tk, link: Optional[SimLink] = None) -> None:
        self.root = root
        self.root.title("kilnsim")
        self.link: SimLink = link if link is not None else MockSimLink()
        self.work = _WorkQueue(root)
        self.last_report = None

        self._build_menu()
        self._build_status_bar()
        self._build_notebook()

    # -- chrome ---------------------------------------------------------------
    def _build_menu(self) -> None:
        menubar = tk.Menu(self.root)

        link_menu = tk.Menu(menubar, tearoff=0)
        link_menu.add_command(label="Connect (mock)...", command=self._connect_mock)
        link_menu.add_command(label="Connect (serial)...", command=self._connect_serial)
        link_menu.add_command(label="Disconnect", command=self._disconnect)
        menubar.add_cascade(label="Link", menu=link_menu)

        self.root.config(menu=menubar)

    def _build_status_bar(self) -> None:
        bar = ttk.Frame(self.root)
        bar.pack(side=tk.BOTTOM, fill=tk.X)
        self.status_var = tk.StringVar(value="not connected")
        ttk.Label(bar, textvariable=self.status_var, anchor="w").pack(side=tk.LEFT, padx=4, pady=2)

    def _set_status(self, text: str) -> None:
        self.status_var.set(text)

    def _connect_mock(self) -> None:
        self.link = MockSimLink()
        self._connect_common()

    def _connect_serial(self) -> None:
        self.link = SerialSimLink()
        self._connect_common()

    def _connect_common(self) -> None:
        def do_connect():
            return self.link.connect()

        def done(result, exc):
            if exc is not None:
                self._set_status(f"connect failed: {exc}")
                messagebox.showerror("kilnsim", f"Could not connect: {exc}")
                return
            self._set_status(f"connected: {result}")

        self.work.submit(do_connect, done)

    def _disconnect(self) -> None:
        if self.link.is_connected:
            self.link.disconnect()
        self._set_status("not connected")

    # -- notebook ---------------------------------------------------------------
    def _build_notebook(self) -> None:
        nb = ttk.Notebook(self.root)
        nb.pack(fill=tk.BOTH, expand=True)

        self.zone_panel = ZoneChartPanel(nb, self)
        self.relay_panel = RelayPanel(nb, self)
        self.tc_panel = TcPanel(nb, self)
        self.ct_panel = CtPanel(nb, self)
        self.io_panel = IoPanel(nb, self)
        self.fault_panel = FaultSchedulerPanel(nb, self)
        self.scenario_panel = ScenarioRunnerPanel(nb, self)

        for panel, label in (
            (self.zone_panel, "Zones"),
            (self.relay_panel, "Relays"),
            (self.tc_panel, "Thermocouples"),
            (self.ct_panel, "Current Transformers"),
            (self.io_panel, "Discrete I/O"),
            (self.fault_panel, "Fault Scheduler"),
            (self.scenario_panel, "Scenario Runner"),
        ):
            nb.add(panel, text=label)


# ---------------------------------------------------------------------------
# panels
# ---------------------------------------------------------------------------
class ZoneChartPanel(ttk.Frame):
    """Zone-temperature strip chart: truth vs reported vs safety-reported,
    per zone. A Canvas-drawn line chart -- no charting dependency -- refreshed
    on demand; a live tail is the follow-on once telemetry push is wired to a
    real link."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self.canvas = tk.Canvas(self, height=240, background="white")
        self.canvas.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        ttk.Button(self, text="Refresh", command=self._refresh).pack(anchor="w", padx=4, pady=2)
        self._history: dict = {}  # zone index -> list[(t, truth, reported, safety)]

    def _refresh(self) -> None:
        def do_read():
            return self.app.link.send_command(CommandGroup.SYS, 100)  # GET_STATE convenience

        def done(result, exc):
            if exc is not None:
                return
            self._render(result)

        self.app.work.submit(do_read, done)

    def _render(self, state: dict) -> None:
        self.canvas.delete("all")
        zones = state.get("zones", [])
        w = max(self.canvas.winfo_width(), 200)
        h = max(self.canvas.winfo_height(), 100)
        if not zones:
            self.canvas.create_text(w / 2, h / 2, text="(no zone data)")
            return
        bar_w = w / max(1, len(zones))
        for i, z in enumerate(zones):
            t = z.get("t_zone", 0.0)
            bar_h = min(h - 10, max(2, t))
            x0 = i * bar_w + 4
            self.canvas.create_rectangle(x0, h - bar_h, x0 + bar_w - 8, h, fill="#d9534f")
            self.canvas.create_text(x0 + (bar_w - 8) / 2, h - bar_h - 10, text=f"Z{i}: {t:.1f}C")


class RelayPanel(ttk.Frame):
    """Relay lamp row with edge history."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self.lamps: dict = {}
        row = ttk.Frame(self)
        row.pack(fill=tk.X, padx=4, pady=4)
        for relay in _RELAYS:
            frame = ttk.Frame(row)
            frame.pack(side=tk.LEFT, padx=6)
            lamp = tk.Canvas(frame, width=24, height=24, highlightthickness=1, highlightbackground="black")
            oval = lamp.create_oval(2, 2, 22, 22, fill="grey")
            lamp.pack()
            ttk.Label(frame, text=relay).pack()
            self.lamps[relay] = (lamp, oval)
        ttk.Button(self, text="Refresh", command=self._refresh).pack(anchor="w", padx=4, pady=2)
        self.edges_view = scrolledtext.ScrolledText(self, height=10, state="disabled")
        self.edges_view.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    def _refresh(self) -> None:
        def do_read():
            return self.app.link.send_command(CommandGroup.RELAY, RelayCmd.GET_STATES)

        def done(result, exc):
            if exc is not None:
                return
            for relay, (lamp, oval) in self.lamps.items():
                on = bool((result or {}).get(relay, False))
                lamp.itemconfig(oval, fill="green" if on else "grey")

        self.app.work.submit(do_read, done)


class TcPanel(ttk.Frame):
    """Per-channel register hexdump, shadow truth, fault buttons,
    MODEL/MANUAL toggle + manual temp entry."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self.channel_var = tk.IntVar(value=0)
        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=4, pady=4)
        ttk.Label(top, text="Channel:").pack(side=tk.LEFT)
        ttk.Spinbox(top, from_=0, to=_TC_CHANNELS - 1, textvariable=self.channel_var, width=4).pack(side=tk.LEFT)
        ttk.Button(top, text="Read regs", command=self._read_regs).pack(side=tk.LEFT, padx=6)

        self.regs_view = scrolledtext.ScrolledText(self, height=8, state="disabled")
        self.regs_view.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        mode_row = ttk.Frame(self)
        mode_row.pack(fill=tk.X, padx=4, pady=4)
        self.mode_var = tk.StringVar(value="model")
        ttk.Radiobutton(mode_row, text="MODEL", variable=self.mode_var, value="model").pack(side=tk.LEFT)
        ttk.Radiobutton(mode_row, text="MANUAL", variable=self.mode_var, value="manual").pack(side=tk.LEFT)
        self.manual_temp_var = tk.DoubleVar(value=20.0)
        ttk.Entry(mode_row, textvariable=self.manual_temp_var, width=8).pack(side=tk.LEFT, padx=4)
        ttk.Button(mode_row, text="Apply mode", command=self._apply_mode).pack(side=tk.LEFT, padx=4)

        fault_row = ttk.Frame(self)
        fault_row.pack(fill=tk.X, padx=4, pady=4)
        self.fault_var = tk.StringVar(value="disconnected")
        ttk.Combobox(
            fault_row, textvariable=self.fault_var,
            values=["disconnected", "shorted", "stuck", "noisy", "drifting", "cj_fault"],
            state="readonly", width=16,
        ).pack(side=tk.LEFT)
        ttk.Button(fault_row, text="Inject", command=self._inject_fault).pack(side=tk.LEFT, padx=4)
        ttk.Button(fault_row, text="Clear all", command=self._clear_faults).pack(side=tk.LEFT, padx=4)

    def _channel(self) -> int:
        return int(self.channel_var.get())

    def _read_regs(self) -> None:
        def do_read():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": self._channel()})

        def done(result, exc):
            self.regs_view.configure(state="normal")
            self.regs_view.delete("1.0", tk.END)
            self.regs_view.insert(tk.END, f"error: {exc}" if exc else json.dumps(result, indent=2))
            self.regs_view.configure(state="disabled")

        self.app.work.submit(do_read, done)

    def _apply_mode(self) -> None:
        mode = self.mode_var.get()
        payload = {"channel": self._channel(), "mode": mode}
        if mode == "manual":
            payload["manual_temp"] = self.manual_temp_var.get()

        def do_send():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.SET_MODE, payload)

        self.app.work.submit(do_send, lambda result, exc: None)

    def _inject_fault(self) -> None:
        payload = {"channel": self._channel(), "fault_type": self.fault_var.get(), "params": {}}

        def do_send():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.INJECT_FAULT, payload)

        self.app.work.submit(do_send, lambda result, exc: None)

    def _clear_faults(self) -> None:
        def do_send():
            return self.app.link.send_command(
                CommandGroup.TC, TcCmd.CLEAR_FAULT, {"channel": self._channel(), "fault_slot": None}
            )

        self.app.work.submit(do_send, lambda result, exc: None)


class CtPanel(ttk.Frame):
    """Per-channel amps slider, distortion combo, live commanded-vs-
    calibrated readback."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self.rows = []
        for ch in range(_CT_CHANNELS):
            row = ttk.LabelFrame(self, text=f"CT {ch}")
            row.pack(fill=tk.X, padx=4, pady=4)
            amps_var = tk.DoubleVar(value=0.0)
            ttk.Scale(row, from_=0, to=30, variable=amps_var, orient=tk.HORIZONTAL).pack(
                side=tk.LEFT, fill=tk.X, expand=True, padx=4
            )
            ttk.Label(row, textvariable=amps_var).pack(side=tk.LEFT)
            distortion_var = tk.StringVar(value="none")
            ttk.Combobox(
                row, textvariable=distortion_var,
                values=["none", "dc_offset", "clipping", "dropout"],
                state="readonly", width=12,
            ).pack(side=tk.LEFT, padx=4)
            ttk.Button(row, text="Apply", command=lambda c=ch, a=amps_var, d=distortion_var: self._apply(c, a, d)).pack(
                side=tk.LEFT, padx=4
            )
            self.rows.append((amps_var, distortion_var))

    def _apply(self, channel: int, amps_var: tk.DoubleVar, distortion_var: tk.StringVar) -> None:
        amps = amps_var.get()
        distortion = distortion_var.get()

        def do_send():
            self.app.link.send_command(CommandGroup.CT, CtCmd.SET_MODE, {"channel": channel, "mode": "manual"})
            self.app.link.send_command(CommandGroup.CT, CtCmd.SET_AMPS, {"channel": channel, "amps": amps})
            return self.app.link.send_command(
                CommandGroup.CT, CtCmd.SET_DISTORTION, {"channel": channel, "distortion": {"kind": distortion}}
            )

        self.app.work.submit(do_send, lambda result, exc: None)


class IoPanel(ttk.Frame):
    """E-stop, DUT power, J20, fault-line lamp."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app

        estop_row = ttk.LabelFrame(self, text="E-stop")
        estop_row.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(estop_row, text="Open", command=lambda: self._estop("open")).pack(side=tk.LEFT, padx=4)
        ttk.Button(estop_row, text="Close", command=lambda: self._estop("closed")).pack(side=tk.LEFT, padx=4)

        power_row = ttk.LabelFrame(self, text="DUT power")
        power_row.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(power_row, text="On", command=lambda: self._power(True)).pack(side=tk.LEFT, padx=4)
        ttk.Button(power_row, text="Off", command=lambda: self._power(False)).pack(side=tk.LEFT, padx=4)

        fault_row = ttk.LabelFrame(self, text="Fault line")
        fault_row.pack(fill=tk.X, padx=4, pady=4)
        self.fault_lamp = tk.Canvas(fault_row, width=24, height=24, highlightthickness=1, highlightbackground="black")
        self.fault_oval = self.fault_lamp.create_oval(2, 2, 22, 22, fill="grey")
        self.fault_lamp.pack(side=tk.LEFT, padx=4)
        ttk.Button(fault_row, text="Refresh", command=self._refresh_fault_line).pack(side=tk.LEFT, padx=4)

    def _estop(self, state: str) -> None:
        def do_send():
            return self.app.link.send_command(CommandGroup.IO, IoCmd.ESTOP_SET, {"open": state == "open"})

        self.app.work.submit(do_send, lambda result, exc: None)

    def _power(self, on: bool) -> None:
        def do_send():
            return self.app.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": on})

        self.app.work.submit(do_send, lambda result, exc: None)

    def _refresh_fault_line(self) -> None:
        def do_read():
            return self.app.link.send_command(CommandGroup.IO, IoCmd.FAULT_LINE_GET)

        def done(result, exc):
            if exc is not None:
                return
            asserted = bool((result or {}).get("asserted", False))
            self.fault_lamp.itemconfig(self.fault_oval, fill="red" if asserted else "grey")

        self.app.work.submit(do_read, done)


class FaultSchedulerPanel(ttk.Frame):
    """Armed faults on a sim-time axis. Drag-to-re-time is a follow-on once
    a live timeline has real fault slots to show -- this pass lists slots in
    a table and lets a fault be cancelled or fired now."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(top, text="Refresh", command=self._refresh).pack(side=tk.LEFT)

        columns = ("slot", "type", "target", "state")
        self.tree = ttk.Treeview(self, columns=columns, show="headings")
        for col in columns:
            self.tree.heading(col, text=col)
        self.tree.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        bottom = ttk.Frame(self)
        bottom.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(bottom, text="Fire now", command=self._fire_now).pack(side=tk.LEFT, padx=4)
        ttk.Button(bottom, text="Cancel", command=self._cancel).pack(side=tk.LEFT, padx=4)

    def _selected_slot(self) -> Optional[int]:
        sel = self.tree.selection()
        if not sel:
            return None
        values = self.tree.item(sel[0], "values")
        return int(values[0]) if values else None

    def _refresh(self) -> None:
        from .protocol import FaultCmd

        def do_read():
            return self.app.link.send_command(CommandGroup.FAULT, FaultCmd.LIST)

        def done(result, exc):
            self.tree.delete(*self.tree.get_children())
            if exc is not None:
                return
            for slot in (result or {}).get("slots", []):
                self.tree.insert("", tk.END, values=(
                    slot.get("fault_slot"), slot.get("fault_type"), slot.get("target"), slot.get("state"),
                ))

        self.app.work.submit(do_read, done)

    def _fire_now(self) -> None:
        from .protocol import FaultCmd

        slot = self._selected_slot()
        if slot is None:
            return

        def do_send():
            return self.app.link.send_command(CommandGroup.FAULT, FaultCmd.FIRE_NOW, {"fault_slot": slot})

        self.app.work.submit(do_send, lambda result, exc: self._refresh())

    def _cancel(self) -> None:
        from .protocol import FaultCmd

        slot = self._selected_slot()
        if slot is None:
            return

        def do_send():
            return self.app.link.send_command(CommandGroup.FAULT, FaultCmd.CANCEL, {"fault_slot": slot})

        self.app.work.submit(do_send, lambda result, exc: self._refresh())


class ScenarioRunnerPanel(ttk.Frame):
    """Load YAML, progress, live expectation status, report view."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self.scenario = None

        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=4, pady=4)
        self.path_var = tk.StringVar(value="(no scenario loaded)")
        ttk.Label(top, textvariable=self.path_var).pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Button(top, text="Load YAML...", command=self._load).pack(side=tk.LEFT, padx=4)
        ttk.Button(top, text="Run", command=self._run).pack(side=tk.LEFT, padx=4)

        columns = ("name", "verdict", "detail")
        self.tree = ttk.Treeview(self, columns=columns, show="headings")
        for col in columns:
            self.tree.heading(col, text=col)
        self.tree.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self.report_view = scrolledtext.ScrolledText(self, height=10, state="disabled")
        self.report_view.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    def _load(self) -> None:
        path = filedialog.askopenfilename(filetypes=[("YAML scenarios", "*.yaml *.yml")])
        if not path:
            return
        try:
            self.scenario = load_scenario(path)
        except ScenarioError as exc:
            messagebox.showerror("kilnsim", f"Could not load scenario: {exc}")
            return
        self.path_var.set(f"{path} (v{self.scenario.version}, {len(self.scenario.expect)} expectations)")

    def _run(self) -> None:
        if self.scenario is None:
            messagebox.showwarning("kilnsim", "Load a scenario first")
            return
        scenario = self.scenario

        def do_run():
            events = self.app.link.read_events(timeout=0.0)
            return evaluate_expectations(scenario, events, seed=scenario.seed, timescale=scenario.timescale)

        def done(report, exc):
            self.tree.delete(*self.tree.get_children())
            if exc is not None:
                messagebox.showerror("kilnsim", f"Run failed: {exc}")
                return
            self.app.last_report = report
            for r in report.expectations:
                self.tree.insert("", tk.END, values=(r.name, r.verdict, r.detail))
            self.report_view.configure(state="normal")
            self.report_view.delete("1.0", tk.END)
            self.report_view.insert(tk.END, report.to_json())
            self.report_view.configure(state="disabled")

        self.app.work.submit(do_run, done)


def main() -> int:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    root = tk.Tk()
    KilnSimGui(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
