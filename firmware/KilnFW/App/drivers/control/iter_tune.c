// iter_tune.c -- ITER_TUNE_REDESIGN_PLAN.md step 5. See iter_tune.h for
// the design rationale, especially why the old whole-firing IAE path is
// deleted rather than kept alongside.

#include "iter_tune.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float *param_ptr(iter_tune_gains_t *g, uint8_t param)
{
    // kd is deliberately absent: derivative gain is the noise-sensitive
    // term and is out of scope for a blind perturbation with no plant
    // model backing it.
    return (param == ITER_TUNE_PARAM_KI) ? &g->ki : &g->kp;
}

static void stop(iter_tune_zone_state_t *state, iter_tune_status_t status)
{
    state->has_pending = false;
    state->enabled = false;
    state->status = (uint8_t)status;
}

bool iter_tune_enable(iter_tune_zone_state_t *state, iter_tune_gains_t current_gains)
{
    if (state->enabled) return false;
    if (state->status == ITER_TUNE_STATUS_CONVERGED || state->status == ITER_TUNE_STATUS_FAULTED) {
        // Sticky: a zone that stopped does not silently restart on a
        // re-enable. Moving it takes a deliberate re-anchor.
        return false;
    }
    if (!state->has_anchor) {
        // Owner decision 9.1: the FIRST enable snapshots the gains active at
        // that instant as this zone's commissioned anchor. There is no
        // separate manual capture step.
        state->anchor = current_gains;
        state->has_anchor = true;
    }
    if (!state->has_baseline) {
        state->baseline = current_gains;
        state->has_baseline = true;
    }
    state->enabled = true;
    state->status = (uint8_t)ITER_TUNE_STATUS_TUNING;
    state->param = ITER_TUNE_PARAM_KP;
    for (uint8_t p = 0; p < ITER_TUNE_PARAM_COUNT; p++) {
        state->step_frac[p] = ITER_TUNE_STEP_START;
        state->step_negative[p] = false;
        state->consec_accepts[p] = 0;
        state->consec_rejects[p] = 0;
        state->param_done[p] = 0;
    }
    state->trials_scored = 0;
    state->carries = 0;
    state->cage_edge_hits = 0;
    state->has_pending = false;
    return true;
}

void iter_tune_reanchor(iter_tune_zone_state_t *state, iter_tune_gains_t new_anchor)
{
    state->anchor = new_anchor;
    state->has_anchor = true;
    state->baseline = new_anchor;
    state->has_baseline = true;
    state->has_pending = false;
    state->param = ITER_TUNE_PARAM_KP;
    for (uint8_t p = 0; p < ITER_TUNE_PARAM_COUNT; p++) {
        state->step_frac[p] = ITER_TUNE_STEP_START;
        state->step_negative[p] = false;
        state->consec_accepts[p] = 0;
        state->consec_rejects[p] = 0;
        state->param_done[p] = 0;
    }
    state->trials_scored = 0;
    state->carries = 0;
    state->cage_edge_hits = 0;
    if (state->enabled) state->status = (uint8_t)ITER_TUNE_STATUS_TUNING;
}

iter_tune_gains_t iter_tune_restore_commissioned(iter_tune_zone_state_t *state)
{
    iter_tune_gains_t g = state->has_anchor ? state->anchor : state->baseline;
    state->has_pending = false;
    state->baseline = g;
    state->enabled = false;
    state->status = (uint8_t)ITER_TUNE_STATUS_OFF;
    return g;
}

void iter_tune_fault(iter_tune_zone_state_t *state)
{
    stop(state, ITER_TUNE_STATUS_FAULTED);
}

iter_tune_gains_t iter_tune_active_gains(const iter_tune_zone_state_t *state)
{
    if (state->has_pending) return state->pending_gains;
    return state->baseline;
}

iter_tune_gains_t iter_tune_clamp_to_cage(const iter_tune_zone_state_t *state, iter_tune_gains_t g,
                                          bool *out_hit_edge)
{
    bool hit = false;
    iter_tune_gains_t in = g;
    const iter_tune_gains_t *a = &state->anchor;

    for (uint8_t p = 0; p < ITER_TUNE_PARAM_COUNT; p++) {
        iter_tune_gains_t anchor_copy = *a;
        float anchor_v = *param_ptr(&anchor_copy, p);
        float lo = ITER_TUNE_GAIN_FLOOR_C;
        float hi = ITER_TUNE_GAIN_CEIL_C;
        if (state->has_anchor) {
            float cage_lo = anchor_v * ITER_TUNE_CAGE_LOW_FACTOR;
            float cage_hi = anchor_v * ITER_TUNE_CAGE_HIGH_FACTOR;
            if (cage_lo > lo) lo = cage_lo;
            if (cage_hi < hi) hi = cage_hi;
        }
        float *v = param_ptr(&g, p);
        float clamped = clampf(*v, lo, hi);
        if (clamped != *v) hit = true;
        *v = clamped;
    }
    g.kd = in.kd; // never touched
    if (out_hit_edge) *out_hit_edge = hit;
    return g;
}

bool iter_tune_propose_perturbation(iter_tune_zone_state_t *state, iter_tune_gains_t *out_gains)
{
    if (!state->enabled || !state->has_baseline || !state->has_anchor || state->has_pending) return false;
    if (state->status != ITER_TUNE_STATUS_TUNING) return false;

    // Skip a parameter that is already exhausted, so the remaining budget
    // goes to the one that might still be worth something.
    for (uint8_t tries = 0; tries < ITER_TUNE_PARAM_COUNT && state->param_done[state->param]; tries++) {
        state->param = (uint8_t)((state->param + 1u) % ITER_TUNE_PARAM_COUNT);
    }
    if (state->param_done[state->param]) {
        stop(state, ITER_TUNE_STATUS_CONVERGED);
        return false;
    }

    iter_tune_gains_t g = state->baseline;
    uint8_t pi = state->param;
    float dir = state->step_negative[pi] ? -1.0f : 1.0f;
    float *v = param_ptr(&g, pi);
    *v = *v * (1.0f + dir * state->step_frac[pi]);

    bool hit_edge = false;
    g = iter_tune_clamp_to_cage(state, g, &hit_edge);
    if (hit_edge) {
        state->cage_edge_hits++;
        if (state->cage_edge_hits >= ITER_TUNE_MAX_CAGE_EDGE_HITS) {
            stop(state, ITER_TUNE_STATUS_CONVERGED);
            return false;
        }
    }

    // A clamp that produced no movement at all is not a trial: proposing it
    // would burn a firing measuring the baseline against itself.
    iter_tune_gains_t base = state->baseline;
    if (*param_ptr(&g, state->param) == *param_ptr(&base, state->param)) return false;

    state->pending_gains = g;
    state->has_pending = true;
    state->carries = 0;
    if (out_gains) *out_gains = g;
    return true;
}

// Coordinate descent: cycle kp -> ki -> kp ... Called after every scored
// trial so neither parameter monopolises the budget.
static void advance_param(iter_tune_zone_state_t *state)
{
    state->param = (uint8_t)((state->param + 1u) % ITER_TUNE_PARAM_COUNT);
}

iter_tune_result_t iter_tune_process_comparison(iter_tune_zone_state_t *state,
                                                const firing_compare_result_t *cmp, char *reason,
                                                size_t reason_len)
{
    if (reason && reason_len) reason[0] = '\0';

    if (!state->enabled || state->status != ITER_TUNE_STATUS_TUNING) {
        if (reason && reason_len) snprintf(reason, reason_len, "iter_tune not tuning this zone");
        return ITER_TUNE_RESULT_DISABLED;
    }
    if (!state->has_pending) {
        if (reason && reason_len) snprintf(reason, reason_len, "no trial pending -- nothing judged");
        return ITER_TUNE_RESULT_NO_TRIAL;
    }

    if (!cmp || cmp->verdict == FIRING_COMPARE_NO_MATCHED_PAIRS) {
        // n == 0 is a first-class outcome, not an error: this firing pair
        // has nothing to say. Keep the trial armed for the next firing --
        // but only so many times, so a stale trial cannot ride indefinitely
        // against a moving plant.
        state->carries++;
        if (state->carries > ITER_TUNE_MAX_CARRIES) {
            state->has_pending = false;
            state->carries = 0;
            if (reason && reason_len) {
                snprintf(reason, reason_len, "no matched pairs %d times -- trial discarded unscored",
                         ITER_TUNE_MAX_CARRIES + 1);
            }
            return ITER_TUNE_RESULT_CARRY_EXHAUSTED;
        }
        if (reason && reason_len) {
            snprintf(reason, reason_len, "no matched segment classes -- trial carried (%u/%u)",
                     (unsigned)state->carries, (unsigned)ITER_TUNE_MAX_CARRIES);
        }
        return ITER_TUNE_RESULT_CARRIED;
    }

    state->trials_scored++;
    state->carries = 0;
    uint8_t pi = state->param;
    iter_tune_result_t result;

    if (cmp->verdict == FIRING_COMPARE_ACCEPT) {
        state->baseline = state->pending_gains; // gains move to the trial's
        state->has_pending = false;
        state->consec_accepts[pi]++;
        state->consec_rejects[pi] = 0;
        if (state->consec_accepts[pi] >= 2) {
            // Plan sec 4 caps accept-driven growth at 20%; a step already
            // grown past that by the probe path below keeps what it has
            // rather than being pulled back down mid-search.
            float grown = state->step_frac[pi] * 2.0f;
            float cap = (state->step_frac[pi] > ITER_TUNE_STEP_MAX) ? ITER_TUNE_STEP_PROBE_MAX
                                                                   : ITER_TUNE_STEP_MAX;
            state->step_frac[pi] = (grown > cap) ? cap : grown;
            state->consec_accepts[pi] = 0;
        }
        if (reason && reason_len) {
            snprintf(reason, reason_len, "accepted: composite %.3f floors, step now %.1f%%",
                     (double)cmp->composite_normalised, (double)(state->step_frac[pi] * 100.0f));
        }
        result = ITER_TUNE_RESULT_ACCEPTED;
    } else if (cmp->verdict == FIRING_COMPARE_REJECT_DEGRADED) {
        // Measurable, and bad. Exact revert (a flag clear, never an
        // arithmetic undo), reverse direction, and take smaller bites.
        state->has_pending = false;
        state->consec_rejects[pi]++;
        state->consec_accepts[pi] = 0;
        state->step_negative[pi] = !state->step_negative[pi];
        if (state->consec_rejects[pi] >= 2) {
            state->step_frac[pi] = state->step_frac[pi] * 0.5f;
            state->consec_rejects[pi] = 0;
            if (state->step_frac[pi] < ITER_TUNE_STEP_MIN) state->param_done[pi] = 1;
        }
        if (reason && reason_len) {
            snprintf(reason, reason_len, "rejected (degraded): composite %.3f floors, step now %.1f%%",
                     (double)cmp->composite_normalised, (double)(state->step_frac[pi] * 100.0f));
        }
        result = ITER_TUNE_RESULT_REVERTED;
    } else {
        // INSUFFICIENT: nothing cleared the owner's floor, i.e. the change
        // was too small to MEASURE. See iter_tune.h's step-schedule comment
        // for why this GROWS the step instead of halving it.
        state->has_pending = false;
        state->consec_accepts[pi] = 0;
        state->consec_rejects[pi] = 0;
        if (state->step_frac[pi] < ITER_TUNE_STEP_PROBE_MAX) {
            float grown = state->step_frac[pi] * 2.0f;
            state->step_frac[pi] = (grown > ITER_TUNE_STEP_PROBE_MAX) ? ITER_TUNE_STEP_PROBE_MAX : grown;
        } else if (!state->step_negative[pi]) {
            // At the probe cap with nothing to show: try the other direction
            // before giving up on this parameter.
            state->step_negative[pi] = true;
        } else {
            // Both directions probed at the cap and still unmeasurable: this
            // parameter has nothing to offer on this plant.
            state->param_done[pi] = 1;
        }
        if (reason && reason_len) {
            snprintf(reason, reason_len,
                     "reverted (insufficient evidence): composite %.3f floors, step now %.1f%%",
                     (double)cmp->composite_normalised, (double)(state->step_frac[pi] * 100.0f));
        }
        result = ITER_TUNE_RESULT_REVERTED;
    }

    // Coordinate descent, plan sec 4: cycle kp -> ki after EVERY scored
    // trial, so neither parameter can monopolise a six-trial budget.
    advance_param(state);

    // Stopping rules (plan sec 4). Sticky once fired.
    bool all_done = true;
    for (uint8_t p = 0; p < ITER_TUNE_PARAM_COUNT; p++) {
        if (!state->param_done[p]) all_done = false;
    }
    if (all_done) {
        // Every parameter is exhausted -- either halved below the minimum
        // step, or probed to the cap in both directions without a measurable
        // result. Refusing to act is a legitimate outcome, and the plan
        // expects it to be the common one.
        stop(state, ITER_TUNE_STATUS_CONVERGED);
    } else if (state->trials_scored >= ITER_TUNE_MAX_TRIALS) {
        stop(state, ITER_TUNE_STATUS_CONVERGED);
    }
    return result;
}

const char *iter_tune_status_str(iter_tune_status_t status)
{
    switch (status) {
        case ITER_TUNE_STATUS_OFF: return "off";
        case ITER_TUNE_STATUS_TUNING: return "tuning";
        case ITER_TUNE_STATUS_CONVERGED: return "converged";
        case ITER_TUNE_STATUS_FAULTED: return "faulted";
    }
    return "?";
}

const char *iter_tune_result_str(iter_tune_result_t result)
{
    switch (result) {
        case ITER_TUNE_RESULT_DISABLED: return "disabled";
        case ITER_TUNE_RESULT_NO_TRIAL: return "no_trial";
        case ITER_TUNE_RESULT_ACCEPTED: return "accepted";
        case ITER_TUNE_RESULT_REVERTED: return "reverted";
        case ITER_TUNE_RESULT_CARRIED: return "carried";
        case ITER_TUNE_RESULT_CARRY_EXHAUSTED: return "carry_exhausted";
    }
    return "?";
}
