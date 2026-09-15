// sim_factorial_driver -- CELL DRIVER ONLY (docs/audits/scenario_factorial_driver_2026-09-14.md).
//
// SCOPE: drives the 263 cells sim_factorial_design.c generates through three
// arms (A_PID, A_FUZZY50, A_STATIC_MATCHED) and emits one TSV row per
// (cell, arm), plus a per-cell/arm fuzzy rule-cell occupancy histogram. This
// is glue over three already-validated pieces (the design generator, the
// three-node decomposition helper, and the real production control code) --
// it builds NO new control logic and computes NO effects analysis (no P8/P11
// materiality or interaction inference). That is a separate, later dispatch;
// this file reports numbers only, never verdicts.
//
// WHAT THIS LINKS, REAL PRODUCTION CODE, NO MIRROR: pid.c, pid_fuzzy.c,
// pid_autotune.c, firing_score.c, sim_plant.c (real
// sim_plant_decompose_three_node()). The per-tick wiring below is a
// deliberate near-duplicate of sim_scenarios.c's run_firing() -- sim_scenarios.c
// is WI-4/5/6/7's own file (not owned by this dispatch, and it has its own
// main()), so this driver keeps its own copy of the tick loop rather than
// reaching into that file. Any wiring bug fixed in one must be checked
// against the other; see the audit doc for the exact ways they were kept in
// sync at the time this file was written.
//
// EVERY CONSTANT BELOW ("bench base fit", sensor-node fixture values, A2's
// headroom multiplier, A4's bench/kiln absolute temperature spans) IS A TEST
// FIXTURE. Never shipped, never written into zones_config, a preset, or a
// firmware default.
//
// Usage: sim_factorial_driver.exe [--shard I --of M]
// Sharding is cell-index modulo, same style as sim_scenarios.c's WI-7: each
// shard prints only its own rows, in ascending cell order, no reordering, no
// RNG, no shared state across cells -- run_sim_factorial.ps1 proves --of 1
// and --of 4 produce byte-identical data rows.
#include "../drivers/control/pid.h"
#include "../drivers/control/pid_autotune.h"
#include "../drivers/control/pid_fuzzy.h"
#include "../drivers/control/firing_score.h"
#include "sim_factorial_design.h"
#include "sim_high_temp.h"
#include "sim_plant.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_S 1.0f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f

// ---- Bench base fit (design doc sec 3): measured z0 FOPDT, the anchor
// every bench cell's three-node decomposition is built to reproduce in
// aggregate (K, tau). ----
#define BENCH_K0_C_PER_DUTY 42.731f
#define BENCH_TAU0_S 255.6f
#define BENCH_L0_S 40.3f

// ---- Kiln-scale base "fit" for A4=KILN_SPAN cells (200->1250C). The bench
// fit above cannot reach kiln temperatures at any headroom multiplier (its
// full-duty asymptote is ambient+K ~= 67C) -- sim_high_temp_kiln_scale_cfg()
// (sim_high_temp.c) already anchors a physically-sized kiln plant
// (heater_power_w=2500W, c_l tuned for a ~488s tau at the 200C tune point,
// full-duty asymptote ~1288C) for exactly this reason; this driver reuses
// its two headline numbers (K, tau) as the kiln-span decomposition target
// rather than inventing new ones, and still runs every cell through the
// SAME decompose-then-vary-Bi/phi/p pipeline as the bench cells. Dead time
// is held at the bench value across both spans (a TEST FIXTURE
// simplification -- see the audit doc). */
#define KILN_K0_C_PER_DUTY 2500.0f
#define KILN_TAU0_S 488.0f

// ---- [ASSUMED] fixture constants not pinned by the design doc's §3/§9,
// resolved once here so no reader has to guess (see the audit doc's
// "assumptions" section for the reasoning): ----
// A2 headroom: TIGHT keeps the bare measured heater_power_w; AMPLE scales it
// up, i.e. the true plant has more actuator authority per unit duty than
// TIGHT does for the same commanded ramp. 1.5x is a round, documented guess.
#define A2_AMPLE_HEADROOM_MULT 1.5f
// Sensor-node three-node parameters (c_s, its own tau) are OUT OF the
// decomposition helper's scope (sim_plant.h says so explicitly) -- held
// fixed across every cell; only sensor_bias_p (A3) varies.
#define FIXTURE_SENSOR_C_S_J_PER_C 500.0f
#define FIXTURE_SENSOR_TAU_S 15.0f
// A4 bench/kiln absolute spans (design doc sec 3's "bench 24->60" / "kiln
// 200->1250"), anchored the same way sim_high_temp.h's T_REF (55C) already
// is -- the driver applies sim_high_temp_scale_conductances() every tick
// using these as the ambient/segment targets, same mechanism S10/S11 use.
#define BENCH_AMBIENT_C 24.0f
#define BENCH_T1_OFFSET_C 16.0f  // ambient+16 = 40
#define BENCH_T2_OFFSET_C 36.0f  // ambient+36 = 60
#define KILN_AMBIENT_C 25.0f
#define KILN_T1_OFFSET_C 175.0f  // ambient+175 = 200
#define KILN_T2_OFFSET_C 1225.0f // ambient+1225 = 1250

// A6 tune mismatch triples (m_k, m_tau, m_L), SCENARIO_SIMULATION_PLAN.md
// sec 2.4 -- link the SIMC formula via pid_autotune_tune_from_fopdt(),
// never reimplement it; these are just the model-vs-plant multipliers.
typedef struct { float m_k, m_tau, m_l; } tune_mismatch_t;
static tune_mismatch_t tune_mismatch_for(sim_fac_a6_tune_t a6)
{
    switch (a6) {
        case SIM_FAC_A6_MATCHED:       return (tune_mismatch_t){1.0f, 1.0f, 1.0f};
        case SIM_FAC_A6_HOT:           return (tune_mismatch_t){0.5f, 2.0f, 0.5f};
        case SIM_FAC_A6_COLD:          return (tune_mismatch_t){2.0f, 0.5f, 2.0f};
        case SIM_FAC_A6_SLOW_INTEGRAL: return (tune_mismatch_t){1.0f, 3.0f, 1.0f};
        default:                       return (tune_mismatch_t){1.0f, 1.0f, 1.0f};
    }
}

// Three arms this dispatch requires, at minimum (per the task): plain PID,
// fuzzy-50, and the matched-effective-gain static arm (measured per-cell,
// never the centre-cell max).
typedef enum { CELL_ARM_PID = 0, CELL_ARM_FUZZY50, CELL_ARM_STATIC_MATCHED, CELL_ARM_COUNT } cell_arm_t;
static const char *const CELL_ARM_NAMES[CELL_ARM_COUNT] = { "A_PID", "A_FUZZY50", "A_STATIC_MATCHED" };

// Rule-cell occupancy: 3x3 dominant-membership bucket, same axis order as
// pid_fuzzy.c's own RULE_TABLE (0=NEG/FALLING,1=ZERO/STEADY,2=POS/RISING).
// This driver does not call into pid_fuzzy.c's internals (they are static);
// it re-derives the DOMINANT bucket from the same triangular membership
// shape pid_fuzzy.c documents in its own header (symmetric triangles
// centered at -band/0/+band) purely for histogram purposes -- it never
// feeds this back into a gain and is not what pid_fuzzy_adjust() itself
// uses internally (that function blends all three degrees continuously;
// this histogram answers "which single cell would a hard classifier land
// in," the same question the 2026-09-04 field capture (this file's own
// comment block references) asked of the real bench data).
static int dominant_bucket(float x, float band)
{
    if (band <= 0.0f || !isfinite(band)) return 1;
    if (x <= -band) return 0;
    if (x >= band) return 2;
    if (x <= 0.0f) return (-x) > (band + x) ? 0 : 1; /* nearer -band than 0 -> NEG else ZERO, by construction always ZERO- or NEG-leaning below 0 */
    return x > (band - x) ? 2 : 1;
}

typedef struct {
    bool ok;
    char refusal_reason[200];

    bool have_lag;         float lag_s;
    bool have_lag_signed;  float lag_signed_s;
    bool have_settle;      float settle_s_2c;
    bool have_steady;      float steady_rms_c;
    bool have_entry_peak;  float entry_peak_c;
    bool have_entry_under; float entry_undershoot_c;

    float applied_kp_mult_mean, applied_ki_mult_mean, applied_kd_mult_mean;
    float sat_frac;

    long rule_hist[9];
    long rule_hist_total;
} cell_firing_result_t;

// Builds a sim_plant_cfg_t + model (k_dc/tau_s/dead_time_s) + ambient/target
// temperatures for one factorial cell. Returns false (refuses) if the
// decomposition helper itself refuses -- never silently clamped.
static bool build_cell_plant(const sim_factorial_cell_t *cell, sim_plant_cfg_t *out_plant,
                              float *out_model_k_dc, float *out_model_tau_s, float *out_model_dead_time_s,
                              float *out_ambient_c, float *out_t1_offset_c, float *out_t2_offset_c,
                              char *refusal, size_t refusal_n)
{
    bool kiln_span = (cell->a4_loss_scale_span_r_s >= (SIM_FAC_A4_KILN_SPAN * 0.5f));
    float headroom_mult = (cell->a2_headroom == SIM_FAC_A2_AMPLE) ? A2_AMPLE_HEADROOM_MULT : 1.0f;
    float true_k = (kiln_span ? KILN_K0_C_PER_DUTY : BENCH_K0_C_PER_DUTY) * headroom_mult;
    float true_tau = kiln_span ? KILN_TAU0_S : BENCH_TAU0_S;

    sim_plant_decompose_req_t req = { .k = true_k, .tau_s = true_tau, .bi = cell->a8_bi, .phi = cell->a7_phi };
    sim_plant_cfg_t plant;
    memset(&plant, 0, sizeof(plant));
    if (!sim_plant_decompose_three_node(&req, &plant)) {
        snprintf(refusal, refusal_n,
                 "sim_plant_decompose_three_node() refused for k=%.4f tau=%.4f bi=%.4f phi=%.4f",
                 (double)true_k, (double)true_tau, (double)cell->a8_bi, (double)cell->a7_phi);
        return false;
    }

    plant.node_model = SIM_NODE_THREE;
    plant.c_s_j_per_c = FIXTURE_SENSOR_C_S_J_PER_C;
    plant.sensor_tau_s = FIXTURE_SENSOR_TAU_S;
    plant.sensor_bias_p = cell->a3_sensor_bias_p;
    plant.sensor_delay_s = BENCH_L0_S;
    plant.sensor_lag_tau_s = 0.0f;
    plant.load_mass_mult = cell->a1_load_mass_mult;

    plant.ambient_c = kiln_span ? KILN_AMBIENT_C : BENCH_AMBIENT_C;

    tune_mismatch_t tm = tune_mismatch_for(cell->a6_tune);

    *out_plant = plant;
    *out_model_k_dc = true_k * tm.m_k;
    *out_model_tau_s = true_tau * tm.m_tau;
    *out_model_dead_time_s = BENCH_L0_S * tm.m_l;
    *out_ambient_c = plant.ambient_c;
    *out_t1_offset_c = kiln_span ? KILN_T1_OFFSET_C : BENCH_T1_OFFSET_C;
    *out_t2_offset_c = kiln_span ? KILN_T2_OFFSET_C : BENCH_T2_OFFSET_C;
    return true;
}

// Near-duplicate of sim_scenarios.c's run_firing() -- see this file's own
// header comment for why this is a copy, not a shared call, and the audit
// doc for how the two were kept in sync. `static_mult` non-NULL selects the
// A_STATIC_MATCHED arm (fixed multipliers, fuzzy math bypassed entirely).
static bool run_cell_firing(const sim_plant_cfg_t *plant_cfg_in, float model_k_dc, float model_tau_s,
                             float model_dead_time_s, float ambient_c, float t1_offset_c, float t2_offset_c,
                             float ramp_rate_c_per_hr, cell_arm_t arm, const float *static_mult,
                             cell_firing_result_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!(model_k_dc > 0.0f) || !isfinite(model_k_dc) || !(model_tau_s > 0.0f) || !isfinite(model_tau_s) ||
        !(model_dead_time_s >= 0.0f) || !isfinite(model_dead_time_s)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "no valid model installed (k_dc=%.4f tau_s=%.4f dead_time_s=%.4f)",
                 (double)model_k_dc, (double)model_tau_s, (double)model_dead_time_s);
        return false;
    }

    float error_band_c, rate_band_c_per_s;
    if (!pid_fuzzy_derive_bands(model_k_dc, model_tau_s, &error_band_c, &rate_band_c_per_s)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "pid_fuzzy_derive_bands() refused for model_k_dc=%.4f model_tau_s=%.4f",
                 (double)model_k_dc, (double)model_tau_s);
        return false;
    }

    fopdt_model_t model;
    memset(&model, 0, sizeof(model));
    model.k_gain_c_per_duty = model_k_dc;
    model.tau_s = model_tau_s;
    model.dead_time_s = model_dead_time_s;
    model.valid = true;
    model.settled = true;
    model.tau_consistent_with_gain = true;
    model.extrapolation_converged = true;

    autotune_gains_t gains = pid_autotune_tune_from_fopdt(&model, AUTOTUNE_RULE_SIMC, 0.0f);
    if (gains.refusal != AUTOTUNE_REFUSAL_OK) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "pid_autotune_tune_from_fopdt() refused: %s", gains.refusal_reason);
        return false;
    }
    float base_kp = gains.kp, base_ki = gains.ki, base_kd = gains.kd;

    bool static_matched = (arm == CELL_ARM_STATIC_MATCHED);
    uint8_t strength_pct = (arm == CELL_ARM_FUZZY50) ? 50 : 0;
    float static_kp_mult = static_mult ? static_mult[0] : 1.0f;
    float static_ki_mult = static_mult ? static_mult[1] : 1.0f;
    float static_kd_mult = static_mult ? static_mult[2] : 1.0f;

    sim_plant_cfg_t plant_cfg = *plant_cfg_in;
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &plant_cfg);
    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    bool zone_captured = false;
    firing_score_cfg_t fscfg;
    memset(&fscfg, 0, sizeof(fscfg));
    fscfg.band_c = 5.0f;

    float t1 = ambient_c + t1_offset_c;
    float t2 = ambient_c + t2_offset_c;
    float rate_c_per_s = ramp_rate_c_per_hr / 3600.0f;

    int ramp1_ticks = (int)((t1 - ambient_c) / rate_c_per_s / DT_S);
    int ramp2_ticks = (int)((t2 - t1) / rate_c_per_s / DT_S);
    if (ramp1_ticks < 1) ramp1_ticks = 1;
    if (ramp2_ticks < 1) ramp2_ticks = 1;
    int dwell_ticks = (int)(6.0f * model_tau_s / DT_S);
    if (dwell_ticks < 1200) dwell_ticks = 1200;
    if (dwell_ticks > 20000) dwell_ticks = 20000;

    double kp_mult_sum = 0.0, ki_mult_sum = 0.0, kd_mult_sum = 0.0;
    long ramp_tick_count = 0;
    long sat_tick_count = 0, total_tick_count = 0;
    bool nan_seen = false;
    float ceiling_c = ambient_c + model_k_dc + 400.0f;
    float floor_c = ambient_c - 1.0f;
    bool bounds_ok = true;

    firing_segment_score_t seg_scores[4];
    int seg_count = 0;

    struct { bool is_ramp; float rate_c_per_hr_signed; float target_start, target_end; int ticks; } segs[4] = {
        { true,  ramp_rate_c_per_hr, ambient_c, t1, ramp1_ticks },
        { false, 0.0f,               t1, t1, dwell_ticks },
        { true,  ramp_rate_c_per_hr, t1, t2, ramp2_ticks },
        { false, 0.0f,               t2, t2, dwell_ticks },
    };

    for (int s = 0; s < 4; s++) {
        firing_score_seg_t seg;
        float mean_target_c = segs[s].is_ramp ? (segs[s].target_start + segs[s].target_end) / 2.0f
                                               : segs[s].target_start;
        firing_score_seg_begin(&seg, &fscfg, 0, segs[s].rate_c_per_hr_signed, mean_target_c,
                               model_dead_time_s, model_tau_s);

        float target_c = segs[s].target_start;
        float step_per_tick = segs[s].is_ramp ? (segs[s].target_end - segs[s].target_start) / (float)segs[s].ticks
                                               : 0.0f;

        for (int t = 0; t < segs[s].ticks; t++) {
            if (segs[s].is_ramp) target_c += step_per_tick;

            float error_c = target_c - pstate.sensor_c;
            float rate_meas = pid_state.d_filtered;
            float adj_kp = base_kp, adj_ki = base_ki, adj_kd = base_kd;
            if (static_matched) {
                adj_kp = base_kp * static_kp_mult;
                adj_ki = base_ki * static_ki_mult;
                adj_kd = base_kd * static_kd_mult;
            } else {
                pid_fuzzy_adjust(error_c, rate_meas, error_band_c, rate_band_c_per_s,
                                 base_kp, base_ki, base_kd, strength_pct, &adj_kp, &adj_ki, &adj_kd);
            }

            pid_rescale_integral_for_new_ki(&pid_state, prev_effective_ki, adj_ki);
            prev_effective_ki = adj_ki;

            pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
            float duty = pid_update(&pid_state, &cfg, target_c, pstate.sensor_c, DT_S, 0.0f, 0.0f);

            sim_high_temp_scale_conductances(&plant_cfg, plant_cfg_in->g_ea_w_per_c, plant_cfg_in->g_la_w_per_c,
                                              pstate.load_c);
            sim_plant_three_node_step(&pstate, &plant_cfg, duty, DT_S);

            if (isnan(pstate.sensor_c) || isnan(pstate.element_c)) nan_seen = true;
            if (pstate.sensor_c < floor_c || pstate.sensor_c > ceiling_c) bounds_ok = false;

            bool saturated_high = duty >= 0.98f;
            total_tick_count++;
            if (saturated_high) sat_tick_count++;
            bool was_captured = zone_captured;
            firing_score_seg_tick(&seg, &zone_captured, target_c, pstate.sensor_c, saturated_high, DT_S);

            if (segs[s].is_ramp && (was_captured || zone_captured) && !(saturated_high && error_c > 0.0f)) {
                kp_mult_sum += (base_kp != 0.0f) ? (adj_kp / base_kp) : 1.0;
                ki_mult_sum += (base_ki != 0.0f) ? (adj_ki / base_ki) : 1.0;
                kd_mult_sum += (base_kd != 0.0f) ? (adj_kd / base_kd) : 1.0;
                ramp_tick_count++;
            }

            /* Rule-cell occupancy: every tick, whole firing, regardless of
             * arm or capture -- this answers "where does this cell's own
             * (error, rate) trajectory sit relative to the fuzzy bands," a
             * question about the operating point, not about whether fuzzy
             * happened to be engaged this arm. */
            int eb = dominant_bucket(error_c, error_band_c);
            int rb = dominant_bucket(rate_meas, rate_band_c_per_s);
            out->rule_hist[eb * 3 + rb]++;
            out->rule_hist_total++;
        }

        firing_segment_score_t score;
        if (firing_score_seg_finish(&seg, &score) && seg_count < 4) {
            seg_scores[seg_count++] = score;
        }
    }

    if (pstate.delay_truncated) {
        /* sec 6.1: the sensor delay ring ran out of capacity and silently
         * shortened the effective dead time -- the exact DT_S-trap mechanism
         * that once made a 76.9s cell look byte-identical to an unrelated
         * 64.0s one. Refuse loudly rather than measure a wrong dead time. */
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "sensor delay ring truncated (sensor_delay_s=%.2f exceeds SIM_PLANT_DELAY_MAX_STEPS*dt=%.2f)",
                 (double)plant_cfg_in->sensor_delay_s, (double)(SIM_PLANT_DELAY_MAX_STEPS * DT_S));
        return false;
    }
    if (nan_seen) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "NaN observed in plant state");
        return false;
    }
    if (!bounds_ok) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "sensor reading left [%.1f, %.1f]",
                 (double)floor_c, (double)ceiling_c);
        return false;
    }

    double lag_sum = 0.0; int lag_n = 0;
    double lag_signed_sum = 0.0; int lag_signed_n = 0;
    double settle_sum = 0.0; int settle_n = 0;
    double steady_sum = 0.0; int steady_n = 0;
    double peak_sum = 0.0; int peak_n = 0;
    double under_sum = 0.0; int under_n = 0;
    for (int i = 0; i < seg_count; i++) {
        const firing_segment_score_t *sc2 = &seg_scores[i];
        if (sc2->has[FIRING_SUBSCORE_LAG_S]) { lag_sum += sc2->value[FIRING_SUBSCORE_LAG_S]; lag_n++; }
        if (sc2->has[FIRING_SUBSCORE_LAG_SIGNED_S]) { lag_signed_sum += sc2->value[FIRING_SUBSCORE_LAG_SIGNED_S]; lag_signed_n++; }
        if (sc2->has[FIRING_SUBSCORE_SETTLE_S]) { settle_sum += sc2->value[FIRING_SUBSCORE_SETTLE_S]; settle_n++; }
        if (sc2->has[FIRING_SUBSCORE_STEADY_RMS_C]) { steady_sum += sc2->value[FIRING_SUBSCORE_STEADY_RMS_C]; steady_n++; }
        if (sc2->has[FIRING_SUBSCORE_ENTRY_PEAK_C]) { peak_sum += sc2->value[FIRING_SUBSCORE_ENTRY_PEAK_C]; peak_n++; }
        if (sc2->has[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C]) { under_sum += sc2->value[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C]; under_n++; }
    }
    if (lag_n) { out->have_lag = true; out->lag_s = (float)(lag_sum / lag_n); }
    if (lag_signed_n) { out->have_lag_signed = true; out->lag_signed_s = (float)(lag_signed_sum / lag_signed_n); }
    if (settle_n) { out->have_settle = true; out->settle_s_2c = (float)(settle_sum / settle_n); }
    if (steady_n) { out->have_steady = true; out->steady_rms_c = (float)(steady_sum / steady_n); }
    if (peak_n) { out->have_entry_peak = true; out->entry_peak_c = (float)(peak_sum / peak_n); }
    if (under_n) { out->have_entry_under = true; out->entry_undershoot_c = (float)(under_sum / under_n); }

    out->sat_frac = total_tick_count ? (float)((double)sat_tick_count / (double)total_tick_count) : 0.0f;
    out->applied_kp_mult_mean = ramp_tick_count ? (float)(kp_mult_sum / ramp_tick_count) : 1.0f;
    out->applied_ki_mult_mean = ramp_tick_count ? (float)(ki_mult_sum / ramp_tick_count) : 1.0f;
    out->applied_kd_mult_mean = ramp_tick_count ? (float)(kd_mult_sum / ramp_tick_count) : 1.0f;

    /* A cell whose PID/base arm is stuck saturated for essentially the
     * whole firing is not exercising the control law at all -- u_req
     * perpetually saturated. This is the "infeasible, refuse loudly"
     * requirement for a cell the mask did not catch (the mask only
     * excludes the one analytically-derived corner; this is the runtime
     * backstop). 0.97 leaves room for the legitimate brief saturation a
     * fast ramp's entry transient produces. */
    if (out->sat_frac >= 0.97f) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "duty saturated (>=0.98) for %.1f%% of the firing -- u_req perpetually saturated, infeasible cell",
                 (double)(out->sat_frac * 100.0f));
        return false;
    }

    out->ok = true;
    return true;
}

static const char *fmt_val(char *buf, size_t n, bool have, float v)
{
    if (!have) { snprintf(buf, n, "NA"); return buf; }
    snprintf(buf, n, "%.4f", (double)v);
    return buf;
}

int main(int argc, char **argv)
{
    SIM_PLANT_ASSERT_ABI_FRESH();

    int shard = 0, of = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shard") == 0 && i + 1 < argc) shard = atoi(argv[++i]);
        else if (strcmp(argv[i], "--of") == 0 && i + 1 < argc) of = atoi(argv[++i]);
    }
    if (of < 1 || shard < 0 || shard >= of) {
        printf("sim_factorial_driver: invalid --shard/--of (need 0 <= shard < of, of >= 1); got shard=%d of=%d\n",
               shard, of);
        return 1;
    }

    static sim_factorial_cell_t cells[SIM_FACTORIAL_MAX_CELLS];
    size_t n = sim_factorial_generate(cells, SIM_FACTORIAL_MAX_CELLS);
    if (n != SIM_FACTORIAL_TOTAL_COUNT_EXPECTED) {
        printf("sim_factorial_driver: FATAL sim_factorial_generate() returned %zu cells, expected %d\n",
               n, SIM_FACTORIAL_TOTAL_COUNT_EXPECTED);
        return 1;
    }

    printf("=== sim_factorial_driver: %zu cells x %d arms (A_PID, A_FUZZY50, A_STATIC_MATCHED) ===\n", n, CELL_ARM_COUNT);
    printf("shard=%d of=%d. Report numbers, not verdicts -- effects analysis (P8/P11) is a separate dispatch.\n\n", shard, of);

    printf("cell_id\tstage\ta1_load_mass_mult\ta2_headroom\ta3_sensor_bias_p\ta4_loss_scale_span_r_s\t"
           "a5_ramp_rate_c_per_hr\ta6_tune\ta7_phi\ta8_bi\tarm\tlag_s\tlag_signed_s\tsettle_s_2c\t"
           "steady_rms_c\tentry_peak_c\tentry_undershoot_c\tsat_frac\tapplied_kp_mult_mean\t"
           "applied_ki_mult_mean\tapplied_kd_mult_mean\trulecell_center_frac\trulecell_hist9\n");

    long rows_emitted = 0, cells_refused = 0;
    float occ_min = 2.0f, occ_max = -1.0f;

    for (size_t ci = 0; ci < n; ci++) {
        if ((ci % (size_t)of) != (size_t)shard) continue;
        const sim_factorial_cell_t *cell = &cells[ci];

        sim_plant_cfg_t plant;
        float model_k_dc, model_tau_s, model_dead_time_s, ambient_c, t1_off, t2_off;
        char build_refusal[200];
        if (!build_cell_plant(cell, &plant, &model_k_dc, &model_tau_s, &model_dead_time_s,
                               &ambient_c, &t1_off, &t2_off, build_refusal, sizeof(build_refusal))) {
            printf("CELL_REFUSED %s (all arms): %s\n", cell->cell_id, build_refusal);
            cells_refused++;
            continue;
        }

        float measured_mult[3] = {1.0f, 1.0f, 1.0f};
        bool cell_ok = true;

        cell_firing_result_t r_fuzzy50;
        bool have_fuzzy50 = run_cell_firing(&plant, model_k_dc, model_tau_s, model_dead_time_s, ambient_c,
                                             t1_off, t2_off, cell->a5_ramp_rate_c_per_hr, CELL_ARM_FUZZY50, NULL,
                                             &r_fuzzy50);
        if (have_fuzzy50) {
            measured_mult[0] = r_fuzzy50.applied_kp_mult_mean;
            measured_mult[1] = r_fuzzy50.applied_ki_mult_mean;
            measured_mult[2] = r_fuzzy50.applied_kd_mult_mean;
        } else {
            printf("CELL_REFUSED %s A_FUZZY50: %s\n", cell->cell_id, r_fuzzy50.refusal_reason);
            cell_ok = false;
        }

        for (int ai = 0; ai < CELL_ARM_COUNT; ai++) {
            cell_arm_t arm = (cell_arm_t)ai;
            if (arm == CELL_ARM_FUZZY50) {
                if (!have_fuzzy50) continue;
            }
            cell_firing_result_t r;
            bool ok;
            if (arm == CELL_ARM_FUZZY50) {
                r = r_fuzzy50;
                ok = true;
            } else {
                const float *mult_arg = (arm == CELL_ARM_STATIC_MATCHED) ? measured_mult : NULL;
                if (arm == CELL_ARM_STATIC_MATCHED && !have_fuzzy50) continue; /* nothing to match against */
                ok = run_cell_firing(&plant, model_k_dc, model_tau_s, model_dead_time_s, ambient_c, t1_off, t2_off,
                                      cell->a5_ramp_rate_c_per_hr, arm, mult_arg, &r);
            }
            if (!ok) {
                printf("CELL_REFUSED %s %s: %s\n", cell->cell_id, CELL_ARM_NAMES[ai], r.refusal_reason);
                cell_ok = false;
                continue;
            }

            char buf1[32], buf2[32], buf3[32], buf4[32], buf5[32], buf6[32];
            float center_frac = r.rule_hist_total ? (float)((double)r.rule_hist[4] / (double)r.rule_hist_total) : 0.0f;
            if (center_frac < occ_min) occ_min = center_frac;
            if (center_frac > occ_max) occ_max = center_frac;

            printf("%s\t%d\t%.4f\t%s\t%.4f\t%.4f\t%.1f\t%s\t%.4f\t%.4f\t%s\t"
                   "%s\t%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t",
                   cell->cell_id, (int)cell->stage,
                   (double)cell->a1_load_mass_mult, cell->a2_headroom == SIM_FAC_A2_TIGHT ? "TIGHT" : "AMPLE",
                   (double)cell->a3_sensor_bias_p, (double)cell->a4_loss_scale_span_r_s,
                   (double)cell->a5_ramp_rate_c_per_hr,
                   cell->a6_tune == SIM_FAC_A6_MATCHED ? "MATCHED" : cell->a6_tune == SIM_FAC_A6_HOT ? "HOT" :
                       cell->a6_tune == SIM_FAC_A6_COLD ? "COLD" : "SLOW_INTEGRAL",
                   (double)cell->a7_phi, (double)cell->a8_bi, CELL_ARM_NAMES[ai],
                   fmt_val(buf1, sizeof(buf1), r.have_lag, r.lag_s),
                   fmt_val(buf2, sizeof(buf2), r.have_lag_signed, r.lag_signed_s),
                   fmt_val(buf3, sizeof(buf3), r.have_settle, r.settle_s_2c),
                   fmt_val(buf4, sizeof(buf4), r.have_steady, r.steady_rms_c),
                   fmt_val(buf5, sizeof(buf5), r.have_entry_peak, r.entry_peak_c),
                   fmt_val(buf6, sizeof(buf6), r.have_entry_under, r.entry_undershoot_c),
                   (double)r.sat_frac, (double)r.applied_kp_mult_mean, (double)r.applied_ki_mult_mean,
                   (double)r.applied_kd_mult_mean, (double)center_frac);
            for (int h = 0; h < 9; h++) {
                printf("%.4f%s", r.rule_hist_total ? (double)((double)r.rule_hist[h] / (double)r.rule_hist_total) : 0.0,
                       h < 8 ? ":" : "");
            }
            printf("\n");
            rows_emitted++;
        }
        if (!cell_ok) cells_refused++;
    }

    printf("\n=== sim_factorial_driver: shard %d/%d done. rows_emitted=%ld cells_with_a_refusal=%ld "
           "rulecell_center_frac_range=[%.4f,%.4f] ===\n",
           shard, of, rows_emitted, cells_refused, (double)(occ_max < 0.0f ? 0.0f : occ_min), (double)(occ_max < 0.0f ? 0.0f : occ_max));
    return 0;
}
