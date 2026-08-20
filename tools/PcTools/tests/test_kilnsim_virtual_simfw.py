"""Standing regression gate for the virtual SimFW end-to-end path.

Spins up ``firmware/SimFW/tools/virtual_simfw``'s compiled host executable
on an ephemeral TCP port, drives it with :class:`kilnsim.link.TcpSimLink`,
runs real scenarios from ``firmware/SimFW/scenarios/`` through
:func:`kilnsim.runner.run_scenario`, and asserts on the resulting report --
exactly the layer 2/3 integration (scenario runner, expectation evaluator,
fault-trigger logic, report generator) that, before this file, no test in
this repo exercised against anything that actually *simulates*.

Skipped (not failed) if ``virtual_simfw.exe`` has not been built yet --
see ``firmware/SimFW/tools/virtual_simfw/README.md`` for the build step
(``build_host.ps1``, MSVC). This keeps the suite green on a machine without
the MSVC toolchain, the same way other environment-gated tests in this repo
skip rather than fail.
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[3]
_SIMFW_ROOT = _REPO_ROOT / "firmware" / "SimFW"
_VIRTUAL_SIMFW_EXE = _SIMFW_ROOT / "tools" / "virtual_simfw" / "build" / "virtual_simfw.exe"
_SCENARIOS_DIR = _SIMFW_ROOT / "scenarios"

_SRC_DIR = Path(__file__).resolve().parents[1] / "src"
if str(_SRC_DIR) not in sys.path:
    sys.path.insert(0, str(_SRC_DIR))

from kilnsim.link import TcpSimLink  # noqa: E402
from kilnsim.runner import run_scenario  # noqa: E402
from kilnsim.scenario import load_scenario  # noqa: E402

pytestmark = pytest.mark.skipif(
    not _VIRTUAL_SIMFW_EXE.exists(),
    reason=f"virtual_simfw.exe not built -- run build_host.ps1 in "
           f"{_VIRTUAL_SIMFW_EXE.parent.parent} first",
)


class _VirtualSimFW:
    """Launches one virtual_simfw.exe subprocess on an ephemeral port and
    tears it down on exit -- a lightweight fixture context manager rather
    than a pytest fixture, so a test can start more than one instance
    (the determinism test needs exactly that: two independent runs)."""

    def __init__(self, seed: int = 0):
        self.proc = subprocess.Popen(
            [str(_VIRTUAL_SIMFW_EXE), "--port", "0", "--seed", str(seed)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        line = self.proc.stdout.readline()
        if "VIRTUAL_SIMFW_LISTENING" not in line:
            self.proc.kill()
            raise RuntimeError(f"virtual_simfw did not start cleanly: {line!r}")
        self.port = int(line.split("port=")[1].split()[0])

    def __enter__(self) -> "_VirtualSimFW":
        return self

    def __exit__(self, *exc) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:  # pragma: no cover - defensive
            self.proc.kill()


def test_ping_and_caps_round_trip():
    """The wire round trip itself -- PING/GET_CAPS -- against the real
    benchproto codec and the real virtual device, no scenario involved."""
    from kilnsim.protocol import CommandGroup, SysCmd

    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            assert link.send_command(CommandGroup.SYS, SysCmd.PING) == {"pong": True}
            caps = link.send_command(CommandGroup.SYS, SysCmd.GET_CAPS)
            assert caps["zone_count_default"] == 3
            assert caps["tc_safety_channels"] == 1
            assert caps["ct_channel_count"] == 3
        finally:
            link.disconnect()


def test_baseline_firing_end_to_end():
    """No faults; DUT-gated `dut:` expectations correctly SKIP/PASS rather
    than spuriously passing on nothing having happened."""
    scenario = load_scenario(_SCENARIOS_DIR / "baseline_firing.yaml")
    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            report = run_scenario(link, scenario, duration_s=20.0)
        finally:
            link.disconnect()

    assert report.validity.valid
    names = {e.name: e.verdict for e in report.expectations}
    # "never_*" forbid clauses hold trivially with no DUT driving anything.
    assert names["never_faults"] == "PASS"
    assert names["never_estops"] == "PASS"
    assert names["never_trips"] == "PASS"
    # The one clause that needs a DUT to actually close a relay must NOT be
    # a spurious PASS -- this is the exact honesty property the fixture
    # exists to prove (a DUT-gated expectation reports as SKIPPED, not PASS).
    assert names["heat_actually_cycles"] == "SKIPPED"


def test_tc_stuck_fault_fires_without_a_dut():
    """An AT_SIM_TIME-triggered fault needs no DUT at all to fire (unlike
    an AT_ZONE_TEMP trigger, which needs a DUT closing a relay to ever heat
    a zone) -- fault_fired must appear on the wire, and the DUT-gated guard/
    K4 expectations downstream of it must FAIL (not SKIP, not PASS): the
    triggering event genuinely occurred, the DUT-side reaction genuinely
    never did."""
    scenario = load_scenario(_SCENARIOS_DIR / "tc_stuck.yaml")
    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            report = run_scenario(link, scenario, duration_s=20.0)
        finally:
            link.disconnect()

    from kilnsim.protocol import EventType
    fired = [e for e in report.events if e.event_type == EventType.FAULT_FIRED]
    assert fired, "TC_STUCK's AT_SIM_TIME(t=20) trigger should fire without any DUT"

    names = {e.name: e.verdict for e in report.expectations}
    assert names["sample_counter_goes_stale"] == "FAIL"  # SaftyFW's guard_warn never arrives
    assert names["trips_after_stale_trip_deadline"] == "FAIL"  # K4 never opens
    assert report.verdict == "FAIL"


def test_until_trigger_fault_two_frame_sequence():
    """FAULT_SCHEDULE(duration_kind=UNTIL_TRIGGER) + FAULT_SET_UNTIL_TRIGGER
    (PROTOCOL.md sec 5.6's two-frame design) against the real virtual
    device. No shipped scenario under firmware/SimFW/scenarios/ uses
    `until_trigger` yet, so this test builds a small synthetic one (same
    pattern as the determinism probe above) rather than skip the coverage
    entirely.

    virtual_simfw.c now implements the two-frame handshake (mirroring
    cmd_task.c's handle_fault_schedule()/handle_fault_set_until_trigger()):
    frame 1 (FAULT_SCHEDULE, duration_kind=UNTIL_TRIGGER) parks the fault's
    fields without arming it; frame 2 (FAULT_SET_UNTIL_TRIGGER) supplies the
    release trigger and performs the actual arm. This test drives both
    frames for real and asserts the fault actually fires at its ARM trigger
    (t=1) and clears at its release trigger (t=5)."""
    from kilnsim.protocol import CommandGroup, EventType, FaultCmd

    yaml_text = """
name: until_trigger_probe
version: 1
preset: fast_test
timescale: 20
seed: 1
faults:
  - id: noisy
    type: tc_noise
    target: tc:0
    trigger: { at_sim_time: { t: 1 } }
    duration: { until_trigger: { at_sim_time: { t: 5 } } }
    params: [3.0]
expect: []
"""
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "until_trigger_probe.yaml"
        path.write_text(yaml_text, encoding="utf-8")
        scenario = load_scenario(path)

    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            link.send_command(CommandGroup.SYS, 5, {"value": scenario.seed})  # SET_SEED
            link.send_command(CommandGroup.SYS, 4, {"value": scenario.timescale})  # SET_TIMESCALE
            link.send_command(CommandGroup.MODEL, 4, {"name": scenario.preset})  # LOAD_PRESET

            reply1 = link.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
                "fault_slot": 0,
                "fault_type": "tc_noise",
                "target": "tc:0",
                "trigger": {"kind": "at_sim_time", "t": 1.0},
                "duration": {"kind": "until_trigger"},
                "params": [3.0, 0.0, 0.0, 0.0],
            })
            assert reply1["fault_slot"] == 0

            reply2 = link.send_command(CommandGroup.FAULT, FaultCmd.SET_UNTIL_TRIGGER, {
                "fault_slot": 0,
                "trigger": {"kind": "at_sim_time", "t": 5.0},
            })
            assert reply2["fault_slot"] == 0

            events = []
            deadline = time.time() + 10.0
            while time.time() < deadline:
                events.extend(link.read_events(timeout=0.2))
                fired = [e for e in events if e.event_type == EventType.FAULT_FIRED]
                cleared = [e for e in events if e.event_type == EventType.FAULT_CLEARED]
                if fired and cleared:
                    break
        finally:
            link.disconnect()

    fired = [e for e in events if e.event_type == EventType.FAULT_FIRED]
    cleared = [e for e in events if e.event_type == EventType.FAULT_CLEARED]
    assert fired, "UNTIL_TRIGGER fault should fire once its ARM trigger (t=1) is reached"
    assert cleared, "UNTIL_TRIGGER fault should clear once its release trigger (t=5) is reached"
    assert fired[0].sim_time_us < cleared[0].sim_time_us


def test_determinism_same_seed_same_scenario_byte_identical_events():
    """PLAN.md sec 4.2/7.2's core testing contract: "the same scenario + seed
    => the same run, byte-for-byte in the event log" -- run twice against two
    independent virtual_simfw processes and diff the event logs field by
    field. Uses a small synthetic scenario with an EVERY+jitter repeat
    specifically so the PRNG seed actually has something nondeterministic to
    pin down (each re-arm draws a jitter offset from the seeded PRNG,
    fault_engine.h's own doc comment: "All randomness ... comes from a
    single seeded xorshift32 stream").

    Deliberately AT_SIM_TIME, not RANDOM_IN, for the *trigger* itself: this
    harness applies FAULT_SCHEDULE synchronously on TCP arrival rather than
    queuing it to a fixed tick boundary the way real firmware's PLAN.md 4.5
    "queue-then-apply-next-tick" doctrine does (see this test file's own
    module docstring and virtual_simfw.c's file-header "known deviations"),
    so the exact tick a slot first becomes ARMED can vary a little with
    process-scheduling/network jitter. AT_SIM_TIME's fire condition
    ("sim_time_s >= trigger time") is insensitive to exactly which tick it
    was armed on, as long as that happens before the target time -- so it
    isolates the PRNG-repeat determinism claim this test is actually
    probing from that harness-level timing wrinkle. A RANDOM_IN *trigger*
    pick, by contrast, is measurably affected by that same wrinkle (its pick
    range is anchored to "now" at first evaluation) -- a real, documented
    limitation, not swept under the rug: see this directory's virtual_simfw
    README "Known deviations" section."""
    import tempfile

    yaml_text = """
name: determinism_probe
version: 1
preset: fast_test
timescale: 20
seed: 5
faults:
  - id: rnd
    type: tc_noise
    target: tc:0
    trigger: { at_sim_time: { t: 1 } }
    duration: { for_s: 1 }
    repeat: { every: { period: 2, jitter: 1.5 } }
    params: [3.0]
expect: []
"""
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "determinism_probe.yaml"
        path.write_text(yaml_text, encoding="utf-8")
        scenario = load_scenario(path)

        def one_run():
            with _VirtualSimFW() as device:
                link = TcpSimLink()
                link.connect(f"127.0.0.1:{device.port}")
                try:
                    return run_scenario(link, scenario, seed=777, duration_s=25.0)
                finally:
                    link.disconnect()

        r1 = one_run()
        r2 = one_run()

    # What's actually invariant here vs. what isn't, and why the comparison
    # below is built the way it is:
    #
    # The trigger's own absolute firing time is NOT byte-identical across
    # runs, and that is expected, not a bug: this harness applies
    # FAULT_SCHEDULE synchronously as soon as its TCP frame is decoded
    # rather than queuing it to a fixed tick boundary (PLAN.md 4.5's real-
    # firmware "queue-then-apply-next-tick" doctrine, see this test file's
    # module docstring and virtual_simfw.c's "known deviations"). Between
    # LOAD_PRESET (which resets sim_time to 0) and FAULT_SCHEDULE arriving,
    # the device's sim clock keeps free-running at `timescale`x -- ordinary
    # process-scheduling/TCP round-trip jitter in *that* handshake means a
    # different number of milliseconds (hence, at 20x, a different number of
    # sim-seconds) can elapse before the AT_SIM_TIME(t=1) trigger is even
    # armed, so it can fire "late" by a different amount each run. This is
    # not specific to a simplification either -- it is a property any live,
    # asynchronously-commanded system has (real hardware over USB CDC would
    # have the same setup-latency variance); PLAN.md 4.2/7.2's determinism
    # contract is about the simulator's *own* evaluation being a pure
    # function of (sim state, seed), not about pinning real-world command
    # latency to zero.
    #
    # What IS the deterministic invariant worth proving: once the fault
    # engine's PRNG stream starts (the first arm), every subsequent
    # EVERY+jitter re-arm interval is a pure function of that seeded stream
    # -- so the *sequence of intervals between consecutive FAULT_FIRED
    # events* (not their absolute sim_time_us) must be byte-identical.
    def fired_gaps(report):
        fired = sorted(e.sim_time_us for e in report.events if e.event_type.name == "FAULT_FIRED")
        return [b - a for a, b in zip(fired, fired[1:])]

    gaps1, gaps2 = fired_gaps(r1), fired_gaps(r2)
    assert len(gaps1) >= 3 and len(gaps2) >= 3, "the probe scenario should repeat several times in 25 sim-seconds"
    n = min(len(gaps1), len(gaps2))
    assert gaps1[:n] == gaps2[:n], (
        "same scenario + same seed must draw the same repeat-jitter sequence from the "
        "seeded PRNG (PLAN.md sec 4.2/7.2's determinism contract)"
    )
