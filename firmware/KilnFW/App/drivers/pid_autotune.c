#include "pid_autotune.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static fopdt_model_t invalid_model(const char *reason)
{
    fopdt_model_t m;
    memset(&m, 0, sizeof(m));
    m.valid = false;
    snprintf(m.invalid_reason, sizeof(m.invalid_reason), "%s", reason);
    return m;
}

/* Linear-interpolates the time at which the trace crosses `target_c`,
 * scanning forward from the start (the trace is assumed monotonic in the
 * direction of the step, which a real single duty-step response is). */
static bool find_crossing_time(const autotune_sample_t *samples, int n, float target_c, float rise_sign,
                               float *out_t)
{
    for (int i = 1; i < n; i++) {
        float prev_c = samples[i - 1].measurement_c;
        float cur_c = samples[i].measurement_c;
        bool crossed = (rise_sign > 0.0f) ? (prev_c <= target_c && cur_c >= target_c)
                                           : (prev_c >= target_c && cur_c <= target_c);
        if (crossed) {
            float span = cur_c - prev_c;
            float frac = (fabsf(span) > 1e-6f) ? (target_c - prev_c) / span : 0.0f;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            *out_t = samples[i - 1].t_s + frac * (samples[i].t_s - samples[i - 1].t_s);
            return true;
        }
    }
    return false;
}

fopdt_model_t pid_autotune_fit_fopdt(const autotune_sample_t *samples, int sample_count, float baseline_c,
                                     float duty_step)
{
    if (fabsf(duty_step) < 1e-6f) {
        return invalid_model("duty_step too small to identify a gain");
    }
    if (sample_count < 2) {
        return invalid_model("not enough samples");
    }

    float final_c = samples[sample_count - 1].measurement_c;
    float rise = final_c - baseline_c;
    if (fabsf(rise) < 0.5f) {
        return invalid_model("response too small to fit (trace flat or noise-dominated)");
    }
    float rise_sign = (rise > 0.0f) ? 1.0f : -1.0f;

    float target28 = baseline_c + 0.283f * rise;
    float target63 = baseline_c + 0.632f * rise;

    float t28, t63;
    if (!find_crossing_time(samples, sample_count, target28, rise_sign, &t28)) {
        return invalid_model("trace never reaches 28.3% of the total rise");
    }
    if (!find_crossing_time(samples, sample_count, target63, rise_sign, &t63)) {
        return invalid_model("trace never reaches 63.2% of the total rise -- run longer or closer to steady state");
    }

    float tau = 1.5f * (t63 - t28);
    float dead_time = t63 - tau;
    if (dead_time < 0.0f) {
        dead_time = 0.0f; /* noise can push this slightly negative; a real plant's L can't be */
    }
    if (tau <= 0.0f) {
        return invalid_model("fitted tau <= 0 -- t28/t63 crossing times out of order, trace likely too noisy");
    }

    fopdt_model_t m;
    m.k_gain_c_per_duty = rise / duty_step;
    m.tau_s = tau;
    m.dead_time_s = dead_time;
    m.valid = true;
    m.invalid_reason[0] = '\0';
    return m;
}

/* Below this, Cohen-Coon's Kc term divides by L (dead_time_s); a fit whose
 * step response barely lagged at all can legitimately report L at or near
 * zero, and 1/L blows toward infinity right where the fit is least trustworthy
 * anyway (a near-zero dead time is exactly the regime the two-point method is
 * weakest in). Unlike SIMC's L==0 fallback -- which substitutes tau for
 * lambda and keeps producing a (conservative) number -- Cohen-Coon has no
 * substitute that preserves its meaning: the whole rule is parameterized by
 * L/tau. Silently clamping L to some epsilon would hand back an arbitrarily
 * large Kc that looks like a real answer. For a kiln, refusing (all-zero
 * gains, same shape as an invalid model) is the safer failure than emitting
 * a huge proportional gain, so that's the choice here. */
#define AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S 0.5f

autotune_gains_t pid_autotune_tune_from_fopdt(const fopdt_model_t *model, autotune_rule_t rule, float lambda_s)
{
    autotune_gains_t g = {.kp = 0.0f, .ki = 0.0f, .kd = 0.0f, .rule = rule, .refusal = AUTOTUNE_REFUSAL_OK};
    if (!model->valid) {
        g.refusal = AUTOTUNE_REFUSAL_INVALID_MODEL;
        snprintf(g.refusal_reason, sizeof(g.refusal_reason), "FOPDT model is invalid: %s", model->invalid_reason);
        return g;
    }

    if (rule == AUTOTUNE_RULE_COHEN_COON) {
        /* Cohen-Coon (Cohen & Coon, 1953), from the identified FOPDT model
         * {K, tau, L} -- see PID_EXPANSION_PLAN.md Phase 1 / [9] in that
         * plan's literature review. Like Ziegler-Nichols, it is a
         * quarter-amplitude-decay rule (same design target, not derived from
         * it), and it is the more aggressive of the two FOPDT-derivable rules
         * offered here -- SIMC stays the default; this is opt-in only, per
         * §2a's caveat that overshoot on a kiln costs the ware.
         *
         * Series (Kc, Ti, Td) form, in terms of L/tau:
         *   Kc = (1/K) * (tau/L) * (4/3 + L/(4*tau))
         *   Ti = L * (32 + 6*(L/tau)) / (13 + 8*(L/tau))
         *   Td = L * 4 / (11 + 2*(L/tau))
         */
        /* k_gain <= 0, not == 0: a *negative* fitted gain (a step test run
         * while the zone was still cooling, or a miswired relay/thermocouple
         * pair) sails through an == 0 check and yields Kc < 0, i.e. negative
         * Kp/Ki/Kd. autotune_gains_t has no clamp of its own and the operator
         * would be shown three plausible-looking numbers that drive the loop
         * backwards. A heater that cannot cool cannot have a negative gain,
         * so this is always a bad fit, never a real plant. */
        if (model->dead_time_s < AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S) {
            g.refusal = AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL;
            snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                     "Cohen-Coon needs dead time >= %.2f s; this fit has L = %.3f s",
                     (double)AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S, (double)model->dead_time_s);
            return g; /* degenerate L -- refuse rather than emit an inflated Kc, see above */
        }
        if (model->tau_s <= 0.0f) {
            g.refusal = AUTOTUNE_REFUSAL_NONPOSITIVE_TAU;
            snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                     "fitted time constant tau = %.3f s is not positive", (double)model->tau_s);
            return g;
        }
        if (model->k_gain_c_per_duty <= 0.0f) {
            g.refusal = AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN;
            snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                     "fitted plant gain K = %.4f degC/duty is not positive", (double)model->k_gain_c_per_duty);
            return g;
        }

        float L = model->dead_time_s;
        float tau = model->tau_s;
        float r = L / tau;

        float kc = (1.0f / model->k_gain_c_per_duty) * (tau / L) * (4.0f / 3.0f + r / 4.0f);
        float ti = L * (32.0f + 6.0f * r) / (13.0f + 8.0f * r);
        float td = L * 4.0f / (11.0f + 2.0f * r);

        g.kp = kc;
        g.ki = (ti > 0.0f) ? kc / ti : 0.0f;
        g.kd = kc * td;
        return g;
    }

    if (rule != AUTOTUNE_RULE_SIMC) {
        g.refusal = AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH;
        snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                 "rule %d needs a relay (Ku/Tu) test, not the FOPDT step-test path", (int)rule);
        return g; /* relay-test rules need Ku/Tu, not this FOPDT path -- see header */
    }

    /* Same negative-gain refusal as the Cohen-Coon branch above, and for the
     * same reason: SIMC's Kc = tau/(K*(lambda+L)) inherits K's sign, so a
     * negative fitted gain hands back negative Kp/Ki/Kd here too. This check
     * was missing until 2026-08-30; it is not a Cohen-Coon-specific concern. */
    if (model->k_gain_c_per_duty <= 0.0f) {
        g.refusal = AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN;
        snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                 "fitted plant gain K = %.4f degC/duty is not positive", (double)model->k_gain_c_per_duty);
        return g;
    }

    float lambda = (lambda_s > 0.0f) ? lambda_s : 3.0f * model->dead_time_s;
    if (lambda <= 0.0f) {
        lambda = model->tau_s > 0.0f ? model->tau_s : 1.0f; /* degenerate L==0 fallback: use tau itself */
    }

    /* SIMC (Skogestad): Kc = tau / (K*(lambda+L)), Ti = min(tau, 4*(lambda+L)), Td = L/2. */
    float kc = model->tau_s / (model->k_gain_c_per_duty * (lambda + model->dead_time_s));
    float ti = model->tau_s;
    float ti_cap = 4.0f * (lambda + model->dead_time_s);
    if (ti_cap < ti) {
        ti = ti_cap;
    }
    float td = model->dead_time_s / 2.0f;

    /* Convert series (Kc, Ti, Td) form to pid.c's parallel form:
     * Kp = Kc, Ki = Kc/Ti, Kd = Kc*Td. */
    g.kp = kc;
    g.ki = (ti > 0.0f) ? kc / ti : 0.0f;
    g.kd = kc * td;
    return g;
}

/* ------------------------------------------------------------------------
 * Relay feedback (Astrom-Hagglund)
 * ------------------------------------------------------------------------ */

/* How far apart the trailing cycles may be before the trace is called
 * "not a limit cycle". A genuine limit cycle is a fixed point of the
 * relay-plus-plant dynamics: once reached, successive cycles repeat to
 * within measurement noise. Cycles that are still shrinking (or growing, or
 * riding a temperature drift) are still transient, and averaging them yields
 * a number that describes the transient rather than the plant.
 *
 * The period tolerance is the tighter of the two because the period is the
 * more reliable half of the measurement: it comes from crossing times, which
 * noise perturbs only in proportion to the local slope, whereas the
 * amplitude comes from single extreme samples and so eats noise directly.
 * Both are relative to the mean over the cycles used. */
#define RELAY_PERIOD_SPREAD_MAX 0.20f
#define RELAY_AMPLITUDE_SPREAD_MAX 0.35f

/* Maximum cycles we bother to record while scanning. A relay test is
 * hand-driven and slow; anything past this is either a runaway or a caller
 * feeding in the wrong trace, and either way only the last few matter. */
#define RELAY_MAX_CYCLES 64

static relay_model_t invalid_relay(const char *reason)
{
    relay_model_t m;
    memset(&m, 0, sizeof(m));
    m.valid = false;
    snprintf(m.invalid_reason, sizeof(m.invalid_reason), "%s", reason);
    return m;
}

/* Linear-interpolated time of the sample pair (i-1, i) crossing `level`. */
static float interp_crossing_time(const autotune_sample_t *a, const autotune_sample_t *b, float level)
{
    float span = b->measurement_c - a->measurement_c;
    float frac = (fabsf(span) > 1e-6f) ? (level - a->measurement_c) / span : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return a->t_s + frac * (b->t_s - a->t_s);
}

relay_model_t pid_autotune_fit_relay(const autotune_sample_t *samples, int sample_count,
                                     float relay_amplitude_duty, float hysteresis_c)
{
    if (relay_amplitude_duty <= 1e-6f) {
        return invalid_relay("relay amplitude d <= 0 -- nothing was driving the oscillation");
    }
    if (hysteresis_c < 0.0f) {
        return invalid_relay("negative hysteresis band");
    }
    /* Two crossings bound one cycle, and we want several cycles; a handful of
     * samples cannot contain them however the numbers fall. */
    if (sample_count < 8) {
        return invalid_relay("not enough samples");
    }

    /* The midline the cycles are counted against is taken from the *tail* of
     * the trace, not the whole of it. The head of a relay test contains the
     * approach to the limit cycle, which can sit well off the eventual
     * oscillation centre (the plant is still travelling towards the
     * setpoint); a midline contaminated by that would mis-slice the very
     * cycles we care about. The tail is by construction the settled part, so
     * its own min/max midpoint is the right level to slice at. */
    int tail_start = sample_count / 2;
    float tail_min = samples[tail_start].measurement_c;
    float tail_max = tail_min;
    for (int i = tail_start; i < sample_count; i++) {
        float c = samples[i].measurement_c;
        if (c < tail_min) tail_min = c;
        if (c > tail_max) tail_max = c;
    }
    float mid_c = 0.5f * (tail_min + tail_max);
    float tail_pp = tail_max - tail_min;

    /* A trace that never moves has no cycles to find and would otherwise send
     * the crossing detector chasing noise around a meaningless midline. The
     * threshold is deliberately in absolute degC rather than relative: below
     * roughly a degree of swing, a thermocouple reading is quantisation and
     * noise, not an oscillation. */
    if (tail_pp < 1.0f) {
        return invalid_relay("trace never oscillated (flat within noise)");
    }

    /* Crossing detection is itself hysteretic, using a deadband around the
     * midline, for the same reason the relay is: a bare level comparison
     * counts every noise wiggle near the midline as another cycle, and the
     * midline is exactly where the signal is moving fastest and lingers
     * least, so it is where spurious multiple crossings are most likely.
     * Requiring the signal to commit to the low side before the next upward
     * crossing counts makes the detector immune to that. */
    float deadband_c = 0.10f * tail_pp;
    float t_up[RELAY_MAX_CYCLES + 1];
    int up_count = 0;
    bool armed = samples[0].measurement_c < (mid_c - deadband_c);

    for (int i = 1; i < sample_count; i++) {
        float c = samples[i].measurement_c;
        if (!armed) {
            if (c < mid_c - deadband_c) {
                armed = true;
            }
            continue;
        }
        if (c >= mid_c) {
            /* The buffer holds the most recent crossings, not the first ones:
             * on overflow the oldest is dropped rather than the newest
             * ignored, because the whole point of the exercise is to end up
             * looking at the *tail* of the trace. */
            if (up_count > RELAY_MAX_CYCLES) {
                memmove(&t_up[0], &t_up[1], (size_t)RELAY_MAX_CYCLES * sizeof(t_up[0]));
                up_count = RELAY_MAX_CYCLES;
            }
            t_up[up_count++] = interp_crossing_time(&samples[i - 1], &samples[i], mid_c);
            armed = false;
        }
    }

    /* N upward crossings bound N-1 complete cycles. "Complete" matters: a
     * partial cycle at either end of the trace has neither a full period nor
     * a guaranteed peak and trough, and including it would drag both averages
     * towards whatever fraction of a cycle happened to be captured. */
    int cycle_count = up_count - 1;
    if (cycle_count < AUTOTUNE_RELAY_MIN_CYCLES) {
        return invalid_relay("fewer than 3 complete oscillation cycles -- no limit cycle to identify");
    }

    int use = (cycle_count < AUTOTUNE_RELAY_FIT_CYCLES) ? cycle_count : AUTOTUNE_RELAY_FIT_CYCLES;
    int first_cycle = cycle_count - use; /* index into t_up of the first cycle we keep */

    /* Per-cycle period and peak-to-peak, measured strictly between the two
     * crossings that bound that cycle. */
    float period_s[AUTOTUNE_RELAY_FIT_CYCLES];
    float pp_c[AUTOTUNE_RELAY_FIT_CYCLES];
    int scan = 1;
    for (int k = 0; k < use; k++) {
        float t0 = t_up[first_cycle + k];
        float t1 = t_up[first_cycle + k + 1];
        period_s[k] = t1 - t0;
        if (period_s[k] <= 0.0f) {
            return invalid_relay("non-monotonic sample timestamps");
        }

        float lo = 0.0f, hi = 0.0f;
        bool seen = false;
        while (scan < sample_count && samples[scan].t_s < t0) {
            scan++;
        }
        for (int i = scan; i < sample_count && samples[i].t_s <= t1; i++) {
            float c = samples[i].measurement_c;
            if (!seen) {
                lo = hi = c;
                seen = true;
            } else {
                if (c < lo) lo = c;
                if (c > hi) hi = c;
            }
        }
        if (!seen) {
            return invalid_relay("cycle contains no samples -- trace timestamps inconsistent");
        }
        pp_c[k] = hi - lo;
    }

    float period_sum = 0.0f, pp_sum = 0.0f;
    float period_min = period_s[0], period_max = period_s[0];
    float pp_min = pp_c[0], pp_max = pp_c[0];
    for (int k = 0; k < use; k++) {
        period_sum += period_s[k];
        pp_sum += pp_c[k];
        if (period_s[k] < period_min) period_min = period_s[k];
        if (period_s[k] > period_max) period_max = period_s[k];
        if (pp_c[k] < pp_min) pp_min = pp_c[k];
        if (pp_c[k] > pp_max) pp_max = pp_c[k];
    }
    float tu = period_sum / (float)use;
    float pp_mean = pp_sum / (float)use;

    if (tu <= 0.0f || pp_mean <= 0.0f) {
        return invalid_relay("degenerate cycle measurement");
    }
    if ((period_max - period_min) / tu > RELAY_PERIOD_SPREAD_MAX) {
        return invalid_relay("cycle periods inconsistent -- trace has not settled into a limit cycle");
    }
    if ((pp_max - pp_min) / pp_mean > RELAY_AMPLITUDE_SPREAD_MAX) {
        return invalid_relay("cycle amplitudes inconsistent -- oscillation still growing or decaying");
    }

    /* Here is the peak-to-peak -> amplitude halving the header warns about.
     * Everything below is in "half" quantities: a, h and d alike. */
    float a = 0.5f * pp_mean;

    /* a <= h is checked *before* the sqrt, never after: taking sqrt of a
     * negative and testing the result for NaN would leak a NaN into ku on any
     * toolchain that treats the domain error loosely, and a NaN gain that
     * reaches the PID is far worse than a rejected autotune. Physically this
     * says the plant never escaped the switching band, so the relay learned
     * nothing about it. The 1% margin keeps the a-just-above-h case from
     * producing a near-zero denominator and a correspondingly absurd Ku --
     * the formula is numerically ill-conditioned exactly there. */
    if (a <= hysteresis_c * 1.01f) {
        return invalid_relay("oscillation amplitude <= hysteresis band -- identification meaningless");
    }

    float denom = sqrtf(a * a - hysteresis_c * hysteresis_c);
    if (!(denom > 1e-6f)) {
        return invalid_relay("hysteresis correction degenerate");
    }

    relay_model_t m;
    memset(&m, 0, sizeof(m));
    /* Ku = 4d / (pi * sqrt(a^2 - h^2)) -- the describing-function estimate of
     * the gain that would put this loop exactly on the stability boundary.
     * d is in duty, a and h in degC, so Ku comes out in duty per degC, which
     * is the unit pid.c's Kp already carries (its error input is degC and its
     * output is duty). No further conversion at the call site. */
    m.ku = 4.0f * relay_amplitude_duty / (3.14159265358979f * denom);
    m.tu_s = tu;
    m.amplitude_c = a;
    m.cycles_used = use;
    m.valid = true;
    m.invalid_reason[0] = '\0';
    return m;
}

autotune_gains_t pid_autotune_tune_from_relay(const relay_model_t *model, autotune_rule_t rule)
{
    autotune_gains_t g = {.kp = 0.0f, .ki = 0.0f, .kd = 0.0f, .rule = rule, .refusal = AUTOTUNE_REFUSAL_OK};
    if (!model->valid) {
        g.refusal = AUTOTUNE_REFUSAL_INVALID_MODEL;
        snprintf(g.refusal_reason, sizeof(g.refusal_reason), "relay model is invalid: %s", model->invalid_reason);
        return g;
    }
    if (model->ku <= 0.0f || model->tu_s <= 0.0f) {
        g.refusal = AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN;
        snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                 "fitted Ku = %.5f, Tu = %.3f s must both be positive", (double)model->ku, (double)model->tu_s);
        return g;
    }

    float kc, ti, td;
    switch (rule) {
    case AUTOTUNE_RULE_ZIEGLER_NICHOLS:
        /* DANGER, and this is not boilerplate: ZN's PID rule targets roughly
         * quarter-amplitude decay, which means it deliberately leaves the
         * loop ringing. It is what Marlin's M303 emits and it is fine on a
         * hotend with a few grams of aluminium. A kiln at cone temperature
         * has neither the thermal headroom nor the element life to spend on
         * designed-in overshoot, so this must never be selected by default
         * anywhere -- only when a user explicitly asks for it. The default is
         * and stays SIMC from the step test.
         *
         * Ti/Td written out rather than TODO.md's Ki = 2*Kp/Tu, Kd = Kp*Tu/8
         * so that all three rules in this file convert to parallel form
         * through the same two lines below; they are the same numbers
         * (Ti = Tu/2 gives Ki = Kc/Ti = 2*Kc/Tu; Td = Tu/8 gives
         * Kd = Kc*Td = Kc*Tu/8). */
        kc = 0.6f * model->ku;
        ti = model->tu_s / 2.0f;
        td = model->tu_s / 8.0f;
        break;
    case AUTOTUNE_RULE_TYREUS_LUYBEN:
        /* Tyreus-Luyben is ZN's answer to the above: about half the
         * proportional gain and a more than four times longer integral time,
         * trading settling speed for a loop that does not ring. If one of the
         * two oscillation-based rules has to be used on a kiln, it is this
         * one -- but it is still not the default. */
        kc = model->ku / 3.2f;
        ti = 2.2f * model->tu_s;
        td = model->tu_s / 6.3f;
        break;
    case AUTOTUNE_RULE_SIMC:
    default:
        /* SIMC is a model-based rule: it needs tau and L, which a single
         * frequency-response point cannot supply. Mirror image of the FOPDT
         * path's rejection of ZN -- zero gains, not a silently wrong tuning. */
        g.refusal = AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH;
        snprintf(g.refusal_reason, sizeof(g.refusal_reason),
                 "SIMC needs a FOPDT step-test model (tau, L), not a relay (Ku/Tu) test");
        return g;
    }

    /* Same series (Kc, Ti, Td) -> parallel {Kp, Ki, Kd} conversion the SIMC
     * path uses, for the same reason: pid.c's terms are Ki*integral and
     * Kd*d_filtered. */
    g.kp = kc;
    g.ki = (ti > 0.0f) ? kc / ti : 0.0f;
    g.kd = kc * td;
    return g;
}

float pid_autotune_estimate_max_ramp_c_per_hr(const fopdt_model_t *model, float u_max, float t_now_c,
                                              float t_ambient_c)
{
    if (!model->valid || model->tau_s <= 0.0f) {
        return 0.0f;
    }
    float rate_c_per_s = (model->k_gain_c_per_duty * u_max - (t_now_c - t_ambient_c)) / model->tau_s;
    if (rate_c_per_s < 0.0f) {
        rate_c_per_s = 0.0f; /* not enough headroom left to climb -- 0, not a nonsensical negative ramp */
    }
    return rate_c_per_s * 3600.0f;
}

/* ------------------------------------------------------------------------
 * Relative Gain Array -- TODO.md 6A.5(c). See pid_autotune.h for what the
 * number means and why every failure path here refuses instead of
 * approximating.
 * ------------------------------------------------------------------------ */

static autotune_rga_t invalid_rga(autotune_rga_status_t status, const char *reason)
{
    autotune_rga_t r;
    memset(&r, 0, sizeof(r));
    r.valid = false;
    r.status = status;
    snprintf(r.invalid_reason, sizeof(r.invalid_reason), "%s", reason);
    return r;
}

/* Determinant of a 2x2 or 3x3, written out rather than factorized. At this
 * size the closed form is both exact-to-roundoff and shorter than the
 * pivoting logic a general LU would need, and it keeps the adjugate below
 * using the same cofactors -- one formula, not two that could disagree. */
static float rga_det(const float m[AUTOTUNE_RGA_MAX_ZONES][AUTOTUNE_RGA_MAX_ZONES], int n)
{
    if (n == 2) {
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* adj[i][j] is the (j,i) cofactor, i.e. the adjugate is already the
 * transposed cofactor matrix -- so inv = adj / det. */
static void rga_adjugate(const float m[AUTOTUNE_RGA_MAX_ZONES][AUTOTUNE_RGA_MAX_ZONES], int n,
                         float adj[AUTOTUNE_RGA_MAX_ZONES][AUTOTUNE_RGA_MAX_ZONES])
{
    if (n == 2) {
        adj[0][0] =  m[1][1];
        adj[0][1] = -m[0][1];
        adj[1][0] = -m[1][0];
        adj[1][1] =  m[0][0];
        return;
    }
    adj[0][0] =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]);
    adj[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]);
    adj[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]);
    adj[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]);
    adj[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]);
    adj[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]);
    adj[2][0] =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    adj[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]);
    adj[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]);
}

autotune_rga_t pid_autotune_rga(const float *k, const bool *k_valid, int n)
{
    if (!k || !k_valid) {
        return invalid_rga(AUTOTUNE_RGA_ERR_TOO_FEW_ZONES, "no matrix supplied");
    }
    if (n < 2) {
        /* Interaction is a property of a *pair* of loops. A one-zone kiln
         * has nothing to interact with, and its "RGA" is the scalar 1 --
         * true, but vacuous, and printing it would invite reading it as
         * evidence of independence that was never measured. */
        return invalid_rga(AUTOTUNE_RGA_ERR_TOO_FEW_ZONES,
                           "needs at least 2 zones -- one loop cannot interact with itself");
    }
    if (n > AUTOTUNE_RGA_MAX_ZONES) {
        return invalid_rga(AUTOTUNE_RGA_ERR_UNSUPPORTED, "more zones than this module can invert");
    }

    /* Largest fully-measured principal sub-block, by brute force over the
     * 2^n zone subsets. n <= 4 makes that at most 16 iterations of a few
     * comparisons -- cheaper than any cleverer search, and unlike a greedy
     * "drop the emptiest row" heuristic it is guaranteed to find the
     * genuine maximum rather than a locally-plausible one. Ascending mask
     * order means ties go to the subset holding the lowest zone indices. */
    int best_mask = 0;
    int best_count = 0;
    for (int mask = 0; mask < (1 << n); mask++) {
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (mask & (1 << i)) count++;
        }
        if (count <= best_count || count < 2) continue;
        bool complete = true;
        for (int i = 0; i < n && complete; i++) {
            if (!(mask & (1 << i))) continue;
            for (int j = 0; j < n; j++) {
                if (!(mask & (1 << j))) continue;
                if (!k_valid[i * n + j]) { complete = false; break; }
            }
        }
        if (complete) {
            best_mask = mask;
            best_count = count;
        }
    }

    if (best_count < 2) {
        /* Rows arrive one at a time as each zone's autotune completes, so
         * this is the *normal* state early in a session, not an error in
         * the usual sense -- but it is still a refusal, because the only
         * alternative (zero-filling the gaps) fabricates the answer. */
        return invalid_rga(AUTOTUNE_RGA_ERR_INCOMPLETE,
                           "no 2 zones yet have every cross-gain between them measured");
    }
    if (best_count > 3) {
        return invalid_rga(AUTOTUNE_RGA_ERR_UNSUPPORTED, "only 2x2 and 3x3 sub-blocks are supported");
    }

    autotune_rga_t r;
    memset(&r, 0, sizeof(r));
    r.n = best_count;

    float sub[AUTOTUNE_RGA_MAX_ZONES][AUTOTUNE_RGA_MAX_ZONES] = {{0.0f}};
    int rows[AUTOTUNE_RGA_MAX_ZONES];
    int nn = 0;
    for (int i = 0; i < n; i++) {
        if (best_mask & (1 << i)) rows[nn++] = i;
    }
    float scale = 0.0f; /* max|entry|, the yardstick the determinant floor is measured against */
    for (int a = 0; a < nn; a++) {
        r.zone_index[a] = (uint8_t)rows[a];
        for (int b = 0; b < nn; b++) {
            float v = k[rows[a] * n + rows[b]];
            if (!isfinite(v)) {
                /* A NaN or Inf gain means the fit that produced it was
                 * broken; propagating it would poison every element of
                 * Lambda at once and the page would render "NaN" cells that
                 * look like a display bug rather than a data problem. */
                return invalid_rga(AUTOTUNE_RGA_ERR_SINGULAR, "a measured gain is not a finite number");
            }
            sub[a][b] = v;
            if (fabsf(v) > scale) scale = fabsf(v);
        }
    }

    float det = rga_det(sub, nn);
    r.determinant = det;

    /* Scale-aware singularity floor: eps * scale^n, since det carries the
     * n-th power of the entries' units (degC/duty). Comparing det to a
     * fixed constant instead would make the verdict depend on whether the
     * gains happen to be quoted in degC or in tenths of a degC. */
    float floor_det = AUTOTUNE_RGA_SINGULAR_REL_EPS;
    for (int p = 0; p < nn; p++) floor_det *= scale;
    if (scale <= 0.0f || fabsf(det) <= floor_det) {
        /* Physically this is a kiln whose zones are not independently
         * controllable at all: some combination of duties moves no
         * temperature, or two zones respond identically to everything, so
         * no assignment of loops to zones can command them separately. The
         * RGA of that kiln is unbounded, and reporting a huge finite number
         * would present a division-by-almost-zero as a measurement. */
        return invalid_rga(AUTOTUNE_RGA_ERR_SINGULAR,
                           "gain matrix is singular -- zones are not independently controllable");
    }

    float adj[AUTOTUNE_RGA_MAX_ZONES][AUTOTUNE_RGA_MAX_ZONES] = {{0.0f}};
    rga_adjugate(sub, nn, adj);

    /* Lambda[i][j] = K[i][j] * (K^-1)[j][i]; the transpose is what turns
     * the inverse's "effect of temperature j on duty i" back into the same
     * (step zone, response zone) orientation K itself uses, and dropping it
     * is the classic way to compute a matrix that still sums to 1 across
     * rows and is nevertheless the wrong answer for non-symmetric K. */
    for (int a = 0; a < nn; a++) {
        for (int b = 0; b < nn; b++) {
            r.lambda[a][b] = sub[a][b] * (adj[b][a] / det);
        }
    }

    r.valid = true;
    r.status = AUTOTUNE_RGA_OK;
    return r;
}
