// Closed-loop simulation of thermocouple sheath (protection tube) lag and
// its compensation (tc_lag_comp.c), driving the real pid.c and the real
// thermal_guard.c. Results are printed as a table and every row is asserted,
// so a change that makes compensation worse fails the host tests. The table
// and the model are written up in docs/TC_SHEATH_LAG_SIM.md.
//
// Plant (deliberately simple, first order, duty applied continuously):
//   kiln air:  dT/dt  = G * u - (T - 20) / TAU_KILN
//   junction:  dTj/dt = (T - Tj) / tau_true          (tau_true 0: Tj = T)
//   reading:   Tj + noise (LCG, +/-0.15 C), optional spikes and a dropout.
// Controller: tc_lag_comp_step(tau_set) feeds pid_update() exactly as
// profile_executor.c does (offset 0, so actual_cal_c == reading). Metrics
// are taken on the TRUE kiln temperature, which is what the ware sees.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/control/pid.h"
#include "../drivers/control/tc_lag_comp.h"
#include "../drivers/control/thermal_guard.h"

#define SIM_G        0.5f    /* C/s at full duty */
#define SIM_TAU_KILN 3000.0f /* s, loss time constant: full-power ceiling ~1520 C */
#define SIM_DT       1.0f
#define SIM_PEAK     1000.0f
#define SIM_HOLD_S   1800
#define SIM_COOL_TO  800.0f
#define SIM_COOL_CPH 150.0f

typedef struct {
    float ramp_err; /* max |T_true - sp| on the heating ramp, after the first 30 min */
    float overshoot; /* max T_true - SIM_PEAK during the hold */
    float cool_err; /* max |T_true - sp| on the cooling ramp, after its first 10 min */
    uint32_t resets; /* filter resets (spikes/dropout) */
    bool pid_finite; /* duty stayed finite throughout */
} sim_result_t;

static uint32_t s_lcg;
static float noise(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return ((float)(s_lcg >> 8) / 16777216.0f - 0.5f) * 0.3f;
}

static sim_result_t run(float tau_true, float tau_set, float ramp_cph, bool spikes, bool dropout)
{
    sim_result_t r = {0};
    r.pid_finite = true;
    s_lcg = 12345u;
    pid_cfg_t cfg = {.kp = 0.04f, .ki = 0.0001f, .kd = 0.0f, .d_filter_tau_s = 30.0f, .b = 1.0f,
                     .pid_range_c = 1000.0f};
    pid_state_t ps;
    pid_reset(&ps);
    tc_lag_comp_state_t lag;
    memset(&lag, 0, sizeof(lag));

    float T = 20.0f, Tj = 20.0f, sp = 20.0f, u = 0.0f;
    const float rate = ramp_cph / 3600.0f;
    int phase = 0; /* 0 ramp, 1 hold, 2 cool, 3 done */
    int phase_t = 0;
    for (int t = 0; phase < 3 && t < 200000; t++) {
        /* plant */
        T += (SIM_G * u - (T - 20.0f) / SIM_TAU_KILN) * SIM_DT;
        if (tau_true > 0.0f) {
            Tj += (T - Tj) * (SIM_DT / tau_true);
        } else {
            Tj = T;
        }
        /* setpoint schedule */
        if (phase == 0) {
            sp += rate * SIM_DT;
            if (sp >= SIM_PEAK) { sp = SIM_PEAK; phase = 1; phase_t = 0; }
        } else if (phase == 1) {
            if (++phase_t >= SIM_HOLD_S) { phase = 2; phase_t = 0; }
        } else {
            sp -= (SIM_COOL_CPH / 3600.0f) * SIM_DT;
            phase_t++;
            if (sp <= SIM_COOL_TO) { phase = 3; }
        }
        /* sensor */
        float meas = Tj + noise();
        bool valid = true;
        if (spikes && t > 0 && t % 997 == 0) meas += 15.0f;
        if (dropout && t >= 2000 && t < 2030) valid = false;

        bool reset = false;
        float est = tc_lag_comp_step(&lag, tau_set, meas, valid, SIM_DT, &reset);
        if (reset) {
            pid_reseed_derivative(&ps, est);
        }
        if (valid) {
            u = pid_update(&ps, &cfg, sp, est, SIM_DT, 0.0f, 0.0f);
        } else {
            u = 0.0f; /* executor: invalid sensor commands no heat */
        }
        if (!isfinite(u)) r.pid_finite = false;

        float e = fabsf(T - sp);
        if (phase == 0 && t > 1800 && e > r.ramp_err) r.ramp_err = e;
        if (phase == 1 && T - SIM_PEAK > r.overshoot) r.overshoot = T - SIM_PEAK;
        if (phase == 2 && phase_t > 600 && e > r.cool_err) r.cool_err = e;
    }
    r.resets = lag.resets;
    return r;
}

/* Heater stuck full on from a 1200 C hold: time (s after the stick) at which
 * the true kiln temperature crosses 1250 C, and at which thermal_guard
 * guard 5 (max_temp 1250) trips when fed raw only (lead 0) and with the
 * compensation lead from tau_set. */
static void stuck_on(float tau_true, float tau_set, int *t_true, int *t_raw, int *t_comp)
{
    thermal_guard_cfg_t cfg = {.max_temp_c = 1250.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    thermal_guard_state_t g_raw, g_comp;
    thermal_guard_reset(&g_raw);
    thermal_guard_reset(&g_comp);
    tc_lag_comp_state_t lag;
    memset(&lag, 0, sizeof(lag));
    /* steady at 1200: the duty that holds it */
    float T = 1200.0f, Tj = 1200.0f;
    *t_true = *t_raw = *t_comp = -1;
    for (int t = 0; t < 3000; t++) {
        T += (SIM_G * 1.0f - (T - 20.0f) / SIM_TAU_KILN) * SIM_DT;
        Tj = tau_true > 0.0f ? Tj + (T - Tj) * (SIM_DT / tau_true) : T;
        float est = tc_lag_comp_step(&lag, tau_set, Tj, true, SIM_DT, NULL);
        (void)est;
        thermal_guard_input_t in = {0};
        in.sensor_ok = true;
        in.measurement_c = Tj;
        in.setpoint_c = 1200.0f;
        in.commanded_duty = 1.0f;
        in.dt_s = SIM_DT;
        if (*t_true < 0 && T >= 1250.0f) *t_true = t;
        if (*t_raw < 0 && thermal_guard_tick(&g_raw, &cfg, &in) && g_raw.reason == THERMAL_GUARD_TRIP_MAX_TEMP)
            *t_raw = t;
        in.lead_c = lag.corr_c;
        if (*t_comp < 0 && thermal_guard_tick(&g_comp, &cfg, &in) && g_comp.reason == THERMAL_GUARD_TRIP_MAX_TEMP)
            *t_comp = t;
        if (*t_raw >= 0 && *t_comp >= 0 && *t_true >= 0) break;
    }
}

void run_test_tc_lag_sim(void)
{
    TEST_SECTION("tc_lag_sim (sheath lag closed loop)");
    const float taus[] = {0.0f, 60.0f, 180.0f, 300.0f};
    const float ramps[] = {300.0f, 60.0f};
    printf("  ramp  tau_true tau_set | ramp_err overshoot cool_err  (C, true kiln temp)\n");
    for (unsigned ri = 0; ri < 2; ri++) {
        for (unsigned ti = 0; ti < 4; ti++) {
            float tt = taus[ti];
            sim_result_t off = run(tt, 0.0f, ramps[ri], false, false);
            sim_result_t on = run(tt, tt, ramps[ri], false, false);
            sim_result_t lo = run(tt, 0.5f * tt, ramps[ri], false, false);
            sim_result_t hi = run(tt, 1.5f * tt, ramps[ri], false, false);
            const sim_result_t *rows[] = {&off, &on, &lo, &hi};
            const float sets[] = {0.0f, tt, 0.5f * tt, 1.5f * tt};
            for (int k = 0; k < (tt > 0.0f ? 4 : 1); k++) {
                printf("  %4.0f  %7.0f  %6.0f | %8.2f %9.2f %8.2f\n", (double)ramps[ri], (double)tt,
                       (double)sets[k], (double)rows[k]->ramp_err, (double)rows[k]->overshoot,
                       (double)rows[k]->cool_err);
                TEST_CHECK(rows[k]->pid_finite, "sim: duty stays finite");
            }
            if (tt == 0.0f) {
                TEST_CHECK(off.ramp_err < 3.0f && off.overshoot < 3.0f, "sim: no sheath, plain PID tracks");
                continue;
            }
            /* Uncompensated, the true kiln runs ahead of the lagging reading
             * by about tau * rate: that is the problem being fixed. */
            float lag_c = tt * ramps[ri] / 3600.0f;
            TEST_CHECK(off.ramp_err > 0.7f * lag_c, "sim: uncompensated ramp error shows the sheath lag");
            /* Correct tau removes most of it; a 50% mis-set still helps. */
            TEST_CHECK(on.ramp_err < 0.3f * off.ramp_err + 1.0f, "sim: correct tau cuts ramp error by 70%+");
            TEST_CHECK(lo.ramp_err < off.ramp_err, "sim: tau 50% low still better than uncompensated");
            TEST_CHECK(hi.ramp_err < off.ramp_err, "sim: tau 50% high still better than uncompensated");
            TEST_CHECK(on.overshoot <= off.overshoot + 0.5f, "sim: correct tau does not add hold overshoot");
            TEST_CHECK(on.cool_err < off.cool_err + 1.0f, "sim: cooling segment no worse with correct tau");
        }
    }

    /* Noise spikes (+15 C for one tick, every 997 s) and a 30 s sensor
     * dropout (no heat while invalid, as the executor does). A lone spike is
     * held, not a reset, so it costs almost nothing. The dropout resets the
     * filter; while it refills (about 2 * tf) the loop runs on the lagging
     * raw reading again, so the transient is bounded by -- never worse than
     * -- the same dropout with no compensation at all. */
    {
        sim_result_t clean = run(180.0f, 180.0f, 300.0f, false, false);
        sim_result_t spk = run(180.0f, 180.0f, 300.0f, true, false);
        sim_result_t drop = run(180.0f, 180.0f, 300.0f, false, true);
        sim_result_t drop_off = run(180.0f, 0.0f, 300.0f, false, true);
        printf("  tau 180 @300 C/h: clean %.2f | spikes %.2f (resets %u) | dropout %.2f (resets %u), "
               "uncompensated dropout %.2f\n",
               (double)clean.ramp_err, (double)spk.ramp_err, (unsigned)spk.resets, (double)drop.ramp_err,
               (unsigned)drop.resets, (double)drop_off.ramp_err);
        TEST_CHECK(spk.pid_finite && drop.pid_finite, "sim: spikes/dropout keep the duty finite");
        TEST_CHECK(spk.resets == 0u, "sim: lone spikes are held, never a filter reset");
        TEST_CHECK(spk.ramp_err < clean.ramp_err + 2.0f, "sim: spikes cost under 2 C of tracking");
        TEST_CHECK(drop.resets == 1u, "sim: the dropout resets the filter once");
        TEST_CHECK(drop.ramp_err <= drop_off.ramp_err + 1.0f,
                   "sim: dropout transient no worse than the uncompensated loop");
    }

    /* Stuck heater: guard 5 trip time relative to the true crossing. */
    printf("  stuck-on tau_true tau_set | true_cross raw_trip comp_trip (s)\n");
    for (unsigned ti = 1; ti < 4; ti++) {
        float tt = taus[ti];
        const float sets[] = {tt, 0.5f * tt, 1.5f * tt};
        for (int k = 0; k < 3; k++) {
            int tx, tr, tc;
            stuck_on(tt, sets[k], &tx, &tr, &tc);
            printf("           %7.0f  %6.0f | %10d %8d %9d\n", (double)tt, (double)sets[k], tx, tr, tc);
            TEST_CHECK(tx >= 0 && tr >= 0 && tc >= 0, "stuck-on: everything crosses");
            TEST_CHECK(tc <= tr, "stuck-on: compensated guard 5 never trips later than raw");
            TEST_CHECK(tr > tx, "stuck-on: raw guard 5 trips after the true crossing (the sheath lag)");
            if (k == 0) {
                TEST_CHECK(tc - tx < (tr - tx) / 3 + 5, "stuck-on: correct tau removes most of the trip delay");
            }
        }
    }
}
