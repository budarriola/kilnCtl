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

    // --- Refusal: step found, but duty keeps moving through the response --
    // window instead of holding -- RAMP_IDENT_STEP_NOT_HELD. This gate does
    // 21 of 28 real refusals on the fixture captures (ramp_ident.h) but had
    // no test at all: deleting the check left every other test green.
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        const uint32_t STEP_IDX = 10;
        for (uint32_t i = 0; i < N; i++) {
            float t = (float)i * DT;
            float duty;
            if (i < STEP_IDX) {
                duty = 0.30f; // flat pre-step baseline
            } else {
                // Jumps 0.15 at the step, then keeps climbing 0.01/sample --
                // never settles, so the response window's duty range blows
                // well past RAMP_IDENT_STEP_HOLD_TOL (0.06).
                duty = 0.45f + 0.01f * (float)(i - STEP_IDX);
            }
            seg[i].t_s = t;
            seg[i].actual_c = quantize_c(40.0f + 0.01f * t);
            seg[i].target_c = seg[i].actual_c + 5.0f;
            seg[i].duty = duty;
        }
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_STEP_NOT_HELD, "step found but duty keeps climbing afterward: STEP_NOT_HELD");
        TEST_CHECK(!fit.valid, "step not held: fit.valid false");
        TEST_CHECK(fit.refusal_reason[0] != '\0', "step not held: refusal reason populated");
    }

    // --- Refusal: step found, duty holds, transient clears the excitation --
    // floor, but the response never reaches BOTH the 28.3%/63.2% crossings
    // of the expected rise -- RAMP_IDENT_FIT_FAILED. Also untested before
    // this commit. Built with an enormous true tau (5000s) so the response
    // barely rises (~0.35C, above the 0.3C transient floor) within the
    // bounded response window but stays far short of either crossing target
    // (1.49C / 3.32C at this K/delta).
    {
        ramp_ident_sample_t seg[TRACE_MAX];
        build_segment(seg, N, DT, 40.0f, 0.01f, 10, 0.40f, 0.15f, /*inject_response*/ true, K,
                      /*tau_s*/ 5000.0f, /*l_s*/ 0.0f);
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N, K, &fit);
        TEST_CHECK(r == RAMP_IDENT_FIT_FAILED, "transient present but crossings never reached: FIT_FAILED");
        TEST_CHECK(!fit.valid, "fit failed: fit.valid false");
        TEST_CHECK(fit.refusal_reason[0] != '\0', "fit failed: refusal reason populated");
    }

    // --- Artifact reproduction at a REALISTIC tick (10-16s/sample, matching --
    // real telemetry -- ramp_ident.h notes the accept-path tests above
    // deliberately use DT=25s so 15 samples covers t63 at the TRUE bench
    // tau, which real captures never get). This is the defect this module
    // ships with, made executable: a dead-flat pre-step baseline (matching
    // the real fixture segments' near-zero fitted pre-step slope) followed
    // by an ORDINARY LINEAR climb standing in for "the response" -- no FOPDT
    // curve is injected at all, because the whole point is that the fitted
    // tau carries no information about ANY true plant tau. Per ramp_ident.h's
    // derivation, a step of delta_d with gain K into a trace that just climbs
    // linearly at rate R produces crossings at t28=0.283*K*delta_d/R and
    // t63=0.632*K*delta_d/R, so tau_fit = 1.5*(t63-t28) = 0.524*K*delta_d/R --
    // purely a function of K, delta_d, R. If a future rewrite of the
    // estimator quietly starts producing something else here, this test will
    // go red and should be read as a signal to update it deliberately, not a
    // reason to delete it.
    {
        const float DT2 = 12.0f; // realistic tick, not the 25s used above
        const uint32_t N2 = 60;  // 59*12 = 708s span
        const float K2 = 35.0f;
        const uint32_t STEP_IDX2 = 10;
        const float DUTY_BEFORE2 = 0.30f;
        const float DUTY_DELTA2 = 0.15f;
        const float R2 = 0.03318f; // C/s climb rate, chosen so t63 lands well inside the response window

        ramp_ident_sample_t seg[TRACE_MAX];
        for (uint32_t i = 0; i < N2; i++) {
            float t = (float)i * DT2;
            float duty = (i < STEP_IDX2) ? DUTY_BEFORE2 : (DUTY_BEFORE2 + DUTY_DELTA2);
            float actual;
            if (i < STEP_IDX2) {
                actual = 40.0f; // dead-flat pre-step baseline
            } else {
                float t_since_step = (float)(i - STEP_IDX2) * DT2;
                actual = 40.0f + R2 * t_since_step; // ordinary linear climb, not an FOPDT curve
            }
            seg[i].t_s = t;
            seg[i].target_c = actual + 5.0f;
            seg[i].actual_c = quantize_c(actual);
            seg[i].duty = duty;
        }
        ramp_ident_fit_t fit;
        ramp_ident_result_t r = ramp_ident_fit(seg, N2, K2, &fit);
        TEST_CHECK(r == RAMP_IDENT_OK, "realistic-tick artifact: fit accepts a pure-ramp 'response'");
        if (fit.valid) {
            float expected_artifact_tau = 0.524f * K2 * DUTY_DELTA2 / R2;
            TEST_CHECK_NEAR(fit.tau_s, expected_artifact_tau, expected_artifact_tau * 0.15f,
                             "realistic-tick artifact: fitted tau matches the 0.524*K*delta_d/R closed-form "
                             "artifact -- no true plant tau was even simulated");
        }
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
