// test_recovery_auth.c -- host test for recovery_auth.c (secret selection and
// fallback format) and, as later fixes land, the shared ota_auth.c nonce/lockout
// behaviour the recovery routes rely on. Built and run by
// check_recovery_auth.ps1 (MSVC). Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "recovery_auth.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);        \
        }                                                                   \
    } while (0)

static void test_missing_ap_pass(void)
{
    // missing / short / long ap_pass must NOT be usable -> fallback applies.
    CHECK(!rauth_ap_pass_usable(NULL), "NULL ap_pass unusable");
    CHECK(!rauth_ap_pass_usable(""), "empty ap_pass unusable");
    CHECK(!rauth_ap_pass_usable("1234567"), "7-char ap_pass unusable");
    CHECK(rauth_ap_pass_usable("12345678"), "8-char ap_pass usable");
    char p63[64];
    memset(p63, 'a', 63);
    p63[63] = 0;
    CHECK(rauth_ap_pass_usable(p63), "63-char ap_pass usable");
    char p64[65];
    memset(p64, 'a', 64);
    p64[64] = 0;
    CHECK(!rauth_ap_pass_usable(p64), "64-char ap_pass unusable (WPA2 max 63)");
}

static void test_fallback_preimage(void)
{
    const uint8_t macA[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    const uint8_t macB[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x61};
    uint8_t a[96], b[96], c[96];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    memset(c, 0, sizeof(c));
    size_t na = rauth_fallback_preimage("key1", macA, a, sizeof(a));
    size_t nb = rauth_fallback_preimage("key1", macB, b, sizeof(b));
    size_t nc = rauth_fallback_preimage("key2", macA, c, sizeof(c));
    CHECK(na == strlen(RAUTH_PREIMAGE_PREFIX) + 4 + 1 + 6, "preimage length");
    CHECK(na == nb && memcmp(a, b, na) != 0, "mac changes the preimage");
    CHECK(na == nc && memcmp(a, c, na) != 0, "build key changes the preimage");
    CHECK(memcmp(a, RAUTH_PREIMAGE_PREFIX, strlen(RAUTH_PREIMAGE_PREFIX)) == 0, "prefix first");
    CHECK(memcmp(a + na - 6, macA, 6) == 0, "mac last, raw bytes");
    CHECK(rauth_fallback_preimage("key1", macA, a, na - 1) == 0, "too-small buffer refused");
    CHECK(rauth_fallback_preimage(NULL, macA, a, sizeof(a)) == 0, "NULL key refused");
    CHECK(rauth_fallback_preimage("key1", NULL, a, sizeof(a)) == 0, "NULL mac refused");
}

static void test_fallback_format(void)
{
    uint8_t d[32];
    for (int i = 0; i < 32; i++) {
        d[i] = (uint8_t)(0xA0 + i);
    }
    char out[RAUTH_FALLBACK_LEN + 1];
    rauth_fallback_format(d, out);
    CHECK(strcmp(out, "a0a1a2a3a4a5a6a7") == 0, "first 8 digest bytes, lowercase hex");
    CHECK(strlen(out) == RAUTH_FALLBACK_LEN, "16 chars");
    // The formatted secret must itself be a usable WPA2 passphrase / HMAC key.
    CHECK(rauth_ap_pass_usable(out), "fallback secret is a usable passphrase");
}

static void fill(uint8_t *n, uint8_t v)
{
    memset(n, v, OTA_AUTH_NONCE_LEN);
}

static void test_nonce_reuse_and_expiry(void)
{
    ota_auth_nonce_state_t n;
    memset(&n, 0, sizeof(n));
    CHECK(ota_auth_nonce_check(&n, 0) == OTA_AUTH_NONCE_NOT_ISSUED, "fresh state: not issued");
    uint8_t r[OTA_AUTH_NONCE_LEN];
    fill(r, 7);
    ota_auth_nonce_issue(&n, r, 1000);
    CHECK(ota_auth_nonce_check(&n, 1000) == OTA_AUTH_NONCE_OK, "issued nonce usable");
    CHECK(ota_auth_nonce_check(&n, 1000 + OTA_AUTH_NONCE_EXPIRY_MS) == OTA_AUTH_NONCE_OK,
          "usable at exactly 30 s");
    CHECK(ota_auth_nonce_check(&n, 1000 + OTA_AUTH_NONCE_EXPIRY_MS + 1) == OTA_AUTH_NONCE_EXPIRED,
          "expired just past 30 s");
    ota_auth_nonce_invalidate(&n);
    CHECK(ota_auth_nonce_check(&n, 1000) == OTA_AUTH_NONCE_ALREADY_USED, "nonce is single use");
    ota_auth_nonce_issue(&n, r, 2000);
    CHECK(ota_auth_nonce_check(&n, 2000) == OTA_AUTH_NONCE_OK, "reissue makes it usable again");
}

static void test_lockout_backoff(void)
{
    ota_auth_lockout_state_t l;
    memset(&l, 0, sizeof(l));
    uint32_t t = 100000;
    ota_auth_lockout_record_failure(&l, t);
    ota_auth_lockout_record_failure(&l, t);
    CHECK(!ota_auth_lockout_is_locked(&l, t), "two failures: not locked");
    ota_auth_lockout_record_failure(&l, t);
    CHECK(ota_auth_lockout_is_locked(&l, t), "third failure locks");
    CHECK(ota_auth_lockout_is_locked(&l, t + 59999), "still locked at 59.999 s");
    CHECK(!ota_auth_lockout_is_locked(&l, t + 60000), "unlocked at 60 s");
    // Second lockout doubles to 120 s.
    t += 60000;
    for (int i = 0; i < 3; i++) {
        ota_auth_lockout_record_failure(&l, t);
    }
    CHECK(ota_auth_lockout_is_locked(&l, t + 119999), "second lock lasts 120 s");
    CHECK(!ota_auth_lockout_is_locked(&l, t + 120000), "second lock lifts at 120 s");
    // Many more rounds hit the 15 minute ceiling and stay there.
    for (int round = 0; round < 12; round++) {
        t += 1000000;
        for (int i = 0; i < 3; i++) {
            ota_auth_lockout_record_failure(&l, t);
        }
    }
    CHECK(ota_auth_lockout_is_locked(&l, t + OTA_AUTH_LOCKOUT_MAX_MS - 1), "capped lock holds");
    CHECK(!ota_auth_lockout_is_locked(&l, t + OTA_AUTH_LOCKOUT_MAX_MS), "lock never exceeds 15 min");
    ota_auth_lockout_record_success(&l);
    CHECK(!ota_auth_lockout_is_locked(&l, t), "success clears the lock");
    for (int i = 0; i < 3; i++) {
        ota_auth_lockout_record_failure(&l, t);
    }
    CHECK(!ota_auth_lockout_is_locked(&l, t + 60000), "after success the backoff restarts at 60 s");
}

static void test_query_binding(void)
{
    uint8_t n[OTA_AUTH_NONCE_LEN];
    fill(n, 3);
    uint8_t a[160], b[160], c[160];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    memset(c, 0, sizeof(c));
    size_t la = rauth_build_msg(a, sizeof(a), n, "pico-upload", "crc=00000001&slot=A", 19);
    size_t lb = rauth_build_msg(b, sizeof(b), n, "pico-upload", "crc=00000001&slot=B", 19);
    size_t lc = rauth_build_msg(c, sizeof(c), n, "pico-upload", NULL, 0);
    CHECK(la == OTA_AUTH_NONCE_LEN + 11 + 1 + 19, "msg length with query");
    CHECK(lb == la && memcmp(a, b, la) != 0, "a tampered query changes the signed message");
    CHECK(memcmp(a + OTA_AUTH_NONCE_LEN + 11 + 1, "crc=00000001&slot=A", 19) == 0,
          "query bytes are copied verbatim into the message");
    CHECK(lc == OTA_AUTH_NONCE_LEN + 11, "no query: no '?' appended");
    CHECK(la > lc && memcmp(a, c, lc) == 0 && a[lc] == '?', "query is appended after a '?'");
    uint8_t d[160];
    size_t ld = rauth_build_msg(d, sizeof(d), n, "pico-abort", NULL, 0);
    CHECK(ld != lc || memcmp(c, d, lc) != 0, "context is part of the message");
    CHECK(rauth_build_msg(a, la - 1, n, "pico-upload", "crc=00000001&slot=A", 19) == 0,
          "too-small buffer refused");
    CHECK(rauth_build_msg(a, sizeof(a), n, NULL, NULL, 0) == 0, "NULL context refused");
}

static void test_nonce_ring(void)
{
    rauth_nonce_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    uint8_t r1[OTA_AUTH_NONCE_LEN], r2[OTA_AUTH_NONCE_LEN], r3[OTA_AUTH_NONCE_LEN];
    fill(r1, 1);
    fill(r2, 2);
    fill(r3, 3);
    CHECK(rauth_ring_find(&ring, 10) == NULL, "unknown client has no nonce");

    rauth_ring_issue(&ring, 10, r1, 1000);
    rauth_ring_issue(&ring, 20, r2, 1100);
    rauth_nonce_slot_t *a = rauth_ring_find(&ring, 10);
    CHECK(a && ota_auth_nonce_check(&a->st, 1200) == OTA_AUTH_NONCE_OK,
          "client B's challenge leaves client A's nonce live");
    CHECK(a && memcmp(a->st.nonce, r1, OTA_AUTH_NONCE_LEN) == 0, "each client keeps its own nonce");

    // Same client asking again retires only its own previous nonce.
    rauth_ring_issue(&ring, 10, r3, 1300);
    a = rauth_ring_find(&ring, 10);
    CHECK(a && memcmp(a->st.nonce, r3, OTA_AUTH_NONCE_LEN) == 0, "reissue replaces the client's nonce");
    int holders = 0;
    for (size_t i = 0; i < RAUTH_NONCE_SLOTS; i++) {
        holders += ring.slot[i].occupied && ring.slot[i].client == 10;
    }
    CHECK(holders == 1, "a client never occupies two slots");

    // Fill the ring with live nonces, then one more client evicts the OLDEST.
    // Issue order so far: 10 (reissued, seq 2), 20 (seq 1). 20 is the oldest.
    uint8_t rx[OTA_AUTH_NONCE_LEN];
    fill(rx, 9);
    rauth_ring_issue(&ring, 30, rx, 1400);
    rauth_ring_issue(&ring, 40, rx, 1500);
    rauth_ring_issue(&ring, 50, rx, 1600);
    CHECK(rauth_ring_find(&ring, 20) == NULL, "oldest live nonce evicted when the ring is full");
    CHECK(rauth_ring_find(&ring, 10) && rauth_ring_find(&ring, 30) && rauth_ring_find(&ring, 40) &&
              rauth_ring_find(&ring, 50),
          "newer nonces survive the eviction");

    // A spent nonce is reclaimed before any live one is evicted.
    rauth_nonce_slot_t *s30 = rauth_ring_find(&ring, 30);
    ota_auth_nonce_invalidate(&s30->st);
    rauth_ring_issue(&ring, 60, rx, 1700);
    CHECK(rauth_ring_find(&ring, 30) == NULL, "spent slot reused first");
    CHECK(rauth_ring_find(&ring, 10) && rauth_ring_find(&ring, 40) && rauth_ring_find(&ring, 50) &&
              rauth_ring_find(&ring, 60),
          "no live nonce evicted while a spent one was free");
}

static void test_extra_paths(void)
{
    // Lockout: failures below the threshold never lock; a lock resets the count.
    ota_auth_lockout_state_t l;
    memset(&l, 0, sizeof(l));
    for (int i = 0; i < 2; i++) {
        ota_auth_lockout_record_failure(&l, 5000);
    }
    CHECK(l.failure_count == 2 && l.lockout_tier == 0, "two failures: counted, tier 0");
    ota_auth_lockout_record_failure(&l, 5000);
    CHECK(l.failure_count == 0 && l.lockout_tier == 1, "a lock resets the failure count, tier 1");
    CHECK(l.locked_until_ms == 5000 + OTA_AUTH_LOCKOUT_BASE_MS, "first lock is exactly 60 s");
    for (int i = 0; i < 3; i++) {
        ota_auth_lockout_record_failure(&l, 70000);
    }
    for (int i = 0; i < 3; i++) {
        ota_auth_lockout_record_failure(&l, 200000);
    }
    CHECK(l.locked_until_ms == 200000 + 4 * OTA_AUTH_LOCKOUT_BASE_MS, "third lock is 240 s");

    // Nonce expiry is correct across a tick-counter wrap.
    ota_auth_nonce_state_t n;
    memset(&n, 0, sizeof(n));
    uint8_t r[OTA_AUTH_NONCE_LEN];
    fill(r, 5);
    ota_auth_nonce_issue(&n, r, 0xFFFFFF00u);
    CHECK(ota_auth_nonce_check(&n, 0x00000100u) == OTA_AUTH_NONCE_OK, "wrap: 512 ms later still usable");
    CHECK(ota_auth_nonce_check(&n, 0xFFFFFF00u + 40000u) == OTA_AUTH_NONCE_EXPIRED,
          "wrap: 40 s later expired");

    // Constant-time compare.
    uint8_t a[32], b[32];
    memset(a, 0xAB, sizeof(a));
    memcpy(b, a, sizeof(b));
    CHECK(ota_auth_constant_time_equal(a, b, sizeof(a)), "equal MACs compare equal");
    b[31] ^= 1;
    CHECK(!ota_auth_constant_time_equal(a, b, sizeof(a)), "last-byte difference detected");
    b[31] ^= 1;
    b[0] ^= 0x80;
    CHECK(!ota_auth_constant_time_equal(a, b, sizeof(a)), "first-byte difference detected");

    // Message builder edge cases.
    uint8_t nn[OTA_AUTH_NONCE_LEN], out[64];
    fill(nn, 4);
    CHECK(rauth_build_msg(out, sizeof(out), nn, "esp", "", 0) == OTA_AUTH_NONCE_LEN + 3,
          "empty query adds nothing");
    CHECK(rauth_build_msg(out, sizeof(out), nn, "esp", NULL, 4) == 0, "query_len without a query refused");
    CHECK(rauth_build_msg(out, OTA_AUTH_NONCE_LEN + 3, nn, "esp", NULL, 0) == OTA_AUTH_NONCE_LEN + 3,
          "message that exactly fills the buffer is accepted");
    CHECK(rauth_build_msg(out, OTA_AUTH_NONCE_LEN + 3, nn, "esp", "x", 1) == 0,
          "one byte over the buffer is refused");

    // Ring: identical nonce bytes for two clients still get distinct slots, and
    // client key 0 (unreadable peer) is an ordinary client.
    rauth_nonce_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    rauth_nonce_slot_t *s0 = rauth_ring_issue(&ring, 0, nn, 100);
    rauth_nonce_slot_t *s1 = rauth_ring_issue(&ring, 1, nn, 100);
    CHECK(s0 != s1, "distinct clients get distinct slots");
    CHECK(rauth_ring_find(&ring, 0) == s0 && rauth_ring_find(&ring, 1) == s1, "find returns the issued slots");
    CHECK(ota_auth_nonce_check(&s0->st, 100 + OTA_AUTH_NONCE_EXPIRY_MS + 1) == OTA_AUTH_NONCE_EXPIRED,
          "an expired nonce stays findable but reports expired");
}

int main(void)
{
    test_missing_ap_pass();
    test_fallback_preimage();
    test_fallback_format();
    test_nonce_reuse_and_expiry();
    test_lockout_backoff();
    test_query_binding();
    test_nonce_ring();
    test_extra_paths();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
