// Host tests for ramp_ident.c (PID_EXPANSION_PLAN.md 3.3, "Dynamics from
// ramps"). See ramp_ident.h for the design: an excitation gate that must
// REFUSE a flat/well-tracked ramp segment before any fit math runs, and a
// two-point (28.3%/63.2%) reaction-curve fit on the detrended response of
// segments that pass the gate.
//
// Every synthetic trace below is built at 0.1C QUANTIZATION on actual_c --
// this repo has a documented bug class (project_idealized_test_input_bug_
// class) where unquantized synthetic data hid whole branches while the
// suite reported green, so an unquantized trace here would not actually
// exercise the real failure modes this module has to survive.
#include "test_common.h"
#include "../drivers/ramp_ident.h"

#include <math.h>
#include <string.h>

#define TRACE_MAX 128

static float quantize_c(float c)
{
    return roundf(c / 0.1f) * 0.1f;
}

// Builds a segment: a linear background trend (drift_c_per_s) from t=0,
// with a duty step of `duty_delta` applied at sample index `step_idx`
// (duty = duty_before for i < step_idx, duty_before+duty_delta at and after).
// If `inject_response` is true, an ANALYTIC first-order-plus-dead-time step
// response (gain k_gain, tau tau_s, dead time l_s) is added on top of the
// background trend starting at the step; if false, actual_c continues
// exactly along the background trend (the "model already predicted this
// perfectly" / well-tracked case -- no new information for the fitter).
// actual_c is quantized to 0.1C on the way out, same as real telemetry.
static uint32_t build_segment(ramp_ident_sample_t *out, uint32_t n, float dt_s, float start_c, float drift_c_per_s,
                               uint32_t step_idx, float duty_before, float duty_delta, bool inject_response,
                               float k_gain, float tau_s, float l_s)
{
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i * dt_s;
        float trend = start_c + drift_c_per_s * t;
        float duty = (i < step_idx) ? duty_before : (duty_before + duty_delta);
        float actual = trend;
        if (inject_response && i >= step_idx) {
            float t_since_step = (float)(i - step_idx) * dt_s;
            float rise = 0.0f;
            if (t_since_step >= l_s) {
                float expected_rise = k_gain * duty_delta;
                rise = expected_rise * (1.0f - expf(-(t_since_step - l_s) / tau_s));
            }
            actual = trend + rise;
        }
        out[i].t_s = t;
        out[i].target_c = trend + 5.0f; // target running a little ahead; not used by the fit math
        out[i].actual_c = quantize_c(actual);
        out[i].duty = duty;
    }
    return n;
}

void run_test_ramp_ident(void)
{
    TEST_SECTION("ramp_ident");

    const float K = 35.0f;      // model gain, C per unit duty -- PID_EXPANSION_PLAN.md section 2 order of magnitude
    const float TAU_TRUE = 264.0f;
    const float L_TRUE = 45.0f;
    // dt chosen so the bounded post-step response window (ramp_ident.c's
    // RAMP_IDENT_MAX_RESPONSE_SAMPLES, 15 samples) comfortably reaches the
    // 63.2% crossing at this tau/L (needs ~309s after the step): 25s * 15
    // samples = 375s of window, well past that.
    const float DT = 25.0f;
    const uint32_t N = 60;      // duration = 59*25 = 1475s

    // --- Accept path: genuine step, analytic FOPDT response injected ------
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, N, DT, /*start_c*/ 40.0f, /*drift*/ 0.01f, /*step_idx*/ 10, /*duty_before*/ 0.40f,
                      /*duty_delta*/ 0.15f, /*inject_response*/ true, K, TAU_TRUE, L_TRUE);

        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);

        TEST_CHECK(r == RAMP_IDENT_OK, "genuine step: expected OK");
        TEST_CHECK(fit.valid, "genuine step: fit.valid true");
        TEST_CHECK(fit.refusal_reason[0] == '\0', "genuine step: no refusal reason on success");
        TEST_CHECK_NEAR(fit.step_duty_delta, 0.15, 0.02, "genuine step: recovered duty step ~0.15");
        // Two-point method recovers tau/L exactly on a clean analytic trace
        // modulo 0.1C quantization -- allow generous (25%) tolerance rather
        // than pin to float-exact, since that's what an on-target trace
        // will actually look like.
        TEST_CHECK_NEAR(fit.tau_s, TAU_TRUE, TAU_TRUE * 0.25, "genuine step: tau_s within 25% of true tau");
        TEST_CHECK_NEAR(fit.dead_time_s, L_TRUE, L_TRUE * 0.6 + 15.0, "genuine step: dead_time_s within tolerance of true L");
        TEST_CHECK(fit.transient_peak_c > 0.3f, "genuine step: transient_peak_c cleared the excitation floor");
    }

    // --- Refusal: duty steps, but temperature never deviates from the ------
    // pre-step trend -- the exact "well-tracked ramp, no information" case
    // the excitation gate exists to catch (ramp_ident.h's whole design
    // rationale). This is the most important negative test in this file.
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, N, DT, 40.0f, 0.01f, 10, 0.40f, 0.15f, /*inject_response*/ false, K, TAU_TRUE, L_TRUE);

        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);

        TEST_CHECK(r == RAMP_IDENT_INSUFFICIENT_TRANSIENT,
                   "well-tracked ramp (duty steps, temperature does not): must be refused as INSUFFICIENT_TRANSIENT");
        TEST_CHECK(!fit.valid, "well-tracked ramp: fit.valid false");
        TEST_CHECK(fit.refusal_reason[0] != '\0', "well-tracked ramp: refusal reason populated");
        TEST_CHECK(fit.transient_peak_c < 0.3f, "well-tracked ramp: transient_peak_c stayed under the excitation floor");
    }

    // --- Refusal: duty drifts gradually across the whole segment (span ------
    // clears the whole-segment floor) but never forms a localized step --
    // NO_STEP_EVENT.
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        for (uint32_t i = 0; i < N; i++) {
            float t = (float)i * DT;
            float duty = 0.40f + 0.15f * ((float)i / (float)(N - 1)); // 0.40 -> 0.55 spread evenly, no step
            seg[i].t_s = t;
            seg[i].actual_c = quantize_c(40.0f + 0.01f * t);
            seg[i].target_c = seg[i].actual_c + 5.0f;
            seg[i].duty = duty;
        }
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_NO_STEP_EVENT, "gradual duty drift, no localized step: NO_STEP_EVENT");
        TEST_CHECK(!fit.valid, "gradual drift: fit.valid false");
    }

    // --- Refusal: duty essentially flat, whole-segment span too small ------
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        for (uint32_t i = 0; i < N; i++) {
            float t = (float)i * DT;
            seg[i].t_s = t;
            seg[i].actual_c = quantize_c(40.0f + 0.01f * t);
            seg[i].target_c = seg[i].actual_c + 5.0f;
            seg[i].duty = 0.45f + ((i % 2) ? 0.01f : -0.01f); // +/-0.01 dither, span ~0.02
        }
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_INSUFFICIENT_DUTY_EXCITATION, "near-flat duty: INSUFFICIENT_DUTY_EXCITATION");
        TEST_CHECK(!fit.valid, "near-flat duty: fit.valid false");
    }

    // --- Refusal: too few samples --------------------------------------
    {
        ramp_ident_sample_t seg[8];
        build_segment(seg, 8, DT, 40.0f, 0.01f, 2, 0.40f, 0.15f, true, K, TAU_TRUE, L_TRUE);
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, 8, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_TOO_FEW_SAMPLES, "8 samples: TOO_FEW_SAMPLES");
    }

    // --- Refusal: too short duration (enough samples, but packed tight) ----
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, 40, 1.0f, 40.0f, 0.01f, 10, 0.40f, 0.15f, true, K, TAU_TRUE, L_TRUE); // 39s span
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, 40, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_TOO_SHORT_DURATION, "39s span: TOO_SHORT_DURATION");
    }

    // --- Refusal: non-positive model gain -------------------------------
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, N, DT, 40.0f, 0.01f, 10, 0.40f, 0.15f, true, K, TAU_TRUE, L_TRUE);
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, 0.0f, &fit);
        TEST_CHECK(r == RAMP_IDENT_INVALID_GAIN, "K=0: INVALID_GAIN");
        r = ramp_ident_fit(seg, N, -5.0f, &fit);
        TEST_CHECK(r == RAMP_IDENT_INVALID_GAIN, "K<0: INVALID_GAIN");
    }

    // --- Cooling-direction step (duty decreases) fits the same way -------
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, N, DT, 60.0f, -0.01f, 10, 0.55f, -0.15f, true, K, TAU_TRUE, L_TRUE);
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_OK, "cooling step: expected OK");
        if (fit.valid) {
            TEST_CHECK_NEAR(fit.tau_s, TAU_TRUE, TAU_TRUE * 0.25, "cooling step: tau_s within tolerance");
        }
    }

    // --- out must not be left uninitialized/stale on refusal --------------
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, N, DT, 40.0f, 0.01f, 10, 0.40f, 0.15f, false, K, TAU_TRUE, L_TRUE);
        ramp_ident_fit_t fit;
        memset(&fit, 0xAB, sizeof(fit)); // poison, to prove ramp_ident_fit resets it
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_INSUFFICIENT_TRANSIENT, "poisoned out struct: still refused correctly");
        TEST_CHECK(fit.tau_s == 0.0f, "poisoned out struct: tau_s reset to 0 on refusal");
    }
}
