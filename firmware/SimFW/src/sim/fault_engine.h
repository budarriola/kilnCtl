// fault_engine -- pure, host-testable fault-scheduling engine for SimFW.
// docs/DESIGN_NOTES.md sections 7.1-7.3 are authoritative for the fault catalog, the
// standardized trigger/duration/repeat model, and the engine internals this
// implements. This module knows nothing about *what* a fault does to a
// target (that is TC/CT/relay/model-override territory in the real
// firmware's owner tasks) -- it only tracks *when* fault slots fire and
// clear, deterministically, and emits FIRED/CLEARED events for the caller
// to act on.
//
// Determinism is the core contract (DESIGN_NOTES.md 7.2: "the same scenario + seed
// => the same run, byte-for-byte in the event log. That replayability rule
// is the fixture's core testing contract."):
//   - Trigger evaluation is slot-index order every tick (DESIGN_NOTES.md 7.3).
//   - All randomness (RANDOM_IN trigger picks, EVERY/N_TIMES jitter) comes
//     from a single seeded xorshift32 stream per engine instance -- same
//     algorithm as firmware/CommonFW/test/test_fuzz.c's fuzzer and
//     max31856_regs.c's noise/bit-error injection, so this repo has exactly
//     one PRNG idiom for "deterministic randomness in a host-testable pure
//     module."
//   - Nothing here reads a wall clock, a hardware RNG, or any other hidden
//     state; every tick is a pure function of (engine state, snapshot).
#ifndef SIMFW_SIM_FAULT_ENGINE_H
#define SIMFW_SIM_FAULT_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAULT_ENGINE_MAX_SLOTS  32u   /* DESIGN_NOTES.md 7.3: "fixed pool of fault slots (e.g. 32)" */
#define FAULT_ENGINE_MAX_ZONES  4u    /* matches thermal_model.h's THERMAL_MODEL_MAX_ZONES */
#define FAULT_ENGINE_MAX_RELAYS 8u    /* board has 5 (K1/K2/K3/K5/K4) + headroom */
#define FAULT_ENGINE_MAX_NAME_LEN 24u
#define FAULT_ENGINE_INVALID_SLOT 0xFFFFu

typedef enum {
    FAULT_STATE_IDLE = 0,   /* slot never scheduled, or fully cancelled */
    FAULT_STATE_ARMED,      /* scheduled, waiting for its trigger */
    FAULT_STATE_ACTIVE,     /* fired, currently in effect */
    FAULT_STATE_EXPIRED,    /* fired and finished; terminal (ONCE) or
                              * exhausted (N_TIMES) -- never re-arms */
} fault_slot_state_t;

/* DESIGN_NOTES.md 7.2's trigger kinds. */
typedef enum {
    FAULT_TRIGGER_AT_SIM_TIME = 0,
    FAULT_TRIGGER_AT_ZONE_TEMP,
    FAULT_TRIGGER_ON_RELAY_EDGE,
    FAULT_TRIGGER_ON_EVENT,
    FAULT_TRIGGER_AFTER_FAULT,
    FAULT_TRIGGER_RANDOM_IN,
    FAULT_TRIGGER_MANUAL,
} fault_trigger_kind_t;

typedef enum { FAULT_TEMP_EDGE_RISING = 0, FAULT_TEMP_EDGE_FALLING } fault_temp_edge_t;
typedef enum { FAULT_RELAY_EDGE_CLOSE = 0, FAULT_RELAY_EDGE_OPEN } fault_relay_edge_t;

typedef struct {
    fault_trigger_kind_t kind;

    double at_sim_time_s;                        /* AT_SIM_TIME */

    uint8_t zone;                                  /* AT_ZONE_TEMP */
    float temp_c;                                   /* AT_ZONE_TEMP */
    fault_temp_edge_t temp_edge;                    /* AT_ZONE_TEMP */

    uint8_t relay;                                   /* ON_RELAY_EDGE */
    fault_relay_edge_t relay_edge;                    /* ON_RELAY_EDGE */

    double delay_s;                                    /* ON_RELAY_EDGE / ON_EVENT / AFTER_FAULT */

    char event_name[FAULT_ENGINE_MAX_NAME_LEN];          /* ON_EVENT */

    uint16_t after_fault_slot;                            /* AFTER_FAULT */

    double random_t0_s;                                    /* RANDOM_IN */
    double random_t1_s;                                     /* RANDOM_IN */
} fault_trigger_t;

typedef enum { FAULT_DURATION_PERMANENT = 0, FAULT_DURATION_FOR, FAULT_DURATION_UNTIL_TRIGGER } fault_duration_kind_t;

typedef struct {
    fault_duration_kind_t kind;
    double for_s;                    /* FOR */
    fault_trigger_t until_trigger;    /* UNTIL_TRIGGER -- evaluated the same way as a
                                        * top-level trigger, see fault_engine.c */
} fault_duration_t;

typedef enum { FAULT_REPEAT_ONCE = 0, FAULT_REPEAT_EVERY, FAULT_REPEAT_N_TIMES } fault_repeat_kind_t;

typedef struct {
    fault_repeat_kind_t kind;
    double period_s;   /* EVERY / N_TIMES */
    double jitter_s;    /* EVERY / N_TIMES: uniform +-jitter_s added to period_s each re-arm */
    uint16_t n;          /* N_TIMES: total fire count cap */
} fault_repeat_t;

typedef struct {
    uint16_t slot_id;
    fault_slot_state_t state;
    uint16_t fault_type; /* opaque to the engine -- caller-defined */
    uint16_t target;      /* opaque to the engine -- caller-defined */

    fault_trigger_t trigger;
    fault_duration_t duration;
    fault_repeat_t repeat;
    float params[4];

    uint32_t fire_count;
    double active_since_s;

    /* --- internal bookkeeping, do not set directly --- */
    bool has_scheduled_fire;           /* ON_RELAY_EDGE/ON_EVENT/AFTER_FAULT: base
                                         * condition detected, waiting out delay_s */
    double scheduled_fire_at_s;
    bool random_picked;                 /* RANDOM_IN: pick made for this arm cycle */
    double random_pick_s;
    uint32_t after_fault_last_seen_count; /* AFTER_FAULT: last ref-slot fire_count consumed */
    double repeat_ready_at_s;            /* EVERY/N_TIMES: next scheduled re-fire time */
    bool manual_fire_pending;            /* MANUAL/FIRE_NOW: fault_engine_fire_now() requested
                                           * an immediate fire; consumed by the very next
                                           * fault_engine_tick() call, which fires it through
                                           * the exact same ARMED->ACTIVE code path (and thus
                                           * the same FIRED-event emission) as a triggered
                                           * fire -- see fault_engine_fire_now()'s doc for why
                                           * this is deferred rather than applied in-place. */
    bool cancel_pending;                 /* fault_engine_cancel() called while ACTIVE: consumed
                                           * by the next fault_engine_tick() call, which emits
                                           * the CLEARED event through the same code path as a
                                           * natural expiry, then resets to IDLE (not EXPIRED --
                                           * an operator abort, not exhaustion) instead of
                                           * evaluating duration/repeat. See
                                           * fault_engine_cancel()'s doc for why. */
} fault_slot_t;

/* One tick's inputs -- everything trigger evaluation can see. Only the
 * first zone_count/relay_count entries of the arrays are read. event_names
 * lists named events observed *this tick* (e.g. the caller's own edge
 * detection surfacing something as a named event for ON_EVENT to match). */
typedef struct {
    double sim_time_s;
    const float *zone_temps;
    uint8_t zone_count;
    const bool *relay_states; /* true = closed */
    uint8_t relay_count;
    const char *const *event_names;
    uint32_t event_count;
} fault_engine_snapshot_t;

typedef enum { FAULT_EVENT_FIRED = 0, FAULT_EVENT_CLEARED } fault_event_kind_t;

typedef struct {
    fault_event_kind_t kind;
    uint16_t slot_id;
    double sim_time_s;
    uint32_t fire_count; /* which repetition (1-based) this FIRED/CLEARED belongs to */
} fault_event_t;

typedef struct {
    fault_slot_t slots[FAULT_ENGINE_MAX_SLOTS];
    uint32_t rng_state; /* xorshift32 */

    float prev_zone_temps[FAULT_ENGINE_MAX_ZONES];
    bool prev_relay_states[FAULT_ENGINE_MAX_RELAYS];
    bool has_prev_snapshot;
} fault_engine_t;

/* Seeds the engine: all slots to IDLE, PRNG to `seed` (0 remapped to a fixed
 * nonzero value, same convention as max31856_regs_init). */
void fault_engine_init(fault_engine_t *eng, uint32_t seed);

/* Arms slot_id (0..FAULT_ENGINE_MAX_SLOTS-1) with the given definition,
 * overwriting whatever was there. Returns slot_id on success,
 * FAULT_ENGINE_INVALID_SLOT if slot_id is out of range. */
uint16_t fault_engine_schedule(fault_engine_t *eng,
                                uint16_t slot_id,
                                uint16_t fault_type,
                                uint16_t target,
                                const fault_trigger_t *trigger,
                                const fault_duration_t *duration,
                                const fault_repeat_t *repeat,
                                const float params[4]);

/* Cancels slot_id. If the slot is ARMED, IDLE, or EXPIRED, it is reset to
 * IDLE immediately (discarding any bookkeeping) -- nothing was ever ACTIVE,
 * so there is nothing to report a CLEARED for. If the slot is ACTIVE, the
 * reset is deferred: this call only sets cancel_pending, and the very next
 * fault_engine_tick() call emits the CLEARED event (through the same
 * ACTIVE-clearing code path a natural expiry uses) before resetting the
 * slot to IDLE (not EXPIRED -- an operator abort is not exhaustion). This
 * mirrors fault_engine_fire_now()'s "defer to the next tick so there is one
 * event-emitting code path, not two" design -- see that function's doc.
 * Returns false if slot_id is out of range; true otherwise (including for
 * an ACTIVE slot, where the deferred CLEARED is still pending). */
bool fault_engine_cancel(fault_engine_t *eng, uint16_t slot_id);

/* Requests an immediate fire of slot_id regardless of its trigger (the
 * FAULT_FIRE_NOW / MANUAL-trigger path, DESIGN_NOTES.md section 5/7.2). Valid only
 * from ARMED; returns false otherwise (including an out-of-range slot_id).
 * On success, marks the slot's manual_fire_pending flag and returns true --
 * it does NOT transition the slot to ACTIVE or emit a FIRED event itself.
 * The very next fault_engine_tick() call fires the slot through its normal
 * ARMED-slot evaluation (manual_fire_pending short-circuits trigger
 * evaluation to "fire now"), which is the same code path -- and therefore
 * the same FIRED-event emission -- every triggered fire already goes
 * through. This is deliberate: an earlier version of this function fired
 * the slot in place and handed the caller a FIRED event directly, but
 * fault_engine_tick() is the only call that runs in sim_engine's own tick
 * (sim_snapshot.h's single-ring-producer contract requires all FIRED/
 * CLEARED events to flow from there) -- an out-of-band caller (e.g.
 * cmd_task.c handling FAULT_FIRE_NOW/TC_INJECT_FAULT on its own task) had
 * no way to get that directly-returned event into the ring without either a
 * second producer or a cross-task write into sim_engine's state, both of
 * which sim_engine.c's tick-order comment rules out. Deferring the actual
 * state transition to the next tick, and having that tick's ordinary
 * ARMED-slot handling notice the pending request, means fire-now events
 * reach the ring exactly the way triggered ones do -- one path, not two --
 * at the small cost of the fire landing on the next tick boundary (<=1
 * fault_sched period) rather than mid-tick. */
bool fault_engine_fire_now(fault_engine_t *eng, uint16_t slot_id);

/* Evaluates every ARMED/ACTIVE slot in slot-index order (0..31,
 * deterministic per DESIGN_NOTES.md 7.3), firing/expiring slots and writing
 * FIRED/CLEARED events into out[] in the order they occur. Returns the
 * number of events written, capped at max_events (size out generously --
 * FAULT_ENGINE_MAX_SLOTS*2 covers the worst single-tick case of every slot
 * firing and every other slot clearing in the same tick). */
size_t fault_engine_tick(fault_engine_t *eng, const fault_engine_snapshot_t *snapshot,
                          fault_event_t *out, size_t max_events);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_FAULT_ENGINE_H
