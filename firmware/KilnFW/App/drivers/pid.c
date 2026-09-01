#include "pid.h"

#include <math.h>
#include <string.h>

/* ki-blowup guard (separate concern from the hold/climb floor change this
 * file also carries -- see both call sites' comments): the floor division
 * (-ff_hold / ki, formerly -ff_u / ki) is guarded against ki == 0 but was
 * NOT guarded against a small POSITIVE ki. Note the failure mode is NOT
 * primarily that ki*integral (the duty-space i_term) comes out too large --
 * i_term == -ff_hold by construction right after the floor applies, which is
 * an entirely ordinary duty-scale number regardless of ki. The failure mode
 * is that the RAW state->integral = -ff_hold/ki itself becomes enormous for
 * a small ki (e.g. ff_hold=0.5, ki=1e-6 -> integral=-500000), which then
 * feeds pid_rescale_integral_for_new_ki() (state->integral *= old_ki/new_ki)
 * -- a further ki change can blow that raw value up again, and float32
 * precision on an accumulator that size is already poor long before that.
 * So this bounds the RAW integral magnitude directly, not a duty-space
 * quantity -- no legitimate accumulation (a realistic ki together with a
 * duty-scale floor) should ever need |integral| anywhere near this bound;
 * anything reaching it is the small-ki blowup, not a real control need. */
#define PID_INTEGRAL_RAW_ABS_BOUND 100000.0f

void pid_reset(pid_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void pid_seed_bumpless(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float u_desired, float ff_u, float ff_hold)
{
    state->prev_measurement = measurement;
    state->d_filtered = 0.0f;
    float p_term = cfg->kp * (cfg->b * setpoint - measurement);
    float integral_needed = (cfg->ki > 0.0f) ? (u_desired - p_term - ff_u) / cfg->ki : 0.0f;
    /* Same floor pid_update_terms() enforces every tick: the integral may
     * cancel at most what feedforward's HOLD component added
     * (ki*integral >= -ff_hold), never more, and never any of the climb
     * component -- see pid.h's top-of-file doc comment for the full
     * rationale (the 2026-08-31 hold-only-floor fix). ff_hold==ff_u (no
     * hold/climb split) reproduces the old -ff_u floor byte-for-byte;
     * ff_hold==ff_u==0.0f (no feedforward model) reduces this further to the
     * original >= 0 floor. */
    float integral_floor = (cfg->ki > 0.0f) ? (-ff_hold / cfg->ki) : 0.0f;
    if (integral_needed < integral_floor) {
        integral_needed = integral_floor;
    }
    /* ki-blowup guard (separate from the hold/climb change above) -- see
     * PID_INTEGRAL_RAW_ABS_BOUND's doc comment at the top of this file: a
     * small positive ki makes -ff_hold/ki a huge RAW integral even for a
     * modest ff_hold. Bound the raw magnitude directly. */
    if (integral_needed < -PID_INTEGRAL_RAW_ABS_BOUND) {
        integral_needed = -PID_INTEGRAL_RAW_ABS_BOUND;
    }
    state->integral = integral_needed;
    state->initialized = true;
}

void pid_rescale_integral_for_new_ki(pid_state_t *state, float old_ki, float new_ki)
{
    if (!(old_ki > 0.0f) || !(new_ki > 0.0f) || old_ki == new_ki) {
        return; /* nothing sensible to rescale against/onto, or nothing changed */
    }
    state->integral *= (double)old_ki / (double)new_ki;
}

float pid_update(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                 float dt_s, float ff_u, float ff_hold)
{
    return pid_update_terms(state, cfg, setpoint, measurement, dt_s, ff_u, ff_hold, NULL);
}

float pid_update_terms(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float dt_s, float ff_u, float ff_hold, pid_terms_t *out_terms)
{
    if (!state->initialized) {
        state->prev_measurement = measurement;
        state->integral = 0.0f;
        state->d_filtered = 0.0f;
        state->initialized = true;
    }
    if (!(dt_s > 0.0f)) {
        dt_s = 1.0f; /* guards against div-by-zero on a zero/negative tick delta */
    }

    float error = setpoint - measurement;

    /* Functional range blending (TODO.md 6A.2 / Marlin PID_FUNCTIONAL_RANGE):
     * outside pid_range_c, skip the PID math entirely and hold the
     * integrator -- a 900C climb from cold must not spend an hour winding
     * up I only to overshoot on arrival. prev_measurement still updates so
     * D isn't fed a stale value when re-entering the range; the caller is
     * responsible for pid_seed_bumpless() at that transition. Note this
     * early return does NOT update d_filtered -- it stays frozen (0.0f on a
     * cold start) for the whole time error is out of range. Gains don't
     * matter while out of range, but any fuzzy-PID layer reading d_filtered
     * (pid_fuzzy.c) will see a stale/zero rate on the first in-range tick
     * or two after re-entering, until the low-pass filter catches up. */
    if (fabsf(error) > cfg->pid_range_c) {
        state->prev_measurement = measurement;
        float u = (error > 0.0f) ? 1.0f : 0.0f;
        if (out_terms) {
            *out_terms = (pid_terms_t){.p = u, .i = 0.0f, .d = 0.0f, .ff = 0.0f};
        }
        return u;
    }

    /* Derivative on measurement, low-pass filtered. dT/dt sign flips
     * because d(error)/dt = -d(measurement)/dt when setpoint is (locally)
     * constant within one tick. */
    float raw_d = -(measurement - state->prev_measurement) / dt_s;
    float alpha = dt_s / (cfg->d_filter_tau_s + dt_s);
    state->d_filtered += alpha * (raw_d - state->d_filtered);
    state->prev_measurement = measurement;

    float p_term = cfg->kp * (cfg->b * setpoint - measurement);
    float d_term = cfg->kd * state->d_filtered;

    /* Anti-windup: conditional integration (freeze I when the unclamped
     * output is already saturated *and* integrating would push it further
     * out) plus a hard clamp on Ki*I itself -- TODO.md 6A.2 is explicit
     * that both are needed, not either. */
    float unclamped = p_term + cfg->ki * state->integral + d_term + ff_u;
    bool would_push_further_out =
        (unclamped >= 1.0f && error > 0.0f) || (unclamped <= 0.0f && error < 0.0f);
    if (!would_push_further_out) {
        state->integral += error * dt_s;
    }

    /* Floor: the integral may cancel at most what feedforward's HOLD
     * component (ff_hold) added, never more, and never any of the CLIMB
     * component -- there is no active cooling, so once the I term has
     * subtracted ff_hold's entire contribution the total is back to plain
     * P+D+climb, and further negative integral would mean the loop owing a
     * duty debt the hardware cannot repay. ff_hold==ff_u (no hold/climb
     * split, e.g. a dwell where climb is exactly 0, or a caller with no
     * model to split) reproduces the OLD -ff_u floor byte-for-byte;
     * ff_hold==ff_u==0.0f (no feedforward model, or a zone whose feedforward
     * under-predicts and whose integral never approaches this floor)
     * reduces it further to the original i_term >= 0 clamp -- both strict
     * generalizations, not new behavior for those zones/ticks. See pid.h's
     * top-of-file doc comment for why hold-only (not the full ff_u) is the
     * right floor basis: flooring at -ff_u let the integral cancel climb
     * too, which during a constant-rate ramp bound the floor for the WHOLE
     * ramp and ran the loop on P+D alone with feedforward absent -- the
     * dwell case is unaffected by the change (climb is exactly 0 there) but
     * a ramp now keeps its climb contribution once the floor binds.
     * Unbounded negative windup during a genuine cool-down or heat-blocked
     * period (ff_hold ~ 0) is prevented the same way it always was: the
     * conditional-integration freeze above (would_push_further_out) stops
     * the integral from accumulating further once the total output is
     * already pinned at the 0 rail with error still negative, so this floor
     * is rarely even the thing that bites in that case. */
    float i_term = cfg->ki * state->integral;
    float i_floor = -ff_hold;
    if (i_term < i_floor) {
        i_term = i_floor;
        state->integral = (cfg->ki > 0.0f) ? i_floor / cfg->ki : 0.0f;
        /* ki-blowup guard -- see PID_INTEGRAL_RAW_ABS_BOUND's doc comment at
         * the top of this file. Same bound as pid_seed_bumpless(), kept
         * separate because this path recomputes state->integral from i_floor
         * every bound tick rather than once at a seed. Recomputing i_term
         * from the clamped integral (rather than leaving i_term at i_floor)
         * means the reported/used i_term for THIS tick honestly reflects
         * what the clamped integral actually contributes -- it stops
         * matching -ff_hold exactly once this guard has engaged, which is
         * the intended, visible cost of protecting against the ki blowup. */
        if (state->integral < -PID_INTEGRAL_RAW_ABS_BOUND) {
            state->integral = -PID_INTEGRAL_RAW_ABS_BOUND;
            i_term = cfg->ki * state->integral;
        }
    } else if (i_term > 1.0f) {
        i_term = 1.0f;
        state->integral = (cfg->ki > 0.0f) ? 1.0f / cfg->ki : 0.0f;
    }

    float u = p_term + i_term + d_term + ff_u;
    if (u < 0.0f) {
        u = 0.0f;
    } else if (u > 1.0f) {
        u = 1.0f;
    }
    if (out_terms) {
        *out_terms = (pid_terms_t){.p = p_term, .i = i_term, .d = d_term, .ff = ff_u};
    }
    return u;
}
