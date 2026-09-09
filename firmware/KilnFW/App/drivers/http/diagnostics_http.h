// diagnostics_http -- the web twins of three LCD pages that, per UI_PLAN.md's
// "page structure rework" section, have had no web equivalent at all:
// ui_page_diagnostics.c (ESP-only system info), ui_page_safety.c (safety
// processor link), and ui_page_thermo_faults.c (per-channel MAX31856 fault
// detail). UI_PLAN.md's 2026-08-20 decision record explicitly merges the
// LCD's diagnostics + board-health split into ONE web page ("the web can
// scroll, so the ~264px split that forced them apart on the panel doesn't
// apply"); the thermo-faults detail originally got its own route
// (/diagnostics/thermo) instead, but the owner asked 2026-08-27 for it to be
// folded into /diagnostics too ("most of the information is there anyway"),
// so this module now registers two page routes, not three:
//
//   GET /diagnostics          -- ui_page_diagnostics.c + ui_page_board_health.c
//                                 + ui_page_thermo_faults.c (folded in 2026-08-27)
//   GET /safety               -- ui_page_safety.c, plus the DIAG/TRIP_EVENT
//                                 detail the LCD's ~264px budget had no room
//                                 for (already on GET /api/status, per
//                                 UI_PLAN.md section 5)
//   GET /api/thermo/faults    -- per-channel MAX31856 fault/CJ/timeout data,
//                                 added 2026-08-21 (see diagnostics_http.c's
//                                 thermo_faults_get_handler() doc comment for
//                                 the full root-cause writeup). Still its own
//                                 endpoint after the 2026-08-27 page fold --
//                                 only the PAGE that used to live at
//                                 /diagnostics/thermo was removed; the route
//                                 that route rendered is now served from
//                                 /diagnostics's own "Thermocouple faults"
//                                 section instead.
//
// /diagnostics and /safety are pure static pages with no server-side data
// gathering of their own -- every number they show comes from GET /api/status
// (dashboard_http.c), GET /api/board_temps (board_temps.c) and GET
// /api/thermo/faults, same "one reader, existing owner" shape as
// readiness_page.html reading GET /api/readiness. The thermo-faults section
// is the one exception: it used to lean on GET /api/status too (before it had
// its own route), but that endpoint answers every field (relays, safety
// link, heap, thermocouples, everything) in one HTTP response built from one
// MAX31856_read_all() call, so a single wedged channel could leave the whole
// response -- and this section's "Loading..." -- stuck forever. GET
// /api/thermo/faults exists so this section can query each channel
// independently through thermo_owner, which bounds every channel's answer to
// THERMO_OWNER_WAIT_MS regardless of what any other channel is doing.
#ifndef DIAGNOSTICS_HTTP_H
#define DIAGNOSTICS_HTTP_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure state-selection logic pulled out of thermo_faults_get_handler()'s
 * safety-processor block (diagnostics_http.c) after 3ba65080's negative-test
 * verification audit found the "not_converting" state's own negative test
 * (test_safety_tc_diagnostics.js) was vacuous -- it fed a hand-built
 * `state` string straight into the page renderer and never touched the
 * logic below that actually derives "ok"/"faulted"/"not_converting" from
 * the wire data. Pulled out `static inline`, same pattern as this header's
 * own dashboard_safety_ready() in dashboard_http.h, specifically so it is
 * host-testable (test_diagnostics_safety_tc_state.c) without standing up
 * httpd/safety_link/thermo_owner.
 *
 * 2026-09-08 (safety_tc_warn_mask_disagreement audit): the old two-way
 * "not_converting" bucket collapsed two genuinely different hardware states
 * into one honest-but-vague label -- see this repo's docs/audits/
 * safety_tc_warn_mask_disagreement_2026-09-08.md. Now that SaftyFW's V3
 * status frame carries cj_valid independently of temp_valid (flags2 bit 1,
 * SAFETY_LINK_STATUS_FLAG2_CJ_VALID, safety_link.h), this function can tell
 * them apart when the peer has actually reported it:
 *   faulted         -- tc_temp_c is a real (non-NaN) reading AND a
 *                       THERMO_FAULT_* bit is set: the MAX31856 completed a
 *                       conversion and is reporting a genuine fault -- chip
 *                       alive, probe is the problem.
 *   probe_fault     -- tc_temp_c is NaN, fault==0, BUT cj_valid_known is
 *                       true and cj_valid is true (a real, finite cj_temp_c
 *                       from the SAME on-chip sensor, a successful transfer
 *                       just completed): the chip is alive and converting,
 *                       only the external thermocouple probe/wiring/
 *                       commissioned-type verification is the problem
 *                       (matches the project's documented "safety TC invalid
 *                       is one CR1 byte" failure mode).
 *   not_converting  -- tc_temp_c is NaN with fault==0, and EITHER cj_valid
 *                       is not known (an older Pico, or an ESP that hasn't
 *                       confirmed V3 support yet -- no cj-side information
 *                       exists to rule out a dead chip) OR cj_valid is known
 *                       and false (cj_temp_c is ALSO NaN -- the chip itself
 *                       never completed a conversion, e.g. DRDY silence).
 *                       This is the state that cost hours the night this was
 *                       written.
 *   ok              -- tc_temp_c is a real reading and fault==0.
 * `cj_valid_known`/`cj_valid` default to false/false when the caller has no
 * V3 frame (safety_link_status_t's own "false/meaningless until proven
 * otherwise" convention) -- passing those defaults reproduces the exact old
 * two-way behaviour, so this is a strict refinement, not a behaviour change
 * for a peer that has never sent V3. */
static inline const char *diag_safety_tc_state(double tc_temp_c, uint32_t tc_fault,
                                                double cj_temp_c, bool cj_valid_known,
                                                bool cj_valid)
{
    bool temp_valid = !isnan(tc_temp_c);
    if (temp_valid) {
        return (tc_fault != 0u) ? "faulted" : "ok";
    }
    bool chip_alive = cj_valid_known && cj_valid && !isnan(cj_temp_c);
    return chip_alive ? "probe_fault" : "not_converting";
}

/* Registers the three page routes above on the httpd instance
 * wifi_provision_http.c already started. Takes no hardware pointers of its
 * own for the pages themselves -- unlike dashboard_http_start()/
 * ota_http_start(), these are pure static pages with no server-side data
 * gathering, same shape as readiness_http_start(). `safety` is the one
 * exception: GET /api/diagnostics/timing (HW_ABSTRACTION.md "Still open")
 * reads safety_link_get_stats()'s link_reply_us_* fields directly, so this
 * needs the same NULL-tolerant safety pointer dashboard_http_start() takes
 * (ctx->safety_err == ESP_OK ? &ctx->safety : NULL at the call site) --
 * NULL just means that block of the timing response reports all zeros/
 * count 0 rather than a real measurement, same as any other reading taken
 * before the safety link came up. Non-fatal to app_main on failure, same
 * convention as every other *_http_start() in this directory: logs and
 * returns the esp_err_t, and a failed registration just means those routes
 * 404 this boot rather than app_main refusing to come up. */
esp_err_t diagnostics_http_start(SafetyLinkClass *safety);

#ifdef __cplusplus
}
#endif

#endif // DIAGNOSTICS_HTTP_H
