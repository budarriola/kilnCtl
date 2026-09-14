// sim_scenarios -- WI-4 (docs/SCENARIO_SIMULATION_PLAN.md sec 4.3, 7).
//
// Runner skeleton + scenario table + arm selection, for S0/S1/S3 only, all
// six arms. See sim_scenario_table.h for the data table and its "adding a
// scenario needs no runner change" contract.
//
// EXPLICIT WI-4 SCOPE LIMITS (read before trusting a number this prints):
//   - SIM_ARM_PID_AT / SIM_ARM_FUZZY_AT run ONE firing here, identical to
//     SIM_ARM_PID / SIM_ARM_FUZZY50 respectively (adaptive_tune is never
//     invoked). The 9-firing carried-state sequence is WI-8's job. Every row
//     for these two arms carries the literal string "WI8_PENDING" in its
//     notes column.
//   - SIM_ARM_STATIC_MATCHED runs with its gain multipliers pinned at 1.0
//     (i.e. numerically identical to SIM_ARM_PID) because the ramp-phase
//     mean multiplier measurement is WI-5's job, not this file's. Its rows
//     carry "WI5_PENDING" in notes and MUST NOT be read as a real
//     A_STATIC_MATCHED result.
//   - SETTLE_S is reported at the production 2.0 degC band only.
//     WI-4/WI-6's dual-band (also 0.5 degC) sensitivity check is deferred --
//     see the note at the bottom of this file.
//   - ki_state is driven from ONE flag, g_sim_ki_withheld below, per this
//     task's explicit instruction (WI-9 is superseded/in flight elsewhere;
//     flipping this one flag is the entire integration point once it lands).
//
// WHAT THIS LINKS, REAL PRODUCTION CODE, NO MIRROR: pid.c, pid_fuzzy.c,
// pid_autotune.c, firing_score.c, sim_plant.c. The only "mirrored" logic
// here is orchestration (which functions get called in which order), same
// posture sim_fuzzy_closedloop.c's own header comment documents and
// justifies -- read that file first if anything below looks unfamiliar,
// since the per-tick wiring is copied from it deliberately (it is the
// existing, previously negative-tested wiring for exactly this sequence:
// error_c/rate_c_per_s -> pid_fuzzy_adjust() -> pid_rescale_integral_for_
// new_ki() -> pid_update() -> plant step).
//
// Usage: sim_scenarios.exe [--format tsv|human] [--shard I --of N]
// --shard/--of is WI-7's job; accepted here as a no-op (I must be 0, N must
// be 1) so WI-7 does not have to touch this file's argument parsing, only
// its own filtering logic once sharding lands.
#include "../drivers/control/pid.h"
#include "../drivers/control/pid_autotune.h"
#include "../drivers/control/pid_fuzzy.h"
#include "../drivers/control/firing_score.h"
#include "sim_plant.h"
#include "sim_scenario_table.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_S 1.0f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f
#define MATERIALITY_C 0.5f

// Single flag driving KI_WITHHELD labelling (sec 1.4). Flip to false, and
// nothing else in this file, the day WI-9 lands and adaptive_tune_ki.c can
// safely adapt Ki under fuzzy. Currently: withheld, always, per e78fbc5b.
static const bool g_sim_ki_withheld = true;

typedef struct {
    bool ok;
    char refusal_reason[160];

    bool have_lag;          float lag_s;
    bool have_lag_signed;   float lag_signed_s;
    bool have_settle;       float settle_s_2c;
    bool have_steady;       float steady_rms_c;
    bool have_entry_peak;   float entry_peak_c;
    bool have_entry_under;  float entry_undershoot_c;

    float applied_kp_mult_mean;
    float applied_ki_mult_mean;
    float applied_kd_mult_mean;
    bool  ki_withheld;
    int   refusals; /* structural-invariant violations counted, not just first-hit */
} firing_result_t;

static uint8_t arm_strength_pct(sim_arm_t arm)
{
    switch (arm) {
        case SIM_ARM_PID:
        case SIM_ARM_PID_AT:
        case SIM_ARM_STATIC_MATCHED:
            return 0;
        case SIM_ARM_FUZZY25:
            return 25;
        case SIM_ARM_FUZZY50:
        case SIM_ARM_FUZZY_AT:
            return 50;
        default:
            return 0;
    }
}

static bool arm_is_adaptive(sim_arm_t arm)
{
    return arm == SIM_ARM_PID_AT || arm == SIM_ARM_FUZZY_AT;
}

static bool arm_is_static_matched(sim_arm_t arm)
{
    return arm == SIM_ARM_STATIC_MATCHED;
}

// Per-tick control wiring is inlined directly in run_firing()'s own segment
// loop below (bands/gains/plant dispatch all need locals that loop already
// holds) -- EXACT wiring sim_fuzzy_closedloop.c's sim_tick() documents and
// negative-tested (strength_pct==0 bit-exact contract), with node_model
// dispatch added since this suite must run both legacy and three-node
// plants from the same loop, unlike that single-plant file.
static bool run_firing(const sim_scenario_t *sc, sim_arm_t arm, firing_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ki_withheld = g_sim_ki_withheld && (arm == SIM_ARM_FUZZY_AT);

    if (!(sc->model_k_dc > 0.0f) || !isfinite(sc->model_k_dc) ||
        !(sc->model_tau_s > 0.0f) || !isfinite(sc->model_tau_s) ||
        !(sc->model_dead_time_s >= 0.0f) || !isfinite(sc->model_dead_time_s)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "no valid model installed (k_dc=%.4f tau_s=%.4f dead_time_s=%.4f)",
                 (double)sc->model_k_dc, (double)sc->model_tau_s, (double)sc->model_dead_time_s);
        return false;
    }

    float error_band_c, rate_band_c_per_s;
    bool bands_ok = pid_fuzzy_derive_bands(sc->model_k_dc, sc->model_tau_s, &error_band_c, &rate_band_c_per_s);
    if (!bands_ok) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "pid_fuzzy_derive_bands() returned false for model_k_dc=%.4f model_tau_s=%.4f",
                 (double)sc->model_k_dc, (double)sc->model_tau_s);
        return false;
    }

    fopdt_model_t model;
    memset(&model, 0, sizeof(model));
    model.k_gain_c_per_duty = sc->model_k_dc;
    model.tau_s = sc->model_tau_s;
    model.dead_time_s = sc->model_dead_time_s;
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

    uint8_t strength_pct = arm_strength_pct(arm);
    bool static_matched = arm_is_static_matched(arm);
    /* WI-5 pending -- see file header. */
    float static_kp_mult = 1.0f, static_ki_mult = 1.0f, static_kd_mult = 1.0f;

    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &sc->plant);
    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    bool zone_captured = false;
    firing_score_cfg_t fscfg;
    memset(&fscfg, 0, sizeof(fscfg));
    fscfg.band_c = 5.0f;

    float ambient = sc->ambient_c;
    float t1 = ambient + sc->t1_offset_c;
    float t2 = ambient + sc->t2_offset_c;
    float rate_c_per_s = sc->ramp_rate_c_per_hr / 3600.0f;

    int ramp1_ticks = (int)((t1 - ambient) / rate_c_per_s / DT_S);
    int ramp2_ticks = (int)((t2 - t1) / rate_c_per_s / DT_S);
    if (ramp1_ticks < 1) ramp1_ticks = 1;
    if (ramp2_ticks < 1) ramp2_ticks = 1;
    int dwell_ticks = (int)(6.0f * sc->model_tau_s / DT_S);
    if (dwell_ticks < 1200) dwell_ticks = 1200;

    double kp_mult_sum = 0.0, ki_mult_sum = 0.0, kd_mult_sum = 0.0;
    long tick_count = 0;
    bool all_bitexact = true;
    bool nan_seen = false;
    float ceiling_c = ambient + sc->model_k_dc + 50.0f;
    float floor_c = ambient - 1.0f;
    bool bounds_ok = true;

    firing_segment_score_t seg_scores[4];
    int seg_count = 0;

    /* one segment loop, parameterized so the same code runs all 4 segments */
    struct { bool is_ramp; float rate_c_per_hr_signed; float target_start, target_end; int ticks; } segs[4] = {
        { true,  sc->ramp_rate_c_per_hr, ambient, t1, ramp1_ticks },
        { false, 0.0f,                   t1, t1, dwell_ticks },
        { true,  sc->ramp_rate_c_per_hr, t1, t2, ramp2_ticks },
        { false, 0.0f,                   t2, t2, dwell_ticks },
    };

    for (int s = 0; s < 4; s++) {
        firing_score_seg_t seg;
        float mean_target_c = segs[s].is_ramp ? (segs[s].target_start + segs[s].target_end) / 2.0f
                                               : segs[s].target_start;
        firing_score_seg_begin(&seg, &fscfg, 0, segs[s].rate_c_per_hr_signed, mean_target_c,
                               sc->model_dead_time_s, sc->model_tau_s);

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
            if (strength_pct == 0 && !static_matched) {
                if (!(adj_kp == base_kp && adj_ki == base_ki && adj_kd == base_kd)) all_bitexact = false;
            }

            pid_rescale_integral_for_new_ki(&pid_state, prev_effective_ki, adj_ki);
            prev_effective_ki = adj_ki;

            pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
            float duty = pid_update(&pid_state, &cfg, target_c, pstate.sensor_c, DT_S, 0.0f, 0.0f);

            if (sc->plant.node_model == SIM_NODE_THREE) {
                sim_plant_three_node_step(&pstate, &sc->plant, duty, DT_S);
            } else {
                sim_plant_step(&pstate, &sc->plant, duty, DT_S);
            }

            if (isnan(pstate.sensor_c) || isnan(pstate.element_c)) nan_seen = true;
            if (pstate.sensor_c < floor_c || pstate.sensor_c > ceiling_c) bounds_ok = false;

            bool saturated_high = duty >= 0.98f;
            firing_score_seg_tick(&seg, &zone_captured, target_c, pstate.sensor_c, saturated_high, DT_S);

            kp_mult_sum += (base_kp != 0.0f) ? (adj_kp / base_kp) : 1.0;
            ki_mult_sum += (base_ki != 0.0f) ? (adj_ki / base_ki) : 1.0;
            kd_mult_sum += (base_kd != 0.0f) ? (adj_kd / base_kd) : 1.0;
            tick_count++;
        }

        firing_segment_score_t score;
        if (firing_score_seg_finish(&seg, &score) && seg_count < 4) {
            seg_scores[seg_count++] = score;
        }
    }

    if (nan_seen) { out->refusals++; snprintf(out->refusal_reason, sizeof(out->refusal_reason), "NaN observed in plant state"); }
    if (!bounds_ok) { out->refusals++; snprintf(out->refusal_reason, sizeof(out->refusal_reason), "sensor reading left [%.1f, %.1f]", (double)floor_c, (double)ceiling_c); }
    if (strength_pct == 0 && !static_matched && !all_bitexact) {
        out->refusals++;
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "strength_pct=0 bit-exact contract broke mid-firing");
    }

    /* Aggregate the two ramps and the two dwells (mean), per sec 4.2's
     * "enough for a per-scenario comparison without inflating runtime." */
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

    out->applied_kp_mult_mean = tick_count ? (float)(kp_mult_sum / tick_count) : 1.0f;
    out->applied_ki_mult_mean = tick_count ? (float)(ki_mult_sum / tick_count) : 1.0f;
    out->applied_kd_mult_mean = tick_count ? (float)(kd_mult_sum / tick_count) : 1.0f;

    out->ok = (out->refusals == 0);
    if (!out->ok && out->refusal_reason[0] == '\0') {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "structural invariant violated");
    }
    return out->ok;
}

static float diff_or_zero(bool a_have, float a, bool b_have, float b)
{
    if (!a_have || !b_have) return 0.0f;
    return fabsf(a - b);
}

// field_offset_marker: 0=lag_s 1=steady_rms_c 2=entry_peak_c 3=entry_undershoot_c.
// ramp_rate_c_per_s converts LAG_S (seconds) to its degC equivalent at this
// scenario's own commanded rate before comparing to the 0.5 degC materiality
// line -- sec 5.1's table is explicit that objective 1's materiality is
// "0.5 degC, converted at the segment's ramp rate," never a raw 0.5 s figure.
static float max_pairwise_diff(const firing_result_t results[SIM_ARM_COUNT], int field_offset_marker,
                               float ramp_rate_c_per_s)
{
    float maxd = 0.0f;
    for (int i = 0; i < SIM_ARM_COUNT; i++) {
        for (int j = i + 1; j < SIM_ARM_COUNT; j++) {
            float d = 0.0f;
            switch (field_offset_marker) {
                case 0:
                    d = diff_or_zero(results[i].have_lag, results[i].lag_s, results[j].have_lag, results[j].lag_s);
                    d *= ramp_rate_c_per_s;
                    break;
                case 1: d = diff_or_zero(results[i].have_steady, results[i].steady_rms_c, results[j].have_steady, results[j].steady_rms_c); break;
                case 2: d = diff_or_zero(results[i].have_entry_peak, results[i].entry_peak_c, results[j].have_entry_peak, results[j].entry_peak_c); break;
                case 3: d = diff_or_zero(results[i].have_entry_under, results[i].entry_undershoot_c, results[j].have_entry_under, results[j].entry_undershoot_c); break;
            }
            if (d > maxd) maxd = d;
        }
    }
    return maxd;
}

// WI-4 acceptance (a): re-verify the strength_pct==0 bit-exact contract
// INSIDE this suite, rather than citing test_pid_fuzzy.c/1570a65a. Cheap,
// deterministic, a handful of representative (error, rate) points.
static bool self_check_strength_zero_contract(void)
{
    const float errs[] = { -30.0f, -5.0f, -0.1f, 0.0f, 0.1f, 5.0f, 30.0f };
    const float rates[] = { -1.0f, -0.01f, 0.0f, 0.01f, 1.0f };
    bool ok = true;
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        for (size_t j = 0; j < sizeof(rates) / sizeof(rates[0]); j++) {
            float kp = 0.0318f, ki = 0.0002f, kd = 0.84f;
            float akp = kp, aki = ki, akd = kd;
            pid_fuzzy_adjust(errs[i], rates[j], 20.0f, 0.5f, kp, ki, kd, 0, &akp, &aki, &akd);
            if (!(akp == kp && aki == ki && akd == kd)) {
                printf("SELF_CHECK_FAIL strength_pct=0 did not reproduce base gains at err=%.3f rate=%.3f\n",
                       (double)errs[i], (double)rates[j]);
                ok = false;
            }
        }
    }
    return ok;
}

static const char *fmt_val(char *buf, size_t n, bool have, float v)
{
    if (!have) { snprintf(buf, n, "NA"); return buf; }
    snprintf(buf, n, "%.4f", (double)v);
    return buf;
}

int main(int argc, char **argv)
{
    int shard = 0, of = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shard") == 0 && i + 1 < argc) shard = atoi(argv[++i]);
        else if (strcmp(argv[i], "--of") == 0 && i + 1 < argc) of = atoi(argv[++i]);
    }
    if (of != 1 || shard != 0) {
        printf("sim_scenarios: --shard/--of is WI-7's job; this build only runs --shard 0 --of 1\n");
        return 1;
    }

    printf("=== sim_scenarios (WI-4): S0/S1/S3, all six arms ===\n");
    printf("See this file's top comment: SIM_ARM_PID_AT/SIM_ARM_FUZZY_AT are single-firing\n"
           "stand-ins (WI-8 pending), SIM_ARM_STATIC_MATCHED's multipliers are pinned at 1.0\n"
           "(WI-5 pending), and SETTLE_S is the production 2.0 degC band only.\n\n");

    if (!self_check_strength_zero_contract()) {
        printf("=== sim_scenarios: FAIL (strength_pct==0 contract) ===\n");
        return 1;
    }
    printf("self-check: strength_pct=0 bit-exact contract PASS\n\n");

    const sim_scenario_t *table = sim_scenario_table();
    int n = SIM_SCENARIO_COUNT;

    printf("scenario\tarm\tfiring\tki_state\tlag_s\tlag_signed_s\tsettle_s_2c\tsteady_rms_c\t"
           "entry_peak_c\tentry_undershoot_c\tapplied_kp_mult_mean\tapplied_ki_mult_mean\t"
           "applied_kd_mult_mean\trefusals\tnotes\n");

    firing_result_t all_results[16][SIM_ARM_COUNT]; /* n <= 16 comfortably covers WI-4/WI-6 */
    bool any_fail = false;

    for (int si = 0; si < n; si++) {
        const sim_scenario_t *sc = &table[si];
        for (int ai = 0; ai < SIM_ARM_COUNT; ai++) {
            sim_arm_t arm = (sim_arm_t)ai;
            firing_result_t r;
            bool ok = run_firing(sc, arm, &r);
            all_results[si][ai] = r;

            if (!ok) {
                printf("SCENARIO_REFUSED %s %s: %s\n", sc->id, SIM_ARM_NAMES[ai], r.refusal_reason);
                fflush(stdout);
                return 1; /* never emit a row of numbers for a refused scenario */
            }

            char buf1[32], buf2[32], buf3[32], buf4[32], buf5[32], buf6[32];
            const char *notes = "";
            if (arm_is_adaptive(arm)) notes = "WI8_PENDING:single-firing-stand-in,adaptive_tune-not-invoked";
            else if (arm_is_static_matched(arm)) notes = "WI5_PENDING:multipliers-pinned-1.0,NOT-a-real-A_STATIC_MATCHED-result";

            printf("%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%d\t%s\n",
                   sc->id, SIM_ARM_NAMES[ai], 0,
                   r.ki_withheld ? "KI_WITHHELD" : "KI_ACTIVE",
                   fmt_val(buf1, sizeof(buf1), r.have_lag, r.lag_s),
                   fmt_val(buf2, sizeof(buf2), r.have_lag_signed, r.lag_signed_s),
                   fmt_val(buf3, sizeof(buf3), r.have_settle, r.settle_s_2c),
                   fmt_val(buf4, sizeof(buf4), r.have_steady, r.steady_rms_c),
                   fmt_val(buf5, sizeof(buf5), r.have_entry_peak, r.entry_peak_c),
                   fmt_val(buf6, sizeof(buf6), r.have_entry_under, r.entry_undershoot_c),
                   (double)r.applied_kp_mult_mean, (double)r.applied_ki_mult_mean, (double)r.applied_kd_mult_mean,
                   r.refusals, notes);
        }
    }

    printf("\n--- WI-4 acceptance checks ---\n");

    /* (b) S0: all arms identical on all four objectives within materiality. */
    {
        int s0 = 0; /* table[0] == S0 by construction */
        float rate0 = table[s0].ramp_rate_c_per_hr / 3600.0f;
        float d_lag = max_pairwise_diff(all_results[s0], 0, rate0);
        float d_steady = max_pairwise_diff(all_results[s0], 1, rate0);
        float d_peak = max_pairwise_diff(all_results[s0], 2, rate0);
        float d_under = max_pairwise_diff(all_results[s0], 3, rate0);
        printf("S0 max pairwise diffs (lag_s converted to its degC equivalent at %.4f degC/hr): "
               "lag_s_equiv_c=%.4f steady_rms_c=%.4f entry_peak_c=%.4f entry_undershoot_c=%.4f\n",
               (double)table[s0].ramp_rate_c_per_hr,
               (double)d_lag, (double)d_steady, (double)d_peak, (double)d_under);
        if (d_lag > MATERIALITY_C || d_steady > MATERIALITY_C || d_peak > MATERIALITY_C || d_under > MATERIALITY_C) {
            printf("FAIL: S0 (NULL_SLOW, pinned SEP?=no) separated arms above materiality -- "
                   "the harness itself is broken, per the table's own sep_reason.\n");
            any_fail = true;
        } else {
            printf("PASS: S0 arms agree within materiality (%.1f degC)\n", (double)MATERIALITY_C);
        }
    }

    /* (c) S3 vs S1, per arm. */
    {
        int s1 = 1, s3 = 2; /* table order: S0, S1, S3 */
        float rate13 = table[s1].ramp_rate_c_per_hr / 3600.0f; /* S1 and S3 share the same commanded rate */
        bool ok = true;
        for (int ai = 0; ai < SIM_ARM_COUNT; ai++) {
            float d_lag = diff_or_zero(all_results[s1][ai].have_lag, all_results[s1][ai].lag_s,
                                       all_results[s3][ai].have_lag, all_results[s3][ai].lag_s) * rate13;
            float d_steady = diff_or_zero(all_results[s1][ai].have_steady, all_results[s1][ai].steady_rms_c,
                                          all_results[s3][ai].have_steady, all_results[s3][ai].steady_rms_c);
            float d_peak = diff_or_zero(all_results[s1][ai].have_entry_peak, all_results[s1][ai].entry_peak_c,
                                        all_results[s3][ai].have_entry_peak, all_results[s3][ai].entry_peak_c);
            float d_under = diff_or_zero(all_results[s1][ai].have_entry_under, all_results[s1][ai].entry_undershoot_c,
                                         all_results[s3][ai].have_entry_under, all_results[s3][ai].entry_undershoot_c);
            printf("S1 vs S3, %s: d_lag_equiv_c=%.4f d_steady=%.4f d_peak=%.4f d_under=%.4f\n",
                   SIM_ARM_NAMES[ai], (double)d_lag, (double)d_steady, (double)d_peak, (double)d_under);
            if (d_lag > MATERIALITY_C || d_steady > MATERIALITY_C || d_peak > MATERIALITY_C || d_under > MATERIALITY_C) {
                ok = false;
            }
        }
        if (!ok) {
            printf("FAIL: S3 (three-node, sensor_bias_p=0) did not match S1 (legacy) within materiality "
                   "on some arm -- the three-node code path is itself a confound; investigate the "
                   "singular-perturbation fixture constants in sim_scenario_table.c before trusting S2/S4-S12.\n");
            any_fail = true;
        } else {
            printf("PASS: S3 matches S1 within materiality on all arms -- the three-node model at "
                   "sensor_bias_p=0 is not itself a confound.\n");
        }
    }

    if (any_fail) {
        printf("\n=== sim_scenarios: FAIL ===\n");
        return 1;
    }
    printf("\n=== sim_scenarios: PASS (WI-4 scope: S0/S1/S3, all six arms; see file header for what is\n"
           "    NOT yet real -- WI5_PENDING/WI8_PENDING rows, single settle band) ===\n");
    return 0;
}
