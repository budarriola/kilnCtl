// thermal_model -- pure, host-testable per-zone RC thermal model for SimFW.
// docs/PLAN.md section 4.3 is authoritative for the math and the parameter
// table; this header/](.c) implement it verbatim, no FreeRTOS/pico-sdk.
//
//   C_zone * dT/dt = P_heater(t) - k_loss*(T - T_ambient)
//                    - sum_j k_couple[j]*(T - T_neighbor_j)
//   P_heater = duty(relay chain) * V_mains^2 / R_element * element_health
//
// Forward Euler, substepped: PLAN.md 4.3 says the "fast unit-test kiln"
// preset (time constants of seconds) needs 4 Euler substeps per 10 Hz tick
// instead of a fancier integrator, and "at time-scale N the substep count
// multiplies by N so accuracy does not degrade when runs are accelerated" --
// thermal_model_tick()'s `timescale` argument is exactly that N.
//
// A configurable first-order TC lag sits between true zone temperature and
// the *reported* TC temperature (PLAN.md 4.3: "several guards care about the
// difference between element temperature and sensed temperature").
//
// All math in float; state in plain Celsius (not Kelvin-offset internally --
// only the offset choice changes, not the physics, and Celsius keeps the
// presets' typical-value table directly readable).
#ifndef SIMFW_SIM_THERMAL_MODEL_H
#define SIMFW_SIM_THERMAL_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PLAN.md section 3.1/4.3: "1-4 zones (default 3)". */
#define THERMAL_MODEL_MAX_ZONES 4u

/* Per-zone parameters, PLAN.md 4.3's table verbatim (all settable over USB
 * in the real firmware; here just a plain struct). k_couple[j] is this
 * zone's coupling coefficient to zone j -- the caller is responsible for
 * keeping the matrix symmetric (k_couple[i].k_couple[j] == k_couple[j].k_couple[i])
 * across the zones array; this module does not enforce it, it just sums
 * whatever is given. Diagonal entries (k_couple[i] for zone i itself) are
 * ignored (never used to couple a zone to itself). */
typedef struct {
    float C;                                   /* J/degC thermal mass */
    float k_loss;                               /* W/degC loss to ambient */
    float k_couple[THERMAL_MODEL_MAX_ZONES];    /* W/degC coupling to zone j, diag ignored */
    float R_element;                            /* ohms, heater element resistance */
    float element_health;                       /* 0..1, 1 = good, 0 = broken/open */
    float tc_lag_s;                              /* TC sensor first-order time constant, s */
    float T0;                                    /* initial temperature, degC */
} thermal_zone_params_t;

/* Globals, PLAN.md 4.3: "V_mains (default 240), T_ambient". Process noise and
 * safety-TC blend are sim_engine/fault_engine concerns layered on top of this
 * pure model, not part of it. */
typedef struct {
    uint8_t zone_count;                          /* 1..THERMAL_MODEL_MAX_ZONES */
    thermal_zone_params_t zones[THERMAL_MODEL_MAX_ZONES];
    float V_mains;                                /* volts */
    float T_ambient;                              /* degC */
} thermal_model_params_t;

/* PLAN.md 4.3's four named presets. */
typedef enum {
    THERMAL_PRESET_FAST_TEST = 0,   /* seconds-scale time constants, ~2 min full firing */
    THERMAL_PRESET_SMALL_KILN,      /* single-zone dominant, ~1h scale */
    THERMAL_PRESET_THREE_ZONE,      /* realistic 3-zone, top/mid/bottom coupling+lag asymmetry */
    THERMAL_PRESET_STRESS,          /* huge lag, weak coupling, low mass -- PID-hostile */
    THERMAL_PRESET_COUNT
} thermal_preset_id_t;

/* Fills *out_params with the named preset's parameter table. Presets are
 * starting points (PLAN.md 4.3: "scenarios may override any parameter") --
 * the caller is free to mutate the result before calling thermal_model_init. */
void thermal_model_load_preset(thermal_preset_id_t preset, thermal_model_params_t *out_params);

/* Live state: true per-zone temperature and the lagged/reported TC
 * temperature a caller would encode into an emulated MAX31856. Only the
 * first params->zone_count entries of each array are meaningful. */
typedef struct {
    float T_zone[THERMAL_MODEL_MAX_ZONES];  /* true zone temperature, degC */
    float T_tc[THERMAL_MODEL_MAX_ZONES];    /* TC-lagged reported temperature, degC */
} thermal_model_state_t;

/* Seeds state from params->zones[i].T0 for every zone (T_tc starts equal to
 * T_zone -- a sensor that has been sitting at the same temperature forever
 * reports the truth, no transient lag applied at t=0). */
void thermal_model_init(thermal_model_state_t *state, const thermal_model_params_t *params);

/* One tick of the model. duty[i] is zone i's heater duty this tick, in
 * [0,1] (fraction of the tick the heater chain is conducting -- 0 = relay
 * open, 1 = relay fully closed the whole tick). Only the first
 * params->zone_count entries of duty are read.
 *
 * dt_s is the tick period (wall-clock or sim-clock seconds, whichever the
 * caller's clock domain is -- this module has no clock of its own).
 * timescale multiplies the substep count per PLAN.md 4.3's accuracy-at-speed
 * rule; pass 1 for real time, N for an N x accelerated run. timescale==0 is
 * treated as 1 (never divide the substep count to zero). */
void thermal_model_tick(thermal_model_state_t *state,
                         const thermal_model_params_t *params,
                         const float *duty,
                         float dt_s,
                         uint32_t timescale);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_THERMAL_MODEL_H
