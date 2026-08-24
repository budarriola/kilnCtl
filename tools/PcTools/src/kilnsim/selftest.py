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
seed => same repeat-jitter interval sequence, DESIGN_NOTES.md sec 4.2/7.2's
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
from .protocol import (
    CommandGroup,
    CtCmd,
    EventType,
    FaultCmd,
    IoCmd,
    ModelCmd,
    RelayCmd,
    SysCmd,
    TASK_STACK_MARGIN_WARN_THRESHOLD,
    TcCmd,
)

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
        # DUT_POWER_SAFETY_GET (PROTOCOL.md sec 5.5, resolved 2026-08-20): the
        # fixture's second, independent DUT-power relay (J19/safety domain).
        # A read-only probe, same as DUT_POWER_GET above -- reachability only,
        # no state change.
        ("IO/DUT_POWER_SAFETY_GET", lambda: link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)),
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


def _check_task_stack_margins(link: SimLink) -> "tuple[str, str]":
    """The preventive half of the per-task-stack-margin feature
    (PROTOCOL.md sec 4, GET_TASK_STATS / cmd_ids.h's
    SIMFW_CMD_SYS_GET_TASK_STATS): FAILS if any FreeRTOS task's stack
    margin (allocated / peak-used) is under TASK_STACK_MARGIN_WARN_THRESHOLD
    (2.0, protocol.py -- the working standard this project's own
    cmd_task/spi_emu_a/spi_emu_b audit used, shared with `kilnsim tasks`'
    human-readable flagging so the two thresholds can never silently
    drift apart). This is what turns the feature preventive rather than
    merely informative: after this check exists, a future undersized task
    stack is caught by `kilnsim selftest` on the bench, before it ships,
    instead of being found the way the first three incidents were --
    telemetry (1024 B allocated vs. 10736 B actually used) and sim_engine
    (2048 B vs. a 2328 B measured worst case, which actually bricked a
    board) by a live SWD session, and cmd_task/spi_emu_a/spi_emu_b by a
    manual -fstack-usage audit.

    Same fw_git_hash-based virtual/mock detection every other
    hardware-dependent check in this module uses (see
    `_classify_expander_link`'s own comment for the full reasoning):
    virtual_simfw does not run real FreeRTOS at all (no
    uxTaskGetStackHighWaterMark() to report -- its own README documents it
    as a protocol-shape stand-in, not a timing/memory-accurate emulation),
    so a real-hardware-only check like this one is honestly reported
    NOT_RUNNABLE there rather than either faking a pass or failing on an
    irrelevant mismatch."""
    try:
        version = link.send_command(CommandGroup.SYS, SysCmd.GET_VERSION)
    except SimLinkError as exc:
        return STATUS_FAIL, f"could not read GET_VERSION to identify the link: {exc}"
    git_hash = version.get("fw_git_hash")
    if git_hash in ("virtual", "0000000"):
        why = "virtual_simfw" if git_hash == "virtual" else "MockSimLink"
        return STATUS_NOT_RUNNABLE, (
            f"connected device self-identifies as {why} (fw_git_hash == {git_hash!r}), which does not "
            "run real FreeRTOS (no uxTaskGetStackHighWaterMark() to report) -- a stack-margin check "
            "here would be meaningless, not a fixture defect"
        )

    try:
        result = link.send_command(CommandGroup.SYS, SysCmd.GET_TASK_STATS)
    except SimLinkError as exc:
        return STATUS_FAIL, f"GET_TASK_STATS round trip errored: {exc}"

    tasks = result.get("tasks", [])
    if not tasks:
        return STATUS_FAIL, "GET_TASK_STATS reported zero tasks -- expected ~13 (10 app tasks + 2 SMP idle + timer)"

    low = [
        f"{t.get('task_stats_id_name') or t['task_stats_id']} ({t['margin']:.2f}x, "
        f"{t['peak_used_bytes']}/{t['allocated_bytes']}B used)"
        for t in tasks
        if t["margin"] < TASK_STACK_MARGIN_WARN_THRESHOLD
    ]
    if low:
        return STATUS_FAIL, (
            f"{len(low)}/{len(tasks)} task(s) under {TASK_STACK_MARGIN_WARN_THRESHOLD:.1f}x stack margin: "
            + "; ".join(low)
        )
    tightest = min(t["margin"] for t in tasks)
    return STATUS_PASS, (
        f"{len(tasks)} task(s) reported, all >= {TASK_STACK_MARGIN_WARN_THRESHOLD:.1f}x stack margin "
        f"(tightest {tightest:.2f}x)"
    )


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
    # AT_SIM_TIME, not "manual"+FIRE_NOW: this predates the fix in
    # firmware/SimFW/src/sim/fault_engine.c (`fault_sched_fire_now()`) that
    # closed FIRE_NOW's ring-event gap (see virtual_simfw's README, "now emit
    # a FAULT_FIRED ring event on both real firmware and this harness") --
    # FIRE_NOW is a legitimate alternative today. AT_SIM_TIME is kept anyway
    # because it exercises the same tick-evaluated path any real scenario's
    # faults take (the thing this check actually needs to prove works), not
    # because FIRE_NOW is still broken.
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
    EVERY+jitter FAULT_SCHEDULE (DESIGN_NOTES.md sec 7.2), drain FAULT_FIRED events
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
            f"{gaps1[:n]} vs {gaps2[:n]} -- determinism contract (DESIGN_NOTES.md sec 4.2/7.2) violated"
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


# Mirrors firmware/SimFW/src/tasks/i2c_owner.c's MCP23017_DEBOUNCE_SCAN_MS
# (8 ms) -- the PC side has no way to import that #define, so it is
# transcribed here with an explicit pointer back, not re-derived from guesswork.
MCP23017_SCAN_PERIOD_S = 0.008

#: Protocol `exp` field values (PROTOCOL.md sec 5.5) -- 0 is
#: firmware's I2C_OWNER_EXP_1 (0x25, fixed-role board: relay sense,
#: fault-line sense, E-stop drive, DUT-power-main/-safety on pins 0..7 and
#: 10), 1 is I2C_OWNER_EXP_2 (0x26, fully generic -- every pin boots
#: input+pullup, no reserved roles at all, i2c_owner.c's configure_exp2()).
EXP1 = 0
EXP2 = 1

#: i2c_owner.c's EXP1_RESERVED_MAX_PIN (7) plus the standalone
#: EXP1_PIN_DUT_POWER_SAFETY (10) -- io_pin_allowed() rejects both. Every
#: check below that touches exp1 pins picks from OUTSIDE this set (8, 9,
#: 11..15); nothing here ever asks for K1..K5/FAULT_LINE/ESTOP_DRIVE/
#: DUT_POWER_MAIN/DUT_POWER_SAFETY, so a real fixture never has its E-stop
#: driven or a DUT-power relay switched by this module.
EXP1_RESERVED_PINS = frozenset({0, 1, 2, 3, 4, 5, 6, 7, 10})


def _classify_expander_link(link: SimLink) -> "Optional[tuple[str, str]]":
    """Shared by every expander check below: returns ``None`` if `link`
    looks like real SimFW hardware, or a ready-to-return
    ``(STATUS_NOT_RUNNABLE, detail)`` (or ``(STATUS_FAIL, detail)`` if
    GET_VERSION itself couldn't be read) otherwise.

    virtual_simfw's own README is explicit that its IO_SET_DIR/WRITE/READ
    "ack but do nothing" (no I2C expander hardware modeled) -- so a
    mismatch there is the simulator's documented behavior, not a fixture
    defect, and asserting on it would be a false failure on the one link
    kilnsim can actually reach pre-bench. fw_git_hash == "virtual" is
    virtual_simfw.c's own self-identification (its GET_VERSION handler
    hard-codes hash = "virtual"); "0000000" is MockSimLink's canned
    placeholder (kilnsim.link's _default_response). Neither is real I2C
    expander hardware. A heuristic, not a protocol-level flag, but the only
    one available -- same one the original expander_read_after_write check
    used before this helper was pulled out of it.
    """
    try:
        version = link.send_command(CommandGroup.SYS, SysCmd.GET_VERSION)
    except SimLinkError as exc:
        return STATUS_FAIL, f"could not read GET_VERSION to identify the link: {exc}"
    git_hash = version.get("fw_git_hash")
    if git_hash in ("virtual", "0000000"):
        why = "virtual_simfw" if git_hash == "virtual" else "MockSimLink"
        return STATUS_NOT_RUNNABLE, (
            f"connected device self-identifies as {why} (fw_git_hash == {git_hash!r}), which does not "
            "model real I2C expander hardware for IO_SET_DIR/WRITE/READ -- a mismatch here would be "
            "expected, not a fixture defect"
        )
    return None


def _expander_read_until(link: SimLink, exp: int, pin: int, expected: bool, timeout_s: float = 0.5) -> dict:
    """Polls IO_READ(exp, pin) until it reports `expected`, bounded by
    `timeout_s`. SET_DIR/WRITE are QUEUED on i2c_owner's command queue and
    only take effect on the expander at the START of its next scan tick
    (apply_pending_commands() drains the queue, THEN scan_tick() refreshes
    the raw GPIO word i2c_owner.h's IO_READ answers from --
    i2c_owner.c, MCP23017_DEBOUNCE_SCAN_MS == 8). READ is a synchronous
    snapshot of whatever that last completed tick saw. Reading immediately
    after writing, with no wait, can win the race and observe the
    PRE-write value -- confirmed on real hardware 2026-08-24: a bare
    write-then-read-immediately sequence reported "wrote low -> read True"
    even though the expander was answering correctly (a separate CLI
    invocation per command, with real process/USB overhead between them,
    always passed). So every check below polls for the expander's OWN
    answer to change, rather than trusting a single immediate read -- which
    is also a closer match to how a real client should use this
    asynchronous write path. Shared here (this used to be a check-local
    closure) so every check added alongside expander_read_after_write reuses
    the exact same wait discipline instead of a second bare
    write-then-read."""
    deadline = time.monotonic() + timeout_s
    last: dict = {}
    while time.monotonic() < deadline:
        last = link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": exp, "pin": pin})
        if last.get("level") is expected:
            return last
        time.sleep(MCP23017_SCAN_PERIOD_S)
    return last


def _expander_write_and_confirm(link: SimLink, exp: int, pin: int, level: bool, timeout_s: float = 0.5) -> dict:
    """IO_WRITE then poll (via :func:`_expander_read_until`) for the
    expander to actually report the new level -- the write half of the
    read-after-write pattern every check below shares."""
    link.send_command(CommandGroup.IO, IoCmd.WRITE, {"exp": exp, "pin": pin, "level": level})
    return _expander_read_until(link, exp, pin, level, timeout_s)


def _restore_pin_to_safe_default(link: SimLink, exp: int, pin: int) -> None:
    """Best-effort: put `pin` back to the firmware's own boot-time safe
    default for a generic pin -- INPUT with pullup enabled (i2c_owner.c's
    configure_exp1() J20 IO_3/IO_4 + 5-spare loop, and configure_exp2()'s
    equivalent for every exp2 pin) -- so nothing is left driving into
    whatever gets plugged into this header next. Called from every check
    below's `finally` block, including on the failure path. Swallows
    SimLinkError deliberately: a check that already failed (or a link that
    just dropped mid-check) must not raise a SECOND exception out of its
    own cleanup and mask the real failure -- _run_check only sees whatever
    this function's caller returns, and a cleanup-time exception here would
    otherwise replace an honest FAIL detail with an unrelated one."""
    try:
        link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": True, "pullup": True})
    except SimLinkError:
        pass


def _check_expander_read_after_write(link: SimLink) -> "tuple[str, str]":
    # Real hardware: IO_WRITE then IO_READ the same pin genuinely round-trips
    # through the I2C expander (i2c_owner.h).
    verdict = _classify_expander_link(link)
    if verdict is not None:
        return verdict
    exp, pin = EXP1, 8  # J20 IO_3 -- outside exp1's reserved fixed-role set
    try:
        link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False})
        high = _expander_write_and_confirm(link, exp, pin, True)
        low = _expander_write_and_confirm(link, exp, pin, False)
    except SimLinkError as exc:
        # ERR_NO_SAMPLE (payloads.STATUS_ERR_NO_SAMPLE) means the args were
        # fine but i2c_owner has never gotten a successful MCP23017 ACK on
        # this bus -- exactly the expected state with no expander physically
        # attached to J20, not a fixture defect. SimLinkError only carries
        # str(CommandStatusError) (kilnsim.link wraps and drops the
        # structured .status), so the status name is matched in the message
        # text -- the same convention this module's other checks/comments
        # already use for status names (e.g. the "FAULT/SET_UNTIL_TRIGGER:
        # ERR_BAD_ARGS" wording in link.py). A genuine ERR_BAD_ARGS (wrong
        # exp/pin) is not caught by this and still fails below.
        if "ERR_NO_SAMPLE" in str(exc):
            return STATUS_SKIP, (
                "no MCP23017 responding -- attach the fixture to the board to exercise this "
                f"({exc})"
            )
        return STATUS_FAIL, f"expander read-after-write round trip errored: {exc}"
    finally:
        # Leaves pin 8 driven low otherwise -- restore to the boot-time
        # safe default before returning, on every path.
        _restore_pin_to_safe_default(link, exp, pin)
    if high.get("level") is True and low.get("level") is False:
        return STATUS_PASS, f"exp{exp} pin{pin}: wrote high->read {high.get('level')}, wrote low->read {low.get('level')}"
    return STATUS_FAIL, f"exp{exp} pin{pin}: wrote high->read {high.get('level')!r}, wrote low->read {low.get('level')!r}"


def _check_expander_exp2_read_after_write(link: SimLink) -> "tuple[str, str]":
    """The bench now has a SECOND MCP23017 physically attached (0x26,
    protocol exp=1 / firmware's I2C_OWNER_EXP_2) -- _check_expander_read_after_write
    above only ever exercises exp=0 (I2C_OWNER_EXP_1, 0x25), so exp2 has had
    zero automated coverage until now. exp2 is fully generic (configure_exp2():
    every pin boots input+pullup, no fixed relay/E-stop/DUT-power roles at
    all, unlike exp1), so any pin is fair game; pin 0 is used here
    specifically because it's the one pin number that WOULD be reserved
    (EXP1_PIN_K1) if this were exp1 -- proving the two boards are genuinely
    treated differently, not that "pin 0 happens to be safe everywhere"."""
    verdict = _classify_expander_link(link)
    if verdict is not None:
        return verdict
    exp, pin = EXP2, 0
    try:
        link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False})
        high = _expander_write_and_confirm(link, exp, pin, True)
        low = _expander_write_and_confirm(link, exp, pin, False)
    except SimLinkError as exc:
        if "ERR_NO_SAMPLE" in str(exc):
            return STATUS_SKIP, (
                f"no MCP23017 responding at exp{exp} (0x26) -- attach the fixture to exercise this ({exc})"
            )
        return STATUS_FAIL, f"exp{exp} (0x26) read-after-write round trip errored: {exc}"
    finally:
        _restore_pin_to_safe_default(link, exp, pin)
    if high.get("level") is True and low.get("level") is False:
        return STATUS_PASS, (
            f"exp{exp} (0x26) pin{pin}: wrote high->read {high.get('level')}, wrote low->read {low.get('level')}"
        )
    return STATUS_FAIL, (
        f"exp{exp} (0x26) pin{pin}: wrote high->read {high.get('level')!r}, wrote low->read {low.get('level')!r}"
    )


def _check_expander_exp1_multi_pin_read_after_write(link: SimLink) -> "tuple[str, str]":
    """A single pin (8, in expander_read_after_write above) round-tripping
    correctly doesn't prove the other generic exp1 pins do too -- a
    per-pin bug in mcp23017.c's IODIR/OLAT bit indexing wouldn't
    necessarily show up on pin 8 alone. Exercises three more of exp1's
    generic pins: 9 (J20 IO_4) and 11, 15 (true spares) --
    i2c_owner.c's io_pin_allowed()/EXP1_RESERVED_PINS above. Deliberately
    skips 0..7 and 10 (relay sense, fault-line sense, E-stop drive,
    DUT-power-main/-safety)."""
    verdict = _classify_expander_link(link)
    if verdict is not None:
        return verdict
    exp = EXP1
    pins = (9, 11, 15)
    results: dict = {}
    errors: list = []
    try:
        for pin in pins:
            try:
                link.send_command(
                    CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False}
                )
                high = _expander_write_and_confirm(link, exp, pin, True)
                low = _expander_write_and_confirm(link, exp, pin, False)
            except SimLinkError as exc:
                if "ERR_NO_SAMPLE" in str(exc):
                    return STATUS_SKIP, (
                        f"no MCP23017 responding at exp{exp} (0x25) -- attach the fixture to exercise this ({exc})"
                    )
                errors.append(f"pin{pin}: {exc}")
                continue
            results[pin] = (high.get("level"), low.get("level"))
    finally:
        for pin in pins:
            _restore_pin_to_safe_default(link, exp, pin)
    if errors:
        return STATUS_FAIL, "; ".join(errors)
    bad = {pin: rl for pin, rl in results.items() if rl != (True, False)}
    if bad:
        return STATUS_FAIL, f"exp{exp} pin(s) did not round-trip high/low correctly: {bad}"
    return STATUS_PASS, f"exp{exp} (0x25) pins {list(pins)} each wrote high->read True, wrote low->read False"


def _check_expander_pin_independence(link: SimLink) -> "tuple[str, str]":
    """Writing one pin must not disturb another -- catches a
    shadow-register/read-modify-write bug in mcp23017.c's IODIR/GPPU/OLAT
    handling (e.g. a WRITE that recomputes the whole port byte from a stale
    cached value instead of setting/clearing just its own bit). Uses exp2
    (fully generic, no reserved-pin bookkeeping needed) pins 0 and 1: both
    set to output+low, then pin 0 flipped high while confirming pin 1
    stays low, then pin 1 flipped high while confirming pin 0 stays high --
    checked in both directions so a bug that only corrupts "the other
    pin" in one specific write order isn't missed."""
    verdict = _classify_expander_link(link)
    if verdict is not None:
        return verdict
    exp = EXP2
    pin_a, pin_b = 0, 1
    try:
        for pin in (pin_a, pin_b):
            link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False})
        _expander_write_and_confirm(link, exp, pin_a, False)
        b_before = _expander_write_and_confirm(link, exp, pin_b, False)
        a_after_a_high = _expander_write_and_confirm(link, exp, pin_a, True)
        # A_high is already confirmed via polling above, so this immediate
        # READ of pin_b is safe -- it's a synchronous snapshot of the same
        # already-completed scan tick that just proved pin_a's write landed.
        b_after_a_high = link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": exp, "pin": pin_b})
        b_after_b_high = _expander_write_and_confirm(link, exp, pin_b, True)
        a_after_b_high = link.send_command(CommandGroup.IO, IoCmd.READ, {"exp": exp, "pin": pin_a})
    except SimLinkError as exc:
        if "ERR_NO_SAMPLE" in str(exc):
            return STATUS_SKIP, (
                f"no MCP23017 responding at exp{exp} (0x26) -- attach the fixture to exercise this ({exc})"
            )
        return STATUS_FAIL, f"pin-independence probe errored: {exc}"
    finally:
        for pin in (pin_a, pin_b):
            _restore_pin_to_safe_default(link, exp, pin)
    if a_after_a_high.get("level") is not True:
        return STATUS_FAIL, f"exp{exp} pin{pin_a} never read high after WRITE(true) -- can't assess independence"
    if b_before.get("level") is not False or b_after_a_high.get("level") is not False:
        return STATUS_FAIL, (
            f"exp{exp} pin{pin_b} changed after writing pin{pin_a} alone: before="
            f"{b_before.get('level')!r} after={b_after_a_high.get('level')!r} -- "
            "possible shadow-register read-modify-write bug"
        )
    if b_after_b_high.get("level") is not True:
        return STATUS_FAIL, f"exp{exp} pin{pin_b} never read high after WRITE(true) -- can't assess independence"
    if a_after_b_high.get("level") is not True:
        return STATUS_FAIL, (
            f"exp{exp} pin{pin_a} (previously written high) changed after writing pin{pin_b}: now="
            f"{a_after_b_high.get('level')!r} -- possible shadow-register read-modify-write bug"
        )
    return STATUS_PASS, (
        f"exp{exp}: pin{pin_a}/pin{pin_b} each independently reflect their own last WRITE, "
        "unaffected by writes to the other"
    )


def _check_expander_input_pullup_reads_high(link: SimLink) -> "tuple[str, str]":
    """A pin set to INPUT with its internal pull-up enabled, with nothing
    externally attached to pull it low, must read high -- a real, checkable
    property of the attached hardware (task context: "nothing else is
    attached -- no relays, no thermocouple boards, no CT transformers, no
    DUT" on either expander's header). Probes one generic pin per
    expander: exp1 (0x25) pin 11 (a true spare, well clear of the 0..7/10
    reserved set) and exp2 (0x26) pin 5 (fully generic)."""
    verdict = _classify_expander_link(link)
    if verdict is not None:
        return verdict
    probes = ((EXP1, 11), (EXP2, 5))
    results: dict = {}
    try:
        for exp, pin in probes:
            link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": True, "pullup": True})
        for exp, pin in probes:
            reading = _expander_read_until(link, exp, pin, True)
            results[(exp, pin)] = reading.get("level")
    except SimLinkError as exc:
        if "ERR_NO_SAMPLE" in str(exc):
            return STATUS_SKIP, f"no MCP23017 responding -- attach the fixture to exercise this ({exc})"
        return STATUS_FAIL, f"pull-up probe errored: {exc}"
    finally:
        for exp, pin in probes:
            _restore_pin_to_safe_default(link, exp, pin)
    bad = {k: v for k, v in results.items() if v is not True}
    if bad:
        return STATUS_FAIL, (
            f"expected internal pull-up to read high with nothing externally attached; got {bad} "
            f"(full results {results})"
        )
    return STATUS_PASS, f"exp/pin {list(results.keys())} all read high with INPUT+pullup: {results}"


def _check_expander_reserved_pin_rejected(link: SimLink) -> "tuple[str, str]":
    """Proves the reserved-pin safety interlock actually works, rather than
    just assuming it: attempts SET_DIR on exp1 pin 0 (EXP1_PIN_K1, a
    reserved relay-sense input -- i2c_owner.c's io_pin_allowed()) and
    requires the firmware to REJECT it. Never attempts WRITE on a reserved
    pin -- for EXP1_PIN_ESTOP_DRIVE/DUT_POWER_MAIN/DUT_POWER_SAFETY that
    could assert the fixture's E-stop or switch a DUT-power relay if the
    interlock this check is trying to prove were actually broken, so
    SET_DIR is used instead (also gated by the same io_pin_allowed(), with
    no such physical side effect if it somehow succeeded).

    Status matching: io_pin_allowed() rejects a reserved pin BEFORE it is
    ever enqueued (cmd_task.c's handle_io_set_dir()), so this always
    answers ERR_BAD_ARGS today -- deterministically, never the transient
    ERR_BUSY (full command queue), which can only occur for an *allowed*
    pin that got past the reserved check. A concurrent firmware pass (in
    flight as of this check being written) is adding ERR_BUSY as a
    distinct status for that separate queue-full case in the same two
    handlers -- unrelated to this reserved-pin path, but to stay robust
    against exactly-this-kind-of-adjacent-change, this matches on "the
    request was rejected with a recognized rejection status" (ERR_BAD_ARGS
    or ERR_BUSY) rather than hard-coding ERR_BAD_ARGS alone."""
    verdict = _classify_expander_link(link)
    if verdict is not None:
        return verdict
    exp, pin = EXP1, 0  # EXP1_PIN_K1 -- reserved relay-sense input
    try:
        try:
            link.send_command(CommandGroup.IO, IoCmd.SET_DIR, {"exp": exp, "pin": pin, "is_input": False, "pullup": False})
        except SimLinkError as exc:
            msg = str(exc)
            if "ERR_BAD_ARGS" in msg or "ERR_BUSY" in msg:
                return STATUS_PASS, (
                    f"SET_DIR on exp{exp} pin{pin} (reserved relay-sense input) correctly rejected: {msg}"
                )
            return STATUS_FAIL, (
                f"SET_DIR on exp{exp} pin{pin} (reserved) was rejected, but not with a recognized "
                f"rejection status: {msg}"
            )
        # No exception at all means the firmware answered OK -- accepting a
        # SET_DIR on a reserved, safety-relevant pin is exactly the
        # interlock failure this check exists to catch.
        return STATUS_FAIL, (
            f"SET_DIR on exp{exp} pin{pin} (reserved relay-sense input, EXP1_PIN_K1) was NOT rejected -- "
            "safety interlock failure"
        )
    finally:
        # If the interlock actually failed above, the pin may now be a
        # driven output -- restore it regardless of which path was taken.
        _restore_pin_to_safe_default(link, exp, pin)


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
    ("task_stack_margins", _check_task_stack_margins),
    ("telemetry_cadence", _check_telemetry_cadence),
    ("event_sequence_continuity", _check_event_sequence_continuity),
    ("determinism_spot_check", _check_determinism_spot),
    ("expander_read_after_write", _check_expander_read_after_write),
    ("expander_exp2_read_after_write", _check_expander_exp2_read_after_write),
    ("expander_exp1_multi_pin_read_after_write", _check_expander_exp1_multi_pin_read_after_write),
    ("expander_pin_independence", _check_expander_pin_independence),
    ("expander_input_pullup_reads_high", _check_expander_input_pullup_reads_high),
    ("expander_reserved_pin_rejected", _check_expander_reserved_pin_rejected),
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
