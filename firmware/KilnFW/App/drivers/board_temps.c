#include "board_temps.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "wifi_provision_http.h"

static const char *TAG = "board_temps";

/* Install-once/enable-once handle. NULL until board_temps_start() succeeds --
 * same init-once/read-many split as every other driver here (NS2009_start
 * vs NS2009_read, MAX31856_start_all vs MAX31856_read_all). */
static temperature_sensor_handle_t s_tsens = NULL;
static bool s_tsens_ready = false;

esp_err_t board_temps_start(void)
{
    if (s_tsens_ready) {
        /* Already up -- board_temps_start() being called twice (e.g. a
         * future retry path) is a no-op, not an error. */
        return ESP_OK;
    }

    /* Range picked wide (0-100 degC) rather than a tight span: this is a
     * board-health "is anything cooking itself" indicator, not a
     * calibration-grade measurement, and app_main has no a-priori bound on
     * enclosure temperature worth encoding here. Per the temperature_sensor.h
     * doc comment (as documented for IDF v5.0+ -- see this file's header
     * comment on why that could not be checked against the actual installed
     * toolchain this pass), the driver internally selects the on-die
     * measurement range/attenuation that best covers whatever span is
     * requested. */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(0, 100);

    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "temperature_sensor_install failed: %s -- no ESP32-S3 die temp this boot",
                 esp_err_to_name(err));
        s_tsens = NULL;
        return err;
    }

    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "temperature_sensor_enable failed: %s -- no ESP32-S3 die temp this boot",
                 esp_err_to_name(err));
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        return err;
    }

    s_tsens_ready = true;
    ESP_LOGI(TAG, "ESP32-S3 internal temperature sensor up");
    return ESP_OK;
}

esp_err_t board_temps_get(board_temps_t *out, const MAX31856Reading *readings, size_t count)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (s_tsens_ready && s_tsens) {
        float c = 0.0f;
        esp_err_t err = temperature_sensor_get_celsius(s_tsens, &c);
        if (err == ESP_OK) {
            out->esp32_valid = true;
            out->esp32_c = c;
        } else {
            /* Read failure after a successful install/enable is unexpected
             * but not fatal to the rest of this call -- report esp32_valid
             * false and keep going, same "one bad field never blocks the
             * others" convention dashboard_http.c's status_get_handler()
             * uses for thermo channels. */
            ESP_LOGW(TAG, "temperature_sensor_get_celsius failed: %s", esp_err_to_name(err));
        }
    }

    if (readings && count > 0) {
        size_t n = count < MAX31856_CHANNEL_COUNT ? count : MAX31856_CHANNEL_COUNT;
        for (size_t i = 0; i < n; i++) {
            const MAX31856Reading *r = &readings[i];
            /* Same validity test dashboard_http.c applies to tc_temperature_c:
             * a transport failure or a CJRANGE fault leaves cj_temperature_c
             * as NaN (MAX31856.h's MAX31856Reading doc comment), so both are
             * checked rather than trusting spi_failed alone. */
            bool valid = !r->spi_failed && !isnan(r->cj_temperature_c);
            out->thermo_cj_valid[i] = valid;
            out->thermo_cj_c[i] = valid ? r->cj_temperature_c : 0.0f;
        }
        out->thermo_count = n;
    }

    return ESP_OK;
}

/* --- GET /api/board_temps ------------------------------------------------ */

static struct {
    MAX31856BusClass *thermo_bus;
} s_bt;

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

/* {"esp32_c": <float or null>, "thermo_cj_c": [<float or null>, ...]} --
 * null (not 0) for a field that has no valid reading, unlike
 * dashboard_http.c's status_get_handler() which reports 0 alongside a
 * separate valid:false flag for its channels array. Chosen here instead
 * because this endpoint has no per-field companion validity flag in the
 * shape TODO.md 10.7 asked for -- "or null" is explicit in the spec this
 * was built against, so null is the one signal a client can check without
 * a second field to cross-reference. */
static esp_err_t api_board_temps_get_handler(httpd_req_t *req)
{
    board_temps_t bt;
    board_temps_get_live(&bt);

    char json[256];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                             \
    do {                                                                                         \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                  \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                            \
            goto send;                                                                           \
        }                                                                                         \
        o += (size_t)n;                                                                           \
    } while (0)

    APPEND("{\"esp32_c\":");
    if (bt.esp32_valid) {
        APPEND("%.2f", (double)bt.esp32_c);
    } else {
        APPEND("null");
    }
    APPEND(",\"thermo_cj_c\":[");
    for (size_t i = 0; i < bt.thermo_count; i++) {
        if (bt.thermo_cj_valid[i]) {
            APPEND("%s%.2f", i == 0 ? "" : ",", (double)bt.thermo_cj_c[i]);
        } else {
            APPEND("%snull", i == 0 ? "" : ",");
        }
    }
    APPEND("]}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

esp_err_t board_temps_http_start(MAX31856BusClass *thermo_bus_or_null)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    s_bt.thermo_bus = thermo_bus_or_null;

    static const httpd_uri_t api_uri = {
        .uri = "/api/board_temps", .method = HTTP_GET, .handler = api_board_temps_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/board_temps) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "board_temps API up");
    return ESP_OK;
}
