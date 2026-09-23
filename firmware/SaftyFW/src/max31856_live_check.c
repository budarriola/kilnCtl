// max31856_live_check.c -- see max31856_live_check.h for the full rationale.
#include "max31856_live_check.h"

void max31856_live_check_init(max31856_live_check_state_t *state)
{
    if (!state) {
        return;
    }
    state->polls_since_check = 0;
    state->mismatch_count = 0;
    state->mismatch_active = false;
}

bool max31856_live_check_tick(max31856_live_check_state_t *state, bool verified)
{
    if (!state) {
        return false;
    }
    if (!verified) {
        // Nothing commissioned to compare against right now -- hold the
        // cadence at 0 so the next verified tick starts a fresh interval
        // rather than firing on stale progress. See header comment.
        state->polls_since_check = 0;
        return false;
    }
    state->polls_since_check++;
    if (state->polls_since_check >= MAX31856_LIVE_CHECK_INTERVAL_POLLS) {
        state->polls_since_check = 0;
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
