// sim_scenario_table -- the S0/S1/S3 rows (WI-4). See the header for the
// "adding a scenario is one entry" contract.
#include "sim_scenario_table.h"

#include <stddef.h>

#include "sim_measured_zone_constants.h"

const char *const SIM_ARM_NAMES[SIM_ARM_COUNT] = {
    "A_PID", "A_PID_AT", "A_FUZZY25", "A_FUZZY50", "A_FUZZY_AT", "A_STATIC_MATCHED",
};

// Zone 0's real measured bench constants (sim_measured_zone_constants.h,
// live GET /api/zones 2026-09-10), used as-is by S0/S1's legacy plant, same
// convention every other harness in this tree (sim_fuzzy_closedloop.c,
// sim_iter_tune.c) already uses.
#define BENCH_K_DC   (g_k_dc[0])
#define BENCH_TAU_S  (g_tau_s[0])
#define BENCH_DEAD_TIME_S (g_dead_time_s[0])
#define BENCH_AMBIENT_C 24.0f

static sim_plant_cfg_t legacy_bench_plant(void)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = BENCH_AMBIENT_C;
    p.node_model = SIM_NODE_LEGACY;
    p.thermal_mass_j_per_c = BENCH_TAU_S;
    p.heater_power_w = BENCH_K_DC;
    p.loss_coeff_w_per_c = 1.0f;
    p.sensor_delay_s = BENCH_DEAD_TIME_S;
    p.sensor_lag_tau_s = 0.0f;
    return p;
}

// S3 (SENSOR_CENTRE) -- three-node model, sensor_bias_p = 0.0 (all
// conductance to the load, the centre-mounted reference case), built so its
// AGGREGATE (K, tau) matches S1's legacy plant EXACTLY at steady state and
// APPROXIMATELY in the transient (singular-perturbation argument below),
// per WI-4's own acceptance criterion (c): "S3's results match S1's within
// materiality, proving the three-node model at sensor_bias_p = 0 is not
// itself a confound."
//
// Derivation (TEST FIXTURE, not measured):
//   - g_ea_w_per_c = 0 (no direct element->ambient loss; every loss path
//     through the load, matching legacy's single loss term).
//   - g_la_w_per_c = 1.0, c_l_j_per_c = BENCH_TAU_S: at steady state, with
//     g_ea = 0, the E balance gives G_el*(E-L) = u*Pmax, and substituting
//     into the L balance gives L - Tamb = u*Pmax/G_la at steady state --
//     independent of G_el/C_e entirely. Setting Pmax=BENCH_K_DC and
//     G_la=1.0 reproduces legacy's DC gain (heater_power_w/loss_coeff_w_per_c
//     = K_dc/1.0) exactly, bit for bit at steady state.
//   - c_e_j_per_c = 13.0, g_el_w_per_c = 1.0: this makes the element node's
//     own relaxation time C_e/G_el = 13 s, roughly 20x faster than the
//     load's C_l/G_la = BENCH_TAU_S (~264 s on zone 0). By the standard
//     singular-perturbation argument, when the fast state (E) is much
//     faster than the slow state (L), E quasi-instantly satisfies
//     0 = u*Pmax - G_el*(E-L) (with g_ea=0), i.e. E = L + u*Pmax/G_el, and
//     substituting into dL/dt collapses the two-node system to EXACTLY
//     legacy's dL/dt = (u*Pmax - G_la*(L-Tamb))/C_l, to the extent the
//     separation holds (~20x here -- not exact, hence "within materiality,"
//     not "bit-identical," which is the whole point of this being a
//     SEPARATE code path from S1's legacy step per sim_plant.h's own
//     bit-identical-default contract). 13 s is also comfortably above the
//     dt_s = 1.0 s this suite runs at (forward-Euler stability wants the
//     fast mode's own tau well above dt; 13x margin is ample).
//   - sensor_bias_p = 0.0: the sensor sees ONLY the load (G_se=0), so its
//     placement contributes nothing here -- this scenario isolates "is the
//     three-node CODE PATH itself a confound," not sensor placement, which
//     is S2/S4 (WI-6).
//   - sensor_tau_s = 10.0 (TEST FIXTURE, same value test_sim_plant_three_
//     node.c uses for "a sheathed kiln TC"): a small additional lag on top
//     of legacy's sensor_delay_s (applied identically to both plants via
//     the shared transport-delay/lag pipeline), negligible against a ~264 s
//     bulk time constant.
static sim_plant_cfg_t three_node_centre_plant(void)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = BENCH_AMBIENT_C;
    p.node_model = SIM_NODE_THREE;
    p.heater_power_w = BENCH_K_DC;      /* P_max */
    p.c_e_j_per_c = 3.0f;               /* TEST FIXTURE: fast element mode, ~88x faster than the load */
    p.c_l_j_per_c = BENCH_TAU_S;        /* matches legacy thermal_mass_j_per_c exactly */
    p.c_s_j_per_c = 1.0f;               /* TEST FIXTURE: sensor tip capacity, only the ratio to sensor_tau_s matters */
    p.g_el_w_per_c = 1.0f;              /* TEST FIXTURE */
    p.g_ea_w_per_c = 0.0f;              /* all loss through the load, matching legacy's single loss term */
    p.g_la_w_per_c = 1.0f;              /* matches legacy loss_coeff_w_per_c exactly -- sets the DC gain */
    p.sensor_tau_s = 10.0f;             /* TEST FIXTURE, same convention as test_sim_plant_three_node.c */
    p.sensor_bias_p = 0.0f;             /* centre-mounted: sensor sees only the load */
    p.load_mass_mult = 1.0f;
    p.sensor_delay_s = BENCH_DEAD_TIME_S; /* same transport delay as legacy, applied to node S */
    p.sensor_lag_tau_s = 0.0f;
    return p;
}

static const sim_scenario_t TABLE[] = {
    {
        .id = "S0_NULL_SLOW",
        .plant = { 0 }, /* filled below via designated init workaround -- see note */
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 40.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 25.0f,
        .sep_expected = false,
        .sep_reason = "Control: legacy bench plant, matched tune, a slow ramp every arm converges on. "
                      "If THIS one separates, the harness itself is broken, not the controller.",
    },
    {
        .id = "S1_BASELINE",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = false,
        .sep_reason = "Reproduces the 1570a65a bench condition; anchors every other scenario. "
                      "Not pinned yes/no on separation a priori by this table -- WI-4 only asserts "
                      "S0 and S3 against S1, not S1's own arm separation (that is the 1570a65a "
                      "finding itself, out of WI-4's scope).",
    },
    {
        .id = "S3_SENSOR_CENTRE",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = false,
        .sep_reason = "Isolates \"3-node model\" from \"sensor placement\": sensor_bias_p=0 (centre-"
                      "mounted). Must behave like S1 -- see sim_scenario_table_init() for how the "
                      "plant is actually installed (designated-init limitation, C99 requires the "
                      "struct fields in order for a nested initializer here, so plant is patched at "
                      "load time by sim_scenario_table_init(), called once from main()).",
    },
};

#define SIM_SCENARIO_COUNT_EXPECTED 3
_Static_assert(sizeof(TABLE) / sizeof(TABLE[0]) == SIM_SCENARIO_COUNT_EXPECTED,
               "sim_scenario_table.c: TABLE grew or shrank without a deliberate review of "
               "SIM_SCENARIO_COUNT_EXPECTED -- bump the constant here, on purpose, when adding a row "
               "(WI-4 ships S0/S1/S3 only; S2/S4-S12 are WI-6).");

// Mutable copy the runner reads from -- sim_plant_cfg_t is too large/complex
// (nested TEST FIXTURE floats) to hand-write as a C89-compatible nested
// designated initializer inline above without either duplicating
// legacy_bench_plant()/three_node_centre_plant()'s logic per row (exactly
// the kind of duplication WI-1's own compatibility contract warns against)
// or relying on GNU statement-expressions. Patched once, before first use,
// by sim_scenario_table_init() below -- NOT per-call, so the table is still
// effectively `static const` from every caller's perspective after main()
// calls this once.
static sim_scenario_t g_table[SIM_SCENARIO_COUNT_EXPECTED];
static bool g_table_ready = false;

void sim_scenario_table_init(void);
void sim_scenario_table_init(void)
{
    if (g_table_ready) return;
    for (int i = 0; i < SIM_SCENARIO_COUNT_EXPECTED; i++) {
        g_table[i] = TABLE[i];
        /* g_k_dc[0]/g_tau_s[0]/g_dead_time_s[0] are runtime array reads, not
         * compile-time constants, so they cannot appear in TABLE's static
         * initializer above (MSVC C2099) -- patched here instead, once. */
        g_table[i].model_k_dc = BENCH_K_DC;
        g_table[i].model_tau_s = BENCH_TAU_S;
        g_table[i].model_dead_time_s = BENCH_DEAD_TIME_S;
    }
    g_table[0].plant = legacy_bench_plant();
    g_table[1].plant = legacy_bench_plant();
    g_table[2].plant = three_node_centre_plant();
    g_table_ready = true;
}

const sim_scenario_t *sim_scenario_table(void)
{
    sim_scenario_table_init();
    return g_table;
}

const int SIM_SCENARIO_COUNT = SIM_SCENARIO_COUNT_EXPECTED;
