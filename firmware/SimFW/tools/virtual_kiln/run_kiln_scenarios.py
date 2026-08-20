#!/usr/bin/env python3
"""run_kiln_scenarios.py -- closes the KilnFW-side half of the gap
virtual_dut/README.md documents for SaftyFW: virtual_simfw alone has no DUT
at all, so no relay ever actually closes and no KilnFW-side guard ever
actually trips. virtual_dut closed that gap for SaftyFW's guards
(S5/S6b/S7/S12) and K4. This script does the SAME thing for KilnFW's
control loop and its heater relays (K1/K2/K3), for exactly ONE zone.

See this directory's README.md for the full architecture and, most
importantly, for what this deliberately does NOT do: it does not run a
firing profile (ramp/dwell segment stepping, multi-zone ramp-lock,
feedforward, relay_authority, config-reload-while-running are all
profile_executor.c's job -- FreeRTOS/kiln_io/HTTP/NVS-coupled, not
host-compilable, and NOT reimplemented here, because reimplementing them
would be exactly the "second implementation" the parent task's brief
forbids). It drives ONE zone's PID + thermal_guard + heater_output (the
REAL, unmodified firmware/KilnFW/App/drivers/{pid,thermal_guard,
heater_output}.c, see kiln_core/main.c) against a FIXED setpoint supplied
on the command line, closing the loop between:

  virtual_simfw (real SimFW sim/*.c)  <-- TC_GET_REGS(channel=0) --  kiln_core.exe
                                       -- RELAY_SET_SENSE(K1) -->

exactly the same virtual-only command (SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE,
0xF0) virtual_dut/run_dut_scenarios.py already uses for K4, generalized here
to signal 0 (K1). Same reasoning as that file's own comment: a virtual DUT
has no physical relay coil to close, so there is no contact for a real
fixture-style sense wire to ever pick up, and this command is the narrowly-
scoped substitute.

Usage:
    python run_kiln_scenarios.py <scenario.yaml> [--setpoint-c 300] [--seed N]
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
REPO_ROOT = HERE.parents[3]  # firmware/SimFW/tools/virtual_kiln -> repo root
PCTOOLS_SRC = REPO_ROOT / "tools" / "PcTools" / "src"
sys.path.insert(0, str(PCTOOLS_SRC))

from kilnsim import runner as kr  # noqa: E402  (path insert must run first)
from kilnsim.link import TcpSimLink  # noqa: E402
from kilnsim.protocol import CommandGroup, Event, EventType, ModelCmd, SysCmd, TcCmd  # noqa: E402
from kilnsim.report import evaluate_expectations  # noqa: E402
from kilnsim.scenario import Scenario, compile_faults, load_scenario  # noqa: E402

VIRTUAL_SIMFW_EXE = REPO_ROOT / "firmware" / "SimFW" / "tools" / "virtual_simfw" / "build" / "virtual_simfw.exe"
KILN_CORE_EXE = HERE / "kiln_core" / "build" / "kiln_core.exe"

# Zone 0's thermocouple channel and heater relay. firmware/SimFW's fixture
# reserves TC channel 3 / signal K4 for the safety processor (see
# virtual_dut's TC_CHANNEL_SAFETY); channels 0-2 and relays K1-K3 are the
# three main-controller zones. This script only ever drives zone 0 / channel
# 0 / K1 -- see README.md for why multi-zone is out of scope for this pass.
TC_CHANNEL_ZONE0 = 0
_RELAY_SET_SENSE_CMD = 0xF0
_RELAY_SIGNAL_K1, _RELAY_SIGNAL_K2, _RELAY_SIGNAL_K3, _RELAY_SIGNAL_K5, _RELAY_SIGNAL_K4 = range(5)

# profile_executor.c's own #define, PROFILE_EXECUTOR_TICK_MS: "1 Hz per
# TODO.md 6A.7". Hand-copied for the same reason virtual_dut's
# SAFETY_CORE_TICK_S is: profile_executor.c is not host-compilable, so there
# is nothing to import the constant from.
KILN_CONTROL_TICK_S = 1.0

# MAX31856 fault-status bits that invalidate a reading (TC_INVALIDATING_FAULTS
# / profile_executor.c's own fault_bits_bad mask): OPEN | OVUV | TCRANGE.
# The regs[15] byte IS the SR register (see max31856_decode.c's REG_SR);
# sensor_ok mirrors profile_executor.c's own "!spi_failed && !isnan &&
# !fault_bits_bad" -- there is no spi_failed concept in this fixture (no
# real SPI bytes ever flow, per virtual_simfw's own README), so this reduces
# to the fault-bits check plus a sane-number check on the decoded value.
_FAULT_OPEN, _FAULT_OVUV, _FAULT_TCRANGE = 0x01, 0x02, 0x40
_TC_INVALIDATING_FAULTS = _FAULT_OPEN | _FAULT_OVUV | _FAULT_TCRANGE


def _decode_tc_c(regs: bytes) -> tuple[float, bool]:
    """Decodes LTCBH:LTCBM:LTCBL (regs[0x0C:0x0F]) into degC, byte-for-byte
    the same fixed-point conversion firmware/KilnFW/App/drivers/MAX31856.c
    and firmware/SimFW/tools/virtual_dut/dut_core/max31856_decode.c both
    use (a datasheet fact, not a control/safety decision -- see this
    script's README.md section for the cross-reference). Returns
    (tc_c, sensor_ok)."""
    raw = (regs[0x0C] << 16) | (regs[0x0D] << 8) | regs[0x0E]
    raw &= 0x00FFFFE0
    if raw & 0x00800000:
        raw -= 0x01000000
    tc_c = raw * (1.0 / 4096.0)
    sr = regs[0x0F]
    sensor_ok = (sr & _TC_INVALIDATING_FAULTS) == 0
    return tc_c, sensor_ok


def _send_relay_set_sense(link: TcpSimLink, signal: int, level: bool) -> int:
    """SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE(signal, level) -- see
    virtual_dut/run_dut_scenarios.py's identically-named helper for why this
    bypasses kilnsim.payloads (the command has no entry there by design)."""
    from kilnsim import benchproto_codec as bp

    with link._send_lock:  # noqa: SLF001 - intentional low-level reuse, see virtual_dut's own precedent
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


class KilnCore:
    """Thin subprocess wrapper around kiln_core.exe's line protocol (see
    kiln_core/main.c's header comment for the exact protocol)."""

    def __init__(self, exe_path: Path, setpoint_c: float):
        self.setpoint_c = setpoint_c
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
            raise RuntimeError("kiln_core.exe exited unexpectedly")
        return line.strip()

    def tick(self, measurement_c: float, sensor_ok: bool, dt_s: float) -> dict:
        reply = self._send(f"TICK {measurement_c:.6f} {1 if sensor_ok else 0} {self.setpoint_c:.6f} {dt_s:.6f}")
        parts = reply.split()
        if len(parts) != 8:
            raise RuntimeError(f"kiln_core TICK: malformed reply {reply!r}")
        return {
            "is_tripped": parts[0] == "1",
            "reason": int(parts[1]),
            "relay_on": parts[2] == "1",
            "duty": float(parts[3]),
            "p": float(parts[4]), "i": float(parts[5]), "d": float(parts[6]), "ff": float(parts[7]),
        }

    def close(self) -> None:
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
        self.proc.wait(timeout=5)


def start_virtual_simfw(seed: int) -> tuple[subprocess.Popen, int]:
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


class _KilnEdgeTracker:
    """Turns kiln_core.exe's per-tick state dict into a synthetic
    guard_trip Event, one per newly-tripped edge -- same "level changes
    only" discipline as virtual_dut's _GuardEdgeTracker.

    Deliberately does NOT synthesize a relay_edge/K1 event: EventType.
    RELAY_EDGE is a REAL wire event type (kilnsim.protocol.WIRE_EVENT_TYPES)
    -- virtual_simfw's device_tick() already emits a genuine RELAY_EDGE EVT
    frame the instant _send_relay_set_sense() changes K1's sensed contact
    state (same edge-log path K1/K2/K3/K5 already had, per virtual_simfw's
    own README), so that event arrives through the ordinary
    link.read_events() -> kr._translate_wire_event() path below like any
    other real wire event. Synthesizing a second, fabricated RELAY_EDGE
    alongside the real one would be redundant at best and would corrupt
    report.py's per-client wire-sequence-gap check at worst (RELAY_EDGE
    participates in that check; GUARD_TRIP/GUARD_WARN/SIM_CLOCK_MARK do not,
    which is exactly why virtual_dut only ever synthesizes the latter three,
    never RELAY_EDGE, for K4)."""

    _TRIP_REASON_NAME = {
        0: "NONE", 1: "HEATING_FAILED", 2: "WRONG_DIRECTION", 3: "RUNAWAY",
        4: "DRIFT", 5: "MAX_TEMP", 6: "MIN_TEMP", 7: "SENSOR_INVALID", 8: "FROZEN",
        9: "CROSS_ZONE",
    }

    def __init__(self):
        self.next_seq = 3_000_000_000  # disjoint from kilnsim.runner's 1e9 and virtual_dut's 2e9 bases
        self.was_tripped = False

    def _mk(self, sim_time_us: int, event_type: EventType, payload: dict) -> Event:
        seq = self.next_seq
        self.next_seq += 1
        return Event(seq=seq, sim_time_us=sim_time_us, event_type=event_type, payload=payload)

    def observe(self, sim_time_us: int, tick_result: dict) -> list:
        out = []
        if tick_result["is_tripped"] and not self.was_tripped:
            reason = self._TRIP_REASON_NAME.get(tick_result["reason"], f"reason{tick_result['reason']}")
            out.append(self._mk(sim_time_us, EventType.GUARD_TRIP, {"guard": f"KILNFW_{reason}"}))
        self.was_tripped = tick_result["is_tripped"]
        return out


def run_one_scenario(scenario: Scenario, setpoint_c: float, seed_override: Optional[int],
                      timescale_override: Optional[float], poll_interval_s: float) -> dict:
    sim_proc, port = start_virtual_simfw(seed_override if seed_override is not None else scenario.seed)
    kiln = KilnCore(KILN_CORE_EXE, setpoint_c)
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
        kiln_tracker = _KilnEdgeTracker()

        last_sim_time_us = 0
        start_wall = time.time()
        wall_budget = run_duration / max(run_timescale, 1e-6) + 5.0
        last_poll = 0.0

        while time.time() - start_wall < wall_budget:
            for e in link.read_events(timeout=poll_interval_s):
                collected.append(kr._translate_wire_event(e, slot_to_fault_id))

            telemetry = link.get_last_telemetry()
            now = time.time()
            if telemetry and now - last_poll >= poll_interval_s:
                last_poll = now
                collected.extend(wire_tracker.observe(telemetry))

                sim_time_us = int(telemetry.get("sim_time_us", 0))
                reg_reply = link.send_command(CommandGroup.TC, TcCmd.GET_REGS, {"channel": TC_CHANNEL_ZONE0})
                regs = bytes(reg_reply.get("regs", bytes(16)))
                tc_c, sensor_ok = _decode_tc_c(regs)

                # Batch-tick kiln_core to cover the elapsed sim time in real
                # PROFILE_EXECUTOR_TICK_MS (1s) steps -- same documented
                # approximation as virtual_dut's dut.tick() batching (the
                # same TC reading is replayed for every step in one batch).
                step_us = int(KILN_CONTROL_TICK_S * 1_000_000)
                n_steps = max(1, (sim_time_us - last_sim_time_us) // step_us) if sim_time_us > last_sim_time_us else 1
                n_steps = min(n_steps, 20_000)
                result = None
                for _ in range(n_steps):
                    result = kiln.tick(tc_c, sensor_ok, KILN_CONTROL_TICK_S)
                last_sim_time_us = max(sim_time_us, last_sim_time_us)
                if result is not None:
                    collected.extend(kiln_tracker.observe(sim_time_us, result))
                    # Close the loop: feed kiln_core's real, unmodified
                    # heater_output_duty() decision back into the fixture as
                    # K1's sensed contact state -- what a real element relay's
                    # physical contact would report if this were real
                    # hardware. Real firmware also gates this through
                    # relay_authority_zone_blocked() (a global safety-link
                    # fault or this run's own per-zone block); that gate is
                    # out of scope here (see README.md) so this harness's K1
                    # can close even when a real board's relay_authority
                    # would have refused it -- documented, not hidden.
                    _send_relay_set_sense(link, _RELAY_SIGNAL_K1, bool(result["relay_on"]))

        for e in link.read_events(timeout=0.0):
            collected.append(kr._translate_wire_event(e, slot_to_fault_id))

        report = evaluate_expectations(
            scenario, collected, spi_underrun=False,
            seed=run_seed, timescale=run_timescale, start_time=start_wall,
        )
        return report.to_dict()
    finally:
        link.disconnect()
        kiln.close()
        sim_proc.terminate()
        try:
            sim_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            sim_proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scenarios", nargs="*", help="scenario YAML paths (default: all of firmware/SimFW/scenarios)")
    ap.add_argument("--setpoint-c", type=float, default=1200.0,
                     help="fixed zone-0 setpoint (no profile ramp/dwell -- see README.md)")
    ap.add_argument("--poll-interval", type=float, default=0.25)
    ap.add_argument("--out", type=Path, default=HERE / "results")
    args = ap.parse_args()

    if not VIRTUAL_SIMFW_EXE.exists():
        print(f"ERROR: {VIRTUAL_SIMFW_EXE} not found -- build it first (virtual_simfw/build_host.ps1)", file=sys.stderr)
        return 2
    if not KILN_CORE_EXE.exists():
        print(f"ERROR: {KILN_CORE_EXE} not found -- build it first (kiln_core/build_host.ps1)", file=sys.stderr)
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
            report = run_one_scenario(scenario, args.setpoint_c, None, None, args.poll_interval)
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
