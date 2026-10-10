// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests -- specifically to make the AP-fallback
// static-IP regression testable: esp_wifi_set_mode() is the ONLY thing that
// actually tears down (or fails to tear down) the fallback AP, so this stub
// tracks the last mode requested (g_stub_wifi_mode) in a plain global the
// test file reads directly. Everything else here is a no-op sufficient for
// wifi_prov.c to compile and link -- the tests never call wifi_prov_start()
// (the only path that would exercise scanning, AP-client-list, etc.).
#ifndef TEST_STUB_ESP_WIFI_H
#define TEST_STUB_ESP_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

typedef enum {
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA,
    WIFI_MODE_AP,
    WIFI_MODE_APSTA,
} wifi_mode_t;

typedef enum {
    WIFI_IF_STA = 0,
    WIFI_IF_AP,
} wifi_interface_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
} wifi_auth_mode_t;

typedef enum {
    WIFI_SCAN_TYPE_ACTIVE = 0,
} wifi_scan_type_t;

typedef struct {
    uint8_t ssid[33];
    uint8_t ssid_len;
    uint8_t channel;
    uint8_t max_connection;
    uint8_t password[64];
    wifi_auth_mode_t authmode;
} wifi_ap_config_t;

typedef struct {
    uint8_t ssid[33];
    uint8_t password[64];
    struct {
        wifi_auth_mode_t authmode;
    } threshold;
    struct {
        bool capable;
        bool required;
    } pmf_cfg;
} wifi_sta_config_t;

typedef union {
    wifi_ap_config_t ap;
    wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
    int dummy;
} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() \
    { \
        0 \
    }

/* Added for factory_reset.c's host tests --
 * docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md section 5 step
 * 1: esp_wifi_set_storage()/esp_wifi_restore() are the two calls
 * wifi_prov.c/factory_reset.c now make to stop and clear the IDF driver's own
 * persisted STA/AP config copy. */
typedef enum {
    WIFI_STORAGE_FLASH = 0,
    WIFI_STORAGE_RAM,
} wifi_storage_t;

extern int g_stub_wifi_set_storage_calls;
extern wifi_storage_t g_stub_wifi_last_storage;
extern int g_stub_wifi_restore_calls;

static inline esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    g_stub_wifi_set_storage_calls++;
    g_stub_wifi_last_storage = storage;
    return ESP_OK;
}

static inline esp_err_t esp_wifi_restore(void)
{
    g_stub_wifi_restore_calls++;
    return ESP_OK;
}

typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;

typedef struct {
    int dummy;
} wifi_sta_info_t;

typedef struct {
    wifi_sta_info_t sta[10];
    int num;
} wifi_sta_list_t;

typedef struct {
    uint8_t show_hidden;
    wifi_scan_type_t scan_type;
    struct {
        struct {
            uint32_t min;
            uint32_t max;
        } active;
    } scan_time;
} wifi_scan_config_t;

#define WIFI_EVENT_STA_START 0
#define WIFI_EVENT_STA_DISCONNECTED 1

/* The one piece of state the tests actually observe: whatever mode was last
 * requested via esp_wifi_set_mode(). WIFI_MODE_STA after a static join means
 * the fallback AP was torn down; WIFI_MODE_APSTA means it wasn't -- exactly
 * the regression this test suite exists to pin down. Plain (non-static)
 * globals so the single test_wifi_prov.c translation unit (which #includes
 * wifi_prov.c directly) can read/reset them between test cases. */
extern wifi_mode_t g_stub_wifi_mode;
extern int g_stub_wifi_set_mode_calls;

/* Controls esp_wifi_sta_get_ap_info()'s result -- defaults to "associated,
 * rssi -50" so do_ev_got_ip()'s RSSI capture doesn't fail; a test can force
 * ESP_FAIL to exercise the "not associated" branch if it ever needs to. */
extern esp_err_t g_stub_ap_info_result;
extern int8_t g_stub_ap_info_rssi;

static inline esp_err_t esp_wifi_init(const wifi_init_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

/* Last STA config pushed (function-local static so no TU needs a definition). */
static inline wifi_config_t *stub_wifi_last_sta_cfg(void)
{
    static wifi_config_t c;
    return &c;
}

static inline esp_err_t esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *cfg)
{
    if (iface == WIFI_IF_STA && cfg) {
        *stub_wifi_last_sta_cfg() = *cfg;
    }
    return ESP_OK;
}

static inline esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    g_stub_wifi_mode = mode;
    g_stub_wifi_set_mode_calls++;
    return ESP_OK;
}

static inline esp_err_t esp_wifi_get_mode(wifi_mode_t *mode)
{
    if (mode) {
        *mode = g_stub_wifi_mode;
    }
    return ESP_OK;
}

static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }

/* Call counter so a host test can assert do_add_network()/do_set_mode()
 * defer the actual join to start_sta_join() (called by owner_task() AFTER
 * the reply, per the reply-slot pool's out_join_after_reply flag) instead of
 * connecting synchronously inside the do_*() command body itself. */
extern int g_stub_wifi_connect_calls;
static inline esp_err_t esp_wifi_connect(void)
{
    g_stub_wifi_connect_calls++;
    return ESP_OK;
}

static inline esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }

static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info)
{
    if (info) {
        memset(info, 0, sizeof(*info));
        info->rssi = g_stub_ap_info_rssi;
    }
    return g_stub_ap_info_result;
}

static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg, int block)
{
    (void)cfg;
    (void)block;
    return ESP_OK;
}

static inline esp_err_t esp_wifi_scan_get_ap_num(uint16_t *num)
{
    if (num) {
        *num = 0;
    }
    return ESP_OK;
}

static inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t *num, wifi_ap_record_t *records)
{
    (void)records;
    if (num) {
        *num = 0;
    }
    return ESP_OK;
}

// 2026-09-28: controllable count (was hardcoded 0) so a host test can drive
// wifi_prov_get_ap_client_count()'s "a client is on the AP" branch, needed by
// wifi_prov_link.c's new ap_teardown_should_defer() (auth-off signal). See
// test_wifi_prov.c's ap_pending_teardown tests.
extern int g_stub_ap_sta_count;
static inline esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *list)
{
    if (list) {
        list->num = g_stub_ap_sta_count;
    }
    return ESP_OK;
}

#endif // TEST_STUB_ESP_WIFI_H
