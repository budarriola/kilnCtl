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
#include "../drivers/control/firing_score.h" /* FIRING_SCORE_SETTLE_BAND_C only --
                                              * no firing_score_seg_*() call, this
                                              * file keeps its own orchestration */
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
// 2026-09-13 four-objective re-score: SETTLE_BAND_C changed 2.0 -> 0.5 to
// match FIRING_SCORE_SETTLE_BAND_C (firing_score.h) exactly, per the opus
// review appended to docs/audits/fuzzy_overshoot_measurement_2026-09-13.md
// (Finding 2): at 2.0C -- 4x this project's own 0.5C materiality line -- the
// settle-time ordering INVERTS (the "faster settle" arms actually settle
// SLOWER once measured at 0.5C, because the aggressive arms have higher
// steady-state RMS and cross a loose band early, then take longer to truly
// converge). 0.5C is not this file's own choice; it is production's own
// band, now available via firing_score.h's FIRING_SCORE_SETTLE_BAND_C.
#define SETTLE_BAND_C FIRING_SCORE_SETTLE_BAND_C

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
// 2026-09-13 re-score: ramp rate is now a PARAMETER, not a #define. The opus
// review appended to docs/audits/fuzzy_overshoot_measurement_2026-09-13.md
// (Finding 5) found arm separation between bands/strengths appears ONLY at
// 100 degC/hr and above -- at 25/50 degC/hr the arms converge and a
// single-rate report would wrongly read as "no effect". main() now runs the
// full arm/objective sweep at two rates so that dependency is visible rather
// than hidden behind one hardcoded value.
#define RAMP_RATE_100_C_PER_HR (100.0f / 3600.0f)
#define RAMP_RATE_300_C_PER_HR (300.0f / 3600.0f)
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
    float undershoot_entry_windowed_c; /* the OTHER definition, added
                                * 2026-09-13 once FIRING_SUBSCORE_ENTRY_
                                * UNDERSHOOT_C landed (d41da85f): production's
                                * own undershoot instrument, unlike the one
                                * this file shipped with, is NOT unwindowed --
                                * it is the trough of (actual-target) WITHIN
                                * entry_window_s only, same window as
                                * overshoot, reported as a positive magnitude
                                * (0 if it never went negative in that
                                * window). Reported alongside
                                * undershoot_signed_c so the two disagree
                                * visibly rather than silently: a recovery
                                * after entry_window_s elapses is invisible
                                * here but not to undershoot_signed_c. */
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

// error_band_c/rate_band_c_per_s are now explicit PARAMETERS (2026-09-13
// re-score) instead of always being internally derived via
// pid_fuzzy_derive_bands() -- the derivation is still exercised (see main(),
// which calls it once to compute the "derived" arms' band values), but this
// function itself is agnostic to where the caller's bands came from, so it
// can run BOTH the derived bands and the absolute ERROR_BAND_C_DEFAULT/
// RATE_BAND_C_PER_S_DEFAULT (20.0/0.5) through the exact same scenario and
// tick wiring -- the comparison this task asked for that the file's
// previous revision could not make (it always derived internally, with no
// way to select the absolute constants instead).
static double run_overshoot_scenario(uint8_t strength_pct, float base_kp, float base_ki, float base_kd,
                                     float error_band_c, float rate_band_c_per_s,
                                     float ramp_rate_c_per_s,
                                     dwell_result_t out_results[N_PHASES], bool *out_bitexact_ok)
{
    sim_plant_cfg_t pcfg;
    make_plant_cfg(&pcfg);
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &pcfg);

    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    float entry_window_s = g_dead_time_s[0] + 2.0f * g_tau_s[0];

    float target_c = AMBIENT_C;
    bool all_bitexact = true;
    double iae = 0.0;

    for (int p = 0; p < N_PHASES; p++) {
        float dwell_target = PHASES[p].dwell_target_c;
        float ramp_dir = (dwell_target > target_c) ? 1.0f : -1.0f;

        // Ramp phase: step target_c toward dwell_target at ramp_rate_c_per_s
        // until reached. Tracks objective 1 (ramp-lag, FIRING_SUBSCORE_LAG_S)
        // -- entry-window/overshoot tracking still starts only once the
        // dwell itself begins, exactly as firing_score_seg_begin()/
        // classify()'s FIRING_SEG_DWELL-vs-ramp split does.
        int ramp_lag_n = 0;
        while ((ramp_dir > 0.0f && target_c < dwell_target) ||
               (ramp_dir < 0.0f && target_c > dwell_target)) {
            target_c += ramp_dir * ramp_rate_c_per_s * DT_S;
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
            // rate_c_per_s, using the COMMANDED rate (ramp_rate_c_per_s),
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
                g_ramp_lag_buf[ramp_lag_n] = fabsf(error_c) / ramp_rate_c_per_s;
                g_ramp_lag_signed_buf[ramp_lag_n] = error_c / (ramp_dir * ramp_rate_c_per_s);
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
        float min_actual_minus_target_entry = 0.0f; /* entry-window-only mirror,
                                                      * production's own
                                                      * FIRING_SUBSCORE_ENTRY_
                                                      * UNDERSHOOT_C definition */
        double last_outside_s = -1.0; /* FIRING_SUBSCORE_SETTLE_S's own
                                       * definition (firing_score.c
                                       * firing_score_seg_tick()): elapsed
                                       * time of the LAST scored tick seen
                                       * outside SETTLE_BAND_C, across the
                                       * WHOLE dwell (entry+steady) -- moves
                                       * forward again if the zone leaves the
                                       * band after re-entering, so a later
                                       * excursion is not hidden by an
                                       * earlier settle. -1 here (converted to
                                       * 0 below) means never seen outside. */
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
                if (actual_minus_target < min_actual_minus_target_entry) {
                    min_actual_minus_target_entry = actual_minus_target;
                }
            } else {
                // FIRING_SUBSCORE_STEADY_RMS_C's else-branch, exactly.
                res->steady_seen = true;
                steady_sumsq += (double)actual_minus_target * (double)actual_minus_target;
                steady_n++;
            }

            // FIRING_SUBSCORE_SETTLE_S's own algorithm, exactly (firing_score.c
            // firing_score_seg_tick()): record elapsed_s whenever OUTSIDE the
            // band; never reset it back down on re-entry. If never outside,
            // this stays at the sentinel and is reported as 0.0 (instant
            // settle), same as production.
            if (fabsf(actual_minus_target) > SETTLE_BAND_C) {
                last_outside_s = elapsed_s;
            }
        }
        // -1 sentinel (assertion 4 below) means "recorded as never settling
        // AT ALL inside this dwell's own DWELL_TICKS window" -- distinct from
        // production's 0.0 "never seen outside" case, which this file maps
        // onto settle_ticks=0 (elapsed_s of the first tick) rather than -1,
        // since a same-tick settle is a real, reportable result, not a
        // scenario-sizing failure.
        if (last_outside_s < 0.0) {
            res->settle_ticks = 0; /* first tick is already inside SETTLE_BAND_C
                                    * for the whole dwell -- report as settled
                                    * immediately (elapsed_s ~ DT_S), never as
                                    * the -1 "never settled" sentinel. */
        } else if (last_outside_s >= (double)DWELL_TICKS * (double)DT_S) {
            res->settle_ticks = -1; /* still outside on the last scored tick --
                                     * production's own worst-possible reading,
                                     * this file's own -1 "never settled"
                                     * convention (see assertion 4). */
        } else {
            res->settle_ticks = (int)((last_outside_s + (double)DT_S) / (double)DT_S);
        }
        res->steady_rms_c = (steady_n > 0) ? (float)sqrt(steady_sumsq / (double)steady_n) : 0.0f;
        res->undershoot_signed_c = min_actual_minus_target; /* <= 0.0; 0.0 means never dipped below */
        res->undershoot_entry_windowed_c =
            (min_actual_minus_target_entry < 0.0f) ? -min_actual_minus_target_entry : 0.0f;
    }

    *out_bitexact_ok = all_bitexact;
    return iae;
}

// 2026-09-13 re-score: the "fixed_retune_equivalent" arm this file shipped
// with was REMOVED, not kept. An opus review (appended to docs/audits/
// fuzzy_overshoot_measurement_2026-09-13.md, Finding 1) proved it applied
// roughly TWICE fuzzy_50's own measured ramp-phase-average gain perturbation
// (it used the rule table's CENTRE-CELL multiplier -- fuzzy_50's per-tick
// MAXIMUM, not its typical effect during a ramp) -- so its "fuzzy regresses
// overshoot/undershoot vs a plain retune" finding was an artifact of an
// unfairly strong comparison arm, not a property of fuzzy inference. This
// task's brief does not ask for a retune arm; the real open question this
// task exists to answer -- derived bands vs absolute bands, both through
// REAL fuzzy inference -- needs no retune arm to answer it, so it is not
// reintroduced here.
static const char *ARM_NAMES[] = { "fuzzy_off (strength=0)", "absolute_50 (20.0/0.5)",
                                    "derived_50 (autotune bands)", "absolute_25 (20.0/0.5)",
                                    "derived_25 (autotune bands)" };
#define N_ARMS (int)(sizeof(ARM_NAMES) / sizeof(ARM_NAMES[0]))

int main(void)
{
    printf("=== sim_fuzzy_overshoot -- re-measuring the fuzzy layer on OVERSHOOT, not IAE/MAE ===\n");
    printf("See this file's top comment before reading anything below: single-zone,\n"
           "bench-scale (max ~40C above ambient vs a real kiln's ~1200C), no synthetic\n"
           "disturbance injection -- ordinary ramp-to-dwell transitions only.\n"
           "Primary sweep ramp rate: 100 degC/hr (arm separation was found by review to appear only\n"
           "at 100 degC/hr and above -- a second, compact pass at 300 degC/hr runs later in this\n"
           "report to check rate-dependence explicitly).\n\n");

    #define BASE_KP 0.0318f
    #define BASE_KI 0.0001f
    #define BASE_KD 0.8401f

    float derived_error_band_c, derived_rate_band_c_per_s;
    bool bands_ok = pid_fuzzy_derive_bands(g_k_dc[0], g_tau_s[0], &derived_error_band_c,
                                            &derived_rate_band_c_per_s);
    #define ABSOLUTE_ERROR_BAND_C 20.0f     /* pid_fuzzy.c ERROR_BAND_C_DEFAULT, quoted literally --
                                             * this file has no access to that #define (it is
                                             * pid_fuzzy.c-local), so it is restated here and this
                                             * comment is the citation. */
    #define ABSOLUTE_RATE_BAND_C_PER_S 0.5f /* pid_fuzzy.c RATE_BAND_C_PER_S_DEFAULT, same note. */
    printf("Bands: derived (zone 0 autotune model) error_band_c=%.2f, rate_band_c_per_s=%.4f "
           "(from_model=%s); absolute (pid_fuzzy.c defaults) error_band_c=%.1f, "
           "rate_band_c_per_s=%.2f\n\n",
           (double)derived_error_band_c, (double)derived_rate_band_c_per_s, bands_ok ? "yes" : "NO (fell back!)",
           (double)ABSOLUTE_ERROR_BAND_C, (double)ABSOLUTE_RATE_BAND_C_PER_S);

    bool overall_ok = true;

    // Five arms: fuzzy_off (the safety-contract control), then absolute vs
    // derived bands at strength 50 and 25 -- the actual question this task
    // (docs/audits/derived_bands_four_objective_score_2026-09-13.md) exists
    // to answer. NO fixed-gain retune arm here (see ARM_NAMES's comment
    // above): this task's brief does not ask for one, and the prior
    // revision's retune arm was shown by review to be mislabelled/confounded
    // (roughly 2x fuzzy_50's own ramp-phase gain perturbation), so it would
    // add noise, not signal, to the derived-vs-absolute question.
    struct { uint8_t strength; float kp, ki, kd; float error_band_c, rate_band_c_per_s; } arms[N_ARMS] = {
        { 0,  BASE_KP, BASE_KI, BASE_KD, ABSOLUTE_ERROR_BAND_C, ABSOLUTE_RATE_BAND_C_PER_S }, /* bands unused at strength=0 */
        { 50, BASE_KP, BASE_KI, BASE_KD, ABSOLUTE_ERROR_BAND_C, ABSOLUTE_RATE_BAND_C_PER_S },
        { 50, BASE_KP, BASE_KI, BASE_KD, derived_error_band_c, derived_rate_band_c_per_s },
        { 25, BASE_KP, BASE_KI, BASE_KD, ABSOLUTE_ERROR_BAND_C, ABSOLUTE_RATE_BAND_C_PER_S },
        { 25, BASE_KP, BASE_KI, BASE_KD, derived_error_band_c, derived_rate_band_c_per_s },
    };

    // Four independent metric tables, one per objective -- NEVER reduced to
    // one score (the owner's explicit correction: IAE/MAE's flaw is not
    // "wrong metric," it's "collapses four objectives into one number in
    // which they can trade against each other invisibly"). Plus a fifth,
    // non-objective table (undershoot_entry_windowed) reported ONLY to show
    // where this file's own instrument disagrees with production's
    // FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C, per this task's own instruction to
    // surface that disagreement rather than silently pick one.
    //   ramp_lag_median_s (unsigned, ==FIRING_SUBSCORE_LAG_S)  -- objective 1
    //   ramp_lag_signed_median_s (~=FIRING_SUBSCORE_LAG_SIGNED_S, unfiltered) -- objective 1, signed
    //   settle_ticks*DT_S (==FIRING_SUBSCORE_SETTLE_S's own algorithm, 0.5C band) -- objective 2
    //   steady_rms_c (==FIRING_SUBSCORE_STEADY_RMS_C)          -- objective 3
    //   overshoot_c (==FIRING_SUBSCORE_ENTRY_PEAK_C)           -- objective 4a
    //   undershoot_signed_c (this file's own, unwindowed/unclamped -- see disagreement note) -- objective 4b
    //   undershoot_entry_windowed_c (==FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C, windowed/clamped)  -- disagreement only
    float lag_by_arm[N_ARMS][N_PHASES];
    float lag_signed_by_arm[N_ARMS][N_PHASES];
    float settle_s_by_arm[N_ARMS][N_PHASES];
    float steady_rms_by_arm[N_ARMS][N_PHASES];
    float overshoot_by_arm[N_ARMS][N_PHASES];
    float undershoot_signed_by_arm[N_ARMS][N_PHASES];
    float undershoot_entry_windowed_by_arm[N_ARMS][N_PHASES];

    for (int a = 0; a < N_ARMS; a++) {
        printf("-- arm: %s (kp=%.5f ki=%.5f kd=%.5f, error_band_c=%.2f rate_band_c_per_s=%.4f) --\n",
               ARM_NAMES[a], (double)arms[a].kp, (double)arms[a].ki, (double)arms[a].kd,
               (double)arms[a].error_band_c, (double)arms[a].rate_band_c_per_s);
        dwell_result_t results[N_PHASES];
        bool bitexact_ok;
        double iae = run_overshoot_scenario(arms[a].strength, arms[a].kp, arms[a].ki, arms[a].kd,
                                            arms[a].error_band_c, arms[a].rate_band_c_per_s,
                                            RAMP_RATE_100_C_PER_HR,
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
            undershoot_entry_windowed_by_arm[a][p] = r->undershoot_entry_windowed_c;
            printf("  dwell %d (target=%.1fC): [1]ramp_lag_median=%.1fs (signed=%+.1fs)  [2]settle=%s  "
                   "[3]steady_rms=%.3fC  [4a]overshoot=%.3fC [4b]undershoot(whole-dwell)=%+.3fC "
                   "undershoot(entry-window)=%.3fC  (entry_seen=%s steady_seen=%s)\n",
                   p, (double)PHASES[p].dwell_target_c, (double)r->ramp_lag_median_s,
                   (double)r->ramp_lag_signed_median_s,
                   r->settle_ticks >= 0 ? "yes" : "NEVER",
                   (double)r->steady_rms_c, (double)r->overshoot_c, (double)r->undershoot_signed_c,
                   (double)r->undershoot_entry_windowed_c,
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
    // materiality line, absolute-band arms and derived-band arms reported
    // SEPARATELY at both strengths so a strength-25 and strength-50 trade is
    // never averaged away. Materiality line for lag/settle is stated in
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
        int exclude_dwell; /* -1 = none. Set to 2 for objective 4a: the opus
                            * review (docs/audits/fuzzy_overshoot_measurement_
                            * 2026-09-13.md, Finding 3) found dwell 2's
                            * "overshoot" is a RAMP-DOWN RESIDUAL (the
                            * measurement is still above target when a
                            * ramp-down dwell begins), the exact mirror of the
                            * artifact this file already disclosed for
                            * undershoot at dwells 0/1 -- not real overshoot,
                            * and the review found the previous revision's
                            * headline "max fuzzy overshoot delta" figure came
                            * entirely from this artifact. Excluded from the
                            * max|diff| aggregation below; the raw per-dwell
                            * row is still printed with an explicit flag. */
    } metric_t;
    metric_t metrics[] = {
        { "[1] ramp-lag, UNSIGNED median (==FIRING_SUBSCORE_LAG_S)", lag_by_arm, TIME_MATERIALITY_S, "s", -1 },
        { "[1] ramp-lag, SIGNED median (~=FIRING_SUBSCORE_LAG_SIGNED_S, unfiltered; +=lagging, -=leading)",
          lag_signed_by_arm, TIME_MATERIALITY_S, "s", -1 },
        { "[2] settle time (==FIRING_SUBSCORE_SETTLE_S's own algorithm, 0.5C band)",
          settle_s_by_arm, TIME_MATERIALITY_S, "s", -1 },
        { "[3] steady-state RMS error (==FIRING_SUBSCORE_STEADY_RMS_C)", steady_rms_by_arm, 0.5f, "C", -1 },
        { "[4a] overshoot, entry peak (==FIRING_SUBSCORE_ENTRY_PEAK_C) -- dwell 2 EXCLUDED, ramp-down "
          "residual, not overshoot", overshoot_by_arm, 0.5f, "C", 2 },
        { "[4b] undershoot, SIGNED whole-dwell peak (this file's own, unwindowed -- see disagreement "
          "note below)", undershoot_signed_by_arm, 0.5f, "C", -1 },
        { "[disagreement only] undershoot, entry-window-only (==FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C, "
          "clamped+windowed)", undershoot_entry_windowed_by_arm, 0.5f, "C", -1 },
    };
    #define N_METRICS (int)(sizeof(metrics) / sizeof(metrics[0]))

    printf("=== Per-objective comparison vs fuzzy_off (never pooled into one score) ===\n");
    printf("CAVEAT on [1]'s two rows: FIRING_SUBSCORE_LAG_S -- which the unsigned row exactly\n"
           "reproduces -- is UNSIGNED (a leading and a lagging tick score identically) and, IN\n"
           "FIRING_SCORE.C ITSELF, drops saturated-and-short ticks (exactly the worst-tracking\n"
           "ticks). This file's own ramp loop applies no such filter, so that second blind spot\n"
           "does not carry over to either row here.\n"
           "DISAGREEMENT on [4b] vs the last row: this file's own undershoot_signed_c is unwindowed\n"
           "(whole dwell, never suppressed by recovery); production's FIRING_SUBSCORE_ENTRY_\n"
           "UNDERSHOOT_C is windowed to entry_window_s and clamped to >=0, same shape as overshoot.\n"
           "Both are reported; see the doc for which one this task's verdict is built on and why.\n");
    for (int m = 0; m < N_METRICS; m++) {
        printf("\n-- %s (bar: %.1f%s) --\n", metrics[m].name, (double)metrics[m].bar, metrics[m].unit);
        float max_50 = 0.0f, max_25 = 0.0f, max_derived_vs_absolute = 0.0f;
        for (int p = 0; p < N_PHASES; p++) {
            float off = metrics[m].by_arm[0][p];
            printf("  dwell %d%s: off=%.2f%s  abs50=%.2f%s (d=%+.2f)  der50=%.2f%s (d=%+.2f)  "
                   "abs25=%.2f%s (d=%+.2f)  der25=%.2f%s (d=%+.2f)\n",
                   p, (p == metrics[m].exclude_dwell) ? " [EXCLUDED: ramp-down residual, not real "
                   "overshoot -- see metric name]" : "",
                   (double)off, metrics[m].unit,
                   (double)metrics[m].by_arm[1][p], metrics[m].unit, (double)(metrics[m].by_arm[1][p] - off),
                   (double)metrics[m].by_arm[2][p], metrics[m].unit, (double)(metrics[m].by_arm[2][p] - off),
                   (double)metrics[m].by_arm[3][p], metrics[m].unit, (double)(metrics[m].by_arm[3][p] - off),
                   (double)metrics[m].by_arm[4][p], metrics[m].unit, (double)(metrics[m].by_arm[4][p] - off));
            if (p == metrics[m].exclude_dwell) continue; /* not real overshoot -- see comment above */
            for (int a = 1; a < N_ARMS; a++) {
                float d = fabsf(metrics[m].by_arm[a][p] - off);
                if (a == 1 || a == 2) { if (d > max_50) max_50 = d; }
                if (a == 3 || a == 4) { if (d > max_25) max_25 = d; }
            }
            float d_derived_absolute_50 = fabsf(metrics[m].by_arm[2][p] - metrics[m].by_arm[1][p]);
            float d_derived_absolute_25 = fabsf(metrics[m].by_arm[4][p] - metrics[m].by_arm[3][p]);
            if (d_derived_absolute_50 > max_derived_vs_absolute) max_derived_vs_absolute = d_derived_absolute_50;
            if (d_derived_absolute_25 > max_derived_vs_absolute) max_derived_vs_absolute = d_derived_absolute_25;
        }
        printf("  max|diff| strength-50 arms vs off: %.2f%s -- %s bar\n", (double)max_50,
               metrics[m].unit, (max_50 > metrics[m].bar) ? "EXCEEDS" : "does not exceed");
        printf("  max|diff| strength-25 arms vs off: %.2f%s -- %s bar\n", (double)max_25,
               metrics[m].unit, (max_25 > metrics[m].bar) ? "EXCEEDS" : "does not exceed");
        printf("  max|diff| DERIVED vs ABSOLUTE bands (same strength): %.2f%s -- %s bar "
               "(THE question this task asks)\n", (double)max_derived_vs_absolute,
               metrics[m].unit, (max_derived_vs_absolute > metrics[m].bar) ? "EXCEEDS" : "does not exceed");
    }
    // 2026-09-13 addition, opus review Finding 5 (docs/audits/fuzzy_overshoot_
    // measurement_2026-09-13.md): arm separation between bands/strengths was
    // found to appear ONLY at 100 degC/hr and above -- at 25/50 degC/hr the
    // arms converge and a single-rate report would wrongly read as "no
    // effect". Everything above ran at 100 degC/hr, stated explicitly. This
    // second, compact pass reruns ONLY the two headline arms this task cares
    // about (absolute_50, derived_50) at 300 degC/hr (within
    // ZONE_MAX_RAMP_C_PER_HR_MAX) to show whether the derived-vs-absolute
    // gap grows, shrinks, or stays put as the ramp gets brisker -- not a
    // full second sweep of all five arms/seven metrics, to keep this
    // addition bounded.
    printf("\n=== Rate sensitivity: absolute_50 vs derived_50 at 300 degC/hr (vs. this report's "
           "primary 100 degC/hr) ===\n");
    {
        dwell_result_t res_abs300[N_PHASES], res_der300[N_PHASES];
        bool ok_abs300, ok_der300;
        run_overshoot_scenario(50, BASE_KP, BASE_KI, BASE_KD, ABSOLUTE_ERROR_BAND_C,
                               ABSOLUTE_RATE_BAND_C_PER_S, RAMP_RATE_300_C_PER_HR, res_abs300, &ok_abs300);
        run_overshoot_scenario(50, BASE_KP, BASE_KI, BASE_KD, derived_error_band_c,
                               derived_rate_band_c_per_s, RAMP_RATE_300_C_PER_HR, res_der300, &ok_der300);
        if (!ok_abs300 || !ok_der300) {
            printf("  (skipped bit-exact re-verification here; strength=50 on both, already covered "
                   "by the primary sweep's own assertion)\n");
        }
        float max_overshoot_d = 0.0f, max_undershoot_d = 0.0f, max_steady_d = 0.0f, max_settle_d = 0.0f;
        for (int p = 0; p < N_PHASES; p++) {
            if (p != 2) { /* dwell 2's overshoot is the same ramp-down-residual artifact here */
                float d = fabsf(res_der300[p].overshoot_c - res_abs300[p].overshoot_c);
                if (d > max_overshoot_d) max_overshoot_d = d;
            }
            float du = fabsf(res_der300[p].undershoot_signed_c - res_abs300[p].undershoot_signed_c);
            if (du > max_undershoot_d) max_undershoot_d = du;
            float ds = fabsf(res_der300[p].steady_rms_c - res_abs300[p].steady_rms_c);
            if (ds > max_steady_d) max_steady_d = ds;
            float abs_settle = (res_abs300[p].settle_ticks >= 0) ? (float)res_abs300[p].settle_ticks * DT_S : -1.0f;
            float der_settle = (res_der300[p].settle_ticks >= 0) ? (float)res_der300[p].settle_ticks * DT_S : -1.0f;
            if (abs_settle >= 0.0f && der_settle >= 0.0f) {
                float dt_settle = fabsf(der_settle - abs_settle);
                if (dt_settle > max_settle_d) max_settle_d = dt_settle;
            }
            printf("  dwell %d @300C/hr: abs50 overshoot=%.3fC undershoot=%+.3fC steady_rms=%.3fC "
                   "settle=%.0fs | der50 overshoot=%.3fC undershoot=%+.3fC steady_rms=%.3fC settle=%.0fs\n",
                   p, (double)res_abs300[p].overshoot_c, (double)res_abs300[p].undershoot_signed_c,
                   (double)res_abs300[p].steady_rms_c, (double)abs_settle,
                   (double)res_der300[p].overshoot_c, (double)res_der300[p].undershoot_signed_c,
                   (double)res_der300[p].steady_rms_c, (double)der_settle);
        }
        printf("  max|der-abs| @300C/hr: overshoot(excl dwell2)=%.2fC undershoot=%.2fC steady_rms=%.2fC "
               "settle=%.1fs -- overshoot/undershoot/steady %s 0.5C, settle %s 30s\n",
               (double)max_overshoot_d, (double)max_undershoot_d, (double)max_steady_d, (double)max_settle_d,
               (max_overshoot_d > 0.5f || max_undershoot_d > 0.5f || max_steady_d > 0.5f) ? "EXCEEDS" : "does not exceed",
               (max_settle_d > 30.0f) ? "EXCEEDS" : "does not exceed");
        printf("  Compare against this report's 100 degC/hr max|diff| DERIVED vs ABSOLUTE rows above: "
               "if the 300 degC/hr gap is materially larger, the derived-vs-absolute conclusion is "
               "RATE-DEPENDENT and must be reported as such, not as a single flat verdict.\n");
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
