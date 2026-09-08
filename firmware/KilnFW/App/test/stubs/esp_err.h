// Host-test stub for ESP-IDF's esp_err.h.
//
// profile_feasibility.c is pure math, but its headers (profiles_http.h,
// zones_http.h, MAX31856.h) are real firmware headers that declare ESP-IDF
// typed functions. Nothing in the host tests CALLS those functions -- we only
// need the declarations to parse -- so these stubs supply the types and
// nothing else. They live under App/test/stubs/ and are reached via /I, so
// they can never shadow a real header for a real (on-target) build.
#ifndef TEST_STUB_ESP_ERR_H
#define TEST_STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

/* 2026-08-21: added for the wifi_prov.c host tests (test_wifi_prov.c), which
 * pull in wifi_prov.c's real ESP-IDF error-code vocabulary. Values are
 * arbitrary but distinct -- nothing compares these against the real ESP-IDF
 * numeric values, only against each other within this host build. */
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_PART_NOT_FOUND 0x1103
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1104
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1105
#define ESP_ERR_WIFI_CONN 0x3008
#define ESP_ERR_WIFI_MODE 0x3009
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5001
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5002
/* Added for safety_link.c's host build. */
#define ESP_ERR_INVALID_RESPONSE 0x108

/* Named lookups only for the handful of codes host tests actually assert
 * the NAME of (e.g. test_cfg_fs_status.c's GET /api/cfgfs "error" field) --
 * everything else still falls back to the old generic "ERR" so no other
 * test's expectations shift. Not a claim of parity with the real ESP-IDF
 * esp_err_to_name() table, which is far larger. */
static inline const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    default: return "ERR";
    }
}

#endif // TEST_STUB_ESP_ERR_H
