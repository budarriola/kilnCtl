// ota_auth.c -- see ota_auth.h.
#include "ota_auth.h"

#include <string.h>

void ota_auth_nonce_issue(ota_auth_nonce_state_t *s, const uint8_t rand_bytes[OTA_AUTH_NONCE_LEN],
                           uint32_t now_ms)
{
    memcpy(s->nonce, rand_bytes, OTA_AUTH_NONCE_LEN);
    s->valid = true;
    s->used = false;
    s->issued_at_ms = now_ms;
}

ota_auth_nonce_check_t ota_auth_nonce_check(const ota_auth_nonce_state_t *s, uint32_t now_ms)
{
    if (!s->valid) {
        return OTA_AUTH_NONCE_NOT_ISSUED;
    }
    if (s->used) {
        return OTA_AUTH_NONCE_ALREADY_USED;
    }
    // Unsigned subtraction: correct even across a tick-counter wraparound,
    // as long as the expiry window (30s) is far shorter than the wraparound
    // period -- same reasoning this codebase's other tick-delta comparisons
    // rely on.
    if ((uint32_t)(now_ms - s->issued_at_ms) > OTA_AUTH_NONCE_EXPIRY_MS) {
        return OTA_AUTH_NONCE_EXPIRED;
    }
    return OTA_AUTH_NONCE_OK;
}

void ota_auth_nonce_invalidate(ota_auth_nonce_state_t *s)
{
    s->used = true;
}

bool ota_auth_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool ota_auth_lockout_is_locked(const ota_auth_lockout_state_t *s, uint32_t now_ms)
{
    if (s->locked_until_ms == 0u) {
        return false;
    }
    return (int32_t)(s->locked_until_ms - now_ms) > 0;
}

void ota_auth_lockout_record_failure(ota_auth_lockout_state_t *s, uint32_t now_ms)
{
    s->failure_count++;
    if (s->failure_count < OTA_AUTH_LOCKOUT_THRESHOLD) {
        return;
    }

    uint32_t backoff_ms = OTA_AUTH_LOCKOUT_BASE_MS;
    for (uint32_t i = 0; i < s->lockout_tier; i++) {
        if (backoff_ms >= OTA_AUTH_LOCKOUT_MAX_MS / 2u) {
            backoff_ms = OTA_AUTH_LOCKOUT_MAX_MS;
            break;
        }
        backoff_ms *= 2u;
    }
    if (backoff_ms > OTA_AUTH_LOCKOUT_MAX_MS) {
        backoff_ms = OTA_AUTH_LOCKOUT_MAX_MS;
    }

    s->locked_until_ms = now_ms + backoff_ms;
    s->lockout_tier++;
    s->failure_count = 0;
}

void ota_auth_lockout_record_success(ota_auth_lockout_state_t *s)
{
    s->failure_count = 0;
    s->lockout_tier = 0;
    s->locked_until_ms = 0;
}
