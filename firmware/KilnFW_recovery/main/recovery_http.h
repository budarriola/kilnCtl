// recovery_http.h -- the recovery image's HTTP surface
// (docs/OTA_SINGLE_SLOT_PLAN.md section 3 item 2).
#ifndef RECOVERY_HTTP_H
#define RECOVERY_HTTP_H

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
void recovery_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_HTTP_H
