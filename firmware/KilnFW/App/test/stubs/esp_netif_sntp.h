// Host-test stub for esp_netif_sntp.h -- ESP-IDF's SNTP client wrapper.
// Added for time_sync.c's host tests (docs/FILESYSTEM_USER_DATA_PLAN.md item
// 14, TZ move task): time_sync.c was the one MOVE-item module with no host
// stub at all before this pass, blocking it from the project's usual
// "test through the real production function" standard. This is a thin,
// deliberately inert stand-in -- same convention as esp_netif.h/esp_wifi.h
// in this directory: just enough surface for time_sync.c to compile and
// link, no real SNTP behavior (no host clock, no network) is simulated.
//
// SCOPE: only the handful of symbols time_sync.c actually references --
// esp_sntp_config_t (start/wait_for_sync/sync_cb, the three fields it sets),
// ESP_NETIF_SNTP_DEFAULT_CONFIG(), esp_netif_sntp_init(),
// esp_netif_sntp_start(). NOT a general SNTP API stub -- if a future caller
// needs a field this struct does not have, add it deliberately rather than
// widening this to mirror the real (much larger) esp_sntp_config_t blindly.
#ifndef TEST_STUB_ESP_NETIF_SNTP_H
#define TEST_STUB_ESP_NETIF_SNTP_H

#include <stdbool.h>

#include "esp_err.h"

// time_sync.c's sync_cb both takes a `struct timeval *` AND dereferences
// tv->tv_sec, so (unlike a pointer-only use) this needs a full definition,
// not a forward declaration. lwip/sockets.h (this same stub directory)
// defines the identical tag for wifi_prov.c's TU -- the two never land in
// the same translation unit today (time_sync.c does not include
// lwip/sockets.h and vice versa), so there is no ODR conflict; the guard
// below is defensive in case that ever changes.
#ifndef KILNCTL_TEST_STUB_TIMEVAL_DEFINED
#define KILNCTL_TEST_STUB_TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);

typedef struct {
    bool start;
    bool wait_for_sync; // never set true by time_sync.c -- see its header's
                          // "never blocks on a sync" contract.
    sntp_sync_time_cb_t sync_cb;
    const char *server; // holds whatever ESP_NETIF_SNTP_DEFAULT_CONFIG()'s
                          // argument was -- host tests never send a real
                          // NTP request, so nothing reads this back, but a
                          // named field beats silently discarding the
                          // caller's argument.
} esp_sntp_config_t;

#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) \
    { .start = true, .wait_for_sync = true, .sync_cb = NULL, .server = (host) }

// Test-only fault injection: the value esp_netif_sntp_init() returns. Defaults
// to ESP_OK; a host test sets it non-OK to drive time_sync_start()'s
// sntp-init-failure return, then restores ESP_OK. Per-TU (static), which is
// enough because test_time_sync.c #includes time_sync.c into its own TU.
static inline esp_err_t *esp_netif_sntp_stub_init_result(void)
{
    static esp_err_t result = ESP_OK;
    return &result;
}

static inline esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    (void)config;
    return *esp_netif_sntp_stub_init_result(); // host tests never actually run the SNTP state machine
}

static inline esp_err_t esp_netif_sntp_start(void)
{
    return ESP_OK;
}

#endif // TEST_STUB_ESP_NETIF_SNTP_H
