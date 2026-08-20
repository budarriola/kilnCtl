// fault_sched.h -- REAL BODY (milestone M-F, docs/PLAN.md section 4.1):
// "Trigger evaluation each tick; arms/fires scheduled faults." Owns no
// peripheral directly (single-owner-per-peripheral doctrine, PLAN.md
// section 4's opening paragraph) -- firing a fault means flipping the
// target's override in the owning module (a TC corruption knob via
// src/sim/tc_fault_state.h for spi_emu_a/b to read, a power-path override
// in sim_engine via sim_engine.h's fault-only setters, or an existing
// public setter on i2c_owner for system faults), per section 7.3's "engine
// internals". This task never pokes hardware itself.
//
// --- Why fault_sched_tick() is called BY sim_engine, not by an
// independently-scheduled task loop -----------------------------------------
// PLAN.md 4.1 lists fault_sched as its own 10 Hz task, and 4.1's closing
// note says "fault_sched" evaluates "alongside sim_engine" -- section 4.1
// also explicitly leaves the exact mechanism open ("driven by sim_engine
// directly if that's architecturally cleaner -- your call, document it" per
// this pass's own instructions). Two independently FreeRTOS-scheduled 10 Hz
// tasks have no guaranteed relative tick order or alignment -- wall-clock
// jitter between them is not itself a determinism problem (PLAN.md 7.2's
// contract is about the *sim clock*, not wall time), but it DOES create a
// real hazard here: fault_sched's overrides need to land inside the exact
// sim_engine tick that reads them (duty overrides feeding
// thermal_model_tick(), health overrides likewise), and two independent task
// loops give no way to guarantee "fault_sched's Nth evaluation lands before
// sim_engine's Nth physics step" without extra synchronization machinery
// (a rendezvous queue/semaphore pair) that would itself become a second,
// parallel source of tick-ordering bugs to get right. Calling
// fault_sched_tick() synchronously from inside sim_engine's own tick
// (sim_engine.c) instead makes the ordering trivially, structurally
// correct: evaluate faults, apply their overrides, THEN run this tick's
// physics with those overrides already in effect -- one thread of
// execution, no race, no missed-tick window, ever.
//
// The fault_sched TASK (fault_sched.c's fault_sched_task_fn) still exists
// and is still created/scheduled (main.c's task map is unchanged) -- it is
// simply not what drives trigger evaluation. It is reserved for future
// event-driven work that genuinely needs its own task context (e.g. a
// cmd_task-facing request queue, mirroring i2c_owner.c's pattern, if a
// later pass finds fault_sched_schedule()/_cancel() need to be non-blocking
// from a caller's perspective under contention) and currently just idles,
// same as every other stub task before its milestone -- the schedule/
// cancel/fire_now/list API below is already safe to call from any task
// today via its own internal mutex, so no queue is needed yet.
#ifndef SIMFW_TASKS_FAULT_SCHED_H
#define SIMFW_TASKS_FAULT_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim/fault_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates fault_sched at SIMFW_PRIO_FAULT_SCHED, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Also initializes the
// internal fault_engine_t (fault_engine_init(), seed 0 -> engine's own
// fixed-nonzero remap) and its guarding mutex. Returns false if
// task/mutex creation failed.
bool fault_sched_start(void);

// PLAN.md 7.1's fault catalog, restricted to the subset this pass's target
// modules can actually express (see fault_sched.c's header comment for the
// specific gaps and why): each value is fault_engine_schedule()'s opaque
// `fault_type`, and its target_kind (below) says how `target` is
// interpreted for that type.
typedef enum {
    /* TC / sensor faults -- target is a tc_fault_state.h tc_fault_channel_t.
     * params[0] meaning is per-type, documented per value. */
    FAULT_SCHED_TYPE_TC_DISCONNECTED = 0,     /* OPEN SR bit forced (max31856_regs.c's
                                                * "higher layer" hook, see tc_fault_state.h) */
    FAULT_SCHED_TYPE_TC_NOISE,                /* params[0] = noise sigma, degC */
    FAULT_SCHED_TYPE_TC_STUCK,                /* LTCB frozen at last value */
    FAULT_SCHED_TYPE_TC_DEAD_IC,              /* params[0] = max31856_dead_mode_t as float */
    FAULT_SCHED_TYPE_TC_FLAKY_SPI,            /* params[0] = bit error rate, 0..1 */
    FAULT_SCHED_TYPE_TC_SPURIOUS_FAULT_PIN,   /* ~FAULT asserted regardless of SR */

    /* Power-path faults -- target is a zone index (thermal_model.h's
     * zone numbering, matches tc_fault_state's MAIN_0..2 ordinally per
     * sim_engine.h's documented zone-to-relay/TC mapping assumption). */
    FAULT_SCHED_TYPE_WELDED_RELAY,     /* duty forced 1 regardless of relay sense */
    FAULT_SCHED_TYPE_STUCK_OPEN_RELAY, /* duty forced 0 regardless of relay sense */
    FAULT_SCHED_TYPE_BROKEN_ELEMENT,   /* element_health forced 0 */
    FAULT_SCHED_TYPE_PARTIAL_ELEMENT,  /* params[0] = forced element_health, 0..1 */

    /* System faults -- target is a fault_sched_system_target_t (below). */
    FAULT_SCHED_TYPE_ESTOP,            /* opens the E-stop loop via i2c_owner_set_estop() */
    FAULT_SCHED_TYPE_RUNAWAY_ZONE,     /* duty forced 1 on `target` regardless of relay
                                         * (same mechanism as WELDED_RELAY, kept as a
                                         * separate type so a report/UI can label it
                                         * correctly -- target is a zone index here, not
                                         * a fault_sched_system_target_t) */
    FAULT_SCHED_TYPE_AMBIENT_SHIFT,    /* params[0] = new T_ambient, degC. Applied as an
                                         * immediate step on FIRE via sim_engine_set_ambient()
                                         * -- PLAN.md 7.1 describes a ramp; this pass
                                         * implements the step-change simplification only,
                                         * documented in fault_sched.c. Not reverted on
                                         * CLEAR (ambient has no "previous value" concept
                                         * at this layer). target is ignored for this type. */
} fault_sched_fault_type_t;

// FAULT_SCHED_TYPE_ESTOP's `target` value (ignored/reserved for other
// system-fault types, kept as its own enum so a future addition -- e.g. a
// distinct ambient-ramp-rate system fault -- has somewhere to put its
// target without repurposing a zone index).
typedef enum {
    FAULT_SCHED_SYSTEM_TARGET_NONE = 0,
} fault_sched_system_target_t;

// Arms slot_id (0..FAULT_ENGINE_MAX_SLOTS-1) -- thin wrapper over
// fault_engine_schedule() (fault_engine.h) that also validates fault_type/
// target_kind pairing loosely (out-of-range zone/channel indices are
// rejected; PLAN.md 7.2's trigger/duration/repeat structs pass through
// verbatim). Returns slot_id on success, FAULT_ENGINE_INVALID_SLOT
// otherwise.
uint16_t fault_sched_schedule(uint16_t slot_id,
                               fault_sched_fault_type_t fault_type,
                               uint16_t target,
                               const fault_trigger_t *trigger,
                               const fault_duration_t *duration,
                               const fault_repeat_t *repeat,
                               const float params[4]);

// Cancels slot_id (fault_engine_cancel() passthrough). If the slot was
// ACTIVE, its effect is NOT automatically un-applied here -- per
// fault_engine.h's own doc for fault_engine_cancel(), "was this a real
// expiry or an operator abort" is a caller-level distinction; this pass
// resolves it simply by recomputing every target's effective override from
// the active-slot set on the very next fault_sched_tick() (see
// fault_sched.c's header comment: "recompute, don't patch"), so a
// cancelled slot's effect disappears within one tick regardless, with no
// separate un-apply path needed here.
bool fault_sched_cancel(uint16_t slot_id);

// Immediately fires slot_id (fault_engine_fire_now() passthrough) and
// applies its effect synchronously before returning (does not wait for the
// next fault_sched_tick()'s recompute pass -- FAULT_FIRE_NOW is meant to be
// immediate, PLAN.md section 5). Returns false under the same conditions
// fault_engine_fire_now() does (slot not ARMED, or out of range).
bool fault_sched_fire_now(uint16_t slot_id);

// Copies up to max_out slots (fault_engine.h's fault_slot_t, including
// IDLE ones) starting at index 0 into out[]. Returns the number copied
// (always FAULT_ENGINE_MAX_SLOTS today, capped by max_out) -- a thin,
// mutex-guarded snapshot for the future FAULT_LIST command (PLAN.md
// section 5).
size_t fault_sched_list(fault_slot_t *out, size_t max_out);

// --- sim_engine's tick-lockstep entry point ---------------------------------
// Called once per sim_engine tick, from sim_engine.c's own task context,
// after sim_engine has read this tick's relay/estop/temperature inputs but
// BEFORE it computes duty[]/eff_params for thermal_model_tick() -- so any
// override this call applies (via sim_engine_set_zone_duty_override/
// _health_override, tc_fault_state_write, or i2c_owner_set_estop) is in
// effect for the very same tick's physics step. See this header's top
// comment for why this replaces an independently-scheduled fault_sched
// task loop. Not reentrant; sim_engine.c is structurally its only caller.
//
// Evaluates every ARMED/ACTIVE slot against `snapshot` (fault_engine.h's
// own snapshot type -- sim_time_s, zone temps, relay states, named events;
// sim_engine.c builds this from its own state each tick), applies FIRED/
// CLEARED effects to their target modules, and writes the resulting
// fault_event_t sequence into out_events (capped at max_events -- size
// generously, FAULT_ENGINE_MAX_SLOTS*2 covers the documented worst case,
// same as fault_engine_tick()'s own contract). Returns the number of
// events written; sim_engine.c is responsible for translating each into a
// SIM_EVENT_FAULT_FIRED/CLEARED sim_event_t and pushing it into its own
// event ring (fault_sched never touches that ring directly -- sim_engine.c
// is its sole producer, sim_snapshot.h's ownership doctrine).
size_t fault_sched_tick(const fault_engine_snapshot_t *snapshot,
                         fault_event_t *out_events, size_t max_events);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_FAULT_SCHED_H
