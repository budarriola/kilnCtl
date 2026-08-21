#!/usr/bin/env python3
"""run_dut_scenarios.py -- closes the loop that virtual_simfw's own README
documents as the one thing it cannot do alone: "No SaftyFW guards exist, so
guard_warn/guard_trip events ... can never appear."

This script is the orchestrator half of virtual_dut (see this directory's
README.md for the full architecture and its honestly-reported limits). It:

  1. Launches ``virtual_simfw.exe`` (unmodified, another agent's build under
     firmware/SimFW/tools/virtual_simfw/) as the sole TCP client permitted
     by that program (its README: "Binds to 127.0.0.1 only, one client at a
     time") -- this script IS that one client; kilnsim's own CLI/MCP surface
     is not run concurrently against the same virtual_simfw process, see
     the README's "single-client protocol limit" finding for why not.
  2. Launches ``dut_core/build/dut_core.exe`` (the REAL, unmodified
     safety_guards.c + relay_grace.c behind a tiny stdio protocol, see
     dut_core/main.c) as a second child process.
  3. Uses ``tools/PcTools/src/kilnsim`` as a READ-ONLY LIBRARY (imported,
     never edited) for everything it already does correctly: the real
     benchproto/TCP wire codec (kilnsim.link.TcpSimLink), scenario loading
     (kilnsim.scenario), fault-schedule compilation, and expectation
     evaluation (kilnsim.report.evaluate_expectations). It reuses several of
     kilnsim.runner's private helper functions (arming a scenario, wire
     event translation) rather than re-deriving that orchestration logic a
     second time -- this is USING kilnsim as a library, not modifying it;
     nothing under tools/PcTools/ is written to by this file.
  4. Adds the one thing kilnsim.runner cannot do on its own (by design --
     kilnsim.protocol's own docstring on GUARD_TRIP/GUARD_WARN: "kilnsim-
     local/synthetic ... nothing in SimFW's own EVT wire stream can produce
     them today"): for every telemetry poll, reads the safety thermocouple's
     raw registers + E-stop state from virtual_simfw, ticks dut_core.exe the
     right number of 100ms steps to cover the elapsed sim time, and turns
     any WARN/TRIP/K4-state edge into a synthetic
     :class:`kilnsim.protocol.Event` with exactly the payload shape
     report.py's ``_find_events``/``_entity_state_events`` already expect
     (see kilnsim/report.py's module docstring) -- so a scenario's
     ``{event: {type: guard_trip, guard: S7}}`` or ``{dut: K4_open}`` clause
     can genuinely PASS/FAIL against the real guard's real output, not just
     SKIP.

This pass ALSO closes the loop the previous pass correctly reported as
blocked: after each dut_core.exe poll, `_send_relay_set_sense()` feeds
dut_core's real, unmodified `energized` decision (relay_owner_task()'s own
state, via relay_grace.c/safety_guards.c) back into virtual_simfw as K4's
sensed contact state, using a new virtual_simfw-only command
(SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE, RELAY group id 0xF0 -- see that file's
own header comment on why it is namespaced as virtual-only and will never
become a real SimFW/PROTOCOL.md command). See README.md for what this loop
closure did and did not change about scenario results -- the short version:
K4 is never energized in current SaftyFW (a pre-existing, documented
finding, unaffected by this pass), and separately, virtual_simfw's
device_tick() (a faithful, near-verbatim port of REAL, unmodified
firmware/SimFW/src/tasks/sim_engine.c) never gated duty[]/current_a[] on K4
in the first place -- so the loop is now genuinely wired end-to-end, but
produces no numerically different scenario results, for two independent
and separately-verified reasons, not because the wiring is a no-op.

Context, link liveness and CT current (added 2026-08-20, after SaftyFW
commit ``f304392``): ``safety_core_build_input()`` now reads link_task's
published ``context_snapshot_t``, ``link_task_link_up()``,
``link_task_get_relay_on_continuous_ms()`` and ``current_task``'s ADC
snapshot, so a harness that keeps hardcoding those to false is no longer a
faithful mirror -- it is a stale one. This script therefore builds a
``PUSH_CONTEXT``-equivalent every poll out of `virtual_simfw`'s own
telemetry (the very same physical facts a real ESP would report: per-zone
measured temperature, which zones are active, which relays are commanded,
per-zone CT amps) and hands it to ``dut_core.exe``, which runs the **real**
``context_reduce_zones()``/``current_any_present()`` helpers from
``firmware/SaftyFW/src/snapshots.h`` over it. See ``_FixtureContext`` below
for the per-field sourcing, including the one field the fixture genuinely
has no producer for.

What this script still does NOT do (see README.md for the full, honest
list):
  - It does not know any zone's **setpoint**. There is no ESP and no PID in
    this fixture; nothing anywhere in `kilnsim`/`virtual_simfw` carries a
    setpoint (the scenarios' ``dut: {profile: ...}`` key names a KilnFW
    profile that nothing here executes). ``setpoint_c`` is therefore sent as
    NaN -- "unknown", never a guessed number -- which makes S2's own
    ``tc_c > max_zone_setpoint_c + margin`` test false rather than inventing
    a ceiling for S2 to trip on. S2 is reachable on the real target after
    ``f304392``; it is simply not provokable by this fixture.
  - It does not know any zone's PID. Nothing here decides *when* a zone
    relay should be commanded on; a scenario that wants one on says so
    explicitly (see ``operator_actions`` below).

Operator actions (added 2026-08-20, this pass)
----------------------------------------------
The bullet that used to sit here -- "it does not issue
``SAFETY_CMD_REQUEST_ENABLE`` ... so K4 still never closes" -- is now
closed. A scenario may carry an ``operator_actions:`` list under its own
``dut:`` mapping, and this script replays it against the sim clock:

    dut:
      profile: cone6_fast
      operator_actions:
        - { at_sim_time: 5, action: request_enable }
        - { at_sim_time: 5, action: command_relay, relay: K1, state: closed }

Both action kinds model something a real system genuinely does, and neither
invents DUT behavior:

``request_enable``
    ``dut_core.exe``'s ``ENABLE`` command, which is link_task.c's
    ``SAFETY_CMD_REQUEST_ENABLE`` (0x02) decoder ->
    ``safety_core_request_enable()`` -> ``relay_owner_command_energize()``.
    On real hardware this is an operator/PC action (PcTools'
    ``safety_request_enable`` MCP tool); KilnFW does *not* auto-enable on
    profile start, which is exactly why it had to become a scenario-authored
    step rather than something this harness performs on its own. The request
    is issued once and then honoured by relay_owner's own state machine:
    refused while TRIPPED, held-but-not-applied during the 60 s startup
    GRACE, applied the moment it reaches ARMED. So K4 cannot close earlier
    than sim-time 60 s no matter when the scenario asks -- that delay is the
    real ``SAFTYFW_STARTUP_GRACE_MS``, not a harness fudge.

``command_relay``
    ``SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE`` on K1/K2/K3/K5 -- the same
    virtual-only command this file already uses to feed K4's contact state
    back into the fixture, applied to a zone relay instead. On real hardware
    KilnFW's PID drives these; there is no KilnFW in this fixture, so the
    scenario plays that role explicitly. This is honest rather than
    circular: ``_FixtureContext`` derives ``relay_now_mask`` /
    ``relay_recent_mask`` / ``relay_on_continuous_ms`` from the fixture's own
    telemetry ``relay_state_mask``, and ``virtual_simfw``'s ported
    ``device_tick()`` derives each zone's duty (and therefore its CT current)
    from that same sensed contact -- so one command moves the physics and the
    reported context together, the way a real closed contactor does. K4 is
    refused here on purpose: it is owned by the ``energized`` feedback loop
    below and must never be forced by a scenario.

Together these are what make S3 and S4 provokable at all: both need
``any_current_present`` (S3 positively, S4 by its absence), current flows
only when a zone relay is closed *and* K4 permits, and K4 closes only after
an enable request survives GRACE.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]  # firmware/SimFW/tools/virtual_dut -> repo root
PCTOOLS_SRC = REPO_ROOT / "tools" / "PcTools" / "src"
sys.path.insert(0, str(PCTOOLS_SRC))

from kilnsim import runner as kr  # noqa: E402  (path insert must run first)
from kilnsim.link import TcpSimLink  # noqa: E402
from kilnsim.protocol import CommandGroup, Event, EventType, IoCmd, ModelCmd, SysCmd, TcCmd  # noqa: E402
from kilnsim.report import evaluate_expectations  # noqa: E402
from kilnsim.scenario import Scenario, compile_faults, load_scenario  # noqa: E402

VIRTUAL_SIMFW_EXE = REPO_ROOT / "firmware" / "SimFW" / "tools" / "virtual_simfw" / "build" / "virtual_simfw.exe"
DUT_CORE_EXE = HERE / "dut_core" / "build" / "dut_core.exe"

# TC_FAULT_CHANNEL_SAFETY (firmware/SimFW/src/sim/tc_fault_state.h) -- the
# one safety-side MAX31856 channel behind J7/spi_emu_b, index 3 of 4.
TC_CHANNEL_SAFETY = 3

# safety_core.c's own #define, SAFTYFW_PERIOD_SAFETY_CORE_MS -- see
# dut_core/main.c's header comment for why this is hand-copied rather than
# imported from anywhere: there is nothing machine-readable to import it
# from on the Python side either.
SAFETY_CORE_TICK_S = 0.1

# Guard name (SAFETY_MODEL.md/safety_guards.h) for each safety_trip_t numeric
# reason -- ported directly from safety_guards.h's enum comments, used only
# to LABEL synthetic guard_trip events (payload {"guard": "S7"}), never to
# decide anything.
_TRIP_REASON_TO_GUARD = {
    1: "S1", 2: "S2", 3: "S3", 5: "S5", 6: "S6a", 7: "S6b", 8: "S7",
    9: "S8", 10: "S9", 12: "S11", 13: "S12", 14: "S13",
}

# --- K4-feedback loop closure -------------------------------------------------
# This is the piece the README's "Known limitation: the K4 physical loop is
# not closed" section (written before this pass) said could not be done
# without editing virtual_simfw -- which was correctly out of scope for the
# earlier pass. This pass (a different task) DOES own virtual_simfw.c and
# added SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE (RELAY group, cmd id 0xF0) there:
# a virtual-only command, deliberately absent from PROTOCOL.md/cmd_ids.h/
# kilnsim.protocol/kilnsim.payloads (see virtual_simfw.c's RELAY-group
# comment for exactly why it must never become a real command). Because it
# has no entry in kilnsim.payloads' per-group encoder tables, it cannot be
# sent through TcpSimLink.send_command() -- _send_relay_set_sense() below
# reuses that same object's own request/reply/retry primitives
# (_link/_pending/_write_frame/_wait_for_reply) with a hand-built payload
# instead, exactly the pattern tools/PcTools/tests/test_kilnsim_virtual_simfw.py's
# _send_raw_relay_command() helper uses for the same reason.
_RELAY_SET_SENSE_CMD = 0xF0
_RELAY_SIGNAL_K1, _RELAY_SIGNAL_K2, _RELAY_SIGNAL_K3, _RELAY_SIGNAL_K5, _RELAY_SIGNAL_K4 = range(5)


def _send_relay_set_sense(link: TcpSimLink, signal: int, level: bool) -> int:
    """Sends SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE(signal, level) and returns the
    reply's status byte (0 == SIMFW_CMD_STATUS_OK). See the module-level
    comment above for why this bypasses kilnsim.payloads entirely."""
    from kilnsim import benchproto_codec as bp

    with link._send_lock:  # noqa: SLF001 - intentional low-level reuse, see comment above
        msg_index = link._link.next_msg_index()  # noqa: SLF001
        link._pending.begin(dst_device=1, dst_task=int(CommandGroup.RELAY), msg_index=msg_index)  # noqa: SLF001
        frame = bp.Frame(
            msg_type=bp.MsgType.DATA, msg_index=msg_index, src_device=0, src_task=0,
            dst_device=1, dst_task=int(CommandGroup.RELAY),
            payload=bytes([_RELAY_SET_SENSE_CMD, signal, 1 if level else 0]),
        )
        try:
            while True:
                with link._reply_cv:  # noqa: SLF001
                    link._reply_frame = None  # noqa: SLF001
                link._write_frame(frame)  # noqa: SLF001
                reply = link._wait_for_reply(2.0)  # noqa: SLF001
                if reply is not None:
                    break
                if not link._pending.note_retry():  # noqa: SLF001
                    raise TimeoutError("no reply to RELAY_SET_SENSE")
                frame.msg_index = link._pending.msg_index  # noqa: SLF001
        finally:
            link._pending.clear()  # noqa: SLF001
    if reply.msg_type != bp.MsgType.ACK or not reply.payload:
        raise RuntimeError(f"RELAY_SET_SENSE: unexpected reply {reply.msg_type}")
    return reply.payload[0]


_RELAY_SIGNAL_BY_NAME = {
    "K1": _RELAY_SIGNAL_K1, "K2": _RELAY_SIGNAL_K2,
    "K3": _RELAY_SIGNAL_K3, "K5": _RELAY_SIGNAL_K5,
}

# Trailing sim-seconds kept running after the last scheduled operator action's
# own deadline, same role kilnsim.runner's _TRAILING_MARGIN_S plays for
# fault-driven scenarios (that estimator only knows about faults, so a
# scenario whose interesting moment is an operator action would otherwise be
# cut off at the 45 s floor).
_ACTION_TRAILING_MARGIN_S = 20.0
_ACTION_MAX_RUN_DURATION_S = 600.0  # same ceiling kilnsim.runner clamps to


def _parse_operator_actions(scenario: Scenario) -> list:
    """Reads the scenario's ``dut.operator_actions:`` list (see the module
    docstring). Returns a list of ``{"t": float, ...}`` dicts sorted by
    sim-time. Unknown/invalid entries raise rather than being skipped: a
    silently-ignored action is exactly the "dead test that looks like a real
    one" failure mode `firmware/SimFW/scenarios/welded_ssr_midfire.yaml`'s
    header comment warns about for missing `params:`."""
    dut_raw = (scenario.raw or {}).get("dut") or {}
    raw_actions = dut_raw.get("operator_actions") or []
    if not isinstance(raw_actions, list):
        raise ValueError("dut.operator_actions must be a list")
    out = []
    for entry in raw_actions:
        if not isinstance(entry, dict):
            raise ValueError(f"dut.operator_actions entry must be a mapping, got {entry!r}")
        if "at_sim_time" not in entry:
            raise ValueError(f"dut.operator_actions entry needs 'at_sim_time': {entry!r}")
        t = float(entry["at_sim_time"])
        kind = entry.get("action")
        if kind == "request_enable":
            out.append({"t": t, "kind": kind, "enable": bool(entry.get("enable", True))})
        elif kind == "command_relay":
            name = str(entry.get("relay", ""))
            if name not in _RELAY_SIGNAL_BY_NAME:
                raise ValueError(
                    f"dut.operator_actions: relay {name!r} is not commandable by a scenario "
                    f"(valid: {sorted(_RELAY_SIGNAL_BY_NAME)}; K4 is owned by the energized "
                    f"feedback loop and must never be forced)"
                )
            state = str(entry.get("state", "closed")).lower()
            if state not in ("closed", "open"):
                raise ValueError(f"dut.operator_actions: relay state must be closed/open, got {state!r}")
            out.append({"t": t, "kind": kind, "signal": _RELAY_SIGNAL_BY_NAME[name],
                        "name": name, "closed": state == "closed"})
        else:
            raise ValueError(
                f"dut.operator_actions: unknown action {kind!r} "
                f"(valid: request_enable, command_relay)"
            )
    out.sort(key=lambda a: a["t"])
    return out


def _run_duration_with_actions(scenario: Scenario, actions: list) -> float:
    """kilnsim.runner.estimate_run_duration_s() knows only about faults (its
    own docstring: "computed from ... every fault's trigger time"), so a
    scenario whose latest interesting moment is an operator action -- an
    enable at t=5 whose K4 closure cannot happen before the real 60 s
    SAFTYFW_STARTUP_GRACE_MS, then a 150 s correlation window on top -- would
    be stopped at that estimator's 45 s floor. Extend it by the same shape
    the estimator itself uses: latest scheduled moment + the longest
    ``within_s`` deadline any clause hangs off it + a trailing margin,
    clamped to the same 600 s ceiling."""
    base = kr.estimate_run_duration_s(scenario)
    if not actions:
        return base
    latest_action_s = max(a["t"] for a in actions)
    deadline_tail = 0.0
    for e in scenario.expect:
        then = getattr(e, "then", None)
        if isinstance(then, dict) and then.get("within_s") is not None:
            deadline_tail = max(deadline_tail, float(then["within_s"]))
    want = latest_action_s + deadline_tail + _ACTION_TRAILING_MARGIN_S
    return min(_ACTION_MAX_RUN_DURATION_S, max(base, want))


class DutCore:
    """Thin subprocess wrapper around dut_core.exe's line protocol (see
    dut_core/main.c's header comment for the exact protocol)."""

    def __init__(self, exe_path: Path):
        self.proc = subprocess.Popen(
            [str(exe_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self._readline()  # consume the boot-time RESET's "OK"

    def _send(self, line: str) -> str:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        return self._readline()

    def _readline(self) -> str:
        assert self.proc.stdout is not None
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("dut_core.exe exited unexpectedly")
        return line.strip()

    def reset(self) -> None:
        reply = self._send("RESET")
        if reply != "OK":
            raise RuntimeError(f"dut_core RESET failed: {reply!r}")

    def enable(self, enable: bool) -> bool:
        """Mirrors link_task.c's SAFETY_CMD_REQUEST_ENABLE (0x02) decoder ->
        safety_core_request_enable() -> relay_owner_command_energize(). No
        scenario calls this today -- see the module docstring for why (the
        real caller is an operator/PC command, not an automatic KilnFW
        action) -- but the path exists so one can."""
        reply = self._send(f"ENABLE {1 if enable else 0}")
        if not reply.startswith("OK "):
            raise RuntimeError(f"dut_core ENABLE failed: {reply!r}")
        return reply.split()[1] == "1"

    def tick(self, regs: bytes, estop: bool, ctx: dict) -> dict:
        hex_regs = regs.hex()
        zone_args = "".join(
            f" {z['flags']} {z['setpoint_c']!r} {z['measured_c']!r} {z['sample_counter']}"
            for z in ctx["zones"]
        )
        line = (
            f"TICK {hex_regs} {1 if estop else 0}"
            f" {1 if ctx['link_up'] else 0}"
            f" {1 if ctx['ctx_present'] else 0}"
            f" {1 if ctx['ctx_degraded'] else 0}"
            f" {ctx['ctx_age_ms']}"
            f" {ctx['relay_now_mask']} {ctx['relay_recent_mask']}"
            f" {ctx['relay_on_continuous_ms']}"
            f" {ctx['amps'][0]!r} {ctx['amps'][1]!r} {ctx['amps'][2]!r}"
            f" {len(ctx['zones'])}{zone_args}"
        )
        reply = self._send(line)
        parts = reply.split()
        if len(parts) != 19:
            raise RuntimeError(f"dut_core TICK: malformed reply {reply!r}")
        return {
            "is_tripped": parts[0] == "1",
            "reason": int(parts[1]),
            "s5_warn": parts[2] == "1",
            "s12_warn": parts[3] == "1",
            "s4_warn": parts[4] == "1",
            "s10_warn": parts[5] == "1",
            "s13_warn": parts[6] == "1",
            "trip_ineffective": parts[7] == "1",
            "relay_state": int(parts[8]),
            "energized": parts[9] == "1",
            "deciding_threshold_c": float(parts[10]),
            "tc_valid": parts[11] == "1",
            "tc_c": float(parts[12]),
            "cj_c": float(parts[13]),
            "fault_bits": int(parts[14]),
            # Derived by dut_core.exe using the REAL snapshots.h helpers --
            # echoed back so guard-reachability can be reported without
            # re-deriving any of it in Python.
            "context_valid": parts[15] == "1",
            "eff_zone_count": int(parts[16]),
            "any_current_present": parts[17] == "1",
            "link_up": parts[18] == "1",
        }

    def close(self) -> None:
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
        self.proc.wait(timeout=5)


def start_virtual_simfw(seed: int) -> subprocess.Popen:
    proc = subprocess.Popen(
        [str(VIRTUAL_SIMFW_EXE), "--port", "0", "--seed", str(seed)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
    )
    assert proc.stdout is not None
    deadline = time.time() + 10.0
    port = None
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        m = re.search(r"VIRTUAL_SIMFW_LISTENING port=(\d+)", line)
        if m:
            port = int(m.group(1))
            break
    if port is None:
        proc.kill()
        raise RuntimeError("virtual_simfw.exe did not report a listening port")
    return proc, port


class _FixtureContext:
    """Builds the `SAFETY_CMD_PUSH_CONTEXT`-equivalent facts `dut_core.exe`
    needs, out of `virtual_simfw`'s telemetry.

    This is not "inventing context": every field below is a physical fact the
    fixture already reports, mapped onto the wire field a real ESP would
    carry it in (`CommonFW/docs/LINK_PROTOCOL.md` sec 4 /
    `firmware/SaftyFW/src/snapshots.h`'s `context_snapshot_t`). The one field
    with no fixture producer at all (`setpoint_c`) is sent as NaN rather than
    guessed -- see the module docstring.

    The *reductions* over these facts (`context_reduce_zones()`,
    `current_any_present()`, the `CONTEXT_MAX_AGE_MS` staleness test, the
    `correlation_window_s` comparison) are deliberately NOT done here: they
    are done in `dut_core.exe`, by the real, unmodified SaftyFW code, exactly
    as `safety_core_build_input()` does them.
    """

    # snapshots.h's CONTEXT_ZONE_FLAG_* (per-zone flags byte).
    ZONE_FLAG_MEASURED_VALID = 0x01
    ZONE_FLAG_ACTIVE = 0x02
    ZONE_FLAG_RELAY_ON = 0x04

    # link_task.c's LINK_UP_RECENCY_MS: link_up is "a CRC-valid frame within
    # the last second". The fixture's stand-in for "a frame arrived" is "the
    # telemetry stream advanced". Both this and CONTEXT_MAX_AGE_MS are
    # measured in WALL milliseconds, not sim milliseconds: on the real Pico
    # they are `to_ms_since_boot()`/tick quantities compared against a real
    # ESP poll period (~200 ms), and the fixture's telemetry broadcast is
    # likewise a real-time stream (~2 Hz here) whose rate does not scale with
    # `timescale`. Scaling them by timescale would make a healthy 2 Hz
    # broadcast read as a dead link at timescale 10 purely as an artifact of
    # the run being compressed.
    LINK_UP_RECENCY_MS = 1000

    # How far back `relay_recent_mask` ORs. SAFETY_MODEL.md sec 4 S3 wants
    # "anything commanded on in the last correlation_window_s"; 150s is
    # safety_guards.c's own CORRELATION_WINDOW_S_DEFAULT, the same number
    # safety_core.c substitutes when cfg->correlation_window_s is 0.
    RECENT_WINDOW_S = 150.0

    def __init__(self):
        self.prev_sim_us: Optional[int] = None
        self.last_advance_wall: Optional[float] = None
        self.relay_history: list = []  # [(sim_us, mask)]
        self.on_since_us: Optional[int] = None

    def observe(self, telemetry: dict) -> dict:
        sim_us = int(telemetry.get("sim_time_us", 0))
        now_wall = time.time()
        advanced = self.prev_sim_us is None or sim_us > self.prev_sim_us
        if advanced:
            self.last_advance_wall = now_wall
        self.prev_sim_us = sim_us

        # Age in WALL ms since the telemetry stream last advanced (see
        # LINK_UP_RECENCY_MS above for why wall and not sim). A stalled/dead
        # fixture stream is exactly what a stalled/dead ESP link looks like
        # from safety_core's side, and it ages out through the same
        # CONTEXT_MAX_AGE_MS / LINK_UP_RECENCY_MS tests dut_core.exe applies.
        stall_wall_s = 0.0 if self.last_advance_wall is None else (now_wall - self.last_advance_wall)
        age_ms = int(stall_wall_s * 1000.0)

        # Zone relays K1/K2/K3 -> context relay bits 0..2 (LINK_PROTOCOL.md
        # sec 4's "relays 1-4, as actually commanded"). kilnsim's own bit
        # order is K1,K2,K3,K5,K4 (`sim_snapshot.h`'s sim_relay_bit_t), so
        # masking the low three bits is the K1..K3 set and nothing else --
        # K5 (fixture DUT power) and K4 (the safety pilot the DUT itself
        # owns) are deliberately not reported to the DUT as zone relays.
        relay_mask = int(telemetry.get("relay_state_mask", 0)) & 0x07

        self.relay_history.append((sim_us, relay_mask))
        cutoff_us = sim_us - int(self.RECENT_WINDOW_S * 1_000_000)
        self.relay_history = [(t, m) for (t, m) in self.relay_history if t >= cutoff_us]
        recent_mask = 0
        for _, m in self.relay_history:
            recent_mask |= m

        if relay_mask != 0:
            if self.on_since_us is None:
                self.on_since_us = sim_us
        else:
            self.on_since_us = None
        relay_on_ms = 0 if self.on_since_us is None else max(0, (sim_us - self.on_since_us) // 1000)

        zones = []
        amps = [0.0, 0.0, 0.0]
        for i, z in enumerate(telemetry.get("zones", [])[:3]):
            measured = float(z.get("t_tc_reported", float("nan")))
            flags = self.ZONE_FLAG_ACTIVE  # every zone the fixture models is part of the firing
            if math.isfinite(measured):
                flags |= self.ZONE_FLAG_MEASURED_VALID
            if relay_mask & (1 << i):
                flags |= self.ZONE_FLAG_RELAY_ON
            zones.append({
                "flags": flags,
                # No producer anywhere in this fixture -- NaN means unknown.
                "setpoint_c": float("nan"),
                "measured_c": measured,
                # S13 is deliberately dormant in safety_core.c too (no
                # commissioned borrowed_zone_index exists); dut_core.exe
                # hardcodes sample_counter_advancing=false to match, so this
                # field is carried for struct fidelity and read by nothing.
                "sample_counter": 0,
            })
            amps[i] = float(z.get("i_amps", 0.0))

        return {
            "link_up": age_ms < self.LINK_UP_RECENCY_MS and self.last_advance_wall is not None,
            "ctx_present": self.last_advance_wall is not None,
            "ctx_degraded": False,  # no version-mismatch path exists in this fixture
            "ctx_age_ms": age_ms,
            "relay_now_mask": relay_mask,
            "relay_recent_mask": recent_mask,
            "relay_on_continuous_ms": relay_on_ms,
            "amps": amps,
            "zones": zones,
        }


class _GuardEdgeTracker:
    """Turns dut_core.exe's per-tick state dict into synthetic
    guard_warn/guard_trip/K4 entity-state Events, one per edge (level
    changes only -- report.py's _find_events/_entity_state_events both work
    on the sequence of occurrences, not a level sampled every tick)."""

    def __init__(self):
        self.next_seq = 2_000_000_000  # disjoint from kilnsim.runner's own
                                         # _SYNTHETIC_SEQ_BASE (1_000_000_000)
                                         # so the two synthetic streams can
                                         # never collide; both are outside
                                         # WIRE_EVENT_TYPES so neither
                                         # participates in report.py's
                                         # event_seq_gap check either way.
        self.was_tripped = False
        self.warn_state = {"S5": False, "S12": False, "S4": False, "S10": False, "S13": False}
        self.last_k4_open: Optional[bool] = None  # None until first tick observed

    def _mk(self, sim_time_us: int, event_type: EventType, payload: dict) -> Event:
        seq = self.next_seq
        self.next_seq += 1
        return Event(seq=seq, sim_time_us=sim_time_us, event_type=event_type, payload=payload)

    def observe(self, sim_time_us: int, tick_result: dict) -> list:
        out = []

        if tick_result["is_tripped"] and not self.was_tripped:
            guard = _TRIP_REASON_TO_GUARD.get(tick_result["reason"], f"reason{tick_result['reason']}")
            out.append(self._mk(sim_time_us, EventType.GUARD_TRIP, {"guard": guard}))
        self.was_tripped = tick_result["is_tripped"]

        for key, guard in (("s5_warn", "S5"), ("s12_warn", "S12"), ("s4_warn", "S4"),
                            ("s10_warn", "S10"), ("s13_warn", "S13")):
            now = bool(tick_result[key])
            if now and not self.warn_state[guard]:
                out.append(self._mk(sim_time_us, EventType.GUARD_WARN, {"guard": guard}))
            self.warn_state[guard] = now

        # K4 "open" == NOT energized -- same polarity convention
        # kilnsim.runner._relay_wire_payload uses for the fixture's own
        # relay-edge translation (state=True means the scenario's positive
        # word, e.g. K4_open). relay_state==3 (TRIPPED) or plain
        # not-energized both mean "open" here; see README.md for why
        # `energized` in current SaftyFW never actually becomes true.
        k4_open = not tick_result["energized"]
        if self.last_k4_open is None or k4_open != self.last_k4_open:
            out.append(self._mk(sim_time_us, EventType.SIM_CLOCK_MARK, {"entity": "K4", "state": k4_open}))
        self.last_k4_open = k4_open

        if tick_result["trip_ineffective"] and "trip_ineffective_latched_emitted" not in self.__dict__:
            self.trip_ineffective_latched_emitted = True
            out.append(self._mk(sim_time_us, EventType.TRIP_INEFFECTIVE_LATCHED, {}))

        return out


def run_one_scenario(scenario: Scenario, seed_override: Optional[int], timescale_override: Optional[float],
                      poll_interval_s: float, trace: bool = False) -> dict:
    sim_proc, port = start_virtual_simfw(seed_override if seed_override is not None else scenario.seed)
    dut = DutCore(DUT_CORE_EXE)
    link = TcpSimLink()
    try:
        link.connect(f"127.0.0.1:{port}")

        run_seed = seed_override if seed_override is not None else scenario.seed
        run_timescale = timescale_override if timescale_override is not None else scenario.timescale
        actions = _parse_operator_actions(scenario)
        run_duration = _run_duration_with_actions(scenario, actions)

        link.send_command(CommandGroup.SYS, SysCmd.SET_SEED, {"value": run_seed})
        link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": run_timescale})
        if scenario.preset:
            link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": scenario.preset})
        kr._apply_overrides(link, scenario.overrides)

        slot_to_fault_id = {c.fault_slot: scenario.faults[c.fault_slot].id for c in compile_faults(scenario)}
        for compiled in compile_faults(scenario):
            link.send_command(CommandGroup.FAULT, 1, {  # FaultCmd.SCHEDULE
                "fault_slot": compiled.fault_slot, "fault_type": compiled.fault_type,
                "target": compiled.target, "trigger": kr._trigger_payload(compiled.trigger),
                "duration": kr._duration_payload(compiled.duration),
                "repeat": kr._repeat_payload(compiled.repeat), "params": list(compiled.params),
            })
            from kilnsim.protocol import DurationKind
            if compiled.duration.kind == DurationKind.UNTIL_TRIGGER:
                link.send_command(CommandGroup.FAULT, 5, {  # FaultCmd.SET_UNTIL_TRIGGER
                    "fault_slot": compiled.fault_slot, "trigger": kr._trigger_payload(compiled.duration.until),
                })

        collected: list = []
        wire_tracker = kr._TelemetryEdgeTracker()
        guard_tracker = _GuardEdgeTracker()
        fixture_ctx = _FixtureContext()
        reach = {"polls": 0, "context_valid": 0, "any_current_present": 0,
                 "link_up": 0, "max_zone_count": 0, "energized": 0}
        pending_actions = list(actions)  # sorted by sim-time, consumed below

        last_sim_time_us = 0
        start_wall = time.time()
        wall_budget = run_duration / max(run_timescale, 1e-6) + 5.0
        last_dut_poll = 0.0

        while time.time() - start_wall < wall_budget:
            for e in link.read_events(timeout=poll_interval_s):
                collected.append(kr._translate_wire_event(e, slot_to_fault_id))

            telemetry = link.get_last_telemetry()
            now = time.time()
            if telemetry and now - last_dut_poll >= poll_interval_s:
                last_dut_poll = now
                collected.extend(wire_tracker.observe(telemetry))

                sim_time_us = int(telemetry.get("sim_time_us", 0))
                estop = bool(telemetry.get("estop_open", False))

                # --- Scheduled operator actions (see the module docstring) --
                # Fired against the SIM clock, at this poll's resolution: an
                # action nominally at t=5 actually lands at the first poll
                # whose telemetry reports sim_time >= 5. That quantization is
                # bounded by virtual_simfw's own ~2 Hz telemetry broadcast
                # (5 sim-seconds at timescale 10), the same coarseness every
                # other fixture-observed quantity in this loop already has --
                # so scenario deadlines are written with margin for it rather
                # than to the nominal instant.
                sim_time_s = sim_time_us / 1_000_000.0
                while pending_actions and pending_actions[0]["t"] <= sim_time_s:
                    act = pending_actions.pop(0)
                    if act["kind"] == "request_enable":
                        dut.enable(act["enable"])
                    else:
                        _send_relay_set_sense(link, act["signal"], act["closed"])

                reg_reply = link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": TC_CHANNEL_SAFETY})
                regs = bytes(reg_reply.get("regs", bytes(16)))

                # Batch-tick dut_core to cover the elapsed sim time in real
                # SAFTYFW_PERIOD_SAFETY_CORE_MS (100ms) steps -- see
                # run_dut_scenarios.py's module docstring / README.md for
                # why this is a documented approximation (the same TC
                # reading is replayed for every step in one batch) rather
                # than a per-100ms live poll. The context/current/link facts
                # built from this same telemetry sample are replayed across
                # the batch for exactly the same reason and with exactly the
                # same documented consequence (coarser time resolution on a
                # changing input, never a change to any guard's own logic).
                ctx_facts = fixture_ctx.observe(telemetry)
                step_us = int(SAFETY_CORE_TICK_S * 1_000_000)
                n_steps = max(1, (sim_time_us - last_sim_time_us) // step_us) if sim_time_us > last_sim_time_us else 1
                n_steps = min(n_steps, 20_000)  # sanity cap
                result = None
                for i in range(n_steps):
                    result = dut.tick(regs, estop, ctx_facts)
                last_sim_time_us = max(sim_time_us, last_sim_time_us)
                if result is not None:
                    # Guard-reachability accounting: how many polls actually
                    # presented each context/current/link precondition to the
                    # real guard code. Printed per scenario (never used to
                    # decide any verdict) so "S3 did not fire" can be read as
                    # "S3 was evaluated and stayed quiet" vs "S3 was gated
                    # off" without re-deriving anything in Python.
                    reach["polls"] += 1
                    for key in ("context_valid", "any_current_present", "link_up", "energized"):
                        if result[key]:
                            reach[key] += 1
                    reach["max_zone_count"] = max(reach["max_zone_count"], result["eff_zone_count"])
                    if trace:
                        # Per-poll diagnostic for exactly the failure mode
                        # README.md warns about: one poll batching many
                        # 100ms guard ticks against a single telemetry
                        # sample. `steps` is how many ticks this poll
                        # replayed; a large value next to a short guard
                        # window (S3's 20s) is the signal that a verdict
                        # needs a slower timescale before it is believed.
                        print(f"    trace t={sim_time_us/1e6:8.2f}s steps={n_steps:5d} "
                              f"relay_now={ctx_facts['relay_now_mask']} "
                              f"on_ms={ctx_facts['relay_on_continuous_ms']:6d} "
                              f"amps={ctx_facts['amps'][0]:6.2f} "
                              f"cur={int(result['any_current_present'])} "
                              f"en={int(result['energized'])} "
                              f"rs={result['relay_state']} "
                              f"trip={int(result['is_tripped'])}/{result['reason']} "
                              f"s4={int(result['s4_warn'])}")
                    collected.extend(guard_tracker.observe(sim_time_us, result))
                    # Close the loop: feed relay_owner_task()'s real,
                    # unmodified decision (dut_core's `energized`, straight
                    # from relay_grace.c/safety_guards.c -- not re-derived
                    # here) back into the fixture as K4's sensed contact
                    # state, exactly what a real K4 pilot relay's physical
                    # contact would report to the fixture if this were real
                    # hardware. See the module-level comment above
                    # _send_relay_set_sense() for why this uses a virtual-
                    # only command rather than anything in kilnsim.protocol.
                    _send_relay_set_sense(link, _RELAY_SIGNAL_K4, bool(result["energized"]))

        for e in link.read_events(timeout=0.0):
            collected.append(kr._translate_wire_event(e, slot_to_fault_id))

        report = evaluate_expectations(
            scenario, collected, spi_underrun=False,
            seed=run_seed, timescale=run_timescale, start_time=start_wall,
        )
        out = report.to_dict()
        out["dut_input_reachability"] = dict(reach)
        return out
    finally:
        link.disconnect()
        dut.close()
        sim_proc.terminate()
        try:
            sim_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            sim_proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scenarios", nargs="*", help="scenario YAML paths (default: all of firmware/SimFW/scenarios)")
    ap.add_argument("--poll-interval", type=float, default=0.25)
    ap.add_argument("--timescale", type=float, default=None,
                    help="override every scenario's own timescale -- lower it to shrink how much "
                         "sim time one poll's tick batch covers (see --trace)")
    ap.add_argument("--trace", action="store_true",
                    help="print one diagnostic line per poll (sim time, batch size, relay/current/"
                         "guard state) -- use when a trip's timing looks like a batching artifact")
    ap.add_argument("--out", type=Path, default=HERE / "results")
    args = ap.parse_args()

    if not VIRTUAL_SIMFW_EXE.exists():
        print(f"ERROR: {VIRTUAL_SIMFW_EXE} not found -- build it first (virtual_simfw/build_host.ps1)", file=sys.stderr)
        return 2
    if not DUT_CORE_EXE.exists():
        print(f"ERROR: {DUT_CORE_EXE} not found -- build it first (dut_core/build_host.ps1)", file=sys.stderr)
        return 2

    scenario_paths = [Path(p) for p in args.scenarios] or sorted(
        (REPO_ROOT / "firmware" / "SimFW" / "scenarios").glob("*.yaml")
    )
    args.out.mkdir(parents=True, exist_ok=True)

    overall_ok = True
    for path in scenario_paths:
        scenario = load_scenario(path)
        print(f"=== {scenario.name} ===")
        try:
            report = run_one_scenario(scenario, None, args.timescale, args.poll_interval, args.trace)
        except Exception as exc:  # noqa: BLE001
            print(f"  ERROR running scenario: {exc}")
            overall_ok = False
            continue
        out_path = args.out / f"{scenario.name}.json"
        out_path.write_text(json.dumps(report, indent=2))
        print(f"  verdict: {report['verdict']}  (report: {out_path})")
        r = report.get("dut_input_reachability", {})
        if r.get("polls"):
            print(f"    inputs: {r['polls']} polls, context_valid {r['context_valid']}, "
                  f"link_up {r['link_up']}, any_current_present {r['any_current_present']}, "
                  f"K4 energized {r.get('energized', 0)}, "
                  f"max eligible zones {r['max_zone_count']}")
        for exp in report["expectations"]:
            print(f"    [{exp['verdict']:>7}] {exp['name']}: {exp['detail']}")
        if report["verdict"] == "FAIL":
            overall_ok = False

    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
