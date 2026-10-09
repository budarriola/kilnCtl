// sim_high_temp -- TEST FIXTURE: never shipped; bench values never ship.
//
// Ports tools/PcTools/src/kilnctrl/plant_sim.py's loss_conductance_scale()
// into C for the scenario suite's high-temperature work (WI-2,
// docs/SCENARIO_SIMULATION.md sec 2.3), and supplies a kiln-scale
// sim_plant_cfg_t (SIM_NODE_THREE, WI-1) anchored to that file's physical
// high-temperature model (PHYS_P_MAX_W etc.) rather than the bench rig's
// measured constants -- the rig's own identified K_diag cannot reach 40 C
// above ambient at full duty, so no loss model turns that identification
// into a plant that reaches cone-10 range (see plant_sim.py's own comment
// on this, and sec 1.2's "much higher temperatures" row).
//
// Every constant in this file is a TEST FIXTURE -- never shipped, never
// written into zones_config, a preset, or a firmware default.
#ifndef SIM_HIGH_TEMP_H
#define SIM_HIGH_TEMP_H

#include "sim_plant.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Anchor temperature where the scale below is exactly 1.0 -- MEASURED (the
 * bench excitation runs' dwell temperature), matches plant_sim.py's
 * T_REF_C. */
#define SIM_HIGH_TEMP_T_REF_C 55.0f

/* [ASSUMED] -- what share of TOTAL heat loss (conductive + radiative) is
 * radiative at SIM_HIGH_TEMP_T_REF_C. There is NO measurement of this split
 * at any temperature in this dataset (every capture available stays at or
 * below ~80 C); ported verbatim from plant_sim.py's
 * RAD_LOSS_FRACTION_AT_REF, whose own docstring carries the same caveat.
 * This is the number to revisit first if real high-temperature (bisque+)
 * thermocouple data ever becomes available. */
#define SIM_HIGH_TEMP_RAD_LOSS_FRACTION_AT_REF 0.05f

/* Total thermal conductance at temp_c relative to the conductance at
 * SIM_HIGH_TEMP_T_REF_C, i.e. C_total(T)/C_total(T_ref). Port of
 * plant_sim.py's loss_conductance_scale() -- see that function's docstring
 * for the physical justification (K = P_max/C_total and tau = C_thermal/
 * C_total share the loss-conductance denominator, so scaling a
 * conductance by this factor is the physically consistent way to extend a
 * loss-conductance change without inventing a second free constant).
 * Normalized so sim_loss_conductance_scale(SIM_HIGH_TEMP_T_REF_C) == 1.0
 * exactly. */
float sim_loss_conductance_scale(float temp_c);

/* Rewrites cfg->g_ea_w_per_c / cfg->g_la_w_per_c to base_g_ea_w_per_c /
 * base_g_la_w_per_c scaled by sim_loss_conductance_scale(reference_temp_c).
 * Sec 2.3 applies the scale to G_ea and G_la ONLY -- never to g_el, which
 * is the element<->load path, not a loss to ambient, and is left
 * untouched. Call once per tick, BEFORE sim_plant_three_node_step(), with
 * reference_temp_c set to the LOAD node's own temperature (state->load_c)
 * from the start of that tick (K and tau share the *loss*-side
 * conductance, which is the ambient-facing side modelled here). */
void sim_high_temp_scale_conductances(sim_plant_cfg_t *cfg, float base_g_ea_w_per_c,
                                       float base_g_la_w_per_c, float reference_temp_c);

/* A SIM_NODE_THREE cfg anchored to plant_sim.py's physical high-temperature
 * model (single-zone PHYS_P_MAX_W rating), with g_ea_w_per_c/g_la_w_per_c
 * set to their T_REF-anchored (scale == 1.0) BASE values -- the caller must
 * still call sim_high_temp_scale_conductances() every tick (passing those
 * same two base values back in) for the high-temperature growth to take
 * effect; without it this cfg behaves as an ordinary, unscaled three-node
 * plant and will NOT reach kiln-scale temperatures.
 *
 * Chosen (see docs/SCENARIO_SIMULATION.md WI-2 acceptance) so that,
 * with the scaler applied every tick using the LOAD node's own temperature,
 * full duty held forever asymptotes the LOAD node (load_c -- the
 * chamber/ware node a real controller's thermocouple sits closest to)
 * near 1288 C, inside the required [1250, 1400] C band. The ELEMENT node
 * settles well above that (consistent with real kilns, where element
 * temperature runs above chamber air) -- that is expected, not a defect;
 * do not "fix" it by rebalancing g_el_w_per_c.
 *
 * TEST FIXTURE -- never shipped; bench values never ship. */
void sim_high_temp_kiln_scale_cfg(sim_plant_cfg_t *out);

#ifdef __cplusplus
}
#endif

#endif // SIM_HIGH_TEMP_H
