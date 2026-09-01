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

/* How many trailing samples the end-of-trace slope estimate (used by the
 * asymptote extrapolation below) averages over. Final review fix: the
 * comment here used to claim this "uses a first-to-last-of-window rate over
 * several samples instead of adjacent-sample differencing" to avoid "the
 * noise-amplified two-point estimator the settle detector's review
 * flagged" -- that was FALSE. The code computed exactly a first-to-last
 * TWO-POINT difference across the window (samples[window_last] minus
 * samples[window_first]), which means one anomalous final sample fully
 * determines both the magnitude AND the sign of slope_end, identical in
 * kind to the step_peak_slope_c_per_s defect this comment claimed to have
 * learned from. Fixed for real below: slope_end is now a least-squares fit
 * over every sample in the window, so a single outlier sample is one of
 * ASYMPTOTE_SLOPE_WINDOW_SAMPLES points pulling on the fit, not the entire
 * measurement. Clamped to sample_count when the trace is shorter than
 * this. */
#define ASYMPTOTE_SLOPE_WINDOW_SAMPLES 6

/* The firmware trace's packed resolution -- unpack_zone_trace() (autotune_
 * engine.c) stores each sample as (float)dc/10.0f, i.e. this file receives
 * measurement_c already quantized to 0.1 degC steps. Used below (the
 * sign-check break in the asymptote-extrapolation loop) to size a
 * quantization-noise threshold from first principles rather than a
 * hardcoded number -- see that break's own comment. */
#define TRACE_QUANTUM_C 0.1f

/* Round-2 review, items 1/6: bounds on the iterative asymptote refinement
 * below. MAX_EXTRAPOLATION_RATIO caps how far a single fit is allowed to
 * correct itself: 2.0 means rise_inf can never exceed 2x the RAW
 * (last-sample) rise, which is exactly the correction a trace truncated at
 * the 50% mark of its true asymptote needs (rise_raw = 0.5*K =>
 * rise_inf/rise_raw = 2.0) -- the worst case this fix is willing to trust.
 * Below 50%, the two-point crossing times themselves become numerically
 * unreliable (t28/t63 sit close together relative to noise), so capping
 * there instead of trying to correct further is deliberate conservatism:
 * an UNDER-corrected K (the old bug, bounded) is safer than an
 * unboundedly OVER-corrected one (a new, different bug) -- see the review's
 * own defeat case (a noisy trace inflating tau to thousands of seconds and
 * nearly tripling K) for exactly the failure this bound exists to stop.
 * MAX_EXTRAPOLATION_ITERATIONS bounds the iterative refinement below; 5 is
 * generous for a process that converges geometrically once it converges at
 * all (see that loop's own comment) and cheap even on this board's tightest
 * stack budget (a few float ops per pass, no allocation). */
#define MAX_EXTRAPOLATION_RATIO 2.0f
#define MAX_EXTRAPOLATION_ITERATIONS 5
#define EXTRAPOLATION_CONVERGE_EPS_C 0.02f

/* 2026-08-31 bias fix, iterated per round-2 review (item 6) -- root cause of
 * the overshoot defect this whole pass exists to close. The two-point
 * method (28.3%/63.2% crossing times) needs the trace's TRUE steady-state
 * rise to place its target crossings; using samples[n-1].measurement_c -
 * baseline_c instead assumes the LAST sample already IS steady state. On
 * any trace that ends mid-transient -- which includes every fit this engine
 * ever produces from a genuinely honest settle detector (criterion 1 only
 * requires the recent slope to have decayed to a few percent of its peak,
 * not to exactly zero) and, far more severely, any fit that reaches here
 * via the max-duration backstop -- that assumption is false and the fitted
 * K is biased LOW by construction, by however much of the exponential's
 * tail was never reached. Measured on real traces: the two prior overshoot
 * incidents' k_dc values (21.74, 31.96 degC/duty) were low by exactly this
 * mechanism.
 *
 * Fix: extrapolate to the asymptote using the FOPDT relation itself. For a
 * clean single-exponential rise (after dead time), rise(t) obeys
 *   d(rise)/dt = (rise_inf - rise(t)) / tau
 * at every t past the dead time -- not just at t=inf -- so measuring the
 * slope near the end of the trace and combining it with tau recovers
 * rise_inf directly:
 *   rise_inf = rise(t_end) + tau * slope(t_end)
 *
 * ITERATED, not single-pass (round-2 review, item 6): the FIRST version of
 * this fix used the tau fitted from the RAW (still-biased) rise's 28.3%/
 * 63.2% crossings -- circular, since those targets are themselves fractions
 * of the very rise being corrected, so the tau feeding the correction was
 * itself biased (measured: ~3% low at 86.5% of asymptote reached, but ~64%
 * low at 25%, nowhere close to the "within 5%" the first version's test
 * happened to check at one truncation point only). Fixed by RE-FITTING
 * t28/t63/tau/dead_time from each successive rise_inf estimate and
 * repeating until the estimate stops moving (or MAX_EXTRAPOLATION_
 * ITERATIONS is hit, or a refit's crossing times fail -- an overshot
 * candidate the trace cannot support, at which point the PREVIOUS
 * iteration's values are kept rather than accepting a fit the data does not
 * actually reach). tau_s/dead_time_s are therefore also corrected now, not
 * left at their original biased values while only k_gain_c_per_duty moved
 * -- both feed pid_autotune_tune_from_fopdt() and were equally wrong
 * before.
 *
 * A trace that genuinely reached steady state has slope(t_end) ~= 0, so the
 * very first candidate already equals the raw rise and the loop converges
 * immediately -- this fix is a no-op on a fully-settled trace. */
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
    float raw_rise = final_c - baseline_c;
    if (fabsf(raw_rise) < 0.5f) {
        return invalid_model("response too small to fit (trace flat or noise-dominated)");
    }
    float rise_sign = (raw_rise > 0.0f) ? 1.0f : -1.0f;

    /* One two-point fit at a given rise estimate; shared by the initial fit
     * and every refinement iteration below so the two paths cannot drift
     * apart. On failure (the crossings this rise estimate implies aren't
     * reached by the trace -- an overshot candidate the data cannot
     * support), tau and dead_time below are left unset by this block; every
     * caller returns invalid_model() before either is ever read in that
     * case. */
    float tau, dead_time;
    {
        float target28 = baseline_c + 0.283f * raw_rise;
        float target63 = baseline_c + 0.632f * raw_rise;
        float t28, t63;
        if (!find_crossing_time(samples, sample_count, target28, rise_sign, &t28)) {
            return invalid_model("trace never reaches 28.3% of the total rise");
        }
        if (!find_crossing_time(samples, sample_count, target63, rise_sign, &t63)) {
            return invalid_model(
                "trace never reaches 63.2% of the total rise -- run longer or closer to steady state");
        }
        tau = 1.5f * (t63 - t28);
        dead_time = t63 - tau;
        if (dead_time < 0.0f) {
            dead_time = 0.0f; /* noise can push this slightly negative; a real plant's L can't be */
        }
        if (tau <= 0.0f) {
            return invalid_model("fitted tau <= 0 -- t28/t63 crossing times out of order, trace likely too noisy");
        }
    }

    /* End-of-trace slope estimate -- purely data-derived, computed ONCE
     * (unlike tau/dead_time it does not depend on the rise estimate, so
     * re-deriving it inside the iteration loop would be wasted work, not a
     * correctness issue either way). window_first/window_last bound a
     * trailing window; slope_end is the LEAST-SQUARES slope of every
     * sample in that window (see ASYMPTOTE_SLOPE_WINDOW_SAMPLES's own
     * comment for why this replaced a first-to-last two-point difference),
     * standing in for slope(t_end). window_span_s (the window's total time
     * extent) is also used by the sign-check break below to size its
     * quantization-noise threshold -- computed here alongside the fit so
     * both readers agree on exactly what "the window" spans. A window
     * narrower than 2 samples, or one whose samples share a single
     * timestamp (should not happen with real trace data, but guarded
     * anyway), has no slope to measure. */
    float slope_end = 0.0f;
    float window_span_s = 0.0f;
    bool have_slope_end = false;
    {
        int window_span = (sample_count < ASYMPTOTE_SLOPE_WINDOW_SAMPLES) ? sample_count
                                                                           : ASYMPTOTE_SLOPE_WINDOW_SAMPLES;
        if (window_span >= 2) {
            int window_first = sample_count - window_span;
            int window_last = sample_count - 1;
            float span_s = samples[window_last].t_s - samples[window_first].t_s;
            if (span_s > 1e-6f) {
                /* Ordinary least-squares slope: slope = Sxy / Sxx, both
                 * accumulated relative to the window's own mean time (not
                 * absolute t_s, which can be large after a multi-hour run
                 * and would cost float precision squaring it directly). */
                float t_mean = 0.0f, y_mean = 0.0f;
                for (int i = window_first; i <= window_last; i++) {
                    t_mean += samples[i].t_s;
                    y_mean += samples[i].measurement_c;
                }
                t_mean /= (float)window_span;
                y_mean /= (float)window_span;
                float sxy = 0.0f, sxx = 0.0f;
                for (int i = window_first; i <= window_last; i++) {
                    float dt_i = samples[i].t_s - t_mean;
                    sxy += dt_i * (samples[i].measurement_c - y_mean);
                    sxx += dt_i * dt_i;
                }
                if (sxx > 1e-6f) {
                    slope_end = sxy / sxx;
                    window_span_s = span_s;
                    have_slope_end = true;
                }
            }
        }
    }

    float rise_inf = raw_rise;
    float max_rise_c = MAX_EXTRAPOLATION_RATIO * fabsf(raw_rise); /* upper bound -- see its own comment */
    /* Round-3 review, item 4: both flags default true (no correction ran,
     * or the one that did stayed fully self-consistent and converged) --
     * see fopdt_model_t's own doc comment for what each means and why they
     * are separate concepts (a capped-but-refit correction can be
     * tau-consistent yet not "converged" in the eps sense, and vice versa
     * is NOT possible by construction below: an iteration is never counted
     * converged unless its refit also succeeded this same pass). */
    bool tau_consistent_with_gain = true;
    bool extrapolation_converged = true;
    if (have_slope_end) {
        /* Fixed-point iteration: each pass recomputes the candidate from
         * raw_rise (the actual data) and the LATEST tau estimate --
         * candidate = raw_rise + tau_k*slope_end -- rather than compounding
         * onto the previous candidate. Compounding was tried first and
         * diverges: a larger rise_inf pushes target63 further out, which
         * can genuinely increase the refitted tau (63.2% of a bigger
         * asymptote needs more elapsed time to reach on the same trace),
         * and adding tau_k*slope_end AGAIN on top of an already-corrected
         * estimate double-counts that growth every pass. Recomputing from
         * raw_rise each time is the textbook fixed-point form and is what
         * actually converges (or hits the MAX_EXTRAPOLATION_RATIO ceiling /
         * iteration cap, both handled below, instead of diverging). */
        /* Reset to false pessimistically while iterating, then set back to
         * true on whichever exit below actually represents "the correction
         * is done and trustworthy": either no correction was needed at all
         * (the sign-check break -- candidate <= raw_rise, so the trace was
         * already at or past its asymptote and there was nothing to
         * extrapolate) or the loop reached genuine eps convergence. It
         * stays false only on the three exits that are NOT one of those:
         * pinned at the MAX_EXTRAPOLATION_RATIO ceiling, a refit the trace
         * could not support, or exhausting MAX_EXTRAPOLATION_ITERATIONS
         * without settling. Final review fix: the sign-check break used to
         * leave this false, which is wrong -- see that break's own comment
         * for the measured on-target impact (a ~50% nondeterministic
         * refusal rate on trace-quantization noise alone). */
        extrapolation_converged = false;
        for (int iter = 0; iter < MAX_EXTRAPOLATION_ITERATIONS; iter++) {
            float candidate = raw_rise + tau * slope_end;
            /* Sanity floor: the extrapolated rise must not reverse sign (a
             * slope opposite the overall rise -- e.g. thermal noise right
             * at the end of a genuinely flat trace -- would otherwise
             * produce a nonsensical negative correction) and must not be
             * SMALLER in magnitude than the raw observation (extrapolating
             * forward in time on a monotonic rise can only add more rise,
             * never take it away; a candidate that shrinks it means
             * slope_end's sign disagreed with the rise direction, i.e.
             * noise, not signal). Sanity ceiling: MAX_EXTRAPOLATION_RATIO's
             * own comment above -- this is what defeats the review's "tau
             * inflated to thousands of seconds nearly triples K" case, by
             * refusing to let ANY iteration correct past a 2x multiple of
             * the raw observation regardless of what a noisy tau claims. */
            if ((candidate * rise_sign) < (raw_rise * rise_sign)) {
                /* Second final-review fix: sign alone is not enough to call
                 * this "no correction needed". The FIRST fix (see the
                 * unconditional `extrapolation_converged = true` this
                 * replaced) treated EVERY shrink-or-reverse candidate as
                 * trivially converged, on the reasoning that quantization
                 * noise can flip slope_end's sign on a genuinely settled
                 * trace -- true, but the reviewer then measured this same
                 * branch reporting converged=1 on trace shapes that are
                 * NOT settled at all (a real cooling/reversing response,
                 * and truncated-response traces with a trailing dip),
                 * every one of them biased low, i.e. the exact original
                 * overshoot-defect direction:
                 *   rise then cooling/reversing          K=9.3  (true 30, -69%)
                 *   truncated 50% + 2.0C last-sample dip K=12.9 (-57%)
                 *   truncated 70% + 1.0C dip              K=20.0 (-33%)
                 *   truncated 85% + 1.0C dip              K=24.4 (-19%)
                 *   truncated 95% + 0.3C dip               K=28.2  (-6%)
                 * Those are only safe on-target because autotune_engine.c's
                 * settled gate independently rejects them on absolute
                 * slope -- correct in composition, wrong in isolation,
                 * which is the same lesson this whole incident has been
                 * teaching one layer at a time.
                 *
                 * Fixed by making the break MAGNITUDE-aware: a shrink is
                 * still called converged only when it is within about ONE
                 * QUANTIZATION STEP of raw_rise, derived from the actual
                 * trace quantum and this window's own span rather than a
                 * hardcoded constant -- TRACE_QUANTUM_C (0.1 degC, the
                 * firmware's packed trace resolution, unpack_zone_trace's
                 * own (float)dc/10.0f) turns into a SLOPE quantum of
                 * TRACE_QUANTUM_C/window_span_s over this window, and
                 * multiplying by tau converts that slope quantum into the
                 * RISE-magnitude one quantum's worth of slope_end would
                 * have implied via candidate = raw_rise + tau*slope_end --
                 * i.e. exactly the scale a single quantization tick can
                 * move `candidate` by. At tau=600s and a 50s window that
                 * threshold works out to ~1.2 degC: it accepts the +/-0.1
                 * degC dither case (a genuine single quantization tick) and
                 * rejects every row of the table above (each shrinks
                 * raw_rise by several to tens of degrees, orders of
                 * magnitude past one tick) on ITS OWN merits, without
                 * leaning on the settled gate. A shrink past this threshold
                 * is a real reversal or disturbance, not noise, and
                 * extrapolation_converged stays at its pessimistic false
                 * (tau/dead_time still keep the previous iteration's
                 * values, unaffected either way -- this only changes the
                 * CONFIDENCE flag, never k_gain_c_per_duty itself). */
                float quantization_threshold_c = TRACE_QUANTUM_C * tau / window_span_s;
                float shrink_c = fabsf(raw_rise - candidate);
                extrapolation_converged = (shrink_c <= quantization_threshold_c);
                break; /* would shrink or reverse -- keep the current estimate, already tau-consistent */
            }
            bool capped = false;
            if (fabsf(candidate) > max_rise_c) {
                candidate = max_rise_c * rise_sign;
                capped = true;
            }
            float delta = fabsf(candidate - rise_inf);
            rise_inf = candidate;
            /* Round-3 review fix: refit is now attempted for EVERY accepted
             * candidate BEFORE deciding whether to stop, including one that
             * just got capped or is about to be accepted as converged --
             * the first version of this loop skipped straight to a `break`
             * on those two paths, leaving tau_s/dead_time_s one iteration
             * stale relative to the rise_inf/k_gain_c_per_duty just
             * committed to (a smaller, uncorrected tau paired with an up-
             * to-2x-corrected K). A refit the trace cannot support (target
             * levels past what was actually measured) is the one case that
             * genuinely cannot be fixed by refitting -- tau_consistent_
             * with_gain is set false there and the loop stops, keeping the
             * last tau/dead_time that DID fit successfully. */
            float target28 = baseline_c + 0.283f * rise_inf;
            float target63 = baseline_c + 0.632f * rise_inf;
            float t28, t63;
            bool refit_ok = false;
            if (find_crossing_time(samples, sample_count, target28, rise_sign, &t28) &&
                find_crossing_time(samples, sample_count, target63, rise_sign, &t63)) {
                float refit_tau = 1.5f * (t63 - t28);
                float refit_dead = t63 - refit_tau;
                if (refit_dead < 0.0f) {
                    refit_dead = 0.0f;
                }
                if (refit_tau > 0.0f) {
                    tau = refit_tau;
                    dead_time = refit_dead;
                    refit_ok = true;
                }
            }
            if (!refit_ok) {
                tau_consistent_with_gain = false;
                break; /* rise_inf/k_gain moved but tau/dead_time could not follow -- flagged, not silent */
            }
            if (delta < EXTRAPOLATION_CONVERGE_EPS_C) {
                extrapolation_converged = true;
                break; /* converged -- tau/dead_time already refit to match this rise_inf, above */
            }
            if (capped) {
                break; /* pinned at the ceiling -- refit above already matches this capped rise_inf */
            }
        }
    }

    fopdt_model_t m;
    m.k_gain_c_per_duty = rise_inf / duty_step;
    m.tau_s = tau;
    m.dead_time_s = dead_time;
    m.valid = true;
    m.settled = false; /* caller's to set -- see fopdt_model_t's own comment */
    m.tau_consistent_with_gain = tau_consistent_with_gain;
    m.extrapolation_converged = extrapolation_converged;
    m.invalid_reason[0] = '\0';
    /* Diagnostic fit inputs -- see fopdt_model_t's own comment. */
    m.baseline_c = baseline_c;
    m.final_c = final_c;
    m.raw_rise_c = raw_rise;
    m.rise_inf_c = rise_inf;
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
