// Host tests for thermal_model.c. docs/PLAN.md section 13.1: golden-trace
// tests pin the integration math.
#include <string.h>

#include "test_common.h"
#include "../src/sim/thermal_model.h"

/* Golden trace: single zone, no coupling, no TC lag, so the substepped
 * forward-Euler integration is hand-verifiable --
 *   P_heater = duty * V_mains^2 / R_element * element_health
 *            = 1.0 * 100^2 / 10 * 1.0 = 1000 W
 *   dT/dt = (P_heater - k_loss*(T - T_ambient)) / C
 *         = (1000 - 10*(T-25)) / 1000
 * at 4 substeps/tick (timescale=1), dt_s=1.0s. Values below were captured
 * from a real run of this exact code (firmware/SimFW/src/sim/thermal_model.c)
 * and are pinned here -- a future change to the integration math, substep
 * count, or the P_heater/loss formula will change these and must be caught. */
static void test_golden_trace(void)
{
    TEST_SECTION("thermal_model -- golden trace, single zone, no coupling/lag");

    thermal_model_params_t p;
    memset(&p, 0, sizeof(p));
    p.zone_count = 1;
    p.V_mains = 100.0f;
    p.T_ambient = 25.0f;
    p.zones[0].C = 1000.0f;
    p.zones[0].k_loss = 10.0f;
    p.zones[0].R_element = 10.0f;
    p.zones[0].element_health = 1.0f;
    p.zones[0].tc_lag_s = 0.0f;
    p.zones[0].T0 = 25.0f;

    thermal_model_state_t s;
    thermal_model_init(&s, &p);
    TEST_CHECK_NEAR(s.T_zone[0], 25.0, 1e-6, "init: T_zone == T0");
    TEST_CHECK_NEAR(s.T_tc[0], 25.0, 1e-6, "init: T_tc == T0 (no artificial startup transient)");

    float duty[THERMAL_MODEL_MAX_ZONES] = {1.0f, 0.0f, 0.0f, 0.0f};
    static const double expected_heating[6] = {
        25.996254, 26.982584, 27.9590874, 28.9258614, 29.8830032, 30.8306122
    };
    for (int tick = 0; tick < 6; tick++) {
        thermal_model_tick(&s, &p, duty, 1.0f, 1u);
        TEST_CHECK_NEAR(s.T_zone[0], expected_heating[tick], 1e-3, "golden heating trace tick matches pinned value");
        TEST_CHECK_NEAR(s.T_tc[0], expected_heating[tick], 1e-3, "tc_lag_s==0: T_tc tracks T_zone exactly");
    }

    duty[0] = 0.0f;
    static const double expected_cooling[3] = {
        30.7725258, 30.7150173, 30.6580811
    };
    for (int tick = 0; tick < 3; tick++) {
        thermal_model_tick(&s, &p, duty, 1.0f, 1u);
        TEST_CHECK_NEAR(s.T_zone[0], expected_cooling[tick], 1e-3, "golden cooling trace tick matches pinned value");
    }
}

/* Sanity: heater on -> zone warms; heater off -> zone cools toward ambient.
 * Uses the fast_test preset (the default regression preset) rather than
 * hand-picked params, so this test also exercises thermal_model_load_preset. */
static void test_sanity_heat_and_cool(void)
{
    TEST_SECTION("thermal_model -- sanity: heater on warms, heater off cools toward ambient");

    thermal_model_params_t p;
    thermal_model_load_preset(THERMAL_PRESET_FAST_TEST, &p);
    TEST_CHECK(p.zone_count >= 1 && p.zone_count <= THERMAL_MODEL_MAX_ZONES, "fast_test preset has a sane zone count");

    thermal_model_state_t s;
    thermal_model_init(&s, &p);
    float t0 = s.T_zone[0];

    float duty_on[THERMAL_MODEL_MAX_ZONES] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 50; i++) {
        thermal_model_tick(&s, &p, duty_on, 0.1f, 1u);
    }
    float t_hot = s.T_zone[0];
    TEST_CHECK(t_hot > t0, "heater on for 5s (fast_test preset) warms zone 0 above its start temp");

    float duty_off[THERMAL_MODEL_MAX_ZONES] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 2000; i++) {
        thermal_model_tick(&s, &p, duty_off, 0.1f, 1u);
    }
    float t_cooled = s.T_zone[0];
    TEST_CHECK(t_cooled < t_hot, "heater off for 200s cools zone 0 back down from its hot peak");
    TEST_CHECK(t_cooled - p.T_ambient < (t_hot - p.T_ambient), "cooled temp is closer to ambient than the hot peak was");

    /* Reported (lagged) TC temperature must never diverge wildly from true
     * zone temperature -- it is a first-order lag, not an independent
     * process. */
    TEST_CHECK(fabs((double)(s.T_tc[0] - s.T_zone[0])) < 50.0, "lagged TC reading stays within a sane band of true zone temp");
}

/* A multi-zone run with real coupling (three_zone preset) must keep every
 * zone's temperature finite and bounded above ambient while heating -- a
 * cheap guard against a coupling-matrix sign error blowing up the
 * integration. */
static void test_multi_zone_coupling_stays_bounded(void)
{
    TEST_SECTION("thermal_model -- three_zone preset stays numerically sane under coupling");

    thermal_model_params_t p;
    thermal_model_load_preset(THERMAL_PRESET_THREE_ZONE, &p);
    TEST_CHECK(p.zone_count == 3, "three_zone preset declares 3 zones");

    thermal_model_state_t s;
    thermal_model_init(&s, &p);

    float duty[THERMAL_MODEL_MAX_ZONES] = {1.0f, 1.0f, 1.0f, 0.0f};
    for (int i = 0; i < 500; i++) {
        thermal_model_tick(&s, &p, duty, 1.0f, 1u);
        for (uint8_t z = 0; z < p.zone_count; z++) {
            TEST_CHECK(s.T_zone[z] == s.T_zone[z], "T_zone is not NaN"); /* NaN != NaN */
            TEST_CHECK(s.T_zone[z] < 5000.0f, "T_zone stays in a physically sane range (no blow-up)");
            TEST_CHECK(s.T_zone[z] >= p.T_ambient - 1.0f, "T_zone with heaters on never drops meaningfully below ambient");
        }
    }
}

void run_test_thermal_model(void)
{
    test_golden_trace();
    test_sanity_heat_and_cool();
    test_multi_zone_coupling_stays_bounded();
}
