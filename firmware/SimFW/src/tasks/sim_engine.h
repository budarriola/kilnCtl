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
// carries for its own provisional pin choices.
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

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_SIM_ENGINE_H
