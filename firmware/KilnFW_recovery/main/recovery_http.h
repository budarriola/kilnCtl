// recovery_http.h -- the recovery image's HTTP surface
// (docs/OTA_SINGLE_SLOT.md section 3 item 2).
#ifndef RECOVERY_HTTP_H
#define RECOVERY_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts esp_http_server and registers the recovery routes. The image is
// UNAUTHENTICATED (owner decision 2026-10-02): the LCD-passphrase SoftAP is the
// only access control. Main routes:
//   GET  /                          (embedded self-contained page)
//   GET  /api/recovery/status       (unauthenticated JSON)
//   POST /api/recovery/exit  (409 if `app` invalid)
//   POST /api/recovery/wifi_reset
//   POST /api/ota/esp
//   GET  /api/partitions
//   GET  /api/boot_guard
//   POST /api/ota/esp/boot_guard_reset
//   POST /api/sw_reset
//
// Returns ESP_OK only if httpd_start succeeded AND every route registered. On
// any failure the server is stopped again and the error is logged; the LCD
// banner is the caller's job after its last attempt. A per-boot failed-attempt
// count and the last error ("http_start_fail" or "route_register_fail") are
// reported as http_start_attempts / http_last_error in /api/recovery/status;
// an error is returned so the caller can retry.
esp_err_t recovery_http_start(void);

// boot_guard counter clear (0 = cleared or not applicable). Used by the
// unattended-install pre-boot hook, same order as /api/recovery/exit.
int recovery_http_boot_guard_clear(void);

// Auto-exit helpers for a failed unattended install (recovery_install.c).
// recovery_http_app_verifies(): `app` holds a valid image AND passes the full esp_image_verify. It drops the
// cached verify result first (an apply may have rewritten `app` since it was cached).
bool recovery_http_app_verifies(void);
// Drops the cached verify result (call before `app` is erased or rewritten by a non-httpd task).
void recovery_http_app_verify_invalidate(void);
// Selects `app` as the boot target (set + read back) and schedules the restart. 0 = done; nonzero = nothing
// changed or the switch failed (stay in recovery). The caller clears boot_guard and the request first.
int recovery_http_select_app_and_restart(void);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_HTTP_H
