// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). ota_http.c's own
// header comment (hmac_sha256()) explains why it uses the PSA Crypto API
// rather than mbedtls_md_hmac*(): that classic API is compiled out of this
// vendored mbedtls 4.x/TF-PSA-Crypto build.
//
// This is a FAKE, deterministic stand-in, not a real HMAC/SHA-256
// implementation -- and that is the right choice here, not a shortcut: the
// decisions test_ota_http.c actually verifies (the pw_len==0 refusal fires
// before any MAC math runs, each ota_http_context_t compares against its own
// distinct key/context, a wrong MAC is rejected) do not depend on the
// cryptographic strength of the primitive, only on whether the SAME inputs
// produce the SAME output and DIFFERENT inputs (key or message) produce a
// DIFFERENT one -- exactly what psa_mac_compute_fake() below guarantees
// (every output byte depends on every input byte, so changing the context
// string, key, or nonce changes the digest). Do not use this to establish
// "the real crypto is correct" -- it isn't real crypto.
#ifndef TEST_STUB_PSA_CRYPTO_H
#define TEST_STUB_PSA_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int32_t psa_status_t;
#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-132)

typedef struct { int dummy; } psa_key_attributes_t;
typedef uint32_t mbedtls_svc_key_id_t;

typedef uint32_t psa_key_usage_t;
#define PSA_KEY_USAGE_SIGN_MESSAGE ((psa_key_usage_t)0x00000400)

typedef uint32_t psa_algorithm_t;
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
#define PSA_ALG_HMAC(hash_alg) ((psa_algorithm_t)(0x03800000 | ((hash_alg) & 0x000000ffu)))

typedef uint32_t psa_key_type_t;
#define PSA_KEY_TYPE_HMAC ((psa_key_type_t)0x1100)

static inline psa_key_attributes_t psa_key_attributes_init(void)
{
    psa_key_attributes_t a;
    memset(&a, 0, sizeof(a));
    return a;
}
static inline void psa_set_key_usage_flags(psa_key_attributes_t *a, psa_key_usage_t u) { (void)a; (void)u; }
static inline void psa_set_key_algorithm(psa_key_attributes_t *a, psa_algorithm_t alg) { (void)a; (void)alg; }
static inline void psa_set_key_type(psa_key_attributes_t *a, psa_key_type_t t) { (void)a; (void)t; }

// Test-controllable fault injection -- unused by default (every test that
// doesn't care about a crypto-library failure leaves this at PSA_SUCCESS).
extern psa_status_t g_stub_psa_import_key_result;

// Fake key store: the imported key's bytes are copied into a small
// process-wide slot, keyed by an incrementing id -- enough for this file's
// single-key-at-a-time usage (hmac_sha256() imports, uses, destroys one key
// per call, never overlapping).
#define PSA_STUB_KEY_MAX 128
typedef struct {
    bool used;
    uint8_t bytes[PSA_STUB_KEY_MAX];
    size_t len;
} psa_stub_key_slot_t;

static psa_stub_key_slot_t g_psa_stub_keys[4];

static inline psa_status_t psa_import_key(const psa_key_attributes_t *attr, const uint8_t *data,
                                           size_t data_len, mbedtls_svc_key_id_t *out_key)
{
    (void)attr;
    if (g_stub_psa_import_key_result != PSA_SUCCESS) {
        return g_stub_psa_import_key_result;
    }
    for (uint32_t i = 0; i < 4; i++) {
        if (!g_psa_stub_keys[i].used) {
            g_psa_stub_keys[i].used = true;
            size_t len = data_len > PSA_STUB_KEY_MAX ? PSA_STUB_KEY_MAX : data_len;
            memcpy(g_psa_stub_keys[i].bytes, data, len);
            g_psa_stub_keys[i].len = len;
            *out_key = i + 1;
            return PSA_SUCCESS;
        }
    }
    return PSA_ERROR_GENERIC_ERROR; /* stub table full -- never hit by this file's tests */
}

static inline psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key)
{
    if (key >= 1 && key <= 4) {
        g_psa_stub_keys[key - 1].used = false;
    }
    return PSA_SUCCESS;
}

// Deterministic, order-sensitive fake MAC: every output byte is a function
// of every key byte, every message byte, and its own position -- a simple
// (not cryptographically real) mixing so that changing the key, the
// message, or their order changes every output byte, which is what
// test_ota_http.c's "same nonce+different context must produce different
// digests" and "wrong password must be rejected" assertions rely on.
static inline void psa_stub_fake_mac(const uint8_t *key, size_t key_len, const uint8_t *msg,
                                      size_t msg_len, uint8_t out[32])
{
    uint32_t acc[8];
    for (int i = 0; i < 8; i++) {
        acc[i] = 0x9E3779B9u * (uint32_t)(i + 1);
    }
    for (size_t i = 0; i < key_len; i++) {
        acc[i % 8] = (acc[i % 8] * 33u) ^ (key[i] + i);
    }
    /* Key length itself is mixed in -- otherwise a zero-length key and a
     * key of all-zero bytes of some other length could coincide for a
     * short message, which would make the "empty AP password" refusal test
     * meaningless (it must be refused OUTRIGHT before this function ever
     * runs -- see ota_http.c's ota_http_verify_request()). */
    acc[0] ^= (uint32_t)key_len * 0x85EBCA6Bu;
    for (size_t i = 0; i < msg_len; i++) {
        acc[i % 8] = (acc[i % 8] * 33u) ^ (msg[i] + i * 7u);
    }
    for (int i = 0; i < 8; i++) {
        acc[i] ^= acc[(i + 1) % 8];
        out[i * 4 + 0] = (uint8_t)(acc[i] & 0xFF);
        out[i * 4 + 1] = (uint8_t)((acc[i] >> 8) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((acc[i] >> 16) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((acc[i] >> 24) & 0xFF);
    }
}

static inline psa_status_t psa_mac_compute(mbedtls_svc_key_id_t key, psa_algorithm_t alg,
                                            const uint8_t *input, size_t input_length, uint8_t *mac,
                                            size_t mac_size, size_t *mac_length)
{
    (void)alg;
    if (mac_size < 32 || key < 1 || key > 4 || !g_psa_stub_keys[key - 1].used) {
        return PSA_ERROR_GENERIC_ERROR;
    }
    psa_stub_fake_mac(g_psa_stub_keys[key - 1].bytes, g_psa_stub_keys[key - 1].len, input, input_length, mac);
    if (mac_length) *mac_length = 32;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

// --- Hashing (only used by ota_http.c's transfer-streaming paths, never
// reached by test_ota_http.c's tests -- present purely so the whole
// translation unit links). ---
typedef struct { int dummy; } psa_hash_operation_t;
static inline psa_hash_operation_t psa_hash_operation_init(void)
{
    psa_hash_operation_t o;
    memset(&o, 0, sizeof(o));
    return o;
}
static inline psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    (void)op; (void)alg;
    return PSA_SUCCESS;
}
static inline psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *input, size_t input_length)
{
    (void)op; (void)input; (void)input_length;
    return PSA_SUCCESS;
}
static inline psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *hash, size_t hash_size,
                                            size_t *hash_length)
{
    (void)op;
    if (hash_size >= 32) memset(hash, 0, 32);
    if (hash_length) *hash_length = 32;
    return PSA_SUCCESS;
}
static inline psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    (void)op;
    return PSA_SUCCESS;
}

#endif // TEST_STUB_PSA_CRYPTO_H
