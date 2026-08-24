// clock_health.c -- see clock_health.h.
#include "clock_health.h"

bool clock_health_observe(clock_health_state_t *state, uint32_t now_ms, float dt_s)
{
    if (!state) {
        // No state to track against -- cannot possibly declare a stall
        // without history, same "nothing to compare against yet" default as
        // the have_last == false case below. Never crashes a caller that
        // forgot to pass one; just never reports a fault either.
        return false;
    }

    if (!state->have_last) {
        state->last_now_ms = now_ms;
        state->have_last = true;
        state->consecutive_stalled = 0;
        return false;
    }

    if (dt_s > 0.0f) {
        uint32_t advanced_ms = now_ms - state->last_now_ms; // wraparound-safe, see header
        uint32_t expected_ms = (uint32_t)(dt_s * 1000.0f);
        uint32_t half_expected_ms = expected_ms / 2u;

        if (advanced_ms < half_expected_ms) {
            state->consecutive_stalled++;
        } else {
            state->consecutive_stalled = 0;
        }
    }

    state->last_now_ms = now_ms;

    return state->consecutive_stalled >= CLOCK_HEALTH_STALL_DEBOUNCE;
}
