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
//      *** NOT VERIFIED AGAINST THE INSTALLED TOOLCHAIN THIS PASS. *** This
//      driver is written against the `temperature_sensor.h` surface as
//      documented for ESP-IDF v5.0+ (this project targets v6.0.2 per
//      README.md, and the API has been stable since its v5.0 introduction),
//      but no managed_components/ manifest or installed IDF header was
//      available to check against from inside this repo when this was
//      written -- there was no way to grep an actual
//      esp-idf/components/driver/temperature_sensor/include/ tree from here.
//      Function names/signatures/return codes should be treated as
//      "best available documentation," not "confirmed against this repo's
//      toolchain." Build this file first and fix on compiler error before
//      trusting it.
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
    bool thermo_cj_valid[MAX31856_CHANNEL_COUNT];
    float thermo_cj_c[MAX31856_CHANNEL_COUNT]; /* per-channel MAX31856 cold-junction
                                                  * temp, degC; only meaningful where
                                                  * the matching *_valid entry is true */
    size_t thermo_count;                       /* how many of the two arrays above
                                                  * are populated (<= MAX31856_CHANNEL_COUNT) */
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

/* Registers GET /api/board_temps on the httpd instance
 * wifi_provision_http.c already started -- same "server must already exist"
 * precondition and non-fatal-to-app_main failure convention as
 * readiness_http_start()/dashboard_http_start(). `thermo_bus_or_null` is
 * read the same way dashboard_http_start() takes it: NULL if the bus never
 * came up this boot (readiness handler then just reports thermo_cj_c: []),
 * otherwise the handler calls MAX31856_read_all() itself each request --
 * this module does not own the bus, it only borrows the pointer to read it.
 *
 * TODO.md 10.7 scope note: this registers the JSON API only. No HTML page
 * for it yet (deferred, per 10.7's own text, to whichever future pass builds
 * the board-health web page) and no LCD/LVGL menu item -- both explicitly
 * out of scope for this pass. */
esp_err_t board_temps_http_start(MAX31856BusClass *thermo_bus_or_null);

#ifdef __cplusplus
}
#endif

#endif // BOARD_TEMPS_H
