// max31856_live_check.c -- see max31856_live_check.h for the full rationale.
#include "max31856_live_check.h"

void max31856_live_check_init(max31856_live_check_state_t *state)
{
    if (!state) {
        return;
    }
    state->next_check_due_ms = 0;
    state->armed = false;
    state->mismatch_count = 0;
    state->mismatch_active = false;
}

bool max31856_live_check_tick(max31856_live_check_state_t *state, bool verified, uint32_t now_ms)
{
    if (!state) {
        return false;
    }
    if (!verified) {
        // Nothing commissioned to compare against right now -- disarm so
        // the next verified tick arms a fresh interval from that moment,
        // rather than firing on stale progress. See header comment.
        state->armed = false;
        return false;
    }
    if (!state->armed) {
        // Just became verified (or this is the very first tick) -- arm a
        // fresh full interval starting now instead of checking immediately
        // on old, stale progress.
        state->armed = true;
        state->next_check_due_ms = now_ms + MAX31856_LIVE_CHECK_INTERVAL_MS;
        return false;
    }
    // Wraparound-safe "now_ms >= next_check_due_ms", same idiom other
    // xTaskGetTickCount()-derived clock comparisons in this codebase use.
    if ((int32_t)(now_ms - state->next_check_due_ms) >= 0) {
        state->next_check_due_ms = now_ms + MAX31856_LIVE_CHECK_INTERVAL_MS;
        return true;
    }
    return false;
}

bool max31856_live_check_note_result(max31856_live_check_state_t *state, bool match)
{
    if (!state) {
        return false;
    }
    if (match) {
        state->mismatch_active = false;
        return false;
    }
    state->mismatch_count++;
    bool is_new_episode = !state->mismatch_active;
    state->mismatch_active = true;
    return is_new_episode;
}
