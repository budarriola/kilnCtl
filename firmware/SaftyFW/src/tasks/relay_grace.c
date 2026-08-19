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
