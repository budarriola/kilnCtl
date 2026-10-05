// update_fetch.h -- the board fetches a GitHub release itself (docs/GITHUB_RELEASE_UPDATE_PLAN.md
// WP8): release metadata and the app image over HTTPS, streamed into the `stage` partition through
// the same stager the manual upload uses (update_stage.c).
//
// Routes (all ROUTE_TIER_ADMIN, registered by update_fetch_start()):
//   POST /api/update/check            start an async check of the latest release (no flash access)
//   POST /api/update/download         start an async download into the stage; query overrides:
//                                       allow_downgrade=1&confirm_downgrade=<tag>  (typed confirm)
//                                       allow_prerelease=1   force=1
//   GET  /api/update/fetch            job status / last result
//   POST /api/update/fetch/cancel     ask a running job to stop
//
// Two tasks, never on core 0 (gate (b) of the WP7 spike: a web login during ECDH starved IDLE0):
//   update_fetch     TLS + HTTP + JSON + sha256, PSRAM stack, no flash operations;
//   update_fetch_wr  flash writer for the stager, small internal stack (flash operations assert an
//                    internal-RAM stack), created per download.
//
// Policy: the manifest's identity goes through update_policy_decide() against the running image;
// an older version is refused unless the caller passes allow_downgrade plus confirm_downgrade equal
// to the release tag. The partitions_sha256 refusal is never overridable.
#ifndef KILNCTL_UPDATE_FETCH_H
#define KILNCTL_UPDATE_FETCH_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the four routes and the task-margin slots. Call from update_http_start() once the
// stager exists. Returns an error if a route cannot be registered.
esp_err_t update_fetch_start(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_FETCH_H
