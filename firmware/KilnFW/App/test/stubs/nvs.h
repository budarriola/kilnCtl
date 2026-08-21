// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Every open always fails closed
// (ESP_ERR_NVS_NOT_FOUND) -- the tests exercise wifi_prov.c's in-RAM
// s_wifi.static_ip_confirmed state machine directly via its do_*() bodies,
// never through wifi_prov_start()'s real NVS load/save path, so persistence
// itself is out of scope here (nvs_save_*() failing is logged and
// non-fatal in every real call site, by design -- see wifi_prov.c).
#ifndef TEST_STUB_NVS_H
#define TEST_STUB_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct nvs_opaque *nvs_handle_t;

typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
} nvs_open_mode_t;

static inline esp_err_t nvs_open_from_partition(const char *partition, const char *ns, int mode, nvs_handle_t *out)
{
    (void)partition;
    (void)ns;
    (void)mode;
    (void)out;
    return ESP_ERR_NVS_NOT_FOUND;
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
    (void)out;
    (void)len;
    return ESP_ERR_NVS_NOT_FOUND;
}

static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    (void)h;
    (void)key;
    (void)val;
    (void)len;
    return ESP_OK;
}

#endif // TEST_STUB_NVS_H
