#include "login_backoff.h"

const uint32_t LOGIN_BACKOFF_LADDER_MS[5] = { 5000u, 10000u, 30000u, 60000u, 300000u };

bool login_backoff_is_locked(const login_backoff_state_t *s, uint32_t now_ms)
{
    if (s->lock_dur_ms == 0u) {
        return false;
    }
    return (uint32_t)(now_ms - s->lock_start_ms) < s->lock_dur_ms;
}

uint32_t login_backoff_remaining_ms(const login_backoff_state_t *s, uint32_t now_ms)
{
    if (!login_backoff_is_locked(s, now_ms)) {
        return 0u;
    }
    return s->lock_dur_ms - (uint32_t)(now_ms - s->lock_start_ms);
}

void login_backoff_cycle_reset_if_due(login_backoff_state_t *s, uint32_t now_ms)
{
    // Drop an expired lock window now, while the elapsed time is still small.
    // Every slot lookup runs this, so an idle slot cannot sit expired for 2^32 ms
    // and read as locked again (the elapsed form would otherwise revive it).
    if (s->lock_dur_ms != 0u && !login_backoff_is_locked(s, now_ms)) {
        s->lock_dur_ms = 0u;
        s->lock_start_ms = 0u;
    }
    if (s->failure_count >= LOGIN_BACKOFF_LADDER_LEN && !login_backoff_is_locked(s, now_ms)) {
        s->failure_count = 0;
    }
}

void login_backoff_record_failure(login_backoff_state_t *s, uint32_t now_ms)
{
    if (s->failure_count < LOGIN_BACKOFF_LADDER_LEN) {
        s->failure_count++;
    }
    s->lock_start_ms = now_ms;
    s->lock_dur_ms = LOGIN_BACKOFF_LADDER_MS[s->failure_count - 1];
}

void login_backoff_record_success(login_backoff_state_t *s)
{
    s->failure_count = 0;
    s->lock_start_ms = 0;
    s->lock_dur_ms = 0;
}
