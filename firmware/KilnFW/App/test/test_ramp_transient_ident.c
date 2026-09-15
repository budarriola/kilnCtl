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

// Same as build_closed_loop_ramp, but the segment's OWN starting
// temperature (start_c) is independent of the true ambient the plant
// relaxes toward (t_amb) -- every segment past the first one of a real
// firing starts hot, not at ambient. The commanded ramp continues upward
// from start_c (a genuine mid-firing continuation), while the true plant
// dynamics still relax toward t_amb. This is the axis the whole original
// test suite held constant (project review 2026-09-14) and the one that
// exposed the ambient-reference defect.
static uint32_t build_closed_loop_ramp_from(rti_sample_t *out, uint32_t n, float dt_s, float t_amb, float start_c,
                                             float ramp_rate, float k_true, float tau_true, float k_model,
                                             float tau_model, float kp, float ki)
{
    float T = start_c;
    float integ = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i * dt_s;
        float target = start_c + ramp_rate * t;
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
        rti_result_t r = rti_fit(seg, N, 200.0f, T_AMB, /*current_tau=*/100.0f, /*current_dead_time=*/0.0f, &fit);
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
        rti_result_t r = rti_fit(seg, N, 200.0f, T_AMB, 280.0f, 0.0f, &fit);
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
        rti_result_t r = rti_fit(seg, N, k, T_AMB, tau, 0.0f, &fit);
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
        rti_result_t r = rti_fit(seg, N, k, T_AMB, tau, 0.0f, &fit);
        TEST_CHECK(r == RTI_NO_IMPROVEMENT,
                   "segment IS the current model's own prediction: refused via NO_IMPROVEMENT rather than "
                   "reporting some other confident tau");
    }

    // --- Refusal: too few samples ---------------------------------------
    {
        rti_sample_t seg[8];
        build_closed_loop_ramp(seg, 8, DT, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, 8, 200.0f, T_AMB, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_TOO_FEW_SAMPLES, "8 samples: TOO_FEW_SAMPLES");
    }

    // --- Refusal: too short duration -------------------------------------
    {
        static rti_sample_t seg[64];
        build_closed_loop_ramp(seg, 40, 1.0f, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, 40, 200.0f, T_AMB, 100.0f, 0.0f, &fit); // 39s span
        TEST_CHECK(r == RTI_TOO_SHORT_DURATION, "39s span: TOO_SHORT_DURATION");
    }

    // --- Refusal: non-positive gain ---------------------------------------
    {
        static rti_sample_t seg[TRACE_MAX];
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, 200.0f, 280.0f, 200.0f, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        TEST_CHECK(rti_fit(seg, N, 0.0f, T_AMB, 100.0f, 0.0f, &fit) == RTI_INVALID_GAIN, "K=0: INVALID_GAIN");
        TEST_CHECK(rti_fit(seg, N, -5.0f, T_AMB, 100.0f, 0.0f, &fit) == RTI_INVALID_GAIN, "K<0: INVALID_GAIN");
    }

    // --- Ambient-reference regression: segment start temperature swept from
    // true ambient to +65C above it, ambient held fixed and passed
    // correctly. Before the ambient-reference fix (opus review
    // docs/audits/ramp_transient_ident_review_2026-09-14.md), fitted tau
    // inflated from ~280s (correct) to 1200s (search-ceiling saturated) as
    // this offset grew -- because the old code relaxed the model toward the
    // SEGMENT's own start temperature instead of true ambient. This is the
    // one axis (segment start vs. ambient, varied independently) the entire
    // original test suite held constant, which is why the defect shipped
    // green. Every one of these starts a genuine mid-firing ramp -- only
    // segment 1 of a real firing starts at ambient.
    {
        const float offsets[] = {0.0f, 5.0f, 10.2f, 20.0f, 35.0f, 65.0f};
        const float k = 200.0f, tau_true = 280.0f;
        for (size_t idx = 0; idx < sizeof(offsets) / sizeof(offsets[0]); idx++) {
            static rti_sample_t seg[TRACE_MAX];
            float start_c = T_AMB + offsets[idx];
            build_closed_loop_ramp_from(seg, N, DT, T_AMB, start_c, RAMP_RATE, k, tau_true, k, 100.0f, 0.02f,
                                         0.0006f);
            rti_fit_t fit;
            rti_result_t r = rti_fit(seg, N, k, T_AMB, 100.0f, 0.0f, &fit);
            TEST_CHECK(r == RTI_OK, "ambient sweep: expected OK regardless of start-above-ambient offset");
            if (r == RTI_OK) {
                TEST_CHECK_NEAR(fit.tau_s, tau_true, tau_true * 0.20f,
                                 "ambient sweep: fitted tau stays within 20% of true 280s at every start "
                                 "offset above ambient -- the ambient-reference defect inflated this to "
                                 "31%-255% wrong (and the search ceiling entirely) as offset grew");
            }
        }
    }

    // --- Non-rested baseline, SLOW-decay case -- corrected regression.
    // This is the same segment the design doc used to (mis)document as a
    // "slow decay tail the trend gate can't see" limitation. The 2026-09-14
    // review showed the tau=658s/true=280s result it pinned was NOT the
    // decay tail (removing the decay entirely and starting at the same
    // 35.2C-above-ambient offset reproduces ~640s) -- it was this same
    // ambient-reference defect wearing a different label. With the fix,
    // this segment (prior 30s decay step, genuinely non-rested, trend
    // residual still not flagged by RTI_TREND_RESIDUAL_TOO_LARGE for the
    // reasons the doc gives) now recovers tau close to the true value: the
    // remaining "quiet period" gap the doc calls out is real but small,
    // not the 2-3x bias previously measured and previously misattributed.
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
        // Ramp continues from the decayed (non-rested) starting point, true
        // ambient unchanged.
        build_closed_loop_ramp_from(seg, N, DT, T_AMB, T, RAMP_RATE, k, tau_true, k, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, k, T_AMB, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_OK, "mid-decay start: still accepted (trend gate genuinely can't see a slow "
                                 "decay this shallow), but no longer badly wrong now ambient is correct");
        if (r == RTI_OK) {
            TEST_CHECK_NEAR(fit.tau_s, tau_true, tau_true * 0.25f,
                             "mid-decay start: fitted tau now close to true 280s (previously 640-658s, "
                             "a 2-3x bias from the ambient-reference defect, not the decay tail)");
        }
    }

    // --- RTI_FLAT_COST, made reachable: caller-supplied k_gain badly
    // UNDERSTATED. Curvature was found unreachable across 1200 constructed
    // no-information segments in the 2026-09-14 review when only segment
    // shape/duration/amplitude were varied -- but a badly wrong CALLER gain
    // (a real possibility: K comes from a prior, possibly stale, fit) makes
    // every candidate tau fit the trace about equally (badly), because the
    // dominant residual is the gain mismatch, not a tau mismatch, so
    // perturbing tau barely moves the SSE. Confirmed by direct simulation
    // before this test was written (K=10 against a true K=200: curvature
    // fraction ~0.0001, four orders of magnitude below the 0.05 floor).
    {
        static rti_sample_t seg[TRACE_MAX];
        float k_true = 200.0f, tau_true = 280.0f;
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, k_true, tau_true, k_true, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, /*k_gain (badly wrong)=*/10.0f, T_AMB, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_FLAT_COST, "badly understated caller gain: refused via FLAT_COST -- the cost "
                                        "surface genuinely does not distinguish tau values when K itself "
                                        "is this wrong");
    }

    // --- RTI_TREND_RESIDUAL_TOO_LARGE, given real coverage. A fast step
    // INSIDE the trend window (the case the gate is designed to catch, per
    // its own doc comment) must trip it. Reproduces the review's
    // independent confirmation (residual 2.971C against the 0.6C floor) on
    // the unmodified production function.
    {
        static rti_sample_t seg[TRACE_MAX];
        float k = 200.0f, tau_true = 280.0f;
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, k, tau_true, k, 100.0f, 0.02f, 0.0006f);
        // Inject a sharp step into the first RTI_TREND_SAMPLES(=6) samples
        // themselves so the pre-segment window is visibly curved/stepped,
        // not just non-rested-but-locally-linear like the decay case above.
        seg[2].actual_c = quantize_c(seg[2].actual_c + 6.0f);
        seg[3].actual_c = quantize_c(seg[3].actual_c + 6.0f);
        seg[4].actual_c = quantize_c(seg[4].actual_c + 6.0f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, k, T_AMB, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_TREND_RESIDUAL_TOO_LARGE,
                   "step inside the trend window: refused via TREND_RESIDUAL_TOO_LARGE");
        TEST_CHECK(fit.trend_residual_c > 0.6f, "step inside trend window: measured residual exceeds the floor");
    }

    // --- E1 regression: command fixed, TRUE plant varied. Confirms the fix
    // did not disturb the estimator's core property -- it tracks the real
    // plant across a wide tau range, not merely a fixed value.
    {
        const float taus[] = {80.0f, 280.0f, 700.0f};
        const float k = 200.0f;
        for (size_t idx = 0; idx < sizeof(taus) / sizeof(taus[0]); idx++) {
            static rti_sample_t seg[TRACE_MAX];
            build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, k, taus[idx], k, 100.0f, 0.02f, 0.0006f);
            rti_fit_t fit;
            rti_result_t r = rti_fit(seg, N, k, T_AMB, 100.0f, 0.0f, &fit);
            TEST_CHECK(r == RTI_OK, "E1 plant sweep: expected OK");
            if (r == RTI_OK) {
                TEST_CHECK_NEAR(fit.tau_s, taus[idx], taus[idx] * 0.15f,
                                 "E1 plant sweep: fitted tau tracks the true plant across a wide range");
            }
        }
    }

    // --- E2 regression: TRUE plant fixed, commanded ramp rate swept. The
    // estimate must stay flat -- it should not be a disguised function of
    // the commanded ramp rate (that was ramp_ident.c's defect).
    {
        const float ramp_rates_per_hr[] = {40.0f, 100.0f, 220.0f};
        const float k = 200.0f, tau_true = 280.0f;
        float fitted[3];
        for (size_t idx = 0; idx < sizeof(ramp_rates_per_hr) / sizeof(ramp_rates_per_hr[0]); idx++) {
            static rti_sample_t seg[TRACE_MAX];
            build_closed_loop_ramp(seg, N, DT, T_AMB, ramp_rates_per_hr[idx] / 3600.0f, k, tau_true, k, 100.0f,
                                    0.02f, 0.0006f);
            rti_fit_t fit;
            rti_result_t r = rti_fit(seg, N, k, T_AMB, 100.0f, 0.0f, &fit);
            TEST_CHECK(r == RTI_OK, "E2 ramp-rate sweep: expected OK");
            // Absolute anchor as well as the mutual-flatness checks below. A
            // pure "are the three estimates close to each other" assertion
            // passes VACUOUSLY when a broken estimator saturates all three at
            // the same search bound -- demonstrated 2026-09-15 by a sabotage
            // that made every fit return 1200 s, which E1 caught and E2 did
            // not. Flatness is only evidence when the flat value is also right.
            if (r == RTI_OK) {
                TEST_CHECK_NEAR(fit.tau_s, tau_true, tau_true * 0.15f,
                                 "E2 ramp-rate sweep: each estimate is also individually near the true "
                                 "plant tau, not merely equal to its siblings");
            }
            fitted[idx] = (r == RTI_OK) ? fit.tau_s : -1.0f;
        }
        if (fitted[0] > 0.0f && fitted[1] > 0.0f && fitted[2] > 0.0f) {
            TEST_CHECK_NEAR(fitted[0], fitted[1], tau_true * 0.10f,
                             "E2 ramp-rate sweep: estimate flat across a 5.5x ramp-rate change (40 vs 100C/hr)");
            TEST_CHECK_NEAR(fitted[2], fitted[1], tau_true * 0.10f,
                             "E2 ramp-rate sweep: estimate flat across a 5.5x ramp-rate change (220 vs 100C/hr)");
        }
    }

    // --- RTI_INVALID_AMBIENT: a non-finite caller ambient must be refused for
    // the CALLER's reason, not the segment's. Before this gate existed, a NaN
    // ambient poisoned every candidate SSE to NaN; every "is this better"
    // comparison against NaN is false, so the tau search never updated its
    // running best and the module fell out of the bottom reporting
    // RTI_NO_IMPROVEMENT -- "this segment tells us nothing the existing model
    // doesn't already predict" -- about a perfectly informative segment.
    // (2026-09-15 adversarial re-review.)
    {
        static rti_sample_t seg[TRACE_MAX];
        const float k = 200.0f;
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, k, 280.0f, k, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        TEST_CHECK(rti_fit(seg, N, k, NAN, 100.0f, 0.0f, &fit) == RTI_INVALID_AMBIENT,
                   "ambient_c=NaN: refused as INVALID_AMBIENT, not blamed on the segment");
        TEST_CHECK(fit.valid == false, "ambient_c=NaN: not reported valid");
        TEST_CHECK(rti_fit(seg, N, k, INFINITY, 100.0f, 0.0f, &fit) == RTI_INVALID_AMBIENT,
                   "ambient_c=+Inf: refused as INVALID_AMBIENT");
        TEST_CHECK(rti_fit(seg, N, k, -INFINITY, 100.0f, 0.0f, &fit) == RTI_INVALID_AMBIENT,
                   "ambient_c=-Inf: refused as INVALID_AMBIENT");
        // Same treatment for a non-finite gain, which previously slipped past
        // the `<= 0.0f` test (NaN compares false against everything).
        TEST_CHECK(rti_fit(seg, N, NAN, T_AMB, 100.0f, 0.0f, &fit) == RTI_INVALID_GAIN,
                   "k_gain=NaN: refused as INVALID_GAIN");
        // A FINITE but wrong ambient is deliberately NOT gated -- it cannot be,
        // see the header's accuracy-contract block. Pinned here so a future
        // reader does not mistake the gate above for protection against it:
        // ambient overstated by 5C returns a confident, badly wrong tau.
        rti_fit_t wrong;
        rti_result_t rw = rti_fit(seg, N, k, T_AMB + 5.0f, 100.0f, 0.0f, &wrong);
        TEST_CHECK(rw == RTI_OK, "ambient overstated by 5C: still accepted (documented, unfixable here)");
        if (rw == RTI_OK) {
            TEST_CHECK(wrong.tau_s > 280.0f * 1.5f,
                       "ambient overstated by 5C: fitted tau inflates past +50% with every gate passing -- "
                       "this is the caller-contract hazard the header documents, not a gate failure");
        }
    }

    // --- RTI_TAU_AT_SEARCH_BOUND: a fit that settles ON the tau search
    // ceiling is the bound, not a measurement. Reached here from a plausible
    // input -- a caller gain overstated by 3x (K from a stale prior fit) --
    // which before this gate returned RTI_OK, valid=true, tau=1200.0 and an
    // empty refusal_reason. (2026-09-15 adversarial re-review.)
    {
        static rti_sample_t seg[TRACE_MAX];
        const float k_true = 200.0f;
        build_closed_loop_ramp(seg, N, DT, T_AMB, RAMP_RATE, k_true, 280.0f, k_true, 100.0f, 0.02f, 0.0006f);
        rti_fit_t fit;
        rti_result_t r = rti_fit(seg, N, /*k_gain overstated 3x=*/600.0f, T_AMB, 100.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_TAU_AT_SEARCH_BOUND,
                   "overstated caller gain driving the fit to the tau ceiling: refused at the search bound "
                   "instead of reporting tau=RTI_TAU_MAX_S as a confident answer");
        TEST_CHECK(fit.valid == false, "search-bound refusal: not reported valid");
        TEST_CHECK(fit.refusal_reason[0] != '\0', "search-bound refusal: carries a refusal reason");
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
        rti_result_t r = rti_fit(seg, N, 200.0f, T_AMB, 280.0f, 0.0f, &fit);
        TEST_CHECK(r == RTI_INSUFFICIENT_DUTY_EXCITATION, "poisoned out struct: still refused correctly");
        TEST_CHECK(fit.tau_s == 0.0f, "poisoned out struct: tau_s reset to 0 on refusal");
        TEST_CHECK(fit.valid == false, "poisoned out struct: valid reset to false on refusal");
    }
}
