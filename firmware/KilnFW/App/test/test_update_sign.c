// Host tests for App/drivers/update/update_sign.c (GITHUB_RELEASE_UPDATE_PLAN.md WP11):
// Ed25519 verification of release.json against a key list, the enforcement decision, and the
// release.json.sig asset pick in the API parser. TEST keys only: the RFC 8032 section 7.1 vector 2
// pair and a throwaway keypair whose private half was discarded after signing KNOWN_MANIFEST.
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_release.h"
#include "../drivers/update/update_sign.h"

#define REPO "budarriola/kilnCtl"
#define BASE "https://github.com/budarriola/kilnCtl/releases/download/"

// Throwaway pair signed with Python's cryptography (tools/sign_release.py's engine), verified here
// by an independent implementation (Monocypher).
static const uint8_t PUB_A[32] = { 0x38, 0xde, 0xc7, 0x31, 0xdd, 0x4b, 0xf5, 0x71, 0x3e, 0xef, 0xf6, 0xc3, 0x52, 0xa2, 0x76, 0xcc,
                                   0xd4, 0xda, 0x78, 0x8c, 0x04, 0x59, 0x52, 0xac, 0x69, 0x88, 0x7f, 0x75, 0x8b, 0x29, 0xa3, 0xf8 };
static const uint8_t KNOWN_MANIFEST[] = "{\"schema\":1,\"tag\":\"v9.9.9\",\"repo\":\"budarriola/kilnCtl\"}\n";
static const uint8_t SIG_A[64] = { 0x32, 0x99, 0x47, 0xbd, 0xb2, 0x21, 0x45, 0x27, 0x01, 0xe7, 0x60, 0xe2, 0x83, 0x83, 0xb3, 0xb7,
                                   0x94, 0xc1, 0x8e, 0x2e, 0x9a, 0x4b, 0xf6, 0x75, 0xaf, 0xfc, 0xa8, 0x1f, 0x3d, 0x6b, 0xa8, 0x19,
                                   0x2e, 0x97, 0xd4, 0x7c, 0x4b, 0xe5, 0x60, 0x2f, 0x3d, 0xad, 0xf5, 0x53, 0x33, 0xc5, 0x09, 0xb0,
                                   0xab, 0x42, 0xf3, 0x1b, 0x43, 0xf4, 0x08, 0x91, 0x2b, 0x58, 0x71, 0x80, 0xf6, 0x36, 0xb9, 0x02 };

// RFC 8032 7.1 TEST 2: message 0x72.
static const uint8_t PUB_RFC[32] = { 0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
                                     0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c };
static const uint8_t SIG_RFC[64] = { 0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40,
                                     0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f, 0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda,
                                     0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
                                     0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00 };

static const uint8_t KEYS_AB[2][32] = {
    { 0x00 }, // a key that matches nothing (all zero but the first byte)
    { 0x38, 0xde, 0xc7, 0x31, 0xdd, 0x4b, 0xf5, 0x71, 0x3e, 0xef, 0xf6, 0xc3, 0x52, 0xa2, 0x76, 0xcc,
      0xd4, 0xda, 0x78, 0x8c, 0x04, 0x59, 0x52, 0xac, 0x69, 0x88, 0x7f, 0x75, 0x8b, 0x29, 0xa3, 0xf8 },
};

static void test_verify(void)
{
    TEST_SECTION("update_sign -- Ed25519 verify, good / bad / missing");
    const size_t mlen = sizeof(KNOWN_MANIFEST) - 1;
    update_sig_keyset_t one = { (const uint8_t (*)[32])PUB_A, 1 };
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen, SIG_A, 64) == UPDATE_SIG_VERIFIED, "good signature verifies");

    uint8_t bad[64];
    memcpy(bad, SIG_A, 64);
    bad[10] ^= 0x01;
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen, bad, 64) == UPDATE_SIG_INVALID, "flipped signature bit refused");
    memcpy(bad, SIG_A, 64);
    bad[63] ^= 0x80;
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen, bad, 64) == UPDATE_SIG_INVALID, "flipped top bit of S refused");
    uint8_t tampered[sizeof(KNOWN_MANIFEST)];
    memcpy(tampered, KNOWN_MANIFEST, sizeof(tampered));
    tampered[20] ^= 0x01;
    TEST_CHECK(update_sig_check(REPO, &one, tampered, mlen, SIG_A, 64) == UPDATE_SIG_INVALID, "tampered manifest refused");
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen - 1, SIG_A, 64) == UPDATE_SIG_INVALID, "truncated manifest refused");

    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen, NULL, 0) == UPDATE_SIG_MISSING, "missing signature");
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen, SIG_A, 63) == UPDATE_SIG_INVALID, "short signature");
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, mlen, SIG_A, 65) == UPDATE_SIG_INVALID, "long signature");
    TEST_CHECK(update_sig_check(REPO, &one, KNOWN_MANIFEST, 0, SIG_A, 64) == UPDATE_SIG_INVALID, "empty manifest");

    update_sig_keyset_t wrong = { (const uint8_t (*)[32])PUB_RFC, 1 };
    TEST_CHECK(update_sig_check(REPO, &wrong, KNOWN_MANIFEST, mlen, SIG_A, 64) == UPDATE_SIG_INVALID, "signature under another key refused");
    update_sig_keyset_t two = { KEYS_AB, 2 };
    TEST_CHECK(update_sig_check(REPO, &two, KNOWN_MANIFEST, mlen, SIG_A, 64) == UPDATE_SIG_VERIFIED, "any key in the list may verify (rotation)");

    const uint8_t m72 = 0x72;
    update_sig_keyset_t rfc = { (const uint8_t (*)[32])PUB_RFC, 1 };
    TEST_CHECK(update_sig_check(REPO, &rfc, &m72, 1, SIG_RFC, 64) == UPDATE_SIG_VERIFIED, "RFC 8032 test 2 vector verifies");
    const uint8_t m73 = 0x73;
    TEST_CHECK(update_sig_check(REPO, &rfc, &m73, 1, SIG_RFC, 64) == UPDATE_SIG_INVALID, "RFC 8032 vector with a changed message refused");
}

static void test_enforcement(void)
{
    TEST_SECTION("update_sign -- enforcement applies to the default repo with keys only");
    const size_t mlen = sizeof(KNOWN_MANIFEST) - 1;
    update_sig_keyset_t one = { (const uint8_t (*)[32])PUB_A, 1 };
    update_sig_keyset_t none = { NULL, 0 };
    TEST_CHECK(update_sig_repo_is_default("budarriola/kilnCtl") && update_sig_repo_is_default("BUDARRIOLA/KILNCTL"),
               "default repo, case-insensitive");
    TEST_CHECK(!update_sig_repo_is_default("budarriola/kilnCtl2") && !update_sig_repo_is_default("budarriola/kilnCt") &&
                   !update_sig_repo_is_default("evil/kilnCtl") && !update_sig_repo_is_default("") &&
                   !update_sig_repo_is_default(NULL),
               "near-misses are not the default repo");
    TEST_CHECK(update_sig_required(REPO, &one) && !update_sig_required(REPO, &none) && !update_sig_required(REPO, NULL) &&
                   !update_sig_required("someone/else", &one),
               "required only for the default repo and a non-empty key list");
    TEST_CHECK(update_sig_check(REPO, &none, KNOWN_MANIFEST, mlen, NULL, 0) == UPDATE_SIG_NOT_ENFORCED,
               "no compiled-in key: unsigned stays allowed (D4)");
    TEST_CHECK(update_sig_check("someone/else", &one, KNOWN_MANIFEST, mlen, NULL, 0) == UPDATE_SIG_NOT_ENFORCED,
               "non-default repo is never enforced (D5)");
    TEST_CHECK(update_sig_check("someone/else", &one, KNOWN_MANIFEST, mlen, SIG_A, 64) == UPDATE_SIG_NOT_ENFORCED,
               "non-default repo is not verified even with a signature (stays UNSIGNED)");
    TEST_CHECK(update_sig_builtin_keys().count == 0, "the shipped key list holds no test key");
    TEST_CHECK(strcmp(update_sig_result_name(UPDATE_SIG_MISSING), "release_unsigned") == 0 &&
                   strcmp(update_sig_result_name(UPDATE_SIG_INVALID), "signature_invalid") == 0,
               "refusal names");
}

static void test_sig_asset_pick(void)
{
    TEST_SECTION("update_release -- release.json.sig asset pick");
    update_release_info_t r;
    const char *with_sig =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin\"},"
        "{\"name\":\"release.json\",\"size\":900,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"},"
        "{\"name\":\"release.json.sig\",\"size\":64,\"browser_download_url\":\"" BASE "v1.2.3/release.json.sig\"}]}";
    TEST_CHECK(update_release_parse_api(with_sig, strlen(with_sig), REPO, 0x400000u, &r) == UPDATE_REL_OK &&
                   strcmp(r.sig_url, BASE "v1.2.3/release.json.sig") == 0 && r.sig_size == 64u,
               "signature asset picked");
    char no_sig[1024];
    snprintf(no_sig, sizeof(no_sig), "%s", with_sig);
    char *p = strstr(no_sig, "release.json.sig");
    p[13] = 'X'; // "release.json.sXg": no longer our asset name
    TEST_CHECK(update_release_parse_api(no_sig, strlen(no_sig), REPO, 0x400000u, &r) == UPDATE_REL_OK && r.sig_url[0] == '\0',
               "release without the signature asset parses with an empty sig_url");
    char bad_size[1024];
    snprintf(bad_size, sizeof(bad_size), "%s", with_sig);
    p = strstr(bad_size, "\"size\":64");
    p[7] = '5'; // "size":64 -> "size":54
    TEST_CHECK(update_release_parse_api(bad_size, strlen(bad_size), REPO, 0x400000u, &r) == UPDATE_REL_E_BAD_SIZE,
               "signature asset of the wrong size refused");
    char dup[2048];
    snprintf(dup, sizeof(dup), "%s", with_sig);
    char *end = strrchr(dup, ']');
    snprintf(end, sizeof(dup) - (size_t)(end - dup),
             ",{\"name\":\"release.json.sig\",\"size\":64,\"browser_download_url\":\"" BASE "v1.2.3/release.json.sig\"}]}");
    TEST_CHECK(update_release_parse_api(dup, strlen(dup), REPO, 0x400000u, &r) == UPDATE_REL_E_DUP_ASSET,
               "duplicate signature asset refused");
    char bad_url[1024];
    snprintf(bad_url, sizeof(bad_url), "%s", with_sig);
    p = strstr(bad_url, "v1.2.3/release.json.sig");
    p[0] = 'v';
    p[1] = '9';
    TEST_CHECK(update_release_parse_api(bad_url, strlen(bad_url), REPO, 0x400000u, &r) == UPDATE_REL_E_BAD_URL,
               "signature asset url pinned to repo/tag/name");
}

void run_test_update_sign(void)
{
    test_verify();
    test_enforcement();
    test_sig_asset_pick();
}
