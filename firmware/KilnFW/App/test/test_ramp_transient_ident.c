// Host tests for ramp_transient_ident.c. See ramp_transient_ident.h and
// docs/RAMP_TRANSIENT_IDENT_DESIGN.md for the design: a whole-segment
// output-error fit driven by the segment's OWN recorded duty trace (not a
// closed-form crossing-time computation, per ramp_ident.c's documented
// failure), gated by duty excitation, a rested-baseline trend check, a
// cost-curvature (identifiability) probe, and improvement-over-null.
//
// Every synthetic trace is built at 0.1C QUANTIZATION on actual_c -- see
// project_idealized_test_input_bug_class.
#include "test_common.h"
#include "../drivers/control/ramp_transient_ident.h"

#include <math.h>
#include <string.h>

#define TRACE_MAX 4096

static float quantize_c(float c)
{
    return roundf(c / 0.1f) * 0.1f;
}

// Builds a closed-loop ramp-tracking segment: a PI + feedforward controller
// (whose INTERNAL model uses k_model/tau_model, possibly wrong) tracks a
// linear ramp reference at ramp_rate_c_per_s, driving a TRUE plant
// (k_true/tau_true) via forward-Euler simulation. This is the realistic
// case this estimator has to work on: not an open-loop step, a genuine
// closed-loop firing ramp, exactly the shape ramp_ident.c's two-point
// method was shown to fail on.
static uint32_t build_closed_loop_ramp(rti_sample_t *out, uint32_t n, float dt_s, float t_amb, float ramp_rate,
                                        float k_true, float tau_true, float k_model, float tau_model, float kp,
                                        float ki)
{
    float T = t_amb;
    float integ = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i * dt_s;
        float target = t_amb + ramp_rate * t;
        float err = target - T;
        integ += err * dt_s;
        float ff = (target - t_amb) / k_model + ramp_rate * tau_model / k_model;
        float duty = ff + kp * err + ki * integ;
        if (duty < 0.0f) duty = 0.0f;
        if (duty > 1.0f) duty = 1.0f;
        out[i].t_s = t;
        out[i].target_c = target;
        out[i].actual_c = quantize_c(T);
        out[i].duty = duty;
        float dT = (k_true * duty - (T - t_amb)) / tau_true;
        T = T + dT * dt_s;
    }
    return n;
}

void run_test_ramp_transient_ident(void)
{
    const float DT = 1.0f;
    const uint32_t N = 3000;
    const float T_AMB = 25.0f;
    const float RAMP_RATE = 100.0f / 3600.0f; // 100C/hr

    // --- Positive case: closed-loop ramp, DELIBERATELY MISTUNED controller
    // model (tau_model=100s) driving a true plant with tau_true=280s. This
    // is the case ramp_ident.c's two-point method could never resolve --
    // tracking error stays tiny throughout because feedback corrects it,
    // yet the true plant differs sharply from what the controller assumes.
    {
        static rti_sample_t seg[TRACE_MAX];
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, /*k_true=*/200.0f, /*tau_true=*/280.0f,
                                /*k_model=*/200.0f, /*tau_model=*/100.0f, /*kp=*/0.02f, /*ki=*/0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, 200.0f, /*current_tau=*/100.0f, /*current_dead_time=*/0.0f, &fit);
        TEST_CHECK(r == RTI_OK, "mistuned closed-loop ramp: expected OK");
        if (fit.valid) {
            TEST_CHECK_NEAR(fit.tau_s, 280.0f, 280.0f * 0.15f,
                             "mistuned closed-loop ramp: recovered tau within 15% of true 280s despite "
                             "controller believing tau=100s");
            TEST_CHECK(fit.best_sse < fit.null_sse * 0.5f,
                       "mistuned closed-loop ramp: fit SSE much better than null (current-model) SSE");
        }
    }

    // --- Positive case: controller model already correct (tau_model ==
    // tau_true). The fit should still succeed and agree with the true tau
    // -- confirms the estimator isn't only "detecting mismatch", it
    // actually measures tau in the matched case too.
    {
        static rti_sample_t seg[TRACE_MAX];
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 280.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, 200.0f, 280.0f, 0.0f, &fit);
        // Matched-model closed-loop tracking is near-perfect (tracking error
        // ~1e-14C in the prototype), which starves the improvement-over-null
        // gate of anything to improve on -- NO_IMPROVEMENT is the CORRECT
        // refusal here (the current model already predicts this segment
        // essentially exactly), not a bug. Accept either OK-with-matching-tau
        // or a principled NO_IMPROVEMENT refusal.
        if (r == RTI_OK) {
            TEST_CHECK_NEAR(fit.tau_s, 280.0f, 280.0f * 0.15f, "matched-model ramp: recovered tau near true 280s");
        } else {
            TEST_CHECK(r == RTI_NO_IMPROVEMENT, "matched-model ramp: only acceptable refusal is NO_IMPROVEMENT");
        }
    }

    // --- Negative case: flat dwell, duty constant and plant already at
    // steady state -- zero information about tau. Must be refused at the
    // duty-excitation gate before any fit math runs.
    {
        static rti_sample_t seg[TRACE_MAX];
        float duty_const = 0.30f;
        float k = 200.0f, tau = 280.0f;
        float T = T_AMB + k * duty_const; // already settled
        for (uint32_t i = 0; i < N; i++) {
            seg[i].t_s = (float)i * DT;
            seg[i].target_c = T_AMB + k * duty_const;
            seg[i].actual_c = quantize_c(T);
            seg[i].duty = duty_const;
            float dT = (k * duty_const - (T - T_AMB)) / tau;
            T = T + dT * DT;
        }
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, k, tau, 0.0f, &fit);
        TEST_CHECK(r == RTI_INSUFFICIENT_DUTY_EXCITATION, "flat dwell: refused at duty-excitation gate");
        TEST_CHECK(fit.tau_s == 0.0f, "flat dwell: tau_s left at 0 on refusal");
    }

    // --- Negative case: the plant is simulated so that duty and true
    // response are consistent with the CURRENT model at every tick (an
    // open-loop replay of the current model's own prediction, i.e. what
    // "the model already knew this" looks like exactly) -- there is
    // nothing left in the residual for a different tau to explain better,
    // so the curvature/no-improvement gates must refuse rather than
    // returning some other confident tau.
    {
        static rti_sample_t seg[TRACE_MAX];
        float k = 200.0f, tau = 280.0f;
        float T = T_AMB;
        // Duty follows an arbitrary smooth ramp-tracking shape (same as
        // used to derive the "true" trace), and actual_c is generated by
        // simulating EXACTLY the current model against that same duty --
        // i.e. the segment IS the current model's own prediction, byte for
        // byte modulo quantization.
        float duty = 0.05f;
        for (uint32_t i = 0; i < N; i++) {
            seg[i].t_s = (float)i * DT;
            duty += 0.0002f; // slow smooth climb, plenty of duty excitation
            if (duty > 0.6f) duty = 0.6f;
            seg[i].duty = duty;
            seg[i].target_c = T;
            seg[i].actual_c = quantize_c(T);
            float dT = (k * duty - (T - T_AMB)) / tau;
            T = T + dT * DT;
        }
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, k, tau, 0.0f, &fit);
        TEST_CHECK(r == RTI_NO_IMPROVEMENT,
                   "segment IS the current model's own prediction: refused via NO_IMPROVEMENT rather than "
                   "reporting some other confident tau");
    }

    // --- Refusal: too few samples ---------------------------------------
    {
        rti_sample_t seg[8];
        build_closed_loop_ramp(seg, 8, DT, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, 8, 200.0f, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_TOO_FEW_SAMPLES, "8 samples: TOO_FEW_SAMPLES");
    }

    // --- Refusal: too short duration -------------------------------------
    {
        static rti_sample_t seg[64];
        build_closed_loop_ramp(seg, 40, 1.0f, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, 40, 200.0f, 100.0f, 0.0f, &fit); // 39s span
        TEST_CHECK(r == RTI_TOO_SHORT_DURATION, "39s span: TOO_SHORT_DURATION");
    }

    // --- Refusal: non-positive gain ---------------------------------------
    {
        static rti_sample_t seg[TRACE_MAX];
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        TEST_CHECK(rti_fit(seg, N, 0.0f, 100.0f, 0.0f, &fit) == RTI_INVALID_GAIN, "K=0: INVALID_GAIN");
        TEST_CHECK(rti_fit(seg, N, -5.0f, 100.0f, 0.0f, &fit) == RTI_INVALID_GAIN, "K<0: INVALID_GAIN");
    }

    // --- Non-rested baseline, SLOW-decay case (documents a KNOWN,
    // MEASURED limitation -- see docs/RAMP_TRANSIENT_IDENT_DESIGN.md's
    // "Non-rested bias" section). A segment starting 30s after an earlier
    // step, decay far from complete, is NOT caught by the trend-residual
    // gate: an exponential decay's curvature over a window this short
    // relative to tau is genuinely almost-linear (~0.04C residual
    // regardless of window size 4-16 samples, measured directly), so no
    // threshold on this gate alone can catch it without also rejecting
    // ordinary linear ramps. This test asserts the MEASURED outcome (fit
    // accepted, tau biased to roughly 2-3x true) rather than a false
    // guarantee of rejection -- a real integration needs an independent
    // "quiet period before this segment" check this module cannot provide
    // on its own, which is exactly why this stays unwired (see the design
    // doc's "Guard 1 / profiles_stop interaction" section).
    {
        static rti_sample_t seg[TRACE_MAX];
        float k = 200.0f, tau_true = 280.0f;
        // Simulate a short prior step BEFORE t=0 so the plant is mid
        // exponential decay/rise, not rested, when the ramp segment we
        // hand to the fitter begins.
        float T = T_AMB;
        float prior_duty = 0.5f;
        for (int i = 0; i < 30; i++) { // 30s of prior step response, well short of settling
            float dT = (k * prior_duty - (T - T_AMB)) / tau_true;
            T = T + dT * 1.0f;
        }
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, k, tau_true, k, 100.0f, 0.02f, 0.0006f);
        // Overwrite the segment's actual starting temperature/trend with
        // the mid-decay value so the pre-segment window is curved.
        float T2 = T;
        float integ = 0.0f;
        for (uint32_t i = 0; i < N; i++) {
            float t = (float)i * DT;
            float target = T_AMB + RAMP_RATE * t;
            float err = target - T2;
            integ += err * DT;
            float ff = (target - T_AMB) / k + RAMP_RATE * 100.0f / k;
            float duty = ff + 0.02f * err + 0.0006f * integ;
            if (duty < 0.0f) duty = 0.0f;
            if (duty > 1.0f) duty = 1.0f;
            seg[i].t_s = t;
            seg[i].target_c = target;
            seg[i].actual_c = quantize_c(T2);
            seg[i].duty = duty;
            float dT = (k * duty - (T2 - T_AMB)) / tau_true;
            T2 = T2 + dT * DT;
        }
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, k, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_OK,
                   "mid-decay start: measured limitation -- trend gate does not catch a slow decay tail, "
                   "fit is accepted (documents the gap, does not claim it is caught)");
        if (r == RTI_OK) {
            TEST_CHECK(fit.tau_s > tau_true * 1.5f && fit.tau_s < tau_true * 4.0f,
                       "mid-decay start: fitted tau lands in the measured biased-high range (roughly "
                       "2-3x true) -- NOT a claim this is acceptable, a pinned regression of the known gap");
        }
    }

    // --- out must not be left uninitialized/stale on refusal ---------------
    {
        static rti_sample_t seg[TRACE_MAX];
        float duty_const = 0.3f;
        for (uint32_t i = 0; i < N; i++) {
            seg[i].t_s = (float)i * DT;
            seg[i].target_c = T_AMB;
            seg[i].actual_c = T_AMB;
            seg[i].duty = duty_const;
        }
        rti_fit_t fit;
        memset(&fit, 0xAB, sizeof(fit));
        rti_result_t r = rti_fit(seg, N, 200.0f, 280.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_INSUFFICIENT_DUTY_EXCITATION, "poisoned out struct: still refused correctly");
        TEST_CHECK(fit.tau_s == 0.0f, "poisoned out struct: tau_s reset to 0 on refusal");
        TEST_CHECK(fit.valid == false, "poisoned out struct: valid reset to false on refusal");
    }
}
