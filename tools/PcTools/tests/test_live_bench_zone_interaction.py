#!/usr/bin/env python3
"""LIVE-BENCH: "auto zone interaction measurement".

WHICH INTERPRETATION, AND WHY. The phrase could mean two different things in
this codebase, and they are not the same feature:

  1. RAW THERMAL COUPLING -- fire one zone, watch whether the others warm.
     Easy to measure from /api/status alone, and genuinely interesting on a
     jig whose three zones share one small enclosure.
  2. THE COUPLING MATRIX AND ITS RGA -- ``autotune_engine.c``'s
     ``autotune_coupling_matrix_t`` (TODO.md 6A.5(b)) and
     ``pid_autotune_rga()`` (6A.5(c)). Cell K[i][j] is the FOPDT fit of zone
     j's response to a duty step on zone i, filled a ROW AT A TIME as
     autotune runs complete, and served with its Relative Gain Array at
     GET /api/autotune/matrix.

(2) is the feature this repo actually built and named, so it is the subject
here -- and it has never once run on real data. ``autotune_engine.h`` says so
in as many words: "no input cell has ever been filled on hardware ... this
has never run on measured data." A third candidate, ``ramp_lock_lagging_mask``
in the profile executor's status, was considered and rejected as the primary
subject: it is a SETPOINT-coordination mechanism (hold the shared ramp while
any participating zone lags by more than PROFILE_EXECUTOR_RAMP_LOCK_BAND_C),
not an interaction measurement. It is exercised by the multi-zone profile
test instead, which is where it belongs.

(1) is measured too, because it comes free: the same samples that fill the
matrix's cross-terms also answer "how much does zone 0's heat leak into
zones 1 and 2", in degrees, which is the number a multi-zone firing profile
on this jig actually has to live with. It is REPORTED rather than tightly
asserted -- the assertion on it is the safety one (no unfired zone runs
away), because a coupling figure is a property of this enclosure, not a
pass/fail criterion someone chose.

COST. An RGA needs at least a 2x2 fully-measured principal sub-block, which
means TWO completed autotune runs, in the SAME power cycle (the matrix lives
in the engine's RAM, not NVS). Each run is ~180 s of settle plus a few
minutes of step, plus a cooldown between them. Budget ~30 minutes.

    KILNCTRL_BENCH_HOST=192.168.1.156 python -m pytest \
        tools/PcTools/tests/test_live_bench_zone_interaction.py -q -s
"""
from __future__ import annotations

import os
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from bench_fixture_session import FIXTURE_MAX_TEMP_C, heat_is_permitted  # noqa: E402

pytestmark = pytest.mark.live_bench

#: The zones autotuned, in order, to fill rows of K. Zone 0 is the bench's
#: only closed-loop zone; zone 1 is the second zone with a real element and a
#: relay mask. Autotune does not require a control_mode -- begin_run_locked()
#: checks the relay mask, not the mode -- so a mode-0 zone can still be
#: stepped open-loop, which is exactly what filling K needs.
INTERACTION_ZONES = [0, 1]

#: Duty for each open-loop step. The engine's own default, inside the
#: 0.15/0.85 band its guard reasoning assumes.
STEP_DUTY = 0.5

#: Per-run budget. The first fit this board ever produced reached DONE at
#: elapsed_s=390; 900 s leaves room for a slower one without turning a
#: bounded test into an unbounded one.
RUN_BUDGET_S = float(os.environ.get("KILNCTRL_BENCH_AUTOTUNE_BUDGET_S", "900"))

#: A zone that is NOT being fired must not approach the fixture ceiling. This
#: is the safety assertion on the coupling measurement -- deliberately well
#: below 80 C, because a passive zone reaching even this is a finding.
PASSIVE_ZONE_CEILING_C = 60.0


def _fire_and_fit(bench, zone: int) -> dict:
    """One cold-start autotune run on `zone`, asserted to DONE with a
    physical fit, sampling every channel throughout. Returns the coupling
    observation plus the engine's own final status."""
    bench.wait_for_cooldown()
    bench.assert_within_fixture_ceiling()
    before = bench.channel_readings()
    start_c = {r["channel"]: r["temp_c"] for r in before}

    code, body = bench.start_autotune_step(zone, STEP_DUTY)
    print(f"\n[live] zone {zone}: POST /api/autotune/start -> {code} {body.strip()[:160]}")
    assert code == 200, (code, body)

    running = bench.wait_for_autotune_state(("settling", "stepping"), timeout_s=30.0)
    assert running["state"] in ("settling", "stepping"), running
    assert running["zone"] == zone, running

    # Follow the run, recording EVERY channel. wait_for_autotune_state polls
    # the engine but keeps no per-channel history, and the cross-zone rise is
    # the whole point here, so the sampling is done explicitly.
    peak_c = dict(start_c)
    deadline = time.monotonic() + RUN_BUDGET_S
    k4_seen = False
    final = bench.autotune_status()
    while final["state"] not in ("done", "aborted"):
        if time.monotonic() >= deadline:
            break
        time.sleep(10.0)
        for r in bench.channel_readings():
            peak_c[r["channel"]] = max(peak_c.get(r["channel"], r["temp_c"]), r["temp_c"])
        status = bench.status()
        if status["safety_relay_energized"]:
            k4_seen = True
        hottest = max(peak_c.values())
        assert hottest <= FIXTURE_MAX_TEMP_C, (
            f"zone {zone} autotune drove the bench to {hottest:.2f} C", peak_c)
        final = bench.autotune_status()

    rise = {ch: peak_c[ch] - start_c[ch] for ch in start_c}
    print(f"[live] zone {zone}: state={final['state']} elapsed_s={final['elapsed_s']} "
          f"K4 seen closed={k4_seen} abort={final['abort_reason']!r}")
    print(f"[live] zone {zone}: per-channel rise (C) {({k: round(v, 2) for k, v in rise.items()})}")

    # The run must have actually heated something, or the fit below is a fit
    # of noise. This is the K4 assertion again, restated as a temperature.
    assert k4_seen, (
        f"zone {zone}'s autotune ran without K4 ever being observed closed -- no element "
        "current flowed, so any model it reports is a model of ambient drift "
        "(see heat_enable.h)")
    assert final["state"] == "done", (
        f"zone {zone}'s autotune did not converge: state={final['state']}, "
        f"abort_reason={final['abort_reason']!r}, rises={rise}")
    assert final["model_valid"] is True, final
    assert final["k_gain_c_per_duty"] > 0.0, final
    assert final["tau_s"] > 0.0, final
    assert final["dead_time_s"] >= 0.0, final
    print(f"[live] zone {zone}: FIT K={final['k_gain_c_per_duty']:.2f} C/duty "
          f"tau={final['tau_s']:.1f}s L={final['dead_time_s']:.1f}s")

    # The safety half of the coupling measurement: an unfired zone may warm,
    # but it must not run away.
    for ch, r in rise.items():
        if ch == zone:
            continue
        assert peak_c[ch] <= PASSIVE_ZONE_CEILING_C, (
            f"channel {ch} was not being fired yet reached {peak_c[ch]:.2f} C while zone "
            f"{zone} was stepped -- that is more coupling than this jig is assumed to have")
    return {"zone": zone, "rise": rise, "peak_c": peak_c, "final": final}


def test_zone_interaction_coupling_matrix_and_rga(bench, capsys):
    """Fill two rows of the coupling matrix with real autotune runs, then
    assert the board can compute an RGA from them and that the RGA it
    computes is arithmetically sound.

    The RGA's own self-check is used as the assertion, rather than a
    hand-derived expected value: for ANY invertible K, every row and every
    column of Lambda sums to exactly 1 (Bristol, 1966). That is a property of
    the math, so it holds whatever this jig's thermal coupling turns out to
    be -- which makes it a real test of the firmware's implementation on real
    data, not a test of a number someone typed in.
    """
    permitted, why = heat_is_permitted(bench)
    if not permitted:
        pytest.skip(f"heat is not permitted on this bench ({why}); a coupling matrix cannot "
                    "be filled without real steps")

    observations = []
    try:
        for zone in INTERACTION_ZONES:
            observations.append(_fire_and_fit(bench, zone))
    finally:
        bench.abort_autotune()
        bench.force_all_stop()

    # ---- (1) raw thermal coupling, reported ---------------------------
    print("\n[live] CROSS-ZONE THERMAL COUPLING, measured:")
    for obs in observations:
        fired = obs["zone"]
        own = obs["rise"][fired]
        for ch in sorted(obs["rise"]):
            if ch == fired:
                continue
            frac = (obs["rise"][ch] / own) if own > 0 else float("nan")
            print(f"[live]   firing zone {fired}: channel {ch} rose {obs['rise'][ch]:+.2f} C "
                  f"vs zone {fired}'s {own:+.2f} C  ({frac * 100:.1f}% of it)")
        assert own > 0.0, f"the fired zone {fired} did not rise at all: {obs['rise']}"

    # ---- (2) the coupling matrix and the RGA --------------------------
    matrix = bench.autotune_matrix()
    print(f"\n[live] /api/autotune/matrix: {matrix}")

    cells = {(c["i"], c["j"]): c for c in matrix["cells"]}
    for zone in INTERACTION_ZONES:
        assert cells[(zone, zone)]["valid"] is True, (
            f"zone {zone}'s autotune reported DONE with a valid model, yet K[{zone}][{zone}] "
            "is not filled -- finalize_fit() did not write the row it was supposed to")

    rga = matrix.get("rga")
    assert rga is not None, matrix
    assert rga["available"] is True, (
        "two zones were autotuned to a valid fit in this power cycle, yet the board still "
        f"cannot compute an RGA: {rga}. This is the first time this code path has ever seen "
        "real hardware data (autotune_engine.h).")

    n = rga["n"]
    assert n >= 2, rga
    lam = rga["lambda"]
    assert len(lam) == n and all(len(row) == n for row in lam), rga

    # Bristol's identity. The tolerance is float32 round-trip through JSON at
    # 4 decimal places, not a fudge factor for a wrong answer.
    for a in range(n):
        assert sum(lam[a]) == pytest.approx(1.0, abs=2e-3), (a, lam[a], rga)
    for b in range(n):
        assert sum(lam[a][b] for a in range(n)) == pytest.approx(1.0, abs=2e-3), (b, lam, rga)

    assert all(v == v for row in lam for v in row), ("RGA contains NaN", rga)
    assert rga["det"] == rga["det"] and rga["det"] != 0.0, rga

    print(f"[live] RGA over zones {rga['zones']} (det {rga['det']:.4g}):")
    for a in range(n):
        print("[live]   " + "  ".join(f"{v:+.4f}" for v in lam[a]))
    diag = [lam[a][a] for a in range(n)]
    print(f"[live] RGA diagonal {['%+.4f' % d for d in diag]} -- 1.0 means the loops do not "
          "interact; far from 1 means per-zone PID is fighting itself; negative is the "
          "pathological case.")

    # The one INTERPRETIVE assertion, and it is deliberately weak: a negative
    # diagonal element would mean closing the other loops REVERSES this
    # loop's own gain, which is the case where decentralized per-zone PID
    # actively destabilizes itself. That is not a tuning preference, it is a
    # statement that the control architecture this firmware implements is
    # wrong for this plant -- worth failing on.
    assert all(d > 0.0 for d in diag), (
        "an RGA diagonal element is negative: closing the other zones' loops reverses this "
        f"zone's own gain, and per-zone PID is not a legitimate architecture here. {rga}")
