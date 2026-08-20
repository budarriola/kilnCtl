"""YAML scenario loader/validator + fault-schedule compiler.

Implements ``firmware/SimFW/docs/PLAN.md`` section 8.1's scenario file shape
exactly (name, version, exercises, preset, timescale, seed, overrides,
dut, faults[], expect[] with event/then, forbid, at_end forms, report_keep,
optional manual_checks).

A separate agent is writing the actual ``firmware/SimFW/scenarios/*.yaml``
files against this same section 8.1 schema, in parallel and without needing
to be waited on: this module is written directly against the spec text, and
:func:`load_scenario` is exercised against any such files that already exist
as an opportunistic smoke test (see ``tests/test_kilnsim_scenario.py``), not
as a dependency.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional, Union

import yaml

from .protocol import (
    Duration,
    DurationKind,
    FaultScheduleCommand,
    Repeat,
    RepeatKind,
    Trigger,
    TriggerKind,
)


class ScenarioError(ValueError):
    """Raised for a malformed/invalid scenario file."""


# ---------------------------------------------------------------------------
# expect clause shapes (PLAN.md sec 8.1 example: event/then, forbid, at_end)
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class EventThenExpect:
    """``expect: - name: ... event: {...} then: {dut: ..., within_s: ...}``"""

    name: str
    event: dict
    then: dict


@dataclass(frozen=True)
class ForbidExpect:
    """``expect: - name: ... forbid: {dut: ..., before: {...}}``"""

    name: str
    forbid: dict


@dataclass(frozen=True)
class AtEndExpect:
    """``expect: - name: ... at_end: {dut: ...}``"""

    name: str
    at_end: dict


ExpectClause = Union[EventThenExpect, ForbidExpect, AtEndExpect]


@dataclass(frozen=True)
class FaultSpec:
    """One entry of the scenario's ``faults:`` list, pre-parse of PLAN.md
    sec 7.2's trigger/duration/repeat spec into :mod:`kilnsim.protocol`
    dataclasses (done by :func:`_parse_trigger` etc below, at load time)."""

    id: str
    type: str
    target: str
    trigger: Trigger
    duration: Duration
    repeat: Repeat
    params: dict = field(default_factory=dict)  # fault-type-specific extra keys, passed through


@dataclass(frozen=True)
class DutSpec:
    profile: Optional[str] = None
    extra: dict = field(default_factory=dict)


@dataclass
class Scenario:
    name: str
    version: int
    source_path: Optional[Path]
    exercises: list = field(default_factory=list)
    preset: Optional[str] = None
    timescale: float = 1.0
    seed: int = 0
    overrides: dict = field(default_factory=dict)
    dut: Optional[DutSpec] = None
    faults: list = field(default_factory=list)  # list[FaultSpec]
    expect: list = field(default_factory=list)  # list[ExpectClause]
    forbid: list = field(default_factory=list)  # top-level forbid list, if used standalone
    at_end: list = field(default_factory=list)  # top-level at_end list, if used standalone
    report_keep: list = field(default_factory=list)
    manual_checks: list = field(default_factory=list)
    raw: dict = field(default_factory=dict)  # the parsed YAML, for anything not modeled above

    def content_hash(self) -> str:
        """Stable hash of the scenario's raw content, for report.py's
        scenario-identity field (PLAN.md sec 8.2: "scenario name/version/hash")."""
        return hashlib.sha256(yaml.safe_dump(self.raw, sort_keys=True).encode("utf-8")).hexdigest()


# ---------------------------------------------------------------------------
# trigger / duration / repeat parsing (PLAN.md sec 7.2)
# ---------------------------------------------------------------------------
def _parse_trigger(data: dict) -> Trigger:
    if not isinstance(data, dict) or len(data) != 1:
        raise ScenarioError(f"trigger must be a single-key mapping, got {data!r}")
    (key, val), = data.items()
    val = val or {}
    if key == "at_sim_time":
        t = val if not isinstance(val, dict) else val.get("t")
        if t is None:
            raise ScenarioError("at_sim_time trigger needs a value")
        return Trigger(kind=TriggerKind.AT_SIM_TIME, t=float(t))
    if key == "at_zone_temp":
        try:
            zone = int(val["zone"])
            temp_c = float(val["temp_c"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ScenarioError(f"at_zone_temp trigger missing/bad zone/temp_c: {val!r}") from exc
        edge = str(val.get("edge", "rising"))
        return Trigger(kind=TriggerKind.AT_ZONE_TEMP, zone=zone, temp_c=temp_c, edge=edge)
    if key == "on_relay_edge":
        try:
            relay = str(val["relay"])
            edge = str(val["edge"])
        except KeyError as exc:
            raise ScenarioError(f"on_relay_edge trigger missing relay/edge: {val!r}") from exc
        return Trigger(kind=TriggerKind.ON_RELAY_EDGE, relay=relay, edge=edge,
                        delay_s=float(val.get("delay_s", 0.0)))
    if key == "on_event":
        name = val.get("name") if isinstance(val, dict) else val
        if not name:
            raise ScenarioError("on_event trigger needs a name")
        return Trigger(kind=TriggerKind.ON_EVENT, event_name=str(name),
                        delay_s=float(val.get("delay_s", 0.0)) if isinstance(val, dict) else 0.0)
    if key == "after_fault":
        fault_id = val.get("fault") if isinstance(val, dict) else val
        if not fault_id:
            raise ScenarioError("after_fault trigger needs a fault id")
        return Trigger(kind=TriggerKind.AFTER_FAULT, fault_id=str(fault_id),
                        delay_s=float(val.get("delay_s", 0.0)) if isinstance(val, dict) else 0.0)
    if key == "random_in":
        try:
            t0 = float(val["t0"])
            t1 = float(val["t1"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ScenarioError(f"random_in trigger missing/bad t0/t1: {val!r}") from exc
        if t1 < t0:
            raise ScenarioError(f"random_in trigger has t1 < t0: {val!r}")
        return Trigger(kind=TriggerKind.RANDOM_IN, t0=t0, t1=t1)
    if key == "manual":
        return Trigger(kind=TriggerKind.MANUAL)
    raise ScenarioError(f"unknown trigger kind {key!r}")


def _parse_duration(data) -> Duration:
    if data == "permanent" or data is None:
        return Duration(kind=DurationKind.PERMANENT)
    if not isinstance(data, dict) or len(data) != 1:
        raise ScenarioError(f"duration must be 'permanent' or a single-key mapping, got {data!r}")
    (key, val), = data.items()
    if key == "permanent":
        return Duration(kind=DurationKind.PERMANENT)
    if key in ("for", "for_s"):
        return Duration(kind=DurationKind.FOR, t=float(val))
    if key == "until_trigger":
        return Duration(kind=DurationKind.UNTIL_TRIGGER, until=_parse_trigger(val))
    raise ScenarioError(f"unknown duration kind {key!r}")


def _parse_repeat(data) -> Repeat:
    if data is None or data == "once":
        return Repeat(kind=RepeatKind.ONCE)
    if not isinstance(data, dict) or len(data) != 1:
        raise ScenarioError(f"repeat must be 'once' or a single-key mapping, got {data!r}")
    (key, val), = data.items()
    if key == "once":
        return Repeat(kind=RepeatKind.ONCE)
    if key == "every":
        val = val if isinstance(val, dict) else {"period": val}
        try:
            period = float(val["period"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ScenarioError(f"'every' repeat needs a period: {val!r}") from exc
        return Repeat(kind=RepeatKind.EVERY, period=period, jitter=float(val.get("jitter", 0.0)))
    if key == "n_times":
        val = val if isinstance(val, dict) else {"n": val}
        try:
            n = int(val["n"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ScenarioError(f"'n_times' repeat needs n: {val!r}") from exc
        return Repeat(kind=RepeatKind.N_TIMES, n=n, period=float(val.get("period")) if val.get("period") else None,
                      jitter=float(val.get("jitter", 0.0)))
    raise ScenarioError(f"unknown repeat kind {key!r}")


def _parse_fault(data: dict) -> FaultSpec:
    required = ("id", "type", "target", "trigger")
    missing = [k for k in required if k not in data]
    if missing:
        raise ScenarioError(f"fault entry missing required key(s) {missing}: {data!r}")
    known = {"id", "type", "target", "trigger", "duration", "repeat"}
    extra = {k: v for k, v in data.items() if k not in known}
    return FaultSpec(
        id=str(data["id"]),
        type=str(data["type"]),
        target=str(data["target"]),
        trigger=_parse_trigger(data["trigger"]),
        duration=_parse_duration(data.get("duration")),
        repeat=_parse_repeat(data.get("repeat")),
        params=extra,
    )


def _parse_expect(data: dict) -> ExpectClause:
    if "name" not in data:
        raise ScenarioError(f"expect entry missing 'name': {data!r}")
    name = str(data["name"])
    has_event = "event" in data and "then" in data
    has_forbid = "forbid" in data
    has_at_end = "at_end" in data
    if sum((has_event, has_forbid, has_at_end)) != 1:
        raise ScenarioError(
            f"expect entry {name!r} must be exactly one of "
            "(event+then) / forbid / at_end, got {list(data)}"
        )
    if has_event:
        return EventThenExpect(name=name, event=dict(data["event"]), then=dict(data["then"]))
    if has_forbid:
        return ForbidExpect(name=name, forbid=dict(data["forbid"]))
    return AtEndExpect(name=name, at_end=dict(data["at_end"]))


# ---------------------------------------------------------------------------
# loader
# ---------------------------------------------------------------------------
_REQUIRED_TOP_KEYS = ("name", "version")


def load_scenario(path: Union[str, Path]) -> Scenario:
    """Load and validate a scenario YAML file per PLAN.md sec 8.1.

    Raises :class:`ScenarioError` on any structural problem -- missing
    required fields, a malformed trigger/duration/repeat, an ``expect``
    entry that is not exactly one of the three recognized forms.
    """
    path = Path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ScenarioError(f"could not read scenario file {path}: {exc}") from exc
    return load_scenario_text(text, source_path=path)


def load_scenario_text(text: str, source_path: Optional[Path] = None) -> Scenario:
    """As :func:`load_scenario`, but from an in-memory YAML string --
    the path tests use, since it needs no filesystem fixture."""
    try:
        data = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise ScenarioError(f"invalid YAML: {exc}") from exc
    if not isinstance(data, dict):
        raise ScenarioError(f"scenario document must be a mapping, got {type(data).__name__}")

    missing = [k for k in _REQUIRED_TOP_KEYS if k not in data]
    if missing:
        raise ScenarioError(f"scenario missing required top-level key(s): {missing}")

    name = str(data["name"])
    try:
        version = int(data["version"])
    except (TypeError, ValueError) as exc:
        raise ScenarioError(f"'version' must be an integer, got {data['version']!r}") from exc

    dut_raw = data.get("dut")
    dut = None
    if dut_raw is not None:
        if not isinstance(dut_raw, dict):
            raise ScenarioError(f"'dut' must be a mapping, got {dut_raw!r}")
        profile = dut_raw.get("profile")
        extra = {k: v for k, v in dut_raw.items() if k != "profile"}
        dut = DutSpec(profile=profile, extra=extra)

    faults_raw = data.get("faults", [])
    if not isinstance(faults_raw, list):
        raise ScenarioError(f"'faults' must be a list, got {faults_raw!r}")
    faults = [_parse_fault(f) for f in faults_raw]
    fault_ids = [f.id for f in faults]
    dupes = {i for i in fault_ids if fault_ids.count(i) > 1}
    if dupes:
        raise ScenarioError(f"duplicate fault id(s): {sorted(dupes)}")

    expect_raw = data.get("expect", [])
    if not isinstance(expect_raw, list):
        raise ScenarioError(f"'expect' must be a list, got {expect_raw!r}")
    expect = [_parse_expect(e) for e in expect_raw]

    # References inside triggers (on_relay_edge chaining via after_fault) and
    # expect clauses' `event: {slot: ...}` should name real fault ids -- this
    # is validated best-effort (a scenario author's typo here is exactly the
    # kind of mistake we want caught at load time, not mid-run).
    known_faults = set(fault_ids)
    for f in faults:
        if f.trigger.kind.value == "after_fault" and f.trigger.fault_id not in known_faults:
            raise ScenarioError(
                f"fault {f.id!r} triggers after_fault {f.trigger.fault_id!r}, "
                "which is not defined in this scenario's faults list"
            )
    for e in expect:
        if isinstance(e, EventThenExpect):
            slot = e.event.get("slot")
            if slot is not None and slot not in known_faults:
                raise ScenarioError(
                    f"expect {e.name!r} references fault slot {slot!r}, "
                    "which is not defined in this scenario's faults list"
                )

    overrides = data.get("overrides", {}) or {}
    if not isinstance(overrides, dict):
        raise ScenarioError(f"'overrides' must be a mapping, got {overrides!r}")

    report_keep = data.get("report_keep", []) or []
    manual_checks = data.get("manual_checks", []) or []

    return Scenario(
        name=name,
        version=version,
        source_path=source_path,
        exercises=list(data.get("exercises", []) or []),
        preset=data.get("preset"),
        timescale=float(data.get("timescale", 1.0)),
        seed=int(data.get("seed", 0)),
        overrides=overrides,
        dut=dut,
        faults=faults,
        expect=expect,
        report_keep=list(report_keep),
        manual_checks=list(manual_checks),
        raw=data,
    )


# ---------------------------------------------------------------------------
# fault compiler
# ---------------------------------------------------------------------------
def compile_faults(scenario: Scenario) -> list:
    """Turn ``scenario.faults`` into :class:`~kilnsim.protocol.FaultScheduleCommand`
    objects, one per entry, in file order (slot ids assigned by that order --
    PLAN.md sec 7.3: "trigger evaluation order is slot order", so the
    compiled order is meaningful and preserved here).
    """
    compiled = []
    for slot, f in enumerate(scenario.faults):
        params = f.params.get("params")
        if params is not None:
            values = tuple(float(x) for x in params)
        else:
            values = (0.0, 0.0, 0.0, 0.0)
        compiled.append(
            FaultScheduleCommand(
                fault_slot=slot,
                fault_type=f.type,
                target=f.target,
                trigger=f.trigger,
                duration=f.duration,
                repeat=f.repeat,
                params=values,
            )
        )
    return compiled
