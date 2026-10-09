#include "login_backoff.h"

const uint32_t LOGIN_BACKOFF_LADDER_MS[5] = { 5000u, 10000u, 30000u, 60000u, 300000u };

bool login_backoff_is_locked(const login_backoff_state_t *s, uint32_t now_ms)
{
    if (s->locked_until_ms == 0u) {
        return false;
    }
    return (int32_t)(s->locked_until_ms - now_ms) > 0;
}

void login_backoff_cycle_reset_if_due(login_backoff_state_t *s, uint32_t now_ms)
{
    if (s->failure_count >= LOGIN_BACKOFF_LADDER_LEN && !login_backoff_is_locked(s, now_ms)) {
        s->failure_count = 0;
        s->locked_until_ms = 0;
    }
}

void login_backoff_record_failure(login_backoff_state_t *s, uint32_t now_ms)
{
    if (s->failure_count < LOGIN_BACKOFF_LADDER_LEN) {
        s->failure_count++;
    }
    s->locked_until_ms = now_ms + LOGIN_BACKOFF_LADDER_MS[s->failure_count - 1];
}

void login_backoff_record_success(login_backoff_state_t *s)
{
    s->failure_count = 0;
    s->locked_until_ms = 0;
}
