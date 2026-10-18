// web_auth_store.c -- see web_auth_store.h for the full contract. Implements
// docs/WEB_AUTH_PLAN.md sections 2, 3 and 11.
#include "web_auth_store.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "hal_kv.h"
#include "nvs_key_check.h"
#include "psa/crypto.h"

// vTaskDelay() -- see web_auth_hash_compute()'s doc comment below (2026-09-20
// watchdog fix) for why this loop sleeps periodically.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
// AU-02: separate "auth was ever enabled" marker, outside the policy key, so a
// path that drops only auth_policy cannot silently open every admin route.
#define WEB_AUTH_KEY_EVER "auth_ever"
NVS_KEY_LEN_CHECK(WEB_AUTH_KEY_EVER);
#define WEB_AUTH_EVER_MAGIC 0x41555448u
typedef struct {
    uint32_t magic;
    uint32_t magic_inv;
} web_auth_ever_blob_t;

// FS-02: "this board's login storage was initialised" evidence. A sentinel in
// the default `nvs` partition (erased with the credentials it vouches for) and
// a mirror in wifi_nvs (survives an nvs erase). nvs sentinel missing while the
// mirror (or a boot-time auto-erase plus surviving setup) says the board was
// set up before => storage was reset => fail closed (see
// web_auth_store_storage_reset_evaluate()).
#define WEB_AUTH_KEY_SEEN "nvs_seen"
NVS_KEY_LEN_CHECK(WEB_AUTH_KEY_SEEN);
#define WEB_AUTH_MIRROR_PARTITION "wifi_nvs"
#define WEB_AUTH_SEEN_MAGIC 0x5345454eu
static bool s_storage_reset_latched;
// F2-01: LCD lock is on but no admin PIN exists (set by a storage-reset recovery).
static bool s_lcd_pin_pending;
static void storage_reset_resolve(void);
static void lcd_pin_pending_refresh(void);

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
    if (len < WEB_AUTH_PIN_MIN_LEN) {
        return WEB_AUTH_PIN_TOO_SHORT;
    }
    if (len > WEB_AUTH_PIN_MAX_LEN) {
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

// 2026-09-20 crash fix (POST /api/auth/login watchdog panic, IDLE1 CPU1):
// web_auth_hash_compute() used to call hmac_sha256() above once per
// iteration -- the record's own iteration count (WEB_AUTH_ITERATIONS or the
// higher WEB_AUTH_PIN_ITERATIONS, whichever a given call is hashing for)
// full psa_import_key / psa_mac_compute / psa_destroy_key round trips
// back-to-back with no yield,
// long enough on the httpd worker task to blow past
// CONFIG_ESP_TASK_WDT_TIMEOUT_S=5 and trigger the idle-task watchdog. The
// key bytes (`secret`) never change across iterations -- only the message
// does (H_1 = HMAC(secret, salt), H_i = HMAC(secret, H_{i-1})) -- so the key
// only needs to be imported ONCE for the whole KDF run: psa_mac_compute() is
// a pure function of (already-imported key id, message), so calling it N
// times against the same key_id produces exactly the same N outputs as N
// separate import/compute/destroy cycles did. web_auth_hash_iterate() below
// is that single-import loop; it also BLOCKS for one tick every
// WEB_AUTH_HASH_YIELD_EVERY iterations (vTaskDelay(1), a no-op on the host
// test build's FreeRTOS shim) so even a slow/blocked crypto backend can
// never starve IDLE1 for the whole run, however many iterations the caller's
// record stamps -- belt-and-suspenders
// against the watchdog, independent of the import-count fix above.
//
// It must be vTaskDelay(1), NOT taskYIELD(): taskYIELD() is portYIELD(),
// which re-runs the scheduler and picks the highest-priority READY task on
// this core. The httpd worker runs at tskIDLE_PRIORITY+5
// (HTTPD_DEFAULT_CONFIG, see wifi_provision_http.c), so the scheduler would
// simply pick this same task again every time and the idle task -- the only
// watchdog subscriber that matters here
// (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1=y) -- would still never run.
// FreeRTOS runs a lower-priority task only when the higher-priority one
// BLOCKS, so this loop has to actually leave the ready list: vTaskDelay(1)
// blocks for one tick (10 ms at CONFIG_FREERTOS_HZ=100), which lets IDLE0/
// IDLE1 run and feed the watchdog. Cost: record_iterations /
// WEB_AUTH_HASH_YIELD_EVERY one-tick sleeps per KDF run -- e.g. 1 for a
// WEB_AUTH_ITERATIONS (2000) web login, 19 for a WEB_AUTH_PIN_ITERATIONS
// (20000) PIN check, up to ~190 ms added there -- deliberately traded for a
// watchdog that is fed no matter how slow the crypto backend is. Neither
// change alters a single output bit: the same HMAC-SHA256 chain over the
// same inputs in the same order still produces the same hash -- only how
// the key is (re)imported and how often the loop yields differ. See
// test_web_auth_store.c's test_hash_compute_pinned_vectors() for two
// pinned-digest regression vectors proving output is byte-identical to
// before this fix.
#define WEB_AUTH_HASH_YIELD_EVERY 1024u

// Returns 0 on success. On failure, *import_failed says WHICH failure it
// was: true = psa_import_key() never succeeded, so nothing was computed and
// the per-iteration fallback below is worth trying; false = the key
// imported fine but a psa_mac_compute() round failed, in which case
// re-running the same iteration count through the fallback would only burn
// another full KDF run to fail the same way (and would double the worst-case
// duration this fix exists to bound).
static int web_auth_hash_iterate(const uint8_t *secret, size_t secret_len,
                                  const uint8_t salt[WEB_AUTH_SALT_LEN], uint32_t iterations,
                                  uint8_t out[WEB_AUTH_HASH_LEN], bool *import_failed)
{
    *import_failed = false;
    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_import_key(&attr, secret, secret_len, &key_id);
    if (status != PSA_SUCCESS) {
        *import_failed = true;
        return (int)status;
    }

    uint8_t buf[WEB_AUTH_HASH_LEN];
    size_t mac_len = 0;
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), salt, WEB_AUTH_SALT_LEN, buf,
                              WEB_AUTH_HASH_LEN, &mac_len);
    for (uint32_t i = 1; i < iterations && status == PSA_SUCCESS; i++) {
        status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), buf, WEB_AUTH_HASH_LEN,
                                  buf, WEB_AUTH_HASH_LEN, &mac_len);
        if ((i % WEB_AUTH_HASH_YIELD_EVERY) == 0u) {
            vTaskDelay(1);
        }
    }
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS) {
        return (int)status;
    }
    memcpy(out, buf, WEB_AUTH_HASH_LEN);
    return 0;
}

// Iterated-HMAC-SHA256 KDF -- see web_auth_store.h's doc comment on
// web_auth_hash_compute() for why this shape (H_1 = HMAC(secret, salt),
// H_i = HMAC(secret, H_{i-1})) rather than a PSA PBKDF2 algorithm object.
void web_auth_hash_compute(const uint8_t *secret, size_t secret_len,
                            const uint8_t salt[WEB_AUTH_SALT_LEN], uint32_t iterations,
                            uint8_t out[WEB_AUTH_HASH_LEN])
{
    if (iterations == 0u) {
        iterations = 1u;
    }
    bool import_failed = false;
    if (web_auth_hash_iterate(secret, secret_len, salt, iterations, out, &import_failed) == 0) {
        return;
    }
    if (!import_failed) {
        // The key imported but a psa_mac_compute() round failed. The
        // fallback below would import the same key and run the same
        // computes, so it can only fail the same way after another full
        // 20000-round pass. Fail closed with a DEFINED value instead of
        // leaving the caller's buffer uninitialized: an all-zero digest is
        // still compared in constant time by every caller and will not match
        // any stored hash, so verification denies rather than accepts.
        memset(out, 0, WEB_AUTH_HASH_LEN);
        return;
    }
    // Fallback, reachable only if psa_import_key() itself fails (e.g. the
    // crypto backend is out of key slots): preserves the previous
    // per-iteration import/compute/destroy behaviour rather than leaving
    // `out` uninitialized, at the cost of the watchdog risk this fix exists
    // to remove. Not expected to be hit in practice.
    // Zero-initialized so that if EVERY hmac_sha256() below also fails,
    // `out` still gets a defined (non-matching) value rather than stack
    // garbage -- same fail-closed reasoning as the !import_failed branch
    // above. On any successful run buf is fully overwritten, so this does
    // not change a single output bit of the normal path.
    uint8_t buf[WEB_AUTH_HASH_LEN] = {0};
    (void)hmac_sha256(secret, secret_len, salt, WEB_AUTH_SALT_LEN, buf);
    for (uint32_t i = 1; i < iterations; i++) {
        (void)hmac_sha256(secret, secret_len, buf, WEB_AUTH_HASH_LEN, buf);
        if ((i % WEB_AUTH_HASH_YIELD_EVERY) == 0u) {
            vTaskDelay(1);
        }
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
    if (err == HAL_NOT_FOUND) {
        return WEB_AUTH_LOAD_ABSENT; // never written -- shipped default / pre-upgrade board
    }
    if (err != HAL_OK) {
        return WEB_AUTH_LOAD_UNREADABLE; // AZ-01: read error / wrong size is not "never written"
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
    bool loaded_ok = web_auth_store_load_password(role, &rec) == WEB_AUTH_LOAD_OK;
    // Finding 6 fix (2026-09-17 review): PBKDF2 must run on every call,
    // whether or not a role has ever been provisioned, so a caller
    // measuring response latency cannot use "fast vs slow" to distinguish
    // "no such account" from "wrong password" -- both must take one
    // PBKDF2 pass. When there is no real record (never loaded, or
    // !rec.configured), hash the supplied password against a fixed
    // decoy salt/iteration-count instead of skipping the work; the
    // result is discarded below via the ORIGINAL configured-ness check,
    // never compared against anything meaningful. Never uses the empty
    // struct's real (all-zero) salt/hash from `rec` in that path -- an
    // all-zero salt is still a valid-looking salt and would make an
    // unconfigured role's timing depend on the (all-zero) iterations
    // field, i.e. skip PBKDF2 entirely via `iterations == 0`.
    static const uint8_t decoy_salt[WEB_AUTH_SALT_LEN] = {0};
    const uint8_t *salt = (loaded_ok && rec.configured) ? rec.salt : decoy_salt;
    uint32_t iterations = (loaded_ok && rec.configured) ? rec.iterations : WEB_AUTH_ITERATIONS;
    uint8_t computed[WEB_AUTH_HASH_LEN];
    web_auth_hash_compute((const uint8_t *)password, strlen(password), salt, iterations, computed);
    if (!loaded_ok || !rec.configured) {
        return false;
    }
    return web_auth_constant_time_equal(computed, rec.hash, WEB_AUTH_HASH_LEN);
}

// AZ-01: true unless the admin record is truly absent (NVS not-found, marker clear) or
// readable-and-unconfigured. UNREADABLE (bad CRC, version, read error) counts as
// configured so the bootstrap route stays closed; recovery is the reset gesture.
bool web_auth_store_admin_configured_or_unreadable(void)
{
    web_auth_password_record_t rec;
    web_auth_load_status_t st = web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &rec);
    if (st == WEB_AUTH_LOAD_UNREADABLE) {
        return true;
    }
    // FX-02: key lost (NOT_FOUND) on a board that was ever set up counts as
    // configured, same as AU-02 does for the policy key. The physical reset
    // gesture does not erase this key: it writes an unconfigured blob (key
    // present, configured=false), so it still reaches bootstrap.
    if (st != WEB_AUTH_LOAD_OK && web_auth_store_ever_enabled()) {
        return true;
    }
    return st == WEB_AUTH_LOAD_OK && rec.configured;
}

bool web_auth_store_password_configured(web_auth_role_t role)
{
    web_auth_password_record_t rec;
    return web_auth_store_load_password(role, &rec) == WEB_AUTH_LOAD_OK && rec.configured;
}

// REVIEW_FIXERS LOW-2 (totpfx): a credential wipe (physical reset / web
// clear_credentials) must not be overwritten by a password setter that loaded
// the old blob before the wipe and is still hashing (seconds) while it runs.
// The wipe functions bump this epoch before AND after writing; a setter reads
// it before it loads, and re-reads it after hashing, just before the write,
// refusing if it moved. Atomic: setters run on httpd, wipes on the LVGL task.
static volatile uint32_t s_cred_wipe_epoch;

static uint32_t cred_wipe_epoch_get(void)
{
#if defined(__GNUC__)
    return __atomic_load_n(&s_cred_wipe_epoch, __ATOMIC_SEQ_CST);
#else
    return s_cred_wipe_epoch; /* host (MSVC) tests are single-threaded */
#endif
}

static void cred_wipe_epoch_bump(void)
{
#if defined(__GNUC__)
    (void)__atomic_add_fetch(&s_cred_wipe_epoch, 1u, __ATOMIC_SEQ_CST);
#else
    s_cred_wipe_epoch++;
#endif
}

// Store lock: held by a setter across its final epoch re-check AND the NVS
// write, and by a wipe across its bump/impl/bump, so a wipe can never land
// between a setter's check and its write (REVIEW_ALL_FIXES L1). The KDF runs
// outside the lock. Lazily created under a critical section (first use can
// race between httpd and LVGL).
#define CRED_STORE_LOCK_WAIT_MS 5000
static volatile uint32_t s_cred_store_busy;

// Test seam (host tests include this file directly): called once, then cleared,
// just before a setter/wipe acquires the store lock, to model another task acting
// in the window between a setter's KDF and its write.
void (*g_web_auth_store_test_pre_lock_hook)(void);

static bool cred_store_try_lock(void)
{
#if defined(__GNUC__)
    return __atomic_exchange_n(&s_cred_store_busy, 1u, __ATOMIC_ACQUIRE) == 0u;
#else
    if (s_cred_store_busy) {
        return false;
    }
    s_cred_store_busy = 1u;
    return true; /* host (MSVC) tests are single-threaded */
#endif
}

static void cred_store_unlock(void)
{
#if defined(__GNUC__)
    __atomic_store_n(&s_cred_store_busy, 0u, __ATOMIC_RELEASE);
#else
    s_cred_store_busy = 0u;
#endif
}

// Polls with vTaskDelay (the critical sections under the lock are NVS writes,
// ~ms). timeout_ms < 0 waits forever (wipes must not give up).
static bool cred_store_lock(int32_t timeout_ms)
{
    if (g_web_auth_store_test_pre_lock_hook) {
        void (*h)(void) = g_web_auth_store_test_pre_lock_hook;
        g_web_auth_store_test_pre_lock_hook = NULL;
        h();
    }
    int32_t waited = 0;
    while (!cred_store_try_lock()) {
        if (timeout_ms >= 0 && waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(1);
        waited += (int32_t)portTICK_PERIOD_MS;
    }
    return true;
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

    const uint32_t wipe_epoch = cred_wipe_epoch_get();
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

    if (!cred_store_lock(CRED_STORE_LOCK_WAIT_MS)) {
        memset(&blob, 0, sizeof(blob));
        return HAL_BUSY;
    }
    if (cred_wipe_epoch_get() != wipe_epoch) {
        // A credential wipe ran while the KDF was hashing: writing now would
        // resurrect the pre-wipe blob with this password in it.
        cred_store_unlock();
        memset(&blob, 0, sizeof(blob));
        return HAL_BUSY;
    }

    blob.version = WEB_AUTH_STORE_VERSION;
    blob.crc32 = web_blob_crc(&blob);

    hal_status_t wst = set_blob_verified(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    cred_store_unlock();
    if (wst == HAL_OK && role == WEB_AUTH_ROLE_ADMINISTRATOR && s_storage_reset_latched) {
        storage_reset_resolve();
    }
    return wst;
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
    if (plen < WEB_AUTH_PIN_MIN_LEN || plen > WEB_AUTH_PIN_MAX_LEN) {
        return HAL_INVALID_ARG;
    }

    const uint32_t wipe_epoch = cred_wipe_epoch_get();
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
    // Review fix (2026-09-21, finding 5): the PIN keeps its own, higher-cost
    // constant -- WEB_AUTH_ITERATIONS was cut for web logon latency only
    // (2026-09-21 owner decision); see WEB_AUTH_PIN_ITERATIONS's comment in
    // web_auth_store.h.
    rec->iterations = WEB_AUTH_PIN_ITERATIONS;
    web_auth_hash_compute((const uint8_t *)pin, plen, rec->salt, rec->iterations, rec->hash);
    rec->digits = (uint8_t)plen;
    rec->configured = true;

    if (!cred_store_lock(CRED_STORE_LOCK_WAIT_MS)) {
        memset(&blob, 0, sizeof(blob));
        return HAL_BUSY;
    }
    if (cred_wipe_epoch_get() != wipe_epoch) {
        // REVIEW_ALL_FIXES L2: a wipe ran during the PIN hash; do not bring the PIN back.
        cred_store_unlock();
        memset(&blob, 0, sizeof(blob));
        return HAL_BUSY;
    }

    blob.version = WEB_AUTH_STORE_VERSION;
    blob.crc32 = lcd_blob_crc(&blob);

    hal_status_t wst = set_blob_verified(WEB_AUTH_KEY_LCD, &blob, sizeof(blob));
    cred_store_unlock();
    // F2-03: this is reached only from the authenticated admin dispatch
    // (security_http_core SET_LCD_PIN), so an admin PIN also ends the reset latch.
    if (wst == HAL_OK && role == WEB_AUTH_ROLE_ADMINISTRATOR && s_storage_reset_latched) {
        storage_reset_resolve();
    }
    if (wst == HAL_OK) {
        lcd_pin_pending_refresh();
    }
    return wst;
}

// --- Policy -------------------------------------------------------------------

// FS-02 helpers. partition NULL == default nvs.
static hal_status_t seen_probe(const char *partition)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, WEB_AUTH_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_key_exists(&h, WEB_AUTH_KEY_SEEN);
    hal_kv_close(&h);
    return err;
}

static hal_status_t seen_write(const char *partition)
{
    uint32_t blob[2] = {WEB_AUTH_SEEN_MAGIC, ~WEB_AUTH_SEEN_MAGIC};
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, WEB_AUTH_NAMESPACE, HAL_KV_MODE_READ_WRITE, partition);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, WEB_AUTH_KEY_SEEN, blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

static bool storage_reset_evaluate_impl(bool setup_evidence_elsewhere);

bool web_auth_store_storage_reset_evaluate(bool setup_evidence_elsewhere)
{
    bool r = storage_reset_evaluate_impl(setup_evidence_elsewhere);
    lcd_pin_pending_refresh();
    return r;
}

static bool storage_reset_evaluate_impl(bool setup_evidence_elsewhere)
{
    s_storage_reset_latched = false;
    hal_status_t nvs_st = seen_probe(NULL);
    bool mirror_ok = hal_kv_mount_probe(WEB_AUTH_MIRROR_PARTITION) == HAL_OK;
    if (nvs_st == HAL_OK) {
        if (mirror_ok && seen_probe(WEB_AUTH_MIRROR_PARTITION) != HAL_OK) {
            (void)seen_write(WEB_AUTH_MIRROR_PARTITION); // backfill, best effort
        }
        return false;
    }
    if (nvs_st != HAL_NOT_FOUND) {
        return false; // unreadable store: the existing AZ-02 paths fail closed
    }
    // Pre-existing auth data without a sentinel (upgrade from before FS-02): healthy.
    web_auth_policy_blob_t pb;
    if (load_blob(WEB_AUTH_KEY_POLICY, &pb, sizeof(pb)) != HAL_NOT_FOUND || web_auth_store_ever_enabled()) {
        (void)seen_write(NULL);
        if (mirror_ok) {
            (void)seen_write(WEB_AUTH_MIRROR_PARTITION);
        }
        return false;
    }
    bool evidence = (mirror_ok && seen_probe(WEB_AUTH_MIRROR_PARTITION) == HAL_OK) ||
                    (hal_kv_partition_was_auto_erased(NULL) && setup_evidence_elsewhere);
    if (evidence) {
        s_storage_reset_latched = true;
        // F2-02: an admin password record already exists, so a previous recovery
        // was cut or failed between the password and the policy write. Finish it
        // (policy, then sentinel) instead of staying latched; failure stays latched.
        web_auth_web_blob_t wb;
        if (load_blob(WEB_AUTH_KEY_WEB, &wb, sizeof(wb)) != HAL_NOT_FOUND) {
            storage_reset_resolve();
        }
        return s_storage_reset_latched;
    }
    // Truly factory-fresh: record it in both places.
    (void)seen_write(NULL);
    if (mirror_ok) {
        (void)seen_write(WEB_AUTH_MIRROR_PARTITION);
    }
    return false;
}

bool web_auth_store_storage_reset_latched(void)
{
    return s_storage_reset_latched;
}

const char *web_auth_store_storage_reset_banner_text(void)
{
    return "AUTH: login storage was reset; set admin";
}

bool web_auth_store_lcd_pin_pending(void)
{
    return s_lcd_pin_pending;
}

const char *web_auth_store_lcd_pin_pending_banner_text(void)
{
    return "AUTH: LCD locked; set LCD admin PIN or disable LCD lock";
}

// Cached: the home banner polls this. Recomputed at boot and after any PIN or policy write.
static void lcd_pin_pending_refresh(void)
{
    web_auth_policy_blob_t pb;
    s_lcd_pin_pending = load_blob(WEB_AUTH_KEY_POLICY, &pb, sizeof(pb)) == HAL_OK && pb.lcd_enabled &&
                        !web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR);
}

// Admin web password was just set while latched: persist an auth-enabled
// policy, then the sentinel, then drop the latch. Policy first so a crash in
// between leaves a policy-present (healthy) board, never an open one.
static void storage_reset_resolve(void)
{
    // F2-01: LCD lock stays ON (gated, no PIN yet) so recovery never opens the LCD silently.
    web_auth_policy_t p = {.web_enabled = true, .lcd_enabled = true,
                           .web_timeout_s = 1800, .lcd_timeout_s = 1800};
    if (web_auth_store_set_policy(&p) != HAL_OK) {
        return; // stays latched: fail closed (load_policy keeps reading UNREADABLE)
    }
    if (seen_write(NULL) == HAL_OK) {
        (void)seen_write(WEB_AUTH_MIRROR_PARTITION);
        s_storage_reset_latched = false;
    }
}

bool web_auth_store_ever_enabled(void)
{
    web_auth_ever_blob_t m;
    hal_status_t err = load_blob(WEB_AUTH_KEY_EVER, &m, sizeof(m));
    if (err == HAL_OK) {
        return true; // present, whatever the contents: fail closed
    }
    // AZ-02: only a true not-found means "never enabled"; any other error
    // (open failure, wrong size, I/O) fails closed as "was enabled".
    return err != HAL_NOT_FOUND;
}

static hal_status_t set_ever_enabled_marker(void)
{
    web_auth_ever_blob_t m = {WEB_AUTH_EVER_MAGIC, ~WEB_AUTH_EVER_MAGIC};
    return set_blob_verified(WEB_AUTH_KEY_EVER, &m, sizeof(m)); // read-back verified
}

bool web_auth_store_clear_policy_and_marker(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, WEB_AUTH_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL) != HAL_OK) {
        return false;
    }
    hal_status_t a = hal_kv_erase_key(&h, WEB_AUTH_KEY_POLICY);
    hal_status_t b = hal_kv_erase_key(&h, WEB_AUTH_KEY_EVER);
    hal_status_t c = hal_kv_commit(&h);
    bool gone = hal_kv_key_exists(&h, WEB_AUTH_KEY_POLICY) == HAL_NOT_FOUND &&
                hal_kv_key_exists(&h, WEB_AUTH_KEY_EVER) == HAL_NOT_FOUND;
    hal_kv_close(&h);
    lcd_pin_pending_refresh();
    return (a == HAL_OK || a == HAL_NOT_FOUND) && (b == HAL_OK || b == HAL_NOT_FOUND) && c == HAL_OK && gone;
}

web_auth_load_status_t web_auth_store_load_policy(web_auth_policy_t *out)
{
    memset(out, 0, sizeof(*out));
    web_auth_policy_blob_t blob;
    hal_status_t err = load_blob(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob));
    if (err != HAL_OK && err != HAL_NOT_FOUND) {
        return WEB_AUTH_LOAD_UNREADABLE; // AZ-02: transient read error must not read as auth off
    }
    if (err != HAL_OK) {
        // AU-02: ABSENT plus the ever-enabled marker means the record was lost
        // on a commissioned board: fail closed (UNREADABLE), never open.
        if (web_auth_store_ever_enabled() || s_storage_reset_latched) {
            return WEB_AUTH_LOAD_UNREADABLE; // FS-02: latched storage reset also fails closed
        }
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

    // AU-02: marker first, so a crash between the writes leaves marker-only
    // (fails closed), never an enabled policy without its marker.
    if (policy->web_enabled || policy->lcd_enabled) {
        hal_status_t mst = set_ever_enabled_marker();
        if (mst != HAL_OK) {
            return mst;
        }
    }
    hal_status_t pst = set_blob_verified(WEB_AUTH_KEY_POLICY, &blob, sizeof(blob));
    if (pst == HAL_OK) {
        lcd_pin_pending_refresh();
    }
    return pst;
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

web_auth_policy_transition_t web_auth_policy_check_transition(const web_auth_policy_t *current,
                                                                const web_auth_policy_t *requested,
                                                                bool admin_password_configured,
                                                                bool admin_pin_configured,
                                                                bool *out_clear_web_sessions,
                                                                bool *out_clear_lcd_session)
{
    bool clear_web = false;
    bool clear_lcd = false;

    if (out_clear_web_sessions != NULL) {
        *out_clear_web_sessions = false;
    }
    if (out_clear_lcd_session != NULL) {
        *out_clear_lcd_session = false;
    }

    if (current == NULL || requested == NULL) {
        return WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_WEB_CREDENTIAL; // never a valid call; pick
                                                                       // the stricter refusal so a
                                                                       // caller that ignores the
                                                                       // return value still fails
                                                                       // to persist anything
    }

    // The invariant is on `requested`, not the edge -- see header comment:
    // a policy record that reads enabled==true must never be persistable
    // without that interface's administrator credential already configured,
    // whether this is a fresh enable or a re-save of an already-enabled
    // record.
    if (requested->web_enabled && !admin_password_configured) {
        return WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_WEB_CREDENTIAL;
    }
    if (requested->lcd_enabled && !admin_pin_configured) {
        return WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_LCD_CREDENTIAL;
    }

    // Edge-triggered: only an actual off->on flip clears sessions (item 11:
    // "Enabling auth: every existing session is cleared" / "Disabling auth:
    // sessions become irrelevant but are kept"). A no-op re-save of an
    // already-enabled interface (e.g. changing only the other interface's
    // timeout) must not silently log anyone out.
    if (requested->web_enabled && !current->web_enabled) {
        clear_web = true;
    }
    if (requested->lcd_enabled && !current->lcd_enabled) {
        clear_lcd = true;
    }

    if (out_clear_web_sessions != NULL) {
        *out_clear_web_sessions = clear_web;
    }
    if (out_clear_lcd_session != NULL) {
        *out_clear_lcd_session = clear_lcd;
    }
    return WEB_AUTH_POLICY_TRANSITION_OK;
}

// --- Physical credential reset (item 10) -------------------------------------

static bool web_auth_store_clear_for_physical_reset_impl(void)
{
    // Item 4a fix (2026-09-17 adversarial review, 1179e2d3): this used to
    // clear only the web administrator PASSWORD record (WEB_AUTH_KEY_WEB),
    // leaving the LCD administrator PIN record (WEB_AUTH_KEY_LCD, a
    // separate blob) untouched. Failure sequence: operator forgets the LCD
    // admin PIN -> performs the documented physical four-corner reset
    // gesture, which exists exactly to recover from a forgotten credential
    // -> the web password clears, but the LCD PIN record is never revisited,
    // so the panel is still locked with the same forgotten PIN and the
    // gesture has not actually recovered anything for the LCD side. Both
    // records are cleared below, in place, one right after the other --
    // same "load existing, discard on absent/wrong-version/corrupt"
    // convention as web_auth_store_set_password()/web_auth_store_set_pin(),
    // except here a discard still produces a valid, empty blob rather than
    // an error: a physical reset must succeed even against a store that was
    // never written or was already corrupt.
    //
    // Deliberately does NOT touch the policy record. Plan section 10 is
    // explicit: this reset "does not disable authentication ... and does
    // not clear any config" -- the failure being recovered from is a
    // forgotten password, and silently unlocking the whole board (which an
    // earlier version of this function did, by erasing the policy key back
    // to WEB_AUTH_LOAD_ABSENT so web_auth_policy_effective_enabled()
    // collapsed to auth-off) is a bigger hole than the one being closed.
    // The policy is left exactly as the owner had it: if web auth was
    // enabled, it stays enabled, now with configured=false/must_change=true
    // on the administrator record. Section 11's login path owns treating
    // "enabled, no administrator credential configured" as a forced
    // set-a-new-password flow rather than a lockout or a silent bypass --
    // that state is reachable starting with this function, but implementing
    // that flow is not this function's job.
    web_auth_web_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    hal_status_t existing = load_blob(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    if (existing != HAL_OK || blob.version != WEB_AUTH_STORE_VERSION ||
        web_blob_crc(&blob) != blob.crc32) {
        memset(&blob, 0, sizeof(blob));
    }

    web_auth_password_record_t *admin = &blob.roles[WEB_AUTH_ROLE_ADMINISTRATOR];
    memset(admin, 0, sizeof(*admin));
    admin->must_change = true;  // item 10: a physical reset sets this true
    admin->configured = false;  // no password value invented/stored -- see
                                 // header comment.

    blob.version = WEB_AUTH_STORE_VERSION;
    blob.crc32 = web_blob_crc(&blob);

    hal_status_t err = set_blob_verified(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    if (err != HAL_OK) {
        return false;
    }

    // Read-back verify the administrator record landed as intended -- never
    // trust the write return code alone, same discipline as every other
    // setter in this file.
    web_auth_web_blob_t check;
    memset(&check, 0, sizeof(check));
    if (load_blob(WEB_AUTH_KEY_WEB, &check, sizeof(check)) != HAL_OK) {
        return false;
    }
    const web_auth_password_record_t *check_admin =
        &check.roles[WEB_AUTH_ROLE_ADMINISTRATOR];
    if (check_admin->configured != false || check_admin->must_change != true) {
        return false;
    }

    // Same treatment for the LCD administrator PIN, in the separate
    // WEB_AUTH_KEY_LCD blob -- the user PIN record is copied through
    // untouched, exactly like the web blob's user password above.
    // web_auth_pin_record_t has no must_change field (the LCD keypad has no
    // forced-change flow -- section 7 never asks for a PIN twice), so
    // configured=false is the entire clear.
    web_auth_lcd_blob_t lcd_blob;
    memset(&lcd_blob, 0, sizeof(lcd_blob));
    hal_status_t lcd_existing = load_blob(WEB_AUTH_KEY_LCD, &lcd_blob, sizeof(lcd_blob));
    if (lcd_existing != HAL_OK || lcd_blob.version != WEB_AUTH_STORE_VERSION ||
        lcd_blob_crc(&lcd_blob) != lcd_blob.crc32) {
        memset(&lcd_blob, 0, sizeof(lcd_blob));
    }

    web_auth_pin_record_t *lcd_admin = &lcd_blob.roles[WEB_AUTH_ROLE_ADMINISTRATOR];
    memset(lcd_admin, 0, sizeof(*lcd_admin));
    lcd_admin->configured = false;

    lcd_blob.version = WEB_AUTH_STORE_VERSION;
    lcd_blob.crc32 = lcd_blob_crc(&lcd_blob);

    hal_status_t lcd_err = set_blob_verified(WEB_AUTH_KEY_LCD, &lcd_blob, sizeof(lcd_blob));
    if (lcd_err != HAL_OK) {
        return false;
    }

    // Read-back verify the LCD administrator record landed as intended --
    // same discipline as the web blob above.
    web_auth_lcd_blob_t lcd_check;
    memset(&lcd_check, 0, sizeof(lcd_check));
    if (load_blob(WEB_AUTH_KEY_LCD, &lcd_check, sizeof(lcd_check)) != HAL_OK) {
        return false;
    }
    if (lcd_check.roles[WEB_AUTH_ROLE_ADMINISTRATOR].configured != false) {
        return false;
    }
    return true;
}

// WEB_AUTH_PLAN.md item 12b's "Clear login credentials" ADMIN-route action
// -- see this function's header-comment for the full rationale (distinct
// from both the physical reset gesture above and from factory_reset.c,
// which must never touch this namespace). Clears both roles in both blobs;
// deliberately leaves auth_policy untouched, same reasoning as
// web_auth_store_clear_for_physical_reset() above.
static bool web_auth_store_clear_all_credentials_impl(void)
{
    web_auth_web_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    hal_status_t existing = load_blob(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    if (existing != HAL_OK || blob.version != WEB_AUTH_STORE_VERSION ||
        web_blob_crc(&blob) != blob.crc32) {
        memset(&blob, 0, sizeof(blob));
    }

    web_auth_password_record_t *admin = &blob.roles[WEB_AUTH_ROLE_ADMINISTRATOR];
    memset(admin, 0, sizeof(*admin));
    admin->must_change = true; // same forced-set-a-new-password state as the
                                // physical reset (item 10)
    admin->configured = false;

    web_auth_password_record_t *user = &blob.roles[WEB_AUTH_ROLE_USER];
    memset(user, 0, sizeof(*user));
    user->configured = false; // the `user` record has no must_change flow

    blob.version = WEB_AUTH_STORE_VERSION;
    blob.crc32 = web_blob_crc(&blob);

    hal_status_t err = set_blob_verified(WEB_AUTH_KEY_WEB, &blob, sizeof(blob));
    if (err != HAL_OK) {
        return false;
    }

    web_auth_web_blob_t check;
    memset(&check, 0, sizeof(check));
    if (load_blob(WEB_AUTH_KEY_WEB, &check, sizeof(check)) != HAL_OK) {
        return false;
    }
    if (check.roles[WEB_AUTH_ROLE_ADMINISTRATOR].configured != false ||
        check.roles[WEB_AUTH_ROLE_ADMINISTRATOR].must_change != true ||
        check.roles[WEB_AUTH_ROLE_USER].configured != false) {
        return false;
    }

    web_auth_lcd_blob_t lcd_blob;
    memset(&lcd_blob, 0, sizeof(lcd_blob));
    hal_status_t lcd_existing = load_blob(WEB_AUTH_KEY_LCD, &lcd_blob, sizeof(lcd_blob));
    if (lcd_existing != HAL_OK || lcd_blob.version != WEB_AUTH_STORE_VERSION ||
        lcd_blob_crc(&lcd_blob) != lcd_blob.crc32) {
        memset(&lcd_blob, 0, sizeof(lcd_blob));
    }

    memset(&lcd_blob.roles[WEB_AUTH_ROLE_ADMINISTRATOR], 0, sizeof(lcd_blob.roles[WEB_AUTH_ROLE_ADMINISTRATOR]));
    lcd_blob.roles[WEB_AUTH_ROLE_ADMINISTRATOR].configured = false;
    memset(&lcd_blob.roles[WEB_AUTH_ROLE_USER], 0, sizeof(lcd_blob.roles[WEB_AUTH_ROLE_USER]));
    lcd_blob.roles[WEB_AUTH_ROLE_USER].configured = false;

    lcd_blob.version = WEB_AUTH_STORE_VERSION;
    lcd_blob.crc32 = lcd_blob_crc(&lcd_blob);

    hal_status_t lcd_err = set_blob_verified(WEB_AUTH_KEY_LCD, &lcd_blob, sizeof(lcd_blob));
    if (lcd_err != HAL_OK) {
        return false;
    }

    web_auth_lcd_blob_t lcd_check;
    memset(&lcd_check, 0, sizeof(lcd_check));
    if (load_blob(WEB_AUTH_KEY_LCD, &lcd_check, sizeof(lcd_check)) != HAL_OK) {
        return false;
    }
    if (lcd_check.roles[WEB_AUTH_ROLE_ADMINISTRATOR].configured != false ||
        lcd_check.roles[WEB_AUTH_ROLE_USER].configured != false) {
        return false;
    }
    return true;
}

// Both wipes bump the epoch before AND after, so a setter that captured the
// epoch at any point inside a wipe still sees it move (see s_cred_wipe_epoch).
bool web_auth_store_clear_for_physical_reset(void)
{
    (void)cred_store_lock(-1);
    cred_wipe_epoch_bump();
    bool ok = web_auth_store_clear_for_physical_reset_impl();
    cred_wipe_epoch_bump();
    cred_store_unlock();
    return ok;
}

bool web_auth_store_clear_all_credentials(void)
{
    (void)cred_store_lock(-1);
    cred_wipe_epoch_bump();
    bool ok = web_auth_store_clear_all_credentials_impl();
    cred_wipe_epoch_bump();
    cred_store_unlock();
    return ok;
}
