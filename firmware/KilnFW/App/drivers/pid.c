#include "pid.h"

#include <math.h>
#include <string.h>

void pid_reset(pid_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void pid_seed_bumpless(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float u_desired, float ff_u)
{
    state->prev_measurement = measurement;
    state->d_filtered = 0.0f;
    float p_term = cfg->kp * (cfg->b * setpoint - measurement);
    float integral_needed = (cfg->ki > 0.0f) ? (u_desired - p_term - ff_u) / cfg->ki : 0.0f;
    if (integral_needed < 0.0f) {
        integral_needed = 0.0f; /* the same floor the anti-windup clamp enforces every tick */
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
                 float dt_s, float ff_u)
{
    return pid_update_terms(state, cfg, setpoint, measurement, dt_s, ff_u, NULL);
}

float pid_update_terms(pid_state_t *state, const pid_cfg_t *cfg, float setpoint, float measurement,
                       float dt_s, float ff_u, pid_terms_t *out_terms)
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

    float i_term = cfg->ki * state->integral;
    if (i_term < 0.0f) {
        i_term = 0.0f;
        state->integral = 0.0f;
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
