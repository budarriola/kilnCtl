// recovery_health_policy.h -- pure come-up health decisions of the recovery
// image, free of ESP-IDF types so a host test can exercise them
// (check_recovery_health.ps1). The image must "reliably work and detect
// problems": a Wi-Fi or HTTP failure must never be reported as "up".
#ifndef RECOVERY_HEALTH_POLICY_H
#define RECOVERY_HEALTH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

// After this many WIFI_EVENT_AP_STOP events the SoftAP is considered
// unrecoverable and the (stateless) image restarts.
#define RHEALTH_AP_STOP_RESTART_LIMIT 3u

// Bounded attempts at recovery_http_start() before the image restarts.
#define RHEALTH_HTTP_START_ATTEMPTS 3

static inline bool rhealth_ap_stop_should_restart(uint32_t ap_stop_count)
{
    return ap_stop_count >= RHEALTH_AP_STOP_RESTART_LIMIT;
}

// The HTTP server came up only if httpd_start returned ESP_OK (0) AND every
// expected route registered.
static inline bool rhealth_http_ok(int start_rc, unsigned registered, unsigned expected)
{
    return start_rc == 0 && expected > 0 && registered == expected;
}

// After a failed HTTP bring-up the image restarts at most this many times (a
// failure that repeats every boot must not loop forever: it then stays up with
// the error on the LCD so JTAG and the screen are stable). The count lives in
// RTC_NOINIT memory and starts fresh on a power cycle.
#define RHEALTH_HTTP_MAX_RESTARTS 3u

static inline bool rhealth_http_restart_allowed(uint32_t restarts_so_far)
{
    return restarts_so_far < RHEALTH_HTTP_MAX_RESTARTS;
}

// Wi-Fi: restarts the image after repeated SoftAP failures share the same cap
// (a radio fault that recurs every boot must not loop forever either).
static inline bool rhealth_wifi_restart_allowed(uint32_t wifi_restarts_so_far)
{
    return wifi_restarts_so_far < RHEALTH_HTTP_MAX_RESTARTS;
}

typedef enum {
    RHEALTH_AP_RERAISE = 0,   // call esp_wifi_start() again
    RHEALTH_AP_RESTART_IMAGE, // esp_restart() (counted, capped)
    RHEALTH_AP_STAY_DOWN      // cap hit: stay up with "AP DOWN" on the LCD
} rhealth_ap_action_t;

// What to do after an unexpected AP_STOP.
static inline rhealth_ap_action_t rhealth_ap_stop_action(uint32_t ap_stop_count,
                                                         uint32_t wifi_restarts_so_far)
{
    if (!rhealth_ap_stop_should_restart(ap_stop_count)) {
        return RHEALTH_AP_RERAISE;
    }
    return rhealth_wifi_restart_allowed(wifi_restarts_so_far) ? RHEALTH_AP_RESTART_IMAGE
                                                              : RHEALTH_AP_STAY_DOWN;
}

// What to do when esp_wifi_start() itself failed after an AP_STOP (no further
// event will arrive, so this failure must feed the capped restart path).
static inline rhealth_ap_action_t rhealth_ap_reraise_failed_action(uint32_t wifi_restarts_so_far)
{
    return rhealth_wifi_restart_allowed(wifi_restarts_so_far) ? RHEALTH_AP_RESTART_IMAGE
                                                              : RHEALTH_AP_STAY_DOWN;
}

// RTC_NOINIT memory is garbage after a power-on reset (and may be after other
// resets): the stored count is trusted only on a non-power-on reset with a
// valid magic.
static inline uint32_t rhealth_restart_count_at_boot(bool power_on_reset, bool magic_ok,
                                                     uint32_t stored)
{
    return (power_on_reset || !magic_ok) ? 0u : stored;
}

// Whether the main task may log "recovery image up".
static inline bool rhealth_all_up(bool wifi_up, bool lcd_ok, bool http_ok)
{
    return wifi_up && lcd_ok && http_ok;
}

#endif // RECOVERY_HEALTH_POLICY_H
