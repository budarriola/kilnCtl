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

#endif // TEST_STUB_ESP_ERR_H
