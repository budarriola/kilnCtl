/* fake_wdt.c -- host fake backend for hal_wdt.h. See fake_wdt.h. */
#include "fake_wdt.h"

#include <string.h>

static bool     s_initialized = false;
static uint32_t s_timeout_ms = 0;
static bool     s_panic_disabled = false;
static uint32_t s_feed_count = 0;
static uint32_t s_elapsed_since_feed_ms = 0;
static bool     s_fired = false;
static bool     s_reboot_requested = false;

void fake_wdt_reset_all(void) {
    s_initialized = false;
    s_timeout_ms = 0;
    s_panic_disabled = false;
    s_feed_count = 0;
    s_elapsed_since_feed_ms = 0;
    s_fired = false;
    s_reboot_requested = false;
}

bool fake_wdt_is_initialized(void) {
    return s_initialized;
}

uint32_t fake_wdt_get_timeout_ms(void) {
    return s_timeout_ms;
}

bool fake_wdt_get_panic_disabled(void) {
    return s_panic_disabled;
}

uint32_t fake_wdt_get_feed_count(void) {
    return s_feed_count;
}

void fake_wdt_advance_ms(uint32_t ms) {
    if (!s_initialized) {
        return; /* un-armed watchdog cannot fire */
    }
    s_elapsed_since_feed_ms += ms;
    if (s_elapsed_since_feed_ms > s_timeout_ms) {
        s_fired = true;
    }
}

bool fake_wdt_fired(void) {
    return s_fired;
}

bool fake_wdt_reboot_requested(void) {
    return s_reboot_requested;
}

/* --- hal_wdt.h implementation --- */

hal_status_t hal_wdt_init(uint32_t timeout_ms, bool panic_disabled) {
    s_initialized = true;
    s_timeout_ms = timeout_ms;
    s_panic_disabled = panic_disabled;
    s_feed_count = 0;
    s_elapsed_since_feed_ms = 0;
    s_fired = false;
    return HAL_OK;
}

hal_status_t hal_wdt_set_panic_disabled(bool panic_disabled) {
    /* Mirrors the ESP backend's shape -- see fake_wdt.h's own comment on why
     * this fake picks ESP-shaped behavior (the one real production caller
     * is ESP-only): requires an already-armed watchdog. */
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    s_panic_disabled = panic_disabled;
    return HAL_OK;
}

hal_status_t hal_wdt_feed(void) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    s_feed_count++;
    s_elapsed_since_feed_ms = 0;
    s_fired = false;
    return HAL_OK;
}

void hal_wdt_reboot(void) {
    /* See fake_wdt.h's top comment: latches instead of accurately never
     * returning, so a host test can observe the call without hanging. */
    s_reboot_requested = true;
}
