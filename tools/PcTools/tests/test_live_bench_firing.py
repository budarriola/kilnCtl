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
    assert code == 200 and '"ok":true' in body.replace(" ", ""), (code, body)
    running = bench.wait_for_exec_state(("running",), timeout_s=15.0)
    assert running["state"] == "running", running
    assert running["target_c"] <= FIXTURE_MAX_TEMP_C, running

    # Watch briefly: the setpoint must be tracking, and the live temperature
    # must stay inside the fixture ceiling the whole time.
    observed = bench.wait_for_exec_state(("__never__",), timeout_s=30.0, poll_s=2.0)
    assert bench.hottest_channel_c() <= FIXTURE_MAX_TEMP_C, observed

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
