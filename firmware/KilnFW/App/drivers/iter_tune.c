// iter_tune.c -- pure decision logic for PID_EXPANSION_PLAN.md 3.3's
// "Iterative tuning" item. See iter_tune.h for the full design rationale
// (especially the noise-floor section -- that is the part that matters).

#include "iter_tune.h"

#include <math.h>
#include <stdio.h>

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool iter_tune_comparable(const iter_tune_firing_t *a, const iter_tune_firing_t *b, char *reason,
                           size_t reason_len)
{
    if (a->profile_id != b->profile_id) {
        if (reason && reason_len) {
            snprintf(reason, reason_len, "profile %u != %u", (unsigned)a->profile_id, (unsigned)b->profile_id);
        }
        return false;
    }
    if (a->zone_mask != b->zone_mask) {
        if (reason && reason_len) {
            snprintf(reason, reason_len, "zone_mask 0x%02x != 0x%02x", (unsigned)a->zone_mask,
                      (unsigned)b->zone_mask);
        }
        return false;
    }
    float dt = fabsf(a->start_temp_c - b->start_temp_c);
    if (dt > ITER_TUNE_START_TEMP_TOLERANCE_C) {
        if (reason && reason_len) {
            snprintf(reason, reason_len, "start temp differs by %.2f degC (> %.1f) -- residual heat",
                      (double)dt, (double)ITER_TUNE_START_TEMP_TOLERANCE_C);
        }
        return false;
    }
    if (reason && reason_len) reason[0] = '\0';
    return true;
}

iter_tune_gains_t iter_tune_active_gains(const iter_tune_zone_state_t *state)
{
    if (state->has_pending) return state->pending_gains;
    return state->baseline.gains;
}

bool iter_tune_propose_perturbation(iter_tune_zone_state_t *state, iter_tune_gains_t *out_gains)
{
    if (!state->enabled || !state->has_baseline || state->has_pending) return false;

    float dir = state->next_perturb_negative ? -1.0f : 1.0f;
    iter_tune_gains_t g = state->baseline.gains;
    g.kp = clampf(g.kp * (1.0f + dir * ITER_TUNE_PERTURB_FRACTION), ITER_TUNE_GAIN_FLOOR_C, ITER_TUNE_GAIN_CEIL_C);
    g.ki = clampf(g.ki * (1.0f + dir * ITER_TUNE_PERTURB_FRACTION), ITER_TUNE_GAIN_FLOOR_C, ITER_TUNE_GAIN_CEIL_C);
    // kd deliberately untouched -- see header doc comment.

    state->pending_gains = g;
    state->has_pending = true;
    state->next_perturb_negative = !state->next_perturb_negative;

    if (out_gains) *out_gains = g;
    return true;
}

iter_tune_result_t iter_tune_process_firing(iter_tune_zone_state_t *state, const iter_tune_firing_t *firing,
                                             char *reason, size_t reason_len)
{
    if (!state->enabled) {
        if (reason && reason_len) snprintf(reason, reason_len, "iter_tune disabled for this zone");
        return ITER_TUNE_RESULT_DISABLED;
    }

    if (!state->has_baseline) {
        state->baseline = *firing;
        state->has_baseline = true;
        state->has_pending = false;
        if (reason && reason_len) snprintf(reason, reason_len, "no prior baseline -- seeded from this firing");
        return ITER_TUNE_RESULT_SEEDED_BASELINE;
    }

    if (!state->has_pending) {
        // This firing ran on the (already accepted) baseline gains with no
        // trial in flight -- e.g. the operator fired the same profile again
        // before a new perturbation was proposed. Refresh the baseline's
        // recorded identity/score to this latest run rather than silently
        // ignoring it, so the NEXT trial is judged against current
        // conditions, not a stale one. This does not accept or reject
        // anything -- no gains changed, none are being judged.
        state->baseline = *firing;
        if (reason && reason_len) snprintf(reason, reason_len, "no trial pending -- baseline score refreshed");
        return ITER_TUNE_RESULT_BASELINE_REFRESHED;
    }

    char why[96];
    if (!iter_tune_comparable(&state->baseline, firing, why, sizeof(why))) {
        // Leave the trial pending and the baseline untouched: an
        // incomparable firing (different profile, different zone set, or
        // residual heat from an earlier run) proves nothing about whether
        // the perturbation was good or bad, so it must not be allowed to
        // accept OR revert it. Try again on the next comparable firing.
        if (reason && reason_len) snprintf(reason, reason_len, "not comparable: %s", why);
        return ITER_TUNE_RESULT_REFUSED_NOT_COMPARABLE;
    }

    // Lower iae_normalized is better. relative_improvement > 0 means the
    // trial scored better than baseline.
    float baseline_score = state->baseline.iae_normalized;
    float relative_improvement = (baseline_score - firing->iae_normalized) / baseline_score;

    if (relative_improvement >= ITER_TUNE_MIN_RELATIVE_IMPROVEMENT) {
        state->baseline = *firing; // gains AND score both move to the trial's
        state->has_pending = false;
        if (reason && reason_len) {
            snprintf(reason, reason_len, "accepted: %.1f%% better (>= %.1f%% floor)",
                      (double)(relative_improvement * 100.0f), (double)(ITER_TUNE_MIN_RELATIVE_IMPROVEMENT * 100.0f));
        }
        return ITER_TUNE_RESULT_ACCEPTED;
    }

    // Below the noise floor (including a genuinely worse score, i.e.
    // relative_improvement < 0) -- revert. baseline is untouched, so
    // iter_tune_active_gains() goes straight back to baseline.gains, the
    // exact float bits accepted last time.
    state->has_pending = false;
    if (reason && reason_len) {
        snprintf(reason, reason_len, "reverted: %.1f%% change (< %.1f%% floor, noise)",
                  (double)(relative_improvement * 100.0f), (double)(ITER_TUNE_MIN_RELATIVE_IMPROVEMENT * 100.0f));
    }
    return ITER_TUNE_RESULT_REVERTED;
}
