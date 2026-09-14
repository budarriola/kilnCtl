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
     * power/temperature range.
     *
     * WI-6/S10-S11 correction, 2026-09-14: the original c_l_j_per_c =
     * 2,000,000 J/C, combined with heater_power_w = 2500 W, gives a
     * near-ambient full-duty heating rate of only P/C_l = 2500/2e6 =
     * 0.00125 C/s = 4.5 C/hr -- more than 30x SLOWER than the 150 C/hr ramp
     * rate S10/S11 command. Ramp/dwell scheduling here is a fixed
     * wall-clock rate independent of the plant (sim_scenarios.c steps
     * target_c by a constant per-tick delta); a plant that cannot
     * physically keep up with that schedule pins duty at 1.0 for the
     * entire firing regardless of gains, kp/ki/kd, or pid_range_c -- the
     * measured "no separation" on S10/S11 traces to THIS (a fixture
     * power/thermal-mass mismatch), not to pid_range_c being too narrow.
     * See docs/audits/scenario_factorial_design_2026-09-14.md and this
     * file's own header before touching this again.
     *
     * Fixed by scaling every capacity down by the same 2,000,000/15,000 =
     * 133.33x factor (steady-state values -- g_ea/g_la/heater_power_w --
     * are capacity-independent, so the documented ~1288 C full-duty
     * asymptote is unaffected; only how fast the plant gets there
     * changes). c_l_j_per_c = 15,000 J/C gives a near-ambient full-duty
     * rate of 2500/15000 = 0.1667 C/s = 600 C/hr, 4x the commanded ramp
     * rate -- enough headroom for the loop to track without permanent
     * saturation, and a tau at the 200 C tune point (~488 s, see
     * kiln_scale_tune_at()) in the same order of magnitude as the bench
     * scenarios' ~264 s, so SIMC tuning behaves sanely. c_e/c_s keep their
     * ORIGINAL ratios to c_l (1/40 and 1/4000 respectively) so the
     * element/load/sensor time-constant separation this model depends on
     * is unchanged, just uniformly rescaled. TEST FIXTURE, not measured. */
    p.c_e_j_per_c = 375.0f;
    p.c_l_j_per_c = 15000.0f;
    p.c_s_j_per_c = 4.0f;

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
