// update_settings_http.h -- ADMIN routes for the persisted update repo
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 9, WP9). See update_settings.h.
//
//   GET  /api/update/settings  {"ok":true,"repo":"owner/name","default_repo":"...","is_default":bool}
//   POST /api/update/settings  form body `repo=owner/name`; an empty `repo=`
//                              resets to the default. 400 on an invalid repo,
//                              409 (system mode gate) while a firing/autotune
//                              is active. Both ROUTE_TIER_ADMIN.
#ifndef KILNCTL_UPDATE_SETTINGS_HTTP_H
#define KILNCTL_UPDATE_SETTINGS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers both routes on the shared httpd instance. Call after
// wifi_provision_http_start(). Independent of update_http_start() (which
// returns early on a board with no `stage` partition).
esp_err_t update_settings_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_SETTINGS_HTTP_H
