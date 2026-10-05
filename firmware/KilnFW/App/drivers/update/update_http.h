// update_http.h -- the HTTP + flash/SHA wiring around update_stage.c
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP4). All three routes are
// ROUTE_TIER_ADMIN (route_tier_table.h):
//
//   POST /api/update/stage        upload an application image (raw body,
//                                 Content-Length required) into the `stage`
//                                 partition. Optional headers X-Stage-Version
//                                 (semver; defaults to the image's own app
//                                 descriptor version) and X-Stage-Commit (40
//                                 lowercase hex).
//   POST /api/update/stage/clear  erase the stage header (nothing staged).
//   GET  /api/update/stage        {"ok":true,"staged":bool,"reason":...,...}.
//
// Refusal order for the two writers, same as every other wired call site:
// system_mode_gate (SYS_ACTION_STAGE_WRITE, 409 while a firing/autotune is
// active) first, then ota_http_check_interlocks(), then the single
// cross-processor update claim (409 while any other OTA/stage operation is in
// flight). The body is drained after every refusal.
#ifndef KILNCTL_UPDATE_HTTP_H
#define KILNCTL_UPDATE_HTTP_H

#include "esp_err.h"

#include "update_stale_stage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the three routes on the shared httpd instance. Call after
// wifi_provision_http_start(). ESP_ERR_NOT_FOUND when the board's partition
// table has no `stage` partition (pre-WP2 layout): no routes are registered.
esp_err_t update_http_start(void);

// Boot-time stale-stage cleanup (OT-G06, update_stale_stage.h): when the
// stage is VERIFIED and is byte-identical to the RUNNING image (a recovery
// apply cut between set_boot and the header erase), clear the stage header.
// `app_marked_valid` must be true ONLY if esp_ota_mark_app_valid_cancel_rollback()
// returned ESP_OK this boot (the caller's own result: esp_ota_get_state_partition()
// is not trusted here, it reads otadata[0] which is not the active entry on
// this single-slot table); false returns immediately. Call after boot_guard is
// cleared. It hashes the whole app image WITHOUT the update claim (a few
// seconds, yielding), then waits up to ~30 s for the claim/gates; run it from
// the existing low-priority ota_confirm task, never from boot. The result is
// boot-scoped and reported as "boot_auto_clear"/"boot_auto_cleared" in
// GET /api/update/stage.
update_stale_result_t update_http_stale_stage_check(bool app_marked_valid);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_HTTP_H
