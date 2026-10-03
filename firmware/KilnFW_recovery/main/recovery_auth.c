// recovery_auth.c -- see recovery_auth.h.
#include "recovery_auth.h"

#include <string.h>

bool rauth_ap_pass_usable(const char *p)
{
    if (!p) {
        return false;
    }
    size_t n = strlen(p);
    return n >= RAUTH_AP_PASS_MIN && n <= RAUTH_AP_PASS_MAX;
}

size_t rauth_fallback_preimage(const char *build_key, const uint8_t mac[6], uint8_t *out,
                               size_t cap)
{
    if (!build_key || !mac || !out) {
        return 0;
    }
    size_t plen = strlen(RAUTH_PREIMAGE_PREFIX);
    size_t klen = strlen(build_key);
    size_t total = plen + klen + 1 + 6;
    if (total > cap) {
        return 0;
    }
    memcpy(out, RAUTH_PREIMAGE_PREFIX, plen);
    memcpy(out + plen, build_key, klen);
    out[plen + klen] = '|';
    memcpy(out + plen + klen + 1, mac, 6);
    return total;
}

void rauth_fallback_format(const uint8_t digest[32], char out[RAUTH_FALLBACK_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < RAUTH_FALLBACK_LEN / 2; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[RAUTH_FALLBACK_LEN] = '\0';
}

rauth_nonce_slot_t *rauth_ring_issue(rauth_nonce_ring_t *r, uint32_t client,
                                     const uint8_t rand_bytes[OTA_AUTH_NONCE_LEN], uint32_t now_ms)
{
    rauth_nonce_slot_t *pick = NULL;
    // 1. the client's own slot: a new challenge retires only ITS previous nonce.
    pick = rauth_ring_find(r, client);
    // 2. a free slot.
    for (size_t i = 0; !pick && i < RAUTH_NONCE_SLOTS; i++) {
        if (!r->slot[i].occupied) {
            pick = &r->slot[i];
        }
    }
    // 3. a slot whose nonce is already spent or expired (never evict a live one
    //    while a dead one is available).
    for (size_t i = 0; !pick && i < RAUTH_NONCE_SLOTS; i++) {
        if (ota_auth_nonce_check(&r->slot[i].st, now_ms) != OTA_AUTH_NONCE_OK) {
            pick = &r->slot[i];
        }
    }
    // 4. every slot holds a live nonce: evict the oldest.
    if (!pick) {
        pick = &r->slot[0];
        for (size_t i = 1; i < RAUTH_NONCE_SLOTS; i++) {
            if ((int32_t)(r->slot[i].seq - pick->seq) < 0) {
                pick = &r->slot[i];
            }
        }
    }
    ota_auth_nonce_issue(&pick->st, rand_bytes, now_ms);
    pick->client = client;
    pick->seq = r->next_seq++;
    pick->occupied = true;
    return pick;
}

rauth_nonce_slot_t *rauth_ring_find(rauth_nonce_ring_t *r, uint32_t client)
{
    for (size_t i = 0; i < RAUTH_NONCE_SLOTS; i++) {
        if (r->slot[i].occupied && r->slot[i].client == client) {
            return &r->slot[i];
        }
    }
    return NULL;
}

size_t rauth_build_msg(uint8_t *out, size_t cap, const uint8_t nonce[OTA_AUTH_NONCE_LEN],
                       const char *context, const char *query, size_t query_len)
{
    if (!out || !nonce || !context || (query_len > 0 && !query)) {
        return 0;
    }
    size_t clen = strlen(context);
    size_t total = OTA_AUTH_NONCE_LEN + clen + (query_len > 0 ? 1 + query_len : 0);
    if (total > cap) {
        return 0;
    }
    memcpy(out, nonce, OTA_AUTH_NONCE_LEN);
    memcpy(out + OTA_AUTH_NONCE_LEN, context, clen);
    size_t n = OTA_AUTH_NONCE_LEN + clen;
    if (query_len > 0) {
        out[n++] = '?';
        memcpy(out + n, query, query_len);
        n += query_len;
    }
    return n;
}
