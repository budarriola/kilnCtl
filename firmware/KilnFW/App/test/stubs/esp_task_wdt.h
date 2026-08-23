// Host-test stub -- see stubs/esp_err.h for why these exist. Added for
// watchdog_cfg.c's host tests (test_watchdog_cfg.c #includes watchdog_cfg.c
// directly, same convention as test_boot_guard.c). Only the pieces
// watchdog_cfg.c actually uses are stubbed: the config struct (matching the
// real esp_task_wdt.h field-for-field, since watchdog_cfg.c's
// build_twdt_config() is itself exercised by the host tests) and a
// reconfigure stub that always succeeds -- watchdog_cfg.c never asserts on
// its return value beyond logging, and no host test exercises the "TWDT not
// initialized" failure path (there is no real TWDT off-target to fail).
#ifndef TEST_STUB_ESP_TASK_WDT_H
#define TEST_STUB_ESP_TASK_WDT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timeout_ms;
    uint32_t idle_core_mask;
    bool trigger_panic;
} esp_task_wdt_config_t;

static inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *config)
{
    (void)config;
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif // TEST_STUB_ESP_TASK_WDT_H
