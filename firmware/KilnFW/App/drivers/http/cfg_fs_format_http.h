// cfg_fs_format_http -- the web-facing half of the "ask the user" leg of the
// owner decision on cfg_fs_mount.c's auto-format gate
// (docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 1, 2026-09-07): when the
// `cfg` partition failed to mount AND the content scan found evidence of
// real data, cfg_fs_mount_device() refuses to format on its own and sets
// cfg_fs_mount_format_confirmation_pending() -- this file is how an operator
// SEES that refusal and how they explicitly confirm the erase.
//
// GET /api/cfgfs/format_pending -- unauthenticated status read, same
// convention as every other plain status endpoint in this codebase
// (/api/status, /api/cfgfs itself): {"pending":bool,"reason":"..."}.
//
// POST /api/cfgfs/format_confirm -- authenticated with the SAME
// challenge/response scheme and the SAME context (OTA_HTTP_CONTEXT_
// FACTORY_RESET) factory_reset.c's POST /api/factory_reset already uses,
// deliberately NOT a new context: formatting the `cfg` partition on
// operator say-so is the identical danger tier as factory_reset.c's own
// scopes (irreversible, config-destroying, requires a signed confirmation),
// and TODO.md 8.1's "no default scope, explicit choice only" reasoning
// applies here just as directly. A MAC signed for "factory-reset" already
// authorizes wiping zone/Wi-Fi/profile config; authorizing an erase of the
// filesystem that (per docs/FILESYSTEM_USER_DATA_PLAN.md) increasingly
// backs that very same config is not a meaningfully different grant, so
// this reuses that lockout budget rather than standing up a fifth
// independent one for a single narrow action.
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
