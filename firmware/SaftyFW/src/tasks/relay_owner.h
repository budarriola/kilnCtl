// relay_owner.h -- owns GPIO6, the safety actuator. docs/ARCHITECTURE.md
// section 3: "The only code in the build that writes GPIO6" (main() itself
// excepted -- it drives GPIO6 low as the literal first statement of boot,
// per section 5, before this task or the scheduler exist).
//
// Phase 5 ("Relay authority"): the INIT -> GRACE -> ARMED -> TRIPPED state
// machine and latching trip semantics live here.
//
//   INIT     Between main()'s GPIO6-low (boot step 1) and this task's first
//            statement. Not a state relay_owner_task ever observably sits
//            in -- see relay_owner.c's header comment for why GRACE begins
//            at the top of the task function rather than needing a separate
//            "enter grace" call from main.c.
//   GRACE    docs/ARCHITECTURE.md section 5 step 8: guards evaluate and
//            report, but K4 stays de-energized no matter what
//            relay_owner_command_energize() is asked for. Lasts
//            SAFTYFW_STARTUP_GRACE_MS (60s default, SAFETY_MODEL.md section
//            2's "startup is not steady state").
//   ARMED    Step 9: a genuine energize command is honoured.
//   TRIPPED  Latched by relay_owner_command_trip(): de-energized, and every
//            subsequent relay_owner_command_energize() is refused until
//            relay_owner_clear_trip() is called.
#ifndef SAFTYFW_TASKS_RELAY_OWNER_H
#define SAFTYFW_TASKS_RELAY_OWNER_H

#include <stdbool.h>

#include "safety_guards.h" // safety_trip_t -- the trip-reason enum relay_owner_command_trip() latches

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RELAY_OWNER_STATE_INIT = 0,
    RELAY_OWNER_STATE_GRACE,
    RELAY_OWNER_STATE_ARMED,
    RELAY_OWNER_STATE_TRIPPED,
} relay_owner_state_t;

// Creates the relay_owner task at SAFTYFW_PRIO_RELAY_OWNER, pinned to
// SAFTYFW_CORE_TRIP_PATH. Must be called after gpio6 has already been driven
// low by main() (boot sequence step 1) -- this task takes over ownership from
// that point, it does not perform the initial safe-state drive itself.
// Returns false if task creation failed.
//
// The GRACE timer starts the moment relay_owner_task itself begins running
// (see relay_owner.c), which in practice is essentially "right after this
// call returns and the scheduler starts" -- relay_owner is both the
// highest-priority task in the system and the first one main.c starts.
bool relay_owner_start(void);

// Non-blocking command enqueue -- "must always be able to drop"
// (ARCHITECTURE.md section 4). Never blocks the caller (safety_core), which
// is why this posts to a queue rather than driving the GPIO directly from the
// caller's context: relay_owner is still the only writer, just asynchronously.
//
// The ordinary (non-trip) command path for a legitimate ARMED-state
// request. Returns false if the command queue was full and the command was
// dropped, OR if relay_owner is currently TRIPPED -- a trip refuses every
// energize command, by design, until relay_owner_clear_trip() runs (checked
// both here, so a caller gets an honest false immediately, and again inside
// relay_owner_task itself as a defensive re-check against the race between
// the two). While GRACE, the command is accepted and tracked (a future
// telemetry read can see "something asked to energize"), but GPIO6 is never
// actually driven high.
bool relay_owner_command_energize(bool energize);

// The trip path safety_core uses instead of a bare
// relay_owner_command_energize(false): de-energizes AND enters the latched
// TRIPPED state in one step, so no caller can observe "de-energized but not
// yet latched" or vice versa. Matches SAFETY_MODEL.md section 6's 4-step
// trip order's step 1 (relay_owner de-energizes first, before anything
// else, before logging) -- relay_owner is priority 7, the highest in the
// system, so posting this command preempts the (lower-priority) caller
// before safety_core.c's next line (latching the reason via
// boot_reason_latch_trip(), step 2) runs. Returns false only if the command
// queue was full and the command was dropped -- a dropped trip command is
// not silently ignored by the caller in that case (see safety_core.c), but
// this function itself does not retry.
bool relay_owner_command_trip(safety_trip_t reason);

// Clears a latched trip, unconditionally, IF called: TRIPPED -> ARMED.
//
// Deliberately does NOT check whether the tripping condition has actually
// gone away -- SAFETY_MODEL.md section 6's "a clear is refused while the
// tripping condition is still true" is real, but enforcing it needs a
// caller that can re-evaluate the guard (safety_core.c re-running
// safety_guards_tick() against current input before deciding to call this),
// and no such caller exists yet: nothing in this build calls
// relay_owner_clear_trip() at all. That enforcement is Phase 7's
// link_task/GUI clear-command job (CLEAR_TRIP, 0x0A) once it exists. Do not
// treat this function's current unconditional behaviour as the finished
// clear path -- it is the API shape, not the policy, and the policy is not
// here.
//
// Returns false if the command queue was full and the command was dropped.
bool relay_owner_clear_trip(void);

// Current state, for safety_core/future telemetry to read without a new
// coupling to relay_owner's internals. Safe to call from any task -- backed
// by a single volatile read of a value only relay_owner_task writes, same
// pattern discrete_task.h uses for its debounced reads.
relay_owner_state_t relay_owner_get_state(void);

// True only while GPIO6 is actually being driven high right now -- distinct
// from relay_owner_state_t, which answers "would an energize command be
// honoured" (ARMED), not "has one landed and is it still in effect". Set the
// instant gpio_put(SAFTYFW_PIN_RELAY, 1) actually executes and cleared on
// every path that drives it low (GRACE refusal, TRIPPED refusal, an explicit
// energize(false), or a trip). This is what lets safety_core answer "is K4
// energized" for real instead of a caller inferring it from the state enum,
// which link_task.c's own header comment used to note this file could not
// yet answer precisely. Safe to call from any task, same volatile-read
// pattern as relay_owner_get_state().
bool relay_owner_is_energized(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_RELAY_OWNER_H
