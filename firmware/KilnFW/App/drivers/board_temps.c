#include "board_temps.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "board_temps";

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt's KILNCTL_GZIP_ASSETS list -- same convention as
 * every other *_page.html in this component (diagnostics_http.c). */
extern const uint8_t board_temps_page_html_gz_start[] asm("_binary_board_temps_page_html_gz_start");
extern const uint8_t board_temps_page_html_gz_end[] asm("_binary_board_temps_page_html_gz_end");

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

    /* 2026-08-20 fix, found live on the bench: this file's original comment
     * (below, kept for the record) assumed the driver picks whichever
     * hardware range "best covers" the requested span. Reading the actual
     * installed IDF v6.0.2 source
     * (esp_driver_tsens/src/temperature_sensor.c's
     * temperature_sensor_choose_best_range()) shows that is wrong: it
     * requires the requested [range_min, range_max] to fall entirely INSIDE
     * one single hardware bucket
     * (esp_hal_ana_conv/esp32s3/temperature_sensor_periph.c's
     * temperature_sensor_attributes[] -- five fixed buckets, e.g. (20,100),
     * (-10,80), none of which contain [0,100] as a subset), or install()
     * fails outright with "Cannot select the correct range" / "Out of
     * testing range" -- exactly the error this board logged every boot.
     * (-10, 80) is used here: an exact match for one whole bucket (±1 degC
     * error, the second-best of the five), and realistically covers both a
     * cold-startup enclosure reading and a genuinely hot one before this
     * "is anything cooking itself" indicator needs to say so.
     *
     * Original comment, for the record (the "best covers" assumption in it
     * is the bug, not a description of intent worth preserving otherwise):
     * "Range picked wide (0-100 degC) rather than a tight span: this is a
     * board-health 'is anything cooking itself' indicator, not a
     * calibration-grade measurement, and app_main has no a-priori bound on
     * enclosure temperature worth encoding here." */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

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

/* Same content-negotiation shape as diagnostics_http.c's send_gz_page(): a
 * pure static page, no server-side data gathering of its own -- it polls
 * GET /api/board_temps client-side, same as diagnostics_page.html's own
 * "Board health" card. */
static esp_err_t board_temps_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "board_temps_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)board_temps_page_html_gz_start,
                            (size_t)(board_temps_page_html_gz_end - board_temps_page_html_gz_start));
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

    static const httpd_uri_t page_uri = {
        .uri = "/board_temps", .method = HTTP_GET, .handler = board_temps_page_get_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/board_temps) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "board_temps API and page up");
    return ESP_OK;
}
