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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The four per-item status values TODO.md 8.3 requires. Exposed here (rather
 * than staying file-static in readiness_http.c) only so the pure predicate
 * below can be host-tested without dragging in esp_http_server. */
typedef enum {
    READY_OK = 0,
    READY_NOT_DONE,
    READY_CANNOT_YET,
    READY_DELIBERATELY_OFF,
} readiness_status_t;

/* Pure decision for the "Safety processor commissioned" item, split out of
 * the handler so its logic can be tested on the host -- the handler around it
 * is all httpd plumbing and NVS reads that cannot run off-target.
 *
 * `cached_crc` is safety_cfg_store_cached_crc() (0 == never fetched, which is
 * never a real CRC), `unset_count`/`param_count` come from walking
 * safety_cfg_store_get_by_index(). `link_up` gates the never-fetched case:
 * with the safety link down the ESP genuinely cannot distinguish an
 * uncommissioned Pico from one it has simply not talked to yet, so that
 * combination is CANNOT_YET rather than an accusation of unfinished work.
 *
 * static inline (rather than a symbol in readiness_http.c) for the same
 * reason dashboard_safety_ready() is: readiness_http.c itself cannot compile
 * on the host, since it pulls in esp_http_server and the whole NVS stack. */
static inline readiness_status_t readiness_commissioning_status(bool link_up, uint16_t cached_crc,
                                                                size_t unset_count, size_t param_count)
{
    if (cached_crc == 0) {
        return link_up ? READY_NOT_DONE : READY_CANNOT_YET;
    }
    /* A zero param_count would make "all parameters have values" vacuously
     * true; treat it as not-done rather than showing a green light for a
     * table that does not exist. */
    if (param_count == 0) {
        return READY_NOT_DONE;
    }
    return (unset_count == 0) ? READY_OK : READY_NOT_DONE;
}

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
