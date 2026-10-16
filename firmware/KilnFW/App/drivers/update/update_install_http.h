// update_install_http.h -- ADMIN routes for the unattended install of the staged update.
//
//   POST /api/update/install  form body `confirm=1`. Refuses (409) unless the stage reads back
//                             VERIFIED and idle, and (via the recovery_boot gates) no firing/autotune
//                             runs, the OTA interlock is satisfied and no relay is energized. Stores a
//                             one-shot request in kiln_nvs (update_install_record.h), then boots the
//                             recovery image, which applies the stage and boots the application.
//   GET  /api/update/status   running version, stage state and the last install result.
//
// Both ROUTE_TIER_ADMIN. See docs/GITHUB_RELEASE_UPDATE_PLAN.md.
#ifndef KILNCTL_UPDATE_INSTALL_HTTP_H
#define KILNCTL_UPDATE_INSTALL_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers both routes. Call after update_http_start() (needs the stager).
esp_err_t update_install_http_start(void);

// Healthy confirmed boot: erase a leftover install request (never while an install/update claim is held).
// 0 = erased or none, 1 = skipped (claim busy), -1 = erase failed.
int update_install_http_drop_stale_request(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_INSTALL_HTTP_H
