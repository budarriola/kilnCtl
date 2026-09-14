// sim_fuzzy_overshoot -- re-measures the fuzzy layer against its actual
// stated purpose: docs/audits/fuzzy_overshoot_measurement_2026-09-13.md.
//
// THE CORRECTION THIS FILE EXISTS FOR. Every measurement of the fuzzy layer
// taken before this file (sim_fuzzy_closedloop.c's own "tracking scenario",
// every iter_tune comparator run) judged it on IAE/MAE -- integral/mean
// ABSOLUTE error averaged over an entire run. The project owner has since
// clarified the fuzzy layer's actual purpose: it exists to reduce OVER/
// UNDERSHOOT, a transient concentrated at setpoint transitions and dwell
// entry. Averaging over thousands of settled dwell ticks dilutes exactly
// that transient toward invisibility -- a controller can have near-
// identical MAE while overshooting substantially less. So "every fuzzy
// difference is under the 0.5 degC materiality line," the standing
// conclusion from every prior measurement, may be true of tracking error
// and IRRELEVANT to the question actually being asked. This file asks the
// right question instead: peak overshoot/undershoot at dwell entry,
// specifically.
//
// SCRATCH FILE, NOT sim_fuzzy_closedloop.c. sim_fuzzy_closedloop.c is mid-
// edit by another session as of 2026-09-13 (docs/audits/fuzzy_dimensionless_
// bands_2026-09-13.md's band-derivation change) -- this file is a separate,
// standalone harness so the two edits never collide. It deliberately
// duplicates a small amount of orchestration from that file (make_plant_cfg,
// sim_tick) rather than #include-ing or depending on it, so it has zero
// coupling to whatever that other session lands. If the two files drift,
// that is an acceptable, bounded cost; if this file gets merged into that
// one later once both settle, that is a fine follow-up, not an obligation
// of this pass.
//
// WHAT THIS LINKS (real production .c, no mirror of the control math):
//   - pid_fuzzy.c: pid_fuzzy_adjust() itself, unmodified, plus
//     pid_fuzzy_derive_bands() (the same real function sim_fuzzy_closedloop.c
//     is being updated to call, per fuzzy_dimensionless_bands_2026-09-13.md --
//     reused here rather than re-deriving band math by hand).
//   - pid.c: pid_update()/pid_reset()/pid_rescale_integral_for_new_ki(),
//     wired identically to sim_fuzzy_closedloop.c's own sim_tick() (see that
//     file's top comment for the citation to profile_executor_pid_tick.c's
//     real call site -- this file reproduces the same wiring, verbatim).
//   - sim_plant.c: sim_plant_reset()/sim_plant_step(), the same single-zone
//     FOPDT model (zone 0's measured constants) sim_fuzzy_closedloop.c uses.
//     NO synthetic element/sensor disturbance injection anywhere in this
//     file -- ordinary ramp-to-dwell transitions only, so the overshoot
//     measured here is a real controller response to a real (simulated)
//     setpoint schedule, not a non-physical shock. This is the fix for the
//     concurrency instructions' explicit warning: the closedloop harness's
//     +-8C disturbance pokes element_c/sensor_c directly, bypassing
//     sensor_delay_s, and its results must never be cited as evidence about
//     firing behaviour. This file cites none of that scenario's numbers.
//
// OVERSHOOT DEFINITION, reusing firing_score.c's FIRING_SUBSCORE_ENTRY_PEAK_C
// (firmware/KilnFW/App/drivers/control/firing_score.c, read in full before
// writing this, including 22cf674b's revert from an EMA back to raw peak
// tracking -- current code, not history):
//
//   entry_window_s = dead_time_s + 2*tau_s   (firing_score_seg_begin())
//   entry_peak_c   = max over the entry window of (actual_c - target_c),
//                    clamped to >= 0 (firing_score_seg_finish(): "a dwell
//                    entered from below never overshoots, and reporting a
//                    negative 'overshoot' would let an undershooting trial
//                    score better on the overshoot axis for the wrong
//                    reason")
//
// This file reproduces that EXACT window and EXACT peak-tracking rule
// (max, not EMA) for the OVERSHOOT number it reports, using this harness's
// own zone-0 dead_time_s/tau_s (the same sim_measured_zone_constants.h
// values sim_fuzzy_closedloop.c's make_plant_cfg() already uses to build
// the plant this harness runs against). It additionally reports the mirror
// image -- peak UNDERSHOOT (min of actual_c - target_c over the same
// window, clamped to <= 0, reported as a positive magnitude) -- which
// firing_score.c deliberately does NOT score (by design, per the comment
// above: overshoot only), but which this task's brief explicitly asks for,
// since a controller could reduce overshoot by simply approaching more
// timidly and undershooting instead, which would not be an improvement.
// "Time to settle" is this file's own addition, not in firing_score.c:
// ticks from dwell entry until |actual_c - target_c| first falls within
// SETTLE_BAND_C and does not leave it again during the remainder of that
// dwell segment (a transient dip through the band that later kicks back out
// does not count as settled).
//
// SCOPE LIMITS (state these every time this file's numbers are quoted):
//   1. Single-zone only. Single-column transport is measured LINEAR on
//      hardware (project_z0_coupling_is_shape_not_scale.md); the multi-zone
//      coupling model is refuted (project_coupling_failure_is_joint_dwell_
//      specific.md) with two replacement attempts failed
//      (coupling_level_schedule_adjudication_2026-09-11.md). Single-zone sim
//      is the trustworthy scope; this file never claims multi-zone coverage.
//   2. Magnitude scope. This bench rig's zone 0 model tops out at roughly
//      ambient + k_dc*duty, ~24 + 39*1.0 = ~63C -- this bench cannot exceed
//      ~40C above ambient, while a real kiln firing reaches ~1200C. The
//      overshoot degC figures this file reports are NOT kiln-scale overshoot
//      figures; they rank controller behaviour on THIS plant model only.
//
// Usage: no arguments. Exit 0 = all assertions pass (see ASSERTIONS below).
// Non-zero = named failure printed to stdout.
//
// ASSERTIONS (falsifiable, not print-only -- project_harness_prints_verdict_
// exits_zero.md's exact failure mode: a harness that prints a verdict and
// exits 0 guards nothing):
//   1. strength_pct=0 bit-for-bit PID-gain contract, same as sim_fuzzy_
//      closedloop.c's check 1 -- re-verified here since this file has its
//      own independent sim_tick().
//   2. Every dwell segment actually reaches its entry window (entry_seen
//      true) -- if the ramp is too slow/fast for the scenario to ever climb
//      through a dwell transition, the whole measurement is vacuous, and
//      this file refuses to report success in that case.
//   3. Negative-tested (see the commit this shipped in): pid_fuzzy.c's
//      strength_pct==0 short-circuit was broken by hand (multiplying the
//      returned kp by 0.999f), confirmed to fail assertion 1 with a
//      non-zero exit, then restored BY HAND, followed by a FULL REBUILD
//      before re-confirming PASS (project_green_build_in_dirty_tree_proves_
//      nothing_about_head.md's exact hazard: a stale poisoned binary must
//      never be measured from -- 8a12521b).

#include "../drivers/control/pid.h"
#include "../drivers/control/pid_fuzzy.h"
#include "sim_plant.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_measured_zone_constants.h"

#define DT_S 5.0f
#define AMBIENT_C 24.0f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f
#define SETTLE_BAND_C 2.0f /* independent of firing_score.c's default 5.0 band_c --
                            * this file's own, tighter, stated explicitly since
                            * "settle" is not a firing_score.c concept */

static void make_plant_cfg(sim_plant_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ambient_c = AMBIENT_C;
    out->thermal_mass_j_per_c = g_tau_s[0];
    out->heater_power_w = g_k_dc[0];
    out->loss_coeff_w_per_c = 1.0f;
    out->sensor_delay_s = g_dead_time_s[0];
    out->sensor_lag_tau_s = 0.0f;
}

// Identical wiring to sim_fuzzy_closedloop.c's sim_tick() (see that file's
// top comment for the citation to the real profile_executor_pid_tick.c call
// site) -- duplicated here, not shared, per this file's own top comment on
// why it stays independent of that file while it is mid-edit. base_kp/ki/kd
// are now PARAMETERS (sim_fuzzy_closedloop.c hardcodes them as #defines)
// so this file can run the fixed-gain-retune arm through the exact same
// tick function as the fuzzy arms, with only the input gains differing.
static float sim_tick(sim_plant_state_t *pstate, const sim_plant_cfg_t *pcfg,
                       pid_state_t *pid_state, float *prev_effective_ki,
                       float target_c, uint8_t strength_pct,
                       float base_kp, float base_ki, float base_kd,
                       float error_band_c, float rate_band_c_per_s,
                       float *out_error_c, bool *out_bitexact_ok)
{
    float measurement = pstate->sensor_c;
    float error_c = target_c - measurement;
    float rate_c_per_s = pid_state->d_filtered;

    float adj_kp = base_kp, adj_ki = base_ki, adj_kd = base_kd;
    pid_fuzzy_adjust(error_c, rate_c_per_s, error_band_c, rate_band_c_per_s,
                     base_kp, base_ki, base_kd, strength_pct, &adj_kp, &adj_ki, &adj_kd);

    if (strength_pct == 0) {
        *out_bitexact_ok = (adj_kp == base_kp) && (adj_ki == base_ki) && (adj_kd == base_kd);
    } else {
        *out_bitexact_ok = true;
    }

    pid_rescale_integral_for_new_ki(pid_state, *prev_effective_ki, adj_ki);
    *prev_effective_ki = adj_ki;

    pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
    float duty = pid_update(pid_state, &cfg, target_c, measurement, DT_S, 0.0f, 0.0f);

    sim_plant_step(pstate, pcfg, duty, DT_S);

    *out_error_c = error_c;
    return duty;
}

// ---------------------------------------------------------------------
// Ramp-to-dwell scenario, representative of a real firing: three ramp/
// dwell segments, alternating an approach from below (overshoot risk) and
// from above (undershoot risk). Ramp rate matches sim_fuzzy_closedloop.c's
// own "brisk but normal" tracking scenario (100 degC/hr, well under the
// fuzzy rate band) -- this is deliberately an ORDINARY commanded ramp, not
// a disturbance.
// ---------------------------------------------------------------------
#define RAMP_RATE_C_PER_S (100.0f / 3600.0f)
#define DWELL_TICKS 400 /* 2000s = 33.3 min; entry_window_s for zone 0 is
                         * dead_time_s+2*tau_s = 52.8+2*263.8 = 580.4s =
                         * 116 ticks, so 400 ticks covers the entry window
                         * plus 284 ticks (23.7 min) of steady dwell */

typedef struct {
    float dwell_target_c;
} dwell_phase_t;

static const dwell_phase_t PHASES[] = {
    { AMBIENT_C + 15.0f }, /* approach from below (ramp up) */
    { AMBIENT_C + 30.0f }, /* approach from below (ramp up) */
    { AMBIENT_C + 12.0f }, /* approach from above (ramp down) -- undershoot risk */
};
#define N_PHASES (int)(sizeof(PHASES) / sizeof(PHASES[0]))

// docs/audits/fuzzy_overshoot_measurement_2026-09-13.md widened from
// overshoot-only to the owner's full four-part objective. All four are
// reported PER ARM PER DWELL, never collapsed into one score -- exactly the
// failure mode IAE/MAE has (objectives trading against each other inside a
// single number, invisibly: e.g. a slower approach can cut overshoot while
// regressing ramp lag and settle time, and an aggregate would hide that).
// docs/audits/reverted_control_decisions_reexamination_2026-09-13.md (opus,
// 2edbb6eb) found firing_score.c's subscores DEFECTIVE as instruments for
// two of the four objectives -- both gaps ACCEPT-PERMISSIVE (they make a
// worse controller look equal, never worse), so they must not be relied on
// here:
//   - Objective 2 (settle quickly): NO INSTRUMENT AT ALL in firing_score.c.
//     A zone that settles in 60s and one that rings for 400s score
//     identically given the same peak and steady RMS. This file's
//     settle_ticks field is its OWN addition, was already NOT reusing a
//     firing_score.c definition, and remains the plan for this objective.
//   - Objective 4b (undershoot): firing_score.c's FIRING_SUBSCORE_ENTRY_
//     PEAK_C clamps the negative case to 0.0f AND is windowed to
//     entry_window_s -- so an undershoot that recovers before dead_time+
//     2*tau elapses leaves NO trace. This file's PREVIOUS revision made the
//     same mistake (windowed undershoot_c, mirroring the overshoot window
//     exactly). Fixed below: undershoot_signed_c is now the most negative
//     (actual-target) reached ANYWHERE in the dwell segment (not windowed,
//     not suppressed by later recovery), reported SIGNED (negative = went
//     below target; a value >= 0 means it never did).
//   - Objective 1 (LAG_S) additionally has two blind spots per that same
//     audit: it is UNSIGNED (a rate-leading and a rate-lagging tick score
//     the same magnitude) and DROPS saturated-and-short ticks (the exact
//     ticks where a tracking failure would be worst). This file's
//     ramp_lag_median_c is unsigned and matches LAG_S's definition
//     (reported alongside that explicit caveat in the printed comparison);
//     it does NOT drop any ticks (no saturation filter, no min-tick
//     threshold anywhere in this file's ramp loop, unlike firing_score.c's
//     `if (saturated_high && err < 0.0f) return;`), so the "drops the worst
//     ticks" blind spot does not apply to THIS number even though it does
//     to firing_score.c's own LAG_S. A separate SIGNED lag figure is also
//     collected (ramp_lag_signed_median_s) so a leading-vs-lagging
//     asymmetry is visible rather than folded into one unsigned number.
typedef struct {
    float ramp_lag_median_s;        /* objective 1: median of
                                     * abs(error_c)/commanded_rate_c_per_s
                                     * over the ramp into this dwell -- same
                                     * definition as FIRING_SUBSCORE_LAG_S,
                                     * with that subscore's two named
                                     * limitations (unsigned; drops
                                     * saturated-and-short ticks in
                                     * firing_score.c, though not in this
                                     * file's own unfiltered ramp loop).
                                     * Median, not mean, for the same
                                     * boundary-artifact-robustness reason
                                     * FIRING_SUBSCORE_LAG_S uses a median.
                                     * -1 if this dwell had no ramp. */
    float ramp_lag_signed_median_s; /* this file's own addition, addressing
                                     * LAG_S's "unsigned" blind spot: median
                                     * of (error_c)/(ramp_dir*commanded_rate)
                                     * -- positive means BEHIND the commanded
                                     * ramp (lagging), negative means AHEAD
                                     * of it (leading/overshooting the ramp
                                     * itself, not just the eventual dwell).
                                     * -1e9 sentinel if this dwell had no
                                     * ramp (checked via ramp_lag_median_s
                                     * instead; this field is never read
                                     * without that guard). */
    float overshoot_c;   /* objective 4a (overshoot): exactly
                          * FIRING_SUBSCORE_ENTRY_PEAK_C -- max(actual-target,
                          * 0) over entry_window_s = dead_time_s+2*tau_s from
                          * dwell entry, RAW PEAK (not EMA -- firing_score.c's
                          * 22cf674b revert, confirmed CORRECT by the same
                          * 2edbb6eb opus pass that found the other two gaps,
                          * so this is the one subscore definition this file
                          * keeps unmodified). */
    float undershoot_signed_c; /* objective 4b (undershoot) -- NOT reusing
                                * firing_score.c's clamped/windowed
                                * definition (see the gap named above).
                                * min(actual-target) over the ENTIRE dwell
                                * segment (all DWELL_TICKS, not just
                                * entry_window_s), reported SIGNED: negative
                                * means the zone went that far below target
                                * at its worst point, and a later recovery
                                * does NOT erase it from this number. A
                                * value >= 0 means the zone never dipped
                                * below target at all during this dwell. */
    int   settle_ticks;  /* objective 2 (settles quickly). NOT a
                          * firing_score.c subscore -- firing_score.c HAS
                          * NO instrument for this objective at all
                          * (2edbb6eb Finding A), so this field was already,
                          * and remains, this file's own definition. Ticks
                          * from dwell entry to first-and-LASTING (a
                          * transient dip back out does not count) arrival
                          * within SETTLE_BAND_C. -1 if never settled during
                          * this dwell's DWELL_TICKS window. */
    float steady_rms_c;  /* objective 3 (settles ACCURATELY, i.e.
                          * steady-state error). Exactly
                          * FIRING_SUBSCORE_STEADY_RMS_C: sqrt(mean(err^2))
                          * over every tick AFTER entry_window_s within this
                          * same dwell segment (firing_score_seg_tick()'s
                          * else-branch) -- 2edbb6eb's audit found this
                          * subscore sound, unlike the other two. 0 if the
                          * dwell never reached its steady portion. */
    bool  entry_seen;    /* did this dwell segment actually run at least one
                          * tick inside its own entry window? (sanity check
                          * against a scenario too short/fast to measure) */
    bool  steady_seen;   /* did this dwell segment actually reach ticks past
                          * its own entry window? (sanity check for
                          * steady_rms_c's validity) */
} dwell_result_t;

static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

#define MAX_RAMP_SAMPLES 4096
static float g_ramp_lag_buf[MAX_RAMP_SAMPLES];
static float g_ramp_lag_signed_buf[MAX_RAMP_SAMPLES];

static float median_of(float *buf, int n)
{
    if (n <= 0) return -1.0f;
    qsort(buf, (size_t)n, sizeof(float), cmp_float);
    if (n % 2 == 1) return buf[n / 2];
    return 0.5f * (buf[n / 2 - 1] + buf[n / 2]);
}

static double run_overshoot_scenario(uint8_t strength_pct, float base_kp, float base_ki, float base_kd,
                                     dwell_result_t out_results[N_PHASES], bool *out_bitexact_ok)
{
    sim_plant_cfg_t pcfg;
    make_plant_cfg(&pcfg);
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &pcfg);

    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    float error_band_c, rate_band_c_per_s;
    pid_fuzzy_derive_bands(g_k_dc[0], g_tau_s[0], &error_band_c, &rate_band_c_per_s);

    float entry_window_s = g_dead_time_s[0] + 2.0f * g_tau_s[0];

    float target_c = AMBIENT_C;
    bool all_bitexact = true;
    double iae = 0.0;

    for (int p = 0; p < N_PHASES; p++) {
        float dwell_target = PHASES[p].dwell_target_c;
        float ramp_dir = (dwell_target > target_c) ? 1.0f : -1.0f;

        // Ramp phase: step target_c toward dwell_target at RAMP_RATE_C_PER_S
        // until reached. Tracks objective 1 (ramp-lag, FIRING_SUBSCORE_LAG_S)
        // -- entry-window/overshoot tracking still starts only once the
        // dwell itself begins, exactly as firing_score_seg_begin()/
        // classify()'s FIRING_SEG_DWELL-vs-ramp split does.
        int ramp_lag_n = 0;
        while ((ramp_dir > 0.0f && target_c < dwell_target) ||
               (ramp_dir < 0.0f && target_c > dwell_target)) {
            target_c += ramp_dir * RAMP_RATE_C_PER_S * DT_S;
            if ((ramp_dir > 0.0f && target_c > dwell_target) ||
                (ramp_dir < 0.0f && target_c < dwell_target)) {
                target_c = dwell_target;
            }
            float error_c; bool bitexact_ok;
            sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, strength_pct,
                    base_kp, base_ki, base_kd, error_band_c, rate_band_c_per_s,
                    &error_c, &bitexact_ok);
            if (!bitexact_ok) all_bitexact = false;
            iae += fabs((double)error_c) * DT_S;

            // FIRING_SUBSCORE_LAG_S's own definition: lag_s = abs_err /
            // rate_c_per_s, using the COMMANDED rate (RAMP_RATE_C_PER_S),
            // not the plant's actual measured rate -- firing_score.c uses
            // seg->rate_c_per_s, set once at firing_score_seg_begin() from
            // the commanded segment rate, never re-measured per tick.
            // error_c = target - measurement here, and ramp_dir is +1 for a
            // ramp-up / -1 for a ramp-down; error_c/(ramp_dir*rate) is
            // positive when the measurement is BEHIND the commanded ramp
            // (lagging, in the direction of travel) and negative when AHEAD
            // of it -- this file's own signed complement to LAG_S's
            // unsigned blind spot (2edbb6eb Finding C).
            if (ramp_lag_n < MAX_RAMP_SAMPLES) {
                g_ramp_lag_buf[ramp_lag_n] = fabsf(error_c) / RAMP_RATE_C_PER_S;
                g_ramp_lag_signed_buf[ramp_lag_n] = error_c / (ramp_dir * RAMP_RATE_C_PER_S);
                ramp_lag_n++;
            }
        }

        // Dwell phase: target_c held flat at dwell_target for DWELL_TICKS.
        dwell_result_t *res = &out_results[p];
        memset(res, 0, sizeof(*res));
        res->settle_ticks = -1;
        res->ramp_lag_median_s = median_of(g_ramp_lag_buf, ramp_lag_n);
        res->ramp_lag_signed_median_s = (ramp_lag_n > 0) ? median_of(g_ramp_lag_signed_buf, ramp_lag_n) : -1e9f;
        float min_actual_minus_target = 0.0f; /* clamped at 0: a value that never goes negative
                                               * reports as exactly 0.0 ("never dipped below"),
                                               * not some arbitrary small positive residual. */
        bool settled = false;
        int settle_start_tick = -1;
        double elapsed_s = 0.0;
        double steady_sumsq = 0.0;
        int steady_n = 0;

        for (int t = 0; t < DWELL_TICKS; t++) {
            float error_c; bool bitexact_ok;
            sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, strength_pct,
                    base_kp, base_ki, base_kd, error_band_c, rate_band_c_per_s,
                    &error_c, &bitexact_ok);
            if (!bitexact_ok) all_bitexact = false;
            iae += fabs((double)error_c) * DT_S;

            float actual_minus_target = -error_c; /* error_c = target - measurement, so
                                                    * actual - target = -error_c, matching
                                                    * firing_score.c's own err = actual -
                                                    * target sign convention exactly. */
            // Undershoot tracked over the WHOLE dwell (not windowed to
            // entry_window_s, unlike overshoot) and NEVER suppressed by a
            // later recovery -- 2edbb6eb Finding B's exact fix for the gap
            // in firing_score.c's own (windowed, clamped) definition.
            if (actual_minus_target < min_actual_minus_target) {
                min_actual_minus_target = actual_minus_target;
            }

            elapsed_s += (double)DT_S;
            if (elapsed_s <= (double)entry_window_s) {
                res->entry_seen = true;
                if (actual_minus_target > res->overshoot_c) res->overshoot_c = actual_minus_target;
            } else {
                // FIRING_SUBSCORE_STEADY_RMS_C's else-branch, exactly.
                res->steady_seen = true;
                steady_sumsq += (double)actual_minus_target * (double)actual_minus_target;
                steady_n++;
            }

            bool in_band = fabsf(actual_minus_target) <= SETTLE_BAND_C;
            if (in_band) {
                if (!settled) { settled = true; settle_start_tick = t; }
            } else {
                settled = false; /* left the band again -- a transient dip does not count */
            }
        }
        res->settle_ticks = settled ? settle_start_tick : -1;
        res->steady_rms_c = (steady_n > 0) ? (float)sqrt(steady_sumsq / (double)steady_n) : 0.0f;
        res->undershoot_signed_c = min_actual_minus_target; /* <= 0.0; 0.0 means never dipped below */
    }

    *out_bitexact_ok = all_bitexact;
    return iae;
}

static const char *ARM_NAMES[] = { "fuzzy_off (strength=0)", "fuzzy_25", "fuzzy_50", "fixed_retune_equivalent" };

int main(void)
{
    printf("=== sim_fuzzy_overshoot -- re-measuring the fuzzy layer on OVERSHOOT, not IAE/MAE ===\n");
    printf("See this file's top comment before reading anything below: single-zone,\n"
           "bench-scale (max ~40C above ambient vs a real kiln's ~1200C), no synthetic\n"
           "disturbance injection -- ordinary ramp-to-dwell transitions only.\n\n");

    #define BASE_KP 0.0318f
    #define BASE_KI 0.0001f
    #define BASE_KD 0.8401f

    float error_band_c, rate_band_c_per_s;
    bool bands_ok = pid_fuzzy_derive_bands(g_k_dc[0], g_tau_s[0], &error_band_c, &rate_band_c_per_s);
    printf("Bands: error_band_c=%.2f, rate_band_c_per_s=%.4f (from_model=%s)\n\n",
           (double)error_band_c, (double)rate_band_c_per_s, bands_ok ? "yes" : "NO (fell back!)");

    bool overall_ok = true;

    // Four arms: strength 0/25/50 (fuzzy), plus one fixed-gain retune
    // equivalent to strength_pct=50's typical effect direction (kp*0.75,
    // ki*1.25, kd*0.75), run through strength_pct=0 so pid_fuzzy_adjust()
    // is a pure no-op and the gain change comes ONLY from the retune,
    // never from fuzzy inference -- this is what actually separates "fuzzy
    // inference reduces overshoot" from "these particular gains reduce
    // overshoot," the same distinction the poisoned-binary incident
    // (8a12521b) got wrong on the tracking metric.
    struct { uint8_t strength; float kp, ki, kd; } arms[4] = {
        { 0,  BASE_KP, BASE_KI, BASE_KD },
        { 25, BASE_KP, BASE_KI, BASE_KD },
        { 50, BASE_KP, BASE_KI, BASE_KD },
        { 0,  BASE_KP * 0.75f, BASE_KI * 1.25f, BASE_KD * 0.75f },
    };

    // Four independent metric tables, one per objective -- NEVER reduced to
    // one score (the owner's explicit correction: IAE/MAE's flaw is not
    // "wrong metric," it's "collapses four objectives into one number in
    // which they can trade against each other invisibly").
    //   ramp_lag_median_s (unsigned, ==FIRING_SUBSCORE_LAG_S)  -- objective 1
    //   ramp_lag_signed_median_s (this file's own)             -- objective 1, signed
    //   settle_ticks*DT_S (this file's own -- NO firing_score.c instrument exists) -- objective 2
    //   steady_rms_c (==FIRING_SUBSCORE_STEADY_RMS_C, sound per 2edbb6eb)  -- objective 3
    //   overshoot_c (==FIRING_SUBSCORE_ENTRY_PEAK_C, sound per 2edbb6eb)   -- objective 4a
    //   undershoot_signed_c (this file's own, unwindowed/unclamped --
    //     firing_score.c's own equivalent is a confirmed gap, 2edbb6eb Finding B) -- objective 4b
    float lag_by_arm[4][N_PHASES];
    float lag_signed_by_arm[4][N_PHASES];
    float settle_s_by_arm[4][N_PHASES];
    float steady_rms_by_arm[4][N_PHASES];
    float overshoot_by_arm[4][N_PHASES];
    float undershoot_signed_by_arm[4][N_PHASES];

    for (int a = 0; a < 4; a++) {
        printf("-- arm: %s (kp=%.5f ki=%.5f kd=%.5f) --\n", ARM_NAMES[a], (double)arms[a].kp,
               (double)arms[a].ki, (double)arms[a].kd);
        dwell_result_t results[N_PHASES];
        bool bitexact_ok;
        double iae = run_overshoot_scenario(arms[a].strength, arms[a].kp, arms[a].ki, arms[a].kd,
                                            results, &bitexact_ok);

        if (arms[a].strength == 0 && !bitexact_ok) {
            printf("  FAIL: strength_pct=0 did NOT reproduce base gains bit-for-bit (the safety\n"
                   "  contract this whole mode rests on)\n");
            overall_ok = false;
        }

        for (int p = 0; p < N_PHASES; p++) {
            dwell_result_t *r = &results[p];
            lag_by_arm[a][p] = r->ramp_lag_median_s;
            lag_signed_by_arm[a][p] = r->ramp_lag_signed_median_s;
            settle_s_by_arm[a][p] = (r->settle_ticks >= 0) ? (float)r->settle_ticks * DT_S : -1.0f;
            steady_rms_by_arm[a][p] = r->steady_rms_c;
            overshoot_by_arm[a][p] = r->overshoot_c;
            undershoot_signed_by_arm[a][p] = r->undershoot_signed_c;
            printf("  dwell %d (target=%.1fC): [1]ramp_lag_median=%.1fs (signed=%+.1fs)  [2]settle=%s  "
                   "[3]steady_rms=%.3fC  [4a]overshoot=%.3fC [4b]undershoot=%+.3fC  "
                   "(entry_seen=%s steady_seen=%s)\n",
                   p, (double)PHASES[p].dwell_target_c, (double)r->ramp_lag_median_s,
                   (double)r->ramp_lag_signed_median_s,
                   r->settle_ticks >= 0 ? "yes" : "NEVER",
                   (double)r->steady_rms_c, (double)r->overshoot_c, (double)r->undershoot_signed_c,
                   r->entry_seen ? "yes" : "NO", r->steady_seen ? "yes" : "NO");
            if (r->settle_ticks >= 0) {
                printf("      settle_time=%.0fs\n", (double)settle_s_by_arm[a][p]);
            }
            if (!r->entry_seen) {
                printf("  FAIL: dwell %d's entry window was never reached -- this measurement is "
                       "vacuous for this dwell\n", p);
                overall_ok = false;
            }
            if (!r->steady_seen) {
                printf("  FAIL: dwell %d never reached its steady (post-entry-window) portion -- "
                       "DWELL_TICKS is too short relative to entry_window_s, a scenario-sizing "
                       "bug, not a controller finding\n", p);
                overall_ok = false;
            }
            if (r->settle_ticks < 0) {
                printf("  FAIL: dwell %d never settled within %.1fC inside DWELL_TICKS -- either a "
                       "real controller finding (that arm genuinely never settles here) or a "
                       "scenario-sizing bug; either way this file refuses to report a -1 settle "
                       "time as if it were comparable to a real one\n", p, (double)SETTLE_BAND_C);
                overall_ok = false;
            }
        }
        printf("  IAE over full scenario (ranking only, NOT one of the four objectives): %.1f "
               "degC*s\n\n", iae);
    }

    // Per-objective comparison, each against its OWN 0.5-unit-equivalent
    // materiality line, and each SEPARATELY for the fuzzy arms vs the
    // fixed-retune arm -- a fuzzy arm and the retune arm answer different
    // questions (does fuzzy INFERENCE help vs do these particular gains
    // help), so pooling them into one max() would hide a case where the
    // retune arm regresses on one objective while a fuzzy arm improves it
    // (or vice versa). Materiality line for lag/settle is stated in
    // SECONDS, not degC -- the 0.5 degC rule does not apply to a time axis,
    // so this file states its own bar for those two rather than
    // misapplying the degC one.
    #define TIME_MATERIALITY_S 30.0f /* half of one DT_S*6 -- a round, stated,
                                      * conservative bar; not derived from any
                                      * prior finding, since none of this
                                      * repo's prior work measured lag/settle
                                      * at dwell entry before this file. */
    typedef struct {
        const char *name;
        float (*by_arm)[N_PHASES];
        float bar;
        const char *unit;
    } metric_t;
    metric_t metrics[] = {
        { "[1] ramp-lag, UNSIGNED median (==FIRING_SUBSCORE_LAG_S)", lag_by_arm, TIME_MATERIALITY_S, "s" },
        { "[1] ramp-lag, SIGNED median (this file's own; +=lagging, -=leading)",
          lag_signed_by_arm, TIME_MATERIALITY_S, "s" },
        { "[2] settle time (this file's own -- firing_score.c has NO instrument for this)",
          settle_s_by_arm, TIME_MATERIALITY_S, "s" },
        { "[3] steady-state RMS error (==FIRING_SUBSCORE_STEADY_RMS_C)", steady_rms_by_arm, 0.5f, "C" },
        { "[4a] overshoot, entry peak (==FIRING_SUBSCORE_ENTRY_PEAK_C)", overshoot_by_arm, 0.5f, "C" },
        { "[4b] undershoot, SIGNED whole-dwell peak (this file's own -- firing_score.c's "
          "equivalent is clamped to 0 and windowed, a confirmed gap)", undershoot_signed_by_arm, 0.5f, "C" },
    };
    #define N_METRICS (int)(sizeof(metrics) / sizeof(metrics[0]))

    printf("=== Per-objective comparison vs fuzzy_off (never pooled into one score) ===\n");
    printf("CAVEAT on [1]'s two rows (docs/audits/reverted_control_decisions_reexamination_2026-\n"
           "09-13.md, 2edbb6eb): FIRING_SUBSCORE_LAG_S -- which the unsigned row exactly reproduces\n"
           "-- is UNSIGNED (a leading and a lagging tick score identically) and, IN FIRING_SCORE.C\n"
           "ITSELF, drops saturated-and-short ticks (exactly the worst-tracking ticks). This file's\n"
           "own ramp loop applies no such filter, so that second blind spot does not carry over to\n"
           "either row here -- but the signed row exists specifically because the unsigned row alone\n"
           "cannot distinguish 'lagging behind the ramp' from 'leading ahead of it,' and both are\n"
           "objective-1 failures in opposite directions.\n");
    for (int m = 0; m < N_METRICS; m++) {
        printf("\n-- %s (bar: %.1f%s) --\n", metrics[m].name, (double)metrics[m].bar, metrics[m].unit);
        float max_fuzzy = 0.0f, max_retune = 0.0f;
        for (int p = 0; p < N_PHASES; p++) {
            float off = metrics[m].by_arm[0][p];
            printf("  dwell %d: off=%.2f%s  f25=%.2f%s (d=%+.2f)  f50=%.2f%s (d=%+.2f)  "
                   "retune=%.2f%s (d=%+.2f)\n",
                   p, (double)off, metrics[m].unit,
                   (double)metrics[m].by_arm[1][p], metrics[m].unit, (double)(metrics[m].by_arm[1][p] - off),
                   (double)metrics[m].by_arm[2][p], metrics[m].unit, (double)(metrics[m].by_arm[2][p] - off),
                   (double)metrics[m].by_arm[3][p], metrics[m].unit, (double)(metrics[m].by_arm[3][p] - off));
            for (int a = 1; a <= 2; a++) {
                float d = fabsf(metrics[m].by_arm[a][p] - off);
                if (d > max_fuzzy) max_fuzzy = d;
            }
            float dr = fabsf(metrics[m].by_arm[3][p] - off);
            if (dr > max_retune) max_retune = dr;
        }
        printf("  max|diff| fuzzy(25/50) vs off: %.2f%s -- %s bar\n", (double)max_fuzzy,
               metrics[m].unit, (max_fuzzy > metrics[m].bar) ? "EXCEEDS" : "does not exceed");
        printf("  max|diff| retune vs off:       %.2f%s -- %s bar\n", (double)max_retune,
               metrics[m].unit, (max_retune > metrics[m].bar) ? "EXCEEDS" : "does not exceed");
    }
    printf("\nNOTE: check every metric above for a TRADE -- an improvement on one objective paired\n"
           "with a regression on another, at the same arm, is the actual finding the owner asked\n"
           "this file to surface, and it will NOT show up if these tables are skimmed for only the\n"
           "overshoot rows.\n");

    if (!overall_ok) {
        printf("\n=== sim_fuzzy_overshoot: FAIL ===\n");
        return 1;
    }
    printf("\n=== sim_fuzzy_overshoot: PASS (assertions held; see comparison above for the actual "
           "finding) ===\n");
    return 0;
}
