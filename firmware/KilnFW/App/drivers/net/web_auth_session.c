// web_auth_session.c -- see web_auth_session.h for scope/ownership.
#include "web_auth_session.h"

#include <string.h>

void web_auth_table_init(web_auth_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

bool web_auth_session_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    // Same discipline as ota_auth_constant_time_equal(): always inspect
    // every byte, never early-exit on the first mismatch.
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static size_t find_lru_slot(const web_auth_table_t *t)
{
    size_t lru = 0;
    uint32_t oldest = t->slots[0].last_seen_ms;
    for (size_t i = 1; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        if (t->slots[i].last_seen_ms < oldest) {
            oldest = t->slots[i].last_seen_ms;
            lru = i;
        }
    }
    return lru;
}

size_t web_auth_table_create_session(web_auth_table_t *t, const uint8_t token_hash[WEB_AUTH_TOKEN_HASH_LEN],
                                      const char *client_ip, web_auth_session_role_t role, uint32_t now_ms)
{
    size_t idx = WEB_AUTH_WEB_SLOT_COUNT; // sentinel, replaced below
    for (size_t i = 0; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        if (!t->slots[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx == WEB_AUTH_WEB_SLOT_COUNT) {
        // Table full -- evict the least-recently-seen slot rather than
        // refusing the new login (WEB_AUTH_PLAN.md section 4: "a stale
        // session can never lock out a real operator").
        idx = find_lru_slot(t);
    }

    web_auth_slot_t *slot = &t->slots[idx];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    memcpy(slot->token_hash, token_hash, WEB_AUTH_TOKEN_HASH_LEN);
    if (client_ip) {
        size_t n = strlen(client_ip);
        if (n >= WEB_AUTH_CLIENT_IP_LEN) {
            // Finding 1 fix (2026-09-17 review): does not fit even in a
            // buffer sized for a full IPv6 address string -- do not silently
            // truncate (that is exactly what produced the IPv6 lockout/
            // collision this fix addresses). Record no binding at all rather
            // than a guessed-at prefix; see this function's header comment.
            slot->client_ip[0] = '\0';
        } else {
            memcpy(slot->client_ip, client_ip, n);
            slot->client_ip[n] = '\0';
        }
    }
    slot->role = role;
    slot->issued_ms = now_ms;
    slot->last_seen_ms = now_ms;
    slot->prompted = false;
    return idx;
}

int web_auth_table_find_by_token(const web_auth_table_t *t, const uint8_t token_hash[WEB_AUTH_TOKEN_HASH_LEN])
{
    for (size_t i = 0; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        const web_auth_slot_t *slot = &t->slots[i];
        if (!slot->in_use) {
            continue;
        }
        if (web_auth_session_constant_time_equal(slot->token_hash, token_hash, WEB_AUTH_TOKEN_HASH_LEN)) {
            return (int)i;
        }
    }
    return -1;
}

void web_auth_table_touch(web_auth_table_t *t, size_t idx, uint32_t now_ms)
{
    if (idx >= WEB_AUTH_WEB_SLOT_COUNT || !t->slots[idx].in_use) {
        return;
    }
    t->slots[idx].last_seen_ms = now_ms;
    t->slots[idx].prompted = false;
}

void web_auth_table_set_via_ap(web_auth_table_t *t, size_t idx, bool via_ap)
{
    if (idx >= WEB_AUTH_WEB_SLOT_COUNT || !t->slots[idx].in_use) {
        return;
    }
    t->slots[idx].via_ap = via_ap;
}

bool web_auth_table_any_ap_session_active(const web_auth_table_t *t, uint32_t timeout_s, uint32_t now_ms)
{
    for (size_t i = 0; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        const web_auth_slot_t *slot = &t->slots[i];
        if (slot->in_use && slot->via_ap && web_auth_session_is_valid(slot->last_seen_ms, timeout_s, now_ms)) {
            return true;
        }
    }
    return false;
}

void web_auth_table_destroy_session(web_auth_table_t *t, size_t idx)
{
    if (idx >= WEB_AUTH_WEB_SLOT_COUNT) {
        return;
    }
    memset(&t->slots[idx], 0, sizeof(t->slots[idx]));
}

void web_auth_table_destroy_role(web_auth_table_t *t, web_auth_session_role_t role)
{
    for (size_t i = 0; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        if (t->slots[i].in_use && t->slots[i].role == role) {
            memset(&t->slots[i], 0, sizeof(t->slots[i]));
        }
    }
}

void web_auth_table_destroy_all(web_auth_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

void web_auth_lcd_session_init(web_auth_lcd_session_t *s)
{
    memset(s, 0, sizeof(*s));
}

void web_auth_lcd_session_create(web_auth_lcd_session_t *s, web_auth_session_role_t role, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->active = true;
    s->role = role;
    s->issued_ms = now_ms;
    s->last_seen_ms = now_ms;
    s->prompted = false;
}

void web_auth_lcd_session_touch(web_auth_lcd_session_t *s, uint32_t now_ms)
{
    if (!s->active) {
        return;
    }
    s->last_seen_ms = now_ms;
    s->prompted = false;
}

void web_auth_lcd_session_destroy(web_auth_lcd_session_t *s)
{
    memset(s, 0, sizeof(*s));
}

bool web_auth_session_is_valid(uint32_t last_seen_ms, uint32_t timeout_s, uint32_t now_ms)
{
    if (timeout_s == WEB_AUTH_TIMEOUT_NEVER_S) {
        return true;
    }
    uint32_t timeout_ms = timeout_s * 1000u;
    uint32_t elapsed_ms = now_ms - last_seen_ms; // unsigned wraparound is fine: now_ms is
                                                   // always >= last_seen_ms in real use, and
                                                   // this module never sees the reverse
    return elapsed_ms <= timeout_ms;
}

bool web_auth_session_in_prompt_window(uint32_t last_seen_ms, uint32_t timeout_s, uint32_t now_ms)
{
    if (timeout_s == WEB_AUTH_TIMEOUT_NEVER_S) {
        return false;
    }
    if (!web_auth_session_is_valid(last_seen_ms, timeout_s, now_ms)) {
        return false;
    }
    uint32_t timeout_ms = timeout_s * 1000u;
    uint32_t elapsed_ms = now_ms - last_seen_ms;
    if (timeout_ms < WEB_AUTH_PROMPT_WINDOW_MS) {
        // A timeout shorter than the prompt window itself -- every valid
        // moment is inside the prompt window. Not expected in practice (the
        // policy page's minimum is 1 minute), but handled rather than
        // underflowing.
        return true;
    }
    return elapsed_ms >= (timeout_ms - WEB_AUTH_PROMPT_WINDOW_MS);
}

web_auth_session_role_t web_auth_effective_role(const web_auth_table_t *t, bool web_enabled,
                                         const uint8_t *token_hash, const char *client_ip,
                                         uint32_t timeout_s, uint32_t now_ms)
{
    if (!web_enabled) {
        // Section 11: "the enforcement pre-handler's first check is
        // web_enabled, and if it is false it returns ALLOW immediately,
        // before any session lookup." Returning ADMIN here (the most
        // permissive role) unconditionally, with no table access at all,
        // IS that short-circuit -- no added latency, no new failure mode.
        return WEB_AUTH_SESSION_ROLE_ADMIN;
    }
    if (!token_hash) {
        return WEB_AUTH_SESSION_ROLE_NONE;
    }
    int idx = web_auth_table_find_by_token(t, token_hash);
    if (idx < 0) {
        return WEB_AUTH_SESSION_ROLE_NONE;
    }
    const web_auth_slot_t *slot = &t->slots[(size_t)idx];
    if (!web_auth_session_is_valid(slot->last_seen_ms, timeout_s, now_ms)) {
        return WEB_AUTH_SESSION_ROLE_NONE;
    }
    // Finding 1 fix (2026-09-17 review / prior defect 5): enforce the
    // client_ip binding the header has always documented as a MUST. Exact
    // match against what was recorded at login -- see web_auth_session.h's
    // declaration comment for why exact, not a prefix. `client_ip == NULL`
    // (no determinable peer address) can never match a real recorded
    // binding, so it denies rather than being treated as "skip the check".
    if (!client_ip || strcmp(slot->client_ip, client_ip) != 0) {
        return WEB_AUTH_SESSION_ROLE_NONE;
    }
    return slot->role;
}

bool web_auth_admin_bootstrap_needed(bool effective_enabled, bool admin_credential_configured)
{
    return effective_enabled && !admin_credential_configured;
}
