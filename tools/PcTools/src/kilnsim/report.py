"""Run-report construction and expectation evaluation.

Implements ``firmware/SimFW/docs/DESIGN_NOTES.md`` section 8.2's run-report JSON
shape: scenario name/version/hash, firmware versions, seed, timescale, start
time, full event list, telemetry samples, per-expectation
``{name, PASS|FAIL|SKIPPED|BLOCKED, evidence: [event seqs]}``, overall
verdict, and validity flags (SPI underruns, event-seq gaps => run invalid,
not failed).

BLOCKED (added this pass) is a fourth verdict class for an expectation that
carries a scenario ``blocked_on: {reason, phase}`` annotation (DESIGN_NOTES.md sec
8.1's template, ``firmware/SimFW/scenarios/welded_ssr_midfire.yaml``'s header
comment) and did not pass -- known, tracked DUT incompleteness, distinct
from a genuine FAIL. See the ``BLOCKED`` module constant and
``_apply_blocked_on`` below for the full policy, including the "unexpectedly
passed" stale-annotation case.

:func:`evaluate_expectations` is pure Python logic over a scenario and a
captured event list -- no hardware, no link -- which is exactly why it is the
most valuable piece to get right and unit test hard (see
``tests/test_kilnsim_report.py``).

Event/observation convention
-----------------------------
DESIGN_NOTES.md sec 8.1 says an ``expect`` clause's ``dut:`` field "reference[s] DUT
observations the fixture can see (relay states, fault line, E-stop loop)".
The wire-level shape of those observations is not frozen yet (DESIGN_NOTES.md sec
5.2/12), so this module works against a normalized, already-decoded form:
whatever assembles the event list before handing it to
:func:`evaluate_expectations` (eventually ``run_test_scenario`` in
``mcp_server.py``) is responsible for turning wire-level relay-edge/IO
frames into :class:`~kilnsim.protocol.Event` objects whose ``payload`` carries

    {"entity": "<name>", "state": <bool>}

for anything observable as a named boolean -- e.g. ``{"entity": "K4",
"state": True}`` for "K4 is now open". A scenario's ``dut: K4_open`` names
the *entity* (``K4``) and the *desired polarity* (``open`` -> ``True``) via
:func:`_parse_dut_flag`; recognized positive/negative words are open/on/
asserted/true and closed/off/deasserted/false respectively.

Fault lifecycle events (``fault_fired``/``fault_cleared`` in an ``event:``
clause) are matched against :data:`~kilnsim.protocol.EventType.FAULT_FIRED` /
``FAULT_CLEARED`` events whose payload carries ``{"fault_id": "<scenario
fault id>"}`` (the scenario YAML's own ``faults[].id``, not the compiled
numeric slot -- see ``scenario.compile_faults``).
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from typing import Any, Optional

from .protocol import Event, EventType, WIRE_EVENT_TYPES
from .scenario import EventThenExpect, ForbidExpect, AtEndExpect, Scenario

#: Verdict strings, matching DESIGN_NOTES.md sec 8.2's "PASS|FAIL|SKIPPED", plus
#: BLOCKED (added this pass -- see the "blocked_on" section below).
PASS = "PASS"
FAIL = "FAIL"
SKIPPED = "SKIPPED"
#: An expectation carrying a scenario ``blocked_on:`` annotation (welded_ssr_
#: midfire.yaml's template comment, kilnsim.scenario's ``_parse_blocked_on``)
#: that did NOT pass -- distinct from FAIL. `blocked_on:` marks expectations
#: that cannot pass against TODAY's DUT by design of the roadmap (a guard
#: input the DUT structurally never populates yet, a relay path with zero
#: callers, etc.), not a defect in the guard, the fixture, or the scenario.
#: Reporting these as FAIL would make the suite look broken when it is
#: actually blocked on known, tracked, upstream incompleteness; reporting
#: them as SKIPPED would lose the reason/phase information and conflate them
#: with "the triggering condition never arose" (a genuinely different
#: situation -- SKIPPED already means that). BLOCKED is its own verdict so a
#: report reader (and report.py's own overall-verdict logic below) can tell
#: all three apart at a glance.
BLOCKED = "BLOCKED"

_POSITIVE_WORDS = {"open", "on", "asserted", "true", "present"}
_NEGATIVE_WORDS = {"closed", "close", "off", "deasserted", "false", "absent"}


class ReportError(ValueError):
    pass


@dataclass
class ExpectationResult:
    name: str
    verdict: str  # PASS | FAIL | SKIPPED | BLOCKED
    evidence: list = field(default_factory=list)  # event seqs
    detail: str = ""
    #: Carried through verbatim from the scenario's own ``blocked_on:``
    #: mapping ({"reason": ..., "phase": ...}) whenever the clause has one,
    #: regardless of which verdict it ended up with -- present even on a
    #: PASS (see ``stale_annotation`` below), so a report reader always has
    #: the full picture for an annotated clause.
    blocked_on: Optional[dict] = None
    #: True only for the "unexpectedly passed" case: the clause carries a
    #: ``blocked_on:`` annotation but evaluated to PASS anyway -- a real,
    #: valuable signal that the DUT gained a capability the annotation
    #: assumed it didn't have. Verdict stays PASS (it genuinely did pass);
    #: this flag is what a report consumer checks to notice the annotation
    #: is now stale and should be removed from the scenario file.
    stale_annotation: bool = False

    def to_dict(self) -> dict:
        d = {
            "name": self.name,
            "verdict": self.verdict,
            "evidence": list(self.evidence),
            "detail": self.detail,
        }
        if self.blocked_on is not None:
            d["blocked_on"] = dict(self.blocked_on)
        if self.stale_annotation:
            d["stale_annotation"] = True
        return d


@dataclass
class ValidityFlags:
    """DESIGN_NOTES.md sec 8.2: "validity flags (SPI underruns, event-seq gaps =>
    run invalid, not failed)"."""

    spi_underrun: bool = False
    event_seq_gap: bool = False
    #: False only when this run's link could not actually serve a real
    #: spi_underrun_count reading at all -- kilnsim.runner.run_scenario()'s
    #: own doc comment on why `spi_underrun` must never silently default to
    #: False when that happens (MockSimLink has no TELEMETRY frame at all;
    #: a real link on which no TELEMETRY broadcast ever arrived during the
    #: run is the same situation). Default True preserves every existing
    #: caller's meaning: a hand-built Report or an evaluate_expectations()
    #: call that never mentions this parameter is asserting "the signal was
    #: actually observed" -- the same thing `spi_underrun=False` on its own
    #: meant before this field existed. Deliberately NOT folded into
    #: `valid` below: an unavailable signal makes a `forbid: spi_underrun`
    #: expectation unable to certify anything either way, which is a
    #: visibility gap to report, not by itself a reason to redden the whole
    #: run's validity (that would conflate "we didn't check" with "we
    #: checked and it's bad", the same distinction BLOCKED vs FAIL exists
    #: to preserve for `expect` clauses).
    spi_underrun_signal_available: bool = True

    @property
    def valid(self) -> bool:
        return not (self.spi_underrun or self.event_seq_gap)

    def to_dict(self) -> dict:
        return {
            "spi_underrun": self.spi_underrun,
            "event_seq_gap": self.event_seq_gap,
            "spi_underrun_signal_available": self.spi_underrun_signal_available,
            "valid": self.valid,
        }


@dataclass
class Report:
    scenario_name: str
    scenario_version: int
    scenario_hash: str
    seed: int
    timescale: float
    start_time: float
    events: list  # list[Event]
    telemetry_samples: list  # list[dict] (protocol.TelemetryFrame-shaped, kept loose here)
    expectations: list  # list[ExpectationResult]
    validity: ValidityFlags
    fw_versions: dict = field(default_factory=dict)
    verdict: str = SKIPPED  # overall

    def to_dict(self) -> dict:
        return {
            "scenario": {
                "name": self.scenario_name,
                "version": self.scenario_version,
                "hash": self.scenario_hash,
            },
            "fw_versions": dict(self.fw_versions),
            "seed": self.seed,
            "timescale": self.timescale,
            "start_time": self.start_time,
            "events": [e.to_dict() for e in self.events],
            "telemetry_samples": list(self.telemetry_samples),
            "expectations": [e.to_dict() for e in self.expectations],
            "verdict": self.verdict,
            "validity": self.validity.to_dict(),
        }

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), indent=indent)

    @property
    def passed(self) -> bool:
        return self.verdict == PASS

    @property
    def blocked(self) -> bool:
        """True for the overall-BLOCKED verdict (see ``evaluate_expectations``'s
        overall-verdict rule): every non-PASS expectation in this run is a
        documented ``blocked_on:`` case and none is a genuine FAIL. Distinct
        from ``passed`` -- a BLOCKED run is not a clean PASS -- but also not
        a failure a CI gate should redden for; see cli.py's ``cmd_run`` for
        the exit-code consequence."""
        return self.verdict == BLOCKED


# ---------------------------------------------------------------------------
# clause-target resolution
#
# An ``event:``/``then:``/``forbid:``/``at_end:`` clause targets one of two
# things -- see the module docstring:
#
#   {"dut": "K4_open"}                       -- entity/polarity from the name
#   {"dut": "safety_temp_valid", "equals": false}  -- explicit polarity
#   {"event": "trip_ineffective_latched"}    -- bare event-type name
#   {"event": {"type": "guard_trip", "guard": "S6a"}}  -- type + payload filters
#
# firmware/SimFW/scenarios/*.yaml (written in parallel against this same
# DESIGN_NOTES.md sec 8.1 schema) use every one of these forms, so this module
# resolves them generically rather than assuming the single ``dut:
# <entity>_<state>`` shorthand DESIGN_NOTES.md sec 8.1's own inline example shows.
# ---------------------------------------------------------------------------
def _parse_dut_flag(flag: str) -> tuple:
    """``"K4_open"`` -> ``("K4", True)``. Raises :class:`ReportError` if the
    trailing word isn't a recognized polarity."""
    if "_" not in flag:
        raise ReportError(f"dut flag {flag!r} must be '<entity>_<state>' (e.g. 'K4_open')")
    entity, state = flag.rsplit("_", 1)
    word = state.lower()
    if word in _POSITIVE_WORDS:
        return entity, True
    if word in _NEGATIVE_WORDS:
        return entity, False
    raise ReportError(
        f"dut flag {flag!r}: unrecognized state word {state!r} "
        f"(expected one of {sorted(_POSITIVE_WORDS | _NEGATIVE_WORDS)})"
    )


def _resolve_dut_target(clause: dict) -> tuple:
    """``{"dut": "K4_open"}`` -> ``("K4", True)``; ``{"dut": "x", "equals":
    false}`` -> ``("x", False)`` (explicit polarity, no suffix parsing)."""
    dut_val = clause["dut"]
    if "equals" in clause:
        return str(dut_val), bool(clause["equals"])
    return _parse_dut_flag(str(dut_val))


def _resolve_event_clause(value) -> tuple:
    """``"trip_ineffective_latched"`` -> ``("trip_ineffective_latched", {})``;
    ``{"type": "guard_trip", "guard": "S6a"}`` -> ``("guard_trip", {"guard":
    "S6a"})``."""
    if isinstance(value, str):
        return value, {}
    if isinstance(value, dict):
        type_name = value.get("type")
        if not type_name:
            raise ReportError(f"event clause missing 'type': {value!r}")
        filters = {k: v for k, v in value.items() if k != "type"}
        return type_name, filters
    raise ReportError(f"event clause must be a string or mapping, got {value!r}")


def _target_kind(clause: dict) -> str:
    """Which of "dut"/"event" a then/forbid/at_end clause targets."""
    if "dut" in clause:
        return "dut"
    if "event" in clause:
        return "event"
    raise ReportError(f"clause needs 'dut' or 'event': {clause!r}")


def _event_type_from_name(type_name: str) -> EventType:
    try:
        return EventType[type_name.upper()]
    except KeyError as exc:
        raise ReportError(f"unknown event type in expect clause: {type_name!r}") from exc


def _entity_state_events(events: list, entity: str) -> list:
    """Events observing ``entity``'s boolean state, in seq order (input is
    assumed already time/seq ordered -- the event ring's own guarantee,
    DESIGN_NOTES.md sec 4.5)."""
    out = []
    for e in events:
        payload = e.payload or {}
        if payload.get("entity") == entity and "state" in payload:
            out.append(e)
    return out


def _find_events(events: list, type_name: str, filters: dict) -> list:
    """Events of ``type_name`` whose payload matches every key/value in
    ``filters`` (``slot`` is a filter alias for the payload's ``fault_id``,
    matching FAULT_FIRED/FAULT_CLEARED's convention -- see the module
    docstring; every other filter key is matched literally against the
    payload, e.g. ``guard``/``relay``/``edge``/``state``)."""
    event_type = _event_type_from_name(type_name)
    out = []
    for e in events:
        if e.event_type is not event_type:
            continue
        payload = e.payload or {}
        ok = True
        for k, v in filters.items():
            actual = payload.get("fault_id") if k == "slot" else payload.get(k)
            if actual != v:
                ok = False
                break
        if ok:
            out.append(e)
    return out


def _us_to_s(sim_time_us: int) -> float:
    return sim_time_us / 1_000_000.0


# ---------------------------------------------------------------------------
# per-clause evaluation
# ---------------------------------------------------------------------------
def _eval_event_then(clause: EventThenExpect, events: list) -> ExpectationResult:
    clause_type, cause_filters = _resolve_event_clause(clause.event)
    causes = _find_events(events, clause_type, cause_filters)
    if not causes:
        return ExpectationResult(
            name=clause.name, verdict=SKIPPED,
            detail=f"triggering event ({clause_type}{f' {cause_filters}' if cause_filters else ''}) never occurred",
        )
    cause = causes[0]

    then = clause.then
    within_s = then.get("within_s")
    not_before_s = then.get("not_before_s", 0.0)
    earliest_us = cause.sim_time_us + int(not_before_s * 1_000_000)
    deadline_us = cause.sim_time_us + int(within_s * 1_000_000) if within_s is not None else None

    kind = _target_kind(then)
    if kind == "dut":
        entity, want = _resolve_dut_target(then)
        target_desc = f"dut:{then['dut']}"
        candidates = [
            e for e in _entity_state_events(events, entity)
            if bool(e.payload.get("state")) == want
        ]
    else:
        type_name, filters = _resolve_event_clause(then["event"])
        target_desc = f"event:{type_name}"
        candidates = _find_events(events, type_name, filters)

    for e in candidates:
        if e.sim_time_us < earliest_us:
            continue
        if deadline_us is not None and e.sim_time_us > deadline_us:
            break
        return ExpectationResult(
            name=clause.name, verdict=PASS,
            evidence=[cause.seq, e.seq],
            detail=f"{target_desc} observed {_us_to_s(e.sim_time_us) - _us_to_s(cause.sim_time_us):.3f}s "
                   f"after {clause_type}",
        )
    return ExpectationResult(
        name=clause.name, verdict=FAIL, evidence=[cause.seq],
        detail=f"{target_desc} not observed"
               + (f" within {within_s}s" if within_s is not None else "")
               + f" after {clause_type} (seq {cause.seq})",
    )


def _generic_boundary(spec: dict, events: list) -> tuple:
    """Returns ``(boundary_us, boundary_seq, skip_detail)``. ``skip_detail``
    is set (and the other two ``None``) when the boundary event never
    occurred, meaning the clause can't be evaluated. Shared by
    ``forbid.before`` and ``forbid.after`` (see :func:`_eval_forbid`) -- both
    are "a point in sim time," just anchoring the forbidden interval from
    opposite ends, so they accept the same spec vocabulary.

    ``"event"`` (added alongside ``after:`` -- see ``_eval_forbid``'s
    docstring) takes the same ``{type: ..., ...filters}``/bare-string shape
    :func:`_resolve_event_clause` already parses for ``event:``/``then:``
    clauses, e.g. ``{"event": {"type": "relay_edge", "relay": "K4", "edge":
    "open"}}`` -- anything in the event stream, not just ``fault_fired``
    (which ``"fault"`` below remains a convenience alias for)."""
    if spec.get("sim_end"):
        return float("inf"), None, None
    if "sim_time_s" in spec:
        return float(spec["sim_time_s"]) * 1_000_000, None, None
    if "sim_time" in spec:
        return float(spec["sim_time"]) * 1_000_000, None, None
    if "fault" in spec:
        fired = _find_events(events, "fault_fired", {"slot": spec["fault"]})
        if not fired:
            return None, None, f"boundary fault {spec['fault']!r} never fired"
        return fired[0].sim_time_us, fired[0].seq, None
    if "event" in spec:
        type_name, filters = _resolve_event_clause(spec["event"])
        matches = _find_events(events, type_name, filters)
        if not matches:
            return None, None, (
                f"boundary event ({type_name}{f' {filters}' if filters else ''}) never occurred"
            )
        return matches[0].sim_time_us, matches[0].seq, None
    raise ReportError(f"boundary needs 'fault', 'event', 'sim_time_s', or 'sim_end': {spec!r}")


def _eval_forbid(clause: ForbidExpect, events: list) -> ExpectationResult:
    """``forbid:`` checks that its target (a ``dut:`` entity/polarity or an
    ``event:``) is never observed inside ``[after, before)`` of sim time.

    ``before:`` alone (the original, still the common case -- "never before
    this boundary") defaults ``after`` to sim-time 0, unchanged from before
    this function grew an ``after:`` side.

    ``after:`` (new) anchors the *other* end: "never forbidden from this
    boundary onward" -- defaults ``before`` to sim_end. This is what lets a
    scenario assert a *persisting* level rather than only a bounded-before
    one: a condition that is already true at some cause and must not stop
    produces no new "it started" edge for a ``then:`` clause to match (see
    the module docstring's target-kind block and
    ``virtual_dut/README.md``'s Finding 2 corollary), but its *negation*
    stopping is exactly an edge, and "that edge must never occur after the
    cause" is a plain ``forbid`` once ``after:`` exists. E.g. "current must
    not stop after K4 opens" is ``forbid: {dut: current_absent, after:
    {event: {type: relay_edge, relay: K4, edge: open}}}`` -- no ``before:``
    needed, so the forbidden interval runs to sim_end.

    At least one of ``after:``/``before:`` must be given.
    """
    forbid = clause.forbid
    after_spec = forbid.get("after")
    before_spec = forbid.get("before")
    if after_spec is None and before_spec is None:
        raise ReportError(f"forbid clause {clause.name!r} needs 'after' and/or 'before'")

    if after_spec is None:
        after_us, after_seq, skip_detail = 0.0, None, None
    else:
        after_us, after_seq, skip_detail = _generic_boundary(after_spec, events)
    if skip_detail:
        return ExpectationResult(name=clause.name, verdict=SKIPPED, detail=skip_detail)

    if before_spec is None:
        before_us, before_seq, skip_detail = float("inf"), None, None
    else:
        before_us, before_seq, skip_detail = _generic_boundary(before_spec, events)
    if skip_detail:
        return ExpectationResult(name=clause.name, verdict=SKIPPED, detail=skip_detail)

    kind = _target_kind(forbid)
    if kind == "dut":
        entity, want = _resolve_dut_target(forbid)
        target_desc = f"dut:{forbid['dut']}"
        candidates = [
            e for e in _entity_state_events(events, entity)
            if bool(e.payload.get("state")) == want
        ]
    else:
        type_name, filters = _resolve_event_clause(forbid["event"])
        target_desc = f"event:{type_name}"
        candidates = _find_events(events, type_name, filters)

    violations = [e for e in candidates if after_us <= e.sim_time_us < before_us]
    boundary_evidence = [s for s in (after_seq, before_seq) if s is not None]
    if violations:
        return ExpectationResult(
            name=clause.name, verdict=FAIL,
            evidence=[e.seq for e in violations],
            detail=f"{target_desc} observed inside the forbidden window (first at seq {violations[0].seq})",
        )
    return ExpectationResult(
        name=clause.name, verdict=PASS,
        evidence=boundary_evidence,
        detail=f"{target_desc} never observed inside the forbidden window",
    )


def _eval_at_end(clause: AtEndExpect, events: list) -> ExpectationResult:
    at_end = clause.at_end
    kind = _target_kind(at_end)
    if kind == "dut":
        entity, want = _resolve_dut_target(at_end)
        target_desc = f"dut:{at_end['dut']}"
        observed = _entity_state_events(events, entity)
        if not observed:
            return ExpectationResult(
                name=clause.name, verdict=FAIL,
                detail=f"{entity} state never observed; cannot be {target_desc} at end",
            )
        last = observed[-1]
        if bool(last.payload.get("state")) == want:
            return ExpectationResult(
                name=clause.name, verdict=PASS, evidence=[last.seq],
                detail=f"{target_desc} held at end of run (last observed seq {last.seq})",
            )
        return ExpectationResult(
            name=clause.name, verdict=FAIL, evidence=[last.seq],
            detail=f"{entity} did not end in the expected state "
                   f"(last observed seq {last.seq}, state={last.payload.get('state')})",
        )

    # event target: "still true at the end" is read as "occurred at least
    # once" -- these are latched conditions (e.g. TRIP_INEFFECTIVE_LATCHED),
    # not events that could sensibly un-occur by end of run.
    type_name, filters = _resolve_event_clause(at_end["event"])
    matches = _find_events(events, type_name, filters)
    if matches:
        return ExpectationResult(
            name=clause.name, verdict=PASS, evidence=[matches[-1].seq],
            detail=f"event:{type_name} occurred (last at seq {matches[-1].seq})",
        )
    return ExpectationResult(
        name=clause.name, verdict=FAIL,
        detail=f"event:{type_name} never occurred",
    )


# ---------------------------------------------------------------------------
# validity
# ---------------------------------------------------------------------------
def _check_event_seq_gap(events: list) -> bool:
    # Only real wire EVT frames carry a meaningful, gap-detectable sequence
    # number (PROTOCOL.md sec 6's `seq`, "sequence-numbered so the PC
    # detects loss"). A scenario runner may also inject *synthetic* events
    # for observations the wire never frames as an EVT (e.g. fault_line_
    # asserted/estop_open telemetry-diff edges, "current_present" derived
    # from I_amps -- see kilnsim.runner) -- those get their own seq
    # namespace (deliberately disjoint from the wire's, so they can never be
    # mistaken for a wire loss) and must be excluded here, or a run with any
    # synthetic events at all would spuriously fail validity.
    seqs = sorted(e.seq for e in events if e.event_type in WIRE_EVENT_TYPES)
    for a, b in zip(seqs, seqs[1:]):
        if b != a + 1:
            return True
    return False


# ---------------------------------------------------------------------------
# blocked_on post-processing
# ---------------------------------------------------------------------------
def _apply_blocked_on(clause, result: ExpectationResult) -> ExpectationResult:
    """Reinterprets a clause's raw PASS/FAIL/SKIPPED ``result`` in light of
    its scenario ``blocked_on:`` annotation (if any) -- see the ``BLOCKED``
    constant's own doc comment above for the reasoning; this is where that
    policy is actually applied. Called once per clause, after the normal
    ``_eval_event_then``/``_eval_forbid``/``_eval_at_end`` evaluation, so the
    per-clause evaluators themselves stay ignorant of ``blocked_on:``
    entirely (DESIGN_NOTES.md 8.1's `_parse_expect` already tolerated the key
    without reading it; this is report.py's own new consumer).

    - No ``blocked_on:`` on the clause: ``result`` is returned unchanged.
    - ``blocked_on:`` present and the raw verdict is FAIL or SKIPPED (the
      documented DUT gap kept the expectation from passing, whether that
      showed up as an outright FAIL or as its triggering condition never
      arising): verdict becomes BLOCKED, carrying ``reason``/``phase``.
    - ``blocked_on:`` present and the raw verdict is PASS (the DUT gained
      the capability the annotation assumed it lacked): verdict STAYS PASS
      -- it genuinely passed -- but ``stale_annotation`` is set and the
      ``blocked_on`` reason/phase are still attached, so whoever owns that
      roadmap phase can find out their annotation needs removing instead of
      the signal being silently swallowed (task brief's explicit ask).
    """
    if clause.blocked_on is None:
        return result
    if result.verdict == PASS:
        return ExpectationResult(
            name=result.name, verdict=PASS, evidence=result.evidence,
            detail=result.detail + " [blocked_on annotation is STALE: this expectation now passes -- "
                                    "the DUT appears to have gained the capability the annotation assumed "
                                    "it lacked; consider removing blocked_on from the scenario]",
            blocked_on=clause.blocked_on, stale_annotation=True,
        )
    return ExpectationResult(
        name=result.name, verdict=BLOCKED, evidence=result.evidence,
        detail=result.detail + f" [BLOCKED: {clause.blocked_on.get('reason', 'no reason given')}]",
        blocked_on=clause.blocked_on,
    )


# ---------------------------------------------------------------------------
# top-level API
# ---------------------------------------------------------------------------
def evaluate_expectations(scenario: Scenario, events: list,
                           telemetry_samples: Optional[list] = None,
                           spi_underrun: bool = False,
                           spi_underrun_signal_available: bool = True,
                           seed: Optional[int] = None,
                           timescale: Optional[float] = None,
                           start_time: Optional[float] = None,
                           fw_versions: Optional[dict] = None) -> Report:
    """Check ``scenario.expect`` against a captured, seq-ordered ``events``
    list and build the run :class:`Report` (DESIGN_NOTES.md sec 8.2).

    Overall verdict is PASS only if every expectation is PASS or SKIPPED and
    the validity flags are clean (a SKIPPED expectation does not fail the
    run -- it means its triggering condition never arose -- so a run with
    any SKIPPED expectation and no FAILs is reported PASS with those SKIPPED
    entries visible, matching "SKIPPED" being a first-class verdict alongside
    PASS/FAIL in DESIGN_NOTES.md sec 8.2 rather than a synonym for FAIL).

    A clause carrying a scenario ``blocked_on:`` annotation (see the
    ``BLOCKED`` module constant's own doc comment) that does not pass is
    reported BLOCKED instead of FAIL/SKIPPED, and the overall verdict
    follows suit: FAIL beats BLOCKED beats PASS. A run with a genuine FAIL
    anywhere is still FAIL regardless of any BLOCKED entries; a run with no
    FAIL but at least one BLOCKED is reported BLOCKED overall -- not PASS
    (something genuinely didn't pass), not FAIL (it is known, tracked DUT
    incompleteness, not a surprise regression). A ``blocked_on:``-annotated
    clause that unexpectedly PASSes stays PASS (see ``_apply_blocked_on``)
    and does not by itself prevent an unqualified overall PASS.
    """
    events = sorted(events, key=lambda e: e.seq)
    results = []
    for clause in scenario.expect:
        if isinstance(clause, EventThenExpect):
            raw = _eval_event_then(clause, events)
        elif isinstance(clause, ForbidExpect):
            raw = _eval_forbid(clause, events)
        elif isinstance(clause, AtEndExpect):
            raw = _eval_at_end(clause, events)
        else:  # pragma: no cover - scenario.py only produces the three above
            raise ReportError(f"unhandled expect clause type: {type(clause)}")
        results.append(_apply_blocked_on(clause, raw))

    validity = ValidityFlags(
        spi_underrun=spi_underrun,
        event_seq_gap=_check_event_seq_gap(events),
        spi_underrun_signal_available=spi_underrun_signal_available,
    )

    # Overall verdict (DESIGN_NOTES.md sec 8.2, extended this pass for BLOCKED):
    #   FAIL    -- the run is invalid, or at least one expectation is a
    #              genuine FAIL (a blocked_on-annotated clause that didn't
    #              pass is BLOCKED, not FAIL, so it never lands here).
    #   BLOCKED -- no FAIL and no invalidity, but at least one expectation
    #              is BLOCKED. Deliberately its own overall verdict, neither
    #              PASS nor FAIL: a run whose only non-passes are documented,
    #              tracked DUT incompleteness should not redden a CI gate
    #              (that would just be re-reporting a known, already-tracked
    #              gap as a surprise failure) but also must not read as an
    #              unqualified clean PASS (something genuinely did not pass)
    #              -- see cli.py's cmd_run for the resulting exit-code choice.
    #   PASS    -- everything else (every expectation PASS or SKIPPED, run
    #              valid) -- unchanged from the pre-BLOCKED behavior.
    if not validity.valid:
        overall = FAIL  # an invalid run cannot be certified PASS, DESIGN_NOTES.md sec 8.2
    elif any(r.verdict == FAIL for r in results):
        overall = FAIL
    elif any(r.verdict == BLOCKED for r in results):
        overall = BLOCKED
    else:
        overall = PASS

    return Report(
        scenario_name=scenario.name,
        scenario_version=scenario.version,
        scenario_hash=scenario.content_hash(),
        seed=seed if seed is not None else scenario.seed,
        timescale=timescale if timescale is not None else scenario.timescale,
        start_time=start_time if start_time is not None else time.time(),
        events=list(events),
        telemetry_samples=list(telemetry_samples or []),
        expectations=results,
        validity=validity,
        fw_versions=dict(fw_versions or {}),
        verdict=overall,
    )


def report_from_dict(data: dict) -> Report:
    """Inverse of :meth:`Report.to_dict`, for ``get_test_report``-style
    round trips (MCP tool returning a previously saved report)."""
    scenario = data.get("scenario", {})
    return Report(
        scenario_name=scenario.get("name", ""),
        scenario_version=int(scenario.get("version", 0)),
        scenario_hash=scenario.get("hash", ""),
        seed=int(data.get("seed", 0)),
        timescale=float(data.get("timescale", 1.0)),
        start_time=float(data.get("start_time", 0.0)),
        events=[Event.from_dict(e) for e in data.get("events", [])],
        telemetry_samples=list(data.get("telemetry_samples", [])),
        expectations=[
            ExpectationResult(
                name=e["name"], verdict=e["verdict"],
                evidence=list(e.get("evidence", [])), detail=e.get("detail", ""),
                blocked_on=dict(e["blocked_on"]) if e.get("blocked_on") is not None else None,
                stale_annotation=bool(e.get("stale_annotation", False)),
            )
            for e in data.get("expectations", [])
        ],
        validity=ValidityFlags(
            spi_underrun=bool((data.get("validity") or {}).get("spi_underrun", False)),
            event_seq_gap=bool((data.get("validity") or {}).get("event_seq_gap", False)),
            spi_underrun_signal_available=bool(
                (data.get("validity") or {}).get("spi_underrun_signal_available", True)
            ),
        ),
        fw_versions=dict(data.get("fw_versions", {})),
        verdict=data.get("verdict", SKIPPED),
    )
