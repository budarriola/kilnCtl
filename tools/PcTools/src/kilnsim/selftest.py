"""``kilnsim selftest`` -- ``firmware/SimFW/docs/PLAN.md`` section 13, layer 2:
"Loopback tests (fixture alone, no DUT) ... a `kilnsim selftest` mode".

This is a *fixture health check*, not a scenario run: it asks "is this
SimFW device itself trustworthy right now" rather than "does a DUT react
correctly". On real hardware, PLAN.md 13.2 describes exercising the SPI
emulation via a scripted on-fixture master (a spare PIO state machine), CT
output amplitude looped to a spare ADC input, and I2C expander
read-after-write -- none of which `kilnsim` can drive purely over the wire
protocol, because they require a second piece of on-board hardware (the
scripted SPI master, a physical loopback wire) that isn't addressable from
here. Those checks are honestly reported as :data:`STATUS_NOT_RUNNABLE`
below, never faked as a pass.

What *is* meaningful to check over the wire, against either real SimFW
hardware or ``virtual_simfw`` (see that tool's README -- the same real
`benchproto` protocol either way): a PING/GET_VERSION/GET_CAPS round trip,
every command group actually reachable and answering with a well-formed
reply, TELEMETRY frames arriving at a sane cadence, EVT sequence-number
continuity across a burst of events, and a determinism spot-check (same
seed => same repeat-jitter interval sequence, PLAN.md sec 4.2/7.2's
determinism contract, using the same "compare gaps between FAULT_FIRED
events across two seeded runs" technique
``tools/PcTools/tests/test_kilnsim_virtual_simfw.py``'s own regression test
already validates).

Every check function below is (link) -> (status, detail) and is wrapped by
:func:`_run_check`, which times it and turns any :class:`SimLinkError` or
unexpected exception into a FAIL rather than letting `kilnsim selftest`
itself crash on a flaky command -- the whole point of this command is to be
a fast, trustworthy answer on bench day, not one more thing that can throw.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from .link import SimLink, SimLinkError
from .protocol import CommandGroup, CtCmd, EventType, FaultCmd, IoCmd, ModelCmd, RelayCmd, SysCmd, TcCmd

STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_SKIP = "SKIP"
#: A check whose real target is hardware kilnsim cannot address purely over
#: the wire protocol (a spare PIO SM, a physical CT->ADC loopback wire) --
#: reported distinctly from SKIP so a report reader can tell "didn't apply
#: here" from "would need different hardware than any wire link can reach".
STATUS_NOT_RUNNABLE = "NOT_RUNNABLE"

_NON_FAILING = (STATUS_PASS, STATUS_SKIP, STATUS_NOT_RUNNABLE)


@dataclass
class CheckResult:
    name: str
    status: str
    detail: str = ""
    duration_s: float = 0.0

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "status": self.status,
            "detail": self.detail,
            "duration_s": round(self.duration_s, 3),
        }


@dataclass
class SelftestReport:
    checks: "list[CheckResult]" = field(default_factory=list)

    @property
    def passed(self) -> bool:
        """True iff nothing came back FAIL -- SKIP/NOT_RUNNABLE checks don't
        fail the overall run (they're honest "couldn't assess this here"
        answers, not failures), but their presence is still visible in the
        per-check list so a reader sees exactly what wasn't covered."""
        return all(c.status != STATUS_FAIL for c in self.checks)

    def to_dict(self) -> dict:
        return {"passed": self.passed, "checks": [c.to_dict() for c in self.checks]}

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2)

    def to_text(self) -> str:
        lines = []
        for c in self.checks:
            lines.append(f"[{c.status:^12}] {c.name} ({c.duration_s:.2f}s) {c.detail}")
        lines.append("")
        lines.append("PASSED" if self.passed else "FAILED")
        return "\n".join(lines)


def _run_check(name: str, fn: Callable[[SimLink], "tuple[str, str]"], link: SimLink) -> CheckResult:
    t0 = time.monotonic()
    try:
        status, detail = fn(link)
    except SimLinkError as exc:
        status, detail = STATUS_FAIL, f"link error: {exc}"
    except Exception as exc:  # noqa: BLE001 - a check must never crash the whole run
        status, detail = STATUS_FAIL, f"unexpected {type(exc).__name__}: {exc}"
    return CheckResult(name=name, status=status, detail=detail, duration_s=time.monotonic() - t0)


# ---------------------------------------------------------------------------
# individual checks
# ---------------------------------------------------------------------------
def _check_ping_roundtrip(link: SimLink) -> "tuple[str, str]":
    for _ in range(3):
        reply = link.send_command(CommandGroup.SYS, SysCmd.PING)
        if reply.get("pong") is not True:
            return STATUS_FAIL, f"PING did not reply {{'pong': True}}: {reply!r}"
    return STATUS_PASS, "3/3 PING round trips answered {'pong': True}"


def _check_version_and_caps(link: SimLink) -> "tuple[str, str]":
    version = link.send_command(CommandGroup.SYS, SysCmd.GET_VERSION)
    caps = link.send_command(CommandGroup.SYS, SysCmd.GET_CAPS)
    problems = []
    if caps.get("protocol_version", 0) < caps.get("min_compatible", 0):
        problems.append(
            f"protocol_version {caps.get('protocol_version')} < its own min_compatible "
            f"{caps.get('min_compatible')}"
        )
    for field_name in ("zone_count_max", "tc_channel_count", "ct_channel_count", "relay_count"):
        if not caps.get(field_name):
            problems.append(f"GET_CAPS.{field_name} is 0/missing")
    if problems:
        return STATUS_FAIL, "; ".join(problems)
    detail = (
        f"fw {version.get('fw_version')} (git {version.get('fw_git_hash')}"
        f"{'*dirty' if version.get('fw_git_dirty') else ''}), "
        f"protocol v{caps.get('protocol_version')} (min {caps.get('min_compatible')}), "
        f"zones {caps.get('zone_count_min')}..{caps.get('zone_count_max')}, "
        f"tc={caps.get('tc_channel_count')} ct={caps.get('ct_channel_count')} "
        f"relay={caps.get('relay_count')}"
    )
    return STATUS_PASS, detail


#: One safe, read-only (or trivially reversible) probe per command group --
#: "every command group reachable and returning sane replies" from the
#: task's own scoping. FAULT/TC/CT/RELAY/IO all have real GET_*/LIST/*_GET
#: reads; MODEL's only pure read is GET_ZONE_PARAMS, which needs a zone
#: index that's always valid (zone 0, since GET_CAPS just proved
#: zone_count_min >= 1).
def _group_probes(link: SimLink) -> "list[tuple[str, Callable[[], dict]]]":
    return [
        ("MODEL/GET_ZONE_PARAMS", lambda: link.send_command(CommandGroup.MODEL, ModelCmd.GET_ZONE_PARAMS, {"zone": 0})),
        ("TC/GET_REGS", lambda: link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": 0})),
        ("TC/GET_MASTER_CONFIG", lambda: link.send_command(CommandGroup.TC, TcCmd.GET_MASTER_CONFIG, {"channel": 0})),
        ("CT/GET_STATE", lambda: link.send_command(CommandGroup.CT, CtCmd.GET_STATE, {"channel": 0})),
        ("RELAY/GET_STATES", lambda: link.send_command(CommandGroup.RELAY, RelayCmd.GET_STATES)),
        ("RELAY/GET_EDGES", lambda: link.send_command(CommandGroup.RELAY, RelayCmd.GET_EDGES, {"since_seq": 0})),
        ("IO/ESTOP_GET", lambda: link.send_command(CommandGroup.IO, IoCmd.ESTOP_GET)),
        ("IO/DUT_POWER_GET", lambda: link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)),
        ("IO/FAULT_LINE_GET", lambda: link.send_command(CommandGroup.IO, IoCmd.FAULT_LINE_GET)),
        ("FAULT/LIST", lambda: link.send_command(CommandGroup.FAULT, FaultCmd.LIST, {"start_index": 0, "max_count": 1})),
    ]


def _check_command_groups_reachable(link: SimLink) -> "tuple[str, str]":
    failures = []
    ok_names = []
    for name, probe in _group_probes(link):
        try:
            reply = probe()
        except SimLinkError as exc:
            failures.append(f"{name}: {exc}")
            continue
        if not isinstance(reply, dict):
            failures.append(f"{name}: reply was not a dict ({reply!r})")
            continue
        ok_names.append(name)
    if failures:
        return STATUS_FAIL, f"{len(ok_names)}/{len(ok_names) + len(failures)} groups OK; failed: " + "; ".join(failures)
    return STATUS_PASS, f"all {len(ok_names)} probed command groups (SYS already proven by earlier checks) answered"


def _check_telemetry_cadence(link: SimLink) -> "tuple[str, str]":
    get_last = getattr(link, "get_last_telemetry", None)
    if get_last is None:
        return STATUS_SKIP, (
            "this link type doesn't expose push telemetry (e.g. MockSimLink never "
            "simulates the TELEMETRY broadcast) -- nothing to observe"
        )
    deadline = time.monotonic() + 2.0
    samples = []
    last_seen = None
    while time.monotonic() < deadline and len(samples) < 3:
        snap = get_last()
        if snap is not None and snap.get("sim_time_us") != last_seen:
            last_seen = snap.get("sim_time_us")
            samples.append(last_seen)
        time.sleep(0.05)
    if len(samples) < 2:
        return STATUS_FAIL, f"only {len(samples)} distinct TELEMETRY sim_time_us update(s) observed in 2s"
    gaps_us = [b - a for a, b in zip(samples, samples[1:])]
    return STATUS_PASS, f"{len(samples)} TELEMETRY updates in 2s, sim_time_us deltas {gaps_us}"


def _check_event_sequence_continuity(link: SimLink) -> "tuple[str, str]":
    link.read_events(timeout=0.0)  # drain any stale backlog first
    # fault_engine.h's slot pool is FAULT_ENGINE_MAX_SLOTS == 31 (0..30) --
    # PROTOCOL.md sec 5.6's LIST doc confirms "the fixed 32-slot pool" --
    # so slot ids must stay in range or FAULT_SCHEDULE answers ERR_BAD_ARGS.
    # 30 is the top of that range, unlikely to collide with a real scenario
    # (the shipped scenario library uses small slot ids from 0).
    slot = 30
    # Deliberately AT_SIM_TIME, not "manual"+FIRE_NOW: probing this against
    # virtual_simfw found that FAULT_FIRE_NOW forces the slot's state
    # (fault_sched_fire_now(), confirmed via FAULT_LIST going armed->active,
    # fire_count incrementing) WITHOUT emitting a FAULT_FIRED EVT frame --
    # a real gap in virtual_simfw's FIRE_NOW handler (reported, not
    # something this tree can fix -- see this task's final report). A
    # natural trigger-driven fire (evaluated on tick, the same path any real
    # scenario's faults take) does emit the EVT frame correctly, which is
    # what this check actually needs to exercise.
    link.send_command(
        CommandGroup.FAULT, FaultCmd.SCHEDULE,
        {
            "fault_slot": slot,
            "fault_type": "welded_ssr",
            "target": "relay:K1",
            "trigger": {"kind": "at_sim_time", "t": 0.05},
            "duration": {"kind": "permanent"},
        },
    )
    try:
        events = []
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline and not events:
            events.extend(link.read_events(timeout=0.3))
            if not events:
                time.sleep(0.02)  # MockSimLink's read_events never blocks -- avoid busy-spinning
    finally:
        try:
            link.send_command(CommandGroup.FAULT, FaultCmd.CANCEL, {"fault_slot": slot})
        except SimLinkError:
            pass  # best-effort cleanup; the check's own verdict doesn't depend on this

    if not events:
        return STATUS_FAIL, "AT_SIM_TIME(t=0.05) FAULT_SCHEDULE produced no EVT frames at all within 1.5s"

    seqs = [e.seq for e in events]
    gaps = [b - a for a, b in zip(seqs, seqs[1:]) if b - a != 1]
    reported_gap_count = getattr(link, "evt_seq_gap_count", None)
    if gaps:
        return STATUS_FAIL, f"non-consecutive seq numbers observed in this burst: {seqs}"
    if reported_gap_count:
        return STATUS_FAIL, f"link reports {reported_gap_count} EVT sequence gap(s) since connect"
    fired = [e for e in events if e.event_type == EventType.FAULT_FIRED]
    return STATUS_PASS, (
        f"{len(events)} EVT frame(s) received, seq {seqs[0]}..{seqs[-1]} consecutive, "
        f"{len(fired)} FAULT_FIRED, link-level gap counter "
        f"{'unavailable' if reported_gap_count is None else reported_gap_count}"
    )


def _fault_fired_gaps(link: SimLink, seed: int) -> "list[int]":
    """One determinism-probe run: reset+reseed+load a fast preset, arm an
    EVERY+jitter FAULT_SCHEDULE (PLAN.md sec 7.2), drain FAULT_FIRED events
    for a few seconds of accelerated sim time, return the sim_time_us gaps
    between consecutive fires -- the same "compare interval sequences, not
    absolute times" technique
    tools/PcTools/tests/test_kilnsim_virtual_simfw.py's own determinism
    regression test uses and documents at length (that test's own comment
    explains why absolute fire time is NOT expected to match run-to-run but
    the *interval sequence* is)."""
    link.send_command(CommandGroup.SYS, SysCmd.RESET_SIM, {"keep_params": False})
    link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": seed})
    link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": "fast_test"})
    link.read_events(timeout=0.0)
    slot = 29  # see event-sequence-continuity's comment on the 0..30 slot range
    link.send_command(
        CommandGroup.FAULT, FaultCmd.SCHEDULE,
        {
            "fault_slot": slot,
            "fault_type": "tc_noise",
            "target": "tc:0",
            "trigger": {"kind": "at_sim_time", "t": 0.2},
            "duration": {"kind": "for", "t": 0.1},
            "repeat": {"kind": "every", "period": 0.4, "jitter": 0.3},
            "params": [3.0, 0.0, 0.0, 0.0],
        },
    )
    fired_times = []
    deadline = time.monotonic() + 2.5
    try:
        while time.monotonic() < deadline and len(fired_times) < 4:
            got = link.read_events(timeout=0.3)
            for evt in got:
                if evt.event_type == EventType.FAULT_FIRED:
                    fired_times.append(evt.sim_time_us)
            if not got:
                time.sleep(0.02)  # MockSimLink's read_events never blocks -- avoid busy-spinning
    finally:
        try:
            link.send_command(CommandGroup.FAULT, FaultCmd.CANCEL, {"fault_slot": slot})
        except SimLinkError:
            pass
    return [b - a for a, b in zip(fired_times, fired_times[1:])]


def _check_determinism_spot(link: SimLink) -> "tuple[str, str]":
    # SET_TIMESCALE is not used here deliberately: payloads.py now encodes
    # it as `u32 timescale_x100` LE, matching PROTOCOL.md sec 4 / real
    # firmware's cmd_task.c and virtual_simfw.c's decoder (a previous pass
    # encoded a raw f32 here, a real wire-format mismatch against real
    # hardware -- fixed on both the PC and virtual-device sides in the same
    # pass). Leaving the default 1.00x timescale still sidesteps needing a
    # non-default value in this particular probe; the probe's own
    # period/jitter are small enough to finish in a few real seconds at 1x.
    gaps1 = _fault_fired_gaps(link, seed=13)
    gaps2 = _fault_fired_gaps(link, seed=13)
    if len(gaps1) < 2 or len(gaps2) < 2:
        return STATUS_SKIP, (
            f"not enough repeat fires to compare ({len(gaps1)}, {len(gaps2)} gaps) -- "
            "probe scenario didn't repeat enough times in the check's time budget"
        )
    n = min(len(gaps1), len(gaps2))
    if gaps1[:n] != gaps2[:n]:
        return STATUS_FAIL, (
            f"same seed (13) produced different EVERY+jitter interval sequences: "
            f"{gaps1[:n]} vs {gaps2[:n]} -- determinism contract (PLAN.md sec 4.2/7.2) violated"
        )
    return STATUS_PASS, f"seed 13 run twice: identical {n}-interval FAULT_FIRED gap sequence {gaps1[:n]}"


def _check_spi_master_loopback(link: SimLink) -> "tuple[str, str]":
    return STATUS_NOT_RUNNABLE, (
        "PLAN.md sec 13.2: needs a spare on-fixture PIO state machine clocked as a "
        "scripted SPI master to exercise the MAX31856 emulation end-to-end -- not "
        "addressable purely over the benchproto wire link kilnsim speaks"
    )


def _check_ct_adc_loopback(link: SimLink) -> "tuple[str, str]":
    return STATUS_NOT_RUNNABLE, (
        "PLAN.md sec 13.2: needs a CT output physically looped to a spare ADC input "
        "for amplitude sanity -- a hardware loopback wire, not something a wire-protocol "
        "command can observe"
    )


def _check_expander_read_after_write(link: SimLink) -> "tuple[str, str]":
    # Real hardware: IO_WRITE then IO_READ the same pin genuinely round-trips
    # through the I2C expander (i2c_owner.h). virtual_simfw's own README is
    # explicit that its IO_SET_DIR/WRITE/READ "ack but do nothing" (no I2C
    # expander hardware modeled) -- so a mismatch there is the simulator's
    # documented behavior, not a fixture defect, and asserting on it here
    # would be a false failure on the one link kilnsim can actually reach
    # today. fw_git_hash == "virtual" is virtual_simfw.c's own self-
    # identification (its GET_VERSION handler hard-codes hash = "virtual");
    # a heuristic, not a protocol-level flag, but the only one available.
    try:
        version = link.send_command(CommandGroup.SYS, SysCmd.GET_VERSION)
    except SimLinkError as exc:
        return STATUS_FAIL, f"could not read GET_VERSION to identify the link: {exc}"
    git_hash = version.get("fw_git_hash")
    if git_hash in ("virtual", "0000000"):
        # "virtual" is virtual_simfw.c's own self-identification; "0000000"
        # is MockSimLink's canned placeholder (kilnsim.link's
        # _default_response) -- neither is real I2C expander hardware.
        why = "virtual_simfw" if git_hash == "virtual" else "MockSimLink"
        return STATUS_NOT_RUNNABLE, (
            f"connected device self-identifies as {why} (fw_git_hash == {git_hash!r}), which does not "
            "model real I2C expander hardware for IO_SET_DIR/WRITE/READ -- a read-after-write "
            "mismatch here would be expected, not a fixture defect"
        )
    pin, exp = 8, 0  # port B pin 0 -- outside exp1's reserved fixed-role 0..7 range
    try:
        link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False})
        link.send_command(CommandGroup.IO, IoCmd.WRITE, {"exp": exp, "pin": pin, "level": True})
        high = link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": exp, "pin": pin})
        link.send_command(CommandGroup.IO, IoCmd.WRITE, {"exp": exp, "pin": pin, "level": False})
        low = link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": exp, "pin": pin})
    except SimLinkError as exc:
        return STATUS_FAIL, f"expander read-after-write round trip errored: {exc}"
    if high.get("level") is True and low.get("level") is False:
        return STATUS_PASS, f"exp{exp} pin{pin}: wrote high->read {high.get('level')}, wrote low->read {low.get('level')}"
    return STATUS_FAIL, f"exp{exp} pin{pin}: wrote high->read {high.get('level')!r}, wrote low->read {low.get('level')!r}"


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------
#: Ordered so cheap/foundational checks (that later checks implicitly rely
#: on -- e.g. it's not worth spending the determinism probe's several
#: seconds if PING itself is failing) run first.
_CHECKS: "list[tuple[str, Callable[[SimLink], tuple]]]" = [
    ("ping_roundtrip", _check_ping_roundtrip),
    ("version_and_caps", _check_version_and_caps),
    ("command_groups_reachable", _check_command_groups_reachable),
    ("telemetry_cadence", _check_telemetry_cadence),
    ("event_sequence_continuity", _check_event_sequence_continuity),
    ("determinism_spot_check", _check_determinism_spot),
    ("expander_read_after_write", _check_expander_read_after_write),
    ("spi_master_loopback", _check_spi_master_loopback),
    ("ct_adc_loopback", _check_ct_adc_loopback),
]


def run_selftest(link: SimLink) -> SelftestReport:
    """Runs every check in :data:`_CHECKS` against an already-connected
    ``link`` and returns the assembled report. Never raises -- a check that
    blows up is recorded as a FAIL for that check, not a crash of the whole
    run (see :func:`_run_check`)."""
    report = SelftestReport()
    for name, fn in _CHECKS:
        report.checks.append(_run_check(name, fn, link))
    return report
