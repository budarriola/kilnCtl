// max31856_reconfig_retry.c -- see max31856_reconfig_retry.h for the full
// rationale.
#include "max31856_reconfig_retry.h"

void max31856_reconfig_retry_init(max31856_reconfig_retry_state_t *state)
{
    if (!state) {
        return;
    }
    state->retry_count = 0;
    // First attempt is allowed to fire as soon as the interval has elapsed
    // from tick 0 -- not immediately, so a boot-time transient the initial
    // main.c probe already tried has a moment to settle before the first
    // retry, matching the "every 500 ms-1 s" cadence rather than hammering
    // the bus back-to-back with the boot-time attempt.
    state->next_attempt_due_ms = MAX31856_RECONFIG_RETRY_INTERVAL_MS;
    state->gave_up = false;
}

bool max31856_reconfig_retry_should_attempt(max31856_reconfig_retry_state_t *state, bool verified,
                                             uint32_t now_ms)
{
    if (!state || verified || state->gave_up) {
        return false;
    }
    // Unsigned subtraction: correct across a tick-count wraparound the same
    // way FreeRTOS's own TickType_t comparisons are, since now_ms only ever
    // increases (mod 2^32) between calls.
    return (now_ms - state->next_attempt_due_ms) < 0x80000000u;
}

void max31856_reconfig_retry_note_result(max31856_reconfig_retry_state_t *state, bool verified,
                                          uint32_t now_ms)
{
    if (!state) {
        return;
    }
    if (verified) {
        state->retry_count = 0;
        state->gave_up = false;
        return;
    }
    state->retry_count++;
    state->next_attempt_due_ms = now_ms + MAX31856_RECONFIG_RETRY_INTERVAL_MS;
    if (state->retry_count >= MAX31856_RECONFIG_RETRY_MAX_ATTEMPTS) {
        state->gave_up = true;
    }
}
