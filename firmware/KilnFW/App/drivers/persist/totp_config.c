// totp_config.c -- see totp_config.h for the contract.
#include "totp_config.h"

#include <stddef.h>
#include <string.h>

#include "hal_kv.h"
#include "nvs_key_check.h"

// Same namespace as web_auth_store.c (kiln_auth) -- deliberate: TOTP is
// part of the same administrator-credential concept, gets the same
// backup/restore exclusion and the same factory_reset.c non-target
// treatment (see totp_config.h's header comment).
#define TOTP_NAMESPACE "kiln_auth"
#define TOTP_KEY_SECRET "totp_secret"     // 11 chars
#define TOTP_KEY_LAST_CTR "totp_last_ctr" // 13 chars
NVS_KEY_LEN_CHECK(TOTP_NAMESPACE);
NVS_KEY_LEN_CHECK(TOTP_KEY_SECRET);
NVS_KEY_LEN_CHECK(TOTP_KEY_LAST_CTR);

#define TOTP_SECRET_BLOB_VERSION 1u

// Layout is fully explicit -- no compiler-inserted padding: 4 (version) +
// 20 (secret) + 4 (reserved, always 0) = 28 bytes CRC-covered, crc32 at
// offset 28, 32 bytes total. The CRC covers the version field. The
// _Static_asserts pin this so a future field change cannot silently
// reintroduce an implicit padding byte (the earlier reserved[3] left one
// compiler-inserted byte inside the CRC-covered range).
typedef struct {
    uint32_t version;
    uint8_t  secret[TOTP_SECRET_LEN];
    uint8_t  reserved[4];
    uint32_t crc32;
} totp_secret_blob_t;
_Static_assert(offsetof(totp_secret_blob_t, crc32) == 28u, "totp_secret_blob_t: crc32 must follow 28 packed bytes");
_Static_assert(sizeof(totp_secret_blob_t) == 32u, "totp_secret_blob_t: no trailing padding expected");

// --- CRC32 (IEEE 802.3 / zlib polynomial), table-less -----------------------
// Identical construction to web_auth_store.c's crc32_compute() / boot_guard.c's
// -- deliberately re-derived here rather than shared via a common header,
// same as those two already do independently; see either file's host test
// for the "123456789" -> 0xCBF43926 vector.
static uint32_t crc32_compute(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t secret_blob_crc(const totp_secret_blob_t *b)
{
    return crc32_compute(b, offsetof(totp_secret_blob_t, crc32));
}

// --- Generic load/verify-write helpers, same discipline as web_auth_store.c -

static hal_status_t load_blob(const char *key, void *out, size_t out_len)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, TOTP_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL);
    if (err != HAL_OK) {
        return err;
    }
    size_t len = out_len;
    err = hal_kv_get_blob(&h, key, out, &len);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return err;
    }
    if (len != out_len) {
        return HAL_INVALID_SIZE;
    }
    return HAL_OK;
}

static hal_status_t set_blob_verified(const char *key, const void *buf, size_t len)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, TOTP_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, key, buf, len);
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return err;
    }

    uint8_t readback[64]; // both blobs used here are well under this
    if (len > sizeof(readback)) {
        return HAL_INVALID_SIZE;
    }
    hal_status_t rberr = load_blob(key, readback, len);
    bool same = (rberr == HAL_OK) && (memcmp(readback, buf, len) == 0);
    totp_secure_zero(readback, sizeof(readback));
    return same ? HAL_OK : HAL_VERIFY_FAILED;
}

static hal_status_t erase_key(const char *key)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, TOTP_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_erase_key(&h, key);
    // Erasing a key that was never set is not a failure for this module's
    // purposes -- totp_config_clear() must succeed against a never-enrolled
    // store (see header comment).
    if (err == HAL_OK || err == HAL_NOT_FOUND) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

// --- Secret ------------------------------------------------------------------

totp_config_load_status_t totp_config_load_secret(uint8_t out[TOTP_SECRET_LEN])
{
    totp_secret_blob_t blob;
    totp_config_load_status_t st;
    hal_status_t err = load_blob(TOTP_KEY_SECRET, &blob, sizeof(blob));
    if (err == HAL_NOT_FOUND) {
        st = TOTP_CONFIG_LOAD_ABSENT; // never enrolled -- safe default
    } else if (err != HAL_OK) {
        st = TOTP_CONFIG_LOAD_UNREADABLE; // read error / wrong size: fail closed
    } else if (blob.version != TOTP_SECRET_BLOB_VERSION) {
        st = TOTP_CONFIG_LOAD_UNREADABLE; // fail closed, never reinterpret
    } else if (secret_blob_crc(&blob) != blob.crc32) {
        st = TOTP_CONFIG_LOAD_UNREADABLE;
    } else {
        memcpy(out, blob.secret, TOTP_SECRET_LEN);
        st = TOTP_CONFIG_LOAD_OK;
    }
    totp_secure_zero(&blob, sizeof(blob));
    return st;
}

bool totp_config_enrolled(void)
{
    uint8_t secret[TOTP_SECRET_LEN];
    bool ok = totp_config_load_secret(secret) == TOTP_CONFIG_LOAD_OK;
    totp_secure_zero(secret, sizeof(secret));
    return ok;
}

bool totp_config_set_secret(const uint8_t secret[TOTP_SECRET_LEN])
{
    totp_secret_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = TOTP_SECRET_BLOB_VERSION;
    memcpy(blob.secret, secret, TOTP_SECRET_LEN);
    blob.crc32 = secret_blob_crc(&blob);

    hal_status_t err = set_blob_verified(TOTP_KEY_SECRET, &blob, sizeof(blob));
    totp_secure_zero(&blob, sizeof(blob));
    if (err != HAL_OK) {
        return false;
    }

    // A freshly-enrolled secret has never accepted a code -- reset the
    // replay counter to 0 so a stale counter from a previous enrollment
    // (e.g. re-enrolling after disabling) can never be inherited. This is
    // the one place both keys are written together, deliberately.
    if (!totp_config_set_last_counter(0)) {
        return false;
    }

    // Read-back verify the secret itself landed, never trust set_blob_verified
    // alone for the field content (same discipline as web_auth_store.c).
    uint8_t check[TOTP_SECRET_LEN];
    bool same = (totp_config_load_secret(check) == TOTP_CONFIG_LOAD_OK) &&
                (memcmp(check, secret, TOTP_SECRET_LEN) == 0);
    totp_secure_zero(check, sizeof(check));
    return same;
}

bool totp_config_clear(void)
{
    // Secret first, and stop if it fails: erasing the counter while the
    // secret survives would reset the replay guard for a still-live secret.
    hal_status_t secret_err = erase_key(TOTP_KEY_SECRET);
    if (secret_err != HAL_OK) {
        return false;
    }
    hal_status_t ctr_err = erase_key(TOTP_KEY_LAST_CTR);
    totp_config_ram_reset();
    if (ctr_err != HAL_OK) {
        return false;
    }

    // Read-back verify: both keys must now read ABSENT (not merely
    // "unreadable" -- an erase that did not take can still error on read).
    uint8_t discard_secret[TOTP_SECRET_LEN];
    if (totp_config_load_secret(discard_secret) != TOTP_CONFIG_LOAD_ABSENT) {
        totp_secure_zero(discard_secret, sizeof(discard_secret));
        return false;
    }
    uint32_t discard_ctr;
    return totp_config_load_last_counter(&discard_ctr) == TOTP_CONFIG_LOAD_ABSENT;
}

// --- Replay-guard counter ----------------------------------------------------

// RAM cache: s_ram_loaded distinguishes "not yet loaded this boot" from a
// loaded value. Only an OK or ABSENT load is ever cached; an UNREADABLE
// counter is never cached (and never collapsed to 0), so the next call
// retries NVS and the caller refuses the code meanwhile.
static bool s_ram_loaded = false;
static uint32_t s_ram_last_counter = 0;

totp_config_load_status_t totp_config_load_last_counter(uint32_t *out)
{
    uint32_t counter = 0;
    hal_status_t err = load_blob(TOTP_KEY_LAST_CTR, &counter, sizeof(counter));
    if (err == HAL_NOT_FOUND) {
        *out = 0; // never set -- 0 is the safe "never accepted" sentinel
        return TOTP_CONFIG_LOAD_ABSENT;
    }
    if (err != HAL_OK) {
        return TOTP_CONFIG_LOAD_UNREADABLE; // fail closed, never 0
    }
    *out = counter;
    return TOTP_CONFIG_LOAD_OK;
}

bool totp_config_set_last_counter(uint32_t counter)
{
    hal_status_t err = set_blob_verified(TOTP_KEY_LAST_CTR, &counter, sizeof(counter));
    if (err != HAL_OK) {
        return false;
    }
    s_ram_last_counter = counter;
    s_ram_loaded = true;
    return true;
}

bool totp_config_ram_last_counter(uint32_t *out)
{
    if (!s_ram_loaded) {
        uint32_t counter;
        if (totp_config_load_last_counter(&counter) == TOTP_CONFIG_LOAD_UNREADABLE) {
            return false;
        }
        s_ram_last_counter = counter;
        s_ram_loaded = true;
    }
    *out = s_ram_last_counter;
    return true;
}

void totp_config_ram_reset(void)
{
    s_ram_loaded = false;
    s_ram_last_counter = 0;
}

// --- Verify-and-consume ------------------------------------------------------

totp_consume_result_t totp_config_verify_and_consume(const char *code, uint64_t unix_time_s)
{
    uint32_t last = 0;
    if (!totp_config_ram_last_counter(&last)) {
        return TOTP_CONSUME_UNAVAILABLE; // unreadable counter: never treat as 0
    }

    uint8_t secret[TOTP_SECRET_LEN];
    totp_config_load_status_t st = totp_config_load_secret(secret);
    if (st == TOTP_CONFIG_LOAD_ABSENT) {
        return TOTP_CONSUME_NOT_ENROLLED;
    }
    if (st != TOTP_CONFIG_LOAD_OK) {
        return TOTP_CONSUME_UNAVAILABLE;
    }

    uint64_t matched = 0;
    bool ok = totp_verify(secret, sizeof(secret), code, unix_time_s, last, &matched);
    totp_secure_zero(secret, sizeof(secret));
    if (!ok) {
        return TOTP_CONSUME_REJECTED;
    }
    if (matched > UINT32_MAX) {
        return TOTP_CONSUME_UNAVAILABLE; // cannot be recorded, so cannot be accepted
    }
    // Record the step as used BEFORE reporting success.
    if (!totp_config_set_last_counter((uint32_t)matched)) {
        return TOTP_CONSUME_UNAVAILABLE;
    }
    return TOTP_CONSUME_OK;
}
