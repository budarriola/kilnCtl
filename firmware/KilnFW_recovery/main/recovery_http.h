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

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_HTTP_H
