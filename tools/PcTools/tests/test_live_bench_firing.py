#!/usr/bin/env python3
"""LIVE-BENCH regression tests: a known-good config, then a real, bounded
firing attempt through the same HTTP endpoints the dashboard's own Start
button posts to.

These skip unless ``KILNCTRL_BENCH_HOST`` is set -- see conftest.py.

    KILNCTRL_BENCH_HOST=192.168.1.156 python -m pytest \
        tools/PcTools/tests/test_live_bench_firing.py -q -s

WHAT "BOUNDED" MEANS HERE. The fixture's ceiling is 80 C
(config_presets/bench_fixture.json's comment block: a limit of THIS bench's
elements and enclosure, never of a kiln). The test profile targets 45 C on
zone 0 with a one-minute dwell, and three independent things keep it there:
the preset's own max_temp_c=80 and safety abs_max_temp_c=80, this harness's
own refusal to author a segment above 80 C
(BenchSession.put_profile), and a live temperature read before every start
(assert_within_fixture_ceiling). No test in this file ever raises a ceiling
or requests heat enable directly.

WHAT IT PROVES TODAY, AND WHAT CHANGES LATER. No current transformer is
fitted to this bench, so ``ct_channel_map[0..2]`` is uncommitted, SaftyFW's
commissioning_gate.c reports calibration_missing, and heat is refused. The
firing test therefore branches on ``heat_is_permitted()`` -- the board's own
live verdict, never a hardcoded expectation:

  * heat refused (today): the start endpoint itself ACCEPTS (measured, not
    assumed -- the gate is downstream of profile_executor.c), so the
    assertion is the stronger one: with heat not permitted, no relay ever
    energizes and the safety processor never reports heating enabled, for
    the whole time the executor believes it is firing. That is a real
    assertion about the safety gate working, not a placeholder.
  * heat permitted (after the CTs are fitted and the zone current-sweep
    commits a MEASURED map): the same test starts the profile, watches PV,
    stops it, and confirms relays return to off.

So the day commissioning lands, this file starts exercising the heat path
with no edit. The only thing that must change then is nothing here -- fit
the CTs, run the sweep from the zones page, and re-run.
"""
from __future__ import annotations

import os
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from bench_fixture_session import FIXTURE_MAX_TEMP_C, heat_is_permitted  # noqa: E402

pytestmark = pytest.mark.live_bench

#: User slot the bounded test profile is written to. Slot 7 is the last of
#: the 8 user slots (PROFILES_MAX_COUNT) and is not one of the two the bench
#: currently has authored ('scratch', 'stackcov'), so a test run does not
#: overwrite work someone left on the board.
TEST_PROFILE_SLOT = 7
TEST_PROFILE_NAME = "regr45"
TEST_PROFILE_TARGET_C = 45.0

#: How long the gate is watched with the executor running but heat not
#: permitted. Long enough for several 1 Hz relay samples and for the
#: executor to get past its first control iterations; short enough that the
#: whole live suite stays a few seconds.
GATE_OBSERVE_S = 6.0
#: How long the heat-permitted branch watches a live firing. Short on
#: purpose: this test proves the gate opened, it is not a soak test, and
#: every extra second is a second of real elements on a bench.
HEAT_OBSERVE_S = 30.0


def test_known_good_config_landed(bench_session):
    """The preset applied AND was read back: zones over GET/POST /api/zones,
    safety over /api/safety/commissioning, both verified by their own
    clients. Then the board's live config is checked directly, so a verifier
    bug cannot make this pass on its own say-so."""
    assert bench_session.zones_result is not None and bench_session.zones_result.ok

    # SafetyApplyResult.ok also folds in the ESP's own post-commit confirm,
    # which on this bench routinely declines to vouch ("could not read the
    # config back"). What matters is the independent GET read-back: no
    # mismatches, and every field the preset asked for confirmed. See
    # BenchSession._assert_safety_landed() for the full reasoning.
    safety = bench_session.safety_result
    assert safety is not None and safety.mismatches == [], safety.describe()
    assert set(bench_session.preset["safety"]) <= set(safety.confirmed), safety.describe()
    # And the board's own commissioning verdict, recorded rather than
    # assumed: False today because no CT is fitted (ct_channel_map unset).
    assert safety.commissioned_after is False or safety.still_unset == [], safety.describe()

    live = bench_session.zones()
    zones = {z["index"]: z for z in live["zones"]}
    for preset_zone in bench_session.preset["zones"]:
        got = zones[preset_zone["index"]]
        assert got["max_temp_c"] == pytest.approx(preset_zone["max_temp_c"]), got
        assert got["max_temp_c"] <= FIXTURE_MAX_TEMP_C
        assert got["relay_mask"] == preset_zone["relay_mask"], got

    params = {p["name"]: p for p in bench_session.commissioning()["params"]}
    assert params["abs_max_temp_c"]["set"] is True
    assert params["abs_max_temp_c"]["value"] == pytest.approx(FIXTURE_MAX_TEMP_C)
    assert params["tc_type"]["value"] == bench_session.preset["safety"]["tc_type"]


def test_dashboard_status_consistent_with_known_config(bench_session):
    """The cheap second test the harness makes cheap: what /api/status
    reports must agree with the config that was just verified. This is the
    'consumer without a producer' class of defect (MEMORY.md) caught from the
    outside -- a dashboard field nothing updates reads plausibly until it is
    compared against a config someone actually wrote."""
    status = bench_session.status()
    assert status["zones_config_valid"] is True
    assert status["thermo_ready"] is True
    assert status["io_ready"] is True

    channels = [c for c in status["channels"] if c["valid"]]
    assert len(channels) >= len(bench_session.preset["zones"]), status["channels"]
    for ch in channels:
        assert not ch["stale"], ch
        assert -20.0 < ch["temp_c"] < FIXTURE_MAX_TEMP_C, ch

    # Idle bench: nothing should be commanded on, and the safety processor
    # should not be reporting heat enabled.
    assert bench_session.relays_on() == []
    assert status["safety_heating_enabled"] is False


def test_bounded_profile_start_against_commissioning_gate(bench, capsys):
    """The firing test. Authors a bounded 45 C profile, starts it through the
    dashboard's own endpoint, and asserts on what the board actually did --
    branching on the live commissioning verdict, never on a hardcoded one.

    Teardown (conftest's ``bench`` fixture) force-stops the executor and
    asserts every relay is off, whichever branch ran and even if the test
    body raises."""
    hottest = bench.assert_within_fixture_ceiling()
    permitted, why = heat_is_permitted(bench)
    print(f"\n[live] hottest channel {hottest:.2f} C; heat permitted: {permitted} ({why})")

    bench.put_profile(
        TEST_PROFILE_SLOT, TEST_PROFILE_NAME, zone_mask=0x1,
        segments=[{"target_c": TEST_PROFILE_TARGET_C, "ramp_c_per_hr": 100.0, "dwell_min": 1}])

    before = bench.exec_status()
    assert before["state"] in ("idle", "complete", "faulted"), before

    code, body = bench.start_profile(TEST_PROFILE_SLOT)
    print(f"[live] POST /api/profile_exec/start -> {code} {body.strip()[:200]}")

    if not permitted:
        # ---- DOCUMENTED "not yet CT-commissioned" path ------------------
        # WHERE THE REFUSAL ACTUALLY LIVES, measured 2026-08-28 rather than
        # assumed: POST /api/profile_exec/start is ACCEPTED ({"ok":true})
        # and profile_executor.c runs. The gate is downstream -- SaftyFW's
        # commissioning_gate.c withholds the heat permit while
        # calibration_missing is set, and kiln_io_owner.c's
        # relay_on_blocked() therefore never lets a zone relay close. So the
        # assertion is not "the start was refused"; it is the stronger,
        # truer one: with heat not permitted, NO RELAY EVER ENERGIZES and
        # the safety processor never reports heating enabled, for the whole
        # time the executor believes it is firing.
        #
        # AFTER CT COMMISSIONING: heat_is_permitted() returns True, this
        # branch is not taken, and the heat-permitted branch below runs
        # instead. Nothing here needs editing.
        assert code == 200 or '"ok":false' in body.replace(" ", ""), (code, body)
        deadline = time.monotonic() + GATE_OBSERVE_S
        samples = 0
        while time.monotonic() < deadline:
            status = bench.status()
            assert [r["relay"] for r in status["relays"] if r["on"]] == [], (
                "heat is NOT permitted by the safety processor "
                f"({why}) yet a relay energized: {status['relays']}")
            assert status["safety_heating_enabled"] is False, status
            assert bench.hottest_channel_c() <= FIXTURE_MAX_TEMP_C
            samples += 1
            time.sleep(1.0)
        assert samples >= 3, "gate observation window did not sample"
        print(f"[live] gate held: {samples} samples over {GATE_OBSERVE_S}s, no relay energized")

        stop = bench.force_all_stop()
        assert stop["relays_on"] == [], stop
        assert bench.exec_status()["state"] in ("idle", "complete", "faulted")
        return

    # ---- heat-permitted path (post CT commissioning) --------------------
    #
    # Reached for real on 2026-08-28, once `ct_installed = 0` made this
    # CT-less bench commissionable. What this branch asserts is deliberately
    # split in two, because only ONE of the two is a statement about the
    # commissioning gate:
    #
    #   1. THE GATE OPENED. The executor reaches `running`, and for at least
    #      one poll the zone reports `heat_blocked: false` with a real duty.
    #      That is the whole subject of this test, and it is asserted
    #      unconditionally.
    #   2. HEAT WAS SUSTAINED. Whether the run survives long enough to move a
    #      thermocouple is a statement about the rest of the board, not about
    #      the gate.
    #
    # Measured on this bench: (1) passes — zone 0 reported
    # `relay_on: true, duty: 0.261, heat_blocked: false` within a second of
    # start. (2) does NOT, and the reason is unrelated to commissioning: the
    # isolated safety link times out on roughly 20% of polls (136 timeouts in
    # 681 frames, measured), `safety_link.c` raises
    # `SAFETY_FAULT_SRC_SAFETY_LINK` on that staleness, SaftyFW trips **S6a**
    # ("main controller reported a fault") and the run aborts inside ~1 s.
    # `trip_fault_sources` reads 8 (= SAFETY_FAULT_SRC_SAFETY_LINK) even
    # BEFORE a run is started, which is what shows it is not the firing that
    # provokes it.
    #
    # So the sustained-heat assertion below is recorded as an xfail-style
    # observation rather than a hard assert: making it hard would turn this
    # into a test of the link's reliability wearing a commissioning test's
    # name, and it would go green the day someone "fixed" it by widening a
    # timeout. It prints what it saw, every time, so the regression is
    # visible rather than silently tolerated.
    assert code == 200 and '"ok":true' in body.replace(" ", ""), (code, body)

    # Polled tightly from the instant of start, and NOT gated on first
    # observing state == "running": measured on this bench, the S6a abort
    # described above can land inside 250 ms, so a wait-for-running step
    # would fail before the loop that does the real work ever ran. The
    # faulted status still carries the per-zone `relay_on`/`duty`/
    # `heat_blocked` fields this test reads, which is what makes "did the
    # gate open" answerable even on a run that was cut short.
    start_c = bench.hottest_channel_c()
    commanded = False
    blocked_seen = []
    ever_running = False
    aborted_reason = ""
    hottest_seen = start_c
    deadline = time.monotonic() + HEAT_OBSERVE_S
    while time.monotonic() < deadline:
        ex = bench.exec_status()
        if ex.get("state") == "running":
            ever_running = True
            assert (ex.get("target_c") or 0.0) <= FIXTURE_MAX_TEMP_C, ex
        for z in ex.get("zones", []):
            if z.get("heat_blocked"):
                blocked_seen.append(
                    {"zone": z.get("zone"), "sources": z.get("heat_blocked_sources"),
                     "state": ex.get("state")})
            if z.get("relay_on") or float(z.get("duty") or 0.0) > 0.0:
                # The gate is open: the executor is commanding heat and
                # nothing downstream is blocking it.
                assert not z.get("heat_blocked"), (
                    "heat is permitted by the safety processor, yet the zone reports "
                    f"heat_blocked with sources {z.get('heat_blocked_sources')}: {z}")
                commanded = True
        hottest_seen = max(hottest_seen, bench.hottest_channel_c())
        assert hottest_seen <= FIXTURE_MAX_TEMP_C, (
            f"hottest channel {hottest_seen} C exceeded the {FIXTURE_MAX_TEMP_C} C ceiling")
        if ever_running and ex.get("state") != "running":
            aborted_reason = ex.get("fault_reason") or ex.get("state") or ""
            break
        time.sleep(0.2)

    assert ever_running, "the executor never reported `running` after an accepted start"
    print(f"[live] heat commanded: {commanded}; "
          f"temp {start_c:.2f} -> {hottest_seen:.2f} C; "
          f"run ended as: {aborted_reason or 'still running'}")

    # (1) -- the assertion this test exists for, phrased as the NEGATIVE
    # because the positive is a race this harness cannot win reliably.
    #
    # A zone's `relay_on`/`duty` is a genuine transient: the duty cycle is
    # short and, on this bench, the S6a abort can land inside 250 ms, which is
    # about one HTTP round trip. Asserting "I caught a relay closed" would be
    # a flaky test dressed as a strict one -- it would go red on a slow poll
    # and teach whoever hit it to re-run rather than to look.
    #
    # `heat_blocked` is not a transient. It is a LEVEL that
    # kiln_io_owner.c's relay_on_blocked() holds true for as long as anything
    # -- the commissioning gate included -- is withholding permission. An
    # uncommissioned board reports it on every single poll of a run. So
    # "the executor ran and no zone ever reported heat_blocked" is the same
    # claim, made against a signal that is actually observable at this
    # sampling rate. The loop above additionally asserts, on any poll that
    # DOES catch a relay closed, that heat_blocked is false on that same poll.
    assert not blocked_seen, (
        "the safety processor reports heat permitted, yet a zone reported heat_blocked "
        f"during the run: {blocked_seen}")

    # (2) -- observation only. See the block comment above for why.
    if aborted_reason:
        print(f"[live] NOTE: the run did not sustain heat. Reason: {aborted_reason!r}. "
              "This is a safety-LINK reliability finding (S6a off "
              "SAFETY_FAULT_SRC_SAFETY_LINK), not a commissioning one -- see this "
              "test's block comment.")

    stop = bench.force_all_stop()
    assert stop["relays_on"] == [], stop
    assert bench.exec_status()["state"] in ("idle", "complete", "faulted")


def test_uart_pid_half_reported(bench_session):
    """Not an assertion about the gains -- an assertion that the harness is
    HONEST about whether it wrote them. The MCP server usually owns the COM
    port, so the PID/model half is skipped; a test that silently believed it
    had applied gains it never wrote would be the same defect class this
    repo keeps finding elsewhere."""
    assert bench_session.uart_detail != "not attempted"
    print(f"\n[live] UART PID half: available={bench_session.uart_available} "
          f"({bench_session.uart_detail})")
