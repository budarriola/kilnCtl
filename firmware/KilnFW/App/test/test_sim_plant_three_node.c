// Tests for sim_plant.c's opt-in three-node model (WI-1,
// docs/SCENARIO_SIMULATION_PLAN.md sec 2.1/2.2). Every constant in this file
// is a TEST FIXTURE -- never shipped, never written into zones_config, a
// preset, or a firmware default.
//
// What is being checked, and why: the entire justification for the
// three-node model is the physical claim that a sensor mounted near the
// elements (high sensor_bias_p) LEADS the bulk load on a rise and FALLS
// FASTER than the load when the elements cut out, because its dominant heat
// source disappeared while the load -- holding nearly all the stored energy
// -- barely moves (sec 2.1's "physical claim being modelled" and sec 5.3's
// structural invariant #1). If this file's asserts do not hold, the sensor
// model is wrong and every scenario built on it (S2, S4, S5, S6, S10-S12) is
// void. The centre-mounted case (sensor_bias_p = 0) is the converse check:
// a sensor with all its conductance on the load must LAG it, same as any
// first-order filter on the load's own signal.
#include <math.h>
#include <stdio.h>

#include "sim_plant.h"
#include "test_common.h"

#define DT_S 1.0f

/* Test-fixture plant: an element 100x lighter (by capacity) than the bulk
 * load, so a duty step drives the element to a quasi-equilibrium with the
 * (still nearly stationary) load almost immediately -- at these constants
 * that quasi-equilibrium already sits at ~75% of the eventual, fully joint
 * steady state, so the element's own step response is dominated by a fast
 * mode with time constant on the order of tens of seconds, while the load
 * only reaches the same steady state on a mode roughly two orders of
 * magnitude slower (driven by its own capacity against the small ambient
 * loss conductances). A small, fast-responding sensor tip (its own bare tau
 * = c_s/g_s = 10s) then reports whichever weighted mix of the two the
 * placement bias (sensor_bias_p) selects. None of these numbers are
 * measured or physical; they exist only to give the two nodes a clearly
 * separated pair of effective time constants so the assertions below are
 * unambiguous. */
static sim_plant_cfg_t three_node_cfg(float sensor_bias_p)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = 20.0f;
    p.node_model = SIM_NODE_THREE;
    p.heater_power_w = 1000.0f;   /* P_max -- TEST FIXTURE */
    p.c_e_j_per_c = 50.0f;        /* element capacity -- TEST FIXTURE, 100x lighter than the load */
    p.c_l_j_per_c = 5000.0f;      /* load capacity -- TEST FIXTURE, slow bulk */
    p.c_s_j_per_c = 1.0f;         /* sensor tip capacity -- TEST FIXTURE, tiny */
    p.g_el_w_per_c = 1.0f;        /* element<->load conductance -- TEST FIXTURE */
    p.g_ea_w_per_c = 1.0f;        /* element->ambient loss -- TEST FIXTURE */
    p.g_la_w_per_c = 1.0f;        /* load->ambient loss -- TEST FIXTURE */
    p.sensor_tau_s = 10.0f;       /* sensor tip's own bare time constant -- TEST FIXTURE */
    p.sensor_bias_p = sensor_bias_p;
    p.load_mass_mult = 1.0f;
    /* sensor_delay_s / sensor_lag_tau_s left at 0: this test is isolating
     * the three-node placement effect itself, not the downstream transport
     * pipeline (already covered by the legacy-model tests). */
    return p;
}

/* Runs a duty step from 0 to `duty` for max_steps ticks, recording the
 * sensor NODE (pre-delay/lag; delay/lag are both 0 in this cfg so
 * state.sensor_c tracks sensor_node_c exactly) and load traces. */
static void run_step_response(const sim_plant_cfg_t *cfg, float duty, int max_steps,
                               float *sensor_c, float *load_c)
{
    sim_plant_state_t st;
    sim_plant_reset(&st, cfg);
    for (int i = 0; i < max_steps; i++) {
        sim_plant_three_node_step(&st, cfg, duty, DT_S);
        sensor_c[i] = st.sensor_c;
        load_c[i] = st.load_c;
    }
}

/* First index whose trace has covered `frac` of the way from `start` to the
 * trace's own final value (index max_steps-1) -- a standard normalized rise
 * time, robust to the two nodes having different DC gains. Returns
 * max_steps if it never gets there (should not happen at these fixture
 * constants over the chosen horizon). */
static int rise_time_steps(const float *trace, int max_steps, float start, float frac)
{
    float final_v = trace[max_steps - 1];
    float target = start + frac * (final_v - start);
    for (int i = 0; i < max_steps; i++) {
        if ((final_v >= start && trace[i] >= target) ||
            (final_v < start && trace[i] <= target)) {
            return i;
        }
    }
    return max_steps;
}

/* The coupled (E,L) pair has two time constants -- a fast one set by
 * C_e/(G_el+G_ea) and a much slower one set by the overall capacity vs.
 * ambient loss (C_e+C_l)/(G_ea+G_la), roughly 2750s at this file's fixture
 * constants. rise_steps must run long enough for the LOAD trace itself to
 * approach its true asymptote, or the normalized-rise-time metric below
 * measures against a moving target instead of the real steady state. */
#define RISE_STEPS 15000

static void test_sensor_leads_load_near_element(void)
{
    TEST_SECTION("three-node: sensor_bias_p=0.8333 leads load on rise, falls faster on cutout");

    sim_plant_cfg_t cfg = three_node_cfg(5.0f / 6.0f); /* "5x closer to elements", sec 2.1 */
    const int rise_steps = RISE_STEPS;
    static float sensor_rise[RISE_STEPS], load_rise[RISE_STEPS];
    run_step_response(&cfg, 1.0f, rise_steps, sensor_rise, load_rise);

    /* Sensor leads the load throughout the rise: sensor_c - load_c > 0 at
     * every sample past the first couple of ticks (allow a brief settle-in
     * of the discretisation at t=0..1). */
    bool leads_throughout = true;
    for (int i = 2; i < rise_steps; i++) {
        if (sensor_rise[i] <= load_rise[i]) { leads_throughout = false; break; }
    }
    TEST_CHECK(leads_throughout, "sensor must lead (read hotter than) the load throughout the rise");

    int sensor_t63 = rise_time_steps(sensor_rise, rise_steps, cfg.ambient_c, 0.63f);
    int load_t63 = rise_time_steps(load_rise, rise_steps, cfg.ambient_c, 0.63f);
    TEST_CHECK(sensor_t63 * 3 <= load_t63,
               "sensor's 63% rise time must be at least 3x shorter than the load's");
    printf("  sensor_t63=%ds load_t63=%ds (ratio %.2fx)\n", sensor_t63, load_t63,
           (double)load_t63 / (sensor_t63 > 0 ? sensor_t63 : 1));

    /* Cut-out: start both nodes from the settled rise endpoint, drop duty to
     * 0, and compare the drop over the first 60s. */
    sim_plant_state_t st;
    sim_plant_reset(&st, &cfg);
    for (int i = 0; i < rise_steps; i++) {
        sim_plant_three_node_step(&st, &cfg, 1.0f, DT_S);
    }
    float sensor0 = st.sensor_c;
    float load0 = st.load_c;
    for (int i = 0; i < 60; i++) {
        sim_plant_three_node_step(&st, &cfg, 0.0f, DT_S);
    }
    float sensor_drop = sensor0 - st.sensor_c;
    float load_drop = load0 - st.load_c;
    TEST_CHECK(sensor_drop >= 5.0f * load_drop,
               "sensor must fall at least 5x faster than the load in the first 60s after cutout");
    printf("  60s after cutout: sensor_drop=%.4fC load_drop=%.4fC (ratio %.2fx)\n",
           (double)sensor_drop, (double)load_drop, (double)(sensor_drop / (load_drop > 1e-6f ? load_drop : 1e-6f)));
}

static void test_sensor_lags_load_centre_mounted(void)
{
    TEST_SECTION("three-node: sensor_bias_p=0.0 (centre-mounted) lags the load");

    sim_plant_cfg_t cfg = three_node_cfg(0.0f);
    const int rise_steps = RISE_STEPS;
    static float sensor_rise[RISE_STEPS], load_rise[RISE_STEPS];
    run_step_response(&cfg, 1.0f, rise_steps, sensor_rise, load_rise);

    /* All of the sensor's conductance goes to the load (g_se=0), so the
     * sensor is exactly a first-order lag on load_c and must read AT OR
     * BELOW it throughout a rise (never lead). */
    bool never_leads = true;
    for (int i = 2; i < rise_steps; i++) {
        if (sensor_rise[i] > load_rise[i] + 1e-4f) { never_leads = false; break; }
    }
    TEST_CHECK(never_leads, "centre-mounted sensor must never read hotter than the load during a rise");

    int sensor_t63 = rise_time_steps(sensor_rise, rise_steps, cfg.ambient_c, 0.63f);
    int load_t63 = rise_time_steps(load_rise, rise_steps, cfg.ambient_c, 0.63f);
    TEST_CHECK(sensor_t63 >= load_t63,
               "centre-mounted sensor's 63% rise time must be at least as slow as the load's own (a lag, not a lead)");
    printf("  sensor_t63=%ds load_t63=%ds\n", sensor_t63, load_t63);
}

static void test_legacy_path_untouched(void)
{
    TEST_SECTION("three-node fields are inert under SIM_NODE_LEGACY (sim_plant_step unchanged)");

    /* Same cfg shape test_sim_kiln.c's base_plant() uses, so this is
     * comparing against a known-stable legacy trajectory shape, not a
     * fixture invented just for this file. node_model defaults to
     * SIM_NODE_LEGACY (0) via the {0}-initialisation below; the three-node
     * fields are left non-zero-but-nonsense on purpose, to prove
     * sim_plant_step() never reads them. */
    sim_plant_cfg_t cfg = {0};
    cfg.ambient_c = 20.0f;
    cfg.thermal_mass_j_per_c = 50000.0f;
    cfg.heater_power_w = 2000.0f;
    cfg.loss_coeff_w_per_c = 5.0f;
    cfg.sensor_delay_s = 5.0f;
    cfg.sensor_lag_tau_s = 8.0f;
    /* node_model == SIM_NODE_LEGACY (0) implicitly. Poison the three-node
     * fields with values that would visibly perturb the trajectory if
     * anything in sim_plant_step() accidentally read them. */
    cfg.c_e_j_per_c = 1.0f;
    cfg.c_l_j_per_c = 1.0f;
    cfg.c_s_j_per_c = 1.0f;
    cfg.g_el_w_per_c = 999.0f;
    cfg.g_ea_w_per_c = 999.0f;
    cfg.g_la_w_per_c = 999.0f;
    cfg.sensor_tau_s = 0.001f;
    cfg.sensor_bias_p = 1.0f;
    cfg.load_mass_mult = 1.0f;

    sim_plant_state_t st_poisoned, st_clean;
    sim_plant_reset(&st_poisoned, &cfg);

    sim_plant_cfg_t clean = cfg;
    clean.c_e_j_per_c = 0.0f;
    clean.c_l_j_per_c = 0.0f;
    clean.c_s_j_per_c = 0.0f;
    clean.g_el_w_per_c = 0.0f;
    clean.g_ea_w_per_c = 0.0f;
    clean.g_la_w_per_c = 0.0f;
    clean.sensor_tau_s = 0.0f;
    clean.sensor_bias_p = 0.0f;
    clean.load_mass_mult = 0.0f;
    sim_plant_reset(&st_clean, &clean);

    for (int i = 0; i < 500; i++) {
        sim_plant_step(&st_poisoned, &cfg, 0.5f, DT_S);
        sim_plant_step(&st_clean, &clean, 0.5f, DT_S);
    }
    TEST_CHECK(st_poisoned.element_c == st_clean.element_c,
               "sim_plant_step() must be bit-identical regardless of the (unused) three-node fields");
    TEST_CHECK(st_poisoned.sensor_c == st_clean.sensor_c,
               "sim_plant_step() sensor_c must be bit-identical regardless of the (unused) three-node fields");
}

void run_test_sim_plant_three_node(void)
{
    /* Catches this TU being linked against a stale/mismatched sim_plant.o --
     * see sim_plant.h's SIM_PLANT_ASSERT_ABI_FRESH() comment. */
    SIM_PLANT_ASSERT_ABI_FRESH();
    test_sensor_leads_load_near_element();
    test_sensor_lags_load_centre_mounted();
    test_legacy_path_untouched();
}
