// sim_strength_pct_adapt -- WI-10 (docs/SCENARIO_SIMULATION.md sec 6.2/7,
// docs/audits/strength_pct_adapter_design_2026-09-16.md).
//
// The DESIGN is the audit doc above; this file is its simulation arm. A
// cross-firing scalar hill-climb over `strength_pct`, adjudicated ONLY by
// the real, unmodified firing_compare()'s Bar 1 / Bar 2 / no-degradation
// verdict comparing one firing's own firing_score_set_t against the
// PREVIOUS (adopted) firing's -- never a within-run inference, never a
// quantity strength_pct itself moved inside the same firing. See the design
// doc sec 3 for the full non-circularity argument and sec 3.1 for the
// selection-bias mitigation this file implements (a refused firing counts
// as an implicit REJECT, never a silent no-op retry).
//
// WHAT IS REAL, WHAT IS ORCHESTRATION ONLY: pid.c, pid_fuzzy.c,
// pid_autotune.c, firing_score.c, firing_compare.c, sim_plant.c are ALL
// linked for real, unmodified -- same posture sim_scenarios.c's own header
// documents. Only the per-tick wiring and the adapter's own step logic
// (sec 2.2 of the design doc) are written here.
//
// SCOPE: S2, S5, S7, S12 only (the design doc/plan's own WI-10 line), single
// zone, plant and belief model FIXED for the whole 9-firing chain -- this
// arm never invokes adaptive_tune (that is WI-8's separate question; mixing
// the two would make it impossible to tell which mechanism moved a result).
//
// GATING POSTURE: this executable's exit code is non-zero ONLY for a
// harness defect (a named scenario missing from the table, a NaN outside
// this design's own refusal handling, or the strength_pct=0 bit-exact
// contract breaking). A chain that never accepts a step, oscillates, or
// wanders is a PRINTED, NON-FATAL finding -- exactly the design doc sec 5's
// stated posture and WI-10's own acceptance line ("If the simulation shows
// the adapter wandering or failing to converge, that is a complete and
// valuable result -- record it and stop").
//
// Usage: sim_strength_pct_adapt.exe (no arguments, always the full run)
#include "../drivers/control/pid.h"
#include "../drivers/control/pid_autotune.h"
#include "../drivers/control/pid_fuzzy.h"
#include "../drivers/control/firing_score.h"
#include "../drivers/control/firing_compare.h"
#include "sim_plant.h"
#include "sim_scenario_table.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DT_S 1.0f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f
#define N_FIRINGS 9

// Design doc sec 5: starting strength/step/floor.
#define START_STRENGTH_PCT 50.0f
#define START_STEP_PCT 20.0f
#define STEP_FLOOR_PCT 5.0f
#define MAX_INSUFFICIENT_RETRIES 2 // design doc sec 2.2 step 5: retry the same
                                   // direction/magnitude this many times before
                                   // halving anyway, so a scenario whose
                                   // matched classes rarely recur cannot stall
                                   // the chain forever.

static const char *const SCOPE_IDS[] = {"S2_SENSOR_NEAR_ELEMENT", "S5_MASS_HEAVY", "S7_TUNE_HOT",
                                        "S12_COMPOUND_WORST"};
#define SCOPE_COUNT (int)(sizeof(SCOPE_IDS) / sizeof(SCOPE_IDS[0]))

static const sim_scenario_t *find_scenario(const sim_scenario_t *table, int n, const char *id)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(table[i].id, id) == 0) return &table[i];
    }
    return NULL;
}

typedef struct {
    bool ok;
    bool refused;
    char reason[160];
    firing_score_set_t score;
} one_firing_result_t;

// Runs exactly one firing at the given fixed strength_pct against the
// scenario's own (fixed, never refit) model_k_dc/tau_s/dead_time_s belief,
// scoring it into a firing_score_set_t via the real firing_score.c seam --
// same ramp/dwell/ramp/dwell profile shape sim_scenarios.c/sim_scenarios_
// adaptive.c both use, trimmed to only what this adapter needs (no
// adaptive_tune, no multiplier bookkeeping).
static void run_one_firing(const sim_scenario_t *sc, float strength_pct, one_firing_result_t *out)
{
    memset(out, 0, sizeof(*out));

    float error_band_c, rate_band_c_per_s;
    if (!pid_fuzzy_derive_bands(sc->model_k_dc, sc->model_tau_s, &error_band_c, &rate_band_c_per_s)) {
        out->refused = true;
        snprintf(out->reason, sizeof(out->reason), "pid_fuzzy_derive_bands() refused");
        return;
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
        out->refused = true;
        snprintf(out->reason, sizeof(out->reason), "pid_autotune_tune_from_fopdt() refused: %s", gains.refusal_reason);
        return;
    }
    float base_kp = gains.kp, base_ki = gains.ki, base_kd = gains.kd;

    sim_plant_cfg_t plant_cfg = sc->plant;
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &plant_cfg);
    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;
    bool three_node = (plant_cfg.node_model == SIM_NODE_THREE);
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
    if (dwell_ticks > 20000) dwell_ticks = 20000;

    struct { bool is_ramp; float rate_c_per_hr_signed; float target_start, target_end; int ticks; } segs[4] = {
        { true,  sc->ramp_rate_c_per_hr, ambient, t1, ramp1_ticks },
        { false, 0.0f,                   t1, t1, dwell_ticks },
        { true,  sc->ramp_rate_c_per_hr, t1, t2, ramp2_ticks },
        { false, 0.0f,                   t2, t2, dwell_ticks },
    };

    bool nan_seen = false;
    float ceiling_c = ambient + sc->model_k_dc + 400.0f;
    float floor_c = ambient - 1.0f;
    bool bounds_ok = true;

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
            pid_fuzzy_adjust(error_c, rate_meas, error_band_c, rate_band_c_per_s, base_kp, base_ki, base_kd,
                             strength_pct, &adj_kp, &adj_ki, &adj_kd);
            if (strength_pct == 0.0f && !(adj_kp == base_kp && adj_ki == base_ki && adj_kd == base_kd)) {
                out->refused = true;
                snprintf(out->reason, sizeof(out->reason), "strength_pct=0 bit-exact contract broke mid-firing");
                return;
            }

            pid_rescale_integral_for_new_ki(&pid_state, prev_effective_ki, adj_ki);
            prev_effective_ki = adj_ki;

            pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
            float duty = pid_update(&pid_state, &cfg, target_c, pstate.sensor_c, DT_S, 0.0f, 0.0f);

            if (three_node) {
                sim_plant_three_node_step(&pstate, &plant_cfg, duty, DT_S);
            } else {
                sim_plant_step(&pstate, &plant_cfg, duty, DT_S);
            }

            if (isnan(pstate.sensor_c) || isnan(pstate.element_c)) nan_seen = true;
            if (pstate.sensor_c < floor_c || pstate.sensor_c > ceiling_c) bounds_ok = false;

            bool saturated_high = duty >= 0.98f;
            firing_score_seg_tick(&seg, &zone_captured, target_c, pstate.sensor_c, saturated_high, DT_S);
        }

        // Not fatal if a segment is too short to score (firing_score_set_
        // finish_segment() itself records dropped_short) -- the adapter's
        // own NO_MATCHED_PAIRS handling (sec 3.1) is exactly for this case.
        firing_score_set_finish_segment(&out->score, &seg);

        if (nan_seen || !bounds_ok) break; // stop the firing early, same as sim_scenarios.c's posture
    }

    if (nan_seen) {
        out->refused = true;
        snprintf(out->reason, sizeof(out->reason), "NaN observed in plant state");
        return;
    }
    if (!bounds_ok) {
        out->refused = true;
        snprintf(out->reason, sizeof(out->reason), "sensor reading left [%.1f, %.1f]", (double)floor_c, (double)ceiling_c);
        return;
    }
    out->ok = true;
}

typedef enum { TRACE_SETTLED, TRACE_OSCILLATED, TRACE_WANDERED, TRACE_TOO_SHORT } trace_class_t;

// Design doc sec 2.3: settled if the last 3 ADOPTED values sit within one
// step-floor of each other; oscillated if strength repeatedly reverses
// direction without settling; wandered otherwise (net drift, no settling,
// no consistent direction).
static trace_class_t classify_trace(const float *adopted, int n, int direction_flips)
{
    if (n < 3) return TRACE_TOO_SHORT;
    float lo = adopted[n - 1], hi = adopted[n - 1];
    for (int i = n - 3; i < n; i++) {
        if (adopted[i] < lo) lo = adopted[i];
        if (adopted[i] > hi) hi = adopted[i];
    }
    if ((hi - lo) <= STEP_FLOOR_PCT) return TRACE_SETTLED;
    if (direction_flips >= 2) return TRACE_OSCILLATED;
    return TRACE_WANDERED;
}

static const char *verdict_name(firing_compare_verdict_t v)
{
    switch (v) {
        case FIRING_COMPARE_ACCEPT: return "ACCEPT";
        case FIRING_COMPARE_REJECT_DEGRADED: return "REJECT_DEGRADED";
        case FIRING_COMPARE_INSUFFICIENT: return "INSUFFICIENT";
        case FIRING_COMPARE_NO_MATCHED_PAIRS: return "NO_MATCHED_PAIRS";
        default: return "?";
    }
}

// Runs the design doc's 9-firing chain for one scenario. Returns false only
// on a harness defect (see this file's own top comment); a non-converging
// or wandering trace is reported and returns true.
static bool run_chain(const sim_scenario_t *sc, bool *any_accept_out)
{
    printf("=== STRENGTH_ADAPT %s ===\n", sc->id);

    float current = START_STRENGTH_PCT;
    float step = START_STEP_PCT;
    int insufficient_streak = 0;

    one_firing_result_t baseline;
    run_one_firing(sc, current, &baseline);
    if (baseline.refused) {
        printf("SCENARIO_REFUSED %s: initial firing (strength=%.1f) refused: %s\n", sc->id, (double)current,
               baseline.reason);
        return false;
    }
    printf("STRENGTH_ADAPT\t%s\tfiring=1/%d\tstrength=%.1f\tverdict=%s\tadopted=%.1f\tstep=%.1f\n", sc->id, N_FIRINGS,
           (double)current, "INITIAL", (double)current, (double)step);

    float adopted_trace[N_FIRINGS];
    adopted_trace[0] = current;
    int direction_flips = 0;
    int prev_sign = 0; // sign of the last step actually taken (candidate - baseline)
    bool any_accept = false;

    for (int fi = 2; fi <= N_FIRINGS; fi++) {
        float candidate = current + step;
        if (candidate > 100.0f) candidate = 100.0f;
        if (candidate < 0.0f) candidate = 0.0f;

        one_firing_result_t trial;
        run_one_firing(sc, candidate, &trial);

        firing_compare_verdict_t verdict;
        bool refused_as_reject = false;
        if (trial.refused) {
            // Design doc sec 3.1's mitigation: a refused firing is an
            // IMPLICIT REJECT of the step that produced it, never a silent
            // no-op -- otherwise the retry-same-direction rule below would
            // walk the knob toward instability one refusal at a time with
            // no accept ever required.
            verdict = FIRING_COMPARE_REJECT_DEGRADED;
            refused_as_reject = true;
        } else {
            firing_compare_result_t result;
            verdict = firing_compare(&baseline.score, &trial.score, NULL, &result);
        }

        float adopted_after = current;
        switch (verdict) {
            case FIRING_COMPARE_ACCEPT:
                current = candidate;
                baseline = trial;
                adopted_after = current;
                insufficient_streak = 0;
                any_accept = true;
                {
                    int sign = (step > 0) ? 1 : -1;
                    if (prev_sign != 0 && sign != prev_sign) direction_flips++;
                    prev_sign = sign;
                }
                break;
            case FIRING_COMPARE_REJECT_DEGRADED:
                // Hard revert (design doc sec 2.2 step 6): current/baseline
                // are untouched. Flip direction, halve magnitude (floored).
                step = -step / 2.0f;
                if (fabsf(step) < STEP_FLOOR_PCT) step = (step < 0.0f) ? -STEP_FLOOR_PCT : STEP_FLOOR_PCT;
                insufficient_streak = 0;
                break;
            case FIRING_COMPARE_INSUFFICIENT:
            case FIRING_COMPARE_NO_MATCHED_PAIRS:
                // Retry same direction/magnitude up to MAX_INSUFFICIENT_
                // RETRIES, then halve anyway so a scenario whose matched
                // classes rarely recur cannot stall the chain forever.
                insufficient_streak++;
                if (insufficient_streak > MAX_INSUFFICIENT_RETRIES) {
                    float mag = fabsf(step) / 2.0f;
                    if (mag < STEP_FLOOR_PCT) mag = STEP_FLOOR_PCT;
                    step = (step < 0.0f) ? -mag : mag;
                    insufficient_streak = 0;
                }
                break;
        }

        adopted_trace[fi - 1] = adopted_after;
        printf("STRENGTH_ADAPT\t%s\tfiring=%d/%d\tstrength=%.1f\tverdict=%s%s\tadopted=%.1f\tstep=%.1f\n", sc->id, fi,
               N_FIRINGS, (double)candidate, verdict_name(verdict), refused_as_reject ? " (REFUSED_TREATED_AS_REJECT)" : "",
               (double)adopted_after, (double)step);
        if (refused_as_reject) {
            printf("STRENGTH_ADAPT_REFUSAL\t%s\tfiring=%d/%d\treason=%s\n", sc->id, fi, N_FIRINGS, trial.reason);
        }
        fflush(stdout);
    }

    trace_class_t cls = classify_trace(adopted_trace, N_FIRINGS, direction_flips);
    const char *cls_name = (cls == TRACE_SETTLED) ? "SETTLED" : (cls == TRACE_OSCILLATED) ? "OSCILLATED"
                          : (cls == TRACE_WANDERED) ? "WANDERED" : "TOO_SHORT";
    printf("STRENGTH_ADAPT_SUMMARY\t%s\tfinal_adopted=%.1f\tany_accept=%s\tdirection_flips=%d\tclass=%s\n\n", sc->id,
           (double)current, any_accept ? "yes" : "no", direction_flips, cls_name);

    if (any_accept_out) *any_accept_out = any_accept;
    return true;
}

int main(void)
{
    printf("=== sim_strength_pct_adapt (WI-10): strength_pct cross-firing adapter, design doc\n"
           "docs/audits/strength_pct_adapter_design_2026-09-16.md, simulation arm only ===\n");
    printf("Real production pid.c/pid_fuzzy.c/pid_autotune.c/firing_score.c/firing_compare.c linked directly. "
           "Adjudicated ONLY by firing_compare() comparing consecutive, already-finished firings' own "
           "firing_score_set_t -- never a within-run inference. A chain that never accepts, oscillates, or "
           "wanders is a printed, non-fatal finding, not a build failure: this executable's exit code reports "
           "harness defects only.\n\n");

    const sim_scenario_t *table = sim_scenario_table();
    int n = SIM_SCENARIO_COUNT;
    bool any_defect = false;
    bool any_accept_anywhere = false;

    for (int i = 0; i < SCOPE_COUNT; i++) {
        const sim_scenario_t *sc = find_scenario(table, n, SCOPE_IDS[i]);
        if (!sc) {
            printf("FAIL: scenario %s not found in sim_scenario_table() -- WI-10's scope names it explicitly.\n",
                   SCOPE_IDS[i]);
            any_defect = true;
            continue;
        }
        bool any_accept = false;
        if (!run_chain(sc, &any_accept)) {
            any_defect = true;
        } else if (any_accept) {
            any_accept_anywhere = true;
        }
    }

    printf("INFO: at least one (scenario, step) in this suite reached FIRING_COMPARE_ACCEPT: %s.\n",
           any_accept_anywhere ? "yes" : "no");
    printf("INFO: per design doc sec 5, this INFO line is not a pass/fail gate -- a suite where every chain\n"
           "stays INSUFFICIENT/NO_MATCHED_PAIRS/REJECT_DEGRADED is itself the honest WI-10 finding for this\n"
           "plant model, reported above per scenario, not papered over here.\n");

    if (any_defect) {
        printf("=== sim_strength_pct_adapt: FAIL (harness defect, see above -- NOT an adapter-convergence "
               "verdict) ===\n");
        return 1;
    }
    printf("=== sim_strength_pct_adapt: PASS (harness ran cleanly; see STRENGTH_ADAPT_SUMMARY lines above for "
           "each scenario's own convergence finding) ===\n");
    return 0;
}
