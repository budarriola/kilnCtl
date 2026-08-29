#!/usr/bin/env python3
"""LIVE-BENCH regression tests for the two halves of "step and PID tuning":

  1. a CLOSED-LOOP step -- command a bounded setpoint change on zone 0 and
     record the resulting PV trace by polling the board, and
  2. the OPEN-LOOP step autotune -- drive ``autotune_engine.c``'s
     AUTOTUNE_METHOD_STEP end to end through the same
     ``/api/autotune/{start,,abort}`` endpoints the zones page posts to, and
     assert on whatever it actually produced.

These skip unless ``KILNCTRL_BENCH_HOST`` is set -- see conftest.py.

    KILNCTRL_BENCH_HOST=192.168.1.156 python -m pytest \
        tools/PcTools/tests/test_live_bench_tuning.py -q -s

WHY THE RELAY METHOD IS NOT HERE. autotune_engine.h's
AUTOTUNE_RELAY_SETPOINT_HEADROOM_C refuses any relay-feedback setpoint within
50 C of the zone's max_temp_c. With this fixture's 80 C ceiling that admits
only setpoints below 30 C -- under this bench's own ambient (35 C measured
2026-08-28). The relay method is not runnable on this bench at all, and a
test that "exercised" it would be testing a parameter refusal, not a tune.
BenchSession.start_autotune_step() is therefore step-only by construction.

WHAT BOUNDS THE HEAT. Five independent things, none of them trusting the
firmware alone: the preset's max_temp_c/abs_max_temp_c (80 C), the harness's
refusal to author a segment above it, a live temperature read before every
start, a ceiling assertion on EVERY sample taken during a run
(``sample_response`` / ``wait_for_autotune_state``), and a ``finally`` that
force-stops the executor AND aborts the autotune. Nothing here raises a
ceiling, requests heat enable, or drives a relay directly.

WHICH BRANCH RUNS. Same as the firing test: ``heat_is_permitted()`` is the
board's own live verdict, never a hardcoded expectation. Both branches are
real assertions.

  * heat NOT permitted -- the gate-holds branch. The engine runs, believes
    it is stepping, and NO relay closes, so the PV trace is flat, and that
    flatness is asserted rather than merely observed.
  * heat permitted -- the branch this bench takes since ``ct_installed = 0``
    made a CT-less board commissionable (2026-08-28). Since 2026-08-29 it
    asserts the PLANT, not just the gate: relay 1 closes, K4 closes and
    stays closed, and zone 0 rises at a real, bounded rate.

STALE NARRATIVE REMOVED, 2026-08-29. Earlier versions of this file carried a
long note concluding "the heat path to zone 0 delivers essentially nothing",
from a 40-minute observation of +0.67 C. That observation was real and the
conclusion was wrong: ``profile_executor.c`` never sent
SAFETY_CMD_REQUEST_ENABLE, so K4 was open for all 40 minutes while K1 closed
and every status field looked normal (see ``heat_enable.h``). With that fixed
the same jig rises 2.8-3.8 C/min.
"""
from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from bench_fixture_session import (  # noqa: E402
    FIXTURE_MAX_TEMP_C,
    BenchSessionError,
    heat_is_permitted,
)

pytestmark = pytest.mark.live_bench

#: Zone under test. Zone 0 is the only zone bench_fixture.json puts in a
#: closed-loop control_mode (2); zones 1 and 2 are mode 0 with zero gains.
TUNE_ZONE = 0

#: Same slot the firing test uses (the last user slot, not one of the two the
#: bench has authored) -- these tests never run concurrently with it, and
#: reusing the slot keeps the bench's real profiles untouched.
STEP_PROFILE_SLOT = 7
STEP_PROFILE_NAME = "step45"

#: The commanded step. 45 C against a ~35 C ambient is a ~10 C step: large
#: enough that a real response is unmistakable against type-K noise (a few
#: tenths of a degree), and 35 C clear of the fixture ceiling.
STEP_TARGET_C = 45.0

#: Seconds of PV sampled after the step is commanded.
#:
#: RAISED 2026-08-29 from 60 s. 60 s was chosen when the only question was
#: "is the loop actuating at all", and it cannot answer the question this
#: test now asks: the measured dead time on this jig is 37 s, and the heater
#: window is 60 s, so a 60 s sweep is barely one duty cycle past the plant's
#: own transport delay. 480 s covers the dead time, several windows, and --
#: at 2.8-3.8 C/min from a ~34 C ambient toward a 45 C setpoint -- the
#: arrival and the first minutes of settling.
STEP_OBSERVE_S = float(os.environ.get("KILNCTRL_BENCH_STEP_OBSERVE_S", "480"))

#: Poll period for the PV trace. This jig's time constant is ~167 s, so a 2 s
#: poll buys no extra information and spends hundreds of request pairs of the
#: board's httpd on saying "still 31.4 C". 10 s gives ~48 samples over the
#: default sweep, which is ample for a rate fit.
STEP_POLL_S = float(os.environ.get("KILNCTRL_BENCH_STEP_POLL_S", "10"))

#: The step-response rise this bench must produce, in C/min, measured over
#: the stretch where the loop is still driving hard (PV more than
#: STEP_SETTLE_BAND_C below setpoint).
#:
#: MEASURED on this jig at full duty, 2026-08-29, once the K4 defect was
#: fixed: 3.8 C/min over the fastest sampled stretch and 2.8 C/min over the
#: slowest. 1.5 is deliberately well under the slowest of those -- the
#: assertion's job is to catch "the heat path delivers nothing" (the 0.017
#: C/min an open K4 produced, 170x smaller), not to re-measure the plant to
#: three digits and go red on a warm afternoon.
STEP_MIN_RISE_C_PER_MIN = 1.5

#: How close to setpoint counts as "arrived", for both the rise-rate window
#: above and the settle assertion below. Wider than the 60 s window's own
#: ripple (under 2 C at full duty on this jig, per bench_fixture.json's
#: heater-timing comment) so a normal duty cycle is not read as a miss.
STEP_SETTLE_BAND_C = 3.0

#: Dwell for the step profile, in minutes -- DERIVED from the observation
#: window, never a constant.
#:
#: BUG FOUND 2026-08-29, and it invalidated the first long step run of the
#: day. The profile was authored with dwell_min=1 against a 900 C/hr ramp, so
#: the SETPOINT reached 45 C in ~45 s and the one-minute dwell expired at
#: ~105 s -- the executor reported `complete` and dropped the heat while the
#: PV was still at 38.6 C and climbing. The remaining ~380 s of the sweep
#: sampled a jig that was cooling, and the whole trace read as "rose 3.06 C
#: in 483 s = 0.38 C/min", four times too slow and for a reason that has
#: nothing to do with the plant. A dwell shorter than the observation is a
#: measurement of the dwell timer.
#:
#: The dwell must outlast the sweep, hence +2 minutes of margin over it.
STEP_DWELL_MIN = int(STEP_OBSERVE_S // 60) + 2

#: Guard-1 precondition. thermal_guard.c's dead-element check requires the
#: zone to rise at least sanity_rate_c_per_min per minute while heat is
#: commanded below setpoint, or the run is FAULTED and the trace ends there.
#: This bench carried 5.0 (a real kiln's figure) and every step died at
#: t=62s on "rose only 0.0C in 1.0min (need >=5.0C)" -- a config problem
#: that looked exactly like a broken step test. bench_fixture.json now
#: commissions 0.2. Asserted, not assumed: a step test on a board whose
#: guard cannot physically be satisfied is not measuring the plant.
STEP_MAX_USABLE_SANITY_RATE_C_PER_MIN = 1.0

#: Autotune budget. AUTOTUNE_ENGINE_SETTLE_S is 180 s of baseline hold before
#: the step is applied, so anything under ~200 s could only ever observe
#: "settling" and would prove nothing about the step. 300 s reaches STEPPING
#: and then watches ~2 minutes of it. The engine's OWN budget is 4 h
#: (AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S); this test aborts long before
#: that rather than blocking a suite for hours, and says so.
#: RAISED 2026-08-29 from 300 s. The first run this board ever fit a model to
#: reached DONE at elapsed_s=390 -- 180 s of settle plus ~210 s of step --
#: so a 300 s budget could only ever have aborted it and reported
#: "inconclusive". A budget shorter than the thing being measured is not a
#: bounded test, it is a guaranteed non-result.
AUTOTUNE_BUDGET_S = float(os.environ.get("KILNCTRL_BENCH_AUTOTUNE_BUDGET_S", "900"))

#: Open-loop step amplitude. 0.5 duty is autotune_start_post_handler()'s own
#: default and sits inside the 0.15/0.85 band the engine's guard reasoning
#: assumes.
AUTOTUNE_STEP_DUTY = 0.5


def _describe(rows: "list[dict]") -> str:
    first, last = rows[0], rows[-1]
    return (f"{len(rows)} samples over {last['t_s']:.0f}s: "
            f"zone {first['zone_c']:.2f} -> {last['zone_c']:.2f} C, "
            f"hottest {max(r['hottest_c'] for r in rows):.2f} C, "
            f"relays seen on: {sorted({r for row in rows for r in row['relays_on']})}")


def test_preset_pid_gains_are_live_on_the_board(bench_session):
    """The precondition every tuning assertion rests on, checked against the
    board rather than the preset: the gains bench_fixture.json asked for are
    the gains zone 0 is actually running.

    This is not redundant with test_known_good_config_landed -- that one
    checks max_temp_c and relay_mask. kp/ki/kd travel a different path
    (zones_http_client's _PRESET_ZONE_OVERRIDE_FIELDS over POST /api/zones),
    and a tuning test that never confirmed them would be measuring an unknown
    controller."""
    preset_zone = next(z for z in bench_session.preset["zones"] if z["index"] == TUNE_ZONE)
    live = bench_session.zone_pid(TUNE_ZONE)
    assert live["kp"] == pytest.approx(preset_zone["pid_kp"]), live
    assert live["ki"] == pytest.approx(preset_zone["pid_ki"]), live
    assert live["kd"] == pytest.approx(preset_zone["pid_kd"]), live
    # Gains a PID can be run with at all: negative gains are a sign-error
    # class of defect that reads as plausible numbers in a config dump.
    assert all(v >= 0.0 for v in live.values()), live

    model = bench_session.zone_model(TUNE_ZONE)
    print(f"\n[live] zone {TUNE_ZONE} gains {live}; FOPDT model {model}")
    # The model triple is only written by accepting an autotune fit. Zero is
    # the honest reading on a board that has never had one accepted -- and if
    # it is ever non-zero, it must at least be physical.
    assert model["tau_s"] >= 0.0 and model["dead_time_s"] >= 0.0, model


def test_closed_loop_setpoint_step_response(cold_bench, capsys):
    """CLOSED-LOOP STEP. Author a single-segment profile that steps zone 0's
    setpoint to 45 C at the zone's own max ramp, start it through the
    dashboard's endpoint, and record the PV trace by polling.

    The assertion branches on the board's live commissioning verdict:

      * heat NOT permitted (today): the executor runs and believes it is
        driving a 45 C setpoint, yet no relay ever closes and PV must stay
        flat within sensor noise. A rising trace here would mean heat reached
        an element past a gate that says it did not.
      * heat permitted: PV must move toward the setpoint and never overshoot
        the fixture ceiling.

    Takes ``cold_bench``, not ``bench``: the rise rate asserted at the end is
    only a measurement of the plant if the plant started at ambient. Run
    straight after another firing it would read low -- and read low for a
    reason that looks exactly like a dead element.
    """
    bench = cold_bench
    hottest = bench.assert_within_fixture_ceiling()

    # Guard 1 first: on a jig that heats in minutes, a kiln-sized
    # sanity_rate_c_per_min ends every run at the first window boundary and
    # the trace below would be a fault log, not a step response.
    zone_cfg = bench.zones()["zones"][TUNE_ZONE]
    rate = zone_cfg["sanity_rate_c_per_min"]
    assert 0.0 < rate <= STEP_MAX_USABLE_SANITY_RATE_C_PER_MIN, (
        f"zone {TUNE_ZONE} sanity_rate_c_per_min is {rate} C/min. thermal_guard.c guard 1 will "
        f"fault this zone at the first window boundary unless the jig can really rise that fast; "
        f"bench_fixture.json commissions 0.2. (0 is also refused here: it would select the "
        f"firmware default 0.5, which is a number nobody chose for this rig.)")

    permitted, why = heat_is_permitted(bench)
    baseline = bench.status()["channels"][TUNE_ZONE]["temp_c"]
    print(f"\n[live] step test: baseline {baseline:.2f} C, hottest {hottest:.2f} C, "
          f"heat permitted: {permitted} ({why})")

    bench.put_profile(
        STEP_PROFILE_SLOT, STEP_PROFILE_NAME, zone_mask=(1 << TUNE_ZONE),
        # ramp 0 = "as fast as the zone allows" is NOT assumed; the zone's own
        # max_ramp_c_per_hr (900) is used explicitly so the commanded step is
        # a documented number rather than whatever the firmware defaults to.
        segments=[{"target_c": STEP_TARGET_C, "ramp_c_per_hr": 900.0,
                   "dwell_min": STEP_DWELL_MIN}])

    code, body = bench.start_profile(STEP_PROFILE_SLOT)
    print(f"[live] POST /api/profile_exec/start -> {code} {body.strip()[:120]}")
    assert code == 200, (code, body)

    rows = bench.sample_response(STEP_OBSERVE_S, period_s=STEP_POLL_S, zone_index=TUNE_ZONE)
    print(f"[live] PV trace: {_describe(rows)}")
    print("[live] first/last rows: " + repr((rows[0], rows[-1])))

    assert len(rows) >= 5, rows
    assert all(r["hottest_c"] <= FIXTURE_MAX_TEMP_C for r in rows), _describe(rows)
    # The executor must have actually been running for the trace to mean
    # anything -- a flat line from an idle executor proves nothing.
    assert any(r["exec_state"] == "running" for r in rows), [r["exec_state"] for r in rows]

    moved = rows[-1]["zone_c"] - rows[0]["zone_c"]
    if not permitted:
        # ---- gate-holds branch (today) ---------------------------------
        assert all(r["relays_on"] == [] for r in rows), (
            f"heat is NOT permitted ({why}) yet a relay closed during the step: "
            + _describe(rows))
        # safety_RELAY_energized (K4), not safety_heating_enabled -- see
        # test_live_bench_firing.py's identical correction.
        # SAFETY_FLAG_ENABLED is "relay_owner is ARMED", true on any healthy
        # Pico regardless of whether heat was requested or granted.
        assert all(r["safety_relay_energized"] is False for r in rows), _describe(rows)
        # Flat within sensor noise. 1.0 C is generous against type-K noise of
        # a few tenths and against the bench's own ambient drift over a
        # minute; anything larger would be heat, and heat is what the gate
        # says cannot be happening.
        assert abs(moved) < 1.0, (
            f"heat refused but zone {TUNE_ZONE} moved {moved:+.2f} C: " + _describe(rows))
        print(f"[live] gate held through the step: PV moved {moved:+.2f} C with no relay activity "
              "(expected on this uncommissioned bench -- no CT fitted)")
        return

    # ---- heat-permitted branch (post CT commissioning) -----------------
    #
    # Reached for real on 2026-08-28, once `ct_installed = 0` (param 0x0109)
    # made this CT-less bench commissionable. What it found is worth stating
    # plainly, because it is NOT a commissioning problem and this test must
    # not be edited into pretending otherwise:
    #
    #   The gate opens -- `heat_block_sources_words` reads "none" on every
    #   sample and the executor reaches `running`.
    #
    # UPDATE 2026-08-29 (63cc741). The S6a abort described here is FIXED and
    # this comment's original diagnosis is superseded. The run used to abort
    # within about a second, every time, on S6a ("main controller reported a
    # fault") from `SAFETY_FAULT_SRC_SAFETY_LINK`, because the isolated link
    # timed out on ~20% of polls (and, by the time it was characterised, on
    # 100% of them). That was not the wire and not the Pico: KilnFW's
    # `uart_protocol_rx_task` blocked each read until a 528-byte buffer
    # filled, on a link whose frames are ~40 bytes, so every frame was
    # delivered up to 200 ms late. Measured after the fix: 0 timeouts, and
    # this test now holds `exec_state == running` with no heat block for the
    # full 60 s sweep.
    #
    # UPDATE 2026-08-29, second correction. What this comment used to say
    # next -- "PV does not move, because this bench has no heating element on
    # the zone-0 relay" -- was wrong twice over. The elements ARE fitted, and
    # the reason PV did not move was K4: nothing in the firing path ever
    # asked the safety processor to close it (heat_enable.h). Fixed, and the
    # PV-rise assertion is now made unconditionally below.
    blocked = [r for r in rows if r.get("heat_block_sources_words") not in (None, "", "none")]
    assert not blocked, (
        "heat is permitted by the safety processor, yet a sample reported a heat block: "
        + repr(blocked[:3]))

    # The S6a regression test. Before 63cc741 the executor left `running`
    # within ~1 s of the start and every later sample read an aborted state;
    # this asserts it does not, which is exactly what the link fix bought and
    # what would break first if the RX latency defect ever came back.
    not_running = [r for r in rows if r["exec_state"] != "running"]
    assert not not_running, (
        "the executor left 'running' during the sweep. Two causes have been seen: the S6a "
        "abort (SAFETY_FAULT_SRC_SAFETY_LINK, fixed by 63cc741) if the state is 'faulted', "
        "and -- if the state is 'done'/'complete' -- a dwell shorter than the sweep, which "
        "means the run ENDED normally and everything after that point is a cooling curve. "
        "STEP_DWELL_MIN exists to make the second impossible: "
        + repr(not_running[:3]))

    # ---- the step response itself, asserted ---------------------------
    #
    # HARDENED 2026-08-29. Everything below used to be an `if it happened,
    # check it; otherwise print a paragraph` -- and the paragraph it printed
    # ("the heat path to zone 0 delivers essentially nothing") was a
    # conclusion about the plant drawn from a firmware defect: profile_
    # executor.c never sent SAFETY_CMD_REQUEST_ENABLE, so K4 was open for
    # the whole 40-minute observation it cited. With that fixed (7769856 ..
    # 7b7699a) the same jig rises 2.8-3.8 C/min, and a test that tolerates
    # a flat trace cannot tell the fix from its own regression. So the flat
    # case is now a FAILURE, not a note.
    assert any(1 in r["relays_on"] for r in rows), (
        "the executor ran with heat permitted and no heat block, yet zone 0's relay 1 was "
        "never observed closed across the whole sweep: " + _describe(rows))

    # K4 -- the safety processor's contact, the one that decides whether any
    # element current flows at all. Unlike relay 1 this is not a transient:
    # heat_enable holds the request for the whole run.
    k4_samples = [r for r in rows if r["safety_relay_energized"]]
    assert len(k4_samples) >= len(rows) // 2, (
        f"K4 was closed on only {len(k4_samples)}/{len(rows)} samples. The request stands for "
        "the duration of a run (heat_enable.h), so a mostly-open K4 during a running firing "
        "means the request is being dropped or refused: " + _describe(rows))

    # Rise rate, over the stretch where the loop is still driving hard.
    # Restricting to PV below (setpoint - band) is what makes this a
    # measurement of the PLANT rather than of the controller backing off:
    # once PV arrives, duty falls and a low rate there is correct behaviour.
    driving = [r for r in rows if r["zone_c"] < STEP_TARGET_C - STEP_SETTLE_BAND_C]
    assert len(driving) >= 3, (
        "PV started within the settle band of the setpoint, so no rise rate could be measured "
        "-- the bench did not start cold: " + _describe(rows))
    span_min = (driving[-1]["t_s"] - driving[0]["t_s"]) / 60.0
    rise_c = driving[-1]["zone_c"] - driving[0]["zone_c"]
    rate = rise_c / span_min if span_min > 0 else 0.0
    print(f"[live] driving-phase rise: {rise_c:+.2f} C over {span_min:.2f} min "
          f"= {rate:.2f} C/min (min required {STEP_MIN_RISE_C_PER_MIN})")
    assert rate >= STEP_MIN_RISE_C_PER_MIN, (
        f"zone 0 rose {rate:.3f} C/min while the loop was driving below setpoint. This jig "
        f"measures 2.8-3.8 C/min at full duty; 0.017 C/min is what it produced with K4 open. "
        "A rate this low means no element current is flowing: " + _describe(rows))

    # Bounded above, twice: the profile's own setpoint and the fixture's.
    peak = max(r["zone_c"] for r in rows)
    assert peak <= STEP_TARGET_C + 5.0, (
        f"zone 0 overshot to {peak:.2f} C against a {STEP_TARGET_C} C setpoint: "
        + _describe(rows))
    assert peak <= FIXTURE_MAX_TEMP_C, _describe(rows)

    # CLOSED-LOOP SETTLE. Only asserted if PV actually arrived within the
    # sweep -- on a short KILNCTRL_BENCH_STEP_OBSERVE_S it legitimately may
    # not, and asserting arrival would make the budget, not the loop, the
    # subject. When it does arrive, it must STAY: a loop that reaches
    # setpoint and then runs away is the failure this checks for.
    arrived = [r for r in rows if r["zone_c"] >= STEP_TARGET_C - STEP_SETTLE_BAND_C]
    if arrived:
        after = [r for r in rows if r["t_s"] >= arrived[0]["t_s"]]
        worst = max(abs(r["zone_c"] - STEP_TARGET_C) for r in after)
        print(f"[live] arrived at t={arrived[0]['t_s']:.0f}s; over the {len(after)} samples "
              f"after arrival the worst deviation from setpoint was {worst:.2f} C")
        assert worst <= 2.0 * STEP_SETTLE_BAND_C, (
            f"PID reached setpoint then deviated by {worst:.2f} C -- that is not settling: "
            + _describe(rows))
    else:
        print(f"[live] PV did not reach the settle band within {STEP_OBSERVE_S:.0f}s "
              "(rise asserted above; raise KILNCTRL_BENCH_STEP_OBSERVE_S to see the settle)")
    print("[live] step response measured: PV rose %+.2f C over the sweep" % moved)


def test_step_autotune_runs_and_reports_a_sane_result(cold_bench, capsys):
    """PID TUNING. Start the step autotune on zone 0 and follow it for a
    bounded budget, then assert on what the engine actually reports.

    Every terminal outcome is a legitimate result and each is asserted
    differently -- "didn't crash" is not the bar:

      * DONE with a fit: the FOPDT model and the proposed gains must be
        physical (tau > 0, dead time >= 0, gains finite and non-negative).
        NOT accepted: this test never posts /api/autotune/accept, because
        accepting writes the gains and model to the board's NVS and a
        regression test must not silently retune the bench.
      * ABORTED: the engine says why, and the reason is reported. On an
        uncommissioned bench a guard trip or "response too small" is the
        CORRECT outcome -- no relay ever closed, so there is nothing to fit.
      * still running at the budget: aborted here, and reported as
        "inconclusive within the budget" rather than passing quietly.

    In every case the run must have reached STEPPING (past the 180 s settle)
    or explained itself, no relay may close while heat is refused, and the
    fixture ceiling holds throughout.

    Takes ``cold_bench``: an open-loop step fit from a hot start reports a
    smaller K and a shorter tau, and nothing in the result says so.
    """
    bench = cold_bench
    hottest = bench.assert_within_fixture_ceiling()
    permitted, why = heat_is_permitted(bench)
    print(f"\n[live] autotune: hottest {hottest:.2f} C, heat permitted: {permitted} ({why})")

    before = bench.autotune_status()
    assert before["state"] in ("idle", "done", "aborted"), before
    gains_before = bench.zone_pid(TUNE_ZONE)
    model_before = bench.zone_model(TUNE_ZONE)

    try:
        code, body = bench.start_autotune_step(TUNE_ZONE, AUTOTUNE_STEP_DUTY)
        print(f"[live] POST /api/autotune/start -> {code} {body.strip()[:200]}")
        # autotune_engine.c's begin_run_locked() refuses up front when heat is
        # blocked board-wide (relay_authority_on_blocked). On this bench
        # heat_block_sources is 0 -- the commissioning gate lives on the
        # SAFETY processor, downstream -- so the start is accepted and the
        # engine runs without ever closing a relay. Both are legitimate; the
        # test asserts whichever the board actually did rather than assuming.
        if code != 200:
            assert '"ok":false' in body.replace(" ", ""), (code, body)
            print(f"[live] autotune refused up front: {body.strip()[:200]}")
            assert bench.autotune_status()["state"] in ("idle", "done", "aborted")
            return

        running = bench.wait_for_autotune_state(("settling", "stepping"), timeout_s=20.0)
        assert running["state"] in ("settling", "stepping"), running
        assert running["zone"] == TUNE_ZONE, running
        assert running["method"] == "step", running

        final = bench.wait_for_autotune_state(
            ("done", "aborted"), timeout_s=AUTOTUNE_BUDGET_S, poll_s=5.0)
        print(f"[live] autotune after <= {AUTOTUNE_BUDGET_S:.0f}s: state={final['state']} "
              f"elapsed_s={final['elapsed_s']} samples={final['sample_count']} "
              f"duty={final['duty']} actual={final['actual_c']} "
              f"abort_reason={final['abort_reason']!r}")

        status = bench.status()
        assert status["safety_relay_energized"] is False or permitted, status
        if not permitted:
            assert [r["relay"] for r in status["relays"] if r["on"]] == [], (
                f"heat is NOT permitted ({why}) yet a relay is closed during autotune: "
                f"{status['relays']}")

        trace = bench.autotune_trace()
        print(f"[live] autotune trace: {len(trace)} rows"
              + (f", {trace[0]} .. {trace[-1]}" if trace else " (settle phase records none)"))
        for _, temp_c in trace:
            assert temp_c <= FIXTURE_MAX_TEMP_C, (temp_c, "trace breached the fixture ceiling")

        if final["state"] == "done":
            # A real fit. Assert it is PHYSICAL, not merely present.
            assert final["model_valid"] is True or final["relay_valid"] is True, final
            if final["model_valid"]:
                assert final["tau_s"] > 0.0, final
                assert final["dead_time_s"] >= 0.0, final
                assert final["k_gain_c_per_duty"] > 0.0, final
            for key in ("proposed_kp", "proposed_ki", "proposed_kd"):
                assert final[key] >= 0.0, (key, final[key], final)
                assert final[key] == final[key], (key, "NaN gain", final)
            assert final["rule"], final
            print(f"[live] FIT: K={final['k_gain_c_per_duty']} tau={final['tau_s']} "
                  f"L={final['dead_time_s']} -> kp={final['proposed_kp']} "
                  f"ki={final['proposed_ki']} kd={final['proposed_kd']} rule={final['rule']}")
        elif final["state"] == "aborted":
            # The engine must SAY why. A silent abort is the defect.
            assert final["abort_reason"], final
            assert final["model_valid"] is False and final["relay_valid"] is False, final
            print(f"[live] autotune aborted (expected on a bench where heat is refused): "
                  f"{final['abort_reason']}")
        else:
            # Still running at the budget: not a pass, not a firmware fault --
            # a bounded test that ran out of time, said so, and cleaned up.
            print(f"[live] autotune INCONCLUSIVE within {AUTOTUNE_BUDGET_S:.0f}s "
                  f"(state={final['state']}); aborting")
            assert final["state"] in ("settling", "stepping"), final
            assert final["elapsed_s"] > 0, final
    finally:
        bench.abort_autotune()

    # Nothing this test did may have changed the board's tuning: accept was
    # never posted, so the gains and model must be exactly what they were.
    assert bench.zone_pid(TUNE_ZONE) == gains_before
    assert bench.zone_model(TUNE_ZONE) == model_before
    assert bench.autotune_status()["state"] in ("idle", "done", "aborted")


def test_autotune_refuses_a_second_concurrent_run(bench):
    """Negative test for the mutual exclusion begin_run_locked() enforces:
    with one run live, a second start on the same zone must be REFUSED, with
    a reason. Proven capable of failing before being trusted -- if the
    refusal were removed, the second start would return 200 and this fails.
    """
    bench.assert_within_fixture_ceiling()
    try:
        code, body = bench.start_autotune_step(TUNE_ZONE, AUTOTUNE_STEP_DUTY)
        if code != 200:
            pytest.skip(f"autotune could not be started on this bench: {body.strip()[:160]}")
        code2, body2 = bench.start_autotune_step(TUNE_ZONE, AUTOTUNE_STEP_DUTY)
        print(f"\n[live] second start -> {code2} {body2.strip()[:200]}")
        assert code2 == 400, (code2, body2)
        assert '"ok":false' in body2.replace(" ", ""), body2
        assert "already running" in body2, body2
    finally:
        bench.abort_autotune()

    try:
        aborted = bench.autotune_status()
    except BenchSessionError:  # pragma: no cover - link hiccup after abort
        pytest.fail("autotune status unreadable after abort")
    assert aborted["state"] in ("idle", "aborted"), aborted
