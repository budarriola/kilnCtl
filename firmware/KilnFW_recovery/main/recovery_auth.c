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
