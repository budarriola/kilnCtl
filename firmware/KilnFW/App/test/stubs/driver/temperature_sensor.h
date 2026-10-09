// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for board_temps.c's host test (test_board_temps.c), which
// #includes board_temps.c directly to reach board_temps_get() (the pure
// half of that file has no other seam). board_temps_start() -- and
// therefore this header's install/enable/get_celsius calls -- is never
// exercised by that test (it never calls board_temps_start(), so
// s_tsens_ready stays false and board_temps_get() takes the esp32_valid =
// false path), but the whole of board_temps.c still has to COMPILE and
// LINK as one translation unit, so every symbol it names must resolve.
//
// Declared here only, matching the real esp_driver_tsens header's shape
// closely enough for board_temps.c's call sites to typecheck; DEFINED in
// test_board_temps.c itself (same "declared once here, defined once per
// test file" split as stubs/esp_http_server.h uses).
#ifndef TEST_STUB_DRIVER_TEMPERATURE_SENSOR_H
#define TEST_STUB_DRIVER_TEMPERATURE_SENSOR_H

#include "esp_err.h"

typedef struct temperature_sensor_obj *temperature_sensor_handle_t;

typedef struct {
    int range_min;
    int range_max;
    int clk_src;
} temperature_sensor_config_t;

/* Real macro takes (min, max) and fills in a default clock source; the
 * exact clk_src value is never inspected by anything here, so 0 is fine. */
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(min_val, max_val) \
    { .range_min = (min_val), .range_max = (max_val), .clk_src = 0 }

esp_err_t temperature_sensor_install(const temperature_sensor_config_t *cfg, temperature_sensor_handle_t *out);
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t h);
esp_err_t temperature_sensor_disable(temperature_sensor_handle_t h);
esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t h);
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t h, float *out_c);

#endif // TEST_STUB_DRIVER_TEMPERATURE_SENSOR_H
