// relay_grace.c -- see relay_grace.h. Pure state-transition decisions, no
// RTOS/SDK dependency (host-testable).
#include "relay_grace.h"

relay_owner_state_t relay_grace_tick(relay_owner_state_t state, uint32_t elapsed_ticks,
                                      uint32_t grace_ticks)
{
    if (state != RELAY_OWNER_STATE_GRACE) {
        return state;
    }
    if (elapsed_ticks >= grace_ticks) {
        return RELAY_OWNER_STATE_ARMED;
    }
    return state;
}

relay_owner_state_t relay_trip_transition(relay_owner_state_t state)
{
    (void)state; // unconditional, by design -- see relay_grace.h's doc comment
    return RELAY_OWNER_STATE_TRIPPED;
}

relay_owner_state_t relay_clear_trip_transition(relay_owner_state_t state, uint32_t elapsed_ticks,
                                                  uint32_t grace_ticks)
{
    if (state != RELAY_OWNER_STATE_TRIPPED) {
        return state;
    }
    // Boundary matches relay_grace_tick()'s own inclusive ">=": elapsed
    // EQUAL to grace_ticks already counts as "grace is over", same instant
    // a normal (non-trip) boot would have crossed into ARMED.
    return (elapsed_ticks >= grace_ticks) ? RELAY_OWNER_STATE_ARMED : RELAY_OWNER_STATE_GRACE;
}

bool relay_energize_allowed_during_update(bool update_in_progress)
{
    return !update_in_progress;
}

bool relay_trip_command_still_owed(bool is_tripped, bool send_succeeded)
{
    if (!is_tripped) {
        return false; // cleared out from under the retry -- see relay_grace.h's doc comment
    }
    return !send_succeeded;
}

bool relay_clear_command_still_owed(bool is_tripped, bool send_succeeded)
{
    if (is_tripped) {
        return false; // superseded by a (re-)trip -- SAFE direction wins, see relay_grace.h's doc comment
    }
    return !send_succeeded;
}
