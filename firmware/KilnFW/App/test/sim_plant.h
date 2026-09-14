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
#include <stddef.h>
#include <stdint.h>

#include "max31856_codec.h" /* MAX31856_CHANNEL_COUNT, MAX31856_TC_TEMP_C_PER_LSB */
#include "zones_config_json.h" /* zone_cfg_t -- G1's real-config seam, see sim_plant_from_zone_cfg() */

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_PLANT_DELAY_MAX_STEPS 64

/* SCENARIO_SIMULATION_PLAN.md WI-1: opt-in three-node model (element / bulk
 * load / sensor tip), selected per-plant via sim_plant_cfg_t::node_model.
 * SIM_NODE_LEGACY (0, the default) takes the ORIGINAL one-node code path in
 * sim_plant_step() completely unchanged -- not a three-node model run with
 * degenerate parameters, deliberately, because a "mathematically
 * equivalent" reparameterisation would perturb floating-point results in
 * the last bits and break sim_iter_tune's pinned 24/21/615 acceptance
 * count. Every field below this comment is read ONLY by
 * sim_plant_three_node_step() and is inert (need not even be initialized)
 * under SIM_NODE_LEGACY. TEST FIXTURE constants only -- never shipped, never
 * written into zones_config, a preset, or a firmware default. */
typedef enum {
    SIM_NODE_LEGACY = 0,
    SIM_NODE_THREE = 1,
} sim_node_model_t;

typedef struct {
    float ambient_c;
    float thermal_mass_j_per_c;  /* how much energy raises the element 1C */
    float heater_power_w;        /* power delivered at duty == 1.0 -- also used as P_max under SIM_NODE_THREE */
    float loss_coeff_w_per_c;    /* heat loss rate = loss_coeff * (T - ambient) */
    float sensor_delay_s;        /* transport delay between element and thermocouple reading */
    float sensor_lag_tau_s;      /* first-order lag on the thermocouple reading itself (thermal mass of the TC) */

    /* -------- SIM_NODE_THREE only (SCENARIO_SIMULATION_PLAN.md sec 2.1/2.2) -------- */
    sim_node_model_t node_model;  /* SIM_NODE_LEGACY (0) default */
    float c_e_j_per_c;   /* element/near-element gas capacity C_e */
    float c_l_j_per_c;   /* bulk load (ware + refractory) capacity C_l, before load_mass_mult */
    float c_s_j_per_c;   /* sensor tip capacity C_s */
    float g_el_w_per_c;  /* element<->load conductance G_el */
    float g_ea_w_per_c;  /* element->ambient loss conductance G_ea */
    float g_la_w_per_c;  /* load->ambient loss conductance G_la */
    float sensor_tau_s;  /* sensor tip's OWN time constant (total conductance
                           * G_s = c_s_j_per_c / sensor_tau_s); distinct from
                           * sensor_lag_tau_s above, which is an ADDITIONAL
                           * lag applied downstream of this node, same as legacy */
    float sensor_bias_p; /* fraction of the tip's total conductance G_s that
                           * goes to the element rather than the load, [0,1].
                           * G_se = G_s*p, G_sl = G_s*(1-p). "5x closer to the
                           * elements" is INTERPRETED as a 5:1 conductance
                           * ratio, p = 5/6 ~= 0.8333 -- an interpretation,
                           * not a measurement (sec 2.1). p=0 is the
                           * centre-mounted reference case (sensor sees only
                           * the bulk). */
    float load_mass_mult; /* multiplies c_l_j_per_c only (sec 2.2); a value
                            * of <= 0 is treated as 1.0 (unset), so a caller
                            * building a SIM_NODE_THREE cfg that does not
                            * care about this scenario need not set it. */
} sim_plant_cfg_t;

typedef struct {
    float element_c;                 /* "true" temperature of the heated mass (node E) */
    float sensor_c;                  /* lagged, delayed reading the thermocouple actually reports */
    float delay_ring[SIM_PLANT_DELAY_MAX_STEPS];
    int   delay_len;                 /* number of valid entries in delay_ring, growing to its capacity */
    int   delay_head;                /* next write position (ring buffer) */

    /* SIM_NODE_THREE only. Always initialized to ambient_c by
     * sim_plant_reset() regardless of node_model (harmless under
     * SIM_NODE_LEGACY, which never reads them). */
    float load_c;          /* bulk load node L -- ground truth no real installation can see */
    float sensor_node_c;   /* sensor tip node S, BEFORE the transport delay/lag pipeline below is applied */
} sim_plant_state_t;

/* Starts both element_c and sensor_c at ambient_c, clears the delay ring. */
void sim_plant_reset(sim_plant_state_t *state, const sim_plant_cfg_t *cfg);

/* Advances the model by dt_s using heater duty in [0,1]. dt_s must stay
 * small and constant enough for SIM_PLANT_DELAY_MAX_STEPS * dt_s to cover
 * cfg->sensor_delay_s, or the delay is silently clamped to the ring's
 * capacity -- fine for the tick rates this is meant to exercise (1-10s),
 * not a general-purpose variable-timestep integrator. */
void sim_plant_step(sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float duty, float dt_s);

/* WI-1: the opt-in three-node step (SCENARIO_SIMULATION_PLAN.md sec 2.1).
 * Only valid when cfg->node_model == SIM_NODE_THREE -- the caller is
 * responsible for choosing which step function to call per plant; this file
 * does not dispatch on node_model itself, so SIM_NODE_LEGACY plants must go
 * through sim_plant_step() unchanged, preserving its bit-identical output.
 * Advances E, L and S from one common snapshot (locals hold the deltas
 * before any of the three state fields are mutated), so the order of the
 * three balance equations cannot matter. Reuses the existing transport-delay
 * ring and sensor_lag_tau_s first-order lag, applied to S instead of to E,
 * so the MAX31856 quantiser/noise/fault paths downstream keep working
 * unchanged. */
void sim_plant_three_node_step(sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float duty, float dt_s);

/* ---- ABI-freshness guard (added 2026-09-14, see
 * docs/audits/sim_iter_tune_stale_object_triage_2026-09-14.md) ----
 *
 * WI-1 grew sim_plant_cfg_t/sim_plant_state_t. Every one of this file's
 * consumers is passed pointers to these structs across a translation-unit
 * boundary, so if any TU that touches them (a harness's .c file, or a
 * leftover .obj from before WI-1) was compiled against a DIFFERENT
 * sim_plant.h than sim_plant.c itself -- a stale object file, or a
 * deliberate partial revert during investigation -- the mismatch is
 * undefined behaviour: silently wrong numbers (a moved acceptance bar) or
 * a crash, with no diagnostic pointing at the real cause. That exact shape
 * cost an investigating agent a false "the baseline is broken" conclusion
 * on 2026-09-14, when the actual fault was stale/mismatched objects from a
 * partial manual rebuild, not any defect in firing_score.c/firing_compare.c.
 *
 * sim_plant_assert_abi_fresh_impl() is defined once, in sim_plant.c, and
 * compares the sizes it computes locally (i.e. as sim_plant.c's own
 * compilation sees these structs) against whatever the CALLING TU passes
 * in via the macro below (i.e. as that TU's own compilation sees them). A
 * mismatch means at least one of the linked .o files was built against a
 * different version of this header -- exactly the class of bug this guard
 * exists to catch -- and it aborts loudly naming both sizes rather than
 * letting the program run on a corrupted struct. Call
 * SIM_PLANT_ASSERT_ABI_FRESH() once near the top of main() in any host
 * harness that links sim_plant.c together with other .c files that also
 * touch these structs (sim_iter_tune.c, sim_wide_temp_sweep.c, the
 * test_sim_plant_three_node.c / test_closed_loop.c host tests). */
void sim_plant_assert_abi_fresh_impl(size_t caller_state_size, size_t caller_cfg_size, const char *caller_file);
#define SIM_PLANT_ASSERT_ABI_FRESH() \
    sim_plant_assert_abi_fresh_impl(sizeof(sim_plant_state_t), sizeof(sim_plant_cfg_t), __FILE__)

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
    /* ADDITIVE source-gain coupling: coupling_w_per_c[i][j] is the power
     * (W) delivered into zone i's element balance per unit of zone j's
     * commanded duty (u_j in [0,1]) -- NOT a conductance on (T_j - T_i).
     * Matches the firmware's model class (zone_coupling_solve.c's
     * G = diag(model_k_dc) + coupling_coeff, off-diagonals driven by the
     * neighbour's duty, not the temperature difference between zones) --
     * see sim_kiln_step()'s own comment for why the two must agree.
     * Not necessarily symmetric even in physical reality (nothing requires
     * zone i to warm zone j exactly as much as the reverse), and nothing
     * here enforces symmetry either way. [i][i] is ignored. */
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

/* ------------------------------------------------------------------------
 * G1 (ITER_TUNE_REDESIGN_PLAN.md sec 6.1/6.2) -- build a sim_plant_cfg_t from
 * the board's REAL measured FOPDT parameters (zone_cfg_t::model_k_dc/
 * model_tau_s/model_dead_time_s -- the same fields zone_model_at()'s
 * passthrough seam reads, zones_config_accessors.c/.h) instead of hand-set
 * constants, so a harness built on this cannot silently drift from the
 * on-flash config schema. Deliberately takes a `const zone_cfg_t *` rather
 * than calling zone_model_at()/zones_config_get_model() directly: those
 * read a file-scope global inside zones_config_accessors.c, which would
 * force every caller of this function (including host tests that link
 * sim_plant.c but never touch zones_config, e.g. test_sim_kiln.c) to also
 * link the whole zones_config_accessors.c/zones_config_json.c/NVS stack.
 * Passing the struct keeps the schema dependency (the actual point of G1)
 * without the link-time one; a caller that already has zones_config loaded
 * fetches with zone_model_at()/zones_config_get_model() and passes the
 * result in.
 *
 * Mapping (sec 6.2, exact up to one free scale h = loss_coeff_w_per_c = 1.0):
 *   heater_power_w        = model_k_dc         (steady-state gain K)
 *   thermal_mass_j_per_c  = model_tau_s         (time constant tau = C/h)
 *   sensor_delay_s        = model_dead_time_s   (identified dead time L)
 *   sensor_lag_tau_s      = 0                   (L already lumps sensor lag;
 *                                                 splitting it would double-count)
 * Units become nominal, not physical -- fine, because nothing downstream of
 * sim_plant_step()/sim_kiln_step() scores watts, only input/output dynamics,
 * which this mapping reproduces exactly.
 *
 * Returns false (leaving *out unmodified) if any of model_k_dc/model_tau_s/
 * model_dead_time_s is non-finite or non-positive (a zone whose model has
 * never been fitted reads 0 for all three -- see zones_config_migrate.c --
 * and 0 tau/heater_power would divide by zero in sim_plant_step()). */
bool sim_plant_from_zone_cfg(const zone_cfg_t *zcfg, float ambient_c, sim_plant_cfg_t *out);

/* G1's coupling counterpart: sim_kiln_step() now uses the ADDITIVE
 * source-gain coupling model (see that function's own comment), the same
 * model class the firmware's zone_coupling_solve.c solves
 * (G = diag(model_k_dc) + coupling_coeff). In that model class the measured
 * cross-gain coupling_coeff[i][j] (zone i's rise per unit duty in zone j,
 * as zones_config_get_coupling()/coupling_at() report it) *is* the
 * conductance sim_kiln wants -- coupling_w_per_c[i][j] = coupling_coeff[i][j]
 * directly, no fit or k_dc-scaled algebraic cut required. The two functions
 * that used to live here (sim_kiln_coupling_from_cross_gain(), an algebraic
 * first cut for the retired temperature-difference exchange model, and
 * sim_kiln_coupling_fit_iterative(), its numerical refinement) both existed
 * to compensate for that older model's loading behaviour and are
 * unnecessary now that the coefficient is used as-is; removed
 * 2026-09-10, see docs/audits/sim_credibility_gate_real_cause_2026-09-10.md. */

/* ------------------------------------------------------------------------
 * G3 (plan sec 6.1) -- relay actuation lag: a fixed transport delay on the
 * *commanded relay state* itself, distinct from sim_plant_cfg_t's
 * sensor_delay_s (which sits on the reading, not the actuator). This is the
 * exact mechanism that defeated relay-feedback autotune identification on
 * the bench (project_relay_ident_actuation_lag.md): 1 Hz relay-law switching
 * actuated through a 60 s heater_output.c PWM window, with actuation lag on
 * top, jittering every edge past the fit tolerance. Ring-buffer delay line
 * on a bool, same shape as sim_plant_state_t's own delay_ring.
 * ------------------------------------------------------------------------ */
#define SIM_RELAY_LAG_RING_MAX 8

typedef struct {
    bool ring[SIM_RELAY_LAG_RING_MAX];
    int  head;
    int  len;
} sim_relay_lag_t;

/* Feeds `commanded` in, returns the relay state lag_s in the past (clamped to
 * the ring's capacity, same "acceptable approximation, not a general
 * variable-timestep model" posture as sim_plant.c's own sensor delay). Call
 * once per tick per zone with a fresh sim_relay_lag_t (zero-initialize, e.g.
 * via memset, before the first call -- there is no separate reset function
 * because there is no cfg to reset against, unlike sim_plant_reset()). */
bool sim_relay_lag_step(sim_relay_lag_t *state, bool commanded, float lag_s, float dt_s);

/* ------------------------------------------------------------------------
 * G4 (plan sec 6.1) -- MAX31856 quantisation. Real hardware resolution is
 * NOT 1/MAX31856_TC_TEMP_C_PER_LSB (0.000244 C): that constant is the LSB
 * weight of the raw 24-bit LTCB register word, but max31856_decode_tc()
 * (max31856_codec.c) masks the low 5 bits of that word to 0 before decoding
 * (LTCBL[4:0] is documented don't-care) -- the header comment on
 * MAX31856_TC_TEMP_C_PER_LSB itself says so: "equivalently 0.0078125 degC
 * per 19-bit code". The achievable resolution is therefore
 * 32 * MAX31856_TC_TEMP_C_PER_LSB = 0.0078125 C (2^-7 C, the datasheet's
 * documented 19-bit linearized-TC resolution), not the raw-register LSB.
 * Quantizing at the finer, wrong constant would let the harness exercise
 * precision the real converter never delivers -- exactly the "unquantized
 * synthetic data hides real branches" bug class this gap exists to close. */
#define SIM_MAX31856_TC_RESOLUTION_C (32.0f * MAX31856_TC_TEMP_C_PER_LSB)

/* Rounds to the nearest real MAX31856 code; passes NaN through unchanged
 * (an open/fault reading has no code to round to). Apply AFTER sim_kiln's
 * own sensor noise and BEFORE the value reaches any controller code, per
 * plan sec 6.1's G4 row. */
float sim_max31856_quantize_tc(float temperature_c);

#ifdef __cplusplus
}
#endif

#endif // SIM_PLANT_H
