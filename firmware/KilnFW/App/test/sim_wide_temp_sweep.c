// sim_wide_temp_sweep -- ITER_TUNE_REDESIGN_PLAN.md sec 6: closes the four
// sim_kiln gaps (G1 real measured params, G2 real heater_output.c PWM
// window, G3 relay actuation lag, G4 MAX31856 quantisation) and then sweeps
// ambient-to-cone setpoints through the real pid.c + zone_coupling_solve.c
// control stack against the extended sim_kiln model.
//
// This is NOT a TEST_CHECK-style pass/fail suite -- it is a data-generating
// harness invoked once from tools/run_all_checks.ps1-adjacent tooling (or by
// hand) whose stdout is the raw material for
// docs/audits/sim_wide_temperature_2026-09-08.md. Per the plan's own sec
// 6.4, nothing here is evidence about the real kiln above ~62 C: the
// radiative coefficient is not measured on this bench, only its *shape* is
// modelled (sim_plant.c's existing Stefan-Boltzmann term). Every number
// printed above 62 C is labelled EXTRAPOLATION in its own output line.
//
// Linked as-is, unmodified (plan sec 6.3's "link the real production .c
// files" rule): pid.c, heater_output.c, zone_coupling_solve.c, sim_plant.c.
// New code (this file) supplies G1's data mapping, G3's relay lag, G4's
// quantisation, the fake zones_config_get_coupling*() zone_coupling_solve.c
// needs (same pattern as test_zone_coupling_solve.c), and the sweep driver.

#include "../drivers/control/pid.h"
#include "../drivers/control/heater_output.h"
#include "../drivers/control/zone_coupling_solve.h"
#include "sim_plant.h"
#include "../drivers/hw/max31856_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NZ 3

// ---- G1: measured plant data + coupling cross-gain, now a SINGLE shared
// source (sim_measured_zone_constants.h) rather than a hand-copied literal
// per file -- this file, sim_iter_tune.c and sim_credibility_gate.c used to
// each carry their own copy of tuned_baseline_20260831.json's
// model_k_dc/model_tau_s/model_dead_time_s, a PRESET that went stale
// against the live board (docs/audits/cplval75_coupling_verdict_2026-09-10.md
// sec 4/D2: live k_dc is 39.2459/31.9669/31.6810, not the preset's
// 31.9609/23.4805/21.7422). See that header's own comment for full
// provenance and re-sync instructions. ----
#include "sim_measured_zone_constants.h"
static const float g_kp[NZ] = { 0.0318f, 0.0361f, 0.0355f };
static const float g_ki[NZ] = { 0.0002f, 0.0003f, 0.0003f };
static const float g_kd[NZ] = { 0.6526f, 0.6874f, 0.6598f };

// ---- fakes zone_coupling_solve.c needs (same contract test_zone_coupling_solve.c uses) ----
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= NZ) return false;
    for (int j = 0; j < NZ; j++) out_row[j] = g_coupling_coeff[zone_index][j];
    return true;
}
// The COUPLING RUN's OWN diagonal (docs/audits/high_temperature_transfer_
// analysis_2026-09-08.md's full 3x3, and test_zone_coupling_solve.c's
// OWN_DIAG_Z*): the direct gain measured by the SAME coupled excitation runs
// that produced g_coupling_coeff above, as opposed to g_k_dc, which is the
// separate single-zone FOPDT step test's gain. Not a new measurement and not
// invented here -- these are the already-adopted numbers, carried in so this
// harness can run the column-consistent matrix as well as the mixed one.
static const float g_own_diag[NZ] = { 38.13f, 35.90f, 35.32f };

// Whether zones_config_get_coupling_diag_k_dc() below reports the coupling
// run's own diagonal as measured. false models a board that has never had a
// coupling identification persist its diagonal -- on which
// coupling_column_provenance_ok() now refuses the matrix outright rather
// than completing it from g_k_dc (see docs/audits/
// dc_gain_factor_of_ten_2026-09-09.md sec 4). g_diag_scale mirrors the
// power_scale applied to ff_k_dc in run_firing2(): the diagonal is a gain in
// exactly the same units, so it must be scaled by exactly the same factor or
// the matrix is inconsistent for a second, unrelated reason.
static bool  g_diag_measured = true;
static float g_diag_scale = 1.0f;

bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (zone_index >= NZ) return false;
    if (!g_diag_measured) {
        *out_k_dc = 0.0f; // the field's own "never measured" value, getter still reports true
        return true;
    }
    *out_k_dc = g_own_diag[zone_index] * g_diag_scale;
    return true;
}

// Mirrors profile_executor_feedforward.c's s_coupling_use_measured_diag_k_dc.
// Kept as one named constant rather than two inline `false` literals at the
// call sites, so this harness cannot silently drift away from the production
// value the way it had (production's flag has been flipped to true).
// -DKILN_SWEEP_USE_MEASURED_DIAG=0 exists ONLY so the PRE-FIX arm (flag
// false, i.e. the mixed matrix) can still be run for a before/after
// comparison against a checkout of the old zone_coupling_solve.c; nothing in
// the shipped build defines it.
#ifndef KILN_SWEEP_USE_MEASURED_DIAG
#define KILN_SWEEP_USE_MEASURED_DIAG 1
#endif
static const bool SWEEP_USE_MEASURED_DIAG_K_DC = (KILN_SWEEP_USE_MEASURED_DIAG != 0);

// ---- G3: relay actuation lag. Not bench-measured (ITER_TUNE_REDESIGN_PLAN
// sec 6.1's G3 row: "default from the bench-measured lag" -- no such
// measurement exists in this repo as of writing; docs/audits grep turned up
// nothing beyond the qualitative "1Hz switching through a 60s window"
// autotune-defeat finding). 0.5s is an ASSUMED placeholder for an SSR-class
// device, clearly flagged; the sweep below also runs 0s and 2s variants to
// bound sensitivity to this unmeasured number.
//
// 2026-09-09 (opus review, finding C): this file used to carry its OWN
// local copy of the relay-lag ring and the MAX31856 quantiser instead of the
// promoted sim_plant.h helpers (sim_relay_lag_t/sim_relay_lag_step,
// sim_max31856_quantize_tc) that e0d2e006 introduced specifically so no
// harness could accidentally run unquantized/unlagged synthetic data ever
// again. This file's local quantize_tc() rounded at the raw
// MAX31856_TC_TEMP_C_PER_LSB (1/4096 C) instead of the real achievable
// 32*MAX31856_TC_TEMP_C_PER_LSB (0.0078125 C) -- exactly the too-fine
// constant e0d2e006's own commit message named as the bug it was fixing,
// just never removed from this sibling harness. Now uses the shared
// helpers; the local relay_lag_t/relay_lag_step/quantize_tc definitions are
// deleted rather than kept as unused dead code.

// ---- G1 mapping (plan sec 6.2): h=loss_coeff_w_per_c=1.0 free-scale pick ----
//
// IMPORTANT STRUCTURAL FINDING (see audit doc): a linear FOPDT's
// steady-state gain P/h is INVARIANT to the free-scale choice of h --
// scaling h scales P and C together, but K=P/h never moves. Taking
// model_k_dc literally as the duty=1 steady-state gain therefore caps this
// model's ceiling at ambient + model_k_dc (~32-52C depending on zone) no
// matter what h is picked -- there is no free-scale escape from this.
// That ceiling sits near the TOP of the bench's own 0-80C tested range,
// nowhere near cone temperature. power_scale below is a SEPARATE, EXTRA,
// UNMEASURED assumption (bumping heater_power_w past what model_k_dc
// implies, as if the real kiln's installed element capacity is larger than
// the bench rig's) used ONLY for the illustrative cone-temperature runs --
// it is flagged in every such run's label and is not derived from any
// measurement in this repo.
static void build_zone_cfg(sim_kiln_cfg_t *cfg, float ambient_c, float radiative_coeff, float power_scale)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->zone_count = NZ;
    for (int i = 0; i < NZ; i++) {
        sim_plant_cfg_t *p = &cfg->zone[i].plant;
        p->ambient_c = ambient_c;
        p->heater_power_w = g_k_dc[i] * power_scale; // K, optionally scaled -- see doc comment above
        p->thermal_mass_j_per_c = g_tau_s[i];  // tau (loss_coeff = 1.0 -> C/h = tau)
        p->loss_coeff_w_per_c = 1.0f;          // free scale h
        p->sensor_delay_s = g_dead_time_s[i];  // L
        p->sensor_lag_tau_s = 0.0f;            // lumped into tau already -- plan sec 6.2
        cfg->zone[i].radiative_coeff_w_per_k4 = radiative_coeff;
        for (int j = 0; j < NZ; j++) {
            if (i == j) continue;
            // g_ij = h_i * coupling_coeff[i][j] / diag_k_dc[j] -- algebraic first cut
            cfg->coupling_w_per_c[i][j] = 1.0f * g_coupling_coeff[i][j] / g_k_dc[j];
        }
    }
    cfg->sensor_noise_c = 0.05f; // deterministic LCG noise, small vs quantisation LSB (~0.00024C)
}

// One-zone step test inside the simulator, to check the algebraic
// first-cut coupling conductance against the measured cross-gain
// (plan sec 6.2's "measured inside the sim, not derived" instruction --
// this is the verification half; full iterative re-fit is out of scope for
// this pass and is flagged as such in the audit doc).
static void verify_coupling_step_test(void)
{
    sim_kiln_cfg_t cfg;
    build_zone_cfg(&cfg, 20.0f, 0.0f, 1.0f);
    sim_kiln_state_t st;
    sim_kiln_reset(&st, &cfg);
    float duty[NZ] = {0};
    duty[0] = 1.0f; // step zone 0 full on, others off
    float dt = 1.0f;
    for (int t = 0; t < 20000; t++) { // long enough to approach steady state well below 62C cap
        sim_kiln_step(&st, &cfg, duty, dt);
    }
    printf("# coupling step-test (zone0 stepped, 20000s): element_c z0=%.2f z1=%.2f z2=%.2f\n",
           sim_kiln_element_c(&st, 0), sim_kiln_element_c(&st, 1), sim_kiln_element_c(&st, 2));
    float rise1 = sim_kiln_element_c(&st, 1) - 20.0f;
    float rise2 = sim_kiln_element_c(&st, 2) - 20.0f;
    // measured cross-gain g_10=14.30, g_20=8.33 in the bench's own fitted units;
    // the sim's rise is in degC at duty=1 (not directly comparable in units --
    // logged for residual-direction info only, per plan's "record the residual").
    printf("# rise z1=%.3f rise z2=%.3f (bench measured coupling_coeff z1<-z0=%.2f z2<-z0=%.2f, unit mismatch expected, direction/ratio is the check)\n",
           rise1, rise2, g_coupling_coeff[1][0], g_coupling_coeff[2][0]);
}

typedef struct {
    pid_state_t pid[NZ];
    heater_output_state_t heater[NZ];
    sim_relay_lag_t lag[NZ];
    zone_coupling_hold_cache_t hold_cache[NZ];
    zone_coupling_climb_cache_t climb_cache[NZ];
    uint16_t hold_sig[NZ];
    uint16_t climb_sig[NZ];
} controller_t;

static void controller_reset(controller_t *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < NZ; i++) {
        pid_reset(&c->pid[i]);
        heater_output_reset(&c->heater[i]);
    }
}

// Runs one zone-mask=all-3 firing from ambient to target_c at ramp_c_per_hr,
// holds a dwell, and reports: max tracking error during ramp, dwell steady
// offset, whether ff_hold ever reported infeasible (clamped), the tick
// (temperature) infeasibility first appears at, and the coupling conductance
// term's magnitude vs the radiative loss term's magnitude at dwell end (the
// "does radiative dominate conduction yet" question).
// Owner follow-up (2026-09-08): "does a single gain set work across a full
// firing", "on what should gains be scheduled", "does ff_hold go infeasible
// at every operating point or was 62C an artifact of a low-T matrix", "do
// guards nuisance-trip". GAIN_MODE below drives the first two; power_scale
// (see build_zone_cfg's doc comment) is required to let the model reach
// cone temperature at all, given the structural ceiling finding above.
typedef enum {
    GAIN_BENCH = 0,       // fixed bench (low-T) gains everywhere -- the original run_firing behaviour
    GAIN_HIGH_FIXED,      // fixed gains derived for one high-T operating point, applied everywhere
    GAIN_SCHED_SETPOINT,  // gains rescaled every tick from setpoint_c
    GAIN_SCHED_MEASURED,  // gains rescaled every tick from the measured (quantised) reading
    GAIN_SCHED_ESTIMATED, // gains rescaled every tick from an online loss-coefficient estimator
} gain_mode_t;

// scale(T) = instantaneous effective loss coefficient / ambient loss
// coefficient (h=1 by construction), i.e. how much the plant's steady-state
// gain has fallen at T relative to ambient. Derivation: dT/dt = (P*u -
// h*(T-Ta) - rad*(T^4-Ta^4))/C; linearising the radiative term around T
// gives an incremental loss slope h + 4*rad*T_abs^3, so
// K_eff(T) = P/(h+4*rad*T_abs^3) and K_low = P/h (radiative negligible at
// ambient) -- scale(T) = K_low/K_eff(T) = h+4*rad*T_abs^3 = 1+4*rad*T_abs^3.
// A classic gain-scheduling counter-scale (Kp,Ki,Kd all multiplied by
// scale(T)) holds the LOOP gain (Kp*K_eff) constant as the plant's own gain
// falls with temperature.
static float loss_scale(float t_c, float rad_coeff)
{
    double t_abs = (double)t_c + 273.15;
    return (float)(1.0 + 4.0 * (double)rad_coeff * t_abs * t_abs * t_abs);
}

static void run_firing2(float target_c, float ramp_c_per_hr, float dwell_s, float radiative_coeff,
                         float relay_lag_s, float power_scale, gain_mode_t gain_mode, float high_t_ref_c,
                         const char *label)
{
    float ambient_c = 20.0f;
    sim_kiln_cfg_t cfg;
    build_zone_cfg(&cfg, ambient_c, radiative_coeff, power_scale);

    sim_kiln_state_t sim;
    sim_kiln_reset(&sim, &cfg);

    controller_t ctl;
    controller_reset(&ctl);

    // capacity_correction: classic PID gain-vs-plant-gain scaling (kp*K
    // held constant). The bench gains were tuned against K=model_k_dc; if
    // power_scale assumes the real plant's K is power_scale times larger,
    // applying the UNCORRECTED bench gains against that stronger plant
    // multiplies the loop gain by power_scale and was, in an earlier pass
    // of this harness, indistinguishable from a genuine runaway (rates of
    // >1000 C/min) -- an artifact of the harness's own inconsistent
    // scaling, not a controller finding. This correction and the
    // ff_k_dc-scaling above must both be applied together or neither.
    float capacity_correction = 1.0f / power_scale;
    heater_output_cfg_t hcfg = { .window_ms = 60000, .min_on_ms = 10000, .min_off_ms = 2000 };
    pid_cfg_t pcfg[NZ];
    for (int i = 0; i < NZ; i++) {
        pcfg[i] = (pid_cfg_t){ .kp = g_kp[i] * capacity_correction, .ki = g_ki[i] * capacity_correction,
                                .kd = g_kd[i] * capacity_correction,
                                .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f };
    }
    float high_fixed_scale[NZ];
    for (int i = 0; i < NZ; i++) high_fixed_scale[i] = loss_scale(high_t_ref_c, radiative_coeff);

    float setpoint_c[NZ] = { ambient_c, ambient_c, ambient_c };
    float rate_c_per_s = ramp_c_per_hr / 3600.0f;
    float dt_s = 1.0f;
    uint32_t dt_ms = 1000;

    float ramp_time_s = (target_c - ambient_c) / rate_c_per_s;
    long total_ticks = (long)(ramp_time_s + dwell_s);

    float max_err[NZ] = {0};
    float max_overshoot_low_t[NZ] = {0}; // overshoot (reading-setpoint) while setpoint < 150C
    float max_rate_c_per_min = 0.0f;
    float max_scale_step = 0.0f; // largest tick-to-tick jump in the schedule scale -- "smoothness of handover"
    bool any_infeasible = false;
    long first_infeasible_tick = -1;
    long infeasible_ticks = 0;   // ticks on which ANY zone's ff_hold solve clamped a duty down to 1.0
    long total_ticks_run = 0;
    float last_hold[NZ] = {0}, last_climb[NZ] = {0};
    float last_reading[NZ] = { ambient_c, ambient_c, ambient_c };
    float prev_reading[NZ] = { ambient_c, ambient_c, ambient_c };
    float est_denom[NZ] = { 1.0f, 1.0f, 1.0f }; // online loss-scale estimate, EWMA
    float prev_scale[NZ] = { 1.0f, 1.0f, 1.0f };
    float prev_sim_duty[NZ] = {0};

    zone_coupling_neighbor_t nb[NZ];

    g_diag_scale = power_scale; // see g_diag_scale's own comment: same units as ff_k_dc

    for (long t = 0; t < total_ticks; t++) {
        total_ticks_run++;
        bool in_ramp = ((float)t < ramp_time_s);
        for (int i = 0; i < NZ; i++) {
            if (in_ramp) {
                setpoint_c[i] = ambient_c + rate_c_per_s * (float)t;
            } else {
                setpoint_c[i] = target_c;
            }
        }
        float rate_now = in_ramp ? rate_c_per_s : 0.0f;

        // ff_k_dc must be scaled the SAME way heater_power_w was (plan
        // sec 6.2: K=P/h, h fixed at 1 -- if the plant's real power is
        // assumed power_scale times the bench-identified P, its steady-
        // state gain K scales by exactly the same factor). Feeding the
        // feedforward solve the UNSCALED bench k_dc while the plant is
        // power_scale times stronger was tried first and produced a
        // self-inflicted runaway (ff_hold demanding duty~40+ against a
        // plant that only needed ~0.7): the hold term is not a free
        // parameter independent of power_scale, it is fixed by the same
        // assumption that motivated power_scale in the first place.
        float k_dc_eff[NZ];
        for (int i = 0; i < NZ; i++) k_dc_eff[i] = g_k_dc[i] * power_scale;
        for (int i = 0; i < NZ; i++) {
            nb[i].qualifies = zone_coupling_qualifies_as_neighbor(
                true, true, last_reading[i], true, k_dc_eff[i], ZONE_CONTROL_MODE_PID, false, false);
            nb[i].ff_k_dc = k_dc_eff[i];
            nb[i].ff_tau_s = g_tau_s[i];
        }

        // Online estimated-loss update, using LAST tick's observed duty and
        // dT/dt (this tick's control decision cannot use data from the
        // future) -- a simple EWMA-smoothed instantaneous-balance estimate,
        // skipped near ambient where (T-Ta) is too small to divide by
        // usefully.
        for (int i = 0; i < NZ; i++) {
            float dT = last_reading[i] - prev_reading[i];
            float dTdt = dT / dt_s;
            float delta_t = last_reading[i] - ambient_c;
            if (fabsf(delta_t) > 5.0f) {
                float P = g_k_dc[i] * power_scale;
                float C = g_tau_s[i];
                float raw_denom = (P * prev_sim_duty[i] - C * dTdt) / delta_t;
                if (isfinite(raw_denom) && raw_denom > 0.1f) {
                    float alpha = dt_s / (60.0f + dt_s); // ~60s EWMA, same order as tau
                    est_denom[i] += alpha * (raw_denom - est_denom[i]);
                }
            }
        }

        float duty_cmd[NZ];
        bool infeasible_this_tick = false;
        for (int i = 0; i < NZ; i++) {
            // BUG FOUND DURING THIS PASS: an earlier version of this loop
            // read one shared `infeasible` local for both calls below, so
            // the hold term's flag was silently overwritten by the climb
            // term's before ever being checked -- "ff_hold never reported
            // infeasible" was being printed from the CLIMB term's flag the
            // entire time. climb rarely saturates (it is a small correction
            // on top of hold, not the dominant duty component), so that bug
            // made every run in this file falsely report zero infeasibility.
            // Fixed by giving each call its own flag. See the audit doc for
            // what changes once this is corrected.
            bool used_matrix, hold_infeasible, climb_infeasible, membership_changed;
            coupling_solve_reason_t reason;
            float hold = zone_coupling_solve_hold(nb[i].qualifies, k_dc_eff[i], (uint8_t)i,
                                                   SWEEP_USE_MEASURED_DIAG_K_DC,
                                                   nb, NZ, setpoint_c[i], ambient_c,
                                                   &used_matrix, &hold_infeasible, &reason, &membership_changed,
                                                   &ctl.hold_cache[i], &ctl.hold_sig[i]);
            float climb = zone_coupling_solve_climb(nb[i].qualifies, k_dc_eff[i], g_tau_s[i], (uint8_t)i,
                                                     SWEEP_USE_MEASURED_DIAG_K_DC,
                                                     nb, NZ, rate_now,
                                                     &used_matrix, &climb_infeasible, &reason, &membership_changed,
                                                     &ctl.climb_cache[i], &ctl.climb_sig[i]);
            last_hold[i] = hold;
            last_climb[i] = climb;
            if (hold_infeasible || climb_infeasible) {
                any_infeasible = true;
                infeasible_this_tick = true;
                if (first_infeasible_tick < 0) first_infeasible_tick = t;
            }
            float ff_u = hold + climb;

            float scale = 1.0f;
            switch (gain_mode) {
                case GAIN_BENCH: scale = 1.0f; break;
                case GAIN_HIGH_FIXED: scale = high_fixed_scale[i]; break;
                case GAIN_SCHED_SETPOINT: scale = loss_scale(setpoint_c[i], radiative_coeff); break;
                case GAIN_SCHED_MEASURED: scale = loss_scale(last_reading[i], radiative_coeff); break;
                case GAIN_SCHED_ESTIMATED: scale = est_denom[i]; break;
            }
            if (scale < 1.0f) scale = 1.0f;
            float step = fabsf(scale - prev_scale[i]);
            if (step > max_scale_step) max_scale_step = step;
            prev_scale[i] = scale;

            pid_cfg_t pcfg_i = pcfg[i];
            pcfg_i.kp *= scale;
            pcfg_i.ki *= scale;
            pcfg_i.kd *= scale;
            float duty = pid_update(&ctl.pid[i], &pcfg_i, setpoint_c[i], last_reading[i], dt_s, ff_u, hold);
            duty_cmd[i] = duty;
        }

        if (infeasible_this_tick) infeasible_ticks++;

        bool relay_cmd[NZ], relay_actual[NZ];
        for (int i = 0; i < NZ; i++) {
            relay_cmd[i] = heater_output_duty(&ctl.heater[i], &hcfg, duty_cmd[i], dt_ms);
            relay_actual[i] = sim_relay_lag_step(&ctl.lag[i], relay_cmd[i], relay_lag_s, dt_s);
        }

        float sim_duty[NZ];
        for (int i = 0; i < NZ; i++) sim_duty[i] = relay_actual[i] ? 1.0f : 0.0f;
        sim_kiln_step(&sim, &cfg, sim_duty, dt_s);

        for (int i = 0; i < NZ; i++) {
            prev_reading[i] = last_reading[i];
            prev_sim_duty[i] = sim_duty[i];
            float raw = sim_kiln_reading_c(&sim, &cfg, i);
            float q = sim_max31856_quantize_tc(raw); // G4
            float rate_c_per_min = fabsf(q - last_reading[i]) / dt_s * 60.0f;
            if (rate_c_per_min > max_rate_c_per_min) max_rate_c_per_min = rate_c_per_min;
            last_reading[i] = q;
            float err = fabsf(q - setpoint_c[i]);
            if (in_ramp && err > max_err[i]) max_err[i] = err;
            if (setpoint_c[i] < 150.0f) {
                float overshoot = q - setpoint_c[i];
                if (overshoot > max_overshoot_low_t[i]) max_overshoot_low_t[i] = overshoot;
            }
        }
    }

    printf("\n== %s: target=%.0fC ramp=%.0fC/hr radiative=%.3e relay_lag=%.1fs power_scale=%.0fx gain_mode=%d ==\n",
           label, target_c, ramp_c_per_hr, (double)radiative_coeff, (double)relay_lag_s, (double)power_scale, (int)gain_mode);
    for (int i = 0; i < NZ; i++) {
        float dwell_offset = setpoint_c[i] - last_reading[i];
        double t_k = (double)last_reading[i] + 273.15;
        double amb_k = (double)ambient_c + 273.15;
        double rad_w = (double)cfg.zone[i].radiative_coeff_w_per_k4 * (t_k*t_k*t_k*t_k - amb_k*amb_k*amb_k*amb_k);
        double coupling_w = 0;
        for (int j = 0; j < NZ; j++) if (j != i) coupling_w += (double)cfg.coupling_w_per_c[i][j] *
                                                                (sim_kiln_element_c(&sim,j) - sim_kiln_element_c(&sim,i));
        printf("  z%d: ramp_max_err=%.2fC dwell_offset=%.2fC low_T_overshoot=%.2fC ff_hold=%.4f ff_climb=%.4f rad_loss_w=%.3f coupling_w=%.3f\n",
               i, max_err[i], dwell_offset, max_overshoot_low_t[i], last_hold[i], last_climb[i], rad_w, coupling_w);
    }
    printf("  max_rate=%.2fC/min (S8 guard default 33.3C/min: %s) max_schedule_step=%.3f\n",
           max_rate_c_per_min, (max_rate_c_per_min > 33.3f) ? "WOULD TRIP" : "clear", max_scale_step);
    if (any_infeasible) {
        printf("  ff_hold INFEASIBLE first at tick %ld (elapsed %.1fs, approx temp %.1fC); "
               "infeasible on %ld of %ld ticks (%.1f%%)\n",
               first_infeasible_tick, (double)first_infeasible_tick,
               ambient_c + rate_c_per_s * (float)first_infeasible_tick,
               infeasible_ticks, total_ticks_run,
               100.0 * (double)infeasible_ticks / (double)(total_ticks_run ? total_ticks_run : 1));
    } else {
        printf("  ff_hold never reported infeasible over this firing (0 of %ld ticks)\n", total_ticks_run);
    }
}

int main(void)
{
    printf("# sim_wide_temp_sweep -- ITER_TUNE_REDESIGN_PLAN sec 6 gap closure + wide sweep\n");
    printf("# Results above ~62C are EXTRAPOLATION: radiative_coeff_w_per_k4 is NOT measured on this kiln.\n\n");

    verify_coupling_step_test();

    // Provenance arm (docs/audits/dc_gain_factor_of_ten_2026-09-09.md sec 4).
    // KILN_SWEEP_DIAG_MEASURED=0 in the environment models a board whose
    // coupling identification has NOT persisted its own diagonal: the matrix
    // is half-populated, coupling_column_provenance_ok() refuses it, and
    // every zone falls back to the uncoupled diagonal feedforward. Default
    // (unset, or any other value) models a board that HAS one, i.e. the
    // column-consistent matrix.
    {
        const char *env = getenv("KILN_SWEEP_DIAG_MEASURED");
        if (env != NULL && env[0] == '0') g_diag_measured = false;
        printf("# coupling diagonal measured (column-consistent matrix): %s\n",
               g_diag_measured ? "yes" : "no -- provenance guard refuses the matrix");
    }

    // Nominal radiative coefficient picked ONLY to visibly bend gain over
    // the sweep -- not a measurement (units are the G1 mapping's "nominal"
    // units per sim_plant.c's doc comment, not physical W/m^2K^4, so a real
    // Stefan-Boltzmann constant would not even be dimensionally meaningful
    // here). Sensitivity variants (0.5x, 2x) are run throughout to bound how
    // much every conclusion below actually depends on this unmeasured
    // number.
    float radiative_nominal = 2.5e-10f;

    // power_scale=100 is a SEPARATE, EXTRA, UNMEASURED assumption on top of
    // the radiative sensitivity -- see build_zone_cfg()'s doc comment: the
    // literal G1 mapping caps this model at ambient+model_k_dc (~32-52C)
    // regardless of h, so reaching cone temperature at all requires assuming
    // the real kiln's installed element capacity is larger than what the
    // bench identification's small-signal gain implies. Every run using it
    // is labelled ILLUSTRATIVE and its own gain gets no special treatment
    // for this scaling (kp/ki/kd are the bench-fitted values, or a schedule
    // multiplier on them -- power_scale does not change how gains are
    // computed, only how much heat duty=1 delivers).
    // 100x was tried first and rejected: bumping heater_power_w alone while
    // leaving thermal_mass_j_per_c (tau) unchanged makes the plant's
    // near-ambient heating rate scale with power_scale too (dT/dt ~
    // P/C when loss is small), producing >1000C/min instantaneous rates --
    // a second, independent modelling artifact on top of the ceiling issue,
    // not a controller finding. There is no bench data connecting installed
    // power to thermal mass, so no power_scale is "right"; 5x is chosen as
    // the LARGEST value that keeps near-ambient heating rates within a
    // plausible small multiple of the S8 guard threshold (see the printed
    // max_rate lines) rather than three orders of magnitude past it, so the
    // asymmetry/scheduling/infeasibility questions below are not entirely
    // swamped by an unrelated artifact. Even so, target is capped at 250C,
    // not 1200C -- see the audit doc for why cone temperature is out of
    // reach of this model without a physically-motivated way to extend
    // BOTH gain and time constant together, which no bench data provides.
    float power_scale_cone = 5.0f;

    // 1. Low-temperature control run, inside the region the model was
    //    actually fitted from (bench data <= ~62C) -- a sanity baseline,
    //    NOT extrapolation, power_scale=1.
    run_firing2(60.0f, 100.0f, 1800.0f, 0.0f, 0.5f, 1.0f, GAIN_BENCH, 0.0f,
                "BASELINE (measured region, no radiative term, bench gains)");

    // 2. Cross the ~62C ff_hold-infeasible point the bench data predicted,
    //    still power_scale=1 (structural ceiling for z0 is ~52C, so this
    //    run is already past the model's own literal ceiling -- see printed
    //    dwell_offset; kept for continuity with the original brief).
    run_firing2(90.0f, 150.0f, 1800.0f, 0.0f, 0.5f, 1.0f, GAIN_BENCH, 0.0f,
                "EXTRAPOLATION just above 62C boundary (no radiative, bench gains)");

    printf("\n# ---- ILLUSTRATIVE cone-temperature runs below assume power_scale=%.0fx"
           " (unmeasured, see build_zone_cfg doc comment) ON TOP of the radiative"
           " sensitivity. Absolute numbers are not evidence about the real kiln.\n",
           (double)power_scale_cone);

    // 3. Single gain set across the WHOLE climb, ambient->cone: bench (low-T)
    //    gains vs. gains derived for a high-T operating point (1000C), same
    //    fixed set the entire firing -- the owner's asymmetry question.
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 0.5f, power_scale_cone,
                GAIN_BENCH, 0.0f, "ILLUSTRATIVE: bench(low-T) gains fixed across full climb to 110C");
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 0.5f, power_scale_cone,
                GAIN_HIGH_FIXED, 200.0f, "ILLUSTRATIVE: high-T(200C)-tuned gains fixed across full climb");

    // 4. Scheduling variable comparison -- setpoint vs measured vs an online
    //    estimated-loss coefficient -- same climb, same nominal radiative.
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 0.5f, power_scale_cone,
                GAIN_SCHED_SETPOINT, 0.0f, "ILLUSTRATIVE: gain scheduled on setpoint_c");
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 0.5f, power_scale_cone,
                GAIN_SCHED_MEASURED, 0.0f, "ILLUSTRATIVE: gain scheduled on measured temperature");
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 0.5f, power_scale_cone,
                GAIN_SCHED_ESTIMATED, 0.0f, "ILLUSTRATIVE: gain scheduled on online estimated loss coefficient");

    // 5. Radiative-coefficient sensitivity on the best-performing schedule
    //    (measured-temperature scheduling, per #4's numbers in the audit doc).
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal * 0.5f, 0.5f, power_scale_cone,
                GAIN_SCHED_MEASURED, 0.0f, "ILLUSTRATIVE: scheduled-on-measured, radiative x0.5");
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal * 2.0f, 0.5f, power_scale_cone,
                GAIN_SCHED_MEASURED, 0.0f, "ILLUSTRATIVE: scheduled-on-measured, radiative x2");

    // 6. Relay-lag sensitivity at cone temp (unmeasured G3 parameter).
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 0.0f, power_scale_cone,
                GAIN_SCHED_MEASURED, 0.0f, "ILLUSTRATIVE: scheduled-on-measured, relay_lag=0s");
    run_firing2(110.0f, 150.0f, 3600.0f, radiative_nominal, 2.0f, power_scale_cone,
                GAIN_SCHED_MEASURED, 0.0f, "ILLUSTRATIVE: scheduled-on-measured, relay_lag=2s");

    return 0;
}
