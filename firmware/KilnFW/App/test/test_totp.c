// Host tests for App/drivers/net/totp.c -- RFC 6238 Appendix B vectors,
// the +/-1 step window, replay-guard refusal, constant-time compare, and
// base32 round trip. No ESP-IDF dependency (see totp.h's header comment on
// why this module hand-rolls SHA-1/HMAC rather than going through PSA).
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/net/totp.h"

// RFC 6238 Appendix B's fixed test secret: the ASCII string
// "12345678901234567890" (20 bytes) for the SHA1 vectors.
static const uint8_t RFC6238_SHA1_SECRET[20] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
};

static void test_sha1_known_vector(void)
{
    TEST_SECTION("totp_sha1 -- known vector");

    // FIPS 180-1 / RFC 3174's own worked example: SHA1("abc").
    uint8_t out[TOTP_SHA1_DIGEST_LEN];
    totp_sha1((const uint8_t *)"abc", 3, out);
    uint8_t expected[TOTP_SHA1_DIGEST_LEN] = {
        0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
        0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D,
    };
    TEST_CHECK(memcmp(out, expected, sizeof(expected)) == 0, "SHA1(\"abc\") matches the FIPS 180-1 example");

    // Empty-input vector.
    totp_sha1((const uint8_t *)"", 0, out);
    uint8_t expected_empty[TOTP_SHA1_DIGEST_LEN] = {
        0xDA, 0x39, 0xA3, 0xEE, 0x5E, 0x6B, 0x4B, 0x0D, 0x32, 0x55,
        0xBF, 0xEF, 0x95, 0x60, 0x18, 0x90, 0xAF, 0xD8, 0x07, 0x09,
    };
    TEST_CHECK(memcmp(out, expected_empty, sizeof(expected_empty)) == 0, "SHA1(\"\") matches the standard vector");

    // Padding/multi-block boundaries: SHA1('a' * n) for n straddling the
    // 55/56 (length field no longer fits) and 64 (exact block) edges, plus
    // two- and three-block inputs. Expected digests from Python hashlib.
    static const struct {
        size_t n;
        uint8_t digest[TOTP_SHA1_DIGEST_LEN];
    } boundary[] = {
        {55, {0xC1, 0xC8, 0xBB, 0xDC, 0x22, 0x79, 0x6E, 0x28, 0xC0, 0xE1,
              0x51, 0x63, 0xD2, 0x08, 0x99, 0xB6, 0x56, 0x21, 0xD6, 0x5A}},
        {56, {0xC2, 0xDB, 0x33, 0x0F, 0x60, 0x83, 0x85, 0x4C, 0x99, 0xD4,
              0xB5, 0xBF, 0xB6, 0xE8, 0xF2, 0x9F, 0x20, 0x1B, 0xE6, 0x99}},
        {63, {0x03, 0xF0, 0x9F, 0x5B, 0x15, 0x8A, 0x7A, 0x8C, 0xDA, 0xD9,
              0x20, 0xBD, 0xDC, 0x29, 0xB8, 0x1C, 0x18, 0xA5, 0x51, 0xF5}},
        {64, {0x00, 0x98, 0xBA, 0x82, 0x4B, 0x5C, 0x16, 0x42, 0x7B, 0xD7,
              0xA1, 0x12, 0x2A, 0x5A, 0x44, 0x2A, 0x25, 0xEC, 0x64, 0x4D}},
        {65, {0x11, 0x65, 0x53, 0x26, 0xC7, 0x08, 0xD7, 0x03, 0x19, 0xBE,
              0x26, 0x10, 0xE8, 0xA5, 0x7D, 0x9A, 0x5B, 0x95, 0x9D, 0x3B}},
        {119, {0xEE, 0x97, 0x10, 0x65, 0xAA, 0xA0, 0x17, 0xE0, 0x63, 0x2A,
               0x8C, 0xA6, 0xC7, 0x7B, 0xB3, 0xBF, 0x8B, 0x1D, 0xFC, 0x56}},
        {120, {0xF3, 0x4C, 0x14, 0x88, 0x38, 0x53, 0x46, 0xA5, 0x57, 0x09,
               0xBA, 0x05, 0x6D, 0xDD, 0x08, 0x28, 0x0D, 0xD4, 0xC6, 0xD6}},
        {128, {0xAD, 0x5B, 0x3F, 0xDB, 0xCB, 0x52, 0x67, 0x78, 0xC2, 0x83,
               0x9D, 0x2F, 0x15, 0x1E, 0xA7, 0x53, 0x99, 0x5E, 0x26, 0xA0}},
        {130, {0xE1, 0xCD, 0x43, 0x7E, 0xC3, 0xE8, 0xA6, 0x0D, 0xB3, 0x4E,
               0x1D, 0x15, 0x0A, 0x4F, 0xC7, 0x38, 0x82, 0xD8, 0x3B, 0x41}},
    };
    uint8_t long_input[130];
    memset(long_input, 'a', sizeof(long_input));
    for (size_t i = 0; i < sizeof(boundary) / sizeof(boundary[0]); i++) {
        totp_sha1(long_input, boundary[i].n, out);
        char msg[96];
        snprintf(msg, sizeof(msg), "SHA1('a' x %u) matches hashlib", (unsigned)boundary[i].n);
        TEST_CHECK(memcmp(out, boundary[i].digest, TOTP_SHA1_DIGEST_LEN) == 0, msg);
    }
}

// RFC 2202's HMAC-SHA1 test case 1: key = 20 bytes of 0x0b, data = "Hi There".
static void test_hmac_sha1_known_vector(void)
{
    TEST_SECTION("totp_hmac_sha1 -- RFC 2202 case 1");

    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    uint8_t out[TOTP_SHA1_DIGEST_LEN];
    totp_hmac_sha1(key, sizeof(key), (const uint8_t *)"Hi There", 8, out);

    uint8_t expected[TOTP_SHA1_DIGEST_LEN] = {
        0xB6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xE2, 0x8B,
        0xC0, 0xB6, 0xFB, 0x37, 0x8C, 0x8E, 0xF1, 0x46, 0xBE, 0x00,
    };
    TEST_CHECK(memcmp(out, expected, sizeof(expected)) == 0,
               "HMAC-SHA1(key=20x0x0b, \"Hi There\") matches RFC 2202 test case 1");

    // RFC 2202 case 6: an 80-byte key (> block size) is hashed first.
    uint8_t key80[80];
    memset(key80, 0xAA, sizeof(key80));
    const char *msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    totp_hmac_sha1(key80, sizeof(key80), (const uint8_t *)msg6, strlen(msg6), out);
    static const uint8_t expected6[TOTP_SHA1_DIGEST_LEN] = {
        0xAA, 0x4A, 0xE5, 0xE1, 0x52, 0x72, 0xD0, 0x0E, 0x95, 0x70,
        0x56, 0x37, 0xCE, 0x8A, 0x3B, 0x55, 0xED, 0x40, 0x21, 0x12,
    };
    TEST_CHECK(memcmp(out, expected6, sizeof(expected6)) == 0,
               "HMAC-SHA1 with an 80-byte key matches RFC 2202 test case 6");

    // Key exactly 64 bytes (used as-is, not hashed) with a 100-byte message,
    // and a 65-byte key (hashed) with a 56-byte message. Expected values
    // from Python hmac/hashlib.
    uint8_t key65[65];
    for (unsigned i = 0; i < sizeof(key65); i++) key65[i] = (uint8_t)i;
    uint8_t msg100[100];
    memset(msg100, 'x', sizeof(msg100));
    totp_hmac_sha1(key65, 64, msg100, sizeof(msg100), out);
    static const uint8_t expected64[TOTP_SHA1_DIGEST_LEN] = {
        0xC2, 0x9D, 0xF2, 0x43, 0xDE, 0x87, 0x03, 0x7F, 0x09, 0xB9,
        0x11, 0xA4, 0x1D, 0x79, 0x10, 0x05, 0x14, 0x85, 0x6A, 0xAB,
    };
    TEST_CHECK(memcmp(out, expected64, sizeof(expected64)) == 0,
               "HMAC-SHA1 with a 64-byte key (exactly one block) matches hashlib");

    uint8_t msg56[56];
    memset(msg56, 'y', sizeof(msg56));
    totp_hmac_sha1(key65, sizeof(key65), msg56, sizeof(msg56), out);
    static const uint8_t expected65[TOTP_SHA1_DIGEST_LEN] = {
        0x95, 0x40, 0x3E, 0x6C, 0xE4, 0x98, 0x10, 0x5B, 0x42, 0xEA,
        0x4B, 0x90, 0xE0, 0x99, 0x39, 0xC4, 0xF7, 0x03, 0xEB, 0x9B,
    };
    TEST_CHECK(memcmp(out, expected65, sizeof(expected65)) == 0,
               "HMAC-SHA1 with a 65-byte key (hashed first) matches hashlib");
}

typedef struct {
    uint64_t unix_time_s;
    uint32_t expected_code; // as an 8-digit RFC 6238 Appendix B value
} rfc6238_vector_t;

static void test_rfc6238_appendix_b_vectors(void)
{
    TEST_SECTION("totp_hotp_truncate -- RFC 6238 Appendix B (SHA1, 8 digits)");

    // Published table (SHA1 column only -- this board never uses SHA256/512).
    static const rfc6238_vector_t vectors[] = {
        {59, 94287082u},
        {1111111109, 7081804u},
        {1111111111, 14050471u},
        {1234567890, 89005924u},
        {2000000000, 69279037u},
        {20000000000ull, 65353130u},
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint64_t counter = totp_counter_for_time(vectors[i].unix_time_s);
        uint32_t code = totp_hotp_truncate(RFC6238_SHA1_SECRET, sizeof(RFC6238_SHA1_SECRET),
                                            counter, 8);
        char msg[128];
        snprintf(msg, sizeof(msg), "time=%llu -> counter=%llu expects %08u, got %08u",
                 (unsigned long long)vectors[i].unix_time_s, (unsigned long long)counter,
                 vectors[i].expected_code, code);
        TEST_CHECK(code == vectors[i].expected_code, msg);
    }
}

static void test_totp_verify_window_and_current_code(void)
{
    TEST_SECTION("totp_verify -- +/-1 step window, this board's 6-digit codes");

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x42, sizeof(secret)); // arbitrary fixed secret for this test's own math

    uint64_t now = 1000000000; // arbitrary, well clear of any counter-0 underflow edge
    uint64_t counter = totp_counter_for_time(now);

    uint32_t code_now = totp_hotp_code(secret, sizeof(secret), counter);
    uint32_t code_prev = totp_hotp_code(secret, sizeof(secret), counter - 1);
    uint32_t code_next = totp_hotp_code(secret, sizeof(secret), counter + 1);
    uint32_t code_far = totp_hotp_code(secret, sizeof(secret), counter + 2); // outside window

    char buf[16];
    uint64_t matched;

    snprintf(buf, sizeof(buf), "%06u", code_now);
    TEST_CHECK(totp_verify(secret, sizeof(secret), buf, now, 0, &matched) && matched == counter,
               "current-step code accepted, matched counter reported correctly");

    snprintf(buf, sizeof(buf), "%06u", code_prev);
    TEST_CHECK(totp_verify(secret, sizeof(secret), buf, now, 0, &matched) && matched == counter - 1,
               "one step behind (clock skew) accepted within the +/-1 window");

    snprintf(buf, sizeof(buf), "%06u", code_next);
    TEST_CHECK(totp_verify(secret, sizeof(secret), buf, now, 0, &matched) && matched == counter + 1,
               "one step ahead accepted within the +/-1 window");

    snprintf(buf, sizeof(buf), "%06u", code_far);
    TEST_CHECK(!totp_verify(secret, sizeof(secret), buf, now, 0, NULL),
               "two steps ahead is outside the window -- refused");
}

static void test_totp_verify_replay_guard(void)
{
    TEST_SECTION("totp_verify -- replay guard");

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x99, sizeof(secret));

    uint64_t now = 2000000000;
    uint64_t counter = totp_counter_for_time(now);
    uint32_t code_now = totp_hotp_code(secret, sizeof(secret), counter);

    char buf[16];
    snprintf(buf, sizeof(buf), "%06u", code_now);

    uint64_t matched = 0;
    TEST_CHECK(totp_verify(secret, sizeof(secret), buf, now, 0, &matched) && matched == counter,
               "first use of this counter is accepted");

    // Same code, same time, but the caller now reports it as already
    // accepted (last_accepted_counter == the matched counter) -- must be
    // refused even though the HMAC math would otherwise match.
    TEST_CHECK(!totp_verify(secret, sizeof(secret), buf, now, matched, NULL),
               "replaying the same counter is refused once it is the last-accepted counter");

    // A code for an EARLIER counter than last_accepted must also be
    // refused, even within the nominal +/-1 window -- a used counter never
    // becomes valid again by re-presenting it a second later.
    uint32_t code_prev = totp_hotp_code(secret, sizeof(secret), counter - 1);
    snprintf(buf, sizeof(buf), "%06u", code_prev);
    TEST_CHECK(!totp_verify(secret, sizeof(secret), buf, now, matched, NULL),
               "an earlier counter than last-accepted is refused, not just an exact repeat");

    // Monotonic across the window: once the +1 (newer) step is accepted,
    // the current step's code -- one step back, still inside the window --
    // must be refused.
    uint32_t code_next = totp_hotp_code(secret, sizeof(secret), counter + 1);
    snprintf(buf, sizeof(buf), "%06u", code_next);
    uint64_t matched_next = 0;
    TEST_CHECK(totp_verify(secret, sizeof(secret), buf, now, 0, &matched_next) &&
                   matched_next == counter + 1,
               "the +1 step's code is accepted first");
    snprintf(buf, sizeof(buf), "%06u", code_now);
    TEST_CHECK(!totp_verify(secret, sizeof(secret), buf, now, matched_next, NULL),
               "after the +1 step was accepted, the current step (one back) is refused");
}

static void test_totp_verify_rejects_malformed_code(void)
{
    TEST_SECTION("totp_verify -- malformed code input");

    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x11, sizeof(secret));
    uint64_t now = 1500000000;

    TEST_CHECK(!totp_verify(secret, sizeof(secret), "12345", now, 0, NULL),
               "5 digits (too short) is refused, never zero-padded/reinterpreted");
    TEST_CHECK(!totp_verify(secret, sizeof(secret), "1234567", now, 0, NULL),
               "7 digits (too long) is refused");
    TEST_CHECK(!totp_verify(secret, sizeof(secret), "12345a", now, 0, NULL),
               "a non-digit character is refused");
    TEST_CHECK(!totp_verify(secret, sizeof(secret), "", now, 0, NULL),
               "an empty string is refused");
    TEST_CHECK(!totp_verify(secret, sizeof(secret), NULL, now, 0, NULL),
               "a NULL code is refused, not dereferenced");
}

static void test_constant_time_equal(void)
{
    TEST_SECTION("totp_constant_time_equal");

    uint8_t a[6] = {'1', '2', '3', '4', '5', '6'};
    uint8_t b[6] = {'1', '2', '3', '4', '5', '6'};
    TEST_CHECK(totp_constant_time_equal(a, b, 6), "identical buffers compare equal");

    b[5] = '7';
    TEST_CHECK(!totp_constant_time_equal(a, b, 6), "single differing trailing byte -- not equal");

    uint8_t c[6] = {'0', '2', '3', '4', '5', '6'};
    TEST_CHECK(!totp_constant_time_equal(a, c, 6), "single differing leading byte -- not equal");
}

static void test_base32_round_trip(void)
{
    TEST_SECTION("totp_base32_encode / totp_base32_decode -- round trip");

    // RFC 4648 section 10's own worked examples (unpadded).
    struct {
        const char *input;
        const char *expected_b32;
    } cases[] = {
        {"", ""},
        {"f", "MY"},
        {"fo", "MZXQ"},
        {"foo", "MZXW6"},
        {"foob", "MZXW6YQ"},
        {"fooba", "MZXW6YTB"},
        {"foobar", "MZXW6YTBOI"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char encoded[64];
        size_t len = totp_base32_encode((const uint8_t *)cases[i].input, strlen(cases[i].input),
                                         encoded, sizeof(encoded));
        char msg[128];
        snprintf(msg, sizeof(msg), "encode(\"%s\") == \"%s\"", cases[i].input, cases[i].expected_b32);
        TEST_CHECK(len == strlen(cases[i].expected_b32) && strcmp(encoded, cases[i].expected_b32) == 0,
                   msg);
    }

    // Round trip a realistic 20-byte secret.
    uint8_t secret[TOTP_SECRET_LEN];
    for (unsigned i = 0; i < TOTP_SECRET_LEN; i++) secret[i] = (uint8_t)(i * 13 + 7);

    char encoded[64];
    size_t enc_len = totp_base32_encode(secret, sizeof(secret), encoded, sizeof(encoded));
    TEST_CHECK(enc_len > 0, "encoding a 20-byte secret succeeds");

    uint8_t decoded[TOTP_SECRET_LEN];
    size_t dec_len = totp_base32_decode(encoded, decoded, sizeof(decoded));
    TEST_CHECK(dec_len == TOTP_SECRET_LEN && memcmp(decoded, secret, TOTP_SECRET_LEN) == 0,
               "decoding the encoded form reproduces the original secret exactly");

    // Malformed input (an invalid alphabet character) is rejected, not
    // silently skipped/truncated.
    uint8_t discard[16];
    TEST_CHECK(totp_base32_decode("MZXW6YTB!!", discard, sizeof(discard)) == 0,
               "an invalid base32 character makes decode report failure (0)");

    // '=' only as trailing padding (same rule as PcTools totp.py).
    TEST_CHECK(totp_base32_decode("MZXW6===", discard, sizeof(discard)) == 3 &&
                   memcmp(discard, "foo", 3) == 0,
               "trailing '=' padding is accepted");
    TEST_CHECK(totp_base32_decode("MZ=XW6", discard, sizeof(discard)) == 0,
               "'=' between alphabet characters is malformed");
}

static void test_otpauth_uri(void)
{
    TEST_SECTION("totp_build_otpauth_uri");

    uint8_t secret[TOTP_SECRET_LEN];
    for (unsigned i = 0; i < TOTP_SECRET_LEN; i++) secret[i] = (uint8_t)(i + 1);

    char uri[256];
    size_t len = totp_build_otpauth_uri("admin", secret, sizeof(secret), uri, sizeof(uri));
    TEST_CHECK(len > 0 && len == strlen(uri), "returns the actual written length");
    TEST_CHECK(strstr(uri, "otpauth://totp/kilnCtl:admin?") == uri, "URI starts with the fixed scheme/label");
    TEST_CHECK(strstr(uri, "algorithm=SHA1") != NULL, "algorithm is pinned to SHA1");
    TEST_CHECK(strstr(uri, "digits=6") != NULL, "digits is pinned to 6");
    TEST_CHECK(strstr(uri, "period=30") != NULL, "period is pinned to 30");
    TEST_CHECK(strstr(uri, "issuer=kilnCtl") != NULL, "issuer is present");
    // Exact shape from docs/TOTP_PASSWORD_RESET_PLAN.md section 1; the
    // base32 of bytes 1..20 is from Python base64.b32encode.
    TEST_CHECK(strcmp(uri, "otpauth://totp/kilnCtl:admin?secret=AEBAGBAFAYDQQCIKBMGA2DQPCAIREEYU"
                           "&issuer=kilnCtl&algorithm=SHA1&digits=6&period=30") == 0,
               "URI matches the plan's exact otpauth shape byte for byte");

    // Too-small buffer must fail rather than overflow/truncate silently.
    char tiny[8];
    TEST_CHECK(totp_build_otpauth_uri("admin", secret, sizeof(secret), tiny, sizeof(tiny)) == 0,
               "an undersized output buffer reports failure (0), never a truncated URI");
}

void run_test_totp(void)
{
    test_sha1_known_vector();
    test_hmac_sha1_known_vector();
    test_rfc6238_appendix_b_vectors();
    test_totp_verify_window_and_current_code();
    test_totp_verify_replay_guard();
    test_totp_verify_rejects_malformed_code();
    test_constant_time_equal();
    test_base32_round_trip();
    test_otpauth_uri();
}
