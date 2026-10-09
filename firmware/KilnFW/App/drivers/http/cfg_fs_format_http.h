// cfg_fs_format_http -- the web-facing half of the "ask the user" leg of the
// owner decision on cfg_fs_mount.c's auto-format gate
// (docs/FILESYSTEM_USER_DATA.md section 5 step 1, 2026-09-07): when the
// `cfg` partition failed to mount AND the content scan found evidence of
// real data, cfg_fs_mount_device() refuses to format on its own and sets
// cfg_fs_mount_format_confirmation_pending() -- this file is how an operator
// SEES that refusal and how they explicitly confirm the erase.
//
// GET /api/cfgfs/format_pending -- unauthenticated status read, same
// convention as every other plain status endpoint in this codebase
// (/api/status, /api/cfgfs itself): {"pending":bool,"reason":"..."}.
//
// POST /api/cfgfs/format_confirm -- ROUTE_TIER_ADMIN, same tier as
// factory_reset.c's POST /api/factory_reset (route_tier_table.h): formatting
// the `cfg` partition on operator say-so is the identical danger tier as
// factory_reset.c's own scopes (irreversible, config-destroying). Until
// 2026-09-29 both routes also required a signed AP-password MAC under the
// shared OTA_HTTP_CONTEXT_FACTORY_RESET context; that scheme was retired
// (owner decision "Retire; open when login off") and ADMIN-tier login is now
// the only gate on both.
#ifndef CFG_FS_FORMAT_HTTP_H
#define CFG_FS_FORMAT_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers both routes on the shared httpd instance
// wifi_provision_http_get_server() already returned to every other *_start()
// in main_network_http.c. Call after wifi_provision_http_start().
esp_err_t cfg_fs_format_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_FORMAT_HTTP_H
