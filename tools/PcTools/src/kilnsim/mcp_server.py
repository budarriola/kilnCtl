#!/usr/bin/env python3
"""MCP server (stdio) exposing SimFW's control link as tools.

``firmware/SimFW/docs/DESIGN_NOTES.md`` section 6.1's tool table. A separate MCP
server from ``kilnctrl``'s (DESIGN_NOTES.md sec 6: "since it is a different device
with a different port"), following the same registration pattern
(``mcp.server.mcpserver.MCPServer``, falling back to the pre-2.0
``mcp.server.fastmcp.FastMCP`` -- same decorator/run API either way) so an
agent that already knows one server's shape recognizes the other
immediately.

``run_test_scenario`` is the composite tool: it loads the YAML, compiles its
faults, arms them on the link, drains the EVT stream, evaluates
expectations, and stores the report for a later ``get_test_report`` call
(DESIGN_NOTES.md sec 6.1). Everything else here is a thin 1:1 wrapper over
:class:`~kilnsim.link.SimLink`.

Defaults to :class:`~kilnsim.link.MockSimLink` when no hardware is
connected -- ``sim_connect`` accepts a ``mock`` flag so an agent can drive
the whole tool surface without a fixture attached, matching how the CLI's
``--mock`` and the GUI's default panel wiring work.
"""

from __future__ import annotations

import itertools
import logging
import threading
import time
from typing import Optional

try:
    # mcp >= 2.0 renamed FastMCP to MCPServer (same decorator/run API).
    from mcp.server.mcpserver import MCPServer as _McpServer
except ImportError:  # pragma: no cover - mcp 1.x
    from mcp.server.fastmcp import FastMCP as _McpServer

from .link import MockSimLink, SerialSimLink, SimLink, SimLinkError
from .protocol import CommandGroup, CtCmd, FaultCmd, IoCmd, ModelCmd, RelayCmd, SysCmd, TcCmd
from .report import Report, evaluate_expectations
from .scenario import ScenarioError, compile_faults, load_scenario

log = logging.getLogger(__name__)

mcp = _McpServer("kilnsim")

#: Process-wide link, same one-per-process pattern as kilnctrl.mcp_server's
#: `_link` -- swapped between Serial/Mock by sim_connect(mock=...) rather
#: than being fixed at import time, since which one to use is a per-call
#: decision here (an agent working through this tool surface with no
#: fixture attached still needs every tool to do something sensible).
_link: SimLink = MockSimLink()

_reports_lock = threading.Lock()
_reports: dict = {}
_run_id_counter = itertools.count(1)


def _tool():
    """``mcp.tool()`` registration plus a blanket never-raise guard --
    mirrors kilnctrl.mcp_server._tool(): a bad argument or link failure comes
    back as an ``error: ...`` string, never an exception the transport has
    to render."""
    register = mcp.tool()

    def decorate(fn):
        def wrapper(*args, **kwargs):
            try:
                return fn(*args, **kwargs)
            except SimLinkError as exc:
                return f"error: {exc}"
            except (ValueError, TypeError) as exc:
                return f"error: {exc}"
            except Exception as exc:  # noqa: BLE001 - a tool must always answer
                log.exception("unexpected error in tool %s", fn.__name__)
                return f"error: unexpected {type(exc).__name__}: {exc}"

        wrapper.__name__ = fn.__name__
        wrapper.__doc__ = fn.__doc__
        return register(wrapper)

    return decorate


# ---------------------------------------------------------------------------
# link management
# ---------------------------------------------------------------------------
@_tool()
def sim_connect(port: Optional[str] = None, mock: bool = False) -> str:
    """Connect to SimFW. ``mock=True`` uses an in-memory MockSimLink instead
    of real hardware -- no fixture required. Autodetects by USB VID/PID plus
    a PING round-trip if ``port`` is omitted."""
    global _link
    if _link.is_connected:
        return f"already connected ({'mock' if isinstance(_link, MockSimLink) else 'serial'})"
    _link = MockSimLink() if mock else SerialSimLink()
    opened = _link.connect(port)
    return f"connected: {opened}"


@_tool()
def sim_disconnect() -> str:
    """Disconnect from SimFW."""
    if not _link.is_connected:
        return "not connected"
    _link.disconnect()
    return "disconnected"


@_tool()
def sim_get_state() -> str:
    """Full telemetry snapshot as JSON (DESIGN_NOTES.md sec 5.3's telemetry frame shape)."""
    import json
    state = _link.send_command(CommandGroup.SYS, 100)  # kilnsim-local GET_STATE convenience, see link.py
    return json.dumps(state, indent=2)


@_tool()
def sim_reset(keep_params: bool = False) -> str:
    """Fresh run: model to T0, faults cleared, event seq reset."""
    _link.send_command(CommandGroup.SYS, SysCmd.RESET_SIM, {"keep_params": keep_params})
    return "reset ok"


@_tool()
def sim_load_preset(name: str) -> str:
    """Load a named thermal-model preset (DESIGN_NOTES.md sec 4.3: fast_test/small_kiln/three_zone/stress)."""
    _link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": name})
    return f"preset loaded: {name}"


# ---------------------------------------------------------------------------
# MODEL -- zone parameters
# ---------------------------------------------------------------------------
@_tool()
def sim_set_zone_params(zone: int, params: dict) -> str:
    """Set one zone's thermal-model parameters (DESIGN_NOTES.md sec 4.3's per-zone table)."""
    _link.send_command(CommandGroup.MODEL, ModelCmd.SET_ZONE_PARAMS, {"zone": zone, "params": params})
    return f"zone {zone} params applied"


@_tool()
def sim_get_zone_params(zone: int) -> str:
    """Current parameters for one zone."""
    import json
    reply = _link.send_command(CommandGroup.MODEL, ModelCmd.GET_ZONE_PARAMS, {"zone": zone})
    return json.dumps(reply, indent=2)


@_tool()
def sim_set_timescale(value: float) -> str:
    """Set the simulation time-scale (refused mid-scenario)."""
    _link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": value})
    return f"timescale set: {value}"


@_tool()
def sim_set_seed(value: int) -> str:
    """Set the PRNG seed (refused mid-scenario)."""
    _link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": value})
    return f"seed set: {value}"


# ---------------------------------------------------------------------------
# TC -- MAX31856 emulation
# ---------------------------------------------------------------------------
@_tool()
def tc_get_regs(channel: int) -> str:
    """Register image + shadow truth for one emulated MAX31856 channel
    (DESIGN_NOTES.md sec 5.2: "the pair is what makes 'the DUT was lied to, this is
    the truth' assertions possible")."""
    import json
    reply = _link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": channel})
    return json.dumps(reply, indent=2)


@_tool()
def tc_inject_fault(channel: int, fault_type: str, params: Optional[dict] = None) -> str:
    """Inject a TC fault (DESIGN_NOTES.md sec 7.1's thermocouple/sensor fault catalog). Returns a fault slot id."""
    reply = _link.send_command(
        CommandGroup.TC, TcCmd.INJECT_FAULT,
        {"channel": channel, "fault_type": fault_type, "params": params or {}},
    )
    return f"fault slot: {reply.get('fault_slot', reply)}"


@_tool()
def tc_clear_fault(channel: int, fault_slot: int) -> str:
    """Clear a previously injected TC fault by slot id."""
    _link.send_command(CommandGroup.TC, TcCmd.CLEAR_FAULT, {"channel": channel, "fault_slot": fault_slot})
    return f"channel {channel} fault {fault_slot} cleared"


@_tool()
def tc_set_mode(channel: int, mode: str, manual_temp: Optional[float] = None) -> str:
    """Set channel ``mode`` to "model" or "manual" (DESIGN_NOTES.md sec 5: every
    mutable thing has a mode). ``manual_temp`` is required for "manual"."""
    if mode not in ("model", "manual"):
        raise ValueError("mode must be 'model' or 'manual'")
    if mode == "manual" and manual_temp is None:
        raise ValueError("manual_temp is required when mode='manual'")
    _link.send_command(
        CommandGroup.TC, TcCmd.SET_MODE,
        {"channel": channel, "mode": mode, "manual_temp": manual_temp},
    )
    return f"channel {channel} mode: {mode}"


# ---------------------------------------------------------------------------
# CT -- current transformer emulation
# ---------------------------------------------------------------------------
@_tool()
def ct_set_mode(channel: int, mode: str) -> str:
    """Set CT channel ``mode`` to "model" or "manual"."""
    if mode not in ("model", "manual"):
        raise ValueError("mode must be 'model' or 'manual'")
    _link.send_command(CommandGroup.CT, CtCmd.SET_MODE, {"channel": channel, "mode": mode})
    return f"CT {channel} mode: {mode}"


@_tool()
def ct_set_amps(channel: int, amps: float) -> str:
    """Set a CT channel's commanded amplitude in simulated amps (manual mode)."""
    _link.send_command(CommandGroup.CT, CtCmd.SET_AMPS, {"channel": channel, "amps": amps})
    return f"CT {channel} amps: {amps}"


@_tool()
def ct_set_distortion(channel: int, distortion: dict) -> str:
    """Set CT distortion knobs (dc_offset, clipping, dropout, freq_hz -- DESIGN_NOTES.md sec 3.3)."""
    _link.send_command(CommandGroup.CT, CtCmd.SET_DISTORTION, {"channel": channel, "distortion": distortion})
    return f"CT {channel} distortion set"


# ---------------------------------------------------------------------------
# RELAY
# ---------------------------------------------------------------------------
@_tool()
def relay_get_states() -> str:
    """Current sensed state of every relay (K1/K2/K3/K5/K4)."""
    import json
    reply = _link.send_command(CommandGroup.RELAY, RelayCmd.GET_STATES)
    return json.dumps(reply, indent=2)


@_tool()
def relay_get_edges(since_seq: Optional[int] = None) -> str:
    """Timestamped relay edge log, optionally only edges after ``since_seq``."""
    import json
    reply = _link.send_command(CommandGroup.RELAY, RelayCmd.GET_EDGES, {"since_seq": since_seq})
    return json.dumps(reply, indent=2)


# ---------------------------------------------------------------------------
# discrete I/O, E-stop, DUT power
# ---------------------------------------------------------------------------
@_tool()
def estop_set(state: str) -> str:
    """Open or close the E-stop loop. ``state`` is "open" or "closed"."""
    if state not in ("open", "closed"):
        raise ValueError("state must be 'open' or 'closed'")
    _link.send_command(CommandGroup.IO, IoCmd.ESTOP_SET, {"open": state == "open"})
    return f"e-stop: {state}"


@_tool()
def dut_power_set(state: str) -> str:
    """Switch the DUT's MAIN-domain (J18) 12V power relay. ``state`` is
    "on" or "off". This is the legacy/default relay -- PROTOCOL.md sec 5.5's
    DUT_POWER_SET -- and only ever affects J18; it does not touch the
    safety-domain relay. Use ``dut_power_safety_set`` for J19."""
    if state not in ("on", "off"):
        raise ValueError("state must be 'on' or 'off'")
    _link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": state == "on"})
    return f"dut power (main/J18): {state}"


@_tool()
def dut_power_get() -> str:
    """Read back the MAIN-domain (J18) DUT power relay's commanded state."""
    import json
    reply = _link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)
    return json.dumps(reply, indent=2)


@_tool()
def dut_power_safety_set(state: str) -> str:
    """Switch the DUT's SAFETY-domain (J19) 12V power relay. ``state`` is
    "on" or "off". This is the fixture's second, independent DUT-power relay
    (PROTOCOL.md sec 5.5's DUT_POWER_SAFETY_SET) -- it exists precisely so
    J18 (main) and J19 (safety) can be switched independently without
    bonding GND_Main and GND_Safty through a shared control path. There is
    no combined "power both domains" tool; call ``dut_power_set`` separately
    if the main domain also needs to change."""
    if state not in ("on", "off"):
        raise ValueError("state must be 'on' or 'off'")
    _link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_SET, {"on": state == "on"})
    return f"dut power (safety/J19): {state}"


@_tool()
def dut_power_safety_get() -> str:
    """Read back the SAFETY-domain (J19) DUT power relay's commanded state."""
    import json
    reply = _link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)
    return json.dumps(reply, indent=2)


@_tool()
def io_read(pin: Optional[str] = None) -> str:
    """Read discrete I/O levels, one pin or all if omitted."""
    import json
    reply = _link.send_command(CommandGroup.IO, IoCmd.READ, {"pin": pin})
    return json.dumps(reply, indent=2)


@_tool()
def io_write(pin: str, level: bool) -> str:
    """Drive a discrete I/O pin high or low."""
    _link.send_command(CommandGroup.IO, IoCmd.WRITE, {"pin": pin, "level": level})
    return f"{pin} <- {'high' if level else 'low'}"


@_tool()
def io_set_dir(pin: str, is_input: bool) -> str:
    """Set a discrete I/O pin's direction."""
    _link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"pin": pin, "is_input": is_input})
    return f"{pin} direction: {'input' if is_input else 'output'}"


# ---------------------------------------------------------------------------
# FAULT scheduler (DESIGN_NOTES.md sec 7)
# ---------------------------------------------------------------------------
@_tool()
def fault_schedule(fault_type: str, target: str, trigger: dict,
                    duration: Optional[dict] = None, repeat: Optional[dict] = None,
                    params: Optional[list] = None) -> str:
    """Arm a scheduled fault per DESIGN_NOTES.md sec 7.2's trigger/duration/repeat spec.
    Returns the assigned fault slot id."""
    payload = {
        "fault_type": fault_type,
        "target": target,
        "trigger": trigger,
        "duration": duration or "permanent",
        "repeat": repeat or "once",
        "params": params or [0.0, 0.0, 0.0, 0.0],
    }
    reply = _link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, payload)
    return f"fault slot: {reply.get('fault_slot', reply)}"


@_tool()
def fault_cancel(fault_slot: int) -> str:
    """Cancel an armed/active scheduled fault by slot id."""
    _link.send_command(CommandGroup.FAULT, FaultCmd.CANCEL, {"fault_slot": fault_slot})
    return f"fault {fault_slot} cancelled"


@_tool()
def fault_list() -> str:
    """List all fault slots and their state."""
    import json
    reply = _link.send_command(CommandGroup.FAULT, FaultCmd.LIST)
    return json.dumps(reply, indent=2)


@_tool()
def fault_fire_now(fault_slot: int) -> str:
    """Force an armed fault to fire immediately, bypassing its trigger."""
    _link.send_command(CommandGroup.FAULT, FaultCmd.FIRE_NOW, {"fault_slot": fault_slot})
    return f"fault {fault_slot} fired"


# ---------------------------------------------------------------------------
# scenario runner
# ---------------------------------------------------------------------------
@_tool()
def run_test_scenario(path: str, seed: Optional[int] = None, timescale: Optional[float] = None) -> str:
    """Compile ``path``'s YAML into primitive commands, arm the fault
    schedule, watch the EVT stream, evaluate expectations, and store the
    report. Returns a run id for :func:`get_test_report`.

    This call is synchronous against a MockSimLink (nothing to wait on) and
    against real hardware runs to completion before returning -- DESIGN_NOTES.md sec
    6.1 sketches it as async with progress events; that streaming form is a
    follow-on once a real fixture exists to observe timing against. This
    version already delivers the composite behavior the tool table promises:
    one call, one report.
    """
    try:
        scenario = load_scenario(path)
    except ScenarioError as exc:
        return f"error: could not load scenario: {exc}"

    run_seed = seed if seed is not None else scenario.seed
    run_timescale = timescale if timescale is not None else scenario.timescale

    _link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": run_seed})
    _link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": run_timescale})
    if scenario.preset:
        _link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": scenario.preset})

    for compiled in compile_faults(scenario):
        _link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": compiled.fault_slot,
            "fault_type": compiled.fault_type,
            "target": compiled.target,
            "trigger": compiled.trigger,
            "duration": compiled.duration,
            "repeat": compiled.repeat,
            "params": list(compiled.params),
        })

    events = _link.read_events(timeout=0.0)
    report = evaluate_expectations(scenario, events, seed=run_seed, timescale=run_timescale)

    run_id = next(_run_id_counter)
    with _reports_lock:
        _reports[run_id] = report
    return f"run_id: {run_id}\nverdict: {report.verdict}"


@_tool()
def get_test_report(run_id: int) -> str:
    """Full report JSON for a previous :func:`run_test_scenario` call (DESIGN_NOTES.md sec 8.2)."""
    with _reports_lock:
        report = _reports.get(run_id)
    if report is None:
        return f"error: no report for run_id {run_id}"
    return report.to_json()


# ---------------------------------------------------------------------------
# debug escape hatch
# ---------------------------------------------------------------------------
@_tool()
def sim_raw_command(group: str, cmd: int, hex_payload: str = "") -> str:
    """Send a raw command by group name (SYS/MODEL/TC/CT/RELAY/IO/FAULT) and
    numeric ``cmd``, with a JSON-encoded hex payload -- debug escape hatch
    for anything not covered by a named tool above."""
    import json
    try:
        group_enum = CommandGroup[group.upper()]
    except KeyError:
        return f"error: unknown group {group!r} (expected one of {[g.name for g in CommandGroup]})"
    payload = {}
    if hex_payload:
        try:
            payload = json.loads(bytes.fromhex(hex_payload).decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            return f"error: could not decode hex_payload as JSON: {exc}"
    reply = _link.send_command(group_enum, cmd, payload)
    return json.dumps(reply, indent=2)


def main() -> None:
    logging.basicConfig(level=logging.WARNING)
    mcp.run()


if __name__ == "__main__":
    main()
