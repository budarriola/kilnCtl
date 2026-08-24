"""``kilnsim testmgr`` -- a one-command, tiered regression suite that uses the
SimFW bench fixture as stimulus to validate the real firmwares (SaftyFW on
the RP2040 safety processor, KilnFW on the ESP32-S3).

This module is an ORCHESTRATOR, not a new test engine: the actual scenario
execution is :func:`kilnsim.runner.run_scenario` (already real, already
verified against hardware), the actual pass/fail logic is
:mod:`kilnsim.report`, and the fixture health check is
:mod:`kilnsim.selftest`. What this module adds on top:

1. **Hardware presence detection.** Probes what is actually attached --
   the SimFW fixture itself, the safety processor (SaftyFW), and the main
   controller (KilnFW/ESP) -- before running anything, and reports each
   tier's availability honestly. See :func:`default_fixture_probe` and
   :func:`default_esp_and_saftyfw_probe`.

2. **A DUT-less-scenario problem, closed without editing the runner.**
   Every scenario in ``firmware/SimFW/scenarios/*.yaml`` declares a ``dut:``
   block, and several of their ``expect:`` clauses assert on DUT-driven
   observations (relay sense K1..K5, the isolated Fault line). Run one of
   those with no SaftyFW/ESP attached and :mod:`kilnsim.report` correctly
   reports a genuine **FAIL** -- "K4 state never observed; cannot be
   dut:K4_open at end" is a real, structural non-pass, not a bug in
   report.py. The problem is purely at the ORCHESTRATION layer: a suite that
   runs such a scenario with no DUT attached and reports the result as FAIL
   is lying about what was actually tested. :func:`classify_scenario` reads
   each scenario's ``expect:`` clauses (generically, via :func:`_walk_refs`)
   to work out the *minimum hardware tier* it needs, and the suite runner
   below reports a scenario whose tier isn't met as :data:`NOT_RUNNABLE`,
   never as FAIL.

3. **A second, distinct problem this pass also found and reports
   honestly rather than silently working around**: :mod:`kilnsim.protocol`
   defines four EventType values (GUARD_TRIP, GUARD_WARN, LINK_UP,
   TRIP_INEFFECTIVE_LATCHED) as "kilnsim-local/synthetic only ... nothing in
   SimFW's own EVT wire stream can produce them today" (see that module's
   own comment on ``WIRE_EVENT_TYPES``), and :mod:`kilnsim.runner` -- the
   thing that actually assembles a scenario's event list against real
   hardware -- only ever synthesizes fault_line/estop/current edges from
   TELEMETRY (:class:`kilnsim.runner._TelemetryEdgeTracker`), never these
   four. 25 of the 27 shipped scenarios reference one of these types in an
   ``expect:`` clause. Run one of those scenarios for real via ``kilnsim
   run`` today and that clause's cause event never occurs, so
   ``evaluate_expectations`` reports it SKIPPED ("triggering event never
   occurred") -- which does not fail the run, so a report reader sees a
   clean overall PASS with no obvious sign that the guard-trip evidence the
   scenario exists to produce was never actually gathered. This is exactly
   the kind of vacuous-pass the guard-coverage report (requirement 4) exists
   to prevent, so :func:`classify_scenario` also flags these clauses
   (``has_runner_gap``) and :func:`compute_guard_coverage` refuses to credit
   a guard with "hardware evidence" on the strength of one alone. Fixing
   the underlying gap (teaching ``kilnsim.runner`` to synthesize these from
   SaftyFW telemetry via kilnctrl) is out of scope for this pass -- it would
   mean either polling kilnctrl mid-run from inside kilnsim.runner (a real
   design decision belonging to that module, not something to smuggle in
   here) or extending virtual_dut's separate approach; recorded here so it
   is findable, not silently worked around by this module pretending the
   gap does not exist.

4. **Known-state reset between scenarios.** ``kilnsim.runner.run_scenario``
   already sets seed/timescale fresh on every call (its own first two
   commands), so the specific symptom this project's own notes describe
   (a later scenario silently inheriting a prior one's ``timescale: 10.0,
   seed: 56``) cannot happen *through this orchestrator* provided every
   scenario is run through ``run_scenario`` -- which the suite runner below
   always does. What ``run_scenario`` does NOT do is clear a fault schedule
   or thermal/relay state a *previous* scenario left armed (it only ever
   ADDS fault slots). :func:`run_one_scenario` closes that the rest of the
   way by sending ``SYS/RESET_SIM`` before every scenario, the same call
   :mod:`kilnsim.selftest`'s own determinism probe already uses for exactly
   this purpose.

5. **Guard-coverage reporting.** :func:`compute_guard_coverage` cross-
   references every scenario's ``exercises:`` tags against the full S1..S13
   guard list from ``firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`` and
   reports, per guard: which scenarios declare it, whether any of them
   actually ran, and whether a clean PASS free of the runner-gap problem
   above was ever observed -- the "which guards have HARDWARE evidence"
   question the task cares about more than any raw pass count.

6. **A ``--quick`` subset** (:func:`select_quick_scenarios`) for the "I just
   reflashed, is it still sane" case -- the shortest-estimated-duration
   scenarios, so a quick run stays quick without hand-curating a list that
   would drift out of date as scenarios are added.

Nothing here modifies ``tools/PcTools/src/kilnctrl/**`` -- that package is
owned by a concurrent session. Where this module needs to talk to the real
ESP/SaftyFW it imports kilnctrl's own already-public classes exactly as
kilnctrl's own MCP server does (:class:`kilnctrl.serial_link.UartLink`,
:class:`kilnctrl.info.InfoClient`, :class:`kilnctrl.safety.SafetyClient`),
never duplicating or patching their logic. If kilnctrl is not importable (or
a real board simply is not there), the corresponding tier is reported
unavailable with the reason -- never faked as a pass and never silently
skipped.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

from . import selftest as _selftest
from .link import SimLink, SimLinkError
from .protocol import CommandGroup, ModelCmd, SysCmd
from .report import BLOCKED, FAIL, PASS, SKIPPED, Report, evaluate_expectations
from .runner import estimate_run_duration_s, run_scenario
from .scenario import (
    AtEndExpect,
    EventThenExpect,
    ForbidExpect,
    Scenario,
    ScenarioError,
    load_scenario,
)


class GuardObserverUnavailable(RuntimeError):
    """Raised by :func:`run_suite` when ``guards=True`` was requested but a
    real :class:`~kilnsim.guard_observer.SafetyGuardObserver` cannot be
    attached this session (kilnctrl not importable, no live SaftyFW link,
    or ``mock=True`` requested alongside ``guards=True``).

    Deliberately a loud failure, not a silent downgrade: requirement (2) of
    the task this class closes is that a run asked to attach a guard
    observer must never quietly fall back to running without one and then
    report guard coverage as if nothing were missing -- see this module's
    ``compute_guard_coverage`` and TEST_MANAGER.md sec 9's first bullet for
    the exact failure mode this exists to prevent. Callers (``kilnsim.cli
    .cmd_testmgr``) catch this and print a clear error instead of letting
    the suite run degrade silently."""

# ---------------------------------------------------------------------------
# guard list -- firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md is authoritative
# ---------------------------------------------------------------------------
ALL_GUARDS: "tuple[str, ...]" = (
    "S1", "S2", "S3", "S4", "S5", "S6a", "S6b", "S7", "S8",
    "S9", "S10", "S11", "S12", "S13",
)

#: My own verdict labels, layered on top of kilnsim.report's PASS/FAIL/
#: SKIPPED/BLOCKED (a scenario that DID run always carries one of those,
#: unchanged, on its own Report). These three only ever appear as an
#: OUTCOME's own .verdict when the scenario never ran at all through this
#: suite -- see the module docstring's point 2 and 6.
NOT_RUNNABLE = "NOT_RUNNABLE"
ERROR = "ERROR"
SKIPPED_QUICK = "SKIPPED_QUICK"

_TIER_NAMES = {
    -1: "none (no SimFW fixture)",
    0: "fixture alone",
    1: "fixture + SaftyFW",
    2: "fixture + SaftyFW + ESP (KilnFW)",
}

#: dut: entity -> minimum tier that can actually produce this observation
#: for real (see module docstring point 2). "estop"/"dut_power" are driven
#: by the FIXTURE itself (kilnsim IS the E-stop loop's own driver and the
#: DUT-power relay's own controller, DESIGN_NOTES.md sec 3.4/3.5 -- "the
#: fixture *becomes* the jumper"), so those need no DUT at all. K1..K5 are
#: sensed by the fixture but DRIVEN by SaftyFW's relay_owner through the
#: wetting circuit. "current" only flows because a SaftyFW-driven relay
#: closed (or CT manual mode, not the scenario-library's normal case).
#: "fault_line" is the opto-isolated Fault OUTPUT only the ESP can assert
#: (safety_set_fault_out/link_task's own automatic assertion on PC-link
#: loss/TC fault/watchdog) -- SaftyFW only ever SENSES it, never drives it.
#: An entity not in this table defaults conservatively to tier 1 (assume it
#: needs at least SaftyFW) rather than tier 0, in classify_scenario/_ref_tier.
_DUT_ENTITY_TIER: "dict[str, int]" = {
    "estop": 0,
    "dut_power": 0,
    "K1": 1, "K2": 1, "K3": 1, "K4": 1, "K5": 1,
    "current": 1,
    "fault_line": 2,
}

#: EventType names (report.py's _resolve_event_clause takes the lowercase
#: string form) that kilnsim.runner cannot produce against real hardware
#: today -- see module docstring point 3 and kilnsim.protocol.EventType's
#: own "kilnsim-local/synthetic only" comment.
_SYNTHETIC_UNPRODUCIBLE_EVENT_TYPES = frozenset(
    {"guard_trip", "guard_warn", "link_up", "trip_ineffective_latched"}
)

_POLARITY_WORDS = {
    "open", "on", "asserted", "true", "present",
    "closed", "close", "off", "deasserted", "false", "absent",
}


def default_scenarios_dir() -> Path:
    """``firmware/SimFW/scenarios``, resolved relative to this file rather
    than the current working directory, so ``kilnsim testmgr`` works the
    same regardless of where it's invoked from."""
    return Path(__file__).resolve().parents[4] / "firmware" / "SimFW" / "scenarios"


# ---------------------------------------------------------------------------
# hardware presence
# ---------------------------------------------------------------------------
@dataclass
class PresenceResult:
    present: bool
    detail: str

    def to_dict(self) -> dict:
        return {"present": self.present, "detail": self.detail}


@dataclass
class HardwarePresence:
    fixture: PresenceResult
    saftyfw: PresenceResult
    esp: PresenceResult

    @property
    def max_tier(self) -> int:
        """-1 (nothing runnable) .. 2 (full system). Strictly staged: SaftyFW
        is only ever reported present if the fixture is too (finding 1
        assumes the fixture; nothing here can observe SaftyFW without it),
        and this project's kilnctrl link only ever reaches SaftyFW THROUGH
        the ESP's UART bridge (see default_esp_and_saftyfw_probe), so ESP
        presence with SaftyFW absent still caps out at tier 0 there too --
        this property just encodes the resulting staircase."""
        if not self.fixture.present:
            return -1
        if not self.saftyfw.present:
            return 0
        if not self.esp.present:
            return 1
        return 2

    def to_dict(self) -> dict:
        return {
            "fixture": self.fixture.to_dict(),
            "saftyfw": self.saftyfw.to_dict(),
            "esp": self.esp.to_dict(),
            "max_tier": self.max_tier,
            "max_tier_name": _TIER_NAMES[self.max_tier],
        }


def default_fixture_probe(link: SimLink, port: Optional[str] = None) -> PresenceResult:
    """Attempts to connect ``link`` (already constructed, not yet connected)
    and PING it. Leaves ``link`` connected on success (the caller -- run_suite
    -- reuses this same connection for selftest and every scenario, rather
    than reconnecting per probe)."""
    try:
        opened = link.connect(port)
    except SimLinkError as exc:
        return PresenceResult(False, f"SimFW fixture not reachable: {exc}")
    except Exception as exc:  # noqa: BLE001 - presence detection must never raise
        return PresenceResult(False, f"SimFW fixture not reachable: {exc}")
    try:
        reply = link.send_command(CommandGroup.SYS, SysCmd.PING)
    except SimLinkError as exc:
        return PresenceResult(False, f"connected to {opened} but PING failed: {exc}")
    if reply.get("pong") is not True:
        return PresenceResult(False, f"connected to {opened} but PING did not answer {{'pong': True}}: {reply!r}")
    return PresenceResult(True, f"SimFW fixture responded to PING on {opened}")


def default_esp_and_saftyfw_probe(port: Optional[str] = None) -> "tuple[PresenceResult, PresenceResult]":
    """Real presence probe for KilnFW (ESP32-S3) and, through it, SaftyFW.

    Uses ``tools/PcTools/src/kilnctrl``'s own public, already-shipped classes
    exactly as its own MCP server does (UartLink, InfoClient, SafetyClient)
    -- no kilnctrl code is read, duplicated, or modified beyond importing
    these names (task constraint: that package is owned by a concurrent
    session).

    SaftyFW has no PC-visible link of its own in the shipped protocol --
    SAFETY task traffic is forwarded over the ESP's UART bridge into the
    isolated safety link (``mcp_server.py``'s own module docstring: "the
    opto-isolated link to the RP2040 safety processor") -- so "SaftyFW
    present" is necessarily ESP-mediated here: this probe reports SaftyFW
    present only when the ESP both answers and reports SAFETY_FLAG_LINK_UP
    (SafetyStatus.link_up) with an age that means it has actually heard from
    the Pico (age_ms != SAFETY_AGE_NEVER). A standalone SaftyFW-without-ESP
    presence check would need the OpenOCD/SWD debug_probe path instead
    (a real, different check -- see ``mcp_server.py``'s ``debug_*`` tool
    family) -- this pass does not wire that up, and reports the gap honestly
    here rather than half-building it: if the ESP link is down, SaftyFW is
    reported not-present with a reason that says exactly that, never
    silently degraded to "also absent" for an unrelated cause.
    """
    try:
        from kilnctrl.info import InfoClient, InfoQueryError  # type: ignore
        from kilnctrl.protocol import SAFETY_AGE_NEVER  # type: ignore
        from kilnctrl.safety import SafetyClient  # type: ignore
        from kilnctrl.serial_link import UartLink  # type: ignore
    except ImportError as exc:
        na = PresenceResult(False, f"kilnctrl not importable in this environment: {exc}")
        return na, na

    link = UartLink()
    try:
        opened = link.connect(port)
    except Exception as exc:  # noqa: BLE001 - presence detection must never raise
        na_esp = PresenceResult(False, f"ESP not reachable: {exc}")
        na_safety = PresenceResult(
            False,
            "cannot check SaftyFW without a live ESP link -- SAFETY task traffic is "
            "forwarded over the ESP's UART bridge, there is no other PC-visible path "
            "to it in this build",
        )
        return na_esp, na_safety

    try:
        info = InfoClient(link)
        try:
            version = info.get_fw_version()
            esp = PresenceResult(True, f"ESP responded on {opened}: {version.describe()}")
        except InfoQueryError as exc:
            return (
                PresenceResult(False, f"connected to {opened} but GET_FW_VERSION failed: {exc}"),
                PresenceResult(False, "ESP not confirmed alive -- SaftyFW check not attempted"),
            )
        finally:
            info.close()

        safety = SafetyClient(link)
        try:
            status = safety.get_status()
            if status.age_ms == SAFETY_AGE_NEVER or not status.link_up:
                saftyfw = PresenceResult(
                    False,
                    f"ESP has no live status from the safety processor "
                    f"(age_ms={status.age_ms}, link_up={status.link_up})",
                )
            else:
                saftyfw = PresenceResult(
                    True, f"SaftyFW status last updated {status.age_ms} ms ago via the ESP's isolated link"
                )
        except Exception as exc:  # noqa: BLE001
            saftyfw = PresenceResult(False, f"SAFETY_CMD_GET_STATUS failed: {exc}")
        finally:
            safety.close()
    finally:
        link.disconnect()

    return esp, saftyfw


# ---------------------------------------------------------------------------
# scenario tier classification (module docstring point 2/3)
# ---------------------------------------------------------------------------
def _dut_entity(flag: str) -> str:
    if "_" in flag:
        entity, word = flag.rsplit("_", 1)
        if word.lower() in _POLARITY_WORDS:
            return entity
    return flag


def _walk_refs(node):
    """Recursively yields ('dut', <flag string>) / ('event', <type name>)
    references found anywhere inside a clause's raw dict structure.
    Deliberately generic (walks any dict/list, not report.py's exact clause
    grammar) so it keeps working as that grammar grows new shapes -- see the
    module docstring's point 2 for why this needs to exist at all."""
    if isinstance(node, dict):
        dut_val = node.get("dut")
        if isinstance(dut_val, str):
            yield ("dut", dut_val)
        type_val = node.get("type")
        if isinstance(type_val, str):
            yield ("event", type_val)
        for v in node.values():
            yield from _walk_refs(v)
    elif isinstance(node, list):
        for item in node:
            yield from _walk_refs(item)


def _clause_raw(clause) -> dict:
    if isinstance(clause, EventThenExpect):
        return {"event": clause.event, "then": clause.then}
    if isinstance(clause, ForbidExpect):
        return {"forbid": clause.forbid}
    if isinstance(clause, AtEndExpect):
        return {"at_end": clause.at_end}
    return {}


def _ref_tier(kind: str, name: str) -> "tuple[int, bool]":
    """Returns (min_tier, is_runner_gap)."""
    if kind == "event":
        if name in _SYNTHETIC_UNPRODUCIBLE_EVENT_TYPES:
            return 1, True
        return 0, False  # real wire EventTypes are fixture-native
    entity = _dut_entity(name)
    return _DUT_ENTITY_TIER.get(entity, 1), False


def _wants_operator_enable(dut_extra: dict) -> bool:
    for action in dut_extra.get("operator_actions", []) or []:
        if isinstance(action, dict) and action.get("action") == "request_enable":
            return True
    return False


def _has_timed_relay_actions(dut_extra: dict) -> bool:
    for action in dut_extra.get("operator_actions", []) or []:
        if isinstance(action, dict) and action.get("action") == "command_relay":
            return True
    return False


@dataclass(frozen=True)
class ScenarioRequirement:
    min_tier: int
    has_runner_gap: bool
    has_timed_relay_actions: bool
    refs: "tuple[tuple[str, str, str, int, bool], ...]"  # (clause_name, kind, name, tier, gap)


def classify_scenario(scenario: Scenario) -> ScenarioRequirement:
    """What hardware tier ``scenario`` actually needs to be run meaningfully,
    and whether any of its clauses target an event type this suite cannot
    observe today regardless of tier (module docstring points 2 and 3)."""
    min_tier = 0
    has_gap = False
    refs = []
    for clause in scenario.expect:
        raw = _clause_raw(clause)
        for kind, name in _walk_refs(raw):
            tier, gap = _ref_tier(kind, name)
            min_tier = max(min_tier, tier)
            has_gap = has_gap or gap
            refs.append((clause.name, kind, name, tier, gap))

    dut_extra = scenario.dut.extra if scenario.dut else {}
    if _wants_operator_enable(dut_extra):
        min_tier = max(min_tier, 1)

    return ScenarioRequirement(
        min_tier=min_tier,
        has_runner_gap=has_gap,
        has_timed_relay_actions=_has_timed_relay_actions(dut_extra),
        refs=tuple(refs),
    )


# ---------------------------------------------------------------------------
# scenario discovery
# ---------------------------------------------------------------------------
def discover_scenarios(scenarios_dir: Path) -> "tuple[list[Scenario], list[ScenarioOutcome]]":
    """Loads every ``*.yaml`` in ``scenarios_dir``. A file that fails to load
    becomes its own ERROR outcome (surfaced in the report) instead of
    vanishing silently."""
    scenarios: "list[Scenario]" = []
    errors: "list[ScenarioOutcome]" = []
    for path in sorted(scenarios_dir.glob("*.yaml")):
        try:
            scenarios.append(load_scenario(path))
        except ScenarioError as exc:
            errors.append(
                ScenarioOutcome(
                    name=path.stem, path=str(path), exercises=[], min_tier=0,
                    has_runner_gap=False, verdict=ERROR, detail=f"could not load scenario: {exc}",
                )
            )
    return scenarios, errors


def select_quick_scenarios(scenarios: "list[Scenario]", limit: int = 6) -> "list[Scenario]":
    """The ``limit`` scenarios with the smallest estimated run duration --
    the fast subset for "I just reflashed, is it still sane" (task's own
    framing of the primary use case). Ties broken by name for determinism."""
    ranked = sorted(scenarios, key=lambda s: (estimate_run_duration_s(s), s.name))
    return ranked[:limit]


# ---------------------------------------------------------------------------
# running one scenario
# ---------------------------------------------------------------------------
@dataclass
class ScenarioOutcome:
    name: str
    path: str
    exercises: "list[str]"
    min_tier: int
    has_runner_gap: bool
    verdict: str  # PASS | FAIL | BLOCKED | NOT_RUNNABLE | ERROR | SKIPPED_QUICK
    detail: str = ""
    duration_s: float = 0.0
    report: "Optional[Report]" = None
    #: Whether a real kilnsim.guard_observer.GuardObserver was actually
    #: attached to kilnsim.runner.run_scenario for THIS run. Independent of
    #: has_runner_gap (a static, YAML-only classification of whether a
    #: clause targets a guard-only event type) -- this field is the runtime
    #: fact that decides whether such a clause's outcome is real hardware
    #: evidence or, per runner.py's own
    #: _block_expectations_missing_guard_observer, a BLOCKED placeholder.
    #: See compute_guard_coverage's use of the two fields together for why
    #: requirement (3) needs both, not just has_runner_gap alone.
    guard_observer_attached: bool = False

    def to_dict(self) -> dict:
        d = {
            "name": self.name,
            "path": self.path,
            "exercises": list(self.exercises),
            "min_tier": self.min_tier,
            "min_tier_name": _TIER_NAMES.get(self.min_tier, str(self.min_tier)),
            "has_runner_gap": self.has_runner_gap,
            "guard_observer_attached": self.guard_observer_attached,
            "verdict": self.verdict,
            "detail": self.detail,
            "duration_s": round(self.duration_s, 3),
        }
        if self.report is not None:
            d["report"] = self.report.to_dict()
        return d


def describe_runner_gap(req: ScenarioRequirement, verdict: str) -> Optional[str]:
    """The loud-vacuity note for a scenario whose guard-evidence clause(s)
    target an event type :mod:`kilnsim.runner` cannot produce (module
    docstring point 3), when the run's overall verdict looks clean enough to
    be mistaken for hardware evidence (PASS or SKIPPED). Returns ``None``
    when there is nothing to warn about.

    Factored out of :func:`run_one_scenario` so a caller that runs a single
    scenario *without* going through the full ``testmgr`` suite --
    ``kilnsim.cli.cmd_run``'s ``kilnsim run`` is the motivating one, see that
    module -- can attach the same warning instead of inventing a second,
    differently-worded concept for the same gap."""
    if not (req.has_runner_gap and verdict in (PASS, SKIPPED)):
        return None
    gap_names = sorted({name for _, kind, name, _, gap in req.refs if gap})
    return (
        f"this scenario's guard-evidence clause(s) target event type(s) "
        f"{gap_names} that kilnsim.runner cannot produce against real hardware today "
        "(see this module's docstring point 3) -- do not read this run's overall "
        f"verdict ({verdict}) as hardware evidence that a guard fired"
    )


def run_one_scenario(
    link: SimLink,
    scenario: Scenario,
    presence: HardwarePresence,
    *,
    mock: bool,
    request_enable_fn: "Optional[Callable[[], None]]" = None,
    guard_observer_factory: "Optional[Callable[[], tuple]]" = None,
) -> ScenarioOutcome:
    # `guard_observer_factory`, when given, is called fresh for EVERY
    # scenario (never shared across scenarios) and must return
    # ``(observer, close_fn)``. Fresh per scenario, deliberately: a
    # SafetyGuardObserver's edge trackers (_last_trip_reason etc., see
    # guard_observer.py) carry state, and this module's own known-state-
    # reset discipline (module docstring point 4 -- SYS/RESET_SIM before
    # every scenario) would be undermined if a leftover "already saw this
    # trip reason" from scenario A silently suppressed detecting the SAME
    # trip reason firing again in scenario B. Only called when mock=False
    # (run_suite() itself refuses guards=True with mock=True -- see
    # GuardObserverUnavailable), and only for scenarios that actually run
    # (never for a NOT_RUNNABLE outcome, decided below).
    req = classify_scenario(scenario)
    path = str(scenario.source_path) if scenario.source_path else scenario.name

    if req.min_tier > presence.max_tier:
        return ScenarioOutcome(
            name=scenario.name, path=path, exercises=list(scenario.exercises),
            min_tier=req.min_tier, has_runner_gap=req.has_runner_gap, verdict=NOT_RUNNABLE,
            detail=(
                f"needs tier {req.min_tier} ({_TIER_NAMES[req.min_tier]}); only tier "
                f"{presence.max_tier} is available ({_TIER_NAMES[presence.max_tier]}) -- "
                "reported NOT_RUNNABLE, not FAIL, per this scenario's dut: observations "
                "being genuinely unobservable with this hardware attached"
            ),
        )

    # Known-state reset (module docstring point 4): run_scenario() itself
    # always sets seed/timescale fresh, but never clears a fault schedule or
    # thermal/relay state a PREVIOUS scenario left armed.
    try:
        link.send_command(CommandGroup.SYS, SysCmd.RESET_SIM, {"keep_params": False})
    except SimLinkError as exc:
        return ScenarioOutcome(
            name=scenario.name, path=path, exercises=list(scenario.exercises),
            min_tier=req.min_tier, has_runner_gap=req.has_runner_gap, verdict=ERROR,
            detail=f"could not reset known state before this scenario: {exc}",
        )

    enable_note = ""
    dut_extra = scenario.dut.extra if scenario.dut else {}
    if request_enable_fn is not None and _wants_operator_enable(dut_extra):
        try:
            request_enable_fn()
            enable_note = (
                "manager sent SAFETY_CMD_REQUEST_ENABLE before this run, standing in for "
                "the operator per this scenario's dut.operator_actions"
            )
        except Exception as exc:  # noqa: BLE001
            enable_note = f"manager tried to request enable but failed: {exc}"
        if req.has_timed_relay_actions:
            enable_note += (
                "; this scenario also schedules timed dut.operator_actions "
                "(command_relay at specific sim times) that this pass of the manager does "
                "NOT replay -- only a one-shot request_enable before the run starts. See "
                "docs/TEST_MANAGER.md's known-limitations section."
            )

    # Attach a real guard observer for this one scenario, if requested. Built
    # here (not by the caller) so its lifetime is scoped to exactly this
    # scenario's run -- opened right before run_scenario, closed in the
    # `finally` below regardless of how the run ends. NOT constructed for
    # mock=True (guard_observer_factory is only ever passed non-None when
    # run_suite() has already confirmed mock=False -- see
    # GuardObserverUnavailable's mock+guards check) -- the mock branch below
    # calls evaluate_expectations directly and has no guard_observer
    # parameter to give one to.
    guard_observer = None
    close_guard_observer: "Optional[Callable[[], None]]" = None
    if guard_observer_factory is not None and not mock:
        try:
            guard_observer, close_guard_observer = guard_observer_factory()
        except Exception as exc:  # noqa: BLE001 - fail loudly, never silently run without one
            raise GuardObserverUnavailable(
                f"--guards requested but a guard observer could not be attached for "
                f"scenario {scenario.name!r}: {exc}"
            ) from exc

    t0 = time.monotonic()
    try:
        try:
            if mock:
                link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": scenario.seed})
                link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": scenario.timescale})
                if scenario.preset:
                    link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": scenario.preset})
                events = link.read_events(timeout=0.0)
                report = evaluate_expectations(scenario, events, seed=scenario.seed, timescale=scenario.timescale)
            else:
                report = run_scenario(
                    link, scenario, seed=scenario.seed, timescale=scenario.timescale,
                    guard_observer=guard_observer,
                )
        except SimLinkError as exc:
            detail = f"run failed: {exc}"
            if enable_note:
                detail += f" [{enable_note}]"
            return ScenarioOutcome(
                name=scenario.name, path=path, exercises=list(scenario.exercises),
                min_tier=req.min_tier, has_runner_gap=req.has_runner_gap, verdict=ERROR, detail=detail,
                guard_observer_attached=guard_observer is not None,
            )
    finally:
        if close_guard_observer is not None:
            close_guard_observer()
    duration = time.monotonic() - t0

    detail = enable_note
    gap_note = describe_runner_gap(req, report.verdict)
    if gap_note:
        detail = f"{detail}; {gap_note}" if detail else gap_note

    return ScenarioOutcome(
        name=scenario.name, path=path, exercises=list(scenario.exercises),
        min_tier=req.min_tier, has_runner_gap=req.has_runner_gap, verdict=report.verdict,
        detail=detail, duration_s=duration, report=report,
        guard_observer_attached=guard_observer is not None,
    )


def make_request_enable_fn(port: Optional[str] = None) -> "Optional[Callable[[], None]]":
    """A callable that sends one SAFETY_CMD_REQUEST_ENABLE(True) over a
    fresh kilnctrl UartLink, or None if kilnctrl isn't importable here.
    Opens/closes its own link per call (at most once per scenario) rather
    than sharing the presence probe's connection, to keep the two concerns
    (detecting SaftyFW vs. acting as operator) independently testable and to
    avoid holding a link open across the whole suite for a one-shot send."""
    try:
        from kilnctrl import devices  # type: ignore
        from kilnctrl.serial_link import UartLink  # type: ignore
    except ImportError:
        return None

    def _fn() -> None:
        link = UartLink()
        link.connect(port)
        try:
            result = link.send(
                dst_task=devices.UART_TASK_ID_SAFETY,
                src_task=devices.UART_TASK_ID_SAFETY,
                payload=devices.safety_request_enable(True),
            )
            if not result.ok:
                raise RuntimeError(f"SAFETY_CMD_REQUEST_ENABLE not delivered: {result.describe()}")
        finally:
            link.disconnect()

    return _fn


def default_guard_observer_factory(port: Optional[str] = None) -> "tuple":
    """Builds one live :class:`~kilnsim.guard_observer.SafetyGuardObserver`
    over its own fresh ``kilnctrl.serial_link.UartLink``, and returns
    ``(observer, close_fn)``.

    Unlike :func:`make_request_enable_fn` above, this function does NOT
    catch ``ImportError`` and degrade to returning ``None`` -- requirement
    (2) of the task this closes is that a run asked to attach a guard
    observer must fail loudly, never silently drop it. Any exception here
    (kilnctrl not importable, the port not opening, SafetyClient construction
    failing) is left to propagate to the caller
    (:func:`run_one_scenario`/:func:`run_suite`), which wraps it in
    :class:`GuardObserverUnavailable` -- a distinct, clearly-named error
    rather than a bare ``ImportError``/``SimLinkError`` a caller might
    reasonably mistake for something else going wrong.

    The link is opened fresh per call (mirrors ``make_request_enable_fn``'s
    own per-call UartLink, not the presence probe's shared one) so its
    lifetime is exactly one scenario's run -- see
    :func:`run_one_scenario`'s own comment on why a shared, suite-lifetime
    observer would be wrong (stale edge-tracker state leaking between
    scenarios)."""
    from kilnctrl.serial_link import UartLink  # type: ignore

    from .guard_observer import make_safety_guard_observer

    link = UartLink()
    link.connect(port)
    observer = make_safety_guard_observer(link)

    def _close() -> None:
        link.disconnect()

    return observer, _close


# ---------------------------------------------------------------------------
# guard coverage (module docstring point 5)
# ---------------------------------------------------------------------------
@dataclass
class GuardCoverage:
    guard: str
    declared_in: "list[str]"
    status: str
    detail: "list[str]"

    def to_dict(self) -> dict:
        return {
            "guard": self.guard, "declared_in": list(self.declared_in),
            "status": self.status, "detail": list(self.detail),
        }


def compute_guard_coverage(
    scenarios: "list[Scenario]", outcomes: "list[ScenarioOutcome]"
) -> "dict[str, GuardCoverage]":
    by_name = {o.name: o for o in outcomes}
    coverage: "dict[str, GuardCoverage]" = {}
    for guard in ALL_GUARDS:
        declared = [s.name for s in scenarios if guard in s.exercises]
        detail: "list[str]" = []
        any_clean_pass = False
        any_not_runnable = False
        any_gap_no_observer = False
        any_ran = False
        for name in declared:
            outcome = by_name.get(name)
            if outcome is None:
                detail.append(f"{name}: not run this session")
                continue
            if outcome.verdict in (NOT_RUNNABLE, SKIPPED_QUICK):
                any_not_runnable = True
                detail.append(f"{name}: {outcome.verdict} ({outcome.detail})")
                continue
            any_ran = True
            if outcome.has_runner_gap and not outcome.guard_observer_attached:
                # Requirement (3): "guard observed, no trip seen" and "guard
                # never observable because no observer was attached" are
                # different facts and must not print the same. This branch is
                # the LATTER -- has_runner_gap is a static, YAML-only
                # classification ("this clause targets a guard-only event
                # type"), and guard_observer_attached is the runtime fact of
                # whether anything actually watched SaftyFW for this run. No
                # observer attached means this guard's state was genuinely
                # never observed this session, full stop -- not "observed and
                # quiet".
                any_gap_no_observer = True
                detail.append(
                    f"{name}: ran (overall {outcome.verdict}), but no guard observer was attached "
                    "this session -- guard state was NEVER OBSERVED for this clause (no real "
                    "hardware evidence either way); run `kilnsim testmgr --guards` with SaftyFW "
                    "attached to close this"
                )
                continue
            # Either an ordinary (non-guard-only) clause, or a guard-only
            # clause that a real SafetyGuardObserver actually watched this
            # run (guard_observer_attached=True) -- in both cases
            # outcome.verdict is real evidence, not vacuous:
            # runner.py's own _block_expectations_missing_guard_observer only
            # downgrades a guard-typed clause to BLOCKED when no observer was
            # attached at all, which this branch has already excluded. A
            # PASS here for a has_runner_gap clause means the observer really
            # saw (or, for a `forbid`, really watched for and never saw) the
            # guard trip -- "guard observed, no trip seen" is exactly the
            # SKIPPED/PASS-on-forbid case this note distinguishes from the
            # branch above.
            observed_note = " (guard observer attached)" if outcome.has_runner_gap else ""
            detail.append(f"{name}: ran, overall {outcome.verdict}{observed_note}")
            if outcome.verdict == PASS:
                any_clean_pass = True

        if not declared:
            status = "no scenario declares exercises: [%s]" % guard
        elif any_clean_pass:
            status = "hardware evidence obtained this session (a scenario ran clean, no runner gap)"
        elif any_ran and any_gap_no_observer and not any_not_runnable:
            status = (
                "declared and ran, but every scenario's guard clause is a kilnsim.runner gap -- "
                "no usable evidence (no guard observer was attached this session; run with "
                "--guards and SaftyFW attached to close this)"
            )
        elif any_not_runnable and not any_ran:
            status = "declared, but hardware tier unavailable this session -- none of its scenarios ran"
        elif any_ran:
            status = "declared and ran, but did not produce a clean PASS this session (see detail)"
        else:
            status = "declared, but not exercised this session"

        coverage[guard] = GuardCoverage(guard=guard, declared_in=declared, status=status, detail=detail)
    return coverage


# ---------------------------------------------------------------------------
# suite-level report
# ---------------------------------------------------------------------------
@dataclass
class SuiteReport:
    presence: HardwarePresence
    quick: bool
    selftest: "Optional[_selftest.SelftestReport]"
    scenario_outcomes: "list[ScenarioOutcome]"
    guard_coverage: "dict[str, GuardCoverage]"
    started_at: float
    duration_s: float
    #: Whether `--guards` was requested for this run. Purely a reporting
    #: field -- run_suite() has already raised GuardObserverUnavailable
    #: before this dataclass is ever constructed if `guards=True` couldn't
    #: actually be honored, so by the time a SuiteReport exists, `guards`
    #: True here always means every real (non-mock, non-NOT_RUNNABLE)
    #: scenario outcome's own guard_observer_attached is also True.
    guards: bool = False

    @property
    def exit_code(self) -> int:
        """0 = everything runnable passed. 1 = a real failure (including
        selftest failing, or a scenario ERRORing). 2 = no real failure, but
        at least one scenario is BLOCKED on documented DUT incompleteness --
        mirrors kilnsim.cli's own run exit-code convention (0/1/2)."""
        if self.selftest is not None and not self.selftest.passed:
            return 1
        if any(o.verdict in (FAIL, ERROR) for o in self.scenario_outcomes):
            return 1
        if any(o.verdict == BLOCKED for o in self.scenario_outcomes):
            return 2
        return 0

    @property
    def passed(self) -> bool:
        return self.exit_code == 0

    def to_dict(self) -> dict:
        return {
            "presence": self.presence.to_dict(),
            "quick": self.quick,
            "guards": self.guards,
            "selftest": self.selftest.to_dict() if self.selftest is not None else None,
            "scenario_outcomes": [o.to_dict() for o in self.scenario_outcomes],
            "guard_coverage": {g: c.to_dict() for g, c in self.guard_coverage.items()},
            "started_at": self.started_at,
            "duration_s": round(self.duration_s, 3),
            "exit_code": self.exit_code,
        }

    def to_json(self) -> str:
        import json

        return json.dumps(self.to_dict(), indent=2)

    def to_text(self) -> str:
        lines = ["=== kilnsim testmgr ===", ""]
        lines.append("hardware presence:")
        lines.append(f"  fixture: {'PRESENT' if self.presence.fixture.present else 'absent'} -- {self.presence.fixture.detail}")
        lines.append(f"  SaftyFW: {'PRESENT' if self.presence.saftyfw.present else 'absent'} -- {self.presence.saftyfw.detail}")
        lines.append(f"  ESP:     {'PRESENT' if self.presence.esp.present else 'absent'} -- {self.presence.esp.detail}")
        lines.append(f"  max tier available: {self.presence.max_tier} ({_TIER_NAMES[self.presence.max_tier]})")
        lines.append(
            "  guard observation: "
            + ("ENABLED -- a real SafetyGuardObserver was attached to every real scenario run"
               if self.guards else
               "disabled (default) -- guard-typed expectations were reported BLOCKED, "
               "never vacuously PASS; pass --guards (with SaftyFW attached) for real evidence")
        )
        lines.append("")

        if self.selftest is not None:
            lines.append(f"fixture selftest: {'PASSED' if self.selftest.passed else 'FAILED'} "
                          f"({sum(1 for c in self.selftest.checks if c.status == 'PASS')}/"
                          f"{len(self.selftest.checks)} PASS)")
        else:
            lines.append("fixture selftest: not run (fixture absent)")
        lines.append("")

        lines.append(f"scenarios ({'quick subset' if self.quick else 'full suite'}):")
        for o in self.scenario_outcomes:
            lines.append(f"  [{o.verdict:^13}] {o.name} ({o.duration_s:.1f}s)"
                          + (f" -- {o.detail}" if o.detail else ""))
        lines.append("")

        lines.append("guard coverage (S1..S13):")
        for guard in ALL_GUARDS:
            cov = self.guard_coverage[guard]
            lines.append(f"  {guard}: {cov.status}")
            if cov.declared_in:
                lines.append(f"       declared in: {', '.join(cov.declared_in)}")
        lines.append("")

        lines.append(f"exit code: {self.exit_code} ({'PASS' if self.exit_code == 0 else 'BLOCKED-only' if self.exit_code == 2 else 'FAIL'})")
        return "\n".join(lines)


def _not_runnable_outcomes(scenarios: "list[Scenario]", reason: str) -> "list[ScenarioOutcome]":
    outcomes = []
    for s in scenarios:
        req = classify_scenario(s)
        outcomes.append(
            ScenarioOutcome(
                name=s.name, path=str(s.source_path) if s.source_path else s.name,
                exercises=list(s.exercises), min_tier=req.min_tier, has_runner_gap=req.has_runner_gap,
                verdict=NOT_RUNNABLE, detail=reason,
            )
        )
    return outcomes


def run_suite(
    link: SimLink,
    scenarios_dir: "Optional[Path]" = None,
    *,
    quick: bool = False,
    mock: bool = False,
    port: Optional[str] = None,
    fixture_probe: "Optional[Callable[[SimLink, Optional[str]], PresenceResult]]" = None,
    esp_saftyfw_probe: "Optional[Callable[[], tuple[PresenceResult, PresenceResult]]]" = None,
    request_enable_fn_factory: "Optional[Callable[[Optional[str]], Optional[Callable[[], None]]]]" = None,
    guards: bool = False,
    guard_observer_factory: "Optional[Callable[[Optional[str]], tuple]]" = None,
) -> SuiteReport:
    """Runs the whole tiered suite against ``link`` (constructed but not yet
    connected). This is the one function ``kilnsim testmgr`` calls.

    Every probe/factory parameter defaults to the real implementation
    (:func:`default_fixture_probe`, :func:`default_esp_and_saftyfw_probe`,
    :func:`make_request_enable_fn`, :func:`default_guard_observer_factory`)
    and exists as a parameter specifically so tests can inject fakes -- see
    ``tests/test_kilnsim_testmgr.py``.

    ``guards`` (default **False** -- opt-in): attach a real
    :class:`~kilnsim.guard_observer.SafetyGuardObserver` to every scenario run
    for real (never for ``mock=True``), so guard-typed expectations can get
    real hardware evidence instead of always being downgraded to BLOCKED by
    ``kilnsim.runner``'s own ``_block_expectations_missing_guard_observer``.
    Kept default-off deliberately: attaching one opens a SECOND link, through
    kilnctrl, to a real SaftyFW board, and must never become a surprise
    dependency of a run that didn't ask for it (this is exactly the same
    "``--mock`` must never touch real hardware" property
    ``esp_saftyfw_probe``'s own mock branch above already protects, extended
    to this second link).

    When ``guards=True`` but a real observer genuinely cannot be attached --
    ``mock=True`` requested alongside it, or SaftyFW not reachable this
    session per ``presence.saftyfw`` (this reuses the SAME tier-detection
    presence result ``run_one_scenario`` already uses for tier-gating,
    requirement (4) -- there is no separate, parallel SaftyFW-reachability
    check here) -- this function raises :class:`GuardObserverUnavailable`
    rather than silently running without one. A run that quietly drops guard
    observation and then reports guard coverage as if it had some is exactly
    the failure mode this whole feature exists to close (requirement (2)).
    """
    started_at = time.time()
    t0 = time.monotonic()
    scenarios_dir = scenarios_dir or default_scenarios_dir()
    fixture_probe = fixture_probe or default_fixture_probe
    if guards and mock:
        # Fail fast, before touching `link` at all: mock=True never runs
        # kilnsim.runner.run_scenario (run_one_scenario's mock branch calls
        # evaluate_expectations directly, which has no guard_observer
        # parameter to accept one), so a real observer could never be used
        # even if one were built. Raising here rather than silently ignoring
        # `guards` keeps the promise explicit instead of a caller discovering
        # it only by noticing guard coverage never improved.
        raise GuardObserverUnavailable(
            "--guards requested together with --mock -- a mock run never opens a real "
            "SaftyFW link (kilnsim.runner.run_scenario, which is what a guard observer "
            "attaches to, is never called against MockSimLink), so a guard observer can "
            "never be attached; drop --mock or drop --guards"
        )
    if esp_saftyfw_probe is None:
        if mock:
            # --mock means "no real hardware at all, ever" -- the default
            # ESP/SaftyFW probe opens a real kilnctrl UartLink, which would
            # silently violate that promise (confirmed while smoke-testing
            # this module: `kilnsim --mock testmgr` still tried to open a
            # real COM port for the ESP probe before this guard existed).
            na = PresenceResult(False, "not probed: --mock requested, no real hardware is touched")
            esp_saftyfw_probe = lambda: (na, na)  # noqa: E731
        else:
            esp_saftyfw_probe = lambda: default_esp_and_saftyfw_probe(port)  # noqa: E731
    request_enable_fn_factory = request_enable_fn_factory or make_request_enable_fn
    guard_observer_factory = guard_observer_factory or default_guard_observer_factory

    scenarios, load_errors = discover_scenarios(scenarios_dir)

    fixture = fixture_probe(link, port)
    if fixture.present:
        esp, saftyfw = esp_saftyfw_probe()
    else:
        esp = PresenceResult(False, "not probed: SimFW fixture is not present")
        saftyfw = PresenceResult(False, "not probed: SimFW fixture is not present")
    presence = HardwarePresence(fixture=fixture, saftyfw=saftyfw, esp=esp)

    if guards and not presence.saftyfw.present:
        # Requirement (4): reuse the SAME presence result run_one_scenario's
        # tier-gating already computed above, rather than a second,
        # parallel "is SaftyFW really there" probe that could disagree with
        # it. Best-effort disconnect before raising -- fixture_probe() may
        # have left `link` connected on success (its own docstring), and
        # this early return skips the normal try/finally disconnect below.
        try:
            link.disconnect()
        except Exception:  # noqa: BLE001 - best-effort cleanup
            pass
        raise GuardObserverUnavailable(
            f"--guards requested but SaftyFW is not reachable this session "
            f"({presence.saftyfw.detail}) -- guard-typed expectations cannot be given real "
            "hardware evidence without a live safety-processor link; attach SaftyFW (through "
            "the ESP's UART bridge) or drop --guards"
        )

    if not fixture.present:
        outcomes = list(load_errors) + _not_runnable_outcomes(
            scenarios, f"SimFW fixture not present: {fixture.detail}"
        )
        coverage = compute_guard_coverage(scenarios, outcomes)
        return SuiteReport(
            presence=presence, quick=quick, selftest=None, scenario_outcomes=outcomes,
            guard_coverage=coverage, started_at=started_at, duration_s=time.monotonic() - t0,
            guards=guards,
        )

    outcomes = list(load_errors)
    selftest_report = None
    try:
        selftest_report = _selftest.run_selftest(link)

        run_list = scenarios
        skip_list: "list[Scenario]" = []
        if quick:
            run_list = select_quick_scenarios(scenarios)
            run_names = {s.name for s in run_list}
            skip_list = [s for s in scenarios if s.name not in run_names]

        request_enable_fn = None
        if presence.saftyfw.present and not mock:
            request_enable_fn = request_enable_fn_factory(port)

        # Only ever non-None when `guards=True` -- the two early-exit checks
        # above have already confirmed mock=False and presence.saftyfw.present
        # by the time we get here, so every call below builds a real,
        # fresh-per-scenario observer (run_one_scenario's own comment on why
        # fresh-per-scenario, not one shared across the whole suite).
        per_scenario_guard_factory = (lambda: guard_observer_factory(port)) if guards else None

        for s in run_list:
            outcomes.append(
                run_one_scenario(
                    link, s, presence, mock=mock, request_enable_fn=request_enable_fn,
                    guard_observer_factory=per_scenario_guard_factory,
                )
            )
        outcomes.extend(
            _skipped_quick_outcomes(skip_list)
        )
    finally:
        try:
            link.disconnect()
        except Exception:  # noqa: BLE001 - best-effort cleanup
            pass

    coverage = compute_guard_coverage(scenarios, outcomes)
    return SuiteReport(
        presence=presence, quick=quick, selftest=selftest_report, scenario_outcomes=outcomes,
        guard_coverage=coverage, started_at=started_at, duration_s=time.monotonic() - t0,
        guards=guards,
    )


def _skipped_quick_outcomes(scenarios: "list[Scenario]") -> "list[ScenarioOutcome]":
    outcomes = []
    for s in scenarios:
        req = classify_scenario(s)
        outcomes.append(
            ScenarioOutcome(
                name=s.name, path=str(s.source_path) if s.source_path else s.name,
                exercises=list(s.exercises), min_tier=req.min_tier, has_runner_gap=req.has_runner_gap,
                verdict=SKIPPED_QUICK,
                detail="excluded from --quick's fast subset; run the full suite for full coverage",
            )
        )
    return outcomes
