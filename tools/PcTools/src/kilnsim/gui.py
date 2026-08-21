"""Tkinter GUI skeleton for kilnsim -- ``firmware/SimFW/docs/DESIGN_NOTES.md`` section 6.3.

Panels: zone-temperature strip chart (truth vs reported vs safety-reported
per zone), relay lamp row with edge history, per-TC panel (register hexdump,
shadow truth, fault buttons, MODEL/MANUAL toggle + manual temp entry), CT
panel (per-channel amps slider, distortion combo, live commanded-vs-
calibrated readback), discrete I/O panel (E-stop, two independent DUT-power
relays -- main/J18 and safety/J19, each its own control, PROTOCOL.md sec
5.5 -- J20, fault-line lamp), fault scheduler timeline (armed faults on a
sim-time axis), scenario
runner (load YAML, progress, live expectation status, report view).

Now that ``firmware/SimFW/tools/virtual_simfw`` exists (a host program
speaking the real wire protocol, see its README), every panel below is wired
against a real, responding device via :class:`~kilnsim.link.TcpSimLink` --
"Link > Connect (virtual_simfw)..." -- not just
:class:`~kilnsim.link.MockSimLink`. The zone chart, relay lamps, TC panel,
CT panel, and I/O panel poll and command real replies; the fault-scheduler
timeline's drag-to-re-time (DESIGN_NOTES.md 6.3) is the one piece still a documented
TODO (it lists/cancels/fires slots but doesn't re-time them visually) --
lowest priority per this pass's own instructions. Session logging to
``tools/PcTools/logs/kilnsim/``, matching the existing console-capture
convention `kilnctrl.session_log` uses.
"""

from __future__ import annotations

import json
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk
from typing import Optional

from .link import MockSimLink, SerialSimLink, SimLink, SimLinkError, TcpSimLink, get_state_snapshot
from .protocol import CommandGroup, CtCmd, FaultCmd, IoCmd, RelayCmd, SysCmd, TcCmd
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
        self.root.after(500, self._live_poll)

    def _live_poll(self) -> None:
        """Ticks every 500ms: if connected, asks the "live" panels (zone
        chart, relay lamps, fault-line lamp) to pull a fresh reading. Each
        panel guards its own re-entrancy (skips this tick if its previous
        pull is still in flight) so a slow/real link never piles up
        background threads faster than it can answer them."""
        if self.link.is_connected:
            self.zone_panel.poll_live()
            self.relay_panel.poll_live()
            self.io_panel.poll_live()
        self.root.after(500, self._live_poll)

    # -- chrome ---------------------------------------------------------------
    def _build_menu(self) -> None:
        menubar = tk.Menu(self.root)

        link_menu = tk.Menu(menubar, tearoff=0)
        link_menu.add_command(label="Connect (mock)...", command=self._connect_mock)
        link_menu.add_command(label="Connect (serial)...", command=self._connect_serial)
        link_menu.add_command(label="Connect (virtual_simfw)...", command=self._connect_virtual)
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

    def _connect_virtual(self) -> None:
        from tkinter import simpledialog

        address = simpledialog.askstring(
            "kilnsim", "virtual_simfw address (host:port, blank for 127.0.0.1:8765):", parent=self.root
        )
        if address is None:  # Cancel
            return
        self.link = TcpSimLink()
        self._connect_common(address or None)

    def _connect_common(self, address: Optional[str] = None) -> None:
        def do_connect():
            return self.link.connect(address)

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
    """Zone-temperature strip chart: truth (red) vs TC-reported (blue) vs
    safety-reported (green), per zone, over recent history -- DESIGN_NOTES.md sec
    6.3's own description. Pulls one reading per :meth:`poll_live` tick
    (driven by :class:`KilnSimGui`'s 500ms timer) via
    :func:`kilnsim.link.get_state_snapshot`, which works uniformly whether
    the link is a real one (TELEMETRY broadcast) or :class:`MockSimLink`
    (its SYS/100 convenience) -- see that function's docstring for the bug
    it replaced (a bare ``SYS/100`` send used to crash uncaught against any
    real link)."""

    _MAX_SAMPLES = 120  # 500ms/tick * 120 = 1 minute of history on screen

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self._busy = False

        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=4, pady=2)
        ttk.Label(top, text="Zone:").pack(side=tk.LEFT)
        self.zone_var = tk.IntVar(value=0)
        ttk.Spinbox(top, from_=0, to=3, textvariable=self.zone_var, width=4).pack(side=tk.LEFT)
        ttk.Button(top, text="Refresh now", command=self.poll_live).pack(side=tk.LEFT, padx=6)
        self.readout_var = tk.StringVar(value="(no data yet)")
        ttk.Label(top, textvariable=self.readout_var).pack(side=tk.LEFT, padx=12)

        legend = ttk.Frame(self)
        legend.pack(fill=tk.X, padx=4)
        for text, color in (("truth", "#d9534f"), ("TC reported", "#337ab7"), ("safety reported", "#5cb85c")):
            ttk.Label(legend, text=f"— {text}", foreground=color).pack(side=tk.LEFT, padx=6)

        self.canvas = tk.Canvas(self, height=240, background="white")
        self.canvas.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        #: zone index -> list[(truth, tc_reported, safety_reported)], newest last
        self._history: "dict[int, list]" = {}

    def poll_live(self) -> None:
        if self._busy or not self.app.link.is_connected:
            return
        self._busy = True

        def do_read():
            return get_state_snapshot(self.app.link, wait_s=0.05)

        def done(result, exc):
            self._busy = False
            if exc is not None:
                return
            self._ingest(result)

        self.app.work.submit(do_read, done)

    def _ingest(self, state: dict) -> None:
        zones = state.get("zones", [])
        for i, z in enumerate(zones):
            hist = self._history.setdefault(i, [])
            hist.append((z.get("t_zone", 0.0), z.get("t_tc_reported", 0.0), z.get("t_safety_reported", 0.0)))
            del hist[: max(0, len(hist) - self._MAX_SAMPLES)]
        zi = self.zone_var.get()
        if zones and 0 <= zi < len(zones):
            z = zones[zi]
            self.readout_var.set(
                f"Z{zi}: truth {z.get('t_zone', 0.0):.1f}C  tc {z.get('t_tc_reported', 0.0):.1f}C  "
                f"safety {z.get('t_safety_reported', 0.0):.1f}C  I {z.get('i_amps', 0.0):.2f}A"
            )
        self._render()

    def _render(self) -> None:
        self.canvas.delete("all")
        zi = self.zone_var.get()
        hist = self._history.get(zi, [])
        w = max(self.canvas.winfo_width(), 200)
        h = max(self.canvas.winfo_height(), 100)
        if not hist:
            self.canvas.create_text(w / 2, h / 2, text="(no data yet -- connect and wait for a reading)")
            return
        all_vals = [v for sample in hist for v in sample]
        lo, hi = min(all_vals), max(all_vals)
        if hi - lo < 1.0:
            hi = lo + 1.0
        margin = 20
        plot_w, plot_h = w - 2 * margin, h - 2 * margin

        def x_of(idx: int) -> float:
            return margin + (idx / max(1, len(hist) - 1)) * plot_w

        def y_of(val: float) -> float:
            return margin + plot_h - ((val - lo) / (hi - lo)) * plot_h

        self.canvas.create_text(margin, margin - 8, text=f"{hi:.0f}C", anchor="w")
        self.canvas.create_text(margin, h - margin + 12, text=f"{lo:.0f}C", anchor="w")

        for series_idx, color in ((0, "#d9534f"), (1, "#337ab7"), (2, "#5cb85c")):
            points = []
            for i, sample in enumerate(hist):
                points.extend((x_of(i), y_of(sample[series_idx])))
            if len(points) >= 4:
                self.canvas.create_line(*points, fill=color, width=2)


class RelayPanel(ttk.Frame):
    """Relay lamp row with edge history."""

    #: relay name -> the GET_STATES reply key that carries it
    # (kilnsim.payloads._relay_decode's real field names, PROTOCOL.md sec
    # 5.4) -- the panel used to index the reply by the bare relay name
    # ("K1"), which is not a key GET_STATES actually returns, so every lamp
    # silently stayed grey forever. FAULT_LINE gets its own lamp alongside
    # the 5 relays since GET_STATES reports it in the same reply.
    _STATE_KEYS = {"K1": "k1_closed", "K2": "k2_closed", "K3": "k3_closed", "K5": "k5_closed", "K4": "k4_closed"}
    _LAMPS = _RELAYS + ("FAULT",)

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self._busy = False
        self._last_edge_seq = 0
        self.lamps: dict = {}
        row = ttk.Frame(self)
        row.pack(fill=tk.X, padx=4, pady=4)
        for relay in self._LAMPS:
            frame = ttk.Frame(row)
            frame.pack(side=tk.LEFT, padx=6)
            lamp = tk.Canvas(frame, width=24, height=24, highlightthickness=1, highlightbackground="black")
            oval = lamp.create_oval(2, 2, 22, 22, fill="grey")
            lamp.pack()
            ttk.Label(frame, text=relay).pack()
            self.lamps[relay] = (lamp, oval)
        ttk.Button(self, text="Refresh now", command=self.poll_live).pack(anchor="w", padx=4, pady=2)
        self.edges_view = scrolledtext.ScrolledText(self, height=10, state="disabled")
        self.edges_view.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    def poll_live(self) -> None:
        if self._busy or not self.app.link.is_connected:
            return
        self._busy = True

        def do_read():
            states = self.app.link.send_command(CommandGroup.RELAY, RelayCmd.GET_STATES)
            edges = self.app.link.send_command(
                CommandGroup.RELAY, RelayCmd.GET_EDGES, {"since_seq": self._last_edge_seq, "max_count": 8}
            )
            return states, edges

        def done(result, exc):
            self._busy = False
            if exc is not None:
                return
            states, edges = result
            self._render(states, edges)

        self.app.work.submit(do_read, done)

    def _render(self, states: dict, edges: dict) -> None:
        states = states or {}
        for relay, key in self._STATE_KEYS.items():
            lamp, oval = self.lamps[relay]
            lamp.itemconfig(oval, fill="green" if states.get(key) else "grey")
        lamp, oval = self.lamps["FAULT"]
        lamp.itemconfig(oval, fill="red" if states.get("fault_line_asserted") else "grey")

        for edge in (edges or {}).get("edges", []):
            self._last_edge_seq = max(self._last_edge_seq, edge.get("seq", 0) + 1)
            self.edges_view.configure(state="normal")
            self.edges_view.insert(
                tk.END,
                f"[{edge.get('seq')}] t={edge.get('sim_time_us')}us {edge.get('signal')} "
                f"{'closed' if edge.get('level') else 'open'}\n",
            )
            self.edges_view.see(tk.END)
            self.edges_view.configure(state="disabled")


class TcPanel(ttk.Frame):
    """Per-channel register hexdump, shadow truth, fault buttons,
    MODEL/MANUAL toggle + manual temp entry."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self.channel_var = tk.IntVar(value=0)
        #: channel -> most recently injected fault's slot id (TC_INJECT_FAULT
        #: reply, PROTOCOL.md sec 5.2), so "Clear" has a real slot to target
        #: instead of the old code's bare ``fault_slot: None`` (which would
        #: TypeError inside struct.pack -- CLEAR_FAULT needs a real u16).
        self._last_fault_slot: "dict[int, int]" = {}
        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=4, pady=4)
        ttk.Label(top, text="Channel:").pack(side=tk.LEFT)
        ttk.Spinbox(top, from_=0, to=_TC_CHANNELS - 1, textvariable=self.channel_var, width=4).pack(side=tk.LEFT)
        ttk.Button(top, text="Read regs", command=self._read_regs).pack(side=tk.LEFT, padx=6)
        ttk.Button(top, text="Master config", command=self._read_master_config).pack(side=tk.LEFT, padx=6)

        self.regs_view = scrolledtext.ScrolledText(self, height=8, state="disabled")
        self.regs_view.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        mode_row = ttk.Frame(self)
        mode_row.pack(fill=tk.X, padx=4, pady=4)
        self.mode_var = tk.StringVar(value="model")
        ttk.Radiobutton(mode_row, text="MODEL", variable=self.mode_var, value="model").pack(side=tk.LEFT)
        ttk.Radiobutton(mode_row, text="MANUAL", variable=self.mode_var, value="manual").pack(side=tk.LEFT)
        self.manual_temp_var = tk.DoubleVar(value=20.0)
        ttk.Entry(mode_row, textvariable=self.manual_temp_var, width=8).pack(side=tk.LEFT, padx=4)
        ttk.Label(mode_row, text="C").pack(side=tk.LEFT)
        ttk.Button(mode_row, text="Apply mode", command=self._apply_mode).pack(side=tk.LEFT, padx=4)

        fault_row = ttk.Frame(self)
        fault_row.pack(fill=tk.X, padx=4, pady=4)
        self.fault_var = tk.StringVar(value="tc_disconnected")
        ttk.Combobox(
            fault_row, textvariable=self.fault_var,
            # fault_catalog.FAULT_TYPE_NAMES' TC-kind entries (DESIGN_NOTES.md sec
            # 7.1) -- the panel used to offer "disconnected"/"shorted"/
            # "noisy"/... which are not catalog names at all and would
            # raise FaultCatalogError the instant Inject was clicked.
            values=["tc_disconnected", "tc_shorted", "tc_stuck", "tc_noise", "tc_drift", "cj_fault", "tc_dead_ic",
                    "tc_flaky_spi", "tc_spurious_fault_pin"],
            state="readonly", width=20,
        ).pack(side=tk.LEFT)
        ttk.Button(fault_row, text="Inject", command=self._inject_fault).pack(side=tk.LEFT, padx=4)
        ttk.Button(fault_row, text="Clear last", command=self._clear_faults).pack(side=tk.LEFT, padx=4)
        self.fault_status_var = tk.StringVar(value="")
        ttk.Label(fault_row, textvariable=self.fault_status_var).pack(side=tk.LEFT, padx=8)

    def _channel(self) -> int:
        return int(self.channel_var.get())

    def _read_regs(self) -> None:
        # self._channel() reads a Tk IntVar -- must happen on the main
        # thread, before do_read is handed to _WorkQueue's background
        # worker (Tcl raises "main thread is not in main loop" for a
        # cross-thread variable touch; found while exercising this against
        # a real link, where the round trip is slow enough to reliably hit
        # the race). Pre-existing bug: the same pattern was already here.
        channel = self._channel()

        def do_read():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": channel})

        def done(result, exc):
            self.regs_view.configure(state="normal")
            self.regs_view.delete("1.0", tk.END)
            if exc is not None:
                self.regs_view.insert(tk.END, f"error: {exc}")
            else:
                shown = dict(result)
                regs = shown.get("regs")
                if isinstance(regs, (bytes, bytearray)):
                    shown["regs"] = "0x" + regs.hex()
                self.regs_view.insert(tk.END, json.dumps(shown, indent=2))
            self.regs_view.configure(state="disabled")

        self.app.work.submit(do_read, done)

    def _read_master_config(self) -> None:
        channel = self._channel()  # see _read_regs's comment on why this must happen here

        def do_read():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.GET_MASTER_CONFIG, {"channel": channel})

        def done(result, exc):
            self.regs_view.configure(state="normal")
            self.regs_view.delete("1.0", tk.END)
            self.regs_view.insert(tk.END, f"error: {exc}" if exc else json.dumps(result, indent=2))
            self.regs_view.configure(state="disabled")

        self.app.work.submit(do_read, done)

    def _apply_mode(self) -> None:
        mode = self.mode_var.get()
        # payloads._tc_encode's SET_MODE reads "manual_temp_c" (falling back
        # to "temp_c") -- the old code sent "manual_temp", a key that
        # encoder never looks at, so MANUAL mode silently forced 0.0C.
        payload = {"channel": self._channel(), "mode": mode, "manual_temp_c": self.manual_temp_var.get()}

        def do_send():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.SET_MODE, payload)

        self.app.work.submit(do_send, lambda result, exc: None)

    def _inject_fault(self) -> None:
        channel = self._channel()
        payload = {"channel": channel, "fault_type": self.fault_var.get(), "param0": 0.0}

        def do_send():
            return self.app.link.send_command(CommandGroup.TC, TcCmd.INJECT_FAULT, payload)

        def done(result, exc):
            if exc is not None:
                self.fault_status_var.set(f"error: {exc}")
                return
            slot = (result or {}).get("fault_slot")
            self._last_fault_slot[channel] = slot
            self.fault_status_var.set(f"injected: slot {slot}")

        self.app.work.submit(do_send, done)

    def _clear_faults(self) -> None:
        channel = self._channel()
        slot = self._last_fault_slot.get(channel)
        if slot is None:
            self.fault_status_var.set("nothing injected yet on this channel")
            return

        def do_send():
            return self.app.link.send_command(
                CommandGroup.TC, TcCmd.CLEAR_FAULT, {"channel": channel, "fault_slot": slot}
            )

        def done(result, exc):
            if exc is not None:
                self.fault_status_var.set(f"clear error: {exc}")
                return
            self._last_fault_slot.pop(channel, None)
            self.fault_status_var.set(f"cleared slot {slot}")

        self.app.work.submit(do_send, done)


#: distortion_var combobox choice -> the real distortion dict shape
#: payloads._ct_encode's SET_DISTORTION expects (PROTOCOL.md sec 5.3: f32
#: dc_offset, f32 clip_fraction, u8 dropout_half_cycle, u8
#: dropout_negative_half, u8 apply_immediately). The old code sent
#: {"kind": distortion} instead, a shape that encoder never reads (every
#: field silently defaulted to 0/False), so no combobox choice ever did
#: anything on the wire.
_CT_DISTORTION_PRESETS = {
    "none": {},
    "dc_offset": {"dc_offset": 2.0},
    "clipping": {"clip_fraction": 0.3},
    "dropout": {"dropout_half_cycle": True},
}


class CtPanel(ttk.Frame):
    """Per-channel amps slider, distortion combo, live commanded-vs-
    calibrated readback (CT_GET_STATE)."""

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
                values=list(_CT_DISTORTION_PRESETS),
                state="readonly", width=12,
            ).pack(side=tk.LEFT, padx=4)
            ttk.Button(row, text="Apply", command=lambda c=ch, a=amps_var, d=distortion_var: self._apply(c, a, d)).pack(
                side=tk.LEFT, padx=4
            )
            readback_var = tk.StringVar(value="(no reading yet)")
            ttk.Label(row, textvariable=readback_var).pack(side=tk.LEFT, padx=8)
            ttk.Button(row, text="Read state", command=lambda c=ch, r=readback_var: self._read_state(c, r)).pack(
                side=tk.LEFT, padx=4
            )
            self.rows.append((amps_var, distortion_var, readback_var))

    def _apply(self, channel: int, amps_var: tk.DoubleVar, distortion_var: tk.StringVar) -> None:
        amps = amps_var.get()
        distortion = _CT_DISTORTION_PRESETS[distortion_var.get()]

        def do_send():
            self.app.link.send_command(CommandGroup.CT, CtCmd.SET_MODE, {"channel": channel, "mode": "manual"})
            self.app.link.send_command(CommandGroup.CT, CtCmd.SET_AMPS, {"channel": channel, "amps": amps})
            return self.app.link.send_command(
                CommandGroup.CT, CtCmd.SET_DISTORTION, {"channel": channel, "distortion": distortion}
            )

        self.app.work.submit(do_send, lambda result, exc: None)

    def _read_state(self, channel: int, readback_var: tk.StringVar) -> None:
        def do_read():
            return self.app.link.send_command(CommandGroup.CT, CtCmd.GET_STATE, {"channel": channel})

        def done(result, exc):
            if exc is not None:
                readback_var.set(f"error: {exc}")
                return
            mode = "MANUAL" if result.get("mode") else "MODEL"
            readback_var.set(
                f"{mode}  amps={result.get('amps', 0.0):.2f}  pwm_scale={result.get('last_pwm_scale', 0.0):.3f}"
                f"{'' if result.get('valid', True) else '  (INVALID)'}"
            )

        self.app.work.submit(do_read, done)


class IoPanel(ttk.Frame):
    """E-stop, two independent DUT-power relay domains -- main (J18) and
    safety (J19), each with its own On/Off buttons and commanded-state
    readback, PROTOCOL.md sec 5.5 -- a spare-pin (J20-style expander pin)
    read/write control, and the fault-line lamp."""

    def __init__(self, parent, app: KilnSimGui) -> None:
        super().__init__(parent)
        self.app = app
        self._busy = False

        estop_row = ttk.LabelFrame(self, text="E-stop")
        estop_row.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(estop_row, text="Open", command=lambda: self._estop("open")).pack(side=tk.LEFT, padx=4)
        ttk.Button(estop_row, text="Close", command=lambda: self._estop("closed")).pack(side=tk.LEFT, padx=4)
        self.estop_state_var = tk.StringVar(value="(unknown)")
        ttk.Label(estop_row, textvariable=self.estop_state_var).pack(side=tk.LEFT, padx=12)

        # Two independent DUT-power relays (PROTOCOL.md sec 5.5, resolved
        # 2026-08-20): main domain (J18) and safety domain (J19) are two
        # separate MCP23017 output bits driving two separate relays, kept
        # deliberately unable to be commanded together -- ganging them would
        # bond GND_Main and GND_Safty through the shared 12V return and
        # defeat the isolation the fixture exists to preserve. The two
        # LabelFrames below are named by domain (never "relay 1/2") and the
        # safety one is styled distinctly (bold, warning-colored label) so a
        # domain mix-up is obvious on screen, not just in the code.
        style = ttk.Style(self)
        try:
            style.configure("Safety.TLabelframe.Label", foreground="#8a2b00", font=("TkDefaultFont", 9, "bold"))
        except tk.TclError:  # pragma: no cover - headless/theme-less test envs
            pass

        power_row = ttk.LabelFrame(self, text="DUT power -- MAIN (J18)")
        power_row.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(power_row, text="On", command=lambda: self._power("main", True)).pack(side=tk.LEFT, padx=4)
        ttk.Button(power_row, text="Off", command=lambda: self._power("main", False)).pack(side=tk.LEFT, padx=4)
        self.power_main_state_var = tk.StringVar(value="(unknown)")
        ttk.Label(power_row, textvariable=self.power_main_state_var).pack(side=tk.LEFT, padx=12)

        power_safety_row = ttk.LabelFrame(
            self, text="DUT power -- SAFETY (J19)", style="Safety.TLabelframe"
        )
        power_safety_row.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(power_safety_row, text="On", command=lambda: self._power("safety", True)).pack(
            side=tk.LEFT, padx=4
        )
        ttk.Button(power_safety_row, text="Off", command=lambda: self._power("safety", False)).pack(
            side=tk.LEFT, padx=4
        )
        self.power_safety_state_var = tk.StringVar(value="(unknown)")
        ttk.Label(
            power_safety_row, textvariable=self.power_safety_state_var, foreground="#8a2b00"
        ).pack(side=tk.LEFT, padx=12)

        # A "spare pin" control, DESIGN_NOTES.md 6.3's "J20" -- the I2C-expander
        # discrete I/O group (IO_SET_DIR/WRITE/READ, PROTOCOL.md sec 5.5)
        # beyond the fixed-role E-stop/DUT-power/relay-sense pins.
        j20_row = ttk.LabelFrame(self, text="J20 / spare expander pin")
        j20_row.pack(fill=tk.X, padx=4, pady=4)
        ttk.Label(j20_row, text="exp:").pack(side=tk.LEFT)
        self.j20_exp_var = tk.IntVar(value=1)
        ttk.Spinbox(j20_row, from_=0, to=1, textvariable=self.j20_exp_var, width=3).pack(side=tk.LEFT)
        ttk.Label(j20_row, text="pin:").pack(side=tk.LEFT, padx=(6, 0))
        self.j20_pin_var = tk.IntVar(value=0)
        ttk.Spinbox(j20_row, from_=0, to=15, textvariable=self.j20_pin_var, width=3).pack(side=tk.LEFT)
        ttk.Button(j20_row, text="Read", command=self._j20_read).pack(side=tk.LEFT, padx=4)
        ttk.Button(j20_row, text="Write high", command=lambda: self._j20_write(True)).pack(side=tk.LEFT, padx=4)
        ttk.Button(j20_row, text="Write low", command=lambda: self._j20_write(False)).pack(side=tk.LEFT, padx=4)
        self.j20_state_var = tk.StringVar(value="(no reading yet)")
        ttk.Label(j20_row, textvariable=self.j20_state_var).pack(side=tk.LEFT, padx=8)

        fault_row = ttk.LabelFrame(self, text="Fault line")
        fault_row.pack(fill=tk.X, padx=4, pady=4)
        self.fault_lamp = tk.Canvas(fault_row, width=24, height=24, highlightthickness=1, highlightbackground="black")
        self.fault_oval = self.fault_lamp.create_oval(2, 2, 22, 22, fill="grey")
        self.fault_lamp.pack(side=tk.LEFT, padx=4)
        ttk.Button(fault_row, text="Refresh now", command=self.poll_live).pack(side=tk.LEFT, padx=4)

    def _estop(self, state: str) -> None:
        def do_send():
            return self.app.link.send_command(CommandGroup.IO, IoCmd.ESTOP_SET, {"open": state == "open"})

        self.app.work.submit(do_send, lambda result, exc: None)

    #: domain name -> its own SET command (PROTOCOL.md sec 5.5). Named by
    #: domain, never by wire command number, and there is deliberately no
    #: "both" entry -- each button click commands exactly one relay.
    _POWER_DOMAIN_SET_CMD = {"main": IoCmd.DUT_POWER_SET, "safety": IoCmd.DUT_POWER_SAFETY_SET}

    def _power(self, domain: str, on: bool) -> None:
        set_cmd = self._POWER_DOMAIN_SET_CMD[domain]

        def do_send():
            return self.app.link.send_command(CommandGroup.IO, set_cmd, {"on": on})

        self.app.work.submit(do_send, lambda result, exc: None)

    def _j20_read(self) -> None:
        exp, pin = self.j20_exp_var.get(), self.j20_pin_var.get()

        def do_read():
            return self.app.link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": exp, "pin": pin})

        def done(result, exc):
            self.j20_state_var.set(f"error: {exc}" if exc is not None else f"level={result.get('level')}")

        self.app.work.submit(do_read, done)

    def _j20_write(self, level: bool) -> None:
        exp, pin = self.j20_exp_var.get(), self.j20_pin_var.get()

        def do_send():
            self.app.link.send_command(
                CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False}
            )
            return self.app.link.send_command(CommandGroup.IO, IoCmd.WRITE, {"exp": exp, "pin": pin, "level": level})

        def done(result, exc):
            self.j20_state_var.set(f"error: {exc}" if exc is not None else f"wrote {'high' if level else 'low'}")

        self.app.work.submit(do_send, done)

    def poll_live(self) -> None:
        if self._busy or not self.app.link.is_connected:
            return
        self._busy = True

        def do_read():
            fault_line = self.app.link.send_command(CommandGroup.IO, IoCmd.FAULT_LINE_GET)
            estop = self.app.link.send_command(CommandGroup.IO, IoCmd.ESTOP_GET)
            power_main = self.app.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)
            power_safety = self.app.link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)
            return fault_line, estop, power_main, power_safety

        def done(result, exc):
            self._busy = False
            if exc is not None:
                return
            fault_line, estop, power_main, power_safety = result
            asserted = bool((fault_line or {}).get("asserted", False))
            self.fault_lamp.itemconfig(self.fault_oval, fill="red" if asserted else "grey")
            self.estop_state_var.set("OPEN (tripped)" if (estop or {}).get("open") else "closed")
            self.power_main_state_var.set("on" if (power_main or {}).get("on") else "off")
            self.power_safety_state_var.set("on" if (power_safety or {}).get("on") else "off")

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
            return self.app.link.send_command(CommandGroup.FAULT, FaultCmd.LIST, {"start_index": 0, "max_count": 8})

        def done(result, exc):
            self.tree.delete(*self.tree.get_children())
            if exc is not None:
                return
            # payloads._fault_decode's LIST reply key is "faults", not
            # "slots" -- the old code's `.get("slots", [])` silently always
            # returned an empty list, so this table never populated.
            for slot in (result or {}).get("faults", []):
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
