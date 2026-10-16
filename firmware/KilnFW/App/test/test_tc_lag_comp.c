// Host tests for tc_lag_comp.c (thermocouple protection-tube lag
// compensation) and for the thermal_guard lead_c input it feeds.
//
// What these pin:
//   - tau 0 is an exact passthrough (byte-identical to no compensation);
//   - on a steady ramp the estimate converges to meas + tau * rate;
//   - the correction is clamped to +/-TC_LAG_COMP_MAX_CORR_C;
//   - every reset trigger (invalid, NaN, long gap, glitch, tau change)
//     drops the correction to 0 and reports out_reset only when a non-zero
//     correction was thrown away;
//   - the guards never react later with a lead than without one.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/control/tc_lag_comp.h"
#include "../drivers/control/thermal_guard.h"
#include "../persist/zones_config_accessors.h"

/* Bounds shared with the zone setting (profile_executor.c asserts the same
 * at compile time for the target build). */
_Static_assert(TC_LAG_COMP_TAU_MAX_S == (unsigned)ZONE_TC_SHEATH_TAU_MAX_S, "tau max mismatch");
_Static_assert(TC_LAG_COMP_TAU_MIN_S == (unsigned)ZONE_TC_SHEATH_TAU_MIN_S, "tau min mismatch");

/* Drive a steady ramp for n ticks; returns the last estimate. */
static float ramp(tc_lag_comp_state_t *st, float tau, float start_c, float rate_c_per_s, float dt, int n,
                  float *last_meas)
{
    float est = start_c;
    float m = start_c;
    for (int i = 0; i < n; i++) {
        est = tc_lag_comp_step(st, tau, m, true, dt, NULL);
        *last_meas = m;
        m += rate_c_per_s * dt;
    }
    return est;
}

static thermal_guard_input_t guard_in(void)
{
    thermal_guard_input_t in = {0};
    in.sensor_ok = true;
    in.measurement_c = 20.0f;
    in.setpoint_c = 20.0f;
    in.dt_s = 10.0f;
    return in;
}

/* Ticks until the guard trips (or n if it never does). lead_per_tick grows
 * the lead each tick; meas_per_tick the raw reading. */
static int ticks_to_trip(float sanity, float setpoint, float duty, float meas0, float meas_per_tick,
                         float lead_per_tick, int n, thermal_guard_trip_t *reason)
{
    thermal_guard_state_t s;
    thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = sanity};
    thermal_guard_reset(&s);
    thermal_guard_input_t in = guard_in();
    in.setpoint_c = setpoint;
    in.commanded_duty = duty;
    in.measurement_c = meas0;
    for (int i = 0; i < n; i++) {
        in.measurement_c += meas_per_tick;
        in.lead_c = lead_per_tick * (float)i;
        if (thermal_guard_tick(&s, &cfg, &in)) {
            *reason = s.reason;
            return i;
        }
    }
    *reason = THERMAL_GUARD_TRIP_NONE;
    return n;
}

void run_test_tc_lag_comp(void)
{
    TEST_SECTION("tc_lag_comp");

    /* tau 0: exact passthrough, correction stays 0, never primes. */
    {
        tc_lag_comp_state_t st;
        memset(&st, 0, sizeof(st));
        bool exact = true;
        float m = 20.0f;
        for (int i = 0; i < 500; i++) {
            m += (i % 7 == 0) ? 3.3f : 0.17f;
            bool r = true;
            float est = tc_lag_comp_step(&st, 0.0f, m, true, 1.0f, &r);
            if (memcmp(&est, &m, sizeof(float)) != 0 || st.corr_c != 0.0f || r) exact = false;
        }
        TEST_CHECK(exact, "tau 0 returns the input bit-for-bit with zero correction and no reset");
        TEST_CHECK(!st.primed, "tau 0 never primes the filter");
        float n = NAN;
        TEST_CHECK(isnan(tc_lag_comp_step(&st, 0.0f, n, false, 1.0f, NULL)), "tau 0 passes NaN through");
    }

    /* Steady ramp: estimate converges to meas + tau * rate. */
    {
        const float taus[] = {60.0f, 180.0f, 300.0f};
        for (unsigned k = 0; k < sizeof(taus) / sizeof(taus[0]); k++) {
            tc_lag_comp_state_t st;
            memset(&st, 0, sizeof(st));
            float rate = 300.0f / 3600.0f; /* 300 C/h */
            float last = 0.0f;
            float est = ramp(&st, taus[k], 100.0f, rate, 1.0f, 1200, &last);
            TEST_CHECK_NEAR(est - last, taus[k] * rate, 0.05f * taus[k] * rate,
                            "ramp: correction converges to tau * dT/dt");
        }
    }

    /* Clamp: a fast rise through a long tau never moves the estimate more
     * than TC_LAG_COMP_MAX_CORR_C from the reading. */
    {
        tc_lag_comp_state_t st;
        memset(&st, 0, sizeof(st));
        float last = 0.0f;
        float est = ramp(&st, 600.0f, 100.0f, 1.0f, 1.0f, 600, &last);
        TEST_CHECK_NEAR(est - last, TC_LAG_COMP_MAX_CORR_C, 1e-3f, "clamp holds at +60 C on a 1 C/s rise");
        memset(&st, 0, sizeof(st));
        est = ramp(&st, 600.0f, 1000.0f, -1.0f, 1.0f, 600, &last);
        TEST_CHECK_NEAR(est - last, -TC_LAG_COMP_MAX_CORR_C, 1e-3f, "clamp holds at -60 C on a 1 C/s fall");
    }

    /* Reset triggers: each drops to raw, reports out_reset, then re-grows. */
    {
        for (int trig = 0; trig < 5; trig++) {
            tc_lag_comp_state_t st;
            memset(&st, 0, sizeof(st));
            float last = 0.0f;
            (void)ramp(&st, 180.0f, 100.0f, 0.1f, 1.0f, 300, &last);
            TEST_CHECK(st.corr_c > 1.0f, "reset setup: correction built up");
            uint32_t resets0 = st.resets;
            bool r = false;
            float m = last + 0.1f;
            float tau = 180.0f, dt = 1.0f;
            bool valid = true;
            if (trig == 0) valid = false;
            if (trig == 1) m = NAN;
            if (trig == 2) dt = TC_LAG_COMP_MAX_DT_S + 1.0f;
            if (trig == 3) m = last + TC_LAG_COMP_GLITCH_C + 1.0f;
            if (trig == 4) tau = 181.0f;
            if (trig == 3) {
                /* First tick of a jump is held: raw out, no correction, no reset yet. */
                float held = tc_lag_comp_step(&st, tau, m, valid, dt, &r);
                TEST_CHECK(held == m && st.corr_c == 0.0f && st.resets == resets0 && r,
                           "glitch tick 1: held (raw out, corr 0, PID re-base asked, filter kept)");
                r = false;
                m += 0.1f; /* the jump persists: a real step */
            }
            float est = tc_lag_comp_step(&st, tau, m, valid, dt, &r);
            TEST_CHECK(r || trig == 3, "reset trigger reports out_reset when a correction was discarded");
            TEST_CHECK(st.resets == resets0 + 1u, "reset trigger bumps the reset counter");
            TEST_CHECK(st.corr_c == 0.0f, "reset trigger zeroes the correction");
            if (valid && isfinite(m)) {
                TEST_CHECK(est == m, "first tick after a reset returns the raw reading");
            } else {
                TEST_CHECK(!st.primed, "invalid reading leaves the filter unprimed");
            }
        }
        /* Turning tau off with a correction in flight is also a reset. */
        tc_lag_comp_state_t st;
        memset(&st, 0, sizeof(st));
        float last = 0.0f;
        (void)ramp(&st, 120.0f, 100.0f, 0.1f, 1.0f, 300, &last);
        bool r = false;
        float m = last + 0.1f;
        TEST_CHECK(tc_lag_comp_step(&st, 0.0f, m, true, 1.0f, &r) == m && r, "tau 0 after use resets and passes through");
        /* An unprimed reset (no correction) does not ask for a PID re-base. */
        r = true;
        memset(&st, 0, sizeof(st));
        (void)tc_lag_comp_step(&st, 120.0f, 50.0f, true, 1.0f, &r);
        TEST_CHECK(!r, "priming tick is not a reset");
    }

    /* A lone one-tick spike is held, not a reset: the correction is back on
     * the very next tick, close to where it was. */
    {
        tc_lag_comp_state_t st;
        memset(&st, 0, sizeof(st));
        float last = 0.0f;
        (void)ramp(&st, 180.0f, 100.0f, 0.1f, 1.0f, 300, &last);
        float before = st.corr_c;
        uint32_t resets0 = st.resets;
        (void)tc_lag_comp_step(&st, 180.0f, last + 0.1f + 15.0f, true, 1.0f, NULL);
        (void)tc_lag_comp_step(&st, 180.0f, last + 0.3f, true, 1.0f, NULL);
        TEST_CHECK(st.resets == resets0, "lone spike: no filter reset");
        TEST_CHECK_NEAR(st.corr_c, before, 0.1f * before, "lone spike: correction resumes where it was");
    }

    /* Filter time constant bounds. */
    TEST_CHECK_NEAR(tc_lag_comp_filter_tau_s(4.0f), 5.0f, 1e-6f, "tf floor 5 s");
    TEST_CHECK_NEAR(tc_lag_comp_filter_tau_s(120.0f), 30.0f, 1e-6f, "tf = tau/4");
    TEST_CHECK_NEAR(tc_lag_comp_filter_tau_s(600.0f), 60.0f, 1e-6f, "tf ceiling 60 s");

    /* Noise: 0.25 C peak-to-peak alternating noise on a hold stays small. */
    {
        tc_lag_comp_state_t st;
        memset(&st, 0, sizeof(st));
        float worst = 0.0f;
        for (int i = 0; i < 2000; i++) {
            float m = 1000.0f + ((i & 1) ? 0.125f : -0.125f);
            float est = tc_lag_comp_step(&st, 300.0f, m, true, 1.0f, NULL);
            if (i > 100 && fabsf(est - 1000.0f) > worst) worst = fabsf(est - 1000.0f);
        }
        TEST_CHECK(worst < 2.0f, "tau 300: alternating 0.25 C noise amplified to under 2 C");
    }

    TEST_SECTION("thermal_guard lead_c (sheath compensation)");

    /* Guard 5: raw + max(0, lead) -- a positive lead trips earlier, a
     * negative lead never hides a raw over-limit, NaN lead is 0. */
    {
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_state_t s;
        thermal_guard_input_t in = guard_in();

        thermal_guard_reset(&s);
        in.measurement_c = 1296.0f;
        in.lead_c = 5.0f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) && s.reason == THERMAL_GUARD_TRIP_MAX_TEMP,
                   "guard 5 trips on raw 1296 + lead 5 over a 1300 ceiling");

        thermal_guard_reset(&s);
        in.lead_c = 0.0f;
        TEST_CHECK(!thermal_guard_tick(&s, &cfg, &in), "guard 5 does not trip on raw 1296 with lead 0");

        thermal_guard_reset(&s);
        in.measurement_c = 1301.0f;
        in.lead_c = -50.0f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) && s.reason == THERMAL_GUARD_TRIP_MAX_TEMP,
                   "guard 5: negative lead never hides a raw over-limit");

        thermal_guard_reset(&s);
        in.measurement_c = 1299.0f;
        in.lead_c = NAN;
        TEST_CHECK(!thermal_guard_tick(&s, &cfg, &in), "guard 5: NaN lead is treated as 0");
    }

    /* Guard 1: a growing lead must not delay heating-failed (the guard uses
     * the smaller of the raw and compensated window rise). */
    {
        thermal_guard_trip_t r0, r1;
        int t0 = ticks_to_trip(10.0f, 500.0f, 1.0f, 20.0f, 0.01f, 0.0f, 200, &r0);
        int t1 = ticks_to_trip(10.0f, 500.0f, 1.0f, 20.0f, 0.01f, 2.0f, 200, &r1);
        TEST_CHECK(r0 == THERMAL_GUARD_TRIP_HEATING_FAILED, "guard 1 baseline trips");
        TEST_CHECK(r1 == THERMAL_GUARD_TRIP_HEATING_FAILED && t1 <= t0,
                   "guard 1: a rising lead never trips later than raw");
        int t2 = ticks_to_trip(10.0f, 500.0f, 1.0f, 20.0f, 0.01f, -2.0f, 200, &r1);
        TEST_CHECK(r1 == THERMAL_GUARD_TRIP_HEATING_FAILED && t2 <= t0,
                   "guard 1: a falling lead never trips later than raw");
    }

    /* Guard 3: runaway with heat off -- the lead only ever makes it earlier. */
    {
        thermal_guard_trip_t r0, r1;
        int t0 = ticks_to_trip(0.5f, 20.0f, 0.0f, 200.0f, 3.0f, 0.0f, 200, &r0);
        int t1 = ticks_to_trip(0.5f, 20.0f, 0.0f, 200.0f, 3.0f, 3.0f, 200, &r1);
        int t2 = ticks_to_trip(0.5f, 20.0f, 0.0f, 200.0f, 3.0f, -3.0f, 200, &r1);
        TEST_CHECK(r0 == THERMAL_GUARD_TRIP_RUNAWAY, "guard 3 baseline trips");
        TEST_CHECK(t1 <= t0, "guard 3: a rising lead trips no later than raw");
        TEST_CHECK(r1 == THERMAL_GUARD_TRIP_RUNAWAY && t2 <= t0, "guard 3: a falling lead never delays the trip");
        /* A slow raw rise just under the threshold plus a strong lead trips:
         * the compensated rise is what the kiln is really doing. */
        int t3 = ticks_to_trip(0.5f, 20.0f, 0.0f, 200.0f, 0.0f, 3.0f, 200, &r1);
        int t4 = ticks_to_trip(0.5f, 20.0f, 0.0f, 200.0f, 0.0f, 0.0f, 200, &r0);
        TEST_CHECK(r1 == THERMAL_GUARD_TRIP_RUNAWAY && t3 < t4, "guard 3: lead alone reveals a hidden rise");
    }
}
