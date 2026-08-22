// diagnostics_http -- the web twins of three LCD pages that, per UI_PLAN.md's
// "page structure rework" section, have had no web equivalent at all:
// ui_page_diagnostics.c (ESP-only system info), ui_page_safety.c (safety
// processor link), and ui_page_thermo_faults.c (per-channel MAX31856 fault
// detail). UI_PLAN.md's 2026-08-20 decision record explicitly merges the
// LCD's diagnostics + board-health split into ONE web page ("the web can
// scroll, so the ~264px split that forced them apart on the panel doesn't
// apply") while keeping thermo-faults separate, so this module registers
// three routes, not four:
//
//   GET /diagnostics          -- ui_page_diagnostics.c + ui_page_board_health.c
//   GET /diagnostics/thermo   -- ui_page_thermo_faults.c
//   GET /safety               -- ui_page_safety.c, plus the DIAG/TRIP_EVENT
//                                 detail the LCD's ~264px budget had no room
//                                 for (already on GET /api/status, per
//                                 UI_PLAN.md section 5)
//   GET /api/thermo/faults    -- per-channel MAX31856 fault/CJ/timeout data
//                                 for /diagnostics/thermo, added 2026-08-21
//                                 (see diagnostics_http.c's
//                                 thermo_faults_get_handler() doc comment for
//                                 the full root-cause writeup)
//
// /diagnostics and /safety are pure static pages with no server-side data
// gathering of their own -- every number they show comes from GET /api/status
// (dashboard_http.c) and GET /api/board_temps (board_temps.c), same
// "one reader, existing owner" shape as readiness_page.html reading GET
// /api/readiness. /diagnostics/thermo is the one exception: it used to lean
// on GET /api/status too, but that endpoint answers every field (relays,
// safety link, heap, thermocouples, everything) in one HTTP response built
// from one MAX31856_read_all() call, so a single wedged channel could leave
// the whole response -- and the thermo-faults page's "Loading..." -- stuck
// forever. GET /api/thermo/faults exists so this page can query each channel
// independently through thermo_owner, which bounds every channel's answer to
// THERMO_OWNER_WAIT_MS regardless of what any other channel is doing.
#ifndef DIAGNOSTICS_HTTP_H
#define DIAGNOSTICS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the three page routes above on the httpd instance
 * wifi_provision_http.c already started. Takes no hardware pointers --
 * unlike dashboard_http_start()/ota_http_start(), these are pure static
 * pages with no server-side data gathering of their own, same shape as
 * readiness_http_start(). Non-fatal to app_main on failure, same convention
 * as every other *_http_start() in this directory: logs and returns the
 * esp_err_t, and a failed registration just means those routes 404 this
 * boot rather than app_main refusing to come up. */
esp_err_t diagnostics_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // DIAGNOSTICS_HTTP_H
