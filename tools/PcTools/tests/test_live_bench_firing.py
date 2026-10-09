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

WHICH BRANCH RUNS. Every firing test here branches on
``heat_is_permitted()`` -- the board's own live verdict, never a hardcoded
expectation. Since ``ct_installed = 0`` made this CT-less bench commissionable
(2026-08-28) the answer is True and the heat-permitted branches are the ones
that run; the refusal branches remain, asserted, for a board that is not
commissioned:

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

    # Idle bench: nothing should be commanded on, and the safety processor's
    # own heat relay should not be energized.
    assert bench_session.relays_on() == []
    assert status["safety_relay_energized"] is False

    # NOT safety_heating_enabled. That field is SAFETY_FLAG_ENABLED, which
    # dashboard_http.h documents as "SaftyFW's relay_owner state machine is
    # currently ARMED (not tripped) -- true on any healthy, past-its-grace-
    # period Pico REGARDLESS of whether anyone ever sent
    # SAFETY_CMD_REQUEST_ENABLE. It is NOT 'heat was granted'." Asserting
    # False here was that documented misreading, and it only ever passed
    # because the safety link was timing out on every poll, so no STATUS
    # frame arrived and the field stayed at its null/false default. With the
    # link fixed (63cc741) a healthy idle board reports True, correctly.
    # safety_relay_energized above is the field that actually means "no heat".
    assert status["safety_heating_enabled"] is True


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
            # safety_RELAY_energized, not safety_heating_enabled. The same
            # correction test_dashboard_status_consistent_with_known_config()
            # above already carries: SAFETY_FLAG_ENABLED means "SaftyFW's
            # relay_owner is ARMED", which is true on any healthy Pico
            # whether or not heat was ever asked for or granted. This branch
            # asserted the wrong one, and would have started failing the day
            # the link got reliable enough for a STATUS frame to arrive --
            # for a reason that has nothing to do with the gate it is
            # testing. K4 (safety_relay_energized) is the contact.
            assert status["safety_relay_energized"] is False, status
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
    # THE assertion this file was missing until 2026-08-29, and the reason
    # every "the bench barely heats" observation before that date was
    # measuring nothing: profile_executor.c never sent
    # SAFETY_CMD_REQUEST_ENABLE, so K4 on the safety processor stayed OPEN
    # for the whole firing while K1 on this board closed and the run looked
    # completely normal. Nothing here, or anywhere else in the suite, could
    # tell that apart from a slow kiln. K4 closing is now checked directly.
    k4_closed = False
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
        if bench.status()["safety_relay_energized"]:
            k4_closed = True
        hottest_seen = max(hottest_seen, bench.hottest_channel_c())
        assert hottest_seen <= FIXTURE_MAX_TEMP_C, (
            f"hottest channel {hottest_seen} C exceeded the {FIXTURE_MAX_TEMP_C} C ceiling")
        if ever_running and ex.get("state") != "running":
            aborted_reason = ex.get("fault_reason") or ex.get("state") or ""
            break
        time.sleep(0.2)

    assert ever_running, "the executor never reported `running` after an accepted start"
    print(f"[live] heat commanded: {commanded}; K4 closed: {k4_closed}; "
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

    # (1b) -- K4. Unlike a zone's relay_on/duty, this is not a transient a
    # poll can miss: the request stands for the whole run and is only
    # released on an exit path, so any poll during a running firing should
    # see it closed. A run that commands heat with K4 open is a run that
    # heats nothing, however healthy every other field looks.
    assert k4_closed, (
        "the executor ran with heat permitted, yet the safety processor's K4 never closed "
        "-- no element current flowed. This is the defect of 2026-08-29: the firing path "
        "never sent SAFETY_CMD_REQUEST_ENABLE at all (see heat_enable.h).")

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


# ---------------------------------------------------------------------------
# FULL MULTI-SEGMENT PROFILE
# ---------------------------------------------------------------------------
#
# Everything above fires a SINGLE segment. That answers "does the gate open"
# and "does heat flow", and it cannot answer the questions a real firing is
# made of: does the executor advance from one segment to the next, does a
# dwell hold, does a DOWN-ramp behave (heat off, PV falls, no fault), and
# does the run reach `complete` on its own with every relay released and K4
# given back -- rather than being stopped by the test's own teardown.
#
# Slot and shape. The same user slot 7 the other tests use, so the bench's
# authored profiles stay untouched. Three segments, chosen against MEASURED
# numbers rather than round ones: this jig rises 2.8-3.8 C/min at full duty,
# so a 180 C/hr (3.0 C/min) ramp is achievable without saturating for the
# whole segment, and the 10 C steps take a few minutes each.

FULL_PROFILE_SLOT = 7
FULL_PROFILE_NAME = "multi3"
#: (target_c, ramp_c_per_hr, dwell_min). Peak 52 C -- 28 C of margin under
#: the 80 C fixture ceiling, and the harness refuses any segment above it
#: independently (BenchSession.put_profile).
#
#: RAMP AND DWELL ARE CHOSEN AGAINST THE PLANT, not for tidiness. A dwell is
#: timed from the SETPOINT arriving, not the PV, so a profile whose ramp
#: outruns the jig completes with the PV still climbing -- and then reads as
#: a firing that never got hot. (Exactly that bug was found the same day in
#: the step test's own profile: a 900 C/hr ramp with a 1-minute dwell ended
#: the run at 105 s with PV at 38.6 C.) 150 C/hr is 2.5 C/min, comfortably
#: inside this jig's measured 2.8-3.8 C/min, so the PV tracks the setpoint
#: instead of chasing it; the 6-minute dwells then give it time to close any
#: remaining lag before the next segment starts.
FULL_PROFILE_SEGMENTS = [
    {"target_c": 42.0, "ramp_c_per_hr": 150.0, "dwell_min": 6},
    {"target_c": 52.0, "ramp_c_per_hr": 150.0, "dwell_min": 6},
    # The down-ramp. No cooling hardware exists, so this segment is
    # satisfied by the executor holding heat OFF and letting the jig fall --
    # which is exactly the behaviour worth checking, because a controller
    # that keeps driving into a descending setpoint is one that overshoots
    # every real cooling ramp.
    {"target_c": 46.0, "ramp_c_per_hr": 600.0, "dwell_min": 1},
]
#: Total budget. Ramp+dwell is ~8 minutes per rising segment; the passive
#: down-ramp is the slow one (this jig sheds heat far more slowly than it
#: gains it). 45 minutes is generous against a ~30 minute expectation, and
#: it is a BUDGET: exceeding it fails, naming the state it was stuck in.
FULL_PROFILE_BUDGET_S = float(os.environ.get("KILNCTRL_BENCH_PROFILE_BUDGET_S", "2700"))
FULL_PROFILE_POLL_S = 10.0


def test_full_multi_segment_profile_runs_to_completion(cold_bench, capsys):
    """Run a real three-segment firing end to end and assert on every stage.

    Takes ``cold_bench``: segment 0's ramp is only meaningful from ambient,
    and starting on a previous test's residual heat would let the executor
    walk straight into the dwell without ever ramping.
    """
    bench = cold_bench
    permitted, why = heat_is_permitted(bench)
    if not permitted:
        pytest.skip(f"heat is not permitted on this bench ({why}); a full profile firing "
                    "cannot be verified without it")

    start_c = bench.hottest_channel_c()
    print(f"\n[live] full profile: starting at {start_c:.2f} C")
    assert start_c < FULL_PROFILE_SEGMENTS[0]["target_c"] - 2.0, (
        f"the bench is already at {start_c:.2f} C, at or above segment 0's "
        f"{FULL_PROFILE_SEGMENTS[0]['target_c']} C target -- there is no ramp to observe")

    bench.put_profile(FULL_PROFILE_SLOT, FULL_PROFILE_NAME, zone_mask=0x1,
                      segments=FULL_PROFILE_SEGMENTS)
    code, body = bench.start_profile(FULL_PROFILE_SLOT)
    print(f"[live] POST /api/profile_exec/start -> {code} {body.strip()[:160]}")
    assert code == 200 and '"ok":true' in body.replace(" ", ""), (code, body)

    running = bench.wait_for_exec_state(("running",), timeout_s=20.0)
    assert running["state"] == "running", running

    # ---- follow the whole run ------------------------------------------
    rows = []
    segments_seen = []
    dwelled = set()
    k4_samples = 0
    peak_c = start_c
    deadline = time.monotonic() + FULL_PROFILE_BUDGET_S
    t0 = time.monotonic()
    final = running
    while True:
        ex = bench.exec_status()
        status = bench.status()
        temps = {int(c["channel"]): float(c["temp_c"])
                 for c in status["channels"] if c.get("valid")}
        zone_c = temps.get(0, float("nan"))
        peak_c = max(peak_c, max(temps.values()) if temps else peak_c)
        row = {"t_s": round(time.monotonic() - t0, 1), "state": ex.get("state"),
               "seg": ex.get("segment_index"), "dwelling": ex.get("dwelling"),
               "target_c": ex.get("target_c"), "zone_c": zone_c,
               "relays_on": [r["relay"] for r in status["relays"] if r["on"]],
               "k4": status["safety_relay_energized"],
               "ramp_lock_held": ex.get("ramp_lock_held"),
               "ramp_lock_lagging_mask": ex.get("ramp_lock_lagging_mask")}
        rows.append(row)
        if row["k4"]:
            k4_samples += 1
        if row["state"] == "running" and row["seg"] is not None and (
                not segments_seen or segments_seen[-1] != row["seg"]):
            segments_seen.append(row["seg"])
            print(f"[live] t={row['t_s']:.0f}s entering segment {row['seg']} "
                  f"(target {row['target_c']}, PV {zone_c:.2f} C)")
        if row["dwelling"] and row["seg"] is not None:
            dwelled.add(row["seg"])

        # Ceiling, on every sample, from the test side as well as the board's.
        assert peak_c <= FIXTURE_MAX_TEMP_C, (f"peak {peak_c:.2f} C", row)
        assert (row["target_c"] or 0.0) <= FIXTURE_MAX_TEMP_C, row

        if row["state"] != "running":
            final = ex
            break
        if time.monotonic() >= deadline:
            final = ex
            break
        time.sleep(FULL_PROFILE_POLL_S)

    elapsed = time.monotonic() - t0
    print(f"[live] run ended after {elapsed:.0f}s in state {final.get('state')!r} "
          f"(fault_reason={final.get('fault_reason')!r}); peak {peak_c:.2f} C; "
          f"segments seen {segments_seen}; dwelled in {sorted(dwelled)}; "
          f"K4 closed on {k4_samples}/{len(rows)} samples")

    # ---- assertions ----------------------------------------------------
    # "done", not "complete" -- dashboard_http.c's exec_state_name() maps
    # PROFILE_EXEC_DONE to "done", and that is the string a client sees. The
    # first version of this assertion guessed "complete" and failed a run that
    # had in fact finished perfectly, which is its own small lesson about
    # asserting on a wire format from memory.
    assert final.get("state") == "done", (
        f"the profile did not complete: state={final.get('state')!r}, "
        f"fault_reason={final.get('fault_reason')!r}, last rows {rows[-3:]}")

    # Every segment was entered, in order. A profile that jumps a segment
    # completes just as cleanly as one that ran it.
    assert segments_seen == list(range(len(FULL_PROFILE_SEGMENTS))), (
        f"segments were not entered in order: {segments_seen}")
    # And every one of them actually dwelled -- a dwell that is skipped is
    # the difference between a firing schedule and a list of temperatures.
    # Superset, not equality: on the final tick the executor has already
    # advanced segment_index past the last segment (to segment_count) while
    # `dwelling` is still set from the segment that just ended, so a
    # one-past-the-end index legitimately appears in this set.
    assert dwelled >= set(range(len(FULL_PROFILE_SEGMENTS))), (
        f"some segments never reported dwelling: saw {sorted(dwelled)}")

    # The rising segments really did rise, and the peak got near the top
    # target rather than the profile completing on a timer while cold.
    top = max(s["target_c"] for s in FULL_PROFILE_SEGMENTS)
    assert peak_c >= top - 3.0, (
        f"the profile completed but the bench only reached {peak_c:.2f} C against a "
        f"{top} C peak target -- it ran the clock, not the schedule")

    # K4 was closed for a real fraction of the run. Not "every sample": the
    # down-ramp segment is supposed to release heat, and a dwell at
    # temperature cycles. A run with K4 closed on almost nothing is the
    # 2026-08-29 defect returning (heat_enable.h).
    assert k4_samples >= len(rows) // 4, (
        f"K4 was closed on only {k4_samples} of {len(rows)} samples across the whole firing")

    # The down-ramp: heat must actually have been released. Segment 2's
    # target is BELOW segment 1's, so a controller still driving there would
    # show relay 1 closed while PV sits above target.
    seg2 = [r for r in rows if r["seg"] == 2 and r["state"] == "running"]
    driving_over_target = [r for r in seg2
                           if r["relays_on"] and r["zone_c"] > (r["target_c"] or 0.0) + 1.0]
    assert not driving_over_target, (
        "the executor kept commanding heat during the DOWN-ramp while already above its "
        f"descending setpoint: {driving_over_target[:3]}")
    if seg2:
        print(f"[live] down-ramp: PV {seg2[0]['zone_c']:.2f} -> {seg2[-1]['zone_c']:.2f} C "
              f"over {seg2[-1]['t_s'] - seg2[0]['t_s']:.0f}s with relays "
              f"{sorted({x for r in seg2 for x in r['relays_on']})}")

    # RAMP LOCK. This profile is single-zone (zone_mask 0x1), so the lock can
    # never legitimately engage: profile_executor.c holds the shared setpoint
    # only while a PARTICIPATING zone lags by more than
    # PROFILE_EXECUTOR_RAMP_LOCK_BAND_C, and with one participant the lagging
    # mask has nothing to hold for. Asserting it stayed clear is therefore a
    # real check on the mask's bookkeeping -- a lagging bit set for a zone
    # that is not in the profile would show up here and nowhere else.
    #
    # NOT a test of multi-zone ramp-lock coordination. That needs two zones
    # in closed-loop control, and this bench runs zones 1 and 2 at
    # control_mode 0 with zero gains (bench_fixture.json). Said plainly here
    # rather than implied, so nobody reads this assertion as covering it.
    stuck = [r for r in rows if r["ramp_lock_held"] or r["ramp_lock_lagging_mask"]]
    assert not stuck, (
        "ramp lock engaged during a SINGLE-zone profile, where no zone can lag another: "
        + repr(stuck[:3]))

    # ---- the board is left safe ----------------------------------------
    after = bench.status()
    assert [r["relay"] for r in after["relays"] if r["on"]] == [], after["relays"]
    assert after["safety_relay_energized"] is False, (
        "the run completed but K4 is still energized -- heat_enable's release did not run "
        "on the completion path")
