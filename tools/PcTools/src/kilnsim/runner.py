"""Real, synchronous scenario runner: connects the primitives
(:mod:`kilnsim.scenario`, :mod:`kilnsim.payloads`/:mod:`kilnsim.link`,
:mod:`kilnsim.report`) into an actual end-to-end run against a live
:class:`~kilnsim.link.SimLink` (real hardware, or ``virtual_simfw`` over
:class:`~kilnsim.link.TcpSimLink`) -- the "layer 2/3" glue PLAN.md sec 13
describes and that, before this module, no scenario had ever exercised.

Two real gaps this module closes that a scenario run needs and nothing
upstream provided:

1. **Waiting.** ``mcp_server.run_test_scenario``/``cli.cmd_run`` both arm the
   fault schedule and then call ``link.read_events(timeout=0.0)``
   immediately -- correct against a :class:`~kilnsim.link.MockSimLink`
   (nothing to wait for), silently wrong against anything that actually
   *simulates* time passing: a fault whose trigger is 20 sim-seconds out has
   not fired yet. :func:`run_scenario` below waits for a real, estimated
   duration (:func:`estimate_run_duration_s`), polling for events as they
   arrive.
2. **Wire-event -> report.py vocabulary translation.** ``report.py``'s
   ``evaluate_expectations`` matches ``expect`` clauses against events whose
   ``payload`` already carries ``{"entity": ..., "state": ...}`` (for
   ``dut:`` clauses) or catalog filter keys like ``relay``/``edge``/
   ``fault_id`` (for ``event:`` clauses) -- see that module's own docstring.
   Nothing before this module ever built those payloads: real wire EVT
   frames only carry raw ``(seq, sim_time_us, event_type, a, b, f0)``
   (PROTOCOL.md sec 6), and DESIGN_NOTES.md 5.3's TELEMETRY frame is the *only*
   source for some observations report.py needs (``fault_line_asserted``,
   ``estop_open``, current-presence) that never appear in the EVT stream at
   all. :func:`_translate_wire_event` and :func:`_poll_telemetry_edges`
   below are that translation.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import TYPE_CHECKING, Optional

from .link import SimLink
from .protocol import CommandGroup, DurationKind, Event, EventType, FaultCmd, ModelCmd, SysCmd
from .report import BLOCKED, FAIL, PASS, ExpectationResult, Report, evaluate_expectations
from .scenario import AtEndExpect, EventThenExpect, ForbidExpect, Scenario, compile_faults

if TYPE_CHECKING:  # pragma: no cover - type-checking only, never imported at runtime
    from .guard_observer import GuardObserver

#: Synthetic (non-wire) event_type used for telemetry-derived entity/state
#: edges (fault_line_asserted, estop_open, current presence -- PLAN.md's EVT
#: catalog, sim_snapshot.h's sim_event_type_t, has no wire event for any of
#: these). Deliberately NOT in kilnsim.protocol.WIRE_EVENT_TYPES, and given
#: its own disjoint seq namespace (see _SYNTHETIC_SEQ_BASE) so it can never
#: be mistaken for wire loss by report.py's gap check.
_SYNTHETIC_EVENT_TYPE = EventType.SIM_CLOCK_MARK
_SYNTHETIC_SEQ_BASE = 1_000_000_000
#: kilnsim.guard_observer.GuardObserver events (GUARD_TRIP/GUARD_WARN/
#: LINK_UP/TRIP_INEFFECTIVE_LATCHED, an entirely separate SaftyFW-derived
#: synthetic source -- see that module's own doc comment) get a THIRD
#: disjoint seq namespace, kilnsim.guard_observer.GUARD_OBSERVER_SEQ_BASE
#: (2,000,000,000): far enough from both the wire's own seq range and
#: _SYNTHETIC_SEQ_BASE above that this function's merge of all three into
#: one `collected` list (see the guard-observer poll in run_scenario())
#: could never mistake one source for another.

_RELAY_ENTITY_BY_BIT = ("K1", "K2", "K3", "K5", "K4")  # sim_snapshot.h's sim_relay_bit_t order

# Reasonable defaults for scenarios that never got explicit numbers to run
# by (a scenario file has no "how long does this run" field, DESIGN_NOTES.md sec
# 8.1's frozen shape) -- see estimate_run_duration_s()'s own doc comment.
_DEFAULT_AT_ZONE_TEMP_ESTIMATE_S = 150.0  # DESIGN_NOTES.md 4.3: fast_test preset's
                                            # "full firing in ~2 min" plus margin
_MIN_RUN_DURATION_S = 45.0
_MAX_RUN_DURATION_S = 600.0
_TRAILING_MARGIN_S = 20.0


def estimate_run_duration_s(scenario: Scenario) -> float:
    """How long (in **sim-clock** seconds) to run before declaring the
    scenario over and evaluating its `expect` clauses. Approximate by
    necessity -- DESIGN_NOTES.md sec 8.1's scenario schema has no explicit "run
    duration" field -- computed from the latest thing in the file that could
    plausibly still need to happen: every fault's trigger time (an
    AT_SIM_TIME trigger's own `t`, or a generous fallback for triggers whose
    firing time depends on the simulation actually reaching a temperature/
    event this estimator cannot predict) plus every `expect` clause's own
    `within_s`/`not_before_s` deadline relative to it, plus a trailing
    margin, clamped to a floor/ceiling so a scenario with nothing in it
    (baseline_firing) still gets a sane wait and a pathological one can't
    hang a CI run forever."""
    latest = 0.0
    for f in scenario.faults:
        t = getattr(f.trigger, "t", None)
        if f.trigger.kind.value == "at_sim_time" and t is not None:
            latest = max(latest, float(t))
        elif f.trigger.kind.value == "manual":
            pass  # fired explicitly by the runner, not time-driven
        else:
            latest = max(latest, _DEFAULT_AT_ZONE_TEMP_ESTIMATE_S)
        if f.duration.kind.value == "for" and f.duration.t is not None:
            latest = max(latest, latest + float(f.duration.t))

    deadline_tail = 0.0
    for e in scenario.expect:
        then = getattr(e, "then", None)
        if isinstance(then, dict):
            within = then.get("within_s")
            if within is not None:
                deadline_tail = max(deadline_tail, float(within))

    total = latest + deadline_tail + _TRAILING_MARGIN_S
    return min(_MAX_RUN_DURATION_S, max(_MIN_RUN_DURATION_S, total))


def _relay_wire_payload(a: int, b: int) -> dict:
    entity = _RELAY_ENTITY_BY_BIT[a] if 0 <= a < len(_RELAY_ENTITY_BY_BIT) else f"relay{a}"
    closed = b != 0
    return {
        "entity": entity,
        "state": not closed,  # canonical convention: state=True means the
                               # "positive word" condition (open), matching
                               # report.py's K4_open/K1_closed polarity split
        "relay": entity,
        "edge": "close" if closed else "open",
    }


def _translate_wire_event(e: Event, slot_to_fault_id: dict) -> Event:
    """Wire EVT -> report.py's expected payload shape, in place (same seq/
    sim_time_us/event_type -- this is annotation, not synthesis, so it stays
    inside WIRE_EVENT_TYPES and participates in gap detection normally)."""
    payload = dict(e.payload)
    if e.event_type == EventType.RELAY_EDGE:
        payload.update(_relay_wire_payload(e.a, e.b))
    elif e.event_type in (EventType.FAULT_FIRED, EventType.FAULT_CLEARED):
        fault_id = slot_to_fault_id.get(e.a)
        if fault_id is not None:
            payload["fault_id"] = fault_id
        payload["slot"] = e.a
    elif e.event_type == EventType.DUT_POWER:
        payload.update({"entity": "dut_power", "state": e.b != 0})
    return Event(seq=e.seq, sim_time_us=e.sim_time_us, event_type=e.event_type, payload=payload, a=e.a, b=e.b, f0=e.f0)


@dataclass
class _TelemetryEdgeTracker:
    """Turns TELEMETRY frame (PROTOCOL.md sec 6) polling into synthetic
    entity/state edge events for observations that never appear in the EVT
    stream at all: `fault_line_asserted`, `estop_open`, and current presence
    (derived from per-zone I_amps > a small epsilon -- "current_absent"/
    "current_present" in scenario `dut:` clauses, e.g.
    welded_ssr_midfire.yaml's `current_decays_after_k4_opens`)."""

    next_seq: int = _SYNTHETIC_SEQ_BASE
    last_fault_line: Optional[bool] = None
    last_estop_open: Optional[bool] = None
    last_current_present: Optional[bool] = None

    def observe(self, telemetry: dict) -> list:
        out = []
        sim_time_us = int(telemetry.get("sim_time_us", 0))

        fault_line = bool(telemetry.get("fault_line_asserted", False))
        if self.last_fault_line is not None and fault_line != self.last_fault_line:
            out.append(self._mk(sim_time_us, "fault_line", fault_line))
        self.last_fault_line = fault_line

        estop_open = bool(telemetry.get("estop_open", False))
        if self.last_estop_open is not None and estop_open != self.last_estop_open:
            out.append(self._mk(sim_time_us, "estop", estop_open))
        self.last_estop_open = estop_open

        current_present = any(abs(float(z.get("i_amps", 0.0))) > 0.05 for z in telemetry.get("zones", []))
        if self.last_current_present is not None and current_present != self.last_current_present:
            out.append(self._mk(sim_time_us, "current", current_present))
        self.last_current_present = current_present

        return out

    def _mk(self, sim_time_us: int, entity: str, state: bool) -> Event:
        seq = self.next_seq
        self.next_seq += 1
        return Event(
            seq=seq, sim_time_us=sim_time_us, event_type=_SYNTHETIC_EVENT_TYPE,
            payload={"entity": entity, "state": state},
        )


def _apply_overrides(link: SimLink, overrides: dict) -> None:
    """``overrides: {"zones[0].R_element": 12.0, ...}`` (DESIGN_NOTES.md sec 8.1) --
    read-modify-write per zone via MODEL GET/SET_ZONE_PARAMS, the same
    pattern cmd_task.c's own SET_TC_LAG handler uses for a single-field
    change (no owner API needs a standalone per-field setter)."""
    import re

    per_zone: dict = {}
    for key, value in (overrides or {}).items():
        m = re.match(r"zones\[(\d+)\]\.(\w+)", str(key))
        if not m:
            continue
        zone = int(m.group(1))
        field = m.group(2)
        per_zone.setdefault(zone, {})[field] = value

    for zone, fields in per_zone.items():
        current = link.send_command(CommandGroup.MODEL, ModelCmd.GET_ZONE_PARAMS, {"zone": zone})
        params = dict(current.get("params", current))
        params.update(fields)
        link.send_command(CommandGroup.MODEL, ModelCmd.SET_ZONE_PARAMS, {"zone": zone, "params": params})


def _trigger_payload(trigger) -> dict:
    d = {"kind": trigger.kind.value}
    if trigger.t is not None:
        d["t"] = trigger.t
    if trigger.zone is not None:
        d["zone"] = trigger.zone
    if trigger.temp_c is not None:
        d["temp_c"] = trigger.temp_c
    if trigger.edge is not None:
        d["edge"] = trigger.edge
    if trigger.relay is not None:
        d["relay"] = trigger.relay
    d["delay_s"] = trigger.delay_s
    if trigger.event_name is not None:
        d["event_name"] = trigger.event_name
    if trigger.t0 is not None:
        d["t0"] = trigger.t0
    if trigger.t1 is not None:
        d["t1"] = trigger.t1
    return d


def _duration_payload(duration) -> dict:
    d = {"kind": duration.kind.value}
    if duration.t is not None:
        d["t"] = duration.t
    return d


def _repeat_payload(repeat) -> dict:
    d = {"kind": repeat.kind.value}
    if repeat.period is not None:
        d["period"] = repeat.period
    d["jitter"] = repeat.jitter
    if repeat.n is not None:
        d["n"] = repeat.n
    return d


def _rescale_guard_event(e: Event, start_wall: float, run_timescale: float) -> Event:
    """kilnsim.guard_observer events arrive stamped with the OBSERVER's own
    wall-clock reading (that module's own doc comment on ``_mk()`` -- SaftyFW's
    guard state is on a separate device/clock from SimFW's sim clock
    entirely), not a SimFW ``sim_time_us``. Approximate a comparable
    ``sim_time_us`` here using the same wall-clock/sim-clock relationship
    this function's own module docstring already assumes for `wall_budget`
    (DESIGN_NOTES.md sec 4.2's "device's own sim-clock/wall-clock
    relationship"): elapsed wall-clock seconds since THIS run started,
    times `run_timescale`. This is a best-effort alignment so a scenario's
    `within_s`/`not_before_s` deadline windows (report.py's
    ``_eval_event_then``) can compare a guard event against a SimFW-side
    cause at all -- it is not a claim the two clocks are synchronized to
    any real precision; a cross-device scenario asserting a tight deadline
    against a guard event should budget generous margin accordingly."""
    wall_s = e.sim_time_us / 1_000_000.0
    approx_sim_time_us = int(max(0.0, wall_s - start_wall) * run_timescale * 1_000_000.0)
    return Event(seq=e.seq, sim_time_us=approx_sim_time_us, event_type=e.event_type,
                 payload=e.payload, a=e.a, b=e.b, f0=e.f0)


#: The four EventType values kilnsim.guard_observer.GuardObserver
#: implementations exist to produce (kilnsim.protocol.EventType's own class
#: comment: "no corresponding sim_event_type_t value ... nothing in SimFW's
#: own EVT wire stream can produce them today"). Used by
#: _references_guard_event() below to recognize an `expect` clause that
#: structurally cannot be evaluated for real without one of these observers
#: attached.
_GUARD_ONLY_EVENT_TYPES = frozenset(
    {EventType.GUARD_TRIP, EventType.GUARD_WARN, EventType.LINK_UP, EventType.TRIP_INEFFECTIVE_LATCHED}
)
_GUARD_ONLY_EVENT_TYPE_NAMES = {t.name.lower() for t in _GUARD_ONLY_EVENT_TYPES}


def _event_spec_type_name(value) -> Optional[str]:
    """``"guard_trip"`` -> ``"guard_trip"``; ``{"type": "guard_trip", "guard":
    "S6a"}`` -> ``"guard_trip"`` -- the same two shapes report.py's own
    ``_resolve_event_clause`` accepts, read here just far enough to name the
    event type without needing report.py's stricter validation (an
    unrecognized/malformed spec is report.py's job to reject at evaluation
    time, not this pre-check's)."""
    if isinstance(value, str):
        return value.lower()
    if isinstance(value, dict):
        type_name = value.get("type")
        return str(type_name).lower() if type_name else None
    return None


def _clause_event_specs(clause) -> list:
    """Every place a guard-shaped ``event:`` filter can appear on one
    ``expect:`` clause -- mirrors report.py's own module-docstring
    enumeration of the dut/event clause-target vocabulary (``event:``/
    ``then.event``/``forbid.event``/``forbid.after.event``/
    ``forbid.before.event``/``at_end.event``)."""
    specs = []
    if isinstance(clause, EventThenExpect):
        specs.append(clause.event)
        then_event = clause.then.get("event") if isinstance(clause.then, dict) else None
        if then_event is not None:
            specs.append(then_event)
    elif isinstance(clause, ForbidExpect):
        forbid = clause.forbid
        if "event" in forbid:
            specs.append(forbid["event"])
        for side in ("after", "before"):
            side_spec = forbid.get(side)
            if isinstance(side_spec, dict) and "event" in side_spec:
                specs.append(side_spec["event"])
    elif isinstance(clause, AtEndExpect):
        if "event" in clause.at_end:
            specs.append(clause.at_end["event"])
    return specs


def _references_guard_event(clause) -> bool:
    return any(_event_spec_type_name(spec) in _GUARD_ONLY_EVENT_TYPE_NAMES
               for spec in _clause_event_specs(clause))


#: Attached verbatim as `blocked_on: {"reason": ..., "phase": "runner"}` on
#: every guard-typed `expect` clause when this run had no `guard_observer`
#: attached -- see `_block_expectations_missing_guard_observer` below.
#: `"phase": "runner"` (not a roadmap milestone name like scenario-authored
#: `blocked_on:` entries use, e.g. welded_ssr_midfire.yaml's template) is
#: deliberate: this gap is closable today, this run, by passing a
#: `guard_observer=` -- it is not tracking missing DUT capability the way a
#: scenario-authored annotation does, so it should never be confused with
#: one when a report reader filters by phase.
_MISSING_GUARD_OBSERVER_REASON = (
    "no guard observation source attached to this run (run_scenario() was called with "
    "guard_observer=None) -- guard state lives on SaftyFW, a second device this scenario "
    "runner never talks to unless a kilnsim.guard_observer.GuardObserver (e.g. "
    "SafetyGuardObserver) is wired to a live SaftyFW link; SimFW's own EVT stream has no "
    "sim_event_type_t value for GUARD_TRIP/GUARD_WARN/LINK_UP/TRIP_INEFFECTIVE_LATCHED at all "
    "(kilnsim.protocol.EventType's own class comment)"
)


def _block_expectations_missing_guard_observer(scenario: Scenario, report: Report) -> Report:
    """Reinterprets every ``expect`` clause that targets a guard-only event
    type (GUARD_TRIP/GUARD_WARN/LINK_UP/TRIP_INEFFECTIVE_LATCHED) as BLOCKED
    when this run had no ``guard_observer`` attached. Such a clause must
    neither silently FAIL (it never had a chance to see the event at all --
    that is not a DUT defect, it is a missing observation source) nor
    silently PASS (a ``forbid:`` clause on an event that structurally can
    never occur this run would otherwise trivially "pass" with zero real
    signal behind it, which is worse than a FAIL: it looks like verification
    that never actually happened). Mirrors report.py's own
    ``_apply_blocked_on`` FAIL/BLOCKED-beats-PASS priority so the overall
    verdict recomputed below stays consistent with what a scenario-authored
    ``blocked_on:`` would do to the same clause.

    Deliberately skips any clause that already carries a scenario-authored
    ``blocked_on:`` (``clause.blocked_on is not None``) -- report.py's own
    ``_apply_blocked_on`` already gave that clause a verdict/reason before
    this function is ever called (see the call site in ``run_scenario``);
    this runner-level block only fills in for guard-typed clauses with no
    scenario-level annotation at all, never overrides one.
    """
    new_results = []
    changed = False
    for clause, result in zip(scenario.expect, report.expectations):
        if clause.blocked_on is None and result.verdict != BLOCKED and _references_guard_event(clause):
            changed = True
            new_results.append(ExpectationResult(
                name=result.name, verdict=BLOCKED, evidence=result.evidence,
                detail=result.detail + f" [BLOCKED: {_MISSING_GUARD_OBSERVER_REASON}]",
                blocked_on={"reason": _MISSING_GUARD_OBSERVER_REASON, "phase": "runner"},
            ))
        else:
            new_results.append(result)
    if not changed:
        return report

    # Recompute overall verdict with report.py's own FAIL > BLOCKED > PASS
    # priority (evaluate_expectations()'s own doc comment on this exact
    # rule) -- duplicated here in three lines rather than calling back into
    # report.py, whose verdict-computation is entangled with the
    # blocked_on pass it already ran (on the scenario-authored annotations
    # only) before this function was ever invoked.
    if not report.validity.valid:
        overall = FAIL
    elif any(r.verdict == FAIL for r in new_results):
        overall = FAIL
    elif any(r.verdict == BLOCKED for r in new_results):
        overall = BLOCKED
    else:
        overall = PASS

    report.expectations = new_results
    report.verdict = overall
    return report


def run_scenario(link: SimLink, scenario: Scenario, *, seed: Optional[int] = None,
                  timescale: Optional[float] = None, duration_s: Optional[float] = None,
                  poll_interval_s: float = 0.5, sleep_fn=time.sleep,
                  guard_observer: Optional["GuardObserver"] = None) -> Report:
    """Arms `scenario` on `link` (already connected) and runs it to
    completion for real: sets seed/timescale, loads the preset + overrides,
    schedules every fault (translating catalog names via
    :mod:`kilnsim.fault_catalog`), waits `duration_s` sim-clock seconds
    (:func:`estimate_run_duration_s` if not given) while draining and
    translating the EVT stream plus polling TELEMETRY for the observations
    the EVT stream can't carry (see module docstring), then evaluates
    `scenario.expect` and returns the :class:`~kilnsim.report.Report`.

    Real time actually elapsed is `duration_s / timescale` (the device's own
    sim-clock/wall-clock relationship, DESIGN_NOTES.md sec 4.2) plus a small fixed
    poll overhead -- against `virtual_simfw` at the scenarios' own
    `timescale: 10`, this keeps a full run in the tens-of-seconds range.

    `guard_observer` (default None): an optional
    :class:`~kilnsim.guard_observer.GuardObserver` (e.g.
    :class:`~kilnsim.guard_observer.SafetyGuardObserver`) polled at the same
    cadence as the TELEMETRY poll for GUARD_TRIP/GUARD_WARN/LINK_UP/
    TRIP_INEFFECTIVE_LATCHED events -- SimFW's own EVT stream cannot produce
    any of these (kilnsim.protocol.EventType's own class comment); they only
    exist if something is watching SaftyFW too. When left None and the
    scenario has any `expect` clause targeting one of those four event
    types, that clause is reinterpreted BLOCKED after evaluation (see
    :func:`_block_expectations_missing_guard_observer`) rather than being
    left to silently FAIL (never had a chance to see the event) or silently
    PASS (a `forbid:` on a structurally-impossible-to-observe event would
    otherwise "pass" with zero real signal behind it).
    """
    run_seed = seed if seed is not None else scenario.seed
    run_timescale = timescale if timescale is not None else scenario.timescale
    run_duration = duration_s if duration_s is not None else estimate_run_duration_s(scenario)

    link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": run_seed})
    link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": run_timescale})
    if scenario.preset:
        link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": scenario.preset})
    _apply_overrides(link, scenario.overrides)

    # Item 1 fix (spi_underrun): baseline captured BEFORE any fault is armed,
    # per this function's own task brief -- the ONLY source for this counter
    # is the TELEMETRY frame's own `spi_underrun_count` field (PROTOCOL.md
    # sec 6, kilnsim.payloads.decode_telemetry_frame(), protocol.py's
    # TelemetryFrame.spi_underrun_count); there is no separate "health
    # command" to poll -- polling get_last_telemetry() at the run loop's own
    # poll_interval_s cadence below IS polling that signal, the same
    # TELEMETRY poll _TelemetryEdgeTracker already rides on.
    get_last_telemetry = getattr(link, "get_last_telemetry", None)
    spi_underrun_baseline: Optional[int] = None
    pre_baseline_events: list = []
    if get_last_telemetry is not None:
        telemetry = get_last_telemetry()
        if telemetry is None:
            # Real link, but no TELEMETRY broadcast has arrived yet this
            # early (right after connect) -- one short grace window, same
            # pattern kilnsim.link.get_state_snapshot() uses for the same
            # "nothing published since connect?" case. read_events() here is
            # a side effect of draining the wire for the TELEMETRY demux
            # (kilnsim.link._FramedSimLink._handle_broadcast()); any EVT
            # events that happen to arrive in this window are preserved into
            # pre_baseline_events rather than silently dropped -- no fault is
            # armed yet at this point, so this should normally be empty, but
            # "should normally be empty" is not "provably always empty."
            pre_baseline_events = link.read_events(timeout=poll_interval_s)
            telemetry = get_last_telemetry()
        if telemetry is not None:
            spi_underrun_baseline = int(telemetry["spi_underrun_count"])
    #: True only once a real spi_underrun_count reading was actually
    #: obtained. False (never silently assumed clean) is the honest state
    #: for MockSimLink (no get_last_telemetry at all) and for a real link
    #: that never produced a single TELEMETRY frame during the whole run --
    #: see report.py's ValidityFlags.spi_underrun_signal_available and this
    #: task's own "do NOT silently fall back to False" instruction.
    spi_underrun_signal_available = spi_underrun_baseline is not None
    spi_underrun_latest = spi_underrun_baseline

    slot_to_fault_id = {compiled.fault_slot: scenario.faults[compiled.fault_slot].id
                         for compiled in compile_faults(scenario)}
    for compiled in compile_faults(scenario):
        # FAULT_SCHEDULE always goes first, PROTOCOL.md sec 5.6: for a
        # PERMANENT/FOR duration it arms the slot directly; for UNTIL_TRIGGER
        # it only parks the fault's fields (fault_type/target/ARM trigger/
        # repeat/params) without arming. send_command() raises SimLinkError
        # if the reply's status byte isn't OK (kilnsim.link.SerialSimLink/
        # kilnsim.payloads.decode_reply), so a bad first frame never reaches
        # the second send below.
        link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
            "fault_slot": compiled.fault_slot,
            "fault_type": compiled.fault_type,
            "target": compiled.target,
            "trigger": _trigger_payload(compiled.trigger),
            "duration": _duration_payload(compiled.duration),
            "repeat": _repeat_payload(compiled.repeat),
            "params": list(compiled.params),
        })
        if compiled.duration.kind == DurationKind.UNTIL_TRIGGER:
            # Frame 2 of the two-frame design: supplies the release trigger
            # and performs the actual arm (fault_sched_schedule() combining
            # it with what frame 1 parked). Must follow frame 1 for this
            # slot_id -- ERR_BAD_ARGS if nothing is pending for it.
            link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
                "fault_slot": compiled.fault_slot,
                "trigger": _trigger_payload(compiled.duration.until),
            })

    tracker = _TelemetryEdgeTracker()
    start_wall = time.time()
    wall_budget = run_duration / max(run_timescale, 1e-6) + 5.0

    # pre_baseline_events were captured before slot_to_fault_id existed (see
    # above) -- translate and seed `collected` with them now so nothing that
    # arrived during the baseline grace window is lost.
    collected: "list[Event]" = [_translate_wire_event(e, slot_to_fault_id) for e in pre_baseline_events]

    last_telemetry_poll = 0.0
    last_guard_poll = 0.0
    while time.time() - start_wall < wall_budget:
        for e in link.read_events(timeout=poll_interval_s):
            collected.append(_translate_wire_event(e, slot_to_fault_id))

        if get_last_telemetry is not None and time.time() - last_telemetry_poll >= poll_interval_s:
            telemetry = get_last_telemetry()
            if telemetry:
                collected.extend(tracker.observe(telemetry))
                spi_underrun_latest = int(telemetry["spi_underrun_count"])
            last_telemetry_poll = time.time()

        if guard_observer is not None and time.time() - last_guard_poll >= poll_interval_s:
            for ge in guard_observer.observe():
                collected.append(_rescale_guard_event(ge, start_wall, run_timescale))
            last_guard_poll = time.time()

    # Drain anything still buffered after the wait loop ends.
    for e in link.read_events(timeout=0.0):
        collected.append(_translate_wire_event(e, slot_to_fault_id))
    if get_last_telemetry is not None:
        # One last read, mirroring the trailing read_events() drain above --
        # catches a final TELEMETRY broadcast that arrived right at the
        # wall_budget boundary, so the spi_underrun comparison below isn't
        # one poll_interval_s stale relative to everything else this run
        # collected.
        telemetry = get_last_telemetry()
        if telemetry is not None:
            spi_underrun_latest = int(telemetry["spi_underrun_count"])
    if guard_observer is not None:
        for ge in guard_observer.observe():
            collected.append(_rescale_guard_event(ge, start_wall, run_timescale))

    if spi_underrun_signal_available:
        spi_underrun = spi_underrun_latest > spi_underrun_baseline
    else:
        # NOT the silently-False bug this fix closes: the unavailability
        # itself is recorded in validity.spi_underrun_signal_available below
        # (report.py's own field, whose doc comment explains why it's kept
        # separate from `valid`), rather than being indistinguishable from
        # "checked, found clean."
        spi_underrun = False

    report = evaluate_expectations(
        scenario, collected, spi_underrun=spi_underrun,
        spi_underrun_signal_available=spi_underrun_signal_available,
        seed=run_seed, timescale=run_timescale, start_time=start_wall,
    )
    if guard_observer is None:
        report = _block_expectations_missing_guard_observer(scenario, report)
    return report
