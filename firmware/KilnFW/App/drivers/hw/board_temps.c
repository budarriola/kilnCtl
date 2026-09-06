#include "board_temps.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#include "hal_esp_common.h"
#include "hal_sysinfo.h"

static const char *TAG = "board_temps";

/* 2026-09-06 migration (HW_ABSTRACTION.md "hal_time / hal_wdt / hal_pwm
 * / hal_sysinfo" item 4, "board_temps stays open" note): this module used to
 * own its own temperature_sensor_handle_t and call
 * temperature_sensor_install()/_enable()/_get_celsius() directly -- the same
 * ESP32-S3 on-die peripheral hal_sysinfo_esp.c's hal_sysinfo_temp_init()/
 * _read_celsius() family independently reproduced (see that file's
 * "OWNERSHIP HAZARD" comment, which named exactly this double-install risk
 * and asked whoever wired a hal_sysinfo_temp_* call site to pick one owner).
 * This is that reconciliation: board_temps.c is now the ONLY caller of the
 * hal_sysinfo_temp_* lifecycle, board_temps_start()/board_temps_get() below
 * carry the same init-once/read-many split and (-10, 80) range as before,
 * just through the HAL instead of driver/temperature_sensor.h directly, and
 * hal_sysinfo_esp.c's own install/enable/get_celsius sequence is now
 * load-bearing rather than dead/duplicate code. */
esp_err_t board_temps_start(void)
{
    /* hal_sysinfo_temp_init() is itself already-up-is-a-no-op (see
     * hal_sysinfo.h's threading contract and both backends' own comments),
     * so no separate readiness flag is kept here -- board_temps_get() below
     * asks the HAL each call whether a read is possible instead of caching
     * that state a second time in this file. */
    hal_status_t st = hal_sysinfo_temp_init();
    if (st != HAL_OK) {
        ESP_LOGW(TAG, "hal_sysinfo_temp_init failed: %s -- no ESP32-S3 die temp this boot",
                 esp_err_to_name(hal_status_to_esp_err(st)));
        return hal_status_to_esp_err(st);
    }

    ESP_LOGI(TAG, "ESP32-S3 internal temperature sensor up");
    return ESP_OK;
}

esp_err_t board_temps_get(board_temps_t *out, const MAX31856Reading *readings, size_t count)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    {
        float c = 0.0f;
        hal_status_t st = hal_sysinfo_temp_read_celsius(&c);
        if (st == HAL_OK) {
            out->esp32_valid = true;
            out->esp32_c = c;
        } else if (st != HAL_NOT_READY) {
            /* HAL_NOT_READY just means board_temps_start() was never called
             * or failed -- expected, not logged (esp32_valid stays false,
             * same silent-absent convention as a channel that never
             * answered in the MAX31856 loop below). Any other failure is a
             * read failure after a successful install/enable, unexpected
             * but not fatal to the rest of this call -- report esp32_valid
             * false and keep going, same "one bad field never blocks the
             * others" convention dashboard_http.c's status_get_handler()
             * uses for thermo channels. */
            ESP_LOGW(TAG, "hal_sysinfo_temp_read_celsius failed: %s",
                     esp_err_to_name(hal_status_to_esp_err(st)));
        }
    }

    if (readings && count > 0) {
        /* 2026-08-27 fix, found in the TODO.md audit: MAX31856_read_all()
         * fills readings[0..out_count) for *initialized* channels only,
         * packed by array POSITION, not by channel number (MAX31856.h's own
         * doc comment on that function) -- so with channel 0 dead,
         * readings[0] holds channel 1's reading, readings[1] holds channel
         * 2's, and so on. Indexing thermo_cj_valid[]/thermo_cj_c[] by the
         * loop position `i` (the old code here) therefore mislabeled every
         * channel above the first dead one AND never reported the dead
         * channel itself as absent -- it just silently disappeared instead
         * of showing up missing. ui_page_thermo_faults.c already gets this
         * right (see that file's header comment); this now follows the same
         * rule: index by MAX31856Reading::channel, never by array position.
         *
         * thermo_count is therefore always the full channel count when any
         * readings were supplied, not however many entries `readings`
         * happened to carry -- a channel the bus did not answer this poll is
         * left at its memset-zero default (thermo_cj_valid[ch] = false),
         * which is what "reported as absent" means for this struct. */
        out->thermo_count = MAX31856_CHANNEL_COUNT;
        size_t n = count < MAX31856_CHANNEL_COUNT ? count : MAX31856_CHANNEL_COUNT;
        for (size_t i = 0; i < n; i++) {
            const MAX31856Reading *r = &readings[i];
            if (r->channel >= MAX31856_CHANNEL_COUNT) {
                continue; /* defensive; channel is always 0..2 on this board */
            }
            /* Same validity test dashboard_http.c applies to tc_temperature_c:
             * a transport failure or a CJRANGE fault leaves cj_temperature_c
             * as NaN (MAX31856.h's MAX31856Reading doc comment), so both are
             * checked rather than trusting spi_failed alone. */
            bool valid = !r->spi_failed && !isnan(r->cj_temperature_c);
            out->thermo_cj_valid[r->channel] = valid;
            out->thermo_cj_c[r->channel] = valid ? r->cj_temperature_c : 0.0f;
        }
    }

    return ESP_OK;
}

/* --- live snapshot, borrowed thermo_bus pointer -------------------------- */

static struct {
    MAX31856BusClass *thermo_bus;
} s_bt;

/* board_temps.c is a hw-layer driver and must not depend on the HTTP/net
 * stack (HW_ABSTRACTION.md "drivers/ layering", item 3) -- registering
 * GET /api/board_temps and its JSON handler now lives in board_temps_http.c
 * instead, which calls this setter once at bring-up rather than this file
 * reaching up into esp_http_server.h/wifi_provision_http.h itself. Also
 * usable by any other non-HTTP caller that wants get_live() to see a bus
 * pointer (none today). */
void board_temps_bind_thermo_bus(MAX31856BusClass *thermo_bus_or_null)
{
    s_bt.thermo_bus = thermo_bus_or_null;
}

/* See board_temps.h's doc comment -- the plain-C getter extracted from
 * api_board_temps_get_handler() so a non-HTTP consumer (ui_page_board_health.c)
 * can get the same live snapshot without going through the JSON layer. */
void board_temps_get_live(board_temps_t *out)
{
    if (!out) {
        return;
    }

    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t count = 0;
    if (s_bt.thermo_bus && s_bt.thermo_bus->initialized) {
        MAX31856_read_all(s_bt.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
    }

    board_temps_get(out, count > 0 ? readings : NULL, count);
}
