// ramp_transient_ident.c -- see ramp_transient_ident.h and
// docs/RAMP_TRANSIENT_IDENT_DESIGN.md for the design rationale. Pure math
// only, no ESP-IDF/FreeRTOS dependency, not wired into any control path.

#include "ramp_transient_ident.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------
// Tunable thresholds.
// ---------------------------------------------------------------------

// Enough samples to plausibly contain a trend-fit window plus a real
// simulated trace.
#define RTI_MIN_SAMPLES 16u

// Short against typical dead times/tau, but a floor rather than a target --
// the duty-excitation and curvature gates do the real discriminating work.
#define RTI_MIN_DURATION_S 60.0f

// Below this whole-segment duty standard deviation there is essentially no
// excitation to identify from, regardless of shape (a flat/held duty --
// pure dwell -- converges to the same steady state independent of tau).
#define RTI_MIN_DUTY_STD 0.01f

// Number of leading samples used to fit (and remove) the pre-segment linear
// trend -- the "was the plant already rested/linear, not still decaying
// from an earlier transient" check. Small relative to RTI_MIN_SAMPLES so a
// short segment can still be evaluated.
#define RTI_TREND_SAMPLES 6u

// Max allowed fit residual (max |actual - fitted-line|) over the trend
// window before refusing as "not locally linear / still decaying". 0.1C
// quantization means every real telemetry sample already carries +-0.05C
// of rounding noise; this allows a few LSBs of slack without accepting a
// window that is visibly curved.
#define RTI_MAX_TREND_RESIDUAL_C 0.6f

// tau search range (seconds). Wide enough to span the bench-identified
// diagonal dynamics (hundreds of seconds) and a plant an order of
// magnitude faster or slower (mistuned / different kiln entirely).
#define RTI_TAU_MIN_S 10.0f
#define RTI_TAU_MAX_S 1200.0f
#define RTI_TAU_COARSE_STEPS 60u

// Dead-time search range, in whole samples (kept integer/sample-quantized
// rather than continuous -- dead time this small relative to tau does not
// need sub-sample resolution to be useful to a caller).
#define RTI_MAX_DEAD_TIME_SAMPLES 8u

// Curvature (identifiability) probe: perturb the fitted tau by this
// fraction and require the SSE to rise by at least RTI_MIN_CURVATURE_FRAC.
#define RTI_PERTURB_FRAC 0.20f
#define RTI_MIN_CURVATURE_FRAC 0.05f

// Improvement-over-null: require the best fit's SSE to be at most this
// fraction of the null (current-model) SSE.
#define RTI_MAX_SSE_RATIO_VS_NULL 0.70f

static void set_reason(rti_fit_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->refusal_reason, sizeof(out->refusal_reason), fmt, ap);
    va_end(ap);
}

static void reset_out(rti_fit_t *out)
{
    memset(out, 0, sizeof(*out));
}

// Ordinary least-squares fit of actual_c = slope*t + intercept over
// samples [0, count), t measured from samples[0].t_s. Also reports the max
// absolute residual over that window (the "how linear is this really"
// check).
static void fit_trend(const rti_sample_t *s, uint32_t count, float *out_slope, float *out_intercept,
                       float *out_max_residual)
{
    double n = 0.0, sum_t = 0.0, sum_c = 0.0, sum_tt = 0.0, sum_tc = 0.0;
    float t0 = s[0].t_s;
    for (uint32_t i = 0; i < count; i++) {
        double t = (double)(s[i].t_s - t0);
        double c = (double)s[i].actual_c;
        n += 1.0;
        sum_t += t;
        sum_c += c;
        sum_tt += t * t;
        sum_tc += t * c;
    }
    double denom = n * sum_tt - sum_t * sum_t;
    double slope = 0.0, intercept = (n > 0.0) ? sum_c / n : 0.0;
    if (fabs(denom) > 1e-9) {
        slope = (n * sum_tc - sum_t * sum_c) / denom;
        intercept = (sum_c - slope * sum_t) / n;
    }
    double max_res = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        double t = (double)(s[i].t_s - t0);
        double pred = slope * t + intercept;
        double res = fabs((double)s[i].actual_c - pred);
        if (res > max_res) {
            max_res = res;
        }
    }
    *out_slope = (float)slope;
    *out_intercept = (float)intercept;
    *out_max_residual = (float)max_res;
}

// Simulates the FOPDT model driven by the segment's own duty trace over
// its ENTIRE span (forward Euler at each sample's own dt), and returns the
// sum-of-squared residual against the actual trace. T_free is a FIXED
// (not extrapolated) free-response baseline -- the pre-segment trend's
// intercept, i.e. "what temperature the plant was at, corrected for the
// tiny near-start bias fit_trend() measures" -- so the model being fit is
// dT/dt = (K*duty_delayed - (T - T_free))/tau. Deliberately NOT
// trend_intercept + trend_slope*t: extrapolating even a tiny fitted slope
// (fit over only RTI_TREND_SAMPLES points) linearly across an entire
// multi-thousand-second segment blows up into tens of degrees of spurious
// baseline drift by the end of the segment -- confirmed empirically during
// development (a 0.014 C/s slope, fit over 6 points near t=0, extrapolated
// to +43C by t=3000s and corrupted the whole fit). The trend-residual GATE
// (RTI_TREND_RESIDUAL_TOO_LARGE, checked separately) is what actually
// polices "was this segment rested" -- it does not need the slope fed into
// the dynamics to do that job.
static double simulate_sse(const rti_sample_t *s, uint32_t n, float k_gain, float tau_s, uint32_t dead_samples,
                            float t_free)
{
    double T = (double)s[0].actual_c;
    double sse = 0.0;
    for (uint32_t i = 1; i < n; i++) {
        float dt = s[i].t_s - s[i - 1].t_s;
        if (dt <= 0.0f) {
            dt = 1.0f;
        }
        uint32_t duty_idx = (i - 1 >= dead_samples) ? (i - 1 - dead_samples) : 0;
        double duty = (double)s[duty_idx].duty;
        double dT = (k_gain * duty - (T - (double)t_free)) / (double)tau_s;
        T = T + dT * (double)dt;
        double res = T - (double)s[i].actual_c;
        sse += res * res;
    }
    return sse;
}

static double best_sse_over_dead_times(const rti_sample_t *s, uint32_t n, float k_gain, float tau_s,
                                        float t_free, uint32_t *out_best_dead_samples)
{
    double best = -1.0;
    uint32_t best_d = 0;
    for (uint32_t d = 0; d <= RTI_MAX_DEAD_TIME_SAMPLES; d++) {
        double sse = simulate_sse(s, n, k_gain, tau_s, d, t_free);
        if (best < 0.0 || sse < best) {
            best = sse;
            best_d = d;
        }
    }
    if (out_best_dead_samples) {
        *out_best_dead_samples = best_d;
    }
    return best;
}

rti_result_t rti_fit(const rti_sample_t *samples, uint32_t n, float k_gain_c_per_duty, float current_tau_s,
                      float current_dead_time_s, rti_fit_t *out)
{
    reset_out(out);

    if (n < RTI_MIN_SAMPLES) {
        out->result = RTI_TOO_FEW_SAMPLES;
        set_reason(out, "only %u samples, need >= %u", n, RTI_MIN_SAMPLES);
        return out->result;
    }
    float duration = samples[n - 1].t_s - samples[0].t_s;
    if (duration < RTI_MIN_DURATION_S) {
        out->result = RTI_TOO_SHORT_DURATION;
        set_reason(out, "span %.1fs below minimum %.1fs", (double)duration, (double)RTI_MIN_DURATION_S);
        return out->result;
    }
    if (k_gain_c_per_duty <= 0.0f) {
        out->result = RTI_INVALID_GAIN;
        set_reason(out, "k_gain_c_per_duty=%.4f must be > 0", (double)k_gain_c_per_duty);
        return out->result;
    }

    // Duty excitation gate.
    double sum_duty = 0.0, sum_duty2 = 0.0;
    float duty_min = samples[0].duty, duty_max = samples[0].duty;
    for (uint32_t i = 0; i < n; i++) {
        sum_duty += samples[i].duty;
        sum_duty2 += (double)samples[i].duty * (double)samples[i].duty;
        if (samples[i].duty < duty_min) duty_min = samples[i].duty;
        if (samples[i].duty > duty_max) duty_max = samples[i].duty;
    }
    double mean_duty = sum_duty / (double)n;
    double var_duty = sum_duty2 / (double)n - mean_duty * mean_duty;
    if (var_duty < 0.0) var_duty = 0.0;
    float duty_std = (float)sqrt(var_duty);
    out->duty_std = duty_std;
    if (duty_std < RTI_MIN_DUTY_STD) {
        out->result = RTI_INSUFFICIENT_DUTY_EXCITATION;
        set_reason(out, "duty std %.4f below floor %.4f (range %.4f-%.4f) -- flat/held duty carries no "
                        "tau information",
                   (double)duty_std, (double)RTI_MIN_DUTY_STD, (double)duty_min, (double)duty_max);
        return out->result;
    }

    // Pre-segment trend / rested-baseline gate.
    uint32_t trend_n = RTI_TREND_SAMPLES;
    if (trend_n > n) {
        trend_n = n;
    }
    float trend_slope = 0.0f, trend_intercept = 0.0f, trend_residual = 0.0f;
    fit_trend(samples, trend_n, &trend_slope, &trend_intercept, &trend_residual);
    out->trend_residual_c = trend_residual;
    if (trend_residual > RTI_MAX_TREND_RESIDUAL_C) {
        out->result = RTI_TREND_RESIDUAL_TOO_LARGE;
        set_reason(out, "pre-segment trend residual %.3fC exceeds %.3fC -- plant was still decaying "
                        "from an earlier transient, not rested",
                   (double)trend_residual, (double)RTI_MAX_TREND_RESIDUAL_C);
        return out->result;
    }
    float t_free = trend_intercept;

    // Coarse grid search over tau, best dead-time at each tau.
    double best_sse = -1.0;
    float best_tau = RTI_TAU_MIN_S;
    uint32_t best_dead_samples = 0;
    for (uint32_t k = 0; k <= RTI_TAU_COARSE_STEPS; k++) {
        float frac = (float)k / (float)RTI_TAU_COARSE_STEPS;
        float tau = RTI_TAU_MIN_S + frac * (RTI_TAU_MAX_S - RTI_TAU_MIN_S);
        uint32_t d = 0;
        double sse = best_sse_over_dead_times(samples, n, k_gain_c_per_duty, tau, t_free, &d);
        if (best_sse < 0.0 || sse < best_sse) {
            best_sse = sse;
            best_tau = tau;
            best_dead_samples = d;
        }
    }

    // Golden-section refinement around the coarse winner.
    {
        float lo = best_tau - (RTI_TAU_MAX_S - RTI_TAU_MIN_S) / (float)RTI_TAU_COARSE_STEPS;
        float hi = best_tau + (RTI_TAU_MAX_S - RTI_TAU_MIN_S) / (float)RTI_TAU_COARSE_STEPS;
        if (lo < RTI_TAU_MIN_S) lo = RTI_TAU_MIN_S;
        if (hi > RTI_TAU_MAX_S) hi = RTI_TAU_MAX_S;
        const double gr = 0.6180339887498949;
        double a = lo, b = hi;
        double c = b - gr * (b - a);
        double d2 = a + gr * (b - a);
        uint32_t dead_c = best_dead_samples, dead_d = best_dead_samples;
        double fc = best_sse_over_dead_times(samples, n, k_gain_c_per_duty, (float)c, t_free, &dead_c);
        double fd = best_sse_over_dead_times(samples, n, k_gain_c_per_duty, (float)d2, t_free, &dead_d);
        for (int iter = 0; iter < 25 && (b - a) > 0.5; iter++) {
            if (fc < fd) {
                b = d2;
                d2 = c;
                fd = fc;
                c = b - gr * (b - a);
                fc = best_sse_over_dead_times(samples, n, k_gain_c_per_duty, (float)c, t_free, &dead_c);
            } else {
                a = c;
                c = d2;
                fc = fd;
                d2 = a + gr * (b - a);
                fd = best_sse_over_dead_times(samples, n, k_gain_c_per_duty, (float)d2, t_free, &dead_d);
            }
        }
        double mid = (a + b) / 2.0;
        uint32_t dead_mid = best_dead_samples;
        double f_mid = best_sse_over_dead_times(samples, n, k_gain_c_per_duty, (float)mid, t_free, &dead_mid);
        if (f_mid < best_sse) {
            best_sse = f_mid;
            best_tau = (float)mid;
            best_dead_samples = dead_mid;
        }
    }
    out->best_sse = (float)best_sse;

    // Curvature (identifiability) gate.
    float tau_perturbed = best_tau * (1.0f + RTI_PERTURB_FRAC);
    if (tau_perturbed > RTI_TAU_MAX_S) {
        tau_perturbed = best_tau * (1.0f - RTI_PERTURB_FRAC);
    }
    double perturbed_sse = simulate_sse(samples, n, k_gain_c_per_duty, tau_perturbed, best_dead_samples, t_free);
    out->perturbed_sse = (float)perturbed_sse;
    double denom_curv = (best_sse > 1e-9) ? best_sse : 1e-9;
    double curvature_frac = (perturbed_sse - best_sse) / denom_curv;
    if (curvature_frac < (double)RTI_MIN_CURVATURE_FRAC) {
        out->result = RTI_FLAT_COST;
        set_reason(out, "cost curvature %.4f below floor %.4f at tau=%.1fs -- segment does not "
                        "distinguish candidate tau values (closed-loop identifiability failure)",
                   curvature_frac, (double)RTI_MIN_CURVATURE_FRAC, (double)best_tau);
        return out->result;
    }

    // Improvement-over-null gate.
    uint32_t current_dead_samples = 0;
    if (n >= 2) {
        float dt_est = (samples[n - 1].t_s - samples[0].t_s) / (float)(n - 1);
        if (dt_est > 0.0f) {
            current_dead_samples = (uint32_t)(current_dead_time_s / dt_est + 0.5f);
        }
    }
    if (current_dead_samples > RTI_MAX_DEAD_TIME_SAMPLES) {
        current_dead_samples = RTI_MAX_DEAD_TIME_SAMPLES;
    }
    float current_tau = (current_tau_s > 0.0f) ? current_tau_s : best_tau;
    double null_sse = simulate_sse(samples, n, k_gain_c_per_duty, current_tau, current_dead_samples, t_free);
    out->null_sse = (float)null_sse;
    double ratio = (null_sse > 1e-9) ? (best_sse / null_sse) : 1.0;
    if (ratio > (double)RTI_MAX_SSE_RATIO_VS_NULL) {
        out->result = RTI_NO_IMPROVEMENT;
        set_reason(out, "best-fit SSE is %.1f%% of the current-model SSE (need <= %.0f%%) -- this "
                        "segment tells us nothing the existing model doesn't already predict",
                   ratio * 100.0, (double)RTI_MAX_SSE_RATIO_VS_NULL * 100.0);
        return out->result;
    }

    out->result = RTI_OK;
    out->valid = true;
    out->tau_s = best_tau;
    out->dead_time_s = (float)best_dead_samples * ((n >= 2) ? (samples[n - 1].t_s - samples[0].t_s) / (float)(n - 1)
                                                             : 0.0f);
    return out->result;
}
