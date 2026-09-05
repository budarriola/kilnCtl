// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Every open always fails closed
// (ESP_ERR_NVS_NOT_FOUND) -- the tests exercise wifi_prov.c's in-RAM
// s_wifi.static_ip_confirmed state machine directly via its do_*() bodies,
// never through wifi_prov_start()'s real NVS load/save path, so persistence
// itself is out of scope here (nvs_save_*() failing is logged and
// non-fatal in every real call site, by design -- see wifi_prov.c).
#ifndef TEST_STUB_NVS_H
#define TEST_STUB_NVS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

typedef struct nvs_opaque *nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
} nvs_open_mode_t;

/* Added for safety_cfg_store.c's host test (test_safety_cfg_store.c): that
 * file's version-refuse-newer-blob test needs an actual round trip through
 * nvs_set_blob()/nvs_get_blob() to stage a blob, which the always-fails
 * default below cannot provide. Opt-in and OFF by default (s_stub_nvs_
 * enabled starts false) so every existing test that relies on "every open
 * fails closed" (wifi_prov.c's tests, kiln_cfg_store.c's, zones_http.c's) is
 * completely unaffected -- nothing here changes unless a test explicitly
 * calls nvs_test_enable(true). `static` (not extern): each translation unit
 * including this header gets its own independent copy, same as every other
 * file-scope stub state in this directory (see esp_timer.h's identical
 * note). One single blob slot only -- enough for one module's one key at a
 * time, which is all any test needs; a second concurrent key would need a
 * real per-(namespace,key) map this stub deliberately does not build. */
/* 6144 -> 7168 (2026-09-01, ZONES_CFG_VERSION 12->13, the tuning-quality
 * record: ZONES_CONFIG_BLOB_MAX_SIZE 640->768 grows kiln_cfg_store_blob_t by
 * 128 bytes * KILN_CFG_MAX_COUNT (8) = 1024 bytes, which no longer fits the
 * old 6144-byte slot). Originally sized so kiln_cfg_store.c's host test
 * (test_kiln_cfg_store.c) could round-trip a REAL full-size
 * kiln_cfg_store_blob_t (5420 bytes as of ZONES_CONFIG_BLOB_MAX_SIZE=640)
 * and kiln_cfg_store_blob_v1_t (4396 bytes) through this stub to exercise
 * nvs_load_store()'s migration path -- the exact path where those structs
 * previously lived as oversized function locals and blew app_main's stack.
 * A slot too small to hold them just makes that path silently untestable
 * again. */
/* 7168 -> 8192 (2026-09-04, ZONES_CFG_VERSION 18->19, PID_EXPANSION_PLAN.md
 * sec 3.6g: pid_fuzzy.c's two membership-band widths promoted to per-zone
 * config): ZONES_CONFIG_BLOB_MAX_SIZE 768->896 grows kiln_cfg_store_blob_t
 * by another 128 bytes * KILN_CFG_MAX_COUNT (8) = 1024 bytes (now 7468
 * bytes total, measured via sizeof() rather than hand-added -- see this
 * bump's own commit for the exact number), which again no longer fits the
 * old 7168-byte slot -- same failure mode as the 640->768 bump above,
 * caught by test_kiln_cfg_store.c's test_nvs_load_store_current_version_
 * full_size_happy_path() (three assertions there went red: nvs_set_blob()
 * itself refused the now-oversized blob, so nothing downstream had real
 * data to check). Rounded up to the next generous power-of-two-ish number
 * rather than the exact 7468, same "loose headroom, not a tight fit"
 * discipline the 6144->7168 bump used. */
static bool s_stub_nvs_enabled = false;
static uint8_t s_stub_nvs_blob[8192];
static size_t s_stub_nvs_blob_len = 0;
static bool s_stub_nvs_has_blob = false;

/* Added for adaptive_tune.c's host test (test_adaptive_tune.c): its opt-in
 * round-trip needs a REAL nvs_set_u8()/nvs_get_u8() round trip (the en_mask
 * byte), which the pre-existing always-write-nowhere/always-not-found
 * u8 stubs below could not provide -- same one-slot-is-enough reasoning as
 * the blob slot above, since this module uses exactly one key. Only takes
 * effect when nvs_test_enable(true) has been called, so every other test
 * relying on "u8 reads always fail closed" is unaffected. */
/* 1 -> TEST_STUB_NVS_U8_SLOTS (2026-09-01, adaptive_tune.c's opt-in-flag
 * migration host test): the module under test now writes TWO distinct u8
 * keys in the same namespace ('en_mask' and the new 'en_migrated' marker --
 * see adaptive_tune_migrate_enable_flags()). A single shared slot that
 * ignores `key` entirely (the original comment above's "one key at a time"
 * design) makes those two collide: writing en_migrated would silently
 * stomp en_mask's value and vice versa. Widened to a small fixed table,
 * looked up by key name, so each distinct key gets its own slot -- for the
 * common single-key case this behaves identically to before. */
#define TEST_STUB_NVS_U8_SLOTS 4
static char    s_stub_nvs_u8_keys[TEST_STUB_NVS_U8_SLOTS][24];
static uint8_t s_stub_nvs_u8_vals[TEST_STUB_NVS_U8_SLOTS];
static bool    s_stub_nvs_u8_has[TEST_STUB_NVS_U8_SLOTS]; /* slot holds a written value for its key */

static inline void nvs_test_enable(bool enable)
{
    s_stub_nvs_enabled = enable;
}

static inline void nvs_test_clear(void)
{
    s_stub_nvs_has_blob = false;
    s_stub_nvs_blob_len = 0;
    for (int i = 0; i < TEST_STUB_NVS_U8_SLOTS; i++) {
        s_stub_nvs_u8_has[i] = false;
        s_stub_nvs_u8_keys[i][0] = '\0';
        s_stub_nvs_u8_vals[i] = 0;
    }
}

static inline esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *out)
{
    (void)partition;
    (void)ns;
    (void)mode;
    if (!s_stub_nvs_enabled) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    static int dummy;
    if (out) {
        *out = (nvs_handle_t)&dummy;
    }
    return ESP_OK;
}

/* Added for run_state.c's/relay_cycles.c's host tests (test_run_state.c/
 * test_relay_cycles.c): both files' migrate_from_default_partition() opens
 * the DEFAULT NVS partition (plain nvs_open(), no partition argument) to
 * look for a pre-partition-split blob, in addition to their real writes
 * which go through nvs_open_from_partition(). Neither guard test under test
 * (persist_locked()) calls migrate_from_default_partition() -- only
 * run_state_init()/relay_cycles_init() do, and this stub only needs to
 * satisfy the linker for that unreached call, not model a second partition.
 * Forwards to the same single-slot store as nvs_open_from_partition() above. */
static inline esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    return nvs_open_from_partition(NULL, ns, mode, out);
}

static inline void nvs_close(nvs_handle_t h) { (void)h; }

static inline esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h;
    if (!s_stub_nvs_enabled) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    for (int i = 0; i < TEST_STUB_NVS_U8_SLOTS; i++) {
        if (s_stub_nvs_u8_has[i] && strcmp(s_stub_nvs_u8_keys[i], key) == 0) {
            if (out) {
                *out = s_stub_nvs_u8_vals[i];
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    (void)h;
    if (!s_stub_nvs_enabled) {
        return ESP_OK; /* pre-existing "always succeeds, nothing actually stored" behavior */
    }
    for (int i = 0; i < TEST_STUB_NVS_U8_SLOTS; i++) {
        if (s_stub_nvs_u8_has[i] && strcmp(s_stub_nvs_u8_keys[i], key) == 0) {
            s_stub_nvs_u8_vals[i] = val;
            return ESP_OK;
        }
    }
    for (int i = 0; i < TEST_STUB_NVS_U8_SLOTS; i++) {
        if (!s_stub_nvs_u8_has[i]) {
            strncpy(s_stub_nvs_u8_keys[i], key, sizeof(s_stub_nvs_u8_keys[i]) - 1);
            s_stub_nvs_u8_keys[i][sizeof(s_stub_nvs_u8_keys[i]) - 1] = '\0';
            s_stub_nvs_u8_vals[i] = val;
            s_stub_nvs_u8_has[i] = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM; /* stub out of slots -- widen TEST_STUB_NVS_U8_SLOTS if a future
                             * module needs more than 4 distinct u8 keys live at once */
}

static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    (void)key;
    (void)out;
    (void)len;
    return ESP_ERR_NVS_NOT_FOUND;
}

static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val)
{
    (void)h;
    (void)key;
    (void)val;
    return ESP_OK;
}

static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    (void)key;
    if (!s_stub_nvs_enabled || !s_stub_nvs_has_blob) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!len) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Real nvs_get_blob()'s documented size-query idiom: out==NULL just
     * reports the stored length via *len, no copy. kiln_cfg_store.c's
     * nvs_load_store() relies on exactly this to size its v1-vs-current
     * branch before reading the blob for real -- added when that path's
     * host test (test_kiln_cfg_store.c) found this stub previously treated
     * a NULL `out` as an error instead, which made every call into
     * nvs_load_store() silently take the "unreadable, defaults stand"
     * branch no matter what was staged. */
    if (!out) {
        *len = s_stub_nvs_blob_len;
        return ESP_OK;
    }
    if (*len < s_stub_nvs_blob_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, s_stub_nvs_blob, s_stub_nvs_blob_len);
    *len = s_stub_nvs_blob_len;
    return ESP_OK;
}

static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    (void)h;
    (void)key;
    if (!s_stub_nvs_enabled) {
        return ESP_OK; /* pre-existing "always succeeds, nothing actually stored" behavior */
    }
    if (len > sizeof(s_stub_nvs_blob)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_stub_nvs_blob, val, len);
    s_stub_nvs_blob_len = len;
    s_stub_nvs_has_blob = true;
    return ESP_OK;
}

/* Added 2026-08-22 for crash_report.c's host tests (crash_report_clear()
 * erases its NVS key). Same single-slot model as the rest of this stub: the
 * one blob slot is simply marked absent. */
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    (void)key;
    if (!s_stub_nvs_enabled) {
        return ESP_OK;
    }
    if (!s_stub_nvs_has_blob) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_stub_nvs_has_blob = false;
    s_stub_nvs_blob_len = 0;
    return ESP_OK;
}

#endif // TEST_STUB_NVS_H
