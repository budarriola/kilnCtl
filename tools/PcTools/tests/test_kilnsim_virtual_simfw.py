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
    results = {e.name: e for e in report.expectations}
    names = {n: r.verdict for n, r in results.items()}
    # "never_*" forbid clauses hold trivially with no DUT driving anything --
    # except `never_trips`, which targets `guard_trip` (SaftyFW-only, no
    # sim_event_type_t value exists for it -- kilnsim.protocol.EventType's
    # own class comment). This run_scenario() call passes no
    # `guard_observer=`, so kilnsim.runner's own honesty fix applies: that
    # clause must be BLOCKED, not a spurious vacuous PASS (the exact bug
    # kilnsim.runner._block_expectations_missing_guard_observer exists to
    # close -- see that function's own doc comment).
    assert names["never_faults"] == "PASS"
    assert names["never_estops"] == "PASS"
    assert names["never_trips"] == "BLOCKED"
    assert results["never_trips"].blocked_on["phase"] == "runner"
    # The one clause that needs a DUT to actually close a relay must NOT be
    # a spurious PASS -- this is the exact honesty property the fixture
    # exists to prove (a DUT-gated expectation reports as SKIPPED, not PASS).
    assert names["heat_actually_cycles"] == "SKIPPED"
    # Overall verdict follows report.py's FAIL > BLOCKED > PASS priority: no
    # FAIL anywhere, but never_trips is BLOCKED, so the whole run is BLOCKED
    # rather than an unqualified PASS that would hide the missing signal.
    assert report.verdict == "BLOCKED"


def test_tc_stuck_fault_fires_without_a_dut():
    """An AT_SIM_TIME-triggered fault needs no DUT at all to fire (unlike
    an AT_ZONE_TEMP trigger, which needs a DUT closing a relay to ever heat
    a zone) -- fault_fired must appear on the wire, and the DUT-gated guard/
    K4 expectations downstream of it must NOT pass: the triggering event
    genuinely occurred, the DUT-side reaction genuinely never did.

    Both of those downstream clauses carry the scenario's own `blocked_on:`
    annotation (tc_stuck.yaml: context_valid is never set true by present-day
    SaftyFW, Phase 7/link_task) -- report.py's BLOCKED verdict (added in the
    same pass as this test's update) is exactly for this case: a well-formed
    expectation that cannot pass today by documented, tracked DUT
    incompleteness, not a surprise regression. FAIL would have been correct
    before BLOCKED existed; asserting FAIL here now would be re-testing the
    OLD, less-informative behavior."""
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
    assert names["sample_counter_goes_stale"] == "BLOCKED"  # SaftyFW's guard_warn never arrives (blocked_on)
    assert names["trips_after_stale_trip_deadline"] == "BLOCKED"  # K4 never opens (blocked_on)
    assert report.verdict == "BLOCKED"  # no genuine FAIL, but not an unqualified PASS either


def _measure_sim_rate(timescale: float, window_s: float = 4.0) -> float:
    """Sim-seconds advanced per WALL second, measured against a freshly
    started ``virtual_simfw.exe`` at the given timescale. Sampled from the
    telemetry stream's own ``sim_time_us``, which is the same clock every
    scenario deadline and every fault trigger is evaluated against."""
    from kilnsim.protocol import CommandGroup, ModelCmd, SysCmd

    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            link.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": timescale})
            link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": "fast_test"})
            deadline = time.time() + 5.0
            while link.get_last_telemetry() is None and time.time() < deadline:
                time.sleep(0.05)
            first = link.get_last_telemetry()
            assert first is not None, "virtual_simfw published no telemetry"
            sim0, wall0 = int(first["sim_time_us"]), time.time()
            time.sleep(window_s)
            last = link.get_last_telemetry()
            sim1, wall1 = int(last["sim_time_us"]), time.time()
        finally:
            link.disconnect()
    return ((sim1 - sim0) / 1e6) / (wall1 - wall0)


def test_timescale_advances_sim_linearly_not_quadratically():
    """`timescale` must mean N sim-seconds per wall second -- the contract
    every consumer already assumes (``kilnsim.runner.run_scenario``'s own
    docstring: "Real time actually elapsed is `duration_s / timescale`") and
    the one real firmware implements (``sim_engine_task()`` is a
    ``vTaskDelayUntil(SIMFW_PERIOD_SIM_ENGINE_MS)`` loop -- a REAL-time
    cadence -- whose body advances the sim clock by
    ``SIMFW_PERIOD_SIM_ENGINE_MS * timescale``; the factor appears exactly
    once).

    ``virtual_simfw.c``'s host main loop reproduces that body verbatim, so
    scaling its tick ACCUMULATOR by timescale as well applied the factor a
    second time and advanced the sim clock by **timescale squared** per wall
    second. That is not a harmless speed-up: at ``timescale: 10`` the 2 Hz
    telemetry broadcast landed one frame every ~50 sim-seconds -- coarser
    than S3's 20 s ``stuck_on_time_s`` and several other guard windows under
    test -- so scenarios silently could not resolve the events they assert
    on, while still reporting confident verdicts. Two shipped scenarios were
    additionally dead because their ``at_zone_temp`` triggers were sized
    against the wrong clock.

    Measuring the ratio at ONE timescale would not catch the reintroduction
    (any single value could be explained by a slow machine), so this asserts
    the SHAPE: the ratio must track `timescale` itself, not its square. At
    timescale 5 the two hypotheses are 5x apart (5 vs 25), far outside the
    tolerance a loaded CI box needs.
    """
    slow = _measure_sim_rate(1.0)
    fast = _measure_sim_rate(5.0)

    # Generous absolute bounds: the measurement is quantized by the 2 Hz
    # telemetry broadcast at both ends (up to ~0.5 wall-seconds of staleness
    # each), which biases it a few percent LOW, never high.
    assert 0.7 <= slow <= 1.3, f"timescale 1 should advance ~1 sim-second per wall second, got {slow:.2f}"
    assert 3.0 <= fast <= 7.5, (
        f"timescale 5 should advance ~5 sim-seconds per wall second, got {fast:.2f} "
        f"-- ~25 would mean virtual_simfw.c's main loop is scaling its tick accumulator "
        f"by timescale again on top of device_tick()'s own scaling (the timescale-squared bug)"
    )
    # The shape check: 5x, not 25x.
    assert 3.0 <= fast / slow <= 8.0, (
        f"sim rate must scale LINEARLY with timescale; measured {fast / slow:.1f}x between "
        f"timescale 1 and timescale 5 (linear => ~5x, squared => ~25x)"
    )


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


def _send_raw_relay_command(link: TcpSimLink, cmd: int, payload_bytes: bytes, timeout: float = 2.0) -> bytes:
    """Sends a RELAY-group command by raw byte payload, bypassing
    ``kilnsim.payloads``'s per-command-group encoder tables entirely --
    needed for :data:`SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE` (0xF0), a
    virtual-only extension that intentionally has NO entry in
    ``kilnsim.payloads`` or ``kilnsim.protocol`` (it must never leak into
    real SimFW's numbering; see virtual_simfw.c's RELAY-group comment and
    this directory's README "Known, virtual-only extensions" section).
    Reuses :class:`TcpSimLink`'s own request/reply/retry machinery
    (``_link``/``_pending``/``_write_frame``/``_wait_for_reply``) so this
    test exercises the exact same wire path a real client would, just with
    a hand-built payload instead of ``payloads.encode_request``. Returns the
    raw reply payload bytes (byte 0 is the status code)."""
    from kilnsim import benchproto_codec as bp
    from kilnsim.protocol import CommandGroup

    assert link.is_connected, "not connected"
    with link._send_lock:  # noqa: SLF001 - intentional low-level test helper
        msg_index = link._link.next_msg_index()  # noqa: SLF001
        link._pending.begin(dst_device=1, dst_task=int(CommandGroup.RELAY), msg_index=msg_index)  # noqa: SLF001
        frame = bp.Frame(
            msg_type=bp.MsgType.DATA, msg_index=msg_index, src_device=0, src_task=0,
            dst_device=1, dst_task=int(CommandGroup.RELAY),
            payload=bytes([cmd]) + payload_bytes,
        )
        try:
            while True:
                with link._reply_cv:  # noqa: SLF001
                    link._reply_frame = None  # noqa: SLF001
                link._write_frame(frame)  # noqa: SLF001
                reply = link._wait_for_reply(timeout)  # noqa: SLF001
                if reply is not None:
                    break
                if not link._pending.note_retry():  # noqa: SLF001
                    raise TimeoutError("no reply to raw RELAY command")
                frame.msg_index = link._pending.msg_index  # noqa: SLF001
        finally:
            link._pending.clear()  # noqa: SLF001
    assert reply.msg_type == bp.MsgType.ACK, f"expected ACK, got {reply.msg_type}"
    return bytes(reply.payload)


def test_multi_client_two_connections_get_independent_evt_streams():
    """Task 2's headline claim: kilnsim and virtual_dut (or, here, two plain
    TcpSimLinks) can both be connected to the SAME running virtual_simfw.exe
    process at once, each gets its own telemetry, and each's EVT sequence
    stream is gap-free from ITS OWN connect point -- report.py's
    ``evt_seq_gap_count`` staying 0 for both is the honesty property this
    test exists to prove (see virtual_simfw.c's per-client
    ``telemetry_next_evt_seq`` cursor, and this file's design note there)."""
    from kilnsim.protocol import CommandGroup, FaultCmd, ModelCmd, SysCmd

    with _VirtualSimFW() as device:
        link_a = TcpSimLink()
        link_b = TcpSimLink()
        link_a.connect(f"127.0.0.1:{device.port}")
        try:
            link_b.connect(f"127.0.0.1:{device.port}")
            try:
                # Both already-connected clients must still be independently
                # reachable (PING) -- proves the listener kept accepting past
                # the first connection instead of only ever serving one.
                assert link_a.send_command(CommandGroup.SYS, SysCmd.PING) == {"pong": True}
                assert link_b.send_command(CommandGroup.SYS, SysCmd.PING) == {"pong": True}

                link_a.send_command(CommandGroup.SYS, SysCmd.SET_TIMESCALE, {"value": 20.0})
                link_a.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": "fast_test"})

                # Schedule a few AT_SIM_TIME faults (device_tick()'s own
                # fault_engine_tick()/ring_push() path -- see this file's
                # other tests -- unlike a manual FIRE_NOW, which this pass
                # discovered never posts a ring event at all, a pre-existing
                # gap unrelated to multi-client support and out of scope to
                # fix here) so both clients have real EVT traffic to drain,
                # then confirm both saw fault_fired without a gap.
                for slot in range(3):
                    link_a.send_command(CommandGroup.FAULT, FaultCmd.SCHEDULE, {
                        "fault_slot": slot, "fault_type": "tc_noise", "target": "tc:0",
                        "trigger": {"kind": "at_sim_time", "t": float(slot) + 0.5},
                        "duration": {"kind": "permanent"},
                        "params": [1.0, 0.0, 0.0, 0.0],
                    })

                deadline = time.time() + 8.0
                seen_a, seen_b = [], []
                while time.time() < deadline and (len(seen_a) < 3 or len(seen_b) < 3):
                    seen_a.extend(link_a.read_events(timeout=0.2))
                    seen_b.extend(link_b.read_events(timeout=0.2))

                assert len(seen_a) >= 3, "client A should see its own fault_fired events"
                assert len(seen_b) >= 3, "client B (a second, independent client) must ALSO see them"
                assert link_a.evt_seq_gap_count == 0, "client A's own EVT sequence must be gap-free"
                assert link_b.evt_seq_gap_count == 0, "client B's own EVT sequence must be gap-free"

                # Both must also be receiving TELEMETRY broadcasts.
                deadline = time.time() + 5.0
                while time.time() < deadline and (
                        link_a.get_last_telemetry() is None or link_b.get_last_telemetry() is None):
                    time.sleep(0.1)
                assert link_a.get_last_telemetry() is not None
                assert link_b.get_last_telemetry() is not None
            finally:
                link_b.disconnect()
        finally:
            link_a.disconnect()


def test_virtual_only_relay_set_sense_command():
    """SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE (0xF0): a virtual-DUT-only relay
    command that exists nowhere in PROTOCOL.md/cmd_ids.h (see virtual_simfw.c
    RELAY-group comment). Sets K1 and K4's sensed state exactly the way a
    real relay contact closing would be sensed, and confirms
    RELAY_GET_STATES reports it back, and that closing K1 (a real relay,
    already gated into duty[]/current_a[] by the unmodified thermal model)
    changes reported current -- the same path a real DUT physically closing
    the contact would drive. Also asserts the currently-true, load-bearing
    finding this task's brief asked to be verified rather than assumed:
    K4 (index 4) reaching 'closed' does NOT gate CT current on its own
    (device_tick()'s duty[]/current_a[] never reads k4, matching real,
    unmodified firmware/SimFW/src/tasks/sim_engine.c) -- so this is a
    documented, real property of the fixture's model, not a bug in this
    command."""
    from kilnsim.protocol import CommandGroup, ModelCmd

    RELAY_SET_SENSE = 0xF0
    K1, K4 = 0, 4

    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            link.send_command(CommandGroup.MODEL, ModelCmd.LOAD_PRESET, {"name": "fast_test"})

            # Unknown/out-of-range signal must be refused, not silently
            # accepted (ERR_BAD_ARGS == 0x02, cmd_ids.h's status byte 0).
            bad = _send_raw_relay_command(link, RELAY_SET_SENSE, bytes([5, 1]))
            assert bad[0] == 0x02, "signal=5 (FAULT_LINE, not a relay) must be ERR_BAD_ARGS"

            # Close K4 alone (the safety pilot) -- state must read back, but
            # per the finding above, current must NOT appear from K4 alone.
            ok = _send_raw_relay_command(link, RELAY_SET_SENSE, bytes([K4, 1]))
            assert ok[0] == 0x00
            time.sleep(0.3)
            states = link.send_command(CommandGroup.RELAY, 1, {})  # RelayCmd.GET_STATES
            assert states["k4_closed"] is True

            telem_k4_only = link.get_last_telemetry()
            zone_currents_k4_only = telem_k4_only["zones"][0]["i_amps"] if telem_k4_only else None

            # Now also close K1 -- current should now appear (duty[] IS
            # gated on K1/K2/K3, unlike K4).
            ok2 = _send_raw_relay_command(link, RELAY_SET_SENSE, bytes([K1, 1]))
            assert ok2[0] == 0x00
            time.sleep(0.5)
            states2 = link.send_command(CommandGroup.RELAY, 1, {})
            assert states2["k1_closed"] is True
            assert states2["k4_closed"] is True  # K4 sense unchanged by the K1 command

            deadline = time.time() + 3.0
            zone_currents_with_k1 = None
            while time.time() < deadline:
                t = link.get_last_telemetry()
                if t and t["zones"][0]["i_amps"] > 0.0:
                    zone_currents_with_k1 = t["zones"][0]["i_amps"]
                    break
                time.sleep(0.1)

            if zone_currents_k4_only is not None:
                assert zone_currents_k4_only == 0.0, (
                    "K4 alone must not gate/produce heater current -- verified finding, "
                    "not an assumption (see test docstring)"
                )
            assert zone_currents_with_k1 is not None and zone_currents_with_k1 > 0.0, (
                "K1 closing (a real relay already wired into duty[]) must produce current"
            )
        finally:
            link.disconnect()


def test_determinism_same_seed_same_scenario_byte_identical_events():
    """DESIGN_NOTES.md sec 4.2/7.2's core testing contract: "the same scenario + seed
    => the same run, byte-for-byte in the event log" -- run twice against two
    independent virtual_simfw processes and diff the event logs field by
    field. Uses a small synthetic scenario with an EVERY+jitter repeat
    specifically so the PRNG seed actually has something nondeterministic to
    pin down (each re-arm draws a jitter offset from the seeded PRNG,
    fault_engine.h's own doc comment: "All randomness ... comes from a
    single seeded xorshift32 stream").

    Deliberately AT_SIM_TIME, not RANDOM_IN, for the *trigger* itself: this
    harness applies FAULT_SCHEDULE synchronously on TCP arrival rather than
    queuing it to a fixed tick boundary the way real firmware's DESIGN_NOTES.md 4.5
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
    # rather than queuing it to a fixed tick boundary (DESIGN_NOTES.md 4.5's real-
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
    # have the same setup-latency variance); DESIGN_NOTES.md 4.2/7.2's determinism
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
        "seeded PRNG (DESIGN_NOTES.md sec 4.2/7.2's determinism contract)"
    )


def test_dut_power_safety_get_set_independent_of_main():
    """Defect fixed 2026-08-24: virtual_simfw used to answer IO/
    DUT_POWER_SAFETY_GET (0x0A) with ERR_NOT_IMPL, which failed kilnsim
    selftest's command_groups_reachable check and dragged every `--virtual`
    run's exit code to 1 for a reason unrelated to whatever scenario was
    actually being tested (see this repo's task notes / PROTOCOL.md sec
    5.5). This proves the safety-domain (J19) SET/GET pair now works, and --
    the actual point of two independent relays existing at all -- that
    commanding one domain never leaks into the other's readback."""
    from kilnsim.protocol import CommandGroup, IoCmd

    with _VirtualSimFW() as device:
        link = TcpSimLink()
        link.connect(f"127.0.0.1:{device.port}")
        try:
            # Both domains default on (virtual_simfw.c device_init()'s
            # documented deviation from real firmware's power-off boot
            # default -- see that function's comment).
            main_reply = link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)
            safety_reply = link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)
            assert main_reply["on"] is True
            assert safety_reply["on"] is True

            # Turning the safety domain off must not touch the main domain.
            link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_SET, {"on": False})
            assert link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)["on"] is False
            assert link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)["on"] is True

            # And the reverse: turning the main domain off must not touch
            # the (now off) safety domain, nor turn it back on.
            link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SET, {"on": False})
            assert link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)["on"] is False
            assert link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)["on"] is False

            # Turning the safety domain back on independently must not
            # revive the main domain either.
            link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_SET, {"on": True})
            assert link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_SAFETY_GET)["on"] is True
            assert link.send_command(CommandGroup.IO, IoCmd.DUT_POWER_GET)["on"] is False
        finally:
            link.disconnect()
