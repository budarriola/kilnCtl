// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. Every timer here is a no-op: the tests call
// wifi_prov.c's do_*() bodies directly rather than wifi_prov_start(), so no
// real timer ever needs to fire.
#ifndef TEST_STUB_ESP_TIMER_H
#define TEST_STUB_ESP_TIMER_H

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_timer_obj *esp_timer_handle_t;

typedef struct {
    void (*callback)(void *arg);
    void *arg;
    const char *name;
} esp_timer_create_args_t;

/* ET-03: forced esp_timer_create result (ESP_OK default) and call counter. */
static esp_err_t s_stub_timer_create_result = ESP_OK;
static int s_stub_timer_create_calls = 0;

static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    (void)args;
    static int dummy;
    s_stub_timer_create_calls++;
    if (s_stub_timer_create_result != ESP_OK) {
        return s_stub_timer_create_result;
    }
    if (out) {
        *out = (esp_timer_handle_t)&dummy;
    }
    return ESP_OK;
}

/* Per-TU counter, AUDIT_WIFI LOW-3 test: how many times a one-shot was armed. */
static int s_stub_timer_start_once_calls = 0;

/* OR-03: last one-shot timeout and a forced start result (ESP_OK default). */
static uint64_t s_stub_timer_last_timeout_us = 0;
static esp_err_t s_stub_timer_start_once_result = ESP_OK;

static inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    (void)timer;
    s_stub_timer_last_timeout_us = timeout_us;
    s_stub_timer_start_once_calls++;
    return s_stub_timer_start_once_result;
}

static inline esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    (void)timer;
    return ESP_OK;
}

static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    (void)timer;
    (void)period_us;
    return ESP_OK;
}

static inline esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    (void)timer;
    return ESP_OK;
}

/* Added for safety_cfg_store.c's host test (test_safety_cfg_store.c):
 * that file calls the real esp_timer_get_time() (a monotonic microsecond
 * clock) to compute "how long ago was this fetched" -- deterministic here,
 * a plain settable counter rather than a real clock, so a test can assert an
 * exact elapsed value instead of racing a wall clock. `static` (not extern):
 * each translation unit that includes this header gets its own independent
 * copy, same as every other file-scope stub state in this directory. */
static int64_t s_stub_esp_timer_now_us = 0;

static inline int64_t esp_timer_get_time(void)
{
    return s_stub_esp_timer_now_us;
}

static inline void esp_timer_test_set_now_us(int64_t us)
{
    s_stub_esp_timer_now_us = us;
}

#endif // TEST_STUB_ESP_TIMER_H
