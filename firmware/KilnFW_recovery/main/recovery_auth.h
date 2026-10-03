// recovery_auth.h -- pure (no ESP-IDF) pieces of the recovery image's auth:
// which secret is in force (stored ap_pass or the eFuse-MAC fallback), the
// fallback's preimage/format, and the per-client nonce ring. Host-tested by
// check_recovery_auth.ps1 (test_recovery_auth.c, MSVC).
//
// Fallback secret (docs/RECOVERY_IMAGE_PLAN.md, "Auth fallback secret"):
//   preimage = "kilnctl-recovery-auth-v1|" || BUILD_KEY || "|" || mac[6]
//   secret   = lowercase hex of the first 8 bytes of SHA-256(preimage)   (16 chars)
// mac is the factory eFuse base MAC (esp_efuse_mac_get_default). The secret is
// used both as the SoftAP WPA2 passphrase and as the HMAC key material in
// place of ap_pass, so the AP is never OPEN and every mutating route still
// authenticates. The SHA-256 itself runs in recovery_wifi.c (mbedtls); this
// file stays hash-free so it needs no hand-rolled crypto.
#ifndef RECOVERY_AUTH_H
#define RECOVERY_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RAUTH_FALLBACK_LEN 16u
#define RAUTH_AP_PASS_MIN 8u   // WPA2 passphrase minimum
#define RAUTH_AP_PASS_MAX 63u  // WPA2 passphrase maximum
#define RAUTH_PREIMAGE_PREFIX "kilnctl-recovery-auth-v1|"

// True when `p` can serve as both a WPA2 passphrase and the HMAC key material:
// non-NULL, 8..63 characters.
bool rauth_ap_pass_usable(const char *p);

// Writes the fallback preimage into `out` (no NUL). Returns its length, or 0
// if `cap` is too small or an argument is NULL.
size_t rauth_fallback_preimage(const char *build_key, const uint8_t mac[6], uint8_t *out,
                               size_t cap);

// Formats digest[0..8) as 16 lowercase hex chars plus NUL.
void rauth_fallback_format(const uint8_t digest[32], char out[RAUTH_FALLBACK_LEN + 1]);

// ---- per-client nonce ring --------------------------------------------------
// One nonce per client (keyed by the peer address httpd reports), RAUTH_NONCE_SLOTS
// clients deep, instead of ONE global nonce. With a single global slot any host
// that fetched /api/ota/challenge retired the operator's nonce mid-handshake (a
// trivial denial of service); here a challenge only replaces the SAME client's
// previous nonce. Lifecycle per slot is ota_auth.c's (single use, 30 s expiry).
#define RAUTH_NONCE_SLOTS 4u

typedef struct {
    ota_auth_nonce_state_t st;
    uint32_t client;   // opaque client key (peer address)
    uint32_t seq;      // issue order, for oldest-first eviction
    bool occupied;
} rauth_nonce_slot_t;

typedef struct {
    rauth_nonce_slot_t slot[RAUTH_NONCE_SLOTS];
    uint32_t next_seq;
} rauth_nonce_ring_t;

// Issues `rand_bytes` as `client`'s nonce and returns its slot. Reuses the
// client's own slot; otherwise a free slot; otherwise a slot whose nonce is
// already used or expired; otherwise evicts the OLDEST live nonce.
rauth_nonce_slot_t *rauth_ring_issue(rauth_nonce_ring_t *r, uint32_t client,
                                     const uint8_t rand_bytes[OTA_AUTH_NONCE_LEN], uint32_t now_ms);

// The slot holding `client`'s nonce, or NULL if the client never got one (or it
// was evicted). The caller runs ota_auth_nonce_check()/_invalidate() on ->st.
rauth_nonce_slot_t *rauth_ring_find(rauth_nonce_ring_t *r, uint32_t client);

// ---- MAC message ------------------------------------------------------------
// message = nonce || context [|| "?" || query]. The query is authenticated so a
// route's parameters (the Pico upload's ?crc=&slot=) cannot be swapped without
// the secret. Returns the length, or 0 if it does not fit in `cap` or `context`
// is NULL. `query_len` 0 means no query (no "?" appended).
size_t rauth_build_msg(uint8_t *out, size_t cap, const uint8_t nonce[OTA_AUTH_NONCE_LEN],
                       const char *context, const char *query, size_t query_len);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_AUTH_H
