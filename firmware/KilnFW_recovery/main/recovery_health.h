// recovery_health.h -- restart counters that survive esp_restart() so a failure
// that recurs every boot cannot restart the recovery image forever
// (policy: recovery_health_policy.h). Counters live in RTC_NOINIT memory, start
// at 0 after a power-on reset, and are zeroed by every deliberate restart
// (recovery_exit, sw_reset, OTA reboot: any esp_restart() not made through
// recovery_health_restart_for_*()), so an old count never leaks into a later
// recovery session.
#ifndef RECOVERY_HEALTH_H
#define RECOVERY_HEALTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once, early in app_main(): validates the counters and registers the
// shutdown handler that zeroes them on deliberate restarts.
void recovery_health_boot_init(void);

uint32_t recovery_health_http_restarts(void);
uint32_t recovery_health_wifi_restarts(void);

// Zero the HTTP counter (the image came up, or is staying up for good).
void recovery_health_clear_http(void);

// Zero the Wi-Fi counter (the SoftAP has proven stable, or the image is
// staying up with the AP down for good). Call it AFTER the restart decision.
void recovery_health_clear_wifi(void);

// Count a failure-driven restart, keep the count across it, and restart.
void recovery_health_restart_for_http(void) __attribute__((noreturn));
void recovery_health_restart_for_wifi(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_HEALTH_H
