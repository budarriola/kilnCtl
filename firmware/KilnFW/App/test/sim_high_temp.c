// sim_high_temp -- TEST FIXTURE: never shipped; bench values never ship.
// See sim_high_temp.h for what this ports and why.
#include "sim_high_temp.h"

float sim_loss_conductance_scale(float temp_c)
{
    const float rad_ref = SIM_HIGH_TEMP_RAD_LOSS_FRACTION_AT_REF;
    const float cond_ref = 1.0f - rad_ref;
    const float t_ref_k = SIM_HIGH_TEMP_T_REF_C + 273.15f;
    const float t_k = temp_c + 273.15f;
    const float ratio = t_k / t_ref_k;
    const float ratio4 = ratio * ratio * ratio * ratio;
    return cond_ref + rad_ref * ratio4;
}

void sim_high_temp_scale_conductances(sim_plant_cfg_t *cfg, float base_g_ea_w_per_c,
                                       float base_g_la_w_per_c, float reference_temp_c)
{
    const float scale = sim_loss_conductance_scale(reference_temp_c);
    cfg->g_ea_w_per_c = base_g_ea_w_per_c * scale;
    cfg->g_la_w_per_c = base_g_la_w_per_c * scale;
}

void sim_high_temp_kiln_scale_cfg(sim_plant_cfg_t *out)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = 20.0f;
    p.node_model = SIM_NODE_THREE;

    /* PHYS_P_MAX_W single-zone rating, plant_sim.py -- ASSUMED, a typical
     * small/medium home/studio electric kiln zone, not measured on any
     * hardware in this repo. */
    p.heater_power_w = 2500.0f;

    /* TEST FIXTURE capacities: element much lighter than the bulk
     * chamber+ware+refractory load, sensor tip lighter still -- same
     * "clearly separated time constants" posture as
     * test_sim_plant_three_node.c's fixture, just rescaled to a kiln-sized
     * power/temperature range. */
    p.c_e_j_per_c = 50000.0f;
    p.c_l_j_per_c = 2000000.0f;
    p.c_s_j_per_c = 500.0f;

    /* g_el chosen 50x the total base ambient conductance (below) so E and L
     * stay tightly coupled rather than the element running away completely
     * unbounded relative to the load -- TEST FIXTURE, not measured. */
    p.g_el_w_per_c = 3.294f;

    /* Base (T == SIM_HIGH_TEMP_T_REF_C, scale == 1.0) ambient conductances.
     * Solved from the steady-state balance
     * P_max = (g_ea0 + g_la0) * sim_loss_conductance_scale(T) * (T - ambient)
     * for T = 1325 C (band centre), 60/40 split between element-ambient and
     * load-ambient loss, then verified by direct simulation (40000 ticks at
     * dt=100s, i.e. ~46 simulated days -- comfortably past settling) to
     * converge the LOAD node to ~1288 C, inside [1250, 1400]. TEST FIXTURE,
     * not measured -- see this file's header comment. */
    p.g_ea_w_per_c = 0.039528f;
    p.g_la_w_per_c = 0.026352f;

    p.sensor_tau_s = 10.0f;
    p.sensor_bias_p = 0.5f; /* neutral placement; WI-2 is not testing sec 2.1 */
    p.load_mass_mult = 1.0f;

    *out = p;
}
