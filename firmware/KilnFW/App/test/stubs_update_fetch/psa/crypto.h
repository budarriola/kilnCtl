// update_fetch host test: PSA hash API backed by a real SHA-256 (the shared stub's hash is a fake of zeros,
// useless for a digest-compare path). Only the hash slice update_fetch.c / update_http.c use.
#ifndef UF_STUB_PSA_CRYPTO_H
#define UF_STUB_PSA_CRYPTO_H
#define TEST_STUB_PSA_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int32_t psa_status_t;
#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-132)
typedef uint32_t psa_algorithm_t;
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
#define PSA_HASH_LENGTH(alg) 32u

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t buflen;
    uint64_t total;
    int active;
} psa_hash_operation_t;

extern int g_fr_psa_setup_fail; // next psa_hash_setup() fails (then clears)

static inline psa_hash_operation_t psa_hash_operation_init(void)
{
    psa_hash_operation_t o;
    memset(&o, 0, sizeof(o));
    return o;
}
psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *input, size_t len);
psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *hash, size_t hash_size, size_t *hash_length);
psa_status_t psa_hash_abort(psa_hash_operation_t *op);
#endif
