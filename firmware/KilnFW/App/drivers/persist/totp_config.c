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

typedef struct {
    uint32_t version;
    uint8_t  secret[TOTP_SECRET_LEN];
    uint8_t  reserved[3]; // pad to 4-byte alignment before crc32 (20+3=23 -> +1 to 24, see below)
    uint32_t crc32;
} totp_secret_blob_t;

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
    if (rberr != HAL_OK) {
        return HAL_VERIFY_FAILED;
    }
    if (memcmp(readback, buf, len) != 0) {
        return HAL_VERIFY_FAILED;
    }
    return HAL_OK;
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
    hal_status_t err = load_blob(TOTP_KEY_SECRET, &blob, sizeof(blob));
    if (err != HAL_OK) {
        return TOTP_CONFIG_LOAD_ABSENT; // never enrolled -- safe default
    }
    if (blob.version != TOTP_SECRET_BLOB_VERSION) {
        return TOTP_CONFIG_LOAD_UNREADABLE; // fail closed, never reinterpret
    }
    if (secret_blob_crc(&blob) != blob.crc32) {
        return TOTP_CONFIG_LOAD_UNREADABLE;
    }
    memcpy(out, blob.secret, TOTP_SECRET_LEN);
    return TOTP_CONFIG_LOAD_OK;
}

bool totp_config_enrolled(void)
{
    uint8_t secret[TOTP_SECRET_LEN];
    return totp_config_load_secret(secret) == TOTP_CONFIG_LOAD_OK;
}

bool totp_config_set_secret(const uint8_t secret[TOTP_SECRET_LEN])
{
    totp_secret_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = TOTP_SECRET_BLOB_VERSION;
    memcpy(blob.secret, secret, TOTP_SECRET_LEN);
    blob.crc32 = secret_blob_crc(&blob);

    hal_status_t err = set_blob_verified(TOTP_KEY_SECRET, &blob, sizeof(blob));
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
    totp_secret_blob_t check;
    memset(&check, 0, sizeof(check));
    if (load_blob(TOTP_KEY_SECRET, &check, sizeof(check)) != HAL_OK) {
        return false;
    }
    return memcmp(check.secret, secret, TOTP_SECRET_LEN) == 0;
}

bool totp_config_clear(void)
{
    hal_status_t secret_err = erase_key(TOTP_KEY_SECRET);
    hal_status_t ctr_err = erase_key(TOTP_KEY_LAST_CTR);
    totp_config_ram_reset();

    if (secret_err != HAL_OK || ctr_err != HAL_OK) {
        return false;
    }

    // Read-back verify: both keys must now read ABSENT.
    uint8_t discard_secret[TOTP_SECRET_LEN];
    if (totp_config_load_secret(discard_secret) != TOTP_CONFIG_LOAD_ABSENT) {
        return false;
    }
    totp_secret_blob_t probe;
    if (load_blob(TOTP_KEY_LAST_CTR, &probe, sizeof(uint32_t)) == HAL_OK) {
        return false; // still readable -- erase did not really take
    }
    return true;
}

// --- Replay-guard counter ----------------------------------------------------

// RAM cache: 0 doubles as both "not yet loaded this boot" and "no counter
// ever accepted" -- totp_config_ram_last_counter() disambiguates by tracking
// whether it has loaded at all, so a genuinely-persisted 0 (freshly
// enrolled) and "haven't checked NVS yet" behave identically anyway: both
// correctly mean "nothing accepted yet, no candidate counter is <= this."
static bool s_ram_loaded = false;
static uint32_t s_ram_last_counter = 0;

uint32_t totp_config_load_last_counter(void)
{
    uint32_t counter = 0;
    hal_status_t err = load_blob(TOTP_KEY_LAST_CTR, &counter, sizeof(counter));
    if (err != HAL_OK) {
        return 0; // never set -- 0 is the safe "never accepted" sentinel
    }
    return counter;
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

uint32_t totp_config_ram_last_counter(void)
{
    if (!s_ram_loaded) {
        s_ram_last_counter = totp_config_load_last_counter();
        s_ram_loaded = true;
    }
    return s_ram_last_counter;
}

void totp_config_ram_reset(void)
{
    s_ram_loaded = false;
    s_ram_last_counter = 0;
}
