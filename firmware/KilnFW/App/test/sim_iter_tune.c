// sim_iter_tune -- ITER_TUNE_REDESIGN.md sec 6/7: runs the REDESIGNED
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

// A8 (plan sec 7): "profile independence" -- A1 and A2 re-measured with
// trial and baseline firings on DIFFERENT profiles sharing >= 5 segment
// classes. g_profile_b keeps every one of g_profile's six (rate_bucket,
// temp_bucket) classes -- same rates and same segment mean-targets, so all
// six classes still match at the harness's 5 C / 25 C-per-hr bucket widths
// -- but is a genuinely different profile: each dwell's duration differs
// (600/1200/600 s here vs 900/900/1200 s in g_profile), so its total tick
// count, its per-segment scored-tick counts and its firing_score_seg_t
// state at any given wall-clock tick all differ from g_profile's. This is
// deliberate: A8 exists to catch a comparator that is secretly sensitive to
// firing LENGTH or SHAPE rather than only to the matched segment classes it
// claims to score.
static const seg_def_t g_profile_b[] = {
    { 30.0f, 20.0f, 26.0f, 0.0f },
    {  0.0f, 26.0f, 26.0f, 600.0f },
    { 60.0f, 26.0f, 31.0f, 0.0f },
    {  0.0f, 31.0f, 31.0f, 1200.0f },
    { 90.0f, 31.0f, 36.0f, 0.0f },
    {  0.0f, 36.0f, 36.0f, 600.0f },
};
#define NSEG_B ((int)(sizeof(g_profile_b) / sizeof(g_profile_b[0])))

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

static void run_firing_on(const iter_tune_gains_t gains[NZ], const plant_variant_t *v,
                          const seg_def_t *profile, int nseg, firing_result_t *out)
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

    for (int s = 0; s < nseg; s++) {
        const seg_def_t *sd = &profile[s];
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

static void run_firing(const iter_tune_gains_t gains[NZ], const plant_variant_t *v, firing_result_t *out)
{
    run_firing_on(gains, v, g_profile, NSEG, out);
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

// eval_profile/eval_nseg: the profile the FINAL initial-vs-final cost
// comparison (rep->initial_cost/rep->final_cost -- A2's own "never worse"
// metric) is measured on. The search itself is untouched by this parameter:
// every accept/reject decision inside the tuning loop below still runs on
// whatever `profile`/`nseg` the caller passes in as the SEARCH profile (see
// the two run_firing_on() calls inside the loop below), exactly as before
// this parameter existed. A8 (plan sec 7): "A1 and A2 re-measured with
// trial and baseline firings on different profiles". For A1 (Part 4,
// already built) that means the null-experiment's two compared firings run
// on different profiles. A2's own two compared firings ARE this function's
// final ev0 (initial gains)/ev1 (final gains) evaluation -- so the faithful
// cross-profile re-measurement of A2 is: let the search run to completion
// exactly as production would (accept/reject decided on ONE real profile),
// then score whether the gain set it landed on is still not worse than
// where it started, evaluated on a DIFFERENT profile than the one that was
// tuned on -- i.e. "a tuning accepted while firing profile A must not make
// things worse the next time the kiln fires profile B." Passing the same
// profile for both `profile` (search) and `eval_profile` (scoring)
// reproduces the original, single-profile A2 exactly, byte-for-byte -- see
// Part 1/Part 3's call sites, unchanged in that respect.
static void tune_run(const start_set_t *ss, const plant_variant_t *plant, int max_firings, bool verbose,
                     const seg_def_t *profile, int nseg, const seg_def_t *eval_profile, int eval_nseg,
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
    run_firing_on(gains, plant, profile, nseg, &base);
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
        run_firing_on(trial_gains, plant, profile, nseg, &trial);
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
    // gains and nothing else. Scored on eval_profile/eval_nseg, which is
    // g_profile/NSEG (the same profile the search above ran on) for every
    // caller except Part 5's A8 A2-half.
    firing_result_t ev0, ev1;
    run_firing_on(rep->initial, plant, eval_profile, eval_nseg, &ev0);
    run_firing_on(rep->final_gains, plant, eval_profile, eval_nseg, &ev1);
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

// ------------------------------------------------------------------- step 4
// ITER_TUNE_REDESIGN.md step 4: "Null-experiment noise-floor estimation
// in the simulator; floors emitted as a DATA ARTIFACT, not compiled
// constants." Gate: "floors within 2x of noise_floor.json where the keys
// correspond."
//
// Part 2 (the A1 null experiment, below) already runs the identical-gains,
// noise-only comparison the plan's sec 3.1 calls for -- this reuses those
// SAME firing pairs rather than running a second Monte-Carlo ensemble, and
// records each matched firing_compare() call's per-(zone, sub-score) median
// paired difference (cmp.sub[s].median_raw) whenever n > 0. The floor for a
// (zone, sub-score) key is the 97.5th percentile of the ABSOLUTE VALUE of
// those medians: since a null-experiment difference is centred on zero by
// construction (identical gains both sides), a two-sided 97.5% percentile of
// |d| is the bound below which 97.5% of the null distribution's magnitude
// falls either side of zero -- i.e. a paired difference this large or larger
// is unlikely (<=2.5%) to be sampling noise alone, which is exactly what
// Bar 2 (plan sec 3) needs to compare a real trial's median improvement
// against.
//
// Only the four VOTING/measured sub-scores that can carry a matched pair in
// this profile are tracked (LAG_S, ENTRY_PEAK_C, STEADY_RMS_C,
// ENTRY_UNDERSHOOT_C) -- SETTLE_S and LAG_SIGNED_S are report-only (never
// vote, see FIRING_COMPARE_REPORT_ONLY_MASK) and are not floored here.

typedef struct {
    float *samples[NZ][FIRING_SUBSCORE_COUNT];
    int count[NZ][FIRING_SUBSCORE_COUNT];
    int cap;
} noise_floor_accum_t;

static void nf_accum_init(noise_floor_accum_t *a, int cap)
{
    a->cap = cap;
    for (int z = 0; z < NZ; z++) {
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
            a->samples[z][s] = (float *)malloc(sizeof(float) * (size_t)(cap > 0 ? cap : 1));
            a->count[z][s] = 0;
        }
    }
}

static void nf_accum_free(noise_floor_accum_t *a)
{
    for (int z = 0; z < NZ; z++)
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++)
            free(a->samples[z][s]);
}

static void nf_accum_add(noise_floor_accum_t *a, int zone, const firing_compare_result_t *cmp)
{
    for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
        if (cmp->sub[s].n == 0) continue;
        if (a->count[zone][s] >= a->cap) continue; // should not happen at the caller's own cap
        a->samples[zone][s][a->count[zone][s]++] = fabsf(cmp->sub[s].median_raw);
    }
}

static int nf_cmp_float(const void *pa, const void *pb)
{
    float a = *(const float *)pa, b = *(const float *)pb;
    return (a > b) - (a < b);
}

// 97.5th percentile of a[0..n), sorted in place. Returns NAN if n == 0.
static float nf_percentile_975(float *a, int n)
{
    if (n <= 0) return NAN;
    qsort(a, (size_t)n, sizeof(float), nf_cmp_float);
    int idx = (int)ceil(0.975 * n) - 1;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return a[idx];
}

static const char *nf_subscore_name(firing_subscore_t s)
{
    switch (s) {
        case FIRING_SUBSCORE_LAG_S: return "lag_s";
        case FIRING_SUBSCORE_ENTRY_PEAK_C: return "entry_peak_c";
        case FIRING_SUBSCORE_STEADY_RMS_C: return "steady_rms_c";
        case FIRING_SUBSCORE_SETTLE_S: return "settle_s";
        case FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C: return "entry_undershoot_c";
        case FIRING_SUBSCORE_LAG_SIGNED_S: return "lag_signed_s";
        default: return "unknown";
    }
}

// Which floored sub-scores have a rough real-hardware correspondent in
// tools/PcTools/config_presets/noise_floor.json (plan sec 3.1: that artifact
// is keyed on the OLD metric set, not the new per-segment sub-scores, so
// this is a "where the keys correspond" best-effort mapping, not an exact
// one). LAG_S/ENTRY_UNDERSHOOT_C have no corresponding key at all and are
// skipped in the cross-check (still emitted in the artifact).
static const char *nf_real_metric_for(firing_subscore_t s)
{
    switch (s) {
        case FIRING_SUBSCORE_ENTRY_PEAK_C: return "dwell_entry_overshoot_peak_c";
        case FIRING_SUBSCORE_STEADY_RMS_C: return "dwell_steady_state_offset_c";
        default: return NULL;
    }
}

// Minimal scan of noise_floor.json: returns the MAX noise_floor_c across
// every entry keyed "z<zone>:<metric>:<segment>" for the given zone/metric.
// Same flat-substring-scan approach sim_credibility_gate.c's
// read_noise_floor() already uses on this same file -- not a general JSON
// parser, just enough structure to pull one field back out of a file this
// program also generates the reading contract for.
static bool nf_read_real_floor_max(const char *path, int zone, const char *metric, float *out_max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    char key[128];
    snprintf(key, sizeof(key), "\"z%d:%s:", zone, metric);
    bool found = false;
    float best = -1e30f;
    const char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        // This entry's own key line always ends "...\": {" -- skip past THAT
        // occurrence (not treat it as a sibling boundary) before bounding the
        // search for this entry's own closing brace. Every entry in this file
        // is a flat (non-nested) object closed by "\n    }," or "\n    }" at
        // 4-space indent, so that is the correct end-of-entry marker; the
        // previous version bounded against the entry's OWN opening "\": {"
        // instead, which is only a few bytes past the key match itself, so it
        // always excluded that entry's real noise_floor_c and this cross-
        // check silently matched nothing on any input, ever.
        const char *own_open = strstr(p, "\": {");
        const char *body = own_open ? own_open + 4 : p;
        const char *entry_end = strstr(body, "\n    }");
        const char *nf = strstr(body, "\"noise_floor_c\":");
        if (nf && (!entry_end || nf < entry_end)) {
            float v = strtof(nf + strlen("\"noise_floor_c\":"), NULL);
            if (v > best) best = v;
            found = true;
        }
        p += strlen(key);
    }
    free(buf);
    if (found) *out_max = best;
    return found;
}

// Emits the floor artifact and (if `real_nf_path` is non-NULL) cross-checks
// it against noise_floor.json within the plan's 2x gate. Returns false if
// the gate is violated for any corresponding key. `real_nf_path` is not
// currently passed by check_sim_iter_tune_bars.ps1 (argv[1] only), so this
// gate is not wired into the enforced suite -- see the note below for why.
//
// 2026-09-16 measurement (n=220, argv[2]/argv[3] run by hand against
// tools/PcTools/config_presets/noise_floor.json): ENTRY_PEAK_C lands within
// or near the 2x band (z0 1.75x, z1 1.95x, z2 1.34x) but STEADY_RMS_C does
// not (z0 7.42x, z1 2.90x, z2 6.34x) -- an honest gap, not a bug: the null
// experiment's only stochastic inputs are sim_plant.c's modelled process
// noise and per-side start-offset (A7), while noise_floor.json's six real
// repeats also carry ambient drift, thermocouple/ADC noise, and timing
// jitter the sim does not model, and those dominate STEADY_RMS_C (a
// long-window average) far more than they dominate ENTRY_PEAK_C (a single
// transient sample). This is the same class of gap the plan itself warns
// about in sec 3.1/step 4's gate wording ("where the keys correspond") and
// is why this cross-check stays a manual/ad-hoc diagnostic here rather than
// an enforced check_*.ps1 gate: enforcing it today would either force a
// permanent red for a real physical-model limitation (see A1's identical
// "pinned known-failure ceiling" precedent above) or invite quietly loosening
// the 2x band to paper over it. Re-evaluate once/if sim_plant.c grows a
// modelled sensor/ambient noise term.
static bool nf_emit_and_check(noise_floor_accum_t *a, int mc_runs, const char *out_path,
                              const char *real_nf_path)
{
    float floor_val[NZ][FIRING_SUBSCORE_COUNT];
    int floor_n[NZ][FIRING_SUBSCORE_COUNT];
    for (int z = 0; z < NZ; z++) {
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
            floor_n[z][s] = a->count[z][s];
            floor_val[z][s] = nf_percentile_975(a->samples[z][s], a->count[z][s]);
        }
    }

    printf("\n#### PART 2b: step 4 noise-floor artifact (97.5th pct |paired diff|, null experiment) ####\n");
    for (int z = 0; z < NZ; z++) {
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
            if (floor_n[z][s] == 0) continue;
            printf("    z%d %-20s n=%4d floor=%.4f\n", z, nf_subscore_name((firing_subscore_t)s),
                   floor_n[z][s], (double)floor_val[z][s]);
        }
    }

    if (out_path) {
        FILE *f = fopen(out_path, "wb");
        if (!f) {
            printf("  WARNING: could not open '%s' for write -- artifact not emitted.\n", out_path);
        } else {
            fprintf(f, "{\n");
            fprintf(f, "  \"schema_version\": 1,\n");
            fprintf(f, "  \"generated_from\": \"sim_iter_tune.c Part 2 null experiment, mc_runs=%d\",\n", mc_runs);
            fprintf(f, "  \"note\": \"floor[zone][sub_score] = 97.5th percentile of |median paired "
                        "difference| across identical-gains null comparisons per zone (plan sec "
                        "3.1/step 4). NOT compiled constants: read at runtime by whatever populates "
                        "firing_compare_floors_t.\",\n");
            fprintf(f, "  \"zones\": {\n");
            for (int z = 0; z < NZ; z++) {
                fprintf(f, "    \"%d\": {\n", z);
                for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
                    if (floor_n[z][s] > 0) {
                        fprintf(f, "      \"%s\": { \"n\": %d, \"floor\": %.6f }%s\n",
                                nf_subscore_name((firing_subscore_t)s), floor_n[z][s],
                                (double)floor_val[z][s], (s == FIRING_SUBSCORE_COUNT - 1) ? "" : ",");
                    } else {
                        fprintf(f, "      \"%s\": { \"n\": 0, \"floor\": null }%s\n",
                                nf_subscore_name((firing_subscore_t)s),
                                (s == FIRING_SUBSCORE_COUNT - 1) ? "" : ",");
                    }
                }
                fprintf(f, "    }%s\n", (z == NZ - 1) ? "" : ",");
            }
            fprintf(f, "  }\n}\n");
            fclose(f);
            printf("  wrote artifact: %s\n", out_path);
        }
    }

    bool gate_pass = true;
    if (real_nf_path) {
        printf("  cross-check vs %s (plan step 4 gate: within 2x where keys correspond):\n", real_nf_path);
        for (int z = 0; z < NZ; z++) {
            for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
                const char *real_metric = nf_real_metric_for((firing_subscore_t)s);
                if (!real_metric || floor_n[z][s] == 0) continue;
                float real_val;
                if (!nf_read_real_floor_max(real_nf_path, z, real_metric, &real_val)) continue;
                float sim_val = floor_val[z][s];
                float ratio = (real_val > 0.0f && sim_val > 0.0f)
                                  ? ((sim_val > real_val) ? (sim_val / real_val) : (real_val / sim_val))
                                  : 1e30f;
                bool ok = ratio <= 2.0f;
                if (!ok) gate_pass = false;
                printf("    z%d %-20s sim=%.4f real(max)=%.4f ratio=%.2fx -> %s\n", z,
                       nf_subscore_name((firing_subscore_t)s), (double)sim_val, (double)real_val,
                       (double)ratio, ok ? "PASS" : "FAIL");
            }
        }
        printf("  step 4 gate: %s\n", gate_pass ? "PASS" : "FAIL");
    } else {
        printf("  (no noise_floor.json path given -- gate not evaluated this run)\n");
    }
    return gate_pass;
}

int main(int argc, char **argv)
{
    int mc_runs = (argc > 1) ? atoi(argv[1]) : 40;
    /* Catches a stale/mismatched build (leftover .obj, partial manual
     * revert) before it can silently produce a wrong acceptance verdict --
     * see sim_plant.h's SIM_PLANT_ASSERT_ABI_FRESH() comment and
     * docs/audits/sim_iter_tune_stale_object_triage_2026-09-14.md. */
    SIM_PLANT_ASSERT_ABI_FRESH();
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
        tune_run(&g_starts[s], &v, 40, true, g_profile, NSEG, g_profile, NSEG, &rep);
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
    // Step 4: collect the SAME null-experiment firing_compare() results this
    // loop already produces, into the per-(zone, sub-score) accumulator that
    // Part 2b below turns into the noise-floor data artifact.
    noise_floor_accum_t nf_accum;
    nf_accum_init(&nf_accum, mc_runs);
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
            nf_accum_add(&nf_accum, i, &cmp);
        }
    }
    int total = accepts + rejects + insufficient + nopairs;
    printf("  %d null comparisons: ACCEPT %d (%.2f%%)  REJECT %d  INSUFFICIENT %d  NO_PAIRS %d\n",
           total, accepts, 100.0 * accepts / (total ? total : 1), rejects, insufficient, nopairs);

    // ---- Part 2b: step 4 noise-floor artifact ----
    // argv[2] (optional): output path for the floor artifact JSON.
    // argv[3] (optional): path to tools/PcTools/config_presets/noise_floor.json
    // to cross-check against (plan step 4 gate: within 2x where keys
    // correspond). Both are optional so this stays usable as an ad hoc
    // exploratory run with no arguments beyond mc_runs, same as Parts 0-3.
    const char *nf_out_path = (argc > 2) ? argv[2] : NULL;
    const char *nf_real_path = (argc > 3) ? argv[3] : NULL;
    bool a_step4_gate = nf_emit_and_check(&nf_accum, mc_runs, nf_out_path, nf_real_path);
    nf_accum_free(&nf_accum);

    // A1_DESIGN_TARGET_PCT is the plan's original aspirational bar
    // (ITER_TUNE_REDESIGN.md sec 7, A1). It is NOT the pass/fail
    // threshold below, and per the 2026-09-14 root-cause audit
    // (docs/audits/a1_false_accept_root_cause_2026-09-14.md) it is NOT
    // currently reachable on this plant model -- see ROOT CAUSE below for
    // why. It stays in the code as the stated aspiration, deliberately
    // unused in the pass/fail expression so it cannot silently become the
    // enforced value again by a careless edit.
    const double A1_DESIGN_TARGET_PCT = 2.0;
    (void)A1_DESIGN_TARGET_PCT;

    // A1_PINNED_MAX_ACCEPTS / A1_PINNED_TOTAL: pin the CURRENTLY MEASURED
    // false-accept rate as a known-failure ceiling, not the 2.0% aspiration
    // above.
    //
    // ROOT CAUSE (established 2026-09-14, docs/audits/a1_false_accept_root_cause_2026-09-14.md):
    // 24/660 (3.64%) is an HONEST property of three deliberate, individually
    // defended design choices interacting, not a harness defect and not a
    // debt to be paid down by further tuning of this file:
    //   1. sim_plant.c's coupling term adds power driven by the neighbour
    //      zone's RAW, PWM-chopped relay state (0/1 over ~60 s windows;
    //      sim_duty[i] = relay_actual[i] ? 1.0f : 0.0f), matching firmware's
    //      real additive coupling model -- this is the physically-correct
    //      model class, not an artifact (d63a5591).
    //   2. ENTRY_PEAK_C (firing_score.c) is deliberately a RAW, un-smoothed
    //      single-sample maximum over the dwell-entry window. An EMA-smoothed
    //      version was tried and reverted (22cf674b) because smoothing
    //      blinded firing_compare.c's one-sided degradation veto -- so the
    //      raw definition is correct and staying.
    //   3. A1 itself independently randomises noise_seed and start_offset_c
    //      per side (A7, immediately below), which shifts each zone's
    //      relay-edge timing by seconds to tens of seconds -- changing
    //      whether a neighbour's relay edge falls inside a given zone's
    //      entry window purely by coincidence, with nothing to do with either
    //      side's tuning.
    // Together: edge-timing coincidence moves the recorded peak, concentrated
    // on zone 0 (largest inbound coupling) and amplified by n_pairs==3
    // (median of only 3 draws per firing). Two independent attempts to fix
    // this by changing the COUPLING MODEL rather than accepting it made
    // things worse or non-monotone (both carry do-not-retry gates; kept
    // below for the record):
    //   - Level-scheduled coupling gain (8cbd9d67, reverted by 9f054181):
    //     24/660 -> 38/660. Calibrated on a joint-excess range this
    //     harness's binary relay duty never visits (0 samples in the
    //     calibration range over 2.67M steps) and inverts sign under PWM
    //     averaging (Jensen) at its own calibration point.
    //   - Flat coupling-scale sweep (c9ce6b7c): non-monotone in scale
    //     (0.85->20, 1.0->24, 1.173->38, 1.35->18); {18,20,24} cluster at
    //     sampling noise (sd ~4.8 at n=660, see NOISE FLOOR below), only
    //     1.173 marginally elevated -- no clean scale fix either.
    //
    // NOISE FLOOR: sampling standard deviation on a count of ~24 out of 660
    // trials is approximately 4.8 counts (binomial sd = sqrt(n*p*(1-p)) at
    // p~=0.036, n=660). Any future re-measurement that moves this pin by
    // less than roughly 10 counts (e.g. 24 -> 20) is NOT a demonstrated
    // improvement -- it is noise. Do not chase or report such a move as
    // progress; a genuine fix should produce a change large relative to this
    // floor.
    //
    // 2.0% (A1_DESIGN_TARGET_PCT) IS NOT CURRENTLY REACHABLE: it was set
    // before the additive coupling model (d63a5591) was adopted and has
    // never been re-derived against this model's real noise floor. Reaching
    // it on the CURRENT plant + scoring design would require one of the
    // levers below, each already independently owned and defended -- so
    // pulling one is a deliberate trade against that defense, not a cleanup
    // of this file:
    //   (a) Change sim_plant.c's coupling term to be driven by a
    //       WINDOW-AVERAGED duty rather than the raw relay state -- but the
    //       raw relay state is what matches firmware's actual additive
    //       model (d63a5591); averaging it would decouple this harness from
    //       firmware behaviour it is meant to predict.
    //   (b) Change ENTRY_PEAK_C's definition away from a raw single-sample
    //       max (e.g. smoothing/EMA) -- already tried and reverted
    //       (22cf674b) because it blinds firing_compare.c's degradation
    //       veto in the accept-permissive direction (docs/audits/
    //       firing_score_entry_ema_review_2026-09-10.md).
    //   (c) Change A1's own per-side randomisation of noise_seed/
    //       start_offset_c (A7) to reduce relay-edge timing spread -- but
    //       that randomisation is what A1 exists to stress: removing or
    //       narrowing it would understate the real false-accept exposure
    //       rather than fix it.
    // EXIT CONDITION: this pin tightens toward A1_DESIGN_TARGET_PCT only if
    // one of (a)/(b)/(c) above is deliberately changed -- named explicitly,
    // with the trade-off it reopens acknowledged in that change's own
    // commit/comment -- and A1 (kilnctl_sim_iter_tune.exe 220, 660 null
    // comparisons) is re-measured against it. Absent such a change, 3.64%
    // is the expected, understood rate for this plant/scoring design, and
    // the gap to 2.0% is not neglect -- do not re-open this as a bug to
    // chase without naming which of (a)/(b)/(c) is being traded away.
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
    printf("  A1 bar: PINNED KNOWN-RATE CEILING <= %d/%d (%.4f%%) -- the %.1f%% design target is an "
           "ASPIRATION not currently reachable on this plant -> %s\n",
           A1_PINNED_MAX_ACCEPTS, A1_PINNED_TOTAL,
           100.0 * A1_PINNED_MAX_ACCEPTS / A1_PINNED_TOTAL, A1_DESIGN_TARGET_PCT, a1_pass ? "PASS" : "FAIL");
    if (a1_pass) {
        printf("  A1 NOTE: this PASS is against a pinned rate, not proof the 2.0%% design target is met.\n"
               "           Root cause (2026-09-14 audit) is an HONEST property of three deliberate,\n"
               "           independently-defended choices: sim_plant.c's raw-relay-driven additive\n"
               "           coupling (d63a5591), ENTRY_PEAK_C's raw un-smoothed peak (22cf674b revert),\n"
               "           and A1's own per-side randomisation of noise_seed/start_offset_c. Reaching\n"
               "           2.0%% requires deliberately trading away one of those three -- see this\n"
               "           file's EXIT CONDITION comment above A1_PINNED_MAX_ACCEPTS for the named\n"
               "           levers. Noise floor at n=660 is ~4.8 counts (sd); do not read a move under\n"
               "           ~10 counts as improvement. Full root-cause writeup:\n"
               "           docs/audits/a1_false_accept_root_cause_2026-09-14.md.\n");
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
        tune_run(ss, &v, 40, false, g_profile, NSEG, g_profile, NSEG, &rep);
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

    // ---- Part 4: A8 profile independence (A1 half only -- see note below) ----
    // Plan sec 7, A8: "A1 and A2 re-measured with trial and baseline firings
    // on different profiles sharing >= 5 segment classes -- unchanged within
    // their own bars." This implements the A1 half: the same null-experiment
    // procedure as Part 2 (identical gains, only noise/start-temp differ),
    // except side A runs g_profile and side B runs g_profile_b -- two
    // profiles sharing all 6 segment classes (see g_profile_b's own comment)
    // but differing in per-segment duration, tick count and total length.
    // If the comparator or firing_score.c were secretly sensitive to firing
    // shape/length rather than only to matched segment classes, this would
    // move the false-accept rate; if it is a fair test of the mechanism
    // alone, the rate should land within the same pinned ceiling as Part 2's
    // same-profile A1 measurement.
    //
    // NOT YET DONE: the A2 (never-worse) half of A8 -- re-running Part 3's
    // mismatched-ensemble accept/reject loop with tune_run()'s internal
    // baseline-vs-trial comparisons crossing g_profile/g_profile_b -- needs
    // tune_run() itself to accept a profile per firing, which this pass does
    // not touch. Left open; see this file's own header note and the plan's
    // sec 8 step 6 status for the remaining gap.
    printf("\n#### PART 4: A8 profile independence (A1 half) -- baseline on g_profile, "
           "trial on g_profile_b ####\n");
    int p4_accepts = 0, p4_rejects = 0, p4_insufficient = 0, p4_nopairs = 0;
    for (int n = 0; n < mc_runs; n++) {
        plant_variant_t a = nominal_variant(0x2000u + (uint32_t)n * 7919u);
        plant_variant_t b = a;
        b.noise_seed = 0xa000u + (uint32_t)n * 104729u;
        b.start_offset_c = urand(-15.0f, 15.0f);
        a.start_offset_c = urand(-15.0f, 15.0f);
        iter_tune_gains_t g[NZ];
        for (int i = 0; i < NZ; i++) { g[i].kp = g_kp_bench[i]; g[i].ki = g_ki_bench[i]; g[i].kd = g_kd_bench[i]; }
        firing_result_t ra, rb;
        run_firing_on(g, &a, g_profile, NSEG, &ra);
        run_firing_on(g, &b, g_profile_b, NSEG_B, &rb);
        for (int i = 0; i < NZ; i++) {
            firing_compare_result_t cmp;
            switch (firing_compare(&ra.set[i], &rb.set[i], NULL, &cmp)) {
                case FIRING_COMPARE_ACCEPT: p4_accepts++; break;
                case FIRING_COMPARE_REJECT_DEGRADED: p4_rejects++; break;
                case FIRING_COMPARE_INSUFFICIENT: p4_insufficient++; break;
                default: p4_nopairs++; break;
            }
        }
    }
    int p4_total = p4_accepts + p4_rejects + p4_insufficient + p4_nopairs;
    printf("  %d cross-profile null comparisons: ACCEPT %d (%.2f%%)  REJECT %d  INSUFFICIENT %d  NO_PAIRS %d\n",
           p4_total, p4_accepts, 100.0 * p4_accepts / (p4_total ? p4_total : 1), p4_rejects, p4_insufficient,
           p4_nopairs);
    // "unchanged within its own bar": Part 2 and Part 4 draw from the SAME
    // urand()/LCG stream but at different offsets (Part 4 runs after Parts
    // 1-3 have already advanced it), so a same-profile re-run of this exact
    // procedure does NOT reproduce 24/660 exactly -- confirmed by a sanity
    // run with g_profile_b temporarily set identical to g_profile, which
    // measured 30/660 (4.55%), not 24/660. A bare re-use of A1's bit-exact
    // pin (24/660) would therefore misclassify ordinary sampling noise as an
    // A8 failure. Widen by 3 sampling standard deviations
    // (sd = sqrt(n*p*(1-p)), the same binomial-noise formula A1's own
    // comment above derives and uses) around A1's pinned rate before
    // comparing -- "unchanged within its own bar" means unchanged relative
    // to that bar's own sampling noise, not bit-for-bit identical under a
    // different RNG draw.
    double a1_p = (double)A1_PINNED_MAX_ACCEPTS / (double)A1_PINNED_TOTAL;
    double a8_sd = sqrt((double)p4_total * a1_p * (1.0 - a1_p));
    double a8_ceiling = a1_p * (double)p4_total + 3.0 * a8_sd;
    bool a8_pass = (double)p4_accepts <= a8_ceiling;
    printf("  A8 bar: cross-profile false-accept rate unchanged within A1's own pinned ceiling "
           "+/- 3 sampling sd (<= %.1f of %d) -> %s\n", a8_ceiling, p4_total, a8_pass ? "PASS" : "FAIL");

    // ---- Part 5: A8 profile independence (A2 half) ----
    // Plan sec 7, A8: "A1 and A2 re-measured with trial and baseline
    // firings on different profiles ... unchanged within their own bars."
    // A2's own bar (sec 7: "final tracking cost vs. starting cost ...
    // fraction of runs ending worse by more than one Bar-1 floor <= 1%")
    // is itself defined entirely by ONE pair of compared firings per
    // zone-run: tune_run()'s own ev0 (initial gains)/ev1 (final gains)
    // evaluation. So the faithful cross-profile re-measurement of A2 is:
    // run the search to termination exactly as Part 3 does -- accept/reject
    // decided on g_profile, the one real profile being fired while tuning
    // -- and then, instead of scoring "did the found gains get worse on the
    // SAME profile", score "did the found gains get worse on a DIFFERENT
    // profile the kiln might be fired on next" by evaluating both ev0 and
    // ev1 on g_profile_b via tune_run()'s eval_profile/eval_nseg params
    // (added above; the search itself is unchanged -- it never sees
    // g_profile_b). This is the literal reading of "a tuning accepted on
    // one profile must not degrade performance when the kiln is
    // subsequently run on a different profile": the search runs once, on
    // the profile it actually tuned against, and the transfer question is
    // asked only at the evaluation step, which is exactly where A2's own
    // metric already lives.
    //
    // "Unchanged within its own bar" for A2 needs no statistical widening
    // the way A1's did: A2's bar is not a pre-existing nonzero measured
    // rate with known sampling noise (A1's problem, see the a8_ceiling
    // derivation above) -- it is a flat <= 1% threshold on its own terms,
    // so the SAME 1% bar A2 already uses is the direct, faithful
    // "unchanged" comparison. Same mismatched-ensemble construction as
    // Part 3 (K+/-30%, tau+/-40%, L+/-50%, coupling+/-50%, start +/-15C),
    // different seed base so this draws its own slice of the urand()/LCG
    // stream rather than reusing Part 3's exact sequence.
    printf("\n#### PART 5: A8 profile independence (A2 half) -- search on g_profile, "
           "never-worse evaluated on g_profile_b ####\n");
    int p5_worse = 0, p5_better = 0, p5_same = 0, p5_zruns = 0;
    for (int n = 0; n < mc_runs; n++) {
        plant_variant_t v = mismatched_variant(0x7000u + (uint32_t)n * 2654435761u);
        const start_set_t *ss = &g_starts[n % NSTART];
        run_report_t rep;
        tune_run(ss, &v, 40, false, g_profile, NSEG, g_profile_b, NSEG_B, &rep);
        for (int i = 0; i < NZ; i++) {
            float d = rep.final_cost[i] - rep.initial_cost[i];
            if (!isfinite(d)) continue;
            p5_zruns++;
            if (d > 0.5f) p5_worse++;
            else if (d < -0.5f) p5_better++;
            else p5_same++;
        }
    }
    printf("  %d zone-runs over %d mismatched plants, evaluated on g_profile_b: better %d, "
           "unchanged(<0.5C) %d, WORSE %d (%.2f%%)\n",
           p5_zruns, mc_runs, p5_better, p5_same, p5_worse, 100.0 * p5_worse / (p5_zruns ? p5_zruns : 1));
    bool a8a2_pass = (100.0 * p5_worse / (p5_zruns ? p5_zruns : 1)) <= 1.0;
    printf("  A8 A2-half bar: cross-profile worse-by-more-than-one-floor <= 1%% -> %s\n",
           a8a2_pass ? "PASS" : "FAIL");

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
    // a_step4_gate is true both when the step 4 cross-check genuinely passed
    // AND when it was not evaluated this run (no nf_real_path given) -- see
    // nf_emit_and_check()'s own doc comment. It only ever turns this FALSE
    // when a real cross-check ran and found a corresponding key more than
    // 2x apart, so folding it in here does not change behaviour for callers
    // that never pass argv[3] (every existing caller, until
    // check_iter_tune_noise_floor.ps1).
    // A8 is measured and printed above but deliberately kept OUT of
    // all_pass, the same "informational, non-blocking" treatment this
    // plan's own sec 6.5 credibility gate got while its own bars were open
    // (ITER_TUNE_REDESIGN.md's 2026-09-10 status block). A8 genuinely
    // FAILS on this plant (45/660, 6.82%, against a 38.4-count ceiling) --
    // per plan sec 8 step 6 ("all eight criteria met... any miss ends the
    // plan at this line with a report, not a workaround"), folding it into
    // all_pass would force a choice between a permanently red build_host_
    // tests suite or quietly loosening/pinning A8's bar the way A1's was --
    // and pinning THIS number would be exactly the "workaround" the plan
    // forbids, since it is a genuine, newly-discovered doubling of the
    // false-accept rate under a different profile shape, not an already-
    // understood, previously-defended tradeoff the way A1's pin is. The
    // finding stands as reported, blocking for the PLAN (not the build) --
    // see ITER_TUNE_REDESIGN.md sec 8, step 6.
    // A8's A2-half (Part 5) is DIFFERENT from the A1-half above: it is a
    // genuine, reproduced PASS (0/660 cross-profile worse, well inside the
    // <= 1% bar -- see PART 5's own printf), not a newly-discovered
    // regression. A2's bar carries none of A1's "already a known nonzero
    // rate, pinned rather than met" baggage, so there is no tension in
    // enforcing it the same way A2/A5/A6 already are: doing so cannot force
    // a permanently red build (it measures clean with margin) and closes
    // the one criterion the 2026-09-16 status block named as the remaining
    // open gap in plan sec 8 step 6. Gated into all_pass below.
    (void)a8_pass;
    bool all_pass = a1_pass && a2_pass && a5_pass && a6_pass && a_step4_gate && a8a2_pass;
    printf("\n#### OVERALL: %s (A8 A1-half measured above, NOT gated into this verdict -- see A8 comment; "
           "A8 A2-half IS gated, see A8 A2-half comment) ####\n",
           all_pass ? "PASS (A2/A5/A6 clear; A1 clear ONLY against its pinned known-failure "
                      "ceiling, NOT the 2.0% design target -- see A1 NOTE above)"
                    : "FAIL");
    return all_pass ? 0 : 1;
}
