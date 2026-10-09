// adaptive_tune_http -- HTTP surface for adaptive_tune.c's Phase 7d
// continuous/adaptive tuning feature. Own file, not zones_http.c/
// dashboard_json.c: both were owned by parallel work in flight when this
// was added (same "own file, register on the shared httpd instance"
// pattern log_http.c/readiness_http.c/diagnostics_http.c already use).
// This file talks to adaptive_tune.c ONLY through its public accessors
// (adaptive_tune_get_enabled/set_enabled/get_status) -- no access to that
// module's internals, so it carries none of the run-end/tick logic and none
// of the safety property (gains only ever change at a run boundary, never
// mid-firing) -- that property lives entirely in adaptive_tune.c and is
// unaffected by where its HTTP wrapper lives.
#ifndef ADAPTIVE_TUNE_HTTP_H
#define ADAPTIVE_TUNE_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /api/adaptive_tune (status, all zones) and POST
 * /api/adaptive_tune/enable (form body "zone=<n>&enabled=<0|1>") on the
 * shared httpd instance (wifi_provision_http_get_server() -- must already be
 * running, same precondition as log_http_start()/readiness_http_start()).
 * Call once at boot, after adaptive_tune_init() has had a chance to load the
 * persisted opt-in mask (main.c calls both from app_main's task; order
 * between them does not matter for correctness -- adaptive_tune_get_status()/
 * get_enabled() are safe to call before adaptive_tune_init() runs, they just
 * read the struct-zero "not yet loaded" default, same as an unconfigured
 * zone -- but calling this after keeps the very first HTTP response
 * consistent with what NVS actually holds). */
esp_err_t adaptive_tune_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // ADAPTIVE_TUNE_HTTP_H
