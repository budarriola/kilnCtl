// readiness_http -- TODO.md 8.3's "is this kiln ready to fire?" status page.
//
// This is a READ-ONLY aggregator, same spirit as nvs_report.c: it owns no
// config of its own and duplicates no storage. Every item on the checklist
// is computed by calling the read-only getters zones_http.h/profiles_http.h/
// wifi_prov.h/nvs_report.h/dashboard_http.h already expose to other
// consumers (profiles_http.c's feasibility check, profile_executor.c,
// autotune_engine.c, /api/status) -- this module just asks the same
// questions those consumers already ask and renders the answers as a
// checklist instead of gating a control decision on them.
//
// Serves GET /readiness (the page) and GET /api/readiness (the JSON the page
// polls). See readiness_http.c's status_get_handler() doc comment for the
// exact JSON shape and the four possible per-item status values.
#ifndef READINESS_HTTP_H
#define READINESS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /readiness + GET /api/readiness on the server
 * wifi_provision_http.c already started. No hardware pointers needed --
 * every hardware-adjacent fact (io_ready/thermo_ready/safety_ready) is read
 * through dashboard_http_get_hw_ready() rather than owned here. Call after
 * zones_http_start()/profiles_http_start()/wifi_prov_start()/
 * dashboard_http_start()/nvs_report_capture() -- this only reads what those
 * have already established, same ordering rule nvs_report_capture() itself
 * documents. */
esp_err_t readiness_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // READINESS_HTTP_H
