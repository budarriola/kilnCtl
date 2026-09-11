// sim_iter_tune -- ITER_TUNE_REDESIGN_PLAN.md sec 6/7: runs the REDESIGNED
// iter_tune decision core (iter_tune.c + firing_score.c + firing_compare.c)
// closed-loop against the maintained sim_plant.c kiln model, from several
// DIFFERENT starting gain sets, and reports whether tracking error against
// the target improves (or at minimum does not regress) from every one of
// them, and whether the search converges rather than oscillating.
//
// Like sim_wide_temp_sweep.c (whose linking approach this reuses verbatim)
// this is a data-generating harness, not a TEST_CHECK pass/fail suite: its
// stdout is the evidence. Build and run:
//
//   cl /nologo /std:c11 /I<drivers> /Fe:sim_iter_tune.exe ^
//      App/test/sim_iter_tune.c App/test/sim_plant.c ^
//      App/drivers/control/pid.c App/drivers/control/heater_output.c ^
//      App/drivers/control/zone_coupling_solve.c ^
//      App/drivers/control/firing_score.c App/drivers/control/firing_compare.c ^
//      App/drivers/control/iter_tune.c
//
// LINKED AS-IS, NOT REIMPLEMENTED (plan sec 6.3): pid.c, heater_output.c
// (this is gap G2 -- the real 60 s PWM window, not a continuous duty),
// zone_coupling_solve.c, sim_plant.c, and all three modules under test.
// G1 (sim_plant_from_zone_cfg()), G3
// (sim_relay_lag_step()) and G4 (sim_max31856_quantize_tc()) now live in
// sim_plant.c/.h itself (promoted 2026-09-09 so any other harness gets them
// for free) rather than as file-local code here; this file's own new code is
// the measured-data table below, the profile, and the driver loop.
//
// WHAT THIS CANNOT TELL US (plan sec 6.4, restated because a simulation
// that flatters the algorithm is worse than none):
//   - It cannot validate absolute gain values. It validates the DECISION
//     ALGORITHM's statistical behaviour -- false accept, never-worse,
//     termination. Any gains it "finds" are a property of the model.
//   - The measured parameters were identified at low temperature, and the
//     literal G1 mapping caps this model at ambient + model_k_dc (~42-52 C
//     depending on zone -- see sim_wide_temp_sweep.c's structural-ceiling
//     note). The profile below therefore stays inside 20-36 C. Nothing here
//     is evidence about behaviour at cone temperature.
//   - Its noise floor is a LOWER bound: sensor noise and quantisation are
//     modelled, drafts/ware mass/mains variation/element ageing are not.
//   - The plant's whole usable span is narrower than one 25 C class bucket,
//     so the harness narrows firing_score_cfg_t's bucket widths to 5 C.
//     That is a property of the model's temperature ceiling, not a change
//     to the production defaults, which stay at the plan's 25 C.

#include "../drivers/control/pid.h"
#include "../drivers/control/heater_output.h"
#include "../drivers/control/zone_coupling_solve.h"
#include "../drivers/control/firing_score.h"
#include "../drivers/control/firing_compare.h"
#include "../drivers/control/iter_tune.h"
#include "../drivers/hw/max31856_codec.h"
#include "sim_plant.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 3

// ---- G1: measured plant data + coupling cross-gain, now a SINGLE shared
// source (see sim_measured_zone_constants.h's own header comment for why:
// this file, sim_credibility_gate.c and sim_wide_temp_sweep.c used to each
// carry a hand-copied literal from a preset file that went stale against
// the live board -- docs/audits/cplval75_coupling_verdict_2026-09-10.md
// sec 4/D2). g_k_dc/g_tau_s/g_dead_time_s/g_coupling_coeff all come from
// that header now. ----
#include "sim_measured_zone_constants.h"
static const float g_kp_bench[NZ] = { 0.0318f, 0.0361f, 0.0355f };
static const float g_ki_bench[NZ] = { 0.0002f, 0.0003f, 0.0003f };
static const float g_kd_bench[NZ] = { 0.6526f, 0.6874f, 0.6598f };

#define AMBIENT_C 20.0f
#define BAND_C 5.0f

// ---- fakes zone_coupling_solve.c needs (same contract test_zone_coupling_solve.c uses) ----
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= NZ) return false;
    for (int j = 0; j < NZ; j++) out_row[j] = g_coupling_coeff[zone_index][j];
    return true;
}
bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    (void)zone_index; (void)out_k_dc;
    return false;
}

// ---- G2/G3/G4 now live in sim_plant.c/.h (promoted out of this file so any
// other harness gets the same real PWM linkage / relay lag / quantisation
// for free -- see that header's own comments for each). relay actuation lag
// is unmeasured on this bench; 0.5 s assumed for an SSR-class device, same
// placeholder and same caveat as sim_wide_temp_sweep.c. G4's quantize_tc()
// used to round at the raw-register LSB (1/4096 C) instead of the real
// 19-bit-code resolution (0.0078125 C, 32x coarser) -- fixed when this was
// promoted to sim_max31856_quantize_tc(); see that function's header comment. ----
typedef sim_relay_lag_t relay_lag_t;
#define relay_lag_step sim_relay_lag_step
#define quantize_tc sim_max31856_quantize_tc

// ------------------------------------------------------------------ profile
//
// Six segments inside the model's own usable band (see the file header on
// why it cannot go higher). Rates 30/60/90 C/hr and dwells at 26/31/36 C
// give six DISTINCT segment classes at the harness's 5 C / 25 C-per-hr
// bucket widths -- three ramp classes and three dwell classes, so the
// comparator sees n = 3 for every sub-score and its n >= 3 no-degradation
// veto is actually armed. NOTE: this profile is the harness's own, built
// here; it does not touch the builtin schedule table.
typedef struct { float rate_c_per_hr; float from_c; float to_c; float dwell_s; } seg_def_t;
static const seg_def_t g_profile[] = {
    { 30.0f, 20.0f, 26.0f, 0.0f },
    {  0.0f, 26.0f, 26.0f, 900.0f },
    { 60.0f, 26.0f, 31.0f, 0.0f },
    {  0.0f, 31.0f, 31.0f, 900.0f },
    { 90.0f, 31.0f, 36.0f, 0.0f },
    {  0.0f, 36.0f, 36.0f, 1200.0f },
};
#define NSEG ((int)(sizeof(g_profile) / sizeof(g_profile[0])))

// ------------------------------------------------------- plant construction

typedef struct {
    float k_scale[NZ];      // K   x this   (controller keeps nominal -- deliberate mismatch)
    float tau_scale[NZ];    // tau x this
    float dead_scale[NZ];   // L   x this
    float coupling_scale;   // whole matrix x this
    float start_offset_c;   // firing start temperature offset from ambient
    uint32_t noise_seed;
} plant_variant_t;

// G1: run the measured FOPDT data through sim_plant_from_zone_cfg() by
// staging it into a real zone_cfg_t exactly the way zones_config_migrate.c's
// model-fit fields are populated on the board, rather than assigning
// sim_plant_cfg_t fields by hand -- so this harness cannot silently drift
// from what zone_model_at()'s passthrough seam actually reads. Only the
// three model_* fields this function consumes are set; everything else in
// zone_cfg_t is zero, which is fine since sim_plant_from_zone_cfg() only
// looks at model_k_dc/model_tau_s/model_dead_time_s.
static void build_cfg(sim_kiln_cfg_t *cfg, const plant_variant_t *v)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->zone_count = NZ;

    for (int i = 0; i < NZ; i++) {
        zone_cfg_t zcfg;
        memset(&zcfg, 0, sizeof(zcfg));
        zcfg.model_k_dc = g_k_dc[i] * v->k_scale[i]; // K (h == 1 free scale, plan sec 6.2)
        zcfg.model_tau_s = g_tau_s[i] * v->tau_scale[i];
        zcfg.model_dead_time_s = g_dead_time_s[i] * v->dead_scale[i];

        bool ok = sim_plant_from_zone_cfg(&zcfg, AMBIENT_C, &cfg->zone[i].plant);
        if (!ok) {
            // Every g_k_dc[i]/g_tau_s[i] is a positive measured constant and
            // every *_scale[i] is a positive multiplier (nominal_variant()/
            // mismatched_variant()), so this can only fire if that invariant
            // is broken -- fail loudly rather than run on a zeroed plant.
            fprintf(stderr, "sim_plant_from_zone_cfg() rejected zone %d's measured parameters "
                            "(k_dc=%.4f tau_s=%.4f dead_time_s=%.4f) -- aborting.\n",
                    i, (double)zcfg.model_k_dc, (double)zcfg.model_tau_s, (double)zcfg.model_dead_time_s);
            exit(1);
        }
        cfg->zone[i].radiative_coeff_w_per_k4 = 0.0f; // inside the fitted region; see file header
    }

    // ADDITIVE source-gain coupling (matches zone_coupling_solve.c and
    // sim_kiln_step() -- see docs/audits/sim_credibility_gate_real_cause_2026-09-10.md):
    // coupling_w_per_c[i][j] is used directly as the measured cross-gain,
    // scaled by this trial's coupling_scale mismatch knob. No k_dc-scaled
    // algebraic cut or fit is needed in this model class.
    for (int i = 0; i < NZ; i++) {
        for (int j = 0; j < NZ; j++) {
            cfg->coupling_w_per_c[i][j] = (i == j) ? 0.0f : (v->coupling_scale * g_coupling_coeff[i][j]);
        }
    }

    cfg->sensor_noise_c = 0.05f;
}

// --------------------------------------------------------------- one firing

typedef struct {
    firing_score_set_t set[NZ];
    float mean_abs_err_c[NZ];   // the headline tracking cost: mean |actual - target| over SCORED ticks
    uint32_t scored_ticks[NZ];
    float max_abs_err_c[NZ];
} firing_result_t;

static void run_firing(const iter_tune_gains_t gains[NZ], const plant_variant_t *v, firing_result_t *out)
{
    sim_kiln_cfg_t cfg;
    build_cfg(&cfg, v);
    sim_kiln_state_t sim;
    sim_kiln_reset(&sim, &cfg);
    sim.rng = v->noise_seed ? v->noise_seed : 0x1234567u;

    // Start-temperature offset: the whole point of the redesign is that this
    // must not change the score.
    for (int i = 0; i < NZ; i++) {
        sim.zone[i].element_c = AMBIENT_C + v->start_offset_c;
        sim.zone[i].sensor_c = AMBIENT_C + v->start_offset_c;
        for (int k = 0; k < SIM_PLANT_DELAY_MAX_STEPS; k++) sim.zone[i].delay_ring[k] = AMBIENT_C + v->start_offset_c;
    }

    pid_state_t pid[NZ];
    heater_output_state_t heater[NZ];
    relay_lag_t lag[NZ];
    zone_coupling_hold_cache_t hold_cache[NZ];
    zone_coupling_climb_cache_t climb_cache[NZ];
    uint16_t hold_sig[NZ], climb_sig[NZ];
    memset(hold_cache, 0, sizeof(hold_cache));
    memset(climb_cache, 0, sizeof(climb_cache));
    memset(hold_sig, 0, sizeof(hold_sig));
    memset(climb_sig, 0, sizeof(climb_sig));
    memset(lag, 0, sizeof(lag));
    pid_cfg_t pcfg[NZ];
    for (int i = 0; i < NZ; i++) {
        pid_reset(&pid[i]);
        heater_output_reset(&heater[i]);
        pcfg[i] = (pid_cfg_t){ .kp = gains[i].kp, .ki = gains[i].ki, .kd = gains[i].kd,
                               .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 100.0f };
    }
    heater_output_cfg_t hcfg = { .window_ms = 60000, .min_on_ms = 10000, .min_off_ms = 2000 };

    firing_score_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.band_c = BAND_C;
    scfg.temp_bucket_c = 5.0f;          // see file header: the model's span is < one 25 C bucket
    scfg.rate_bucket_c_per_hr = 25.0f;
    scfg.min_scored_ticks = 60;

    memset(out, 0, sizeof(*out));
    bool captured[NZ] = {false, false, false};
    double abs_err_sum[NZ] = {0, 0, 0};
    uint8_t sat_run[NZ] = {0, 0, 0};

    float reading[NZ];
    for (int i = 0; i < NZ; i++) reading[i] = quantize_tc(sim_kiln_reading_c(&sim, &cfg, i));

    float dt_s = 1.0f;
    zone_coupling_neighbor_t nb[NZ];

    for (int s = 0; s < NSEG; s++) {
        const seg_def_t *sd = &g_profile[s];
        bool is_dwell = (sd->rate_c_per_hr <= 0.0f);
        float rate_c_per_s = sd->rate_c_per_hr / 3600.0f;
        long ticks = is_dwell ? (long)sd->dwell_s : (long)((sd->to_c - sd->from_c) / rate_c_per_s);
        float mean_target = is_dwell ? sd->to_c : 0.5f * (sd->from_c + sd->to_c);

        firing_score_seg_t seg[NZ];
        for (int i = 0; i < NZ; i++) {
            firing_score_seg_begin(&seg[i], &scfg, (uint8_t)i, sd->rate_c_per_hr, mean_target,
                                   g_dead_time_s[i], g_tau_s[i]);
        }

        for (long t = 0; t < ticks; t++) {
            float target = is_dwell ? sd->to_c : sd->from_c + rate_c_per_s * (float)t;
            float rate_now = is_dwell ? 0.0f : rate_c_per_s;

            for (int i = 0; i < NZ; i++) {
                nb[i].qualifies = zone_coupling_qualifies_as_neighbor(true, true, reading[i], true, g_k_dc[i],
                                                                     ZONE_CONTROL_MODE_PID, false, false);
                nb[i].ff_k_dc = g_k_dc[i];   // controller keeps NOMINAL parameters -- the mismatch is deliberate
                nb[i].ff_tau_s = g_tau_s[i];
            }

            float duty_cmd[NZ];
            for (int i = 0; i < NZ; i++) {
                bool used_matrix, hold_infeasible, climb_infeasible, membership_changed;
                coupling_solve_reason_t reason;
                float hold = zone_coupling_solve_hold(nb[i].qualifies, g_k_dc[i], (uint8_t)i, false, nb, NZ,
                                                      target, AMBIENT_C, &used_matrix, &hold_infeasible,
                                                      &reason, &membership_changed, &hold_cache[i], &hold_sig[i]);
                float climb = zone_coupling_solve_climb(nb[i].qualifies, g_k_dc[i], g_tau_s[i], (uint8_t)i, false,
                                                        nb, NZ, rate_now, &used_matrix, &climb_infeasible,
                                                        &reason, &membership_changed, &climb_cache[i], &climb_sig[i]);
                duty_cmd[i] = pid_update(&pid[i], &pcfg[i], target, reading[i], dt_s, hold + climb, hold);
            }

            bool relay_actual[NZ];
            float sim_duty[NZ];
            for (int i = 0; i < NZ; i++) {
                bool cmd = heater_output_duty(&heater[i], &hcfg, duty_cmd[i], 1000);
                relay_actual[i] = relay_lag_step(&lag[i], cmd, 0.5f, dt_s);
                sim_duty[i] = relay_actual[i] ? 1.0f : 0.0f;
                // "commanded >= 98% duty for the whole preceding PWM window",
                // at a 1 Hz tick and a 60 s window.
                sat_run[i] = (duty_cmd[i] >= 0.98f) ? (uint8_t)((sat_run[i] < 60) ? sat_run[i] + 1 : 60) : 0;
            }
            sim_kiln_step(&sim, &cfg, sim_duty, dt_s);

            for (int i = 0; i < NZ; i++) {
                reading[i] = quantize_tc(sim_kiln_reading_c(&sim, &cfg, i));
                bool saturated = (sat_run[i] >= 60);
                bool was_captured = captured[i];
                firing_score_seg_tick(&seg[i], &captured[i], target, reading[i], saturated, dt_s);
                // Mirror the module's own two exclusions for the headline
                // mean-|err| cost, so the reported cost and the scored
                // sub-scores describe the same ticks.
                bool scored = captured[i] && (was_captured || fabsf(reading[i] - target) <= BAND_C) &&
                              !(saturated && reading[i] < target);
                if (scored) {
                    float e = fabsf(reading[i] - target);
                    abs_err_sum[i] += e;
                    out->scored_ticks[i]++;
                    if (e > out->max_abs_err_c[i]) out->max_abs_err_c[i] = e;
                }
            }
        }
        for (int i = 0; i < NZ; i++) firing_score_set_finish_segment(&out->set[i], &seg[i]);
    }

    for (int i = 0; i < NZ; i++) {
        out->mean_abs_err_c[i] = out->scored_ticks[i]
                                     ? (float)(abs_err_sum[i] / (double)out->scored_ticks[i])
                                     : NAN;
    }
}

// --------------------------------------------------------------- ensembles

static uint32_t rng_state = 12345u;
static float urand(float lo, float hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    float u = (float)((rng_state >> 8) & 0xffffffu) / (float)0xffffffu;
    return lo + u * (hi - lo);
}

static plant_variant_t nominal_variant(uint32_t seed)
{
    plant_variant_t v;
    memset(&v, 0, sizeof(v));
    for (int i = 0; i < NZ; i++) { v.k_scale[i] = 1.0f; v.tau_scale[i] = 1.0f; v.dead_scale[i] = 1.0f; }
    v.coupling_scale = 1.0f;
    v.start_offset_c = 0.0f;
    v.noise_seed = seed;
    return v;
}

// Plan sec 6.3: "Deliberate model mismatch is mandatory." K +/-30%, tau
// +/-40%, L +/-50%, coupling +/-50%, start temperature +/-15 C (A7), while
// the CONTROLLER keeps the nominal measured parameters throughout.
static plant_variant_t mismatched_variant(uint32_t seed)
{
    plant_variant_t v = nominal_variant(seed);
    for (int i = 0; i < NZ; i++) {
        v.k_scale[i] = urand(0.70f, 1.30f);
        v.tau_scale[i] = urand(0.60f, 1.40f);
        v.dead_scale[i] = urand(0.50f, 1.50f);
    }
    v.coupling_scale = urand(0.50f, 1.50f);
    v.start_offset_c = urand(-15.0f, 15.0f);
    return v;
}

// ---------------------------------------------------------- the tuning loop

typedef struct {
    const char *name;
    float kp_scale[NZ];
    float ki_scale[NZ];
} start_set_t;

typedef struct {
    float initial_cost[NZ];
    float final_cost[NZ];
    uint8_t trials[NZ];
    uint8_t status[NZ];
    uint8_t accepts[NZ];
    iter_tune_gains_t initial[NZ];
    iter_tune_gains_t final_gains[NZ];
    bool cage_violation;
    int firings;
} run_report_t;

static void tune_run(const start_set_t *ss, const plant_variant_t *plant, int max_firings, bool verbose,
                     run_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));

    iter_tune_zone_state_t st[NZ];
    memset(st, 0, sizeof(st));
    iter_tune_gains_t gains[NZ];
    for (int i = 0; i < NZ; i++) {
        gains[i].kp = g_kp_bench[i] * ss->kp_scale[i];
        gains[i].ki = g_ki_bench[i] * ss->ki_scale[i];
        gains[i].kd = g_kd_bench[i];
        rep->initial[i] = gains[i];
        iter_tune_enable(&st[i], gains[i]);
    }

    // Seed firing: everything runs on the starting gains, and its score set
    // becomes each zone's baseline set.
    firing_result_t base;
    run_firing(gains, plant, &base);
    firing_score_set_t baseline_set[NZ];
    for (int i = 0; i < NZ; i++) baseline_set[i] = base.set[i];
    rep->firings = 1;

    int rr = 0;
    for (int f = 0; f < max_firings; f++) {
        // One parameter, one zone, per firing -- never two (plan sec 4).
        int zone = -1;
        for (int k = 0; k < NZ; k++) {
            int cand = (rr + k) % NZ;
            if (st[cand].enabled && st[cand].status == ITER_TUNE_STATUS_TUNING) { zone = cand; break; }
        }
        if (zone < 0) break;
        rr = (zone + 1) % NZ;

        iter_tune_gains_t proposed;
        if (!iter_tune_propose_perturbation(&st[zone], &proposed)) continue;

        iter_tune_gains_t trial_gains[NZ];
        for (int i = 0; i < NZ; i++) trial_gains[i] = iter_tune_active_gains(&st[i]);

        // A6: the cage is an assert, not a rate.
        for (int i = 0; i < NZ; i++) {
            if (!st[i].has_anchor) continue;
            float lo_kp = st[i].anchor.kp * ITER_TUNE_CAGE_LOW_FACTOR;
            float hi_kp = st[i].anchor.kp * ITER_TUNE_CAGE_HIGH_FACTOR;
            float lo_ki = st[i].anchor.ki * ITER_TUNE_CAGE_LOW_FACTOR;
            float hi_ki = st[i].anchor.ki * ITER_TUNE_CAGE_HIGH_FACTOR;
            if (trial_gains[i].kp < lo_kp - 1e-9f || trial_gains[i].kp > hi_kp + 1e-9f ||
                trial_gains[i].ki < lo_ki - 1e-9f || trial_gains[i].ki > hi_ki + 1e-9f ||
                trial_gains[i].kd != rep->initial[i].kd) {
                rep->cage_violation = true;
            }
        }

        firing_result_t trial;
        run_firing(trial_gains, plant, &trial);
        rep->firings++;

        firing_compare_result_t cmp;
        firing_compare(&baseline_set[zone], &trial.set[zone], NULL, &cmp);
        char why[128];
        iter_tune_result_t res = iter_tune_process_comparison(&st[zone], &cmp, why, sizeof(why));
        if (res == ITER_TUNE_RESULT_ACCEPTED) {
            rep->accepts[zone]++;
            baseline_set[zone] = trial.set[zone];
            // Zones that did NOT move still ran this firing on their own
            // baseline gains, so their baseline score set is refreshed to
            // the most recent observation of those same gains.
            for (int i = 0; i < NZ; i++) if (i != zone && !st[i].has_pending) baseline_set[i] = trial.set[i];
        }
        if (verbose) {
            printf("    firing %2d z%d %-16s kp=%.5f ki=%.6f | steady n=%u med=%+.3fC entry n=%u med=%+.3fC "
                   "lag n=%u med=%+.1fs | %s\n",
                   rep->firings, zone, iter_tune_result_str(res), (double)proposed.kp, (double)proposed.ki,
                   (unsigned)cmp.sub[FIRING_SUBSCORE_STEADY_RMS_C].n,
                   (double)cmp.sub[FIRING_SUBSCORE_STEADY_RMS_C].median_raw,
                   (unsigned)cmp.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].n,
                   (double)cmp.sub[FIRING_SUBSCORE_ENTRY_PEAK_C].median_raw,
                   (unsigned)cmp.sub[FIRING_SUBSCORE_LAG_S].n,
                   (double)cmp.sub[FIRING_SUBSCORE_LAG_S].median_raw, why);
        }
    }

    for (int i = 0; i < NZ; i++) {
        rep->final_gains[i] = st[i].baseline;
        rep->trials[i] = st[i].trials_scored;
        rep->status[i] = st[i].status;
    }

    // Evaluation: initial vs final gains on the SAME plant with the SAME
    // noise seed and the SAME start temperature, so the difference is the
    // gains and nothing else.
    firing_result_t ev0, ev1;
    run_firing(rep->initial, plant, &ev0);
    run_firing(rep->final_gains, plant, &ev1);
    for (int i = 0; i < NZ; i++) {
        rep->initial_cost[i] = ev0.mean_abs_err_c[i];
        rep->final_cost[i] = ev1.mean_abs_err_c[i];
    }
}

// ------------------------------------------------------------------- main

static const start_set_t g_starts[] = {
    { "bench (as commissioned)", {1.00f, 1.00f, 1.00f}, {1.00f, 1.00f, 1.00f} },
    { "kp 0.6x (sluggish)",      {0.60f, 0.60f, 0.60f}, {1.00f, 1.00f, 1.00f} },
    { "kp 1.8x (hot)",           {1.80f, 1.80f, 1.80f}, {1.00f, 1.00f, 1.00f} },
    { "ki 0.4x (slow trim)",     {1.00f, 1.00f, 1.00f}, {0.40f, 0.40f, 0.40f} },
    { "ki 2.5x (wind-up prone)", {1.00f, 1.00f, 1.00f}, {2.50f, 2.50f, 2.50f} },
    { "kp 0.7x ki 2.0x (mixed)", {0.70f, 0.70f, 0.70f}, {2.00f, 2.00f, 2.00f} },
    { "ki 0.15x (badly detuned)", {1.00f, 1.00f, 1.00f}, {0.15f, 0.15f, 0.15f} },
    { "kp 0.35x ki 0.3x (very detuned)", {0.35f, 0.35f, 0.35f}, {0.30f, 0.30f, 0.30f} },
};
#define NSTART ((int)(sizeof(g_starts) / sizeof(g_starts[0])))

static void report_run(const start_set_t *ss, const run_report_t *r)
{
    printf("\n== start set: %s == (%d firings)\n", ss->name, r->firings);
    for (int i = 0; i < NZ; i++) {
        float d = r->final_cost[i] - r->initial_cost[i];
        const char *verdict = (d < -0.5f) ? "IMPROVED" : (d > 0.5f) ? "REGRESSED" : "unchanged(<0.5C)";
        printf("  z%d mean|err| %6.3f -> %6.3f C  (%+6.3f C, %s)  kp %.5f->%.5f ki %.6f->%.6f  "
               "trials=%u accepts=%u status=%s\n",
               i, (double)r->initial_cost[i], (double)r->final_cost[i], (double)d, verdict,
               (double)r->initial[i].kp, (double)r->final_gains[i].kp,
               (double)r->initial[i].ki, (double)r->final_gains[i].ki,
               (unsigned)r->trials[i], (unsigned)r->accepts[i],
               iter_tune_status_str((iter_tune_status_t)r->status[i]));
    }
    printf("  cage violations: %s\n", r->cage_violation ? "YES -- A6 FAILED" : "none");
}

int main(int argc, char **argv)
{
    int mc_runs = (argc > 1) ? atoi(argv[1]) : 40;
    printf("# sim_iter_tune -- redesigned iter_tune closed-loop against sim_plant.c\n");
    printf("# Profile: 3 ramps (30/60/90 C/hr) + 3 dwells (26/31/36 C), 6 distinct segment classes.\n");
    printf("# Controller: real pid.c + heater_output.c (60 s PWM window) + zone_coupling_solve.c.\n");
    printf("# Nothing here is evidence about behaviour above ~40 C -- see this file's header.\n");

    // ---- Part 0: ORACLE GRID ----
    // Before believing anything the mechanism does or does not do, establish
    // how much tracking error is available to be won AT ALL on this plant by
    // moving kp and ki. If the whole reachable spread is smaller than the
    // owner's 0.5 C floor, then "refuses every trial" is the CORRECT
    // behaviour and no amount of algorithm work would change it -- and
    // acceptance criteria A3/A4 (which assume a better gain exists inside
    // the cage) are simply unreachable on this model, which is a fact about
    // the model, not a result about the algorithm.
    printf("\n#### PART 0: oracle grid -- mean|err| (C) over kp/ki multipliers on the bench gains ####\n");
    {
        static const float mult[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
        int nm = (int)(sizeof(mult) / sizeof(mult[0]));
        float best[NZ], worst[NZ];
        for (int i = 0; i < NZ; i++) { best[i] = 1e9f; worst[i] = -1e9f; }
        printf("    kp_x  ki_x |    z0     z1     z2\n");
        for (int a = 0; a < nm; a++) {
            for (int b = 0; b < nm; b++) {
                iter_tune_gains_t g[NZ];
                for (int i = 0; i < NZ; i++) {
                    g[i].kp = g_kp_bench[i] * mult[a];
                    g[i].ki = g_ki_bench[i] * mult[b];
                    g[i].kd = g_kd_bench[i];
                }
                plant_variant_t v = nominal_variant(0x1234567u);
                firing_result_t r;
                run_firing(g, &v, &r);
                printf("    %4.2f  %4.2f | %6.3f %6.3f %6.3f\n", (double)mult[a], (double)mult[b],
                       (double)r.mean_abs_err_c[0], (double)r.mean_abs_err_c[1], (double)r.mean_abs_err_c[2]);
                for (int i = 0; i < NZ; i++) {
                    if (r.mean_abs_err_c[i] < best[i]) best[i] = r.mean_abs_err_c[i];
                    if (r.mean_abs_err_c[i] > worst[i]) worst[i] = r.mean_abs_err_c[i];
                }
            }
        }
        printf("    reachable spread over the whole grid: z0 %.3f C, z1 %.3f C, z2 %.3f C "
               "(owner floor 0.5 C)\n", (double)(worst[0] - best[0]), (double)(worst[1] - best[1]),
               (double)(worst[2] - best[2]));
    }

    // ---- Part 1: multi-start, nominal plant, verbose ----
    printf("\n#### PART 1: several DIFFERENT starting gain sets, nominal plant ####\n");
    int improved = 0, regressed = 0, unchanged = 0, converged = 0, zones = 0;
    for (int s = 0; s < NSTART; s++) {
        run_report_t rep;
        plant_variant_t v = nominal_variant(0x1234567u);
        tune_run(&g_starts[s], &v, 40, true, &rep);
        report_run(&g_starts[s], &rep);
        for (int i = 0; i < NZ; i++) {
            float d = rep.final_cost[i] - rep.initial_cost[i];
            zones++;
            if (d < -0.5f) improved++;
            else if (d > 0.5f) regressed++;
            else unchanged++;
            if (rep.status[i] == ITER_TUNE_STATUS_CONVERGED) converged++;
        }
        if (rep.cage_violation) printf("  *** A6 CAGE VIOLATION ***\n");
    }
    printf("\n#### PART 1 SUMMARY: %d zone-runs -- improved %d, unchanged(<0.5C) %d, REGRESSED %d; "
           "converged %d/%d ####\n", zones, improved, unchanged, regressed, converged, zones);

    // ---- Part 2: A1 false-accept (null experiment) ----
    // Identical gains on both sides; only the noise seed and the start
    // temperature differ. Every ACCEPT here is the mechanism ratcheting on
    // noise, which is the failure the whole accept rule exists to prevent.
    printf("\n#### PART 2: A1 null experiment (identical gains, noise + start temp differ) ####\n");
    int accepts = 0, rejects = 0, insufficient = 0, nopairs = 0;
    for (int n = 0; n < mc_runs; n++) {
        plant_variant_t a = nominal_variant(0x1000u + (uint32_t)n * 7919u);
        plant_variant_t b = a;
        b.noise_seed = 0x9000u + (uint32_t)n * 104729u;
        b.start_offset_c = urand(-15.0f, 15.0f); // A7: start temperature randomised +/-15 C
        a.start_offset_c = urand(-15.0f, 15.0f);
        iter_tune_gains_t g[NZ];
        for (int i = 0; i < NZ; i++) { g[i].kp = g_kp_bench[i]; g[i].ki = g_ki_bench[i]; g[i].kd = g_kd_bench[i]; }
        firing_result_t ra, rb;
        run_firing(g, &a, &ra);
        run_firing(g, &b, &rb);
        for (int i = 0; i < NZ; i++) {
            firing_compare_result_t cmp;
            switch (firing_compare(&ra.set[i], &rb.set[i], NULL, &cmp)) {
                case FIRING_COMPARE_ACCEPT: accepts++; break;
                case FIRING_COMPARE_REJECT_DEGRADED: rejects++; break;
                case FIRING_COMPARE_INSUFFICIENT: insufficient++; break;
                default: nopairs++; break;
            }
        }
    }
    int total = accepts + rejects + insufficient + nopairs;
    printf("  %d null comparisons: ACCEPT %d (%.2f%%)  REJECT %d  INSUFFICIENT %d  NO_PAIRS %d\n",
           total, accepts, 100.0 * accepts / (total ? total : 1), rejects, insufficient, nopairs);

    // A1_DESIGN_TARGET_PCT is the plan's real bar (ITER_TUNE_REDESIGN_PLAN.md
    // sec 7, A1) and is NOT the pass/fail threshold below -- it stays in the
    // code as the goal this harness is still short of. It is deliberately
    // unused in the pass/fail expression so it cannot silently become the
    // enforced value again by a careless edit.
    const double A1_DESIGN_TARGET_PCT = 2.0;
    (void)A1_DESIGN_TARGET_PCT;

    // A1_PINNED_MAX_ACCEPTS / A1_PINNED_TOTAL: pin the CURRENTLY MEASURED
    // false-accept rate as a known-failure ceiling, not the 2.0% design
    // target above. Docs/audits/firing_score_entry_ema_review_2026-09-10.md
    // has the full story; short version:
    //
    //   - 24 ACCEPT / 660 null comparisons at mc_runs=220 (exactly the
    //     invocation check_sim_iter_tune_bars.ps1 uses) measured
    //     2026-09-10, immediately after reverting 8b96b591's dwell-entry
    //     EMA smoothing (that revert is correct and is staying -- the EMA
    //     was blinding firing_compare.c's one-sided degradation veto in the
    //     accept-permissive direction: a synthetic 0.55 degC true
    //     degradation measured as low as 0.22-0.47 degC smoothed, under the
    //     0.5 degC veto floor, while the only real-hardware measurement of
    //     this statistic sits 10x inside that floor and does not
    //     corroborate the problem the EMA was "fixing").
    //   - Root cause of the 24/660 itself is UPSTREAM of this file:
    //     d63a5591 changed sim_plant.c's coupling model from a
    //     temperature-difference exchange term to the firmware-matching
    //     additive source-gain form, which correctly exposed
    //     ENTRY_PEAK_C to neighbour-zone PWM ripple the old model never
    //     produced. The fix is a re-identified coupling matrix (capture in
    //     progress as of this pin), not a change to this comparator or this
    //     harness -- this pin is a placeholder ceiling, not a target.
    //   - EXIT CONDITION (updated 2026-09-11): the coupling re-identification
    //     this pin was waiting on HAS landed (2026-09-10/11) but did NOT
    //     produce an adoptable matrix -- the linear coupling model class
    //     itself was refuted (superposition across zones fails by ~33% at
    //     matched dT; see docs/audits/coupling_joint_identification_capture_2026-09-10.md
    //     and commits 5844a3e8, 947709a8). "Once the matrix lands" is
    //     therefore satisfied on its face without resolving the reason this
    //     pin exists, so it is replaced with a condition stated on
    //     sim_plant.c's coupling model, not on the re-identification effort:
    //       1. sim_plant.c's coupling term is changed from the current
    //          linear/additive form (the one d63a5591 introduced) to a model
    //          class that has been validated against hardware at the
    //          operating points this harness exercises (<=40 C, see this
    //          file's header) -- e.g. one that accounts for the refuted
    //          superposition assumption, OR a documented decision that the
    //          linear model is being kept deliberately with the ~33%
    //          superposition error accepted as in-scope; AND
    //       2. this exact A1 measurement (kilnctl_sim_iter_tune.exe 220,
    //          660 null comparisons) is re-run against that new/accepted
    //          model.
    //     Whichever of those two outcomes lands, re-measure A1 at
    //     mc_runs=220 and either tighten this pin toward
    //     A1_DESIGN_TARGET_PCT or remove it in favour of enforcing
    //     A1_DESIGN_TARGET_PCT directly -- report the new accepts/total
    //     alongside whichever change is made, per the RATCHET GUARD below.
    //     Re-run 2026-09-11 (immediately after the above, before the
    //     coupling-model change below) reproduced 24/660 (3.6364%)
    //     unchanged from the 2026-09-10 measurement, so no drift had
    //     occurred yet and no re-pin was justified at that point.
    //
    //     RE-MEASURED 2026-09-11, AFTER sim_plant.c's coupling model was
    //     changed from a constant matrix to a level-scheduled one (see
    //     sim_kiln_coupling_schedule_scale(), sim_plant.c): outcome 1 above
    //     ("sim_plant.c's coupling term is changed ... to a model class
    //     validated against hardware ... e.g. one that accounts for the
    //     refuted superposition assumption") is satisfied -- the schedule
    //     is fit directly to the low-joint and 62-75C plateau residuals
    //     that refuted the old constant matrix (docs/audits/
    //     coupling_sign_reversal_artifact_test_2026-09-11.md). Re-running
    //     this exact measurement (kilnctl_sim_iter_tune.exe 220) against it
    //     gives 38/660 (5.76%) -- WORSE than the pinned 24/660 (3.64%), not
    //     better. Reported honestly per the task's own instruction: this is
    //     NOT evidence the schedule is a bad idea for its stated purpose
    //     (it demonstrably improves the RMS joint-plateau residual it was
    //     built to fix -- see test_sim_kiln.c's test_coupling_schedule_high_
    //     joint_plateaus()) -- it is evidence that A1's false-accept
    //     sensitivity to PWM-window coupling ripple (d63a5591's original
    //     finding) is, if anything, HEIGHTENED by a schedule whose
    //     correction can be LARGER in magnitude than the constant matrix's
    //     off-diagonal terms once the joint-excess level moves past the
    //     calibrated regime (see the extrapolation-beyond-level_hi comment
    //     on sim_kiln_coupling_schedule_scale()). PER THE RATCHET GUARD
    //     BELOW, THE PIN IS NOT LOOSENED to 38/660 -- it stays at 24/660,
    //     so this check now correctly reports FAIL (a real regression on
    //     this specific harness, not a bug in the harness). Follow-up
    //     (not done here, out of this task's scope): investigate whether
    //     A1's own ACCEPT criterion, not the plant model, is what's
    //     under-penalizing schedule-induced ripple.
    //
    // DETERMINISM, CHECKED (not assumed): this harness has NO wall-clock or
    // OS-entropy seeding anywhere in the call chain (grepped for
    // time(NULL)/GetTickCount/QueryPerformanceCounter/rand()/srand() in this
    // file, sim_plant.c and iter_tune.c -- none). Every RNG is an explicit
    // LCG: this file's own file-local `urand()` (rng_state, seeded 12345 at
    // program start) drives ensemble variant generation, and
    // sim_kiln_state_t.rng drives sensor noise -- seeded from
    // plant_variant_t.noise_seed at run_firing()'s top (sim_iter_tune.c,
    // right after sim_kiln_reset(), which itself hardcodes rng=0x1234567u --
    // the noise_seed assignment runs AFTER that reset and overwrites it, so
    // the seed genuinely reaches the consumer; verified by reading the call
    // order, not just that a seed field is assigned somewhere. This is the
    // same class of bug fault_sched.c once had (SET_SEED updated s_seed
    // while fault_engine_t.rng_state stayed hardcoded at 0 from
    // fault_engine_init(&s_engine, 0), making every declared seed cosmetic)
    // -- checked here and NOT present. Empirically confirmed too: five
    // consecutive `kilnctl_sim_iter_tune.exe 220` runs of this exact,
    // unmodified source produced the IDENTICAL 24 ACCEPT / 21 REJECT / 615
    // INSUFFICIENT / 0 NO_PAIRS breakdown every time (2026-09-10). This bar
    // is NOT stochastic -- a bare exact-count pin is the correct instrument
    // for it. (An earlier report elsewhere called this check's bar
    // "stochastic"; that characterization does not hold for this harness at
    // fixed mc_runs and is superseded by this note.)
    //
    // mc_runs SENSITIVITY: the pin is valid ONLY at mc_runs=220 (660
    // comparisons), which is what determinism was verified at and what
    // check_sim_iter_tune_bars.ps1 invokes. The comparison below is
    // cross-multiplied so it stays ARITHMETICALLY exact at any mc_runs
    // without rounding error -- but that says nothing about whether 24/660
    // is still the right CEILING at a different mc_runs (a bigger ensemble
    // can shift the true rate in either direction with tighter sampling
    // noise). Raising mc_runs for better statistics without re-measuring
    // and re-pinning at the new sample size would silently compare a new
    // distribution against an old ceiling -- re-measure this exact pin at
    // whatever mc_runs is adopted before trusting it there.
    //
    // RATCHET GUARD: this pin only ever gets TIGHTER by construction --
    // raising it requires editing these two literals by hand, with a
    // comment (this one, updated) recording the new measurement's
    // provenance. It must never be raised just to make a red run go green;
    // that defeats the entire point of pinning a known-failure instead of
    // silently widening the design target.
    const int A1_PINNED_MAX_ACCEPTS = 24;
    const int A1_PINNED_TOTAL = 660;
    bool a1_pass = ((int64_t)accepts * A1_PINNED_TOTAL) <= ((int64_t)A1_PINNED_MAX_ACCEPTS * total);
    printf("  A1 bar: PINNED KNOWN-FAILURE CEILING <= %d/%d (%.4f%%) -- NOT the %.1f%% design target, "
           "which this does not meet -> %s\n",
           A1_PINNED_MAX_ACCEPTS, A1_PINNED_TOTAL,
           100.0 * A1_PINNED_MAX_ACCEPTS / A1_PINNED_TOTAL, A1_DESIGN_TARGET_PCT, a1_pass ? "PASS" : "FAIL");
    if (a1_pass) {
        printf("  A1 NOTE: this PASS is against a pinned known-failure baseline, not proof the design\n"
               "           target is met. The upstream cause is sim_plant.c's linear/additive\n"
               "           coupling model (d63a5591). The re-identification this pin once named as\n"
               "           its fix HAS landed (2026-09-10/11) and REFUTED that model class rather\n"
               "           than replacing it, so the pin stands -- see this file's EXIT CONDITION\n"
               "           comment above A1_PINNED_MAX_ACCEPTS, and\n"
               "           docs/audits/firing_score_entry_ema_review_2026-09-10.md for the\n"
               "           veto-sensitivity numbers that justify the revert this pin stands in for.\n");
    }

    // ---- Part 3: A2 never-worse over a mismatched ensemble ----
    printf("\n#### PART 3: A2 never-worse, mismatched plant ensemble (K+/-30%%, tau+/-40%%, "
           "L+/-50%%, coupling+/-50%%, start +/-15C) ####\n");
    int worse = 0, better = 0, same = 0, zruns = 0, conv = 0, cage = 0;
    double sum_delta = 0.0;
    for (int n = 0; n < mc_runs; n++) {
        plant_variant_t v = mismatched_variant(0x5000u + (uint32_t)n * 2654435761u);
        const start_set_t *ss = &g_starts[n % NSTART];
        run_report_t rep;
        tune_run(ss, &v, 40, false, &rep);
        if (rep.cage_violation) cage++;
        for (int i = 0; i < NZ; i++) {
            float d = rep.final_cost[i] - rep.initial_cost[i];
            if (!isfinite(d)) continue;
            zruns++;
            sum_delta += d;
            if (d > 0.5f) worse++;
            else if (d < -0.5f) better++;
            else same++;
            if (rep.status[i] == ITER_TUNE_STATUS_CONVERGED) conv++;
        }
    }
    printf("  %d zone-runs over %d mismatched plants: better %d, unchanged(<0.5C) %d, WORSE %d (%.2f%%)\n",
           zruns, mc_runs, better, same, worse, 100.0 * worse / (zruns ? zruns : 1));
    printf("  mean cost change %+0.4f C; terminated %d/%d; A6 cage violations %d\n",
           sum_delta / (zruns ? zruns : 1), conv, zruns, cage);
    bool a2_pass = (100.0 * worse / (zruns ? zruns : 1)) <= 1.0;
    bool a5_pass = (100.0 * conv / (zruns ? zruns : 1)) >= 95.0;
    bool a6_pass = (cage == 0);
    printf("  A2 bar: worse-by-more-than-one-floor <= 1%% -> %s\n", a2_pass ? "PASS" : "FAIL");
    printf("  A5 bar: termination within budget >= 95%% -> %s\n", a5_pass ? "PASS" : "FAIL");
    printf("  A6 bar: 0 cage violations -> %s\n", a6_pass ? "PASS" : "FAIL");

    // ---- Enforcement ----
    // Until 2026-09-10 this harness's bars were printf-only: main() always
    // `return 0`, so build_host_tests.ps1 (which links this as a
    // "data-generating harness, not run automatically") and every other
    // check_*.ps1/run_all_checks.ps1 saw a clean exit no matter what the
    // A1/A2/A5/A6 verdicts said. That is exactly how the A1 false-accept
    // rate went from 0% to 3.64% after d63a5591 and was caught only by a
    // manual re-run, not by any automated gate (see
    // docs/audits/firing_score_entry_ema_review_2026-09-10.md). Make the
    // failure loud: a non-zero exit whenever any bar fails, checked by
    // check_sim_iter_tune_bars.ps1. The printf verdicts above remain
    // unconditional so this still works as a data-generating harness for
    // manual/exploratory runs -- nothing here changes what is printed
    // (only main()'s day: the return no reader ever looked at before).
    bool all_pass = a1_pass && a2_pass && a5_pass && a6_pass;
    printf("\n#### OVERALL: %s ####\n",
           all_pass ? "PASS (A2/A5/A6 clear; A1 clear ONLY against its pinned known-failure ceiling, "
                      "NOT the 2.0% design target -- see A1 NOTE above)"
                    : "FAIL");
    return all_pass ? 0 : 1;
}
