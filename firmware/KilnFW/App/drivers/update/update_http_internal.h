// update_http_internal.h -- the few update_http.c internals the GitHub fetch path (update_fetch.c)
// reuses so it applies the same refusals as the manual stage upload.
#ifndef KILNCTL_UPDATE_HTTP_INTERNAL_H
#define KILNCTL_UPDATE_HTTP_INTERNAL_H

#include <stdbool.h>

#include "esp_http_server.h"
#include "update_stage.h"

#ifdef __cplusplus
extern "C" {
#endif

// The single stager instance (valid once update_http_start() has run).
update_stage_t *update_http_stage(void);

// The mode gate alone (409 while a firing or autotune runs), for a route that must not claim the
// update slot (the release check). Returns true (response already sent) when refused.
bool update_http_mode_gate_refuses(httpd_req_t *req, const char *what, const char *ip);

// Mode gate, then OTA interlock, then the update claim, in the manual upload's order. Returns true
// (response already sent) when refused. On false the claim is HELD: the caller releases it with
// ota_http_update_end().
bool update_http_gate_refuses(httpd_req_t *req, const char *what, const char *ip);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_HTTP_INTERNAL_H
