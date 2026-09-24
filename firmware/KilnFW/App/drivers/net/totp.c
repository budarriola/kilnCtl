// totp.c -- see totp.h for the design rationale (hand-rolled SHA-1/HMAC,
// no ESP-IDF/mbedtls/PSA dependency).
#include "totp.h"

#include <stdio.h>
#include <string.h>

// --- SHA-1 (RFC 3174) -------------------------------------------------------

typedef struct {
    uint32_t h[5];
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
} totp_sha1_ctx_t;

static uint32_t sha1_rotl(uint32_t v, unsigned bits)
{
    return (v << bits) | (v >> (32u - bits));
}

static void sha1_init(totp_sha1_ctx_t *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

static void sha1_process_block(totp_sha1_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 80; ++i) {
        w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];

    for (unsigned i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotl(b, 30);
        b = a;
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_update(totp_sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->total_len += len;
    while (len > 0) {
        size_t take = 64 - ctx->buf_len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;
        if (ctx->buf_len == 64) {
            sha1_process_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha1_final(totp_sha1_ctx_t *ctx, uint8_t out[TOTP_SHA1_DIGEST_LEN])
{
    uint64_t bit_len = ctx->total_len * 8u;
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);
    // sha1_update() above already added 1 to total_len; recompute pad target
    // against the length BEFORE that call, which bit_len already captured.
    uint8_t zero = 0x00;
    while (ctx->buf_len != 56) {
        sha1_update(ctx, &zero, 1);
    }
    uint8_t len_bytes[8];
    for (unsigned i = 0; i < 8; ++i) {
        len_bytes[i] = (uint8_t)(bit_len >> (56u - 8u * i));
    }
    // Append length directly into the block buffer without recursing through
    // sha1_update()'s total_len accounting (irrelevant past this point).
    memcpy(ctx->buf + ctx->buf_len, len_bytes, 8);
    ctx->buf_len += 8;
    sha1_process_block(ctx, ctx->buf);
    ctx->buf_len = 0;

    for (unsigned i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

void totp_sha1(const uint8_t *data, size_t len, uint8_t out[TOTP_SHA1_DIGEST_LEN])
{
    totp_sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

// --- HMAC-SHA1 (RFC 2104) ---------------------------------------------------

#define SHA1_BLOCK_LEN 64u

void totp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[TOTP_SHA1_DIGEST_LEN])
{
    uint8_t block_key[SHA1_BLOCK_LEN];
    memset(block_key, 0, sizeof(block_key));

    if (key_len > SHA1_BLOCK_LEN) {
        totp_sha1(key, key_len, block_key); // digest occupies first 20 bytes, rest stays 0
    } else {
        memcpy(block_key, key, key_len);
    }

    uint8_t ipad[SHA1_BLOCK_LEN];
    uint8_t opad[SHA1_BLOCK_LEN];
    for (unsigned i = 0; i < SHA1_BLOCK_LEN; ++i) {
        ipad[i] = block_key[i] ^ 0x36u;
        opad[i] = block_key[i] ^ 0x5Cu;
    }

    totp_sha1_ctx_t ctx;
    uint8_t inner_digest[TOTP_SHA1_DIGEST_LEN];
    sha1_init(&ctx);
    sha1_update(&ctx, ipad, sizeof(ipad));
    sha1_update(&ctx, msg, msg_len);
    sha1_final(&ctx, inner_digest);

    sha1_init(&ctx);
    sha1_update(&ctx, opad, sizeof(opad));
    sha1_update(&ctx, inner_digest, sizeof(inner_digest));
    sha1_final(&ctx, out);
}

// --- HOTP dynamic truncation (RFC 4226 section 5.3) -------------------------

static uint32_t pow10u(unsigned n)
{
    uint32_t v = 1;
    for (unsigned i = 0; i < n; ++i) v *= 10u;
    return v;
}

uint32_t totp_hotp_truncate(const uint8_t *secret, size_t secret_len, uint64_t counter,
                             unsigned digits)
{
    uint8_t counter_bytes[8];
    for (unsigned i = 0; i < 8; ++i) {
        counter_bytes[i] = (uint8_t)(counter >> (56u - 8u * i));
    }

    uint8_t hs[TOTP_SHA1_DIGEST_LEN];
    totp_hmac_sha1(secret, secret_len, counter_bytes, sizeof(counter_bytes), hs);

    unsigned offset = hs[TOTP_SHA1_DIGEST_LEN - 1] & 0x0Fu;
    uint32_t p = ((uint32_t)(hs[offset] & 0x7Fu) << 24) | ((uint32_t)hs[offset + 1] << 16) |
                 ((uint32_t)hs[offset + 2] << 8) | (uint32_t)hs[offset + 3];

    return p % pow10u(digits);
}

// --- Constant-time compare ---------------------------------------------------

bool totp_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// --- Verification with replay guard + window --------------------------------

static bool parse_fixed_digits(const char *code, unsigned digits, uint32_t *out_value)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < digits; ++i) {
        char c = code[i];
        if (c < '0' || c > '9') return false;
        v = v * 10u + (uint32_t)(c - '0');
    }
    if (code[digits] != '\0') return false; // must be exactly `digits` long, no trailing junk
    *out_value = v;
    return true;
}

bool totp_verify(const uint8_t *secret, size_t secret_len, const char *code,
                  uint64_t unix_time_s, uint64_t last_accepted_counter,
                  uint64_t *out_matched_counter)
{
    uint32_t submitted;
    if (!parse_fixed_digits(code, TOTP_DIGITS, &submitted)) {
        return false;
    }

    uint64_t center = totp_counter_for_time(unix_time_s);

    // Current step first, then the +/-1 skew steps -- order does not affect
    // correctness (every in-window candidate not already used is checked),
    // just which one is preferred when more than one hypothetically matches
    // (they never will for a fixed secret+time, since HOTP truncation is a
    // deterministic function of the counter).
    int64_t deltas[3] = {0, -1, 1};
    for (unsigned i = 0; i < 3; ++i) {
        int64_t delta = deltas[i];
        if (delta < 0 && center < (uint64_t)(-delta)) {
            continue; // would underflow counter 0 near the epoch; skip
        }
        uint64_t candidate = (uint64_t)((int64_t)center + delta);

        if (candidate <= last_accepted_counter) {
            continue; // replay: never compare an already-used or earlier step
        }

        uint32_t expected = totp_hotp_truncate(secret, secret_len, candidate, TOTP_DIGITS);

        // Compare as fixed-width decimal strings so the comparison touches a
        // constant number of bytes regardless of the numeric value.
        char expected_str[TOTP_DIGITS + 1];
        for (unsigned d = 0; d < TOTP_DIGITS; ++d) {
            uint32_t divisor = pow10u(TOTP_DIGITS - 1u - d);
            expected_str[d] = (char)('0' + (expected / divisor) % 10u);
        }
        expected_str[TOTP_DIGITS] = '\0';

        if (totp_constant_time_equal((const uint8_t *)code, (const uint8_t *)expected_str,
                                      TOTP_DIGITS)) {
            if (out_matched_counter) *out_matched_counter = candidate;
            return true;
        }
    }

    return false;
}

// --- Base32 (RFC 4648 section 6, no padding) --------------------------------

static const char BASE32_ALPHABET[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t totp_base32_encode(const uint8_t *data, size_t len, char *out, size_t out_cap)
{
    size_t out_len = (len * 8u + 4u) / 5u; // ceil(len*8/5)
    if (out_cap < out_len + 1u) return 0;

    size_t bit_buf = 0;
    unsigned bit_count = 0;
    size_t out_i = 0;

    for (size_t i = 0; i < len; ++i) {
        bit_buf = (bit_buf << 8) | data[i];
        bit_count += 8;
        while (bit_count >= 5) {
            bit_count -= 5;
            out[out_i++] = BASE32_ALPHABET[(bit_buf >> bit_count) & 0x1Fu];
        }
    }
    if (bit_count > 0) {
        out[out_i++] = BASE32_ALPHABET[(bit_buf << (5u - bit_count)) & 0x1Fu];
    }
    out[out_i] = '\0';
    return out_i;
}

static int base32_char_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

size_t totp_base32_decode(const char *text, uint8_t *out, size_t out_cap)
{
    size_t bit_buf = 0;
    unsigned bit_count = 0;
    size_t out_i = 0;

    for (const char *p = text; *p; ++p) {
        if (*p == '=') continue;
        int v = base32_char_value(*p);
        if (v < 0) return 0; // malformed
        bit_buf = (bit_buf << 5) | (unsigned)v;
        bit_count += 5;
        if (bit_count >= 8) {
            bit_count -= 8;
            if (out_i >= out_cap) return 0;
            out[out_i++] = (uint8_t)((bit_buf >> bit_count) & 0xFFu);
        }
    }
    return out_i;
}

// --- otpauth:// URI ----------------------------------------------------------

size_t totp_build_otpauth_uri(const char *username, const uint8_t *secret, size_t secret_len,
                               char *out, size_t out_cap)
{
    char b32[64];
    size_t b32_len = totp_base32_encode(secret, secret_len, b32, sizeof(b32));
    if (b32_len == 0) return 0;

    int written = snprintf(out, out_cap,
                            "otpauth://totp/kilnCtl:%s?secret=%s&issuer=kilnCtl&algorithm=SHA1"
                            "&digits=%u&period=%u",
                            username ? username : "admin", b32, TOTP_DIGITS, TOTP_PERIOD_S);
    if (written < 0 || (size_t)written >= out_cap) return 0;
    return (size_t)written;
}
