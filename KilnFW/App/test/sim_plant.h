// sim_plant -- minimal host-side kiln thermal model. TODO.md section 6A.8.
//
// Not a physically-rigorous kiln model -- just enough first-order-lag-plus-
// dead-time behavior (a heating element's thermal mass, heat loss to
// ambient proportional to delta-T, and a transport delay between the
// element and the thermocouple) to falsify pid.c/thermal_guard.c/
// heater_output.c logic bugs cheaply, off-target, before they ever meet a
// real kiln. Pure C, no dependencies beyond libc -- builds with the host
// compiler, not the ESP-IDF cross toolchain.
#ifndef SIM_PLANT_H
#define SIM_PLANT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_PLANT_DELAY_MAX_STEPS 64

typedef struct {
    float ambient_c;
    float thermal_mass_j_per_c;  /* how much energy raises the element 1C */
    float heater_power_w;        /* power delivered at duty == 1.0 */
    float loss_coeff_w_per_c;    /* heat loss rate = loss_coeff * (T - ambient) */
    float sensor_delay_s;        /* transport delay between element and thermocouple reading */
    float sensor_lag_tau_s;      /* first-order lag on the thermocouple reading itself (thermal mass of the TC) */
} sim_plant_cfg_t;

typedef struct {
    float element_c;                 /* "true" temperature of the heated mass */
    float sensor_c;                  /* lagged, delayed reading the thermocouple actually reports */
    float delay_ring[SIM_PLANT_DELAY_MAX_STEPS];
    int   delay_len;                 /* number of valid entries in delay_ring, growing to its capacity */
    int   delay_head;                /* next write position (ring buffer) */
} sim_plant_state_t;

/* Starts both element_c and sensor_c at ambient_c, clears the delay ring. */
void sim_plant_reset(sim_plant_state_t *state, const sim_plant_cfg_t *cfg);

/* Advances the model by dt_s using heater duty in [0,1]. dt_s must stay
 * small and constant enough for SIM_PLANT_DELAY_MAX_STEPS * dt_s to cover
 * cfg->sensor_delay_s, or the delay is silently clamped to the ring's
 * capacity -- fine for the tick rates this is meant to exercise (1-10s),
 * not a general-purpose variable-timestep integrator. */
void sim_plant_step(sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float duty, float dt_s);

/* ------------------------------------------------------------------------
 * sim_kiln -- N coupled zones plus injectable faults (TODO.md 6A.8's
 * "N coupled zones, inter-zone conductance, a radiative loss term,
 * injectable faults" bullet).
 *
 * Same first-order model as the single-zone plant above, extended with:
 *   - a conductance matrix, so zone i's elements really do heat zone j's
 *     thermocouple. This is what makes the cross-gain matrix TODO.md
 *     6A.5(b) captures during autotune testable off-target, and what guard
 *     8's threshold (6A.5's last bullet) eventually needs a real number
 *     from;
 *   - an optional radiative loss term, because a kiln's plant gain falls
 *     with temperature (the reason 6A.4 wants gain-scheduling bands) and a
 *     purely linear model can never show that;
 *   - per-zone fault injection, since the failures the guard suite exists
 *     to catch (welded contact, dead element, a thermocouple that fell out
 *     of the kiln body) cannot be provoked safely on real hardware.
 *
 * Faults are indexed by *physical* zone. cfg->sensor_map is applied on top,
 * at read time only: sim_kiln_reading_c(state, cfg, i) returns the pipeline
 * output of physical zone cfg->sensor_map[i]. So "TC frozen on zone 0" plus
 * "zones 0 and 1 swapped at the connector" means zone 1's reading is the
 * frozen one -- which is exactly how a real miswire behaves.
 * ------------------------------------------------------------------------ */

#define SIM_KILN_MAX_ZONES 3

typedef enum {
    SIM_ZONE_FAULT_NONE = 0,
    SIM_ZONE_FAULT_ELEMENT_DEAD,  /* relay closes, no heat delivered (open element) -- guard 1 */
    SIM_ZONE_FAULT_RELAY_WELDED,  /* full power regardless of commanded duty -- guard 3 */
    SIM_ZONE_FAULT_TC_DETACHED,   /* electrically fine, physically out of the kiln body -- guard 1/8 */
    SIM_ZONE_FAULT_TC_FROZEN,     /* answers SPI, stopped converting -- guard 7 */
    SIM_ZONE_FAULT_TC_OPEN,       /* reads NaN, i.e. THERMO_FAULT_OPEN -- guard 6 */
} sim_zone_fault_t;

typedef struct {
    sim_plant_cfg_t plant;             /* per-zone element mass/power/loss/sensor delay+lag */
    float radiative_coeff_w_per_k4;    /* extra loss = coeff * ((T+273.15)^4 - (Tamb+273.15)^4); 0 disables */
} sim_kiln_zone_cfg_t;

typedef struct {
    int   zone_count;
    sim_kiln_zone_cfg_t zone[SIM_KILN_MAX_ZONES];
    /* Conductance between zone i's and zone j's heated masses, W/degC.
     * Symmetric in physical reality; nothing here enforces that, so an
     * asymmetric matrix is allowed if a test wants one. [i][i] is ignored. */
    float coupling_w_per_c[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES];
    /* Zone i reads physical zone sensor_map[i]'s thermocouple. Identity
     * (sensor_map[i] == i) unless a test is modeling swapped connectors.
     * sim_kiln_reset() fills the identity map if zone_count is set and the
     * map is all zeroes. */
    int   sensor_map[SIM_KILN_MAX_ZONES];
    float sensor_noise_c;              /* peak-to-peak deterministic noise added at read time; 0 disables */
} sim_kiln_cfg_t;

typedef struct {
    sim_plant_state_t zone[SIM_KILN_MAX_ZONES];
    sim_zone_fault_t  fault[SIM_KILN_MAX_ZONES];
    float             frozen_c[SIM_KILN_MAX_ZONES];   /* value latched when TC_FROZEN was injected */
    /* Deterministic LCG, advanced once per sim_kiln_step() rather than per
     * read, so sim_kiln_reading_c() can stay const and repeated reads within
     * one tick agree with each other (a controller that read the same
     * channel twice and got two different numbers would be modeling a bug
     * this sim does not have). */
    uint32_t          rng;
} sim_kiln_state_t;

void sim_kiln_reset(sim_kiln_state_t *state, sim_kiln_cfg_t *cfg);

/* Injects (or with SIM_ZONE_FAULT_NONE, clears) a fault on one physical
 * zone, latching the current reading for TC_FROZEN. */
void sim_kiln_inject_fault(sim_kiln_state_t *state, const sim_kiln_cfg_t *cfg, int zone, sim_zone_fault_t fault);

/* Advances every zone one step. duty[] is one commanded duty per zone, in
 * [0,1] -- what the controller *commanded*, before fault injection rewrites
 * what the hardware actually does (a welded relay ignores it entirely).
 * Elements are updated simultaneously from the same starting temperatures,
 * so coupling is not order-dependent. */
void sim_kiln_step(sim_kiln_state_t *state, const sim_kiln_cfg_t *cfg, const float *duty, float dt_s);

/* The reading zone i's thermocouple reports: sensor-mapped, fault-modified,
 * noise-added. Returns NaN for SIM_ZONE_FAULT_TC_OPEN -- the caller is
 * expected to treat that the way profile_executor.c does (sensor_ok=false
 * into thermal_guard's guard 6), not to special-case it here. */
float sim_kiln_reading_c(const sim_kiln_state_t *state, const sim_kiln_cfg_t *cfg, int zone);

/* True element temperature of a physical zone -- the ground truth a real
 * controller never gets to see. For assertions only. */
float sim_kiln_element_c(const sim_kiln_state_t *state, int zone);

#ifdef __cplusplus
}
#endif

#endif // SIM_PLANT_H
