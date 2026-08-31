// Validates pid_autotune.c's FOPDT fit against sim_plant.c's known ground
// truth -- TODO.md 6A.4/6A.8's "autotune against the sim first... the only
// place the identification math can be validated exactly."
#include "test_common.h"
#include "../drivers/pid_autotune.h"
#include "sim_plant.h"
#include <string.h>

#define MAX_SAMPLES 4096

/* Defined below, after the step-test checks it is called from -- the relay
 * section carries a long explanation of what it can and cannot assert, and
 * that belongs next to the code it explains rather than at the top of the
 * file. */
static void run_test_pid_autotune_relay(void);

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

    run_test_pid_autotune_relay();
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
