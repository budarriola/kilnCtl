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

int main(void)
{
    test_missing_ap_pass();
    test_fallback_preimage();
    test_fallback_format();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
