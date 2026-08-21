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

static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    (void)args;
    static int dummy;
    if (out) {
        *out = (esp_timer_handle_t)&dummy;
    }
    return ESP_OK;
}

static inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    (void)timer;
    (void)timeout_us;
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

#endif // TEST_STUB_ESP_TIMER_H
