// Tests for sim_high_temp.c (WI-2, docs/SCENARIO_SIMULATION_PLAN.md sec
// 2.3). Every constant in this file is a TEST FIXTURE -- never shipped,
// never written into zones_config, a preset, or a firmware default.
#include <math.h>
#include <stdio.h>

#include "sim_high_temp.h"
#include "sim_plant.h"
#include "test_common.h"

static void test_scale_at_reference_is_exactly_one(void)
{
    TEST_SECTION("sim_loss_conductance_scale(T_REF) == 1.0 exactly");
    float s = sim_loss_conductance_scale(SIM_HIGH_TEMP_T_REF_C);
    TEST_CHECK(s == 1.0f, "scale must be exactly 1.0 at the reference temperature");
    printf("  s(%.1f) = %.9f\n", (double)SIM_HIGH_TEMP_T_REF_C, (double)s);
}

static void test_scale_at_1200c_matches_published_table(void)
{
    TEST_SECTION("sim_loss_conductance_scale(1200) within 0.5% of the published 21.3");
    float s = sim_loss_conductance_scale(1200.0f);
    float published = 21.3f;
    float rel_err = fabsf(s - published) / published;
    TEST_CHECK(rel_err <= 0.005f, "scale at 1200C must be within 0.5% of 21.3");
    printf("  s(1200) = %.4f (published 21.3, rel err %.4f%%)\n", (double)s, (double)(rel_err * 100.0));
}

/* Runs the kiln-scale cfg at full duty forever, re-scaling g_ea/g_la from
 * the LOAD node's own temperature every tick (sec 2.3), and asserts the
 * LOAD node settles inside the required [1250, 1400] C band. dt=100s for
 * 40000 ticks is ~46 simulated days -- chosen by direct experiment to
 * comfortably reach the plant's true steady state (see sim_high_temp.c's
 * header comment); trivial to execute, since nothing here is real time. */
static void test_kiln_scale_full_duty_reaches_cone_range(void)
{
    TEST_SECTION("kiln-scale plant at full duty asymptotes the load node between 1250C and 1400C");

    sim_plant_cfg_t cfg;
    sim_high_temp_kiln_scale_cfg(&cfg);
    const float base_g_ea = cfg.g_ea_w_per_c;
    const float base_g_la = cfg.g_la_w_per_c;

    sim_plant_state_t st;
    sim_plant_reset(&st, &cfg);

    const float dt_s = 100.0f;
    const int steps = 40000;
    for (int i = 0; i < steps; i++) {
        sim_high_temp_scale_conductances(&cfg, base_g_ea, base_g_la, st.load_c);
        sim_plant_three_node_step(&st, &cfg, 1.0f, dt_s);
    }

    printf("  after %d ticks (%.1f simulated days): element_c=%.2f load_c=%.2f\n",
           steps, (double)(steps * dt_s / 86400.0), (double)st.element_c, (double)st.load_c);
    TEST_CHECK(st.load_c >= 1250.0f && st.load_c <= 1400.0f,
               "load node must settle between 1250C and 1400C at full duty");

    /* Confirm it has actually settled, not just passed through the band --
     * one more tick should move it by well under 0.1C. */
    float load_before = st.load_c;
    sim_high_temp_scale_conductances(&cfg, base_g_ea, base_g_la, st.load_c);
    sim_plant_three_node_step(&st, &cfg, 1.0f, dt_s);
    TEST_CHECK(fabsf(st.load_c - load_before) < 0.1f,
               "load node must have settled (near-zero further drift), not merely be transiting the band");
}

void run_test_sim_high_temp(void)
{
    /* Catches this TU being linked against a stale/mismatched sim_plant.o --
     * see sim_plant.h's SIM_PLANT_ASSERT_ABI_FRESH() comment. */
    SIM_PLANT_ASSERT_ABI_FRESH();
    test_scale_at_reference_is_exactly_one();
    test_scale_at_1200c_matches_published_table();
    test_kiln_scale_full_duty_reaches_cone_range();
}
