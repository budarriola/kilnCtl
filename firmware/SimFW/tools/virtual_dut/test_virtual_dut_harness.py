#!/usr/bin/env python3
"""Regression coverage for the two harness defects fixed in this pass, both
of them documented in this directory's README.md "Findings" section:

  1. **Manufactured bad-read streaks.** The old batching rule replayed ONE
     telemetry-derived TC sample across many 100 ms guard ticks, so a single
     bad sample could synthesize a 10+ consecutive-bad-read streak that the
     fixture never observed -- and safety_guards.c's S5/S1 bars count *reads*,
     not only seconds. `tc_flaky`'s `no_warn_storm` FAILed on exactly that.
     The fix is one tick per observed sample, with `dt_s` carrying the real
     elapsed sim time (`plan_tick_dt_ms`, and `dut_core.exe`'s new optional
     trailing `<dt_ms>` TICK field).

  2. **S9's persisted current had no observable.** `FT_WELDED_K4_CURRENT_
     PERSIST` drives the CT channel's wave synthesis directly and bypasses
     `duty[]`/`current_a[]` (real `sim_engine.c` does the same, by design),
     while the harness read per-zone `i_amps`, the MODEL current, which K4
     gates to zero. The fix reads the CT channels through the fixture's own
     real `CT_GET_STATE` command (`read_ct_state`) and uses the CT value for
     any channel the model path cannot express (`telemetry_with_ct_amps`).

Run: python firmware/SimFW/tools/virtual_dut/test_virtual_dut_harness.py
(or `pytest` it -- every test is a plain no-argument function). Tests that
need a built `dut_core.exe`/`virtual_simfw.exe` SKIP when it is missing rather
than failing, the same way this directory's runner reports a missing build.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import run_dut_scenarios as rds  # noqa: E402


class SkipTest(Exception):
    """Raised by a test that cannot run here (missing build artifact)."""


# ---------------------------------------------------------------------------
# MAX31856 register images (see dut_core/max31856_decode.c for the layout)
# ---------------------------------------------------------------------------
REG_CJTH, REG_CJTL = 0x0A, 0x0B
REG_LTCBH, REG_LTCBM, REG_LTCBL, REG_SR = 0x0C, 0x0D, 0x0E, 0x0F
FAULT_OPEN = 0x01


def regs_image(tc_c: float = 25.0, cj_c: float = 25.0, sr: int = 0) -> bytes:
    regs = bytearray(16)
    cj_raw = int(round(cj_c * 256.0)) & 0xFFFF
    regs[REG_CJTH] = (cj_raw >> 8) & 0xFF
    regs[REG_CJTL] = cj_raw & 0xFF
    tc_raw = (int(round(tc_c * 4096.0)) & 0x00FFFFE0) & 0xFFFFFF
    regs[REG_LTCBH] = (tc_raw >> 16) & 0xFF
    regs[REG_LTCBM] = (tc_raw >> 8) & 0xFF
    regs[REG_LTCBL] = tc_raw & 0xFF
    regs[REG_SR] = sr & 0xFF
    return bytes(regs)


GOOD_REGS = regs_image(tc_c=25.0)
BAD_REGS = regs_image(tc_c=0.0, sr=FAULT_OPEN)  # open circuit -> S5's "bad read"

# A quiet, healthy context: link up, context fresh, no relay commanded, no
# current. Shaped exactly like _FixtureContext.observe()'s return value.
QUIET_CTX = {
    "link_up": True, "ctx_present": True, "ctx_degraded": False, "ctx_age_ms": 0,
    "relay_now_mask": 0, "relay_recent_mask": 0, "relay_on_continuous_ms": 0,
    "amps": [0.0, 0.0, 0.0],
    "zones": [{"flags": 0x03, "setpoint_c": float("nan"), "measured_c": 25.0,
               "sample_counter": 0}],
}


def _dut():
    if not rds.DUT_CORE_EXE.exists():
        raise SkipTest(f"{rds.DUT_CORE_EXE} not built")
    return rds.DutCore(rds.DUT_CORE_EXE)


# ---------------------------------------------------------------------------
# 1. The batching rule itself (pure, no subprocess)
# ---------------------------------------------------------------------------
def test_no_tick_at_all_when_the_fixture_published_no_new_sample():
    # The fixture republishing the same sim time is not an observation, and
    # the old max(1, ...) floor charged the guards a full 100 ms tick for it.
    assert rds.plan_tick_dt_ms(5_000_000, 5_000_000) is None
    assert rds.plan_tick_dt_ms(4_000_000, 5_000_000) is None
    assert rds.plan_tick_dt_ms(5_000_500, 5_000_000) is None  # sub-millisecond


def test_one_tick_carries_the_true_elapsed_sim_time():
    # 5 sim-seconds elapsed == ONE tick of dt 5000 ms, not fifty ticks of 100.
    assert rds.plan_tick_dt_ms(5_000_000, 0) == 5000
    assert rds.plan_tick_dt_ms(5_100_000, 5_000_000) == 100
    # ... and a poll shorter than the safety-core period is NOT rounded up to
    # one (README.md Finding 8's "guard clock runs fast" bias).
    assert rds.plan_tick_dt_ms(5_020_000, 5_000_000) == 20


def test_tick_count_equals_sample_count_over_a_whole_run():
    # The invariant the fix exists to establish: N observed samples produce
    # exactly N guard ticks, whatever the timescale, and the integrated dt
    # equals the elapsed sim time exactly.
    samples = [1_000_000 * i for i in range(1, 51)]  # 50 samples, 1 s apart
    last = 0
    dts = []
    for s in samples:
        dt = rds.plan_tick_dt_ms(s, last)
        if dt is not None:
            dts.append(dt)
            last = s
    assert len(dts) == len(samples)
    assert sum(dts) == 50_000


# ---------------------------------------------------------------------------
# 2. The guard-visible consequence, against the real safety_guards.c
# ---------------------------------------------------------------------------
def test_one_bad_sample_cannot_synthesize_a_bad_read_streak():
    """The actual `tc_flaky` defect: one bad TC sample covering 5 s of sim
    time must count as ONE bad read, not as fifty. S5's warn needs 10
    consecutive bad reads AND 5 s (safety_guards.c's BAD_READ_COUNT_DEFAULT /
    BAD_READ_TIME_S_DEFAULT); the time bar alone must never be enough."""
    dut = _dut()
    try:
        dut.reset()
        r = dut.tick(BAD_REGS, False, QUIET_CTX, dt_ms=5000)
        assert not r["s5_warn"], "one bad sample cleared S5's 10-read bar"
        assert not r["is_tripped"]
        # Same 5 s of sim time, but genuinely observed as 10 separate bad
        # samples: now the count bar is legitimately cleared.
        dut.reset()
        for _ in range(10):
            r = dut.tick(BAD_REGS, False, QUIET_CTX, dt_ms=500)
        assert r["s5_warn"], "10 real consecutive bad reads should warn"
    finally:
        dut.close()


def test_alternating_good_bad_samples_never_warn():
    """`tc_flaky`'s scenario shape, seen honestly: a bad sample followed by a
    good one resets S5's streak (safety_guards.c: `state->s5_bad_streak = 0`),
    so no amount of flapping can reach the 10-read bar. Before the fix each
    single bad sample was replayed across a whole batch and manufactured the
    streak by itself."""
    dut = _dut()
    try:
        dut.reset()
        for i in range(200):
            r = dut.tick(BAD_REGS if i % 2 == 0 else GOOD_REGS, False, QUIET_CTX, dt_ms=900)
            assert not r["s5_warn"], f"S5 warned on alternating samples at i={i}"
            assert not r["is_tripped"], f"S5 tripped on alternating samples at i={i}"
    finally:
        dut.close()


def test_grace_timer_measures_elapsed_time_not_tick_count():
    """relay_owner's 60 s SAFTYFW_STARTUP_GRACE_MS is a TIME, so one tick of
    60 s must expire it and one tick of 59 s must not -- the bookkeeping now
    accumulates dt_ms rather than counting calls."""
    dut = _dut()
    try:
        dut.reset()
        dut.tick(GOOD_REGS, False, QUIET_CTX, dt_ms=59_000)
        assert dut.enable(True) is False, "K4 energized before the 60 s grace expired"
        dut.reset()
        dut.tick(GOOD_REGS, False, QUIET_CTX, dt_ms=60_000)
        assert dut.enable(True) is True, "K4 did not arm after a full 60 s of grace"
    finally:
        dut.close()


# ---------------------------------------------------------------------------
# 3. The CT-amps observable (S9's persisted current)
# ---------------------------------------------------------------------------
def test_telemetry_with_ct_amps_substitutes_only_manual_channels():
    telemetry = {"sim_time_us": 1, "zones": [{"i_amps": 0.0}, {"i_amps": 3.0}]}
    # ch0 MANUAL (a persisted, K4-bypassing current the model cannot express)
    # -> substituted; ch1 MODEL -> the frame's own, consistently-stamped value
    # is kept, so a live readback cannot drag an edge ahead of the EVT-stream
    # edge that caused it.
    out = rds.telemetry_with_ct_amps(telemetry, [(1, 20.0), (0, 0.0), (0, 0.0)])
    assert [z["i_amps"] for z in out["zones"]] == [20.0, 3.0]
    # The caller's sample is not mutated -- the substitution is a view.
    assert telemetry["zones"][0]["i_amps"] == 0.0


def test_ct_readback_sees_current_the_model_path_cannot():
    """The GAP-2 regression proper, end to end against the real fixture: a CT
    channel forced to MANUAL amps (what FT_WELDED_K4_CURRENT_PERSIST does on a
    K4-open edge) reports current through CT_GET_STATE while telemetry's
    per-zone i_amps -- the MODEL value -- stays 0, because no zone relay is
    closed and K4 is open. Reading i_amps was why S9's "K4 open but current
    still flowing" half was unobservable here."""
    if not rds.VIRTUAL_SIMFW_EXE.exists():
        raise SkipTest(f"{rds.VIRTUAL_SIMFW_EXE} not built")
    import time

    from kilnsim.link import TcpSimLink
    from kilnsim.protocol import CommandGroup, CtCmd

    sim_proc, port = rds.start_virtual_simfw(1)
    link = TcpSimLink()
    try:
        link.connect(f"127.0.0.1:{port}")
        link.send_command(CommandGroup.CT, CtCmd.SET_MODE, {"channel": 0, "mode": 1})
        link.send_command(CommandGroup.CT, CtCmd.SET_AMPS, {"channel": 0, "amps": 20.0})

        deadline = time.time() + 5.0
        amps = [0.0, 0.0, 0.0]
        while time.time() < deadline:
            ct_state = rds.read_ct_state(link)
            amps = [a for (_m, a) in ct_state]
            if amps[0] > 1.0:
                break
        assert amps[0] > 19.0, f"CT readback did not see the forced amps: {amps}"
        assert amps[1] == 0.0 and amps[2] == 0.0

        deadline = time.time() + 5.0
        telemetry = {}
        while time.time() < deadline:
            telemetry = link.get_last_telemetry() or {}
            if telemetry.get("zones"):
                break
            time.sleep(0.1)
        assert telemetry.get("zones"), "no telemetry with zones arrived"
        model_amps = [float(z.get("i_amps", 0.0)) for z in telemetry.get("zones", [])]
        assert all(a < 0.05 for a in model_amps), (
            f"expected the MODEL current path to stay dark, got {model_amps}"
        )
        assert ct_state[0][0] == 1, "channel 0 should be in MANUAL mode"
        merged = rds.telemetry_with_ct_amps(telemetry, ct_state)
        assert float(merged["zones"][0]["i_amps"]) > 19.0
    finally:
        link.disconnect()
        sim_proc.terminate()
        try:
            sim_proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            sim_proc.kill()


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = skipped = 0
    for t in tests:
        try:
            t()
        except SkipTest as exc:
            skipped += 1
            print(f"  SKIP  {t.__name__}: {exc}")
        except AssertionError as exc:
            failed += 1
            print(f"  FAIL  {t.__name__}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failed += 1
            print(f"  ERROR {t.__name__}: {exc!r}")
        else:
            print(f"  ok    {t.__name__}")
    print(f"{len(tests) - failed - skipped} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
