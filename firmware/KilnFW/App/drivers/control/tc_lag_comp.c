// See tc_lag_comp.h for the model, the filter and the safety posture.
#include "tc_lag_comp.h"

#include <math.h>
#include <string.h>

void tc_lag_comp_reset(tc_lag_comp_state_t *st)
{
    uint32_t resets = st->resets;
    memset(st, 0, sizeof(*st));
    st->resets = resets + 1u;
}

float tc_lag_comp_filter_tau_s(float tau_s)
{
    float tf = tau_s * 0.25f;
    if (tf < 5.0f) tf = 5.0f;
    if (tf > 60.0f) tf = 60.0f;
    return tf;
}

/* Reset and report whether a non-zero correction was thrown away. */
static void reset_noting(tc_lag_comp_state_t *st, bool *out_reset)
{
    bool had_corr = st->primed && st->corr_c != 0.0f;
    tc_lag_comp_reset(st);
    if (out_reset && had_corr) {
        *out_reset = true;
    }
}

float tc_lag_comp_step(tc_lag_comp_state_t *st, float tau_s, float meas_c, bool valid, float dt_s,
                       bool *out_reset)
{
    if (out_reset) {
        *out_reset = false;
    }
    if (!(tau_s > 0.0f)) {
        if (st->primed) {
            reset_noting(st, out_reset);
        }
        return meas_c; /* exact passthrough: compensation off */
    }
    if (!valid || !isfinite(meas_c)) {
        if (st->primed) {
            reset_noting(st, out_reset);
        }
        return meas_c;
    }
    if (st->primed) {
        float span_s = dt_s + st->held_dt_s;
        bool jump = fabsf(meas_c - st->prev_c) > TC_LAG_COMP_GLITCH_C;
        if (!(dt_s > 0.0f) || span_s > TC_LAG_COMP_MAX_DT_S || st->tau_s != tau_s || (jump && st->glitch_held)) {
            reset_noting(st, out_reset); /* gap, tau change, or a jump that persisted: a real step */
        } else if (jump) {
            /* First tick of a jump: hold. The reading goes out uncorrected
             * this tick; the filter keeps its state for the next one. */
            st->glitch_held = true;
            st->held_dt_s += dt_s;
            if (out_reset && st->corr_c != 0.0f) {
                *out_reset = true; /* the estimate stepped: re-base the PID derivative */
            }
            st->corr_c = 0.0f;
            return meas_c;
        }
    }
    if (!st->primed) {
        st->primed = true;
        st->prev_c = meas_c;
        st->tau_s = tau_s;
        st->d1_c_per_s = 0.0f;
        st->d2_c_per_s = 0.0f;
        st->corr_c = 0.0f;
        return meas_c;
    }

    float span_s = dt_s + st->held_dt_s;
    st->glitch_held = false;
    st->held_dt_s = 0.0f;
    float rate = (meas_c - st->prev_c) / span_s;
    st->prev_c = meas_c;
    float tf = tc_lag_comp_filter_tau_s(tau_s);
    float a = span_s / (tf + span_s);
    st->d1_c_per_s += a * (rate - st->d1_c_per_s);
    st->d2_c_per_s += a * (st->d1_c_per_s - st->d2_c_per_s);

    float corr = tau_s * st->d2_c_per_s;
    if (corr > TC_LAG_COMP_MAX_CORR_C) corr = TC_LAG_COMP_MAX_CORR_C;
    if (corr < -TC_LAG_COMP_MAX_CORR_C) corr = -TC_LAG_COMP_MAX_CORR_C;
    st->corr_c = corr;
    return meas_c + corr;
}
