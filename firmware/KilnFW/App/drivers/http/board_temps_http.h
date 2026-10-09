// board_temps_http -- the HTTP/net side of board_temps.c, split out per
// HW_ABSTRACTION.md "drivers/ layering" item 3: board_temps.c is a
// hw-layer driver (ESP32-S3 internal temperature sensor + MAX31856
// cold-junction aggregation) and must not itself depend on esp_http_server.h
// or wifi_provision_http.h. This file owns that dependency instead, and
// calls into board_temps.c's plain-C accessors (board_temps_bind_thermo_bus(),
// board_temps_get_live()) the same way dashboard_http.c calls
// dashboard_get_status().
#ifndef BOARD_TEMPS_HTTP_H
#define BOARD_TEMPS_HTTP_H

#include "esp_err.h"

#include "MAX31856.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /api/board_temps on the httpd instance
 * wifi_provision_http.c already started -- same "server must already exist"
 * precondition and non-fatal-to-app_main failure convention as
 * readiness_http_start()/dashboard_http_start(). `thermo_bus_or_null` is
 * passed straight through to board_temps_bind_thermo_bus(): NULL if the bus
 * never came up this boot (the handler then just reports thermo_cj_c: []),
 * otherwise board_temps_get_live() calls MAX31856_read_all() itself each
 * request -- this module does not own the bus, it only borrows the pointer.
 *
 * TODO.md 10.7: registers the JSON API. It used to also serve a styled GET
 * /board_temps page (board_temps_page.html); that page was removed
 * 2026-08-27 (owner request, "the board health page should be folded into
 * the diagnostics page") since it was a byte-for-byte duplicate of
 * diagnostics_page.html's own "Board health" card. No LCD/LVGL menu item --
 * that stays out of scope for this pass. */
esp_err_t board_temps_http_start(MAX31856BusClass *thermo_bus_or_null);

#ifdef __cplusplus
}
#endif

#endif // BOARD_TEMPS_HTTP_H
