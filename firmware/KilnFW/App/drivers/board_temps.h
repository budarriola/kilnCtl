// board_temps -- TODO.md 10.7's "onboard IC temperature" surface.
//
// This is board-HEALTH diagnostic data, deliberately kept separate from the
// kiln-process temperature that dashboard_http.c/zones_http.c report: mixing
// "how hot is the silicon" into the same JSON/page as "how hot is the kiln"
// is exactly what 10.7 says makes the main dashboard harder to read at a
// glance. Two real sources exist on this board today, and this module
// surfaces both without inventing a third:
//
//   1. The ESP32-S3's own internal die-temperature sensor peripheral,
//      brought up here via ESP-IDF's `driver/temperature_sensor.h` API
//      (temperature_sensor_install()/_enable()/_get_celsius()). Own,
//      init-once bring-up, same split as every other driver in this
//      directory (see NS2009_init vs NS2009_start vs NS2009_read):
//      board_temps_start() installs and enables the peripheral exactly
//      once; board_temps_get() just reads the last conversion.
//
//      *** Verified against the installed toolchain, 2026-08-20. *** The
//      function names/signatures/return codes below were confirmed against
//      the real installed `esp-idf/components/esp_driver_tsens/include/
//      driver/temperature_sensor.h` (IDF v6.0.2). One real bug was found
//      and fixed doing so: `board_temps_start()`'s original range request
//      (0-100 degC) failed `temperature_sensor_install()` on every boot on
//      real hardware ("Cannot select the correct range") -- the driver
//      requires the requested range to fall entirely inside one fixed
//      hardware bucket (`esp_hal_ana_conv/esp32s3/temperature_sensor_periph.c`),
//      not "best effort covers" as this file previously assumed. See
//      `board_temps_start()`'s own comment for the exact fix and the real
//      bucket table.
//
//   2. Each active MAX31856's own cold-junction temperature
//      (MAX31856Reading.cj_temperature_c) -- the IC's own local-ambient
//      reading, already computed by every MAX31856_read_all() call for the
//      thermocouple linearization math. Nothing new is read from hardware
//      for this half: board_temps_get() just accepts the same
//      MAX31856Reading array every other consumer (dashboard_http.c,
//      zones_http.c) already gets from MAX31856_read_all(), and republishes
//      the cj_temperature_c field under a board-health name instead of a
//      kiln-process one. This module does not touch the SPI bus and does
//      not own thermo_bus -- same "read-only aggregator over what already
//      exists" shape as readiness_http.c.
#ifndef BOARD_TEMPS_H
#define BOARD_TEMPS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "MAX31856.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One float per active MAX31856 channel, same channel count/order as
 * MAX31856_read_all()'s own readings[] -- see MAX31856.h. */
typedef struct {
    bool esp32_valid;                          /* false if the internal sensor
                                                  * never came up (board_temps_start()
                                                  * failed, or was never called) */
    float esp32_c;                             /* ESP32-S3 internal die temp, degC;
                                                  * only meaningful if esp32_valid */
    bool thermo_cj_valid[MAX31856_CHANNEL_COUNT]; /* indexed by MAX31856Reading::channel,
                                                  * NEVER by array position -- see
                                                  * board_temps_get()'s 2026-08-27 fix
                                                  * comment. A channel the bus did not
                                                  * answer this poll reads false here,
                                                  * at ITS OWN index, not some other
                                                  * channel's. */
    float thermo_cj_c[MAX31856_CHANNEL_COUNT]; /* per-channel MAX31856 cold-junction
                                                  * temp, degC; only meaningful where
                                                  * the matching *_valid entry is true */
    size_t thermo_count;                       /* MAX31856_CHANNEL_COUNT whenever any
                                                  * readings were supplied to
                                                  * board_temps_get() (0 only if
                                                  * readings/count were NULL/0) -- NOT
                                                  * "how many entries were populated";
                                                  * a missing channel is represented by
                                                  * thermo_cj_valid[that channel] being
                                                  * false, at its own index, not by a
                                                  * shorter count. */
} board_temps_t;

/* Installs and enables the ESP32-S3's internal temperature_sensor peripheral,
 * once. Safe to call with no MAX31856 bus up yet -- this half of the module
 * is entirely independent of thermo_bus. Non-fatal to app_main on failure,
 * same convention as every other driver bring-up call in main.c: logs and
 * returns the esp_err_t, and board_temps_get() reports esp32_valid=false
 * rather than crashing if this was never called or failed. */
esp_err_t board_temps_start(void);

/* Fills *out with the ESP32-S3's current internal die temperature (if
 * board_temps_start() succeeded) and the cold-junction temperature of every
 * reading in `readings[0..count)` (if any). Pure read -- takes a snapshot
 * the caller already has (e.g. from its own MAX31856_read_all() call) rather
 * than reading the SPI bus itself, same "don't duplicate hardware access"
 * rule dashboard_http.c/readiness_http.c already follow. `readings`/`count`
 * may be NULL/0 (e.g. no thermo_bus this boot) -- thermo_count comes back 0
 * and thermo_cj_valid[] is left all-false in that case. */
esp_err_t board_temps_get(board_temps_t *out, const MAX31856Reading *readings, size_t count);

/* TODO.md 10.1a's shared-backend seam, extracted the same way
 * dashboard_get_status() was pulled out of status_get_handler()
 * (dashboard_http.h) -- pure data-in-struct-out, zero httpd_req_t/JSON
 * dependency, so ui_page_board_health.c (the LCD side of this section) reads
 * the exact same numbers GET /api/board_temps serves instead of a second,
 * possibly-drifting read of the same hardware.
 *
 * Unlike board_temps_get() above (which only accepts readings the caller
 * already has), this does the live MAX31856_read_all() call itself, against
 * whichever bus pointer the most recent board_temps_http_start() call was
 * given -- the same "read the bus this module already borrowed a pointer to"
 * shape api_board_temps_get_handler() used before this was extracted from it.
 * Safe to call even if board_temps_http_start() was never reached (reads as
 * thermo_bus NULL, so thermo_count comes back 0) or before board_temps_start()
 * (esp32_valid comes back false) -- same "safe to call any time" convention
 * as dashboard_get_status(). Not free (it hits the SPI bus), but exactly as
 * expensive as GET /api/board_temps always was. */
void board_temps_get_live(board_temps_t *out);

/* Sets which MAX31856 bus board_temps_get_live() borrows a pointer to read
 * each call. `thermo_bus_or_null` is NULL if the bus never came up this boot
 * (get_live() then just reports thermo_count 0) -- this module does not own
 * the bus, it only borrows the pointer. Extracted so this hw-layer module
 * has no dependency on the HTTP/net stack (HW_ABSTRACTION_PLAN.md "drivers/
 * layering", item 3): board_temps_http.c's board_temps_http_start() calls
 * this once at bring-up instead of board_temps.c reaching up into
 * esp_http_server.h/wifi_provision_http.h itself. */
void board_temps_bind_thermo_bus(MAX31856BusClass *thermo_bus_or_null);

#ifdef __cplusplus
}
#endif

#endif // BOARD_TEMPS_H
