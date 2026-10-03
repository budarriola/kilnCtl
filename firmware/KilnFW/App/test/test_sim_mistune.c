// Tests for sim_mistune.c (WI-3, docs/SCENARIO_SIMULATION.md sec 2.4).
// Every constant in this file is a TEST FIXTURE -- never shipped, never
// written into zones_config, a preset, or a firmware default.
#include <math.h>
#include <stdio.h>

#include "pid_autotune.h"
#include "sim_measured_zone_constants.h"
#include "sim_mistune.h"
#include "test_common.h"

/* Zone 0's bench-measured FOPDT model (sim_measured_zone_constants.h),
 * reused here as "the true plant" so this test is not inventing its own
 * disconnected k/tau/L triple. */
#define TRUE_K   g_k_dc[0]
#define TRUE_TAU g_tau_s[0]
#define TRUE_L   g_dead_time_s[0]

static void test_matched_reproduces_direct_call_bit_for_bit(void)
{
    TEST_SECTION("SIM_MISTUNE_MATCHED reproduces a direct pid_autotune_tune_from_fopdt() call, bit-for-bit");

    autotune_gains_t via_helper = sim_mistune_tune(TRUE_K, TRUE_TAU, TRUE_L, SIM_MISTUNE_MATCHED);

    fopdt_model_t direct_model = {0};
    direct_model.k_gain_c_per_duty = TRUE_K;
    direct_model.tau_s = TRUE_TAU;
    direct_model.dead_time_s = TRUE_L;
    direct_model.valid = true;
    autotune_gains_t direct = pid_autotune_tune_from_fopdt(&direct_model, AUTOTUNE_RULE_SIMC, 0.0f);

    TEST_CHECK(via_helper.refusal == AUTOTUNE_REFUSAL_OK, "matched tune must not be refused");
    TEST_CHECK(via_helper.kp == direct.kp, "MATCHED kp must equal the direct call's kp bit-for-bit");
    TEST_CHECK(via_helper.ki == direct.ki, "MATCHED ki must equal the direct call's ki bit-for-bit");
    TEST_CHECK(via_helper.kd == direct.kd, "MATCHED kd must equal the direct call's kd bit-for-bit");
    printf("  matched: kp=%.6f ki=%.6f kd=%.6f\n", (double)via_helper.kp, (double)via_helper.ki, (double)via_helper.kd);
}

/* Independently computes the SIMC Kc the production formula would produce
 * for a given (possibly mismatched) model, WITHOUT calling production code
 * -- this is the "computed in the test from the formula, not a pasted
 * literal" reference the WI-3 acceptance criterion requires. Mirrors
 * pid_autotune.c's SIMC branch: lambda = 3*L (lambda_s<=0 default),
 * Kc = tau / (K * (lambda + L)). */
static float expected_simc_kc(float k, float tau, float l)
{
    float lambda = 3.0f * l;
    return tau / (k * (lambda + l));
}

static void test_hot_kp_matches_formula_predicted_ratio(void)
{
    TEST_SECTION("TUNE_HOT's Kp exceeds TUNE_MATCHED's by exactly the ratio the SIMC formula predicts");

    autotune_gains_t matched = sim_mistune_tune(TRUE_K, TRUE_TAU, TRUE_L, SIM_MISTUNE_MATCHED);
    autotune_gains_t hot = sim_mistune_tune(TRUE_K, TRUE_TAU, TRUE_L, SIM_MISTUNE_HOT);
    TEST_CHECK(matched.refusal == AUTOTUNE_REFUSAL_OK, "matched tune must not be refused");
    TEST_CHECK(hot.refusal == AUTOTUNE_REFUSAL_OK, "hot tune must not be refused");

    sim_mistune_factors_t hot_f = sim_mistune_factors(SIM_MISTUNE_HOT);
    float kc_matched = expected_simc_kc(TRUE_K, TRUE_TAU, TRUE_L);
    float kc_hot = expected_simc_kc(TRUE_K * hot_f.mismatch_k, TRUE_TAU * hot_f.mismatch_tau, TRUE_L * hot_f.mismatch_l);
    float expected_ratio = kc_hot / kc_matched;

    float actual_ratio = hot.kp / matched.kp;
    float rel_err = fabsf(actual_ratio - expected_ratio) / expected_ratio;

    printf("  matched kp=%.6f  hot kp=%.6f  actual ratio=%.4fx  formula-predicted ratio=%.4fx\n",
           (double)matched.kp, (double)hot.kp, (double)actual_ratio, (double)expected_ratio);
    TEST_CHECK(rel_err < 1e-4f, "hot/matched Kp ratio must match the SIMC-formula-predicted ratio");
    TEST_CHECK(actual_ratio > 1.0f, "TUNE_HOT must actually be more aggressive than TUNE_MATCHED (Kp up)");
}

static void test_cold_and_slow_integral_are_refused_never(void)
{
    TEST_SECTION("TUNE_COLD and TUNE_SLOW_INTEGRAL produce valid (unrefused) gains on this plant");

    autotune_gains_t cold = sim_mistune_tune(TRUE_K, TRUE_TAU, TRUE_L, SIM_MISTUNE_COLD);
    autotune_gains_t slow_i = sim_mistune_tune(TRUE_K, TRUE_TAU, TRUE_L, SIM_MISTUNE_SLOW_INTEGRAL);
    autotune_gains_t matched = sim_mistune_tune(TRUE_K, TRUE_TAU, TRUE_L, SIM_MISTUNE_MATCHED);

    TEST_CHECK(cold.refusal == AUTOTUNE_REFUSAL_OK, "cold tune must not be refused");
    TEST_CHECK(slow_i.refusal == AUTOTUNE_REFUSAL_OK, "slow-integral tune must not be refused");
    TEST_CHECK(cold.kp < matched.kp, "TUNE_COLD must be less aggressive than TUNE_MATCHED (Kp down)");
    printf("  cold kp=%.6f (matched %.6f)  slow_integral ki=%.6f (matched %.6f)\n",
           (double)cold.kp, (double)matched.kp, (double)slow_i.ki, (double)matched.ki);
}

void run_test_sim_mistune(void)
{
    test_matched_reproduces_direct_call_bit_for_bit();
    test_hot_kp_matches_formula_predicted_ratio();
    test_cold_and_slow_integral_are_refused_never();
}
