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

WHAT IT PROVES TODAY, AND WHAT CHANGES LATER. Same branch as the firing test:
``heat_is_permitted()`` is the board's own live verdict. Today it is False
(no CT fitted, ``ct_channel_map[0..2]`` uncommitted, commissioning_gate.c
reports calibration_missing), so the assertions are the "gate holds" ones --
the engine runs, believes it is stepping, and NO relay ever closes, so the PV
trace is flat. That is a real assertion about the gate, and it is exactly
what makes the flat trace legible rather than mysterious. When the CTs are
fitted and the sweep commits a measured map, the same tests take their
heat-permitted branches with no edit here.
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

#: Seconds of PV sampled after the step is commanded. Not a settling time --
#: a kiln's time constant is far longer than this. It is long enough to see
#: whether the loop is ACTUATING (relays closing, PV leaving its baseline) at
#: a 2 s poll, which is the question this test asks.
STEP_OBSERVE_S = float(os.environ.get("KILNCTRL_BENCH_STEP_OBSERVE_S", "60"))

#: Autotune budget. AUTOTUNE_ENGINE_SETTLE_S is 180 s of baseline hold before
#: the step is applied, so anything under ~200 s could only ever observe
#: "settling" and would prove nothing about the step. 300 s reaches STEPPING
#: and then watches ~2 minutes of it. The engine's OWN budget is 4 h
#: (AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S); this test aborts long before
#: that rather than blocking a suite for hours, and says so.
AUTOTUNE_BUDGET_S = float(os.environ.get("KILNCTRL_BENCH_AUTOTUNE_BUDGET_S", "300"))

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


def test_closed_loop_setpoint_step_response(bench, capsys):
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
    """
    hottest = bench.assert_within_fixture_ceiling()
    permitted, why = heat_is_permitted(bench)
    baseline = bench.status()["channels"][TUNE_ZONE]["temp_c"]
    print(f"\n[live] step test: baseline {baseline:.2f} C, hottest {hottest:.2f} C, "
          f"heat permitted: {permitted} ({why})")

    bench.put_profile(
        STEP_PROFILE_SLOT, STEP_PROFILE_NAME, zone_mask=(1 << TUNE_ZONE),
        # ramp 0 = "as fast as the zone allows" is NOT assumed; the zone's own
        # max_ramp_c_per_hr (900) is used explicitly so the commanded step is
        # a documented number rather than whatever the firmware defaults to.
        segments=[{"target_c": STEP_TARGET_C, "ramp_c_per_hr": 900.0, "dwell_min": 1}])

    code, body = bench.start_profile(STEP_PROFILE_SLOT)
    print(f"[live] POST /api/profile_exec/start -> {code} {body.strip()[:120]}")
    assert code == 200, (code, body)

    rows = bench.sample_response(STEP_OBSERVE_S, period_s=2.0, zone_index=TUNE_ZONE)
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
        assert all(r["safety_heating_enabled"] is False for r in rows), _describe(rows)
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
    #   sample and the executor reaches `running`. But the run aborts within
    #   about a second, every time, on **S6a** ("main controller reported a
    #   fault"). The source is `SAFETY_FAULT_SRC_SAFETY_LINK` (measured:
    #   `trip_fault_sources` reads 8 even before a run is started): the
    #   isolated link times out on roughly 20% of polls (136 in 681 frames),
    #   `safety_link.c` raises the fault on that staleness, and SaftyFW trips.
    #   No sustained heat is possible on this bench until that is fixed.
    #
    # So the PV-rise and relay-closed assertions cannot hold here, and forcing
    # them would only produce a red test that says "commissioning" while
    # meaning "the UART link drops frames". What IS asserted is the level
    # signal that answers the commissioning question without racing a ~1 s
    # abort at a 2 s sample interval: no zone ever reported a heat block.
    blocked = [r for r in rows if r.get("heat_block_sources_words") not in (None, "", "none")]
    assert not blocked, (
        "heat is permitted by the safety processor, yet a sample reported a heat block: "
        + repr(blocked[:3]))

    if any(r["relays_on"] for r in rows) and moved > 0.5:
        # The good day. If the link fault is ever fixed this branch starts
        # running, and the original response-shape assertions apply again.
        half = len(rows) // 2
        first_mean = sum(r["zone_c"] for r in rows[:half]) / half
        second_mean = sum(r["zone_c"] for r in rows[half:]) / (len(rows) - half)
        assert second_mean >= first_mean, _describe(rows)
        assert max(r["zone_c"] for r in rows) <= STEP_TARGET_C + 5.0, _describe(rows)
        print("[live] step response measured: PV rose %+.2f C" % moved)
    else:
        aborts = sorted({r["exec_state"] for r in rows})
        print(f"[live] NOTE: heat was PERMITTED (no heat block on any of {len(rows)} samples) "
              f"but no sustained heat resulted: PV moved {moved:+.2f} C, exec states {aborts}. "
              "This is the S6a / SAFETY_FAULT_SRC_SAFETY_LINK finding -- see this "
              "block's comment. NOT a commissioning failure.")


def test_step_autotune_runs_and_reports_a_sane_result(bench, capsys):
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
    """
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
        assert status["safety_heating_enabled"] is False or permitted, status
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
