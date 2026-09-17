// web_auth_store.c -- see web_auth_store.h for the full contract. Implements
// docs/WEB_AUTH_PLAN.md sections 2, 3 and 11.
#include "web_auth_store.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "hal_kv.h"
#include "nvs_key_check.h"
#include "psa/crypto.h"

// --- Placement: default `nvs` partition, own namespace (see header) --------
// partition == NULL -- the default `nvs` partition, per hal_kv_open()'s own
// doc comment. Never "kiln_nvs" (that partition is erased by
// factory_reset.c's PARTIAL/FULL scopes; credentials living there would
// contradict this module's entire reason for existing) and never
// "profiles_nvs"/"wifi_nvs" either.
#define WEB_AUTH_NAMESPACE "kiln_auth"
#define WEB_AUTH_KEY_WEB "web_auth"
#define WEB_AUTH_KEY_LCD "lcd_auth"
#define WEB_AUTH_KEY_POLICY "auth_policy"
NVS_KEY_LEN_CHECK(WEB_AUTH_NAMESPACE);
NVS_KEY_LEN_CHECK(WEB_AUTH_KEY_WEB);
NVS_KEY_LEN_CHECK(WEB_AUTH_KEY_LCD);
NVS_KEY_LEN_CHECK(WEB_AUTH_KEY_POLICY);

// --- On-disk blob layouts ----------------------------------------------------
// Each blob is version-tagged and CRC32'd, same convention as
// boot_guard_record_t: a version this build does not recognise, or a CRC
// mismatch, must both be treated as WEB_AUTH_LOAD_UNREADABLE -- never
// silently reinterpreted and never collapsed to ABSENT. Fixed layout,
// explicit padding, persisted verbatim as one hal_kv_set_blob() per key.

typedef struct {
    uint32_t version;
    uint8_t  reserved[4]; // keeps roles[] 8-aligned; explicit, not compiler-dependent
    web_auth_password_record_t roles[WEB_AUTH_ROLE_COUNT];
    uint32_t crc32;
} web_auth_web_blob_t;

typedef struct {
    uint32_t version;
    uint8_t  reserved[4];
    web_auth_pin_record_t roles[WEB_AUTH_ROLE_COUNT];
    uint32_t crc32;
} web_auth_lcd_blob_t;

typedef struct {
    uint32_t version;
    bool     web_enabled;
    bool     lcd_enabled;
    uint8_t  reserved[2];
    int32_t  web_timeout_s;
    int32_t  lcd_timeout_s;
    uint32_t crc32;
} web_auth_policy_blob_t;

// --- CRC32 (IEEE 802.3 / zlib polynomial), table-less -----------------------
// Same construction as boot_guard.c's crc32_compute() -- verified against
// the standard "123456789" -> 0xCBF43926 test vector below in the host test
// file, not duplicated as a comment claim here.
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

static uint32_t web_blob_crc(const web_auth_web_blob_t *b)
{
    return crc32_compute(b, offsetof(web_auth_web_blob_t, crc32));
}
static uint32_t lcd_blob_crc(const web_auth_lcd_blob_t *b)
{
    return crc32_compute(b, offsetof(web_auth_lcd_blob_t, crc32));
}
static uint32_t policy_blob_crc(const web_auth_policy_blob_t *b)
{
    return crc32_compute(b, offsetof(web_auth_policy_blob_t, crc32));
}

// --- Strength rules (item 3) -------------------------------------------------

// Case-insensitive substring test -- "password"/"kiln" must be rejected
// whether they ARE the whole password ("password") or are embedded inside
// a longer one ("MyPassword123"), which is the shape a naive user actually
// picks.
static bool str_icontains(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || *needle == '\0') {
        return false;
    }
    size_t hn = strlen(haystack);
    size_t nn = strlen(needle);
    if (nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        bool match = true;
        for (size_t j = 0; j < nn; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

web_auth_pw_check_t web_auth_password_check(const char *password, const char *username,
                                             const char *ap_ssid, const char *ap_password)
{
    if (password == NULL) {
        return WEB_AUTH_PW_TOO_SHORT;
    }
    size_t len = strlen(password);
    if (len < 10u) {
        return WEB_AUTH_PW_TOO_SHORT;
    }
    if (len > 64u) {
        return WEB_AUTH_PW_TOO_LONG;
    }

    bool has_non_lower = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)password[i];
        if (!(c >= 'a' && c <= 'z')) {
            has_non_lower = true;
            break;
        }
    }
    if (!has_non_lower) {
        return WEB_AUTH_PW_ALL_LOWERCASE;
    }

    if (str_icontains(password, "password") || str_icontains(password, "kiln")) {
        return WEB_AUTH_PW_REJECTED_COMMON;
    }
    // Exact, case-sensitive: these are secrets, not English words.
    if (username != NULL && username[0] != '\0' && strcmp(password, username) == 0) {
        return WEB_AUTH_PW_REJECTED_COMMON;
    }
    if (ap_ssid != NULL && ap_ssid[0] != '\0' && strcmp(password, ap_ssid) == 0) {
        return WEB_AUTH_PW_REJECTED_COMMON;
    }
    if (ap_password != NULL && ap_password[0] != '\0' && strcmp(password, ap_password) == 0) {
        return WEB_AUTH_PW_REJECTED_COMMON;
    }

    return WEB_AUTH_PW_OK;
}

web_auth_pin_check_t web_auth_pin_check(const char *pin, const char *other_pin_or_null)
{
    if (pin == NULL) {
        return WEB_AUTH_PIN_TOO_SHORT;
    }
    size_t len = strlen(pin);
    if (len < 4u) {
        return WEB_AUTH_PIN_TOO_SHORT;
    }
    if (len > 8u) {
        return WEB_AUTH_PIN_TOO_LONG;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)pin[i])) {
            return WEB_AUTH_PIN_NOT_DIGITS;
        }
    }
    if (other_pin_or_null != NULL && other_pin_or_null[0] != '\0' &&
        strcmp(pin, other_pin_or_null) == 0) {
        return WEB_AUTH_PIN_SAME_AS_OTHER;
    }
    return WEB_AUTH_PIN_OK;
}

// --- Hashing (item 2) --------------------------------------------------------

// See ota_http.c's hmac_sha256() -- identical PSA import/compute/destroy
// sequence, duplicated rather than shared because ota_http.c is not linked
// into every consumer of this module (host tests included) and this file
// must not pull in ota_http.c's much larger ESP-IDF-only dependency set.
static int hmac_sha256(const uint8_t *key_bytes, size_t key_len, const uint8_t *msg,
                        size_t msg_len, uint8_t out[32])
{
    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, key_bytes, key_len, &key_id);
    if (status != PSA_SUCCESS) {
        return (int)status;
    }

    size_t mac_len = 0;
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, out, 32,
                              &mac_len);
    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS) ? 0 : (int)status;
}

// Iterated-HMAC-SHA256 KDF -- see web_auth_store.h's doc comment on
// web_auth_hash_compute() for why this shape (H_1 = HMAC(secret, salt),
// H_i = HMAC(secret, H_{i-1})) rather than a PSA PBKDF2 algorithm object.
void web_auth_hash_compute(const uint8_t *secret, size_t secret_len,
                            const uint8_t salt[WEB_AUTH_SALT_LEN], uint32_t iterations,
                            uint8_t out[WEB_AUTH_HASH_LEN])
{
    uint8_t buf[WEB_AUTH_HASH_LEN];
    if (iterations == 0u) {
        iterations = 1u;
    }
    (void)hmac_sha256(secret, secret_len, salt, WEB_AUTH_SALT_LEN, buf);
    for (uint32_t i = 1; i < iterations; i++) {
        (void)hmac_sha256(secret, secret_len, buf, WEB_AUTH_HASH_LEN, buf);
    }
    memcpy(out, buf, WEB_AUTH_HASH_LEN);
}

bool web_auth_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// --- Generic versioned-blob load/verify-write helpers -----------------------
// Mirrors boot_guard.c's verify_persisted_count(): a same-boot read-back
// cannot detect every failure mode (NVS often serves reads from its RAM
// index, not flash), but it is the mandated minimum defensive pattern, and
// it is exactly what caught the historical "HAL_OK but nothing changed"
// bug this codebase already hit once. Never trust the hal_kv_set_blob()
// return code alone.

static hal_status_t load_blob(const char *key, void *out, size_t out_len)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, WEB_AUTH_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL);
    if (err != HAL_OK) {
        return err; // treated as ABSENT by callers on the expected "not found" code
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

// Writes `buf`/`len` under `key`, commits, then reopens READ_ONLY and reads
// it back byte-for-byte before reporting success -- see this file's header
// comment. Returns HAL_IO if the read-back does not match.
static hal_status_t set_blob_verified(const char *key, const void *buf, size_t len)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, WEB_AUTH_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL);
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

    // Read-back verification: never trust the write's own return code.
    void *readback = NULL;
    uint8_t stackbuf[256];
    if (len <= sizeof(stackbuf)) {
        readback = stackbuf;
    } else {
        return HAL_INVALID_SIZE; // none of this module's blobs are this large
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

// --- Web passwords -----------------------------------------------------------

web_auth_load_status_t web_auth_store_load_password(web_auth_role_t role,
                                                      web_auth_password_record_t *out)
{
    memset(out, 0, sizeof(*out));
    if (role != WEB_AUTH_ROLE_USER && role != WEB_AUTH_ROLE_ADMINISTRATOR) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }

    web_auth_web_blob_t blob;
    hal_status_t err = load_blob(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    if (err != HAL_OK) {
        return WEB_AUTH_LOAD_ABSENT; // never written -- shipped default / pre-upgrade board
    }
    if (blob.version != WEB_AUTH_STORE_VERSION) {
        // Includes "newer than this build knows" (OTA rollback hazard,
        // item 12b) -- fail closed, never reinterpret.
        return WEB_AUTH_LOAD_UNREADABLE;
    }
    if (web_blob_crc(&blob) != blob.crc32) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }

    *out = blob.roles[role];
    return WEB_AUTH_LOAD_OK;
}

bool web_auth_store_verify_password(web_auth_role_t role, const char *password)
{
    if (password == NULL) {
        return false;
    }
    web_auth_password_record_t rec;
    if (web_auth_store_load_password(role, &rec) != WEB_AUTH_LOAD_OK) {
        return false;
    }
    if (!rec.configured) {
        return false;
    }
    uint8_t computed[WEB_AUTH_HASH_LEN];
    web_auth_hash_compute((const uint8_t *)password, strlen(password), rec.salt, rec.iterations,
                           computed);
    return web_auth_constant_time_equal(computed, rec.hash, WEB_AUTH_HASH_LEN);
}

bool web_auth_store_password_configured(web_auth_role_t role)
{
    web_auth_password_record_t rec;
    return web_auth_store_load_password(role, &rec) == WEB_AUTH_LOAD_OK && rec.configured;
}

hal_status_t web_auth_store_set_password(web_auth_role_t role, const char *username,
                                          const char *password,
                                          const uint8_t salt[WEB_AUTH_SALT_LEN],
                                          bool must_change)
{
    if ((role != WEB_AUTH_ROLE_USER && role != WEB_AUTH_ROLE_ADMINISTRATOR) ||
        password == NULL || salt == NULL) {
        return HAL_INVALID_ARG;
    }

    web_auth_web_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    hal_status_t existing = load_blob(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    if (existing != HAL_OK || blob.version != WEB_AUTH_STORE_VERSION ||
        web_blob_crc(&blob) != blob.crc32) {
        // No usable prior blob (absent, wrong version, or corrupt): start
        // from a clean slate rather than persist a half-valid mix. Any
        // other role's record is deliberately lost in this case -- same
        // "discard rather than migrate" convention as boot_guard.c.
        memset(&blob, 0, sizeof(blob));
    }

    web_auth_password_record_t *rec = &blob.roles[role];
    memset(rec, 0, sizeof(*rec));
    size_t ulen = (username != NULL) ? strlen(username) : 0;
    if (ulen > WEB_AUTH_USERNAME_MAX_LEN) {
        ulen = WEB_AUTH_USERNAME_MAX_LEN;
    }
    if (username != NULL) {
        memcpy(rec->username, username, ulen);
    }
    rec->username[ulen] = '\0';
    memcpy(rec->salt, salt, WEB_AUTH_SALT_LEN);
    rec->iterations = WEB_AUTH_ITERATIONS;
    web_auth_hash_compute((const uint8_t *)password, strlen(password), rec->salt,
                           rec->iterations, rec->hash);
    rec->must_change = must_change;
    rec->configured = true;

    blob.version = WEB_AUTH_STORE_VERSION;
    blob.crc32 = web_blob_crc(&blob);

    return set_blob_verified(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
}

// --- LCD PINs -----------------------------------------------------------------

web_auth_load_status_t web_auth_store_load_pin(web_auth_role_t role, web_auth_pin_record_t *out)
{
    memset(out, 0, sizeof(*out));
    if (role != WEB_AUTH_ROLE_USER && role != WEB_AUTH_ROLE_ADMINISTRATOR) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }
    web_auth_lcd_blob_t blob;
    hal_status_t err = load_blob(WEB_AUTH_KEY_LCD, &blob, sizeof(blob));
    if (err != HAL_OK) {
        return WEB_AUTH_LOAD_ABSENT;
    }
    if (blob.version != WEB_AUTH_STORE_VERSION) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }
    if (lcd_blob_crc(&blob) != blob.crc32) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }
    *out = blob.roles[role];
    return WEB_AUTH_LOAD_OK;
}

bool web_auth_store_verify_pin(web_auth_role_t role, const char *pin)
{
    if (pin == NULL) {
        return false;
    }
    web_auth_pin_record_t rec;
    if (web_auth_store_load_pin(role, &rec) != WEB_AUTH_LOAD_OK) {
        return false;
    }
    if (!rec.configured) {
        return false;
    }
    uint8_t computed[WEB_AUTH_HASH_LEN];
    web_auth_hash_compute((const uint8_t *)pin, strlen(pin), rec.salt, rec.iterations, computed);
    return web_auth_constant_time_equal(computed, rec.hash, WEB_AUTH_HASH_LEN);
}

bool web_auth_store_pin_configured(web_auth_role_t role)
{
    web_auth_pin_record_t rec;
    return web_auth_store_load_pin(role, &rec) == WEB_AUTH_LOAD_OK && rec.configured;
}

hal_status_t web_auth_store_set_pin(web_auth_role_t role, const char *pin,
                                     const uint8_t salt[WEB_AUTH_SALT_LEN])
{
    if ((role != WEB_AUTH_ROLE_USER && role != WEB_AUTH_ROLE_ADMINISTRATOR) || pin == NULL ||
        salt == NULL) {
        return HAL_INVALID_ARG;
    }
    size_t plen = strlen(pin);
    if (plen < 4u || plen > 8u) {
        return HAL_INVALID_ARG;
    }

    web_auth_lcd_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    hal_status_t existing = load_blob(WEB_AUTH_KEY_LCD, &blob, sizeof(blob));
    if (existing != HAL_OK || blob.version != WEB_AUTH_STORE_VERSION ||
        lcd_blob_crc(&blob) != blob.crc32) {
        memset(&blob, 0, sizeof(blob));
    }

    web_auth_pin_record_t *rec = &blob.roles[role];
    memset(rec, 0, sizeof(*rec));
    memcpy(rec->salt, salt, WEB_AUTH_SALT_LEN);
    rec->iterations = WEB_AUTH_ITERATIONS;
    web_auth_hash_compute((const uint8_t *)pin, plen, rec->salt, rec->iterations, rec->hash);
    rec->digits = (uint8_t)plen;
    rec->configured = true;

    blob.version = WEB_AUTH_STORE_VERSION;
    blob.crc32 = lcd_blob_crc(&blob);

    return set_blob_verified(WEB_AUTH_KEY_LCD, &blob, sizeof(blob));
}

// --- Policy -------------------------------------------------------------------

web_auth_load_status_t web_auth_store_load_policy(web_auth_policy_t *out)
{
    memset(out, 0, sizeof(*out));
    web_auth_policy_blob_t blob;
    hal_status_t err = load_blob(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob));
    if (err != HAL_OK) {
        return WEB_AUTH_LOAD_ABSENT; // item 11: absent record reads as both-off
    }
    if (blob.version != WEB_AUTH_STORE_VERSION) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }
    if (policy_blob_crc(&blob) != blob.crc32) {
        return WEB_AUTH_LOAD_UNREADABLE;
    }
    out->web_enabled = blob.web_enabled;
    out->lcd_enabled = blob.lcd_enabled;
    out->web_timeout_s = blob.web_timeout_s;
    out->lcd_timeout_s = blob.lcd_timeout_s;
    return WEB_AUTH_LOAD_OK;
}

hal_status_t web_auth_store_set_policy(const web_auth_policy_t *policy)
{
    if (policy == NULL) {
        return HAL_INVALID_ARG;
    }
    web_auth_policy_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = WEB_AUTH_STORE_VERSION;
    blob.web_enabled = policy->web_enabled;
    blob.lcd_enabled = policy->lcd_enabled;
    blob.web_timeout_s = policy->web_timeout_s;
    blob.lcd_timeout_s = policy->lcd_timeout_s;
    blob.crc32 = policy_blob_crc(&blob);

    return set_blob_verified(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob));
}

// --- Auth-off / first-boot / field-upgrade collapse (item 11) --------------

bool web_auth_policy_effective_enabled(web_auth_load_status_t status, bool stored_enabled)
{
    switch (status) {
        case WEB_AUTH_LOAD_ABSENT:
            return false;
        case WEB_AUTH_LOAD_OK:
            return stored_enabled;
        case WEB_AUTH_LOAD_UNREADABLE:
        default:
            return true; // fail closed -- see header comment
    }
}
