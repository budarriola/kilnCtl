// --- Gap-closure pass (see this file's/sim_engine.c's inline doc for each
// addition): sim_engine_get_seed() (telemetry.c's documented gap, PLAN.md
// 5.3's "seed" telemetry field); the safety-TC blend/lag/MANUAL/fault-
// override API (PLAN.md 4.3: "The safety-side TC reads a configurable blend
// of zone temps... with its own lag and its own fault knobs"); and three new
// fault-only zone overrides (thermal-mass surprise, TC-lag stress) mirroring
// the existing duty/health override pair's NOT-queued, fault_sched-only
// contract. ------------------------------------------------------------------
// sim_engine.h -- REAL BODY (milestone M-C, docs/PLAN.md section 4.1):
// "Thermal model tick (10 Hz), computes zone temps, element currents;
// publishes snapshot." Owns no peripheral (single-owner-per-peripheral
// doctrine, PLAN.md section 4's opening paragraph) -- it is the sole writer
// of the zone snapshot (section 4.5: "one struct, sim_engine sole writer,
// double-buffered with a sequence counter"), consumed by spi_emu_a/b (reg
// images), wave_owner (amplitudes) and telemetry. The pure thermal-model
// math itself lives in src/sim/thermal_model.c (PLAN.md section 9 repo
// layout, section 13 "host-testable") -- this task only ticks it and
// publishes the result, it does not implement the model.
//
// sim_snapshot_read()/sim_event_ring_drain() (src/sim/sim_snapshot.h's
// reader API) are implemented in sim_engine.c, per that header's ownership
// doctrine -- this file is the sole writer/producer for both.
//
// --- Zone-to-relay / zone-to-TC-channel mapping ASSUMPTION -----------------
// The real board's exact relay-to-zone wiring is not pinned down yet (no
// docs/HARDWARE.md exists). This module assumes the natural, ordinal
// mapping PLAN.md's own text implies wherever it lists relays and TC
// channels side by side (section 2's "Heat loop": "ESP32 closes K1/K2/K3");
// K1<->zone0, K2<->zone1, K3<->zone2, matching CS0/CS1/CS2's ordinal
// mapping to zone0/1/2 too (src/sim/tc_fault_state.h's TC_FAULT_CHANNEL_MAIN_0..2).
// K4 is the safety pilot (not a heater-zone driver) and K5 is unused by this
// mapping. A 4th zone (thermal_model.h supports up to 4) has no dedicated
// relay under this assumption -- its base duty is always 0 unless a fault
// override forces it. Confirm or correct against docs/HARDWARE.md once it
// exists (PLAN.md section 14 bring-up step 2), same caveat i2c_owner.h
// carries for its own provisional pin choices. Unlike K1/K2/K3, K4 is NOT a
// per-zone base-duty source -- it is a veto applied to every zone's final
// duty (sim_engine.c's sim_engine_tick(): open K4 forces duty to 0 for every
// zone, after both the relay-derived base and any fault_sched duty
// override), per PLAN.md's "closed AND K4 permits" and its "Current loop
// (safety side): relay closed and K4 pilot closed and element healthy".
#ifndef SIMFW_TASKS_SIM_ENGINE_H
#define SIMFW_TASKS_SIM_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "sim/thermal_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates sim_engine at SIMFW_PRIO_SIM_ENGINE, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h), ticking at
// SIMFW_PERIOD_SIM_ENGINE_MS (10 Hz). Also creates this module's internal
// command queue, snapshot/event-ring guards, and initializes the thermal
// model from THERMAL_PRESET_FAST_TEST (PLAN.md 4.3: "the default for
// automated regression"). Returns false if task/queue/mutex creation
// failed.
bool sim_engine_start(void);

// --- MODEL-group command surface (PLAN.md section 5) ------------------------
// All setters below queue a command applied at sim_engine's next tick
// boundary (mirrors i2c_owner.c's queue-then-apply-next-tick pattern, per
// this pass's own instructions) and are safe to call from any task. Getters
// are mutex-guarded synchronous reads of the task's own live state.

// Overwrites zone `zone`'s parameter set (thermal_model.h's
// thermal_zone_params_t). Returns false if zone is out of range for the
// currently loaded zone_count or the command could not be queued.
bool sim_engine_set_zone_params(uint8_t zone, const thermal_zone_params_t *params);

// Copies zone `zone`'s current parameter set into *out. Returns false if
// zone is out of range or out is NULL.
bool sim_engine_get_zone_params(uint8_t zone, thermal_zone_params_t *out);

// Loads a named preset (thermal_model_load_preset()) and reinitializes
// state from it (T0 for every zone) at the next tick boundary. Clears any
// MANUAL zone-temp overrides (a fresh preset load is a fresh run baseline).
bool sim_engine_load_preset(thermal_preset_id_t preset);

// Sets thermal_model_params_t.T_ambient globally.
bool sim_engine_set_ambient(float ambient_c);

// Sets the sim-clock time-scale, PLAN.md 4.2/5.2: x100 fixed point (1000 ==
// 10.00x accelerated, 100 == 1.00x real time, 0 treated as 1.00x). Affects
// both the substep count thermal_model_tick() uses (accuracy-at-speed,
// PLAN.md 4.3) and how far sim_time_us advances per wall tick.
bool sim_engine_set_timescale(uint32_t timescale_x100);

// Seeds sim_engine's own PRNG stream (reserved for future process-noise
// use, PLAN.md 4.3's "PRNG-driven process noise level... default 0 for
// byte-exact replays" -- this pass stores the seed but the model runs with
// noise disabled, so nothing yet consumes it beyond storage; wiring actual
// process noise is future work, left out deliberately rather than adding
// dead RNG-consuming code this pass does not need).
bool sim_engine_set_seed(uint32_t seed);

// Fresh run: reinitializes thermal_model_state_t from the current params'
// T0 (keep_params == true) or reloads the last-selected preset first
// (keep_params == false, matching PLAN.md 6.1's sim_reset tool doc: "model
// to T0"). Resets sim_time_us to 0, clears all MANUAL zone-temp overrides,
// and resets the event ring's next sequence number to 0 (PLAN.md 6.1: "...
// event seq reset"). Does NOT clear fault_sched's armed/active slots --
// that pool is fault_sched's own state, out of this function's scope.
bool sim_engine_reset(bool keep_params);

// MANUAL-mode override (PLAN.md 5's TC/MODEL command groups' "SET_TEMP
// (force a zone temp)"): pins zone `zone`'s reported T_true_c/T_tc_reported_c
// to temp_c every tick until sim_engine_clear_zone_manual() is called or a
// preset/reset clears it. The underlying thermal model keeps ticking
// underneath (so current_a/duty bookkeeping is unaffected), but the
// zone's *reported* temperature never drifts from temp_c while MANUAL is
// active -- PLAN.md 5's "MANUAL (frozen at an operator-set value)".
bool sim_engine_force_zone_temp(uint8_t zone, float temp_c);

// Returns zone `zone` to MODEL mode (the model's own computed temperature
// reports again, starting from whatever state.T_zone[zone] currently holds
// -- no jump is synthesized, so the next tick's reported value picks up
// smoothly from the last MANUAL value).
bool sim_engine_clear_zone_manual(uint8_t zone);

// Returns the seed last accepted by sim_engine_set_seed() (0 if never set).
// Word-sized read of a variable only ever written from sim_engine's own task
// context (apply_pending_commands()) -- RP2040 SMP cores are in-order with
// no data cache, so a plain volatile read of one aligned 32-bit word from
// another task needs no mutex, same reasoning sim_engine.c's own file header
// already applies to the published snapshot's seq counter. telemetry.c
// (read-only per this pass's instructions) documents this exact gap and
// names this function, so its next edit can drop the "seed: sim_engine.h
// has no getter" comment and wire this in directly.
uint32_t sim_engine_get_seed(void);

// --- Safety-side TC: blend of zone temps + its own lag + its own fault
// knobs (PLAN.md 4.3: "The safety-side TC reads a configurable blend of zone
// temps (default: zone 0), with its own lag and its own fault knobs, so
// main-vs-safety disagreement scenarios are first-class") -----------------
// There is exactly ONE physical safety-side MAX31856 channel
// (tc_fault_state.h's TC_FAULT_CHANNEL_SAFETY) reading a caller-configured
// blend across zones -- NOT one independent safety reading per zone. The
// single computed value is republished into every zone's
// sim_snapshot_t.zones[i].T_safety_reported_c slot (sim_snapshot.h's LOCKED
// field shape is unchanged; this just fills it in correctly instead of the
// previous "zone i's own T_tc_reported_c stands in" placeholder) so GUI/
// telemetry consumers that already index by zone (PLAN.md 6.3's "truth vs
// reported vs safety-reported per zone" strip chart) see the one safety
// number lined up against every zone's own truth line for comparison, with
// no special-casing needed on the reader side.
typedef struct {
    // Blend weight per zone (need not sum to 1 -- the caller's
    // responsibility, mirroring thermal_zone_params_t.k_couple's "this
    // module does not enforce it" doctrine). Default (sim_engine_start()):
    // weight[0] = 1, rest 0 -- PLAN.md 4.3's "default: zone 0".
    float weight[THERMAL_MODEL_MAX_ZONES];
    // First-order lag time constant, seconds, applied to the blended target
    // exactly like thermal_model.c's own TC lag formula
    // (dT/dt = (target - current) / lag_s). <= 0 means "no lag" (tracks the
    // blended target instantaneously), same convention as
    // thermal_zone_params_t.tc_lag_s.
    float lag_s;
} sim_engine_safety_tc_params_t;

// Overwrites the safety-TC blend/lag config at the next tick boundary
// (queued, same contract as every other MODEL-group setter above). Returns
// false if params is NULL or the command could not be queued.
bool sim_engine_set_safety_tc_params(const sim_engine_safety_tc_params_t *params);

// Copies the current safety-TC blend/lag config into *out. Returns false if
// out is NULL.
bool sim_engine_get_safety_tc_params(sim_engine_safety_tc_params_t *out);

// MANUAL-mode override for the safety channel ONLY (PLAN.md 5's TC/MODEL
// groups' "TC_SET_MODE"/"SET_TEMP" extended to channel SAFETY, which
// cmd_task.c's handle_tc_force_temp()/handle_tc_set_mode() currently reject
// for channel==TC_FAULT_CHANNEL_SAFETY with ERR_BAD_ARGS pending this API --
// see this pass's report for the remaining cmd_task.c wiring). Independent
// of sim_engine_force_zone_temp()/_clear_zone_manual(): forcing a zone's
// main-side reported temp does NOT touch the safety channel and vice versa,
// which is the whole point -- main/safety disagreement must be expressible
// with either side pinned alone. Applied at the next tick boundary, after
// the blend+lag+fault-override computation, so a MANUAL force always wins.
bool sim_engine_force_safety_temp(float temp_c);

// Returns the safety channel to blend/lag-driven reporting (its lag state
// keeps evolving toward the blend target from wherever it was, no jump
// synthesized -- same "picks up smoothly" contract as
// sim_engine_clear_zone_manual()).
bool sim_engine_clear_safety_manual(void);

// --- fault_sched's safety-TC fault hook (PLAN.md 7.1's "Main/safety
// disagree: Skew safety TC vs zone truth by an offset/gain") --------------
// NOT queued, same invariant as sim_engine_set_zone_duty_override()/
// _health_override() below: fault_sched_tick() is sim_engine.c's own
// synchronous subroutine call (see those functions' doc comment for the
// full race-free reasoning), so a direct write here is race-free by
// construction and takes effect the same tick. While active, the published
// safety-reported value is `blend_and_lag_result * gain + offset_c` (before
// any MANUAL override, which still wins if active) instead of the bare
// blend+lag result. Do not call from any other task.
bool sim_engine_set_safety_tc_fault_override(bool active, float offset_c, float gain);

// --- fault_sched's power-path fault hook (PLAN.md 7.1's "Power path
// faults" table) -------------------------------------------------------------
// Unlike every setter above, these two are NOT queued: their only intended
// caller is fault_sched_tick() (fault_sched.h), which sim_engine.c itself
// calls synchronously once per tick, from sim_engine's own task context --
// so a direct write here is already race-free by construction (same
// thread of execution, no cross-task hazard) and takes effect immediately,
// within the very tick fault_sched just evaluated, rather than lagging one
// tick behind through the command queue. Do not call these from any other
// task; use the MODEL-group setters above for anything not driven by
// fault_sched.
//
// sim_engine_set_zone_duty_override(): while active, zone `zone`'s heater
// duty this tick is `duty` (0..1) regardless of what the relay-derived
// mapping says -- covers welded/shorted relay (duty forced 1 while the
// relay reads open), stuck-open relay (duty forced 0 while the relay reads
// closed), and runaway-zone (duty forced 1) faults, all from PLAN.md 7.1's
// power-path and system-fault tables.
bool sim_engine_set_zone_duty_override(uint8_t zone, bool active, float duty);

// sim_engine_set_zone_health_override(): while active, zone `zone`'s
// element_health this tick is `health` (0..1) regardless of the stored
// parameter value -- covers broken-heater-coil (health forced 0) and
// partial-failed-coil (health forced to the fault's configured fraction)
// faults. The stored parameter itself is never mutated, so clearing the
// override (active == false) restores exactly what sim_engine_set_zone_params()
// last set, with no baseline-tracking needed on fault_sched's side.
bool sim_engine_set_zone_health_override(uint8_t zone, bool active, float health);

// sim_engine_set_zone_thermal_override(): PLAN.md 7.1 system table,
// "Thermal-mass surprise: Step-change model params mid-run (lid opened,
// load added)". While active, zone `zone`'s effective C (thermal mass,
// J/degC) and k_loss (loss to ambient, W/degC) this tick are `C`/`k_loss`
// regardless of the stored thermal_zone_params_t values -- same
// "stored parameter never mutated, clearing restores exactly what
// sim_engine_set_zone_params() last set" contract as the health override
// above. Only these two fields are covered (the pair a real "lid opened,
// load added" event actually changes -- more thermal mass, more loss to
// ambient); k_couple/R_element/tc_lag_s are untouched by this override.
// Defensive guard (this function's caller, not thermal_model_tick(), which
// this pass must not edit): `C` <= 0 is ignored (the stored C stays in
// effect for that tick) rather than handed to thermal_model_tick(), whose
// dT/dt = .../C would divide by zero -- `k_loss` has no such restriction
// (0 is a legitimate "lost its loss path" value) and is applied verbatim.
bool sim_engine_set_zone_thermal_override(uint8_t zone, bool active, float C, float k_loss);

// sim_engine_set_zone_tc_lag_override(): PLAN.md 7.1 system table,
// "Sensor-vs-element lag stress: Crank TC lag to provoke overshoot". While
// active, zone `zone`'s effective tc_lag_s this tick is `tc_lag_s`
// regardless of the stored parameter value -- same override contract as
// above. No clamp needed: thermal_model_tick() already treats tc_lag_s <= 0
// as "no lag" rather than dividing by zero (thermal_model.c's own documented
// behavior), so any value is safe to pass through verbatim.
bool sim_engine_set_zone_tc_lag_override(uint8_t zone, bool active, float tc_lag_s);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_SIM_ENGINE_H
