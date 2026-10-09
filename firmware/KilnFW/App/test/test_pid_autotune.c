// Validates pid_autotune.c's FOPDT fit against sim_plant.c's known ground
// truth -- TODO.md 6A.4/6A.8's "autotune against the sim first... the only
// place the identification math can be validated exactly."
#include "test_common.h"
#include "../drivers/control/pid_autotune.h"
#include "sim_plant.h"
#include <string.h>

#define MAX_SAMPLES 4096

/* Defined below, after the step-test checks it is called from -- the relay
 * section carries a long explanation of what it can and cannot assert, and
 * that belongs next to the code it explains rather than at the top of the
 * file. */
static void run_test_pid_autotune_relay(void);
static void run_test_pid_autotune_magnitude_aware_convergence(void);

void run_test_pid_autotune(void)
{
    TEST_SECTION("pid_autotune");

    /* Pure first-order plant (no sensor delay/lag) so the fitted K/tau/L can
     * be checked against exact ground truth: tau_true = thermal_mass/loss,
     * K_true = heater_power/loss, L_true = 0. */
    sim_plant_cfg_t cfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 5000.0f,
        .heater_power_w = 2000.0f,
        .loss_coeff_w_per_c = 5.0f, /* tau_true=1000s, K_true=400C/duty */
        .sensor_delay_s = 0.0f,
        .sensor_lag_tau_s = 0.0f,
    };
    float tau_true = cfg.thermal_mass_j_per_c / cfg.loss_coeff_w_per_c;
    float k_true = cfg.heater_power_w / cfg.loss_coeff_w_per_c;

    sim_plant_state_t plant;
    sim_plant_reset(&plant, &cfg);

    float dt_s = 10.0f;
    float baseline_duty = 0.3f;

    /* Run to steady state at the baseline duty before stepping. */
    for (int i = 0; i < (int)(5.0f * tau_true / dt_s); i++) {
        sim_plant_step(&plant, &cfg, baseline_duty, dt_s);
    }
    float baseline_c = plant.sensor_c;

    float step_duty = baseline_duty + 0.5f;
    static autotune_sample_t samples[MAX_SAMPLES];
    int n = 0;
    int total_steps = (int)(5.0f * tau_true / dt_s);
    for (int i = 0; i < total_steps && n < MAX_SAMPLES; i++) {
        sim_plant_step(&plant, &cfg, step_duty, dt_s);
        samples[n].t_s = (float)(i + 1) * dt_s;
        samples[n].measurement_c = plant.sensor_c;
        n++;
    }

    fopdt_model_t model = pid_autotune_fit_fopdt(samples, n, baseline_c, step_duty - baseline_duty);
    TEST_CHECK(model.valid, "fit succeeds on a clean first-order step response");
    TEST_CHECK_NEAR(model.k_gain_c_per_duty, k_true, k_true * 0.02, "fitted K within 2% of sim's true gain");
    TEST_CHECK_NEAR(model.tau_s, tau_true, tau_true * 0.05, "fitted tau within 5% of sim's true time constant");
    TEST_CHECK(model.dead_time_s < 20.0f, "fitted dead time near 0 for a plant with no real dead time");
    /* Item (4), round-3 review, positive case: a trace already at (near)
     * steady state has slope_end ~= 0, so the very first candidate is
     * already within EXTRAPOLATION_CONVERGE_EPS_C of raw_rise and the loop
     * converges immediately, WITH a successful tau/dead_time refit at that
     * point -- both new fields must read true here. */
    TEST_CHECK(model.tau_consistent_with_gain, "a fully-settled trace's tau must be consistent with K");
    TEST_CHECK(model.extrapolation_converged, "a fully-settled trace's extrapolation must converge immediately");

    /* 2026-09-01 review fix (item 1): asymptote extrapolation. A trace
     * truncated at ~2*tau (instead of the 5*tau one above) has only reached
     * 1-exp(-2) = 86.5% of its true steady-state rise -- fitting the OLD way
     * (K = samples[last].measurement_c - baseline_c, no extrapolation) would
     * recover K biased low by roughly that same ~13.5%, which is the exact
     * mechanism behind the two real overshoot incidents this whole pass
     * exists to fix (measured 21.74/31.96 degC/duty, both low). With
     * pid_autotune_fit_fopdt()'s asymptote extrapolation (rise_inf =
     * rise_last + tau*slope_end, using the SAME tau this two-point fit
     * already recovers), K should come back within a few percent of truth
     * even from this short a trace -- a much tighter bound than the ~13.5%
     * bias the un-extrapolated formula would produce, which is the
     * regression this test guards against. */
    {
        sim_plant_state_t plant2;
        sim_plant_reset(&plant2, &cfg);
        for (int i = 0; i < (int)(5.0f * tau_true / dt_s); i++) {
            sim_plant_step(&plant2, &cfg, baseline_duty, dt_s);
        }
        float baseline2_c = plant2.sensor_c;

        static autotune_sample_t samples2[MAX_SAMPLES];
        int n2 = 0;
        int truncated_steps = (int)(2.0f * tau_true / dt_s); /* ~2*tau, not 5*tau -- the whole point */
        for (int i = 0; i < truncated_steps && n2 < MAX_SAMPLES; i++) {
            sim_plant_step(&plant2, &cfg, step_duty, dt_s);
            samples2[n2].t_s = (float)(i + 1) * dt_s;
            samples2[n2].measurement_c = plant2.sensor_c;
            n2++;
        }

        fopdt_model_t model2 = pid_autotune_fit_fopdt(samples2, n2, baseline2_c, step_duty - baseline_duty);
        TEST_CHECK(model2.valid, "a trace truncated at ~2*tau still fits (two-point crossings are well "
                                  "within a 2*tau trace)");
        /* Sanity: the RAW (un-extrapolated) rise on this truncated trace
         * really is biased low by roughly the expected 13.5% -- proves this
         * trace genuinely exercises the defect the extrapolation fixes,
         * rather than happening to already be close to steady state. */
        float raw_rise = samples2[n2 - 1].measurement_c - baseline2_c;
        float raw_k = raw_rise / (step_duty - baseline_duty);
        TEST_CHECK(raw_k < k_true * 0.90f,
                  "sanity: the raw (last-sample) rise on a ~2*tau trace is itself biased well below "
                  "k_true -- if this fails the truncated trace doesn't reproduce the defect");
        TEST_CHECK_NEAR(model2.k_gain_c_per_duty, k_true, k_true * 0.05,
                        "extrapolated K recovers within 5% of true gain from a trace truncated at "
                        "~2*tau -- the un-extrapolated raw_k above (typically ~13% low) would fail "
                        "this same bound");
        /* Item (4), round-3 review, positive case: an ordinary (well below
         * the MAX_EXTRAPOLATION_RATIO cap) correction on a clean trace must
         * leave tau_s consistent with the reported K -- this field must
         * not read false on the common, healthy path. (extrapolation_
         * converged is NOT asserted true here: empirically this trace's
         * iteration keeps refining by more than EXTRAPOLATION_CONVERGE_
         * EPS_C right up to MAX_EXTRAPOLATION_ITERATIONS -- still a good
         * fit (within 5%, checked above), just not one that happens to
         * satisfy the tight eps-convergence definition on this specific
         * truncation point. That is exactly the kind of case
         * extrapolation_converged exists to distinguish from a fully
         * stabilized one, not a defect in this trace or the flag.) */
        TEST_CHECK(model2.tau_consistent_with_gain,
                  "an ordinary 2*tau-truncated fit must report tau consistent with the corrected K");
    }

    /* 2026-09-02 review fix (item 6): the ORIGINAL single-pass extrapolation
     * used the RAW (still-biased) rise to place its 28.3%/63.2% crossing
     * targets, so the tau feeding the correction was itself biased -- the
     * "within 5%" claim above was an artifact of testing only ONE truncation
     * point (86.5% of asymptote reached). Multiple fractions, per the
     * review's own measurements of the un-iterated formula's accuracy
     * (0.865->3.2% low, 0.78->8% low [would fail a 5% bound], 0.50->33% low,
     * 0.25->64% low): the iterated fit (re-fitting tau/dead_time from each
     * successive rise_inf, converging or hitting MAX_EXTRAPOLATION_RATIO/
     * MAX_EXTRAPOLATION_ITERATIONS) must recover K materially closer to
     * truth than the single-pass formula did at EVERY one of these
     * fractions, not just the one the original test happened to check. */
    {
        static const float truncation_fractions[] = {0.5f, 0.78f, 0.865f, 0.95f};
        /* IMPLEMENTATION-SEPARATING REGRESSION FENCES, NOT AN ACCURACY
         * SPEC (round-3 review, item 5 -- previous wording implied these
         * were derived from a required accuracy; they were fitted to this
         * implementation's actual output instead, which is a materially
         * weaker claim and is labelled as such here). Their only job is:
         * fail if a future change makes the ITERATED fit noticeably worse
         * than it is today. They do NOT assert "this is accurate enough
         * for feedforward" -- see the multi-fraction loop below and this
         * file's own header comment for that judgement (short answer:
         * f=0.5's 17.6% residual is NOT considered acceptable as-is; a
         * follow-up fix is needed, not just a passing test here).
         *
         * Measured empirically at 5 iterations (the shipped value):
         * ~17.6%/1.1%/0.9%/0.5% at these four fractions. Measured at 1
         * iteration (MAX_EXTRAPOLATION_ITERATIONS stubbed down, i.e. the
         * pre-iteration single-pass formula): ~32.2%/7.0%/2.5%/0.05%.
         * Fences at 0.5/0.78/0.865 sit strictly between those two
         * measurements (loose enough not to flake on ordinary floating-
         * point/sim differences, tight enough that the single-pass
         * regression trips every one of them -- confirmed by stubbing
         * MAX_EXTRAPOLATION_ITERATIONS to 1: 3 of these 4 fences fail,
         * restoring 5 makes all 4 pass again).
         *
         * The f=0.95 fence (0.02) is HONESTLY ACKNOWLEDGED TO CONSTRAIN
         * NOTHING: the pre-iteration formula already measured 0.05% here,
         * 40x tighter than this fence -- it is carried only for loop-shape
         * uniformity (one array indexed by fraction) and documented here
         * as non-binding rather than left to look like a real regression
         * guard it is not. */
        static const float regression_fence_err[] = {0.25f, 0.04f, 0.02f, 0.02f};
        for (size_t fi = 0; fi < sizeof(truncation_fractions) / sizeof(truncation_fractions[0]); fi++) {
            float f = truncation_fractions[fi];
            /* rise(t)/rise_inf = 1 - exp(-t/tau) = f  =>  t = -tau*ln(1-f) */
            float elapsed_s = -tau_true * logf(1.0f - f);

            sim_plant_state_t plant3;
            sim_plant_reset(&plant3, &cfg);
            for (int i = 0; i < (int)(5.0f * tau_true / dt_s); i++) {
                sim_plant_step(&plant3, &cfg, baseline_duty, dt_s);
            }
            float baseline3_c = plant3.sensor_c;

            static autotune_sample_t samples3[MAX_SAMPLES];
            int n3 = 0;
            int steps3 = (int)(elapsed_s / dt_s);
            if (steps3 < 2) steps3 = 2; /* pid_autotune_fit_fopdt()'s own "not enough samples" floor */
            for (int i = 0; i < steps3 && n3 < MAX_SAMPLES; i++) {
                sim_plant_step(&plant3, &cfg, step_duty, dt_s);
                samples3[n3].t_s = (float)(i + 1) * dt_s;
                samples3[n3].measurement_c = plant3.sensor_c;
                n3++;
            }

            fopdt_model_t model3 = pid_autotune_fit_fopdt(samples3, n3, baseline3_c, step_duty - baseline_duty);
            char msg[160];
            snprintf(msg, sizeof(msg), "f=%.2f: fit must succeed (two-point crossings reached)", (double)f);
            TEST_CHECK(model3.valid, msg);
            if (!model3.valid) {
                continue;
            }

            float raw_rise3 = samples3[n3 - 1].measurement_c - baseline3_c;
            float raw_k3 = raw_rise3 / (step_duty - baseline_duty);
            float raw_err = fabsf(raw_k3 - k_true) / k_true;
            float corrected_err = fabsf(model3.k_gain_c_per_duty - k_true) / k_true;

            /* The one bound true at every fraction, including the ones
             * where MAX_EXTRAPOLATION_RATIO's cap intentionally prevents a
             * full correction (f=0.5 sits exactly at the 2.0x cap boundary;
             * lower fractions than tested here are deliberately
             * UNDER-corrected, see that constant's own comment): the
             * iterated, tau-consistent fit must never be WORSE than the raw
             * last-sample reading, and must be strictly better whenever the
             * raw reading was already meaningfully biased (>2%, f<~0.98) --
             * this is what the ORIGINAL single-pass-from-biased-tau version
             * could not promise at every truncation point, only the one the
             * first test happened to check. */
            snprintf(msg, sizeof(msg),
                    "f=%.2f: corrected K error (%.1f%%) must not exceed the raw error (%.1f%%)",
                    (double)f, (double)(corrected_err * 100.0f), (double)(raw_err * 100.0f));
            TEST_CHECK(corrected_err <= raw_err + 1e-6f, msg);
            snprintf(msg, sizeof(msg),
                    "f=%.2f: corrected K error (%.1f%%) must clear this fraction's regression fence "
                    "(%.1f%%) -- see regression_fence_err[]'s own comment: these are "
                    "implementation-separating fences, not an accuracy spec",
                    (double)f, (double)(corrected_err * 100.0f), (double)(regression_fence_err[fi] * 100.0f));
            TEST_CHECK(corrected_err <= regression_fence_err[fi], msg);
        }
    }

    /* Item (6), upper bound / defeat case: the review's own example --
     * "a non-monotonic trace where noise puts target28 very early and
     * target63 very late inflates tau to thousands of seconds; slope_end
     * ~0.01 degC/s with tau ~4485s adds ~44.9 degC to a 20 degC rise,
     * tripling K." Built by hand (not sim_plant -- this is deliberately
     * NOT a clean physical response): a fast early rise crosses 28.3% of
     * the eventual 20C rise almost immediately (t28 small), then an
     * unrealistically slow creep delays the 63.2% crossing until deep into
     * the trace (t63 large), giving tau in the thousands of seconds; the
     * last few samples then rise at a plausible-looking ~0.011 degC/s
     * (noise, not signal -- see slope_end's own use). Without
     * MAX_EXTRAPOLATION_RATIO's cap, rise_inf = 20 + tau*slope_end would
     * land near 3x the raw 20C rise (tripling K, matching the review's own
     * arithmetic); WITH it, rise_inf is bounded to at most 2x. */
    {
        static autotune_sample_t samples5[MAX_SAMPLES];
        int n5 = 0;
        float dt5 = 10.0f;
        /* Fast early rise: crosses target28 (5.66C) between sample 0 and 1. */
        samples5[n5].t_s = dt5 * 1.0f;      samples5[n5].measurement_c = 1.0f;  n5++;
        samples5[n5].t_s = dt5 * 2.0f;      samples5[n5].measurement_c = 6.0f;  n5++;
        /* Slow creep from 6.0C to 19.0C over the next 460 samples (4600s) --
         * crosses target63 (12.64C) deep into this segment, well after the
         * fast early rise, so t63-t28 (and therefore tau) is large. */
        float creep_start_t = dt5 * 2.0f;
        float creep_start_v = 6.0f;
        int creep_samples = 460;
        float creep_end_v = 19.0f;
        for (int i = 0; i < creep_samples; i++) {
            float frac = (float)(i + 1) / (float)creep_samples;
            samples5[n5].t_s = creep_start_t + dt5 * (float)(i + 1);
            samples5[n5].measurement_c = creep_start_v + frac * (creep_end_v - creep_start_v);
            n5++;
        }
        /* Final 10 samples: a steeper ~0.011 degC/s tail (noise, not the
         * plant genuinely accelerating) up to the 20C raw rise. This is
         * slope_end's own window. */
        float tail_v = creep_end_v;
        float tail_t = samples5[n5 - 1].t_s;
        for (int i = 0; i < 10; i++) {
            tail_v += 0.11f; /* 0.11C per 10s sample = 0.011 degC/s */
            tail_t += dt5;
            samples5[n5].t_s = tail_t;
            samples5[n5].measurement_c = tail_v;
            n5++;
        }

        float baseline5_c = 0.0f;
        float duty_step5 = 1.0f;
        float raw_rise5 = samples5[n5 - 1].measurement_c - baseline5_c;
        fopdt_model_t model5 = pid_autotune_fit_fopdt(samples5, n5, baseline5_c, duty_step5);
        TEST_CHECK(model5.valid, "the defeat-case trace still fits (crossings are reached)");
        if (model5.valid) {
            TEST_CHECK(model5.tau_s > 500.0f,
                      "sanity: this trace really does inflate tau into the hundreds-to-thousands of "
                      "seconds range -- if this fails, the defeat case isn't reproduced");
            /* 2.0 mirrors pid_autotune.c's own (private, not header-exposed)
             * MAX_EXTRAPOLATION_RATIO -- hardcoded here the same way this
             * file already hardcodes other implementation constants it
             * checks against (see the SIMC/Cohen-Coon expected-value blocks
             * below), so update both together if that constant ever moves. */
            TEST_CHECK(model5.k_gain_c_per_duty <= 2.0f * raw_rise5 / duty_step5 + 0.5f,
                      "MAX_EXTRAPOLATION_RATIO must cap the correction -- K must not exceed "
                      "~2x the raw (last-sample) reading regardless of how large a noisy tau claims");
            TEST_CHECK(model5.k_gain_c_per_duty < 2.5f * (raw_rise5 / duty_step5),
                      "the review's own defeat case: WITHOUT the cap this would come out near 3x "
                      "(tripling K) -- with it, must stay well short of that");

            /* Item (4), round-3 review: on exactly this path (the cap
             * engages) tau_s/dead_time_s used to stay silently stale --
             * corrected up to 2x on k_gain_c_per_duty, uncorrected on tau,
             * no record of the mismatch. Here that inconsistency is real
             * (this trace's fabricated tail deliberately overshoots what a
             * refit at the capped rise level can support) and must now be
             * FLAGGED, not silently absorbed: both new fopdt_model_t
             * fields read false, and tau_s stays at its last successfully-
             * fitted (large, ~thousands-of-seconds) value rather than
             * being silently forced to agree with the capped K. */
            TEST_CHECK(!model5.tau_consistent_with_gain,
                      "the defeat case must be flagged tau-INconsistent -- tau_s could not be "
                      "refit to match the capped k_gain_c_per_duty");
            TEST_CHECK(!model5.extrapolation_converged,
                      "the defeat case must be flagged NOT converged -- the loop stopped on the "
                      "tau-refit failure, not on reaching EXTRAPOLATION_CONVERGE_EPS_C");
        }
    }

    /* Item (6), upper bound / defeat case: a NEGATIVE slope_end (the trace's
     * tail is trending back DOWN, e.g. sensor noise on an already-settled
     * trace) must be ignored, not treated as "correct downward" -- see the
     * sign-check in pid_autotune_fit_fopdt()'s extrapolation loop. Built by
     * taking a fully-settled 5*tau trace and perturbing only its last few
     * samples downward. */
    {
        sim_plant_state_t plant4;
        sim_plant_reset(&plant4, &cfg);
        for (int i = 0; i < (int)(5.0f * tau_true / dt_s); i++) {
            sim_plant_step(&plant4, &cfg, baseline_duty, dt_s);
        }
        float baseline4_c = plant4.sensor_c;
        static autotune_sample_t samples4[MAX_SAMPLES];
        int n4 = 0;
        int steps4 = (int)(5.0f * tau_true / dt_s);
        for (int i = 0; i < steps4 && n4 < MAX_SAMPLES; i++) {
            sim_plant_step(&plant4, &cfg, step_duty, dt_s);
            samples4[n4].t_s = (float)(i + 1) * dt_s;
            samples4[n4].measurement_c = plant4.sensor_c;
            n4++;
        }
        /* Perturb the trailing ASYMPTOTE_SLOPE_WINDOW_SAMPLES (6) samples
         * with a clear downward trend (noise, not signal -- the plant is
         * genuinely at steady state by 5*tau). Subtracts MORE from the
         * LATER samples (i - (n4-6), 0 at the window's first sample, 5*0.3
         * at its last) so first-to-last really is decreasing -- a genuinely
         * negative slope_end, not merely a downward-shifted but still
         * upward-trending window (the first attempt at this test got the
         * direction backwards and had to be fixed). */
        for (int i = n4 - 6; i < n4; i++) {
            samples4[i].measurement_c -= (float)(i - (n4 - 6)) * 0.3f;
        }
        fopdt_model_t model4 = pid_autotune_fit_fopdt(samples4, n4, baseline4_c, step_duty - baseline_duty);
        TEST_CHECK(model4.valid, "a settled trace with a noisy downward tail still fits");
        TEST_CHECK_NEAR(model4.k_gain_c_per_duty, k_true, k_true * 0.05,
                        "a NEGATIVE slope_end (noise trending down on an already-settled trace) must "
                        "be ignored by the extrapolation, not subtracted from K -- fitted K should "
                        "read close to the true, un-perturbed gain");
    }

    /* SIMC tuning from a known model, checked against the formula by hand. */
    {
        fopdt_model_t m = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t g = pid_autotune_tune_from_fopdt(&m, AUTOTUNE_RULE_SIMC, 0.0f);
        float lambda = 3.0f * m.dead_time_s; /* default lambda=3L */
        float kc_expect = m.tau_s / (m.k_gain_c_per_duty * (lambda + m.dead_time_s));
        float ti_expect = m.tau_s;
        float ti_cap = 4.0f * (lambda + m.dead_time_s);
        if (ti_cap < ti_expect) ti_expect = ti_cap;
        float td_expect = m.dead_time_s / 2.0f;
        TEST_CHECK_NEAR(g.kp, kc_expect, 1e-6, "SIMC Kp matches Kc formula");
        TEST_CHECK_NEAR(g.ki, kc_expect / ti_expect, 1e-6, "SIMC Ki == Kc/Ti (parallel-form conversion)");
        TEST_CHECK_NEAR(g.kd, kc_expect * td_expect, 1e-6, "SIMC Kd == Kc*Td (parallel-form conversion)");
    }

    /* An invalid model, or a non-SIMC rule (relay-test rules aren't
     * derivable from a FOPDT model in this pass), returns zero gains rather
     * than something a caller could mistake for a real tuning. */
    {
        fopdt_model_t bad = {.valid = false};
        autotune_gains_t g = pid_autotune_tune_from_fopdt(&bad, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(g.kp == 0.0f && g.ki == 0.0f && g.kd == 0.0f, "invalid model yields zero gains, not garbage");

        fopdt_model_t good = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t g2 = pid_autotune_tune_from_fopdt(&good, AUTOTUNE_RULE_ZIEGLER_NICHOLS, 0.0f);
        TEST_CHECK(g2.kp == 0.0f && g2.ki == 0.0f && g2.kd == 0.0f,
                  "ZN requested from a FOPDT model (not Ku/Tu) yields zero gains, not a silently wrong tuning");

        autotune_gains_t g3 = pid_autotune_tune_from_fopdt(&good, AUTOTUNE_RULE_TYREUS_LUYBEN, 0.0f);
        TEST_CHECK(g3.kp == 0.0f && g3.ki == 0.0f && g3.kd == 0.0f,
                  "Tyreus-Luyben requested from a FOPDT model yields zero gains -- still relay-only (regression)");
    }

    /* Cohen-Coon tuning from a known model (PID_EXPANSION_PLAN.md Phase 1),
     * checked against the published formula by hand:
     *   K=400, tau=1000, L=30, r=L/tau=0.03
     *   Kc = (1/400)*(1000/30)*(4/3 + 0.03/4)
     *      = 0.0025 * 33.3333 * (1.33333 + 0.0075) = 0.0025*33.3333*1.34083
     *      = 0.111736
     *   Ti = 30*(32 + 6*0.03)/(13 + 8*0.03) = 30*32.18/13.24 = 72.925...
     *   Td = 30*4/(11 + 2*0.03) = 120/11.06 = 10.8499...
     * Ki = Kc/Ti, Kd = Kc*Td, same parallel-form conversion as SIMC. */
    {
        fopdt_model_t m = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t cc = pid_autotune_tune_from_fopdt(&m, AUTOTUNE_RULE_COHEN_COON, 0.0f);

        /* Literal expected values, NOT the formula re-evaluated. Re-deriving
         * `kc_expect` from the same expressions the implementation uses makes
         * the check tautological: transpose 32 and 6, or write `r/4` where
         * the rule says `L/(4*tau)`, and both sides move together and the
         * test still passes. These constants come from the arithmetic worked
         * out longhand in the comment above, so an algebra error in
         * pid_autotune.c has nothing to hide behind.
         *
         *   Kc = 0.11173611
         *   Ti = 30*(32 + 0.18)/(13 + 0.24) = 965.4/13.24 = 72.9154129 s
         *   Td = 30*4/(11 + 0.06) = 120/11.06 = 10.8499088 s
         *   Ki = Kc/Ti = 1.53240736e-3
         *   Kd = Kc*Td = 1.21232665
         *
         * These are float32 values, matching the implementation's precision;
         * a double-precision derivation drifts far enough to fail the tighter
         * tolerances below. Worth noting the literals earned their keep
         * immediately: the first version of this test carried a hand-computed
         * Ti of 72.9245, which is wrong in the 4th significant figure, and
         * the mistake surfaced as a failing Ki the moment the tautological
         * self-referential expressions were removed. */
        TEST_CHECK_NEAR(cc.kp, 0.11173611f, 1e-6, "Cohen-Coon Kp matches the hand-computed published formula");
        TEST_CHECK_NEAR(cc.ki, 1.53240736e-3f, 1e-7, "Cohen-Coon Ki == Kc/Ti (parallel-form conversion)");
        TEST_CHECK_NEAR(cc.kd, 1.21232665f, 1e-5, "Cohen-Coon Kd == Kc*Td (parallel-form conversion)");
        TEST_CHECK(cc.rule == AUTOTUNE_RULE_COHEN_COON, "returned gains carry the rule they were computed with");

        /* [9]'s claim (PID_EXPANSION_PLAN.md §2a): Cohen-Coon is the more
         * aggressive rule of the two offered on the FOPDT path. Prove it
         * directly on the same model, rather than asserting it in prose. */
        autotune_gains_t simc = pid_autotune_tune_from_fopdt(&m, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(cc.kp > simc.kp, "Cohen-Coon Kp is larger than SIMC's on the same model (more aggressive)");

        /* Degenerate dead time: L==0 and L tiny both refuse rather than
         * emit an inflated (or infinite/NaN) Kc from dividing by L. */
        fopdt_model_t zero_l = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 0.0f, .valid = true};
        autotune_gains_t cc_zero = pid_autotune_tune_from_fopdt(&zero_l, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(cc_zero.kp == 0.0f && cc_zero.ki == 0.0f && cc_zero.kd == 0.0f,
                  "Cohen-Coon with L==0 refuses (zero gains), doesn't divide by zero");

        fopdt_model_t tiny_l = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 0.001f, .valid = true};
        autotune_gains_t cc_tiny = pid_autotune_tune_from_fopdt(&tiny_l, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(cc_tiny.kp == 0.0f && cc_tiny.ki == 0.0f && cc_tiny.kd == 0.0f,
                  "Cohen-Coon with L near zero also refuses, not an inflated Kc");
        TEST_CHECK(!isnan(cc_tiny.kp) && !isinf(cc_tiny.kp), "no NaN/Inf leaks out of the tiny-L case either way");

        /* Invalid model still refuses, same as SIMC. */
        fopdt_model_t bad = {.valid = false};
        autotune_gains_t cc_bad = pid_autotune_tune_from_fopdt(&bad, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(cc_bad.kp == 0.0f && cc_bad.ki == 0.0f && cc_bad.kd == 0.0f,
                  "invalid model yields zero Cohen-Coon gains too");
    }

    /* A NEGATIVE fitted plant gain must be refused by BOTH FOPDT rules.
     * K < 0 means the zone got colder as duty went up -- a step test started
     * while the kiln was still cooling, or a relay wired to the wrong zone's
     * thermocouple. Both rules' Kc inherits K's sign, so without this the
     * operator is shown three plausible-looking negative gains that drive the
     * loop backwards. A heater cannot have a negative gain, so this is always
     * a bad fit, never a real plant. (Regression: the original Cohen-Coon
     * guard tested `== 0.0f` and SIMC had no gain check at all.) */
    {
        fopdt_model_t neg_k = {.k_gain_c_per_duty = -400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};

        autotune_gains_t cc_neg = pid_autotune_tune_from_fopdt(&neg_k, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(cc_neg.kp == 0.0f && cc_neg.ki == 0.0f && cc_neg.kd == 0.0f,
                  "Cohen-Coon refuses a negative plant gain rather than returning negative PID gains");

        autotune_gains_t simc_neg = pid_autotune_tune_from_fopdt(&neg_k, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(simc_neg.kp == 0.0f && simc_neg.ki == 0.0f && simc_neg.kd == 0.0f,
                  "SIMC refuses a negative plant gain too -- not a Cohen-Coon-specific concern");

        /* Zero gain stays refused by both (the case the original == 0 check
         * did cover -- proving the widened check didn't lose it). */
        fopdt_model_t zero_k = {.k_gain_c_per_duty = 0.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t cc_zero = pid_autotune_tune_from_fopdt(&zero_k, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        autotune_gains_t simc_zero = pid_autotune_tune_from_fopdt(&zero_k, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(cc_zero.kp == 0.0f && simc_zero.kp == 0.0f, "zero plant gain still refused by both rules");
    }

    /* Machine-readable refusal reasons (PID_EXPANSION_PLAN.md Phase 1,
     * "Surface the refusal in the UI"): every distinct way
     * pid_autotune_tune_from_fopdt() can refuse must set its OWN distinct
     * autotune_refusal_t, not just "some non-OK code" -- that's the entire
     * point, an operator picking Cohen-Coon needs to know *which* of five
     * different problems they hit. The success path must report OK. */
    {
        fopdt_model_t good = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t ok = pid_autotune_tune_from_fopdt(&good, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(ok.refusal == AUTOTUNE_REFUSAL_OK, "successful SIMC tuning reports AUTOTUNE_REFUSAL_OK");
        TEST_CHECK(ok.kp == ok.kp && ok.kp != 0.0f, "success path leaves gains as computed (unaffected by refusal field)");

        fopdt_model_t bad = {.valid = false};
        snprintf(bad.invalid_reason, sizeof(bad.invalid_reason), "flat trace");
        autotune_gains_t r_invalid = pid_autotune_tune_from_fopdt(&bad, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(r_invalid.refusal == AUTOTUNE_REFUSAL_INVALID_MODEL, "invalid model -> AUTOTUNE_REFUSAL_INVALID_MODEL");
        TEST_CHECK(strstr(r_invalid.refusal_reason, "flat trace") != NULL,
                  "invalid-model reason string carries the model's own invalid_reason");

        autotune_gains_t r_wrong_rule = pid_autotune_tune_from_fopdt(&good, AUTOTUNE_RULE_ZIEGLER_NICHOLS, 0.0f);
        TEST_CHECK(r_wrong_rule.refusal == AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH,
                  "ZN on the FOPDT path -> AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH");

        fopdt_model_t tiny_l = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 0.21f, .valid = true};
        autotune_gains_t r_dead_time = pid_autotune_tune_from_fopdt(&tiny_l, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(r_dead_time.refusal == AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL,
                  "Cohen-Coon with L < 0.5s -> AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL");
        TEST_CHECK(strstr(r_dead_time.refusal_reason, "0.21") != NULL,
                  "dead-time reason string names the actual L value, not a generic message");

        fopdt_model_t bad_tau = {.k_gain_c_per_duty = 400.0f, .tau_s = 0.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t r_tau = pid_autotune_tune_from_fopdt(&bad_tau, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(r_tau.refusal == AUTOTUNE_REFUSAL_NONPOSITIVE_TAU,
                  "Cohen-Coon with tau<=0 -> AUTOTUNE_REFUSAL_NONPOSITIVE_TAU");

        fopdt_model_t bad_gain = {.k_gain_c_per_duty = -400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        autotune_gains_t r_gain = pid_autotune_tune_from_fopdt(&bad_gain, AUTOTUNE_RULE_COHEN_COON, 0.0f);
        TEST_CHECK(r_gain.refusal == AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN,
                  "Cohen-Coon with K<=0 -> AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN");

        autotune_gains_t r_gain_simc = pid_autotune_tune_from_fopdt(&bad_gain, AUTOTUNE_RULE_SIMC, 0.0f);
        TEST_CHECK(r_gain_simc.refusal == AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN,
                  "SIMC with K<=0 also -> AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN (same reason, both rules)");

        /* All five distinct refusal codes above must actually BE distinct
         * from one another -- this is the assertion that would have caught
         * two refusal paths sharing a code (see the deliberate-break check
         * this task's report documents). */
        autotune_refusal_t codes[] = {
            r_invalid.refusal, r_wrong_rule.refusal, r_dead_time.refusal, r_tau.refusal, r_gain.refusal,
        };
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            for (size_t j = i + 1; j < sizeof(codes) / sizeof(codes[0]); j++) {
                TEST_CHECK(codes[i] != codes[j], "distinct FOPDT refusal paths report distinct refusal codes");
            }
        }
    }

    /* Fit rejects degenerate inputs instead of returning a bogus model. */
    {
        autotune_sample_t s[3] = {{0, 100}, {10, 100}, {20, 100}};
        fopdt_model_t m = pid_autotune_fit_fopdt(s, 3, 100.0f, 0.5f);
        TEST_CHECK(!m.valid, "a flat trace (no response) is rejected, not fit as tau=0");

        fopdt_model_t m2 = pid_autotune_fit_fopdt(s, 3, 100.0f, 0.0f);
        TEST_CHECK(!m2.valid, "duty_step==0 is rejected (can't identify a gain from it)");
    }

    /* Ramp-ceiling estimate: sanity-checked against the closed-form formula
     * in the header, not just "returns something". */
    {
        fopdt_model_t m = {.k_gain_c_per_duty = 400.0f, .tau_s = 1000.0f, .dead_time_s = 30.0f, .valid = true};
        float est = pid_autotune_estimate_max_ramp_c_per_hr(&m, 1.0f, 300.0f, 20.0f);
        float expect_c_per_s = (m.k_gain_c_per_duty * 1.0f - (300.0f - 20.0f)) / m.tau_s;
        TEST_CHECK_NEAR(est, expect_c_per_s * 3600.0f, 1e-3, "ramp estimate matches the (K*u_max - deltaT)/tau formula");

        /* At/above the model's steady-state ceiling, headroom is gone -- 0, not negative. */
        float est_no_headroom = pid_autotune_estimate_max_ramp_c_per_hr(&m, 1.0f, 419.0f, 20.0f);
        TEST_CHECK(est_no_headroom >= 0.0f, "ramp estimate never goes negative when already near the ceiling");
    }

    run_test_pid_autotune_magnitude_aware_convergence();
    run_test_pid_autotune_relay();
}

/* ------------------------------------------------------------------------
 * Final review follow-up: the sign-only break at the top of the asymptote-
 * extrapolation loop reported extrapolation_converged=1 on genuinely bad,
 * low-biased fits -- not just on quantization noise. Fixed by making the
 * break magnitude-aware (TRACE_QUANTUM_C-derived threshold) and by
 * replacing the two-point slope_end estimator with a least-squares one.
 * These tests exercise both fixes AT THE FIT LAYER (pid_autotune_fit_fopdt()
 * directly), without going anywhere near autotune_engine.c's settled gate,
 * per the review's own instruction that the fit layer must be correct in
 * isolation, not merely safe in composition.
 * ------------------------------------------------------------------------ */

/* Builds a trace for the reviewer's own five repro shapes: a clean
 * exponential response (K=k_true, tau=tau_s, no dead time, baseline 0) run
 * out to `frac` of its true asymptote, with the FINAL sample additionally
 * depressed by `dip_c` degrees -- the "truncated NN% + X.XC last-sample
 * dip" shape named in the review. frac >= 1.0 is used for the "rise then
 * cooling/reversing" shape instead: the trace runs past its nominal peak
 * and is mirrored back downward for the back half, a genuine reversal
 * rather than a truncation-plus-dip. Returns the sample count written. */
static int build_reviewer_repro_trace(autotune_sample_t *samples, int max_samples, float k_true, float tau_s,
                                      float dt_s, float frac, float dip_c, bool reversing)
{
    int n = 0;
    if (reversing) {
        /* Rises to ~85% of asymptote, then cools back down for the same
         * duration -- a real "the element was cut or the trace caught a
         * cooling segment" shape, not a truncation. */
        float t_up_end = -tau_s * logf(1.0f - 0.85f);
        int steps_up = (int)(t_up_end / dt_s);
        for (int i = 0; i < steps_up && n < max_samples; i++) {
            float t_s = (float)(i + 1) * dt_s;
            float y = k_true * (1.0f - expf(-t_s / tau_s));
            samples[n].t_s = t_s;
            samples[n].measurement_c = y;
            n++;
        }
        float peak_c = samples[n - 1].measurement_c;
        float peak_t = samples[n - 1].t_s;
        int steps_down = steps_up;
        for (int i = 0; i < steps_down && n < max_samples; i++) {
            float t_s = peak_t + (float)(i + 1) * dt_s;
            /* Symmetric cool-down back toward baseline over the same span. */
            float frac_down = (float)(i + 1) / (float)steps_down;
            /* Cools back to 70% of peak, not further -- final_c must stay
             * large enough that target28/target63 (fractions of raw_rise =
             * final_c - baseline) are not so tiny that the RISING portion's
             * very first recorded sample already overshoots them (which
             * would make find_crossing_time() find no crossing at all,
             * since it only scans PAIRS of recorded samples -- there is no
             * sample "before" samples[0] to compare against). */
            float y = peak_c * (1.0f - frac_down * 0.3f);
            samples[n].t_s = t_s;
            samples[n].measurement_c = y;
            n++;
        }
        return n;
    }

    float elapsed_s = -tau_s * logf(1.0f - frac);
    int steps = (int)(elapsed_s / dt_s);
    if (steps < 2) steps = 2;
    for (int i = 0; i < steps && n < max_samples; i++) {
        float t_s = (float)(i + 1) * dt_s;
        float y = k_true * (1.0f - expf(-t_s / tau_s));
        samples[n].t_s = t_s;
        samples[n].measurement_c = y;
        n++;
    }
    samples[n - 1].measurement_c -= dip_c;
    return n;
}

static void run_test_pid_autotune_magnitude_aware_convergence(void)
{
    TEST_SECTION("pid_autotune (final review: magnitude-aware sign-check break, least-squares slope_end)");

    const float k_true = 30.0f;
    const float tau_s = 200.0f;
    const float dt_s = 10.0f;
    const float baseline_c = 0.0f;
    const float duty_step = 1.0f;

    /* The reviewer's own five repro shapes -- all measured biased LOW
     * (the dangerous, original-overshoot-defect direction) and all
     * reported extrapolation_converged=1 by the pre-fix sign-only break.
     * Every one must now report FALSE, at the fit layer, with no settled
     * gate involved at all. */
    struct {
        const char *name;
        float frac;
        float dip_c;
        bool reversing;
    } cases[] = {
        {"rise then cooling/reversing", 0.0f, 0.0f, true},
        {"truncated 50% + 2.0C last-sample dip", 0.50f, 2.0f, false},
        {"truncated 70% + 1.0C dip", 0.70f, 1.0f, false},
        {"truncated 85% + 5.0C dip", 0.85f, 5.0f, false},
        {"truncated 95% + 2.0C dip", 0.95f, 2.0f, false},
    };
    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        static autotune_sample_t samples[MAX_SAMPLES];
        int n = build_reviewer_repro_trace(samples, MAX_SAMPLES, k_true, tau_s, dt_s, cases[ci].frac,
                                           cases[ci].dip_c, cases[ci].reversing);
        fopdt_model_t m = pid_autotune_fit_fopdt(samples, n, baseline_c, duty_step);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: fit must succeed (crossings reached)", cases[ci].name);
        TEST_CHECK(m.valid, msg);
        if (!m.valid) continue;
        snprintf(msg, sizeof(msg), "%s: extrapolation_converged must be FALSE (K=%.2f vs true %.1f, %.0f%%) "
                                   "-- this must hold on its own, without the settled gate",
                cases[ci].name, (double)m.k_gain_c_per_duty, (double)k_true,
                (double)((m.k_gain_c_per_duty - k_true) / k_true * 100.0f));
        TEST_CHECK(!m.extrapolation_converged, msg);
    }

    /* Re-verified per the review's own instruction: the +/-0.1C dither
     * case (one genuine quantization tick on an otherwise flat, fully-
     * settled trace) must still report converged=true after both fixes --
     * proving the magnitude-aware threshold doesn't overcorrect into
     * rejecting real quantization noise the way the original bug over-
     * accepted real reversals. */
    {
        const float settled_tau = 200.0f;
        const float settled_k = 100.0f;
        const uint16_t n_settled = (uint16_t)((8.0f * settled_tau) / dt_s);
        static autotune_sample_t settled_samples[MAX_SAMPLES];
        int n = 0;
        for (uint16_t i = 0; i < n_settled; i++) {
            float t_s = (float)(i + 1) * dt_s;
            settled_samples[n].t_s = t_s;
            settled_samples[n].measurement_c = settled_k * (1.0f - expf(-t_s / settled_tau));
            n++;
        }
        /* Flatten the trailing 10 samples to one quantized value, same
         * technique as the engine-level end-to-end test, then dither only
         * the last one -- see that test's own comment for why flattening
         * first is required (the raw exponential tail's own residual
         * slope is not exactly zero even at 8*tau). */
        float flat_c = floorf(settled_samples[n - 11].measurement_c * 10.0f + 0.5f) / 10.0f;
        for (int i = n - 10; i < n; i++) {
            settled_samples[i].measurement_c = flat_c;
        }
        float dithers[] = {-0.1f, 0.0f, 0.1f};
        for (size_t di = 0; di < 3; di++) {
            settled_samples[n - 1].measurement_c = flat_c + dithers[di];
            fopdt_model_t m = pid_autotune_fit_fopdt(settled_samples, n, baseline_c, duty_step);
            char msg[128];
            snprintf(msg, sizeof(msg), "dither %.1fC: fit must succeed", (double)dithers[di]);
            TEST_CHECK(m.valid, msg);
            snprintf(msg, sizeof(msg), "dither %.1fC: must still report extrapolation_converged=true "
                                       "(K=%.2f)",
                    (double)dithers[di], (double)m.k_gain_c_per_duty);
            TEST_CHECK(m.valid && m.extrapolation_converged, msg);
        }
    }

    /* Least-squares slope_end: one anomalous FINAL sample must not flip the
     * window's slope sign the way a first-to-last two-point estimator
     * would. Five points climb steeply (100, 120, 140, 160, 180 -- a clear,
     * strong uptrend); the sixth (final) point crashes to 95, BELOW even
     * the window's first point. A first-to-last two-point slope over this
     * window is NEGATIVE ((95-100)/50s), which would trigger the sign-check
     * break and refuse any upward correction at all -- the fitted K would
     * stay pinned near the raw (crashed) last-sample reading. The
     * least-squares slope over all 6 points is POSITIVE (the five-point
     * uptrend dominates one outlier), so the extrapolation instead attempts
     * a genuine upward correction. Distinguished here by K: a positive
     * slope_end can only ever ADD to raw_rise (see the sign-check break's
     * own "must not be smaller in magnitude" comment), so a fitted K
     * meaningfully ABOVE the raw last-sample-implied K is only possible if
     * slope_end's sign survived the outlier. */
    {
        static autotune_sample_t window_samples[6];
        float t0 = 500.0f; /* arbitrary offset -- LSQ is computed relative to the window's own mean t_s */
        float window_vals[6] = {100.0f, 120.0f, 140.0f, 160.0f, 180.0f, 95.0f};
        for (int i = 0; i < 6; i++) {
            window_samples[i].t_s = t0 + (float)i * dt_s;
            window_samples[i].measurement_c = window_vals[i];
        }
        /* Prepend a short, unremarkable rise so t28/t63 crossings (needed
         * for tau) are found well before this window -- the window itself
         * is samples[N-6..N-1], same construction the real code uses. */
        static autotune_sample_t full_samples[16];
        int prefix = 10;
        for (int i = 0; i < prefix; i++) {
            full_samples[i].t_s = (float)(i + 1) * dt_s;
            full_samples[i].measurement_c = 90.0f * (1.0f - expf(-full_samples[i].t_s / 100.0f));
        }
        for (int i = 0; i < 6; i++) {
            full_samples[prefix + i].t_s = window_samples[i].t_s;
            full_samples[prefix + i].measurement_c = window_samples[i].measurement_c;
        }
        int n_full = prefix + 6;
        float raw_k_from_crashed_last_sample = window_vals[5] / duty_step; /* baseline 0 */

        fopdt_model_t m = pid_autotune_fit_fopdt(full_samples, n_full, 0.0f, duty_step);
        TEST_CHECK(m.valid, "one-outlier-window trace must still fit");
        TEST_CHECK(m.k_gain_c_per_duty > raw_k_from_crashed_last_sample + 5.0f,
                  "a single crashed final sample must not flip slope_end's sign -- the least-squares "
                  "fit over all 6 window points must still see the 5-point uptrend and attempt a real "
                  "upward correction, not stay pinned near the raw (crashed) last-sample K");
    }
}

/* ------------------------------------------------------------------------
 * Relay feedback (Astrom-Hagglund)
 *
 * The step-test checks above compare the fit against sim_plant.c's exact
 * ground-truth K/tau/L. A relay test has no such single exact answer -- the
 * describing-function derivation of Ku is a first-harmonic approximation, so
 * even a perfect implementation lands a little off the true ultimate gain.
 * What *is* exactly known for this plant is the linear-theory answer: for a
 * first-order-plus-dead-time plant, the loop hits -180 degrees at the
 * frequency where w*L + atan(w*tau) == pi, and the ultimate gain there is
 * 1/|G(jw)| = sqrt(1 + (w*tau)^2) / K. That is the number the identified Ku
 * is checked against, with a tolerance wide enough to admit the harmonic
 * approximation's known bias but narrow enough that a factor-of-two error --
 * exactly the error a peak-to-peak/amplitude mix-up produces -- fails.
 * ------------------------------------------------------------------------ */
static void run_test_pid_autotune_relay(void)
{
    TEST_SECTION("pid_autotune (relay feedback)");

    /* Same plant as above plus a 60 s transport delay. The delay is not
     * decoration: a pure first-order plant under a hysteretic relay just
     * shuttles between the two switching thresholds, so its oscillation
     * amplitude equals the hysteresis band, a == h, and the identification is
     * degenerate by construction. Dead time is what makes the temperature
     * overshoot the band and gives the test something to measure -- which is
     * also why a real kiln (large transport delay) is a plant the relay
     * method works on at all. */
    sim_plant_cfg_t cfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 5000.0f,
        .heater_power_w = 2000.0f,
        .loss_coeff_w_per_c = 5.0f, /* tau_true=1000s, K_true=400C/duty */
        .sensor_delay_s = 60.0f,
        .sensor_lag_tau_s = 0.0f,
    };
    float tau_true = cfg.thermal_mass_j_per_c / cfg.loss_coeff_w_per_c;
    float k_true = cfg.heater_power_w / cfg.loss_coeff_w_per_c;
    float dead_true = cfg.sensor_delay_s;

    /* Ultimate frequency by bisection on w*L + atan(w*tau) - pi, which is
     * monotonic in w -- solved here rather than hardcoded so the expected
     * values stay correct if the plant constants above are ever changed. */
    double w_lo = 1e-5, w_hi = 1.0;
    for (int i = 0; i < 200; i++) {
        double w = 0.5 * (w_lo + w_hi);
        double f = w * dead_true + atan(w * tau_true) - 3.14159265358979;
        if (f > 0.0) w_hi = w; else w_lo = w;
    }
    double w_u = 0.5 * (w_lo + w_hi);
    double tu_theory = 2.0 * 3.14159265358979 / w_u;
    double ku_theory = sqrt(1.0 + (w_u * tau_true) * (w_u * tau_true)) / k_true;

    /* Relay parameters. u0 +/- d keeps the duty inside [0,1] with margin, so
     * the sim's own duty clamp never truncates the square wave (a clipped
     * relay would mean the d handed to the fit is not the d the plant saw). */
    const float u0 = 0.5f;
    const float d = 0.3f;
    const float h = 2.0f;
    const float dt_s = 5.0f;
    const float setpoint_c = 220.0f; /* steady state at duty u0: 20 + 2000*0.5/5 */

    sim_plant_state_t plant;
    sim_plant_reset(&plant, &cfg);
    for (int i = 0; i < (int)(5.0f * tau_true / dt_s); i++) {
        sim_plant_step(&plant, &cfg, u0, dt_s);
    }

    static autotune_sample_t samples[MAX_SAMPLES];
    int n = 0;
    bool heat_on = true;
    int relay_steps = (int)(6000.0f / dt_s);
    for (int i = 0; i < relay_steps && n < MAX_SAMPLES; i++) {
        /* Bang-bang with a symmetric hysteresis band: heat below
         * setpoint - h, coast above setpoint + h, hold state in between.
         * h is the band's HALF-width, matching pid_autotune_fit_relay()'s
         * convention -- the full band is 2h wide. */
        if (plant.sensor_c < setpoint_c - h) heat_on = true;
        else if (plant.sensor_c > setpoint_c + h) heat_on = false;

        sim_plant_step(&plant, &cfg, heat_on ? (u0 + d) : (u0 - d), dt_s);
        samples[n].t_s = (float)(i + 1) * dt_s;
        samples[n].measurement_c = plant.sensor_c;
        n++;
    }

    relay_model_t rm = pid_autotune_fit_relay(samples, n, d, h);
    TEST_CHECK(rm.valid, "relay fit succeeds on a genuine closed-loop limit cycle");
    TEST_CHECK(rm.cycles_used == AUTOTUNE_RELAY_FIT_CYCLES, "relay fit averages the trailing cycles only");
    TEST_CHECK(rm.amplitude_c > h, "identified amplitude exceeds the hysteresis band (dead time drove overshoot)");
    /* Tu is NOT expected to land on tu_theory, and asserting that it does is
     * wrong physics: tu_theory is the period where the plant alone reaches
     * -180 degrees, but a *hysteretic* relay contributes phase lag of its
     * own, -asin(h/a), so the limit cycle settles at the lower frequency
     * where the plant supplies only the remaining pi - asin(h/a). With this
     * plant and band that is ~300 s against tu_theory's ~234 s -- a 28 %
     * gap that a tolerance wide enough to swallow would also swallow real
     * errors. So the expectation is solved from the same describing-function
     * condition the fit itself assumes, using the amplitude actually
     * measured, which lets the tolerance be tight. */
    double phase_budget = 3.14159265358979 - asin((double)h / (double)rm.amplitude_c);
    double wh_lo = 1e-5, wh_hi = 1.0;
    for (int i = 0; i < 200; i++) {
        double w = 0.5 * (wh_lo + wh_hi);
        double f = w * dead_true + atan(w * tau_true) - phase_budget;
        if (f > 0.0) wh_hi = w; else wh_lo = w;
    }
    double tu_expect = 2.0 * 3.14159265358979 / (0.5 * (wh_lo + wh_hi));
    TEST_CHECK(tu_expect > tu_theory,
              "hysteresis lengthens the limit-cycle period relative to the plant's own ultimate period");
    TEST_CHECK_NEAR(rm.tu_s, tu_expect, tu_expect * 0.12,
                    "identified Tu matches the hysteresis-corrected limit-cycle period");
    TEST_CHECK_NEAR(rm.ku, ku_theory, ku_theory * 0.40, "identified Ku near the FOPDT ultimate gain");
    TEST_CHECK(rm.ku > 0.0f && rm.tu_s > 0.0f, "identified Ku/Tu are positive and finite");

    /* Ku is in duty per degC, the same unit pid.c's Kp carries. A sanity
     * cross-check that does not depend on the describing function at all:
     * the ultimate gain must be smaller than the reciprocal of the plant's
     * DC gain (a plant with 400 degC/duty of DC gain cannot tolerate a
     * proportional gain anywhere near 1/400 * huge), i.e. Ku*K_true is O(10)
     * for a plant this delay-dominated, not O(1000). */
    TEST_CHECK(rm.ku * k_true > 1.0f && rm.ku * k_true < 100.0f,
              "Ku*K_dc lands in the physically sensible range for a delay-dominated plant");

    /* Tuning rules, checked against the formulas by hand in parallel form. */
    {
        relay_model_t m = {.ku = 0.05f, .tu_s = 240.0f, .amplitude_c = 9.0f, .cycles_used = 3, .valid = true};

        autotune_gains_t zn = pid_autotune_tune_from_relay(&m, AUTOTUNE_RULE_ZIEGLER_NICHOLS);
        float zn_kp = 0.6f * m.ku;
        TEST_CHECK_NEAR(zn.kp, zn_kp, 1e-9, "ZN Kp == 0.6*Ku");
        TEST_CHECK_NEAR(zn.ki, 2.0f * zn_kp / m.tu_s, 1e-9, "ZN Ki == 2*Kp/Tu (parallel-form conversion)");
        TEST_CHECK_NEAR(zn.kd, zn_kp * m.tu_s / 8.0f, 1e-9, "ZN Kd == Kp*Tu/8 (parallel-form conversion)");
        TEST_CHECK(zn.rule == AUTOTUNE_RULE_ZIEGLER_NICHOLS, "returned gains carry the rule they were computed with");

        autotune_gains_t tl = pid_autotune_tune_from_relay(&m, AUTOTUNE_RULE_TYREUS_LUYBEN);
        float tl_kp = m.ku / 3.2f;
        TEST_CHECK_NEAR(tl.kp, tl_kp, 1e-9, "Tyreus-Luyben Kp == Ku/3.2");
        TEST_CHECK_NEAR(tl.ki, tl_kp / (2.2f * m.tu_s), 1e-9, "Tyreus-Luyben Ki == Kp/Ti with Ti=2.2*Tu");
        TEST_CHECK_NEAR(tl.kd, tl_kp * m.tu_s / 6.3f, 1e-9, "Tyreus-Luyben Kd == Kp*Td with Td=Tu/6.3");

        /* The reason TODO.md 6A.4 warns about ZN on a kiln, expressed as an
         * assertion: for the same plant it asks for roughly twice the
         * proportional gain and an order of magnitude more integral action
         * than Tyreus-Luyben. Neither may be a default; this documents which
         * of the two is the aggressive one. */
        TEST_CHECK(zn.kp > tl.kp && zn.ki > tl.ki, "ZN is the more aggressive of the two oscillation rules");

        /* SIMC needs tau and L, which (Ku,Tu) cannot supply -- mirror image
         * of the FOPDT path rejecting ZN. */
        autotune_gains_t simc = pid_autotune_tune_from_relay(&m, AUTOTUNE_RULE_SIMC);
        TEST_CHECK(simc.kp == 0.0f && simc.ki == 0.0f && simc.kd == 0.0f,
                  "SIMC requested from Ku/Tu yields zero gains, not a silently wrong tuning");
        TEST_CHECK(simc.refusal == AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH,
                  "SIMC on the relay path -> AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH");

        relay_model_t bad = {.valid = false};
        snprintf(bad.invalid_reason, sizeof(bad.invalid_reason), "no oscillation");
        autotune_gains_t g = pid_autotune_tune_from_relay(&bad, AUTOTUNE_RULE_TYREUS_LUYBEN);
        TEST_CHECK(g.kp == 0.0f && g.ki == 0.0f && g.kd == 0.0f, "invalid relay model yields zero gains");
        TEST_CHECK(g.refusal == AUTOTUNE_REFUSAL_INVALID_MODEL, "invalid relay model -> AUTOTUNE_REFUSAL_INVALID_MODEL");
        TEST_CHECK(strstr(g.refusal_reason, "no oscillation") != NULL,
                  "invalid-relay-model reason carries the model's own invalid_reason");

        relay_model_t bad_ku = {.ku = 0.0f, .tu_s = 240.0f, .valid = true};
        autotune_gains_t g_ku = pid_autotune_tune_from_relay(&bad_ku, AUTOTUNE_RULE_TYREUS_LUYBEN);
        TEST_CHECK(g_ku.refusal == AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN,
                  "Ku<=0 on a valid relay model -> AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN");

        autotune_gains_t ok = pid_autotune_tune_from_relay(&m, AUTOTUNE_RULE_ZIEGLER_NICHOLS);
        TEST_CHECK(ok.refusal == AUTOTUNE_REFUSAL_OK, "successful relay tuning reports AUTOTUNE_REFUSAL_OK");

        /* The three relay-path refusal codes exercised above must be
         * distinct from one another, same rationale as the FOPDT block. */
        TEST_CHECK(g.refusal != simc.refusal && g.refusal != g_ku.refusal && simc.refusal != g_ku.refusal,
                  "distinct relay-path refusal causes report distinct refusal codes");
    }

    /* Negative cases: every one of these is a trace a real aborted or
     * mis-driven relay test can produce, and none of them may come back
     * valid (or NaN). */
    {
        static autotune_sample_t s[MAX_SAMPLES];

        /* Never oscillated: a monotonic climb, e.g. the relay never switched
         * because the setpoint was above what the plant could reach. */
        int m = 0;
        for (int i = 0; i < 240; i++, m++) {
            s[m].t_s = (float)i * 5.0f;
            s[m].measurement_c = 20.0f + 0.1f * (float)i * 5.0f;
        }
        relay_model_t r = pid_autotune_fit_relay(s, m, d, h);
        TEST_CHECK(!r.valid, "a monotonic (never-oscillating) trace is rejected, not fitted");

        /* Dead flat: no cycles at all. */
        for (int i = 0; i < 240; i++) {
            s[i].t_s = (float)i * 5.0f;
            s[i].measurement_c = 220.0f;
        }
        relay_model_t r_flat = pid_autotune_fit_relay(s, 240, d, h);
        TEST_CHECK(!r_flat.valid, "a flat trace is rejected");
        TEST_CHECK(r_flat.ku == 0.0f && r_flat.tu_s == 0.0f, "a rejected relay fit reports zeros, never NaN");

        /* Oscillating, cleanly and for many cycles, but with an amplitude
         * smaller than the hysteresis band -- a^2 - h^2 < 0. The maths would
         * hand back a NaN if the check were done after the sqrt instead of
         * before it. */
        for (int i = 0; i < 480; i++) {
            float t = (float)i * 5.0f;
            s[i].t_s = t;
            s[i].measurement_c = 220.0f + 1.0f * sinf(2.0f * 3.14159265f * t / 200.0f);
        }
        relay_model_t r_small = pid_autotune_fit_relay(s, 480, d, 2.0f);
        TEST_CHECK(!r_small.valid, "an oscillation smaller than the hysteresis band is rejected");
        TEST_CHECK(!isnan(r_small.ku), "sub-hysteresis oscillation yields no NaN Ku");

        /* Same sine, with a hysteresis band it comfortably clears: proves the
         * rejection above was the a<=h guard firing and not the cycle
         * detector failing on this waveform. a = 1.0 by construction. */
        relay_model_t r_ok = pid_autotune_fit_relay(s, 480, d, 0.2f);
        TEST_CHECK(r_ok.valid, "the same sine fits fine once the hysteresis band is below its amplitude");
        TEST_CHECK_NEAR(r_ok.amplitude_c, 1.0f, 0.05, "reported amplitude is HALF peak-to-peak, not peak-to-peak");
        TEST_CHECK_NEAR(r_ok.tu_s, 200.0f, 5.0, "period recovered from a synthetic 200 s sine");

        /* Still converging: a decaying oscillation. Cycle-to-cycle amplitude
         * consistency is what separates "settled into a limit cycle" from
         * "on its way somewhere", and a trace like this must be rejected
         * rather than averaged into a plausible-looking wrong answer. */
        for (int i = 0; i < 480; i++) {
            float t = (float)i * 5.0f;
            s[i].t_s = t;
            s[i].measurement_c = 220.0f + 20.0f * expf(-t / 400.0f) * sinf(2.0f * 3.14159265f * t / 200.0f);
        }
        relay_model_t r_decay = pid_autotune_fit_relay(s, 480, d, h);
        TEST_CHECK(!r_decay.valid, "a decaying (never-settled) oscillation is rejected, not fitted");

        /* Degenerate relay amplitude: nothing was driving the oscillation. */
        relay_model_t r_nod = pid_autotune_fit_relay(samples, n, 0.0f, h);
        TEST_CHECK(!r_nod.valid, "relay amplitude d==0 is rejected (no excitation, no gain)");

        relay_model_t r_short = pid_autotune_fit_relay(samples, 4, d, h);
        TEST_CHECK(!r_short.valid, "a trace too short to contain any cycle is rejected");
    }
}
