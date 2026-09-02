// ramp_ident.c -- see ramp_ident.h for the design rationale (why most ramps
// carry no information, the excitation gate, the fit method, and the
// coupling caveat). Pure math only.

#include "ramp_ident.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------
// Tunable thresholds. Each is documented at its point of use below with
// the reasoning for its numeric value; see ramp_ident.h's top comment for
// how they compose into the excitation gate.
// ---------------------------------------------------------------------

// Need enough of a trace to plausibly contain a pre-step baseline window,
// a step, and a post-step response window with room to spare.
#define RAMP_IDENT_MIN_SAMPLES 20u

// Zone control ticks run a few seconds apart in practice; 60 s is short
// against the identified dead times (34-53 s diagonal, higher off-
// diagonal) in PID_EXPANSION_PLAN.md section 2, but this is a floor, not a
// target -- the step/response window checks below do the real work.
#define RAMP_IDENT_MIN_DURATION_S 60.0f

// Below this whole-segment duty range there is essentially no excitation
// to speak of, regardless of shape -- refuse before even looking for a
// step. 0.10 (10 duty points out of 100) is well above ordinary feedback
// jitter on a settled loop and well below what a genuine ramp-entry or
// mid-ramp correction produces.
#define RAMP_IDENT_MIN_DUTY_SPAN 0.10f

// Width of the leading/trailing smoothing windows used to locate a step
// event (samples, not seconds -- ticks are roughly evenly spaced within a
// segment in practice).
#define RAMP_IDENT_STEP_WINDOW 3u

// Minimum smoothed duty change to call something a "step" rather than
// ordinary ramp-tracking drift.
#define RAMP_IDENT_MIN_STEP_DUTY 0.05f

// Samples required strictly before/after the step index to fit a baseline
// trend and observe a response, respectively.
#define RAMP_IDENT_PRE_STEP_MIN_SAMPLES 4u
#define RAMP_IDENT_MIN_RESPONSE_SAMPLES 8u

// The discriminator for "well-tracked ramp, no information": the
// DETRENDED response (actual_c minus the extrapolated pre-step trend)
// must clear this magnitude somewhere in the post-step window, in the
// direction the duty step implies, or the step is deemed to have carried
// no measurable new information. Sensor quantization is 0.1 C (repo-wide
// documented bug class: idealized unquantized synthetic data hides
// exactly this kind of branch) -- 0.3 C is 3 quantization steps, well
// above single-LSB noise but well below the several-degree transients a
// genuine duty step of RAMP_IDENT_MIN_STEP_DUTY produces on this plant
// (K ~= 32-39 C/duty per PID_EXPANSION_PLAN.md section 2: a 0.05 duty step
// implies ~1.6-2.0 C of eventual rise, most of which should be visible
// within a response window sized by RAMP_IDENT_MIN_RESPONSE_SAMPLES at
// realistic tick spacing).
#define RAMP_IDENT_MIN_TRANSIENT_C 0.3f

// How much duty is allowed to keep moving, after the step, over the rest
// of the segment, before the fit is refused as contaminated by ongoing
// feedforward action rather than a held step -- see the "step must hold"
// comment at its call site. 0.06 is a little more than
// RAMP_IDENT_MIN_STEP_DUTY itself: a response window whose duty has moved
// by roughly another full step's worth after the "step" is clearly not
// holding.
#define RAMP_IDENT_STEP_HOLD_TOL 0.06f

// The hold check (and the crossing search below it) only look at a BOUNDED
// window after the step, not the whole rest of a possibly-long ramp
// segment. Validating against real captures showed why this bound
// matters: a ramp segment's duty legitimately resumes its own ordinary
// climb well after a step-like entry transient settles (feedforward
// keeps tracking the moving target), and none of that later, unrelated
// motion should count against "did duty hold long enough to see THIS
// step's own response". 15 samples is generous against this plant's
// observed closed-loop response scale (tens of seconds of apparent lag)
// while still bounding the window to something the step's own transient
// plausibly finishes within.
#define RAMP_IDENT_MAX_RESPONSE_SAMPLES 15u

static void set_reason(ramp_ident_fit_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->refusal_reason, sizeof(out->refusal_reason), fmt, ap);
    va_end(ap);
}

static void reset_out(ramp_ident_fit_t *out)
{
    memset(out, 0, sizeof(*out));
}

// Ordinary least-squares fit of actual_c = slope*t + intercept over
// samples [lo, hi), returning slope and the intercept evaluated AT t0
// (rather than at t=0) so callers can extrapolate directly from a chosen
// reference point without a second subtraction.
static void fit_trend(const ramp_ident_sample_t *s, uint32_t lo, uint32_t hi, float t0, float *out_slope,
                       float *out_value_at_t0)
{
    double n = 0.0, sum_t = 0.0, sum_c = 0.0, sum_tt = 0.0, sum_tc = 0.0;
    for (uint32_t i = lo; i < hi; i++) {
        double t = (double)s[i].t_s - (double)t0;
        double c = (double)s[i].actual_c;
        n += 1.0;
        sum_t += t;
        sum_c += c;
        sum_tt += t * t;
        sum_tc += t * c;
    }
    double denom = n * sum_tt - sum_t * sum_t;
    double slope, intercept_at_t0;
    if (n < 2.0 || fabs(denom) < 1e-9) {
        // Degenerate (all samples at the same t, or only one sample) --
        // caller-side minimums prevent this in practice, but fall back to
        // a flat trend at the mean rather than dividing by ~0.
        slope = 0.0;
        intercept_at_t0 = (n > 0.0) ? (sum_c / n) : (double)s[lo].actual_c;
    } else {
        slope = (n * sum_tc - sum_t * sum_c) / denom;
        double mean_t = sum_t / n;
        double mean_c = sum_c / n;
        intercept_at_t0 = mean_c - slope * mean_t; // value of the fit AT t = t0 (mean_t is relative to t0)
    }
    *out_slope = (float)slope;
    *out_value_at_t0 = (float)intercept_at_t0;
}

ramp_ident_result_t ramp_ident_fit(const ramp_ident_sample_t *samples, uint32_t n, float k_gain_c_per_duty,
                                    ramp_ident_fit_t *out)
{
    reset_out(out);

    if (n < RAMP_IDENT_MIN_SAMPLES) {
        out->result = RAMP_IDENT_TOO_FEW_SAMPLES;
        set_reason(out, "only %u/%u samples", (unsigned)n, (unsigned)RAMP_IDENT_MIN_SAMPLES);
        return out->result;
    }

    float duration = samples[n - 1].t_s - samples[0].t_s;
    if (duration < RAMP_IDENT_MIN_DURATION_S) {
        out->result = RAMP_IDENT_TOO_SHORT_DURATION;
        set_reason(out, "segment spans %.1fs, need >= %.1fs", (double)duration, (double)RAMP_IDENT_MIN_DURATION_S);
        return out->result;
    }

    if (!(k_gain_c_per_duty > 0.0f)) {
        out->result = RAMP_IDENT_INVALID_GAIN;
        set_reason(out, "model gain %.4f is not positive", (double)k_gain_c_per_duty);
        return out->result;
    }

    float duty_min = samples[0].duty, duty_max = samples[0].duty;
    for (uint32_t i = 1; i < n; i++) {
        if (samples[i].duty < duty_min) duty_min = samples[i].duty;
        if (samples[i].duty > duty_max) duty_max = samples[i].duty;
    }
    float duty_span = duty_max - duty_min;
    if (duty_span < RAMP_IDENT_MIN_DUTY_SPAN) {
        out->result = RAMP_IDENT_INSUFFICIENT_DUTY_EXCITATION;
        set_reason(out, "duty range %.3f over whole segment, need >= %.3f", (double)duty_span,
                    (double)RAMP_IDENT_MIN_DUTY_SPAN);
        return out->result;
    }

    // Locate the strongest step-like event anywhere in the segment: scan
    // every candidate center index that leaves room for both a pre-step
    // baseline-fit window and a post-step response window, and take the
    // one with the largest |smoothed after - smoothed before| duty
    // difference.
    uint32_t best_idx = 0;
    float best_delta = 0.0f;
    bool found_candidate = false;
    uint32_t W = RAMP_IDENT_STEP_WINDOW;
    for (uint32_t i = RAMP_IDENT_PRE_STEP_MIN_SAMPLES; i + RAMP_IDENT_MIN_RESPONSE_SAMPLES < n; i++) {
        uint32_t before_lo = (i >= W) ? (i - W) : 0;
        uint32_t after_hi = (i + W <= n) ? (i + W) : n;
        if (before_lo >= i || i >= after_hi) continue;

        double before_sum = 0.0;
        uint32_t before_n = 0;
        for (uint32_t k = before_lo; k < i; k++) {
            before_sum += samples[k].duty;
            before_n++;
        }
        double after_sum = 0.0;
        uint32_t after_n = 0;
        for (uint32_t k = i; k < after_hi; k++) {
            after_sum += samples[k].duty;
            after_n++;
        }
        if (before_n == 0 || after_n == 0) continue;
        float delta = (float)(after_sum / after_n - before_sum / before_n);
        if (!found_candidate || fabsf(delta) > fabsf(best_delta)) {
            found_candidate = true;
            best_delta = delta;
            best_idx = i;
        }
    }

    if (!found_candidate || fabsf(best_delta) < RAMP_IDENT_MIN_STEP_DUTY) {
        out->result = RAMP_IDENT_NO_STEP_EVENT;
        set_reason(out, "largest duty step found is %.3f, need >= %.3f", (double)fabsf(best_delta),
                    (double)RAMP_IDENT_MIN_STEP_DUTY);
        return out->result;
    }

    uint32_t step_index = best_idx;
    out->step_index = step_index;
    out->step_duty_delta = best_delta;

    // The two-point method (like pid_autotune_fit_fopdt()'s clean step
    // test) assumes duty STEPS and then HOLDS while the response is
    // observed. Real mid-ramp segments often violate this: duty keeps
    // climbing after the "step" because the feedforward term is actively
    // tracking a moving target, not settling to a new constant level --
    // found empirically validating against tools/PcTools/tests/fixtures/
    // plant_sim/*.jsonl (see ramp_ident.h's validation note), where several
    // real segments show duty rising continuously through the whole
    // response window. Fitting the two-point crossings against a duty
    // trace that keeps moving after the step folds the CONTINUED duty
    // increase into the apparent temperature rise, which reaches the
    // 63.2% target faster than the plant's own open-loop lag would --
    // biasing tau low. So require duty to have roughly PLATEAUED over the
    // response window before trusting the fit: its range there must stay
    // within RAMP_IDENT_STEP_HOLD_TOL of the step's own smoothed after-
    // level, or refuse rather than fit a closed-loop-contaminated tau.
    uint32_t hold_hi = (step_index + RAMP_IDENT_MAX_RESPONSE_SAMPLES < n) ? (step_index + RAMP_IDENT_MAX_RESPONSE_SAMPLES) : n;
    {
        float hold_min = samples[step_index].duty, hold_max = samples[step_index].duty;
        for (uint32_t j = step_index; j < hold_hi; j++) {
            if (samples[j].duty < hold_min) hold_min = samples[j].duty;
            if (samples[j].duty > hold_max) hold_max = samples[j].duty;
        }
        float hold_range = hold_max - hold_min;
        if (hold_range > RAMP_IDENT_STEP_HOLD_TOL) {
            out->result = RAMP_IDENT_STEP_NOT_HELD;
            set_reason(out,
                        "duty stepped %.3f but kept moving afterward (range %.3f over the response window, "
                        "need <= %.3f) -- feedforward is still actively tracking a moving target, not holding "
                        "a level long enough to observe the plant's own lag",
                        (double)best_delta, (double)hold_range, (double)RAMP_IDENT_STEP_HOLD_TOL);
            return out->result;
        }
    }

    // Baseline trend: fit actual_c vs t over the samples strictly before
    // the step, evaluated at the step's own t_s so it can be extrapolated
    // forward directly.
    float pre_slope, pre_value_at_step;
    fit_trend(samples, 0, step_index, samples[step_index].t_s, &pre_slope, &pre_value_at_step);
    out->pre_step_slope_c_per_s = pre_slope;

    // Detrend the post-step window: response[j] = actual_c[j] - (linear
    // extrapolation of the pre-step trend to t[j]). By construction
    // response[step_index] ~= 0 (t[step_index] is the trend's own
    // reference point).
    uint32_t resp_lo = step_index;
    uint32_t resp_hi = hold_hi; // same bounded window the hold check just validated
    uint32_t resp_n = resp_hi - resp_lo;
    // Bounded stack buffer: callers hand in one ramp segment, not an
    // unbounded stream -- 512 samples covers a multi-minute segment at
    // sub-second ticks with headroom.
    if (resp_n > 512u) resp_n = 512u;
    resp_hi = resp_lo + resp_n;

    float response[512];
    float transient_peak = 0.0f;
    uint32_t peak_idx = resp_lo;
    for (uint32_t j = resp_lo; j < resp_hi; j++) {
        float trend_at_j = pre_value_at_step + pre_slope * (samples[j].t_s - samples[step_index].t_s);
        float r = samples[j].actual_c - trend_at_j;
        response[j - resp_lo] = r;
        if (fabsf(r) > transient_peak) {
            transient_peak = fabsf(r);
            peak_idx = j;
        }
    }
    out->transient_peak_c = transient_peak;
    (void)peak_idx;

    if (transient_peak < RAMP_IDENT_MIN_TRANSIENT_C) {
        out->result = RAMP_IDENT_INSUFFICIENT_TRANSIENT;
        set_reason(out,
                    "duty stepped %.3f but temperature never deviated more than %.3fC from its pre-step trend "
                    "(need >= %.3fC) -- well-tracked ramp, no usable dynamics information",
                    (double)best_delta, (double)transient_peak, (double)RAMP_IDENT_MIN_TRANSIENT_C);
        return out->result;
    }

    // Expected asymptotic rise implied by the caller's model gain and this
    // step's duty delta -- used only to PLACE the 28.3%/63.2% targets, per
    // pid_autotune_fit_fopdt()'s two-point method (pid_autotune.c), not
    // re-fitted here.
    float expected_rise = k_gain_c_per_duty * best_delta;
    float dir = (expected_rise >= 0.0f) ? 1.0f : -1.0f;
    float target28 = 0.283f * expected_rise;
    float target63 = 0.632f * expected_rise;

    float t28 = 0.0f, t63 = 0.0f;
    bool got28 = false, got63 = false;
    for (uint32_t j = resp_lo + 1; j < resp_hi && !(got28 && got63); j++) {
        float a = response[j - 1 - resp_lo];
        float b = response[j - resp_lo];
        if (!got28) {
            bool crossed = (dir >= 0.0f) ? (a < target28 && b >= target28) : (a > target28 && b <= target28);
            if (crossed) {
                float span = b - a;
                float frac = (fabsf(span) < 1e-6f) ? 0.0f : (target28 - a) / span;
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                t28 = samples[j - 1].t_s + frac * (samples[j].t_s - samples[j - 1].t_s);
                got28 = true;
            }
        }
        if (!got63) {
            bool crossed = (dir >= 0.0f) ? (a < target63 && b >= target63) : (a > target63 && b <= target63);
            if (crossed) {
                float span = b - a;
                float frac = (fabsf(span) < 1e-6f) ? 0.0f : (target63 - a) / span;
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                t63 = samples[j - 1].t_s + frac * (samples[j].t_s - samples[j - 1].t_s);
                got63 = true;
            }
        }
    }
    if (!got28 || !got63) {
        out->result = RAMP_IDENT_FIT_FAILED;
        set_reason(out, "response never reached both the 28.3%% and 63.2%% crossings of the %.3fC expected rise",
                    (double)expected_rise);
        return out->result;
    }

    float t_step = samples[step_index].t_s;
    float t28_rel = t28 - t_step;
    float t63_rel = t63 - t_step;
    float tau = 1.5f * (t63_rel - t28_rel);
    float dead_time = t63_rel - tau;

    if (!(tau > 0.0f) || dead_time < 0.0f) {
        out->result = RAMP_IDENT_FIT_FAILED;
        set_reason(out, "t28/t63 crossing times out of order (t28=%.2fs t63=%.2fs rel. to step)", (double)t28_rel,
                    (double)t63_rel);
        return out->result;
    }

    out->result = RAMP_IDENT_OK;
    out->valid = true;
    out->tau_s = tau;
    out->dead_time_s = dead_time;
    out->refusal_reason[0] = '\0';
    return out->result;
}
