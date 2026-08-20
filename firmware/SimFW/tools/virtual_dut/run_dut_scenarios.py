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

What this script still does NOT do (see README.md for the full, honest
list):
  - It does NOT invent context (context_valid stays false, matching
    safety_core_build_input()'s real current struct literal), so S2/S3/S4/
    S9/S10/S13 correctly never fire here -- not a gap in this script, a
    faithfully-reproduced gap in current SaftyFW (see README.md).
"""
from __future__ import annotations

import argparse
import json
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

    def tick(self, regs: bytes, estop: bool) -> dict:
        hex_regs = regs.hex()
        reply = self._send(f"TICK {hex_regs} {1 if estop else 0}")
        parts = reply.split()
        if len(parts) != 15:
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
                      poll_interval_s: float) -> dict:
    sim_proc, port = start_virtual_simfw(seed_override if seed_override is not None else scenario.seed)
    dut = DutCore(DUT_CORE_EXE)
    link = TcpSimLink()
    try:
        link.connect(f"127.0.0.1:{port}")

        run_seed = seed_override if seed_override is not None else scenario.seed
        run_timescale = timescale_override if timescale_override is not None else scenario.timescale
        run_duration = kr.estimate_run_duration_s(scenario)

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
                reg_reply = link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": TC_CHANNEL_SAFETY})
                regs = bytes(reg_reply.get("regs", bytes(16)))

                # Batch-tick dut_core to cover the elapsed sim time in real
                # SAFTYFW_PERIOD_SAFETY_CORE_MS (100ms) steps -- see
                # run_dut_scenarios.py's module docstring / README.md for
                # why this is a documented approximation (the same TC
                # reading is replayed for every step in one batch) rather
                # than a per-100ms live poll.
                step_us = int(SAFETY_CORE_TICK_S * 1_000_000)
                n_steps = max(1, (sim_time_us - last_sim_time_us) // step_us) if sim_time_us > last_sim_time_us else 1
                n_steps = min(n_steps, 20_000)  # sanity cap
                result = None
                for i in range(n_steps):
                    result = dut.tick(regs, estop)
                last_sim_time_us = max(sim_time_us, last_sim_time_us)
                if result is not None:
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
        return report.to_dict()
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
            report = run_one_scenario(scenario, None, None, args.poll_interval)
        except Exception as exc:  # noqa: BLE001
            print(f"  ERROR running scenario: {exc}")
            overall_ok = False
            continue
        out_path = args.out / f"{scenario.name}.json"
        out_path.write_text(json.dumps(report, indent=2))
        print(f"  verdict: {report['verdict']}  (report: {out_path})")
        for exp in report["expectations"]:
            print(f"    [{exp['verdict']:>7}] {exp['name']}: {exp['detail']}")
        if report["verdict"] == "FAIL":
            overall_ok = False

    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
