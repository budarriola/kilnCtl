// totp_http_core.c -- see totp_http_core.h.
#include "totp_http_core.h"

#include <string.h>

void totp_pending_clear(totp_pending_enrollment_t *p)
{
    totp_secure_zero(p->secret, sizeof(p->secret));
    p->active = false;
    p->started_at_ms = 0;
}

void totp_pending_begin(totp_pending_enrollment_t *p, const uint8_t secret[TOTP_SECRET_LEN], uint32_t now_ms)
{
    totp_pending_clear(p);
    memcpy(p->secret, secret, TOTP_SECRET_LEN);
    p->active = true;
    p->started_at_ms = now_ms;
}

bool totp_pending_is_valid(totp_pending_enrollment_t *p, uint32_t now_ms)
{
    if (!p->active) {
        return false;
    }
    if ((uint32_t)(now_ms - p->started_at_ms) > TOTP_PENDING_TTL_MS) {
        totp_pending_clear(p);
        return false;
    }
    return true;
}

void totp_reset_token_table_init(totp_reset_token_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

static bool totp_reset_token_slot_is_live(const totp_reset_token_slot_t *s, uint32_t now_ms)
{
    return s->active && !s->used && (int32_t)(s->expires_at_ms - now_ms) > 0;
}

void totp_reset_token_store(totp_reset_token_table_t *t, const char *token_hex, const char *username,
                             uint32_t now_ms)
{
    int free_idx = -1;
    int soonest_idx = 0;
    uint32_t soonest_expiry = UINT32_MAX;
    for (unsigned i = 0; i < TOTP_RESET_TOKEN_SLOTS; i++) {
        totp_reset_token_slot_t *s = &t->slots[i];
        if (!totp_reset_token_slot_is_live(s, now_ms)) {
            if (free_idx < 0) {
                free_idx = (int)i;
            }
            continue;
        }
        if (s->expires_at_ms < soonest_expiry) {
            soonest_expiry = s->expires_at_ms;
            soonest_idx = (int)i;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : soonest_idx;
    totp_reset_token_slot_t *s = &t->slots[idx];
    memset(s, 0, sizeof(*s));
    s->active = true;
    s->used = false;
    strncpy(s->token_hex, token_hex, TOTP_RESET_TOKEN_HEX_LEN);
    s->token_hex[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
    strncpy(s->username, username, TOTP_RESET_USERNAME_MAX);
    s->username[TOTP_RESET_USERNAME_MAX] = '\0';
    s->expires_at_ms = now_ms + TOTP_RESET_TOKEN_TTL_MS;
}

totp_reset_token_result_t totp_reset_token_consume(totp_reset_token_table_t *t, const char *token_hex,
                                                    const char *username, uint32_t now_ms)
{
    // The token is a bearer secret: compare it in constant time over its
    // fixed width (never strcmp(), whose early exit leaks how many leading
    // hex digits matched), and only once its length is exactly right.
    if (token_hex == NULL || username == NULL || strnlen(token_hex, TOTP_RESET_TOKEN_HEX_LEN + 1u) !=
                                                     TOTP_RESET_TOKEN_HEX_LEN) {
        return TOTP_RESET_TOKEN_NOT_FOUND;
    }
    for (unsigned i = 0; i < TOTP_RESET_TOKEN_SLOTS; i++) {
        totp_reset_token_slot_t *s = &t->slots[i];
        if (!s->active) {
            continue;
        }
        bool token_ok = totp_constant_time_equal((const uint8_t *)s->token_hex, (const uint8_t *)token_hex,
                                                 TOTP_RESET_TOKEN_HEX_LEN);
        bool user_ok = strcmp(s->username, username) == 0;
        if (!(token_ok && user_ok)) {
            continue;
        }
        // Matched by (token, username): every other outcome below is
        // specific to THIS slot, not "not found" -- a caller presenting the
        // right token for the right user gets an honest expired/used
        // answer, not a generic miss. (The route above this call still
        // folds all of these into one generic 400 response -- see
        // auth_totp_http.c -- this distinction is for host tests only.)
        if (s->used) {
            return TOTP_RESET_TOKEN_ALREADY_USED;
        }
        if ((int32_t)(s->expires_at_ms - now_ms) <= 0) {
            return TOTP_RESET_TOKEN_EXPIRED;
        }
        s->used = true;
        return TOTP_RESET_TOKEN_OK;
    }
    return TOTP_RESET_TOKEN_NOT_FOUND;
}

bool totp_forgot_board_cap_blocked(const totp_forgot_board_cap_t *c)
{
    return c->failures >= TOTP_FORGOT_BOARD_CAP;
}

void totp_forgot_board_cap_record(totp_forgot_board_cap_t *c, bool verified)
{
    if (verified) {
        return; // never a reset -- plan section 4: cleared only by a reboot
    }
    if (c->failures < TOTP_FORGOT_BOARD_CAP) {
        c->failures++;
    }
}
