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
static bool s_stub_nvs_enabled = false;
static uint8_t s_stub_nvs_blob[2048];
static size_t s_stub_nvs_blob_len = 0;
static bool s_stub_nvs_has_blob = false;

static inline void nvs_test_enable(bool enable)
{
    s_stub_nvs_enabled = enable;
}

static inline void nvs_test_clear(void)
{
    s_stub_nvs_has_blob = false;
    s_stub_nvs_blob_len = 0;
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

static inline void nvs_close(nvs_handle_t h) { (void)h; }

static inline esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h;
    (void)key;
    (void)out;
    return ESP_ERR_NVS_NOT_FOUND;
}

static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    (void)h;
    (void)key;
    (void)val;
    return ESP_OK;
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
    if (!out || !len || *len < s_stub_nvs_blob_len) {
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

#endif // TEST_STUB_NVS_H
