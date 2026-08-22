#include "diagnostics_http.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "thermo_owner.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "diagnostics_http";

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component (see web_encoding.h's header comment for the flash-
 * budget reasoning that makes gzip the only stored representation). */
extern const uint8_t diagnostics_page_html_gz_start[] asm("_binary_diagnostics_page_html_gz_start");
extern const uint8_t diagnostics_page_html_gz_end[] asm("_binary_diagnostics_page_html_gz_end");
extern const uint8_t thermo_faults_page_html_gz_start[] asm("_binary_thermo_faults_page_html_gz_start");
extern const uint8_t thermo_faults_page_html_gz_end[] asm("_binary_thermo_faults_page_html_gz_end");
extern const uint8_t safety_page_html_gz_start[] asm("_binary_safety_page_html_gz_start");
extern const uint8_t safety_page_html_gz_end[] asm("_binary_safety_page_html_gz_end");

/* Same content-negotiation shape as readiness_http.c's page_get_handler():
 * web_client_accepts_gzip() covers the "no Accept-Encoding header" (legal,
 * served gzip per RFC 9110 s12.5.3) and "header present but excludes gzip"
 * (406, since this server keeps no uncompressed copy) cases; see
 * web_encoding.h for the full rationale. One tiny helper instead of three
 * near-identical handlers differing only in which *_gz_start/_end pair and
 * page name they use. */
static esp_err_t send_gz_page(httpd_req_t *req, const char *page_name, const uint8_t *start, const uint8_t *end)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, page_name);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t diagnostics_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "diagnostics_page.html", diagnostics_page_html_gz_start, diagnostics_page_html_gz_end);
}

static esp_err_t thermo_faults_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "thermo_faults_page.html", thermo_faults_page_html_gz_start,
                        thermo_faults_page_html_gz_end);
}

static esp_err_t safety_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "safety_page.html", safety_page_html_gz_start, safety_page_html_gz_end);
}

/* GET /api/thermo/faults -- dedicated per-channel data for
 * thermo_faults_page.html. Added 2026-08-21 to fix "the page just says
 * Loading forever":
 *
 * ROOT CAUSE (confirmed live against the board at 192.168.1.156 before this
 * fix): the page used to depend entirely on GET /api/status's combined
 * "channels" array. That endpoint (dashboard_http.c's status_get_handler())
 * calls MAX31856_read_all() directly, ONE call covering every channel in a
 * single HTTP request/response, bypassing thermo_owner's queue entirely. If
 * any single channel's SPI transaction wedges, the whole /api/status
 * response -- relays, safety link, heap, firmware version, everything, not
 * just the thermocouples -- never lands, and the page's fetch() has no
 * timeout of its own, so it sits on "Loading..." forever: not a bug in the
 * JSON, not an HTTP error the page mishandles, but a response that never
 * arrives at all. (Live right now the board answers fine -- all three
 * channels healthy, see this change's commit message for the actual curl
 * output -- so this could not be reproduced live in the failed state; the
 * fix below removes the failure mode regardless of whether a given poll
 * happens to catch it.)
 *
 * FIX: query each channel SEPARATELY through thermo_owner, which already
 * gives exactly the bounded-per-channel answer this page needs:
 * thermo_owner_command_read()'s post_and_wait() waits at most
 * THERMO_OWNER_WAIT_MS (200ms, thermo_owner.c) for that ONE channel's
 * answer, win or lose, regardless of what the owner task itself is doing.
 * A channel whose SPI transaction is wedged still bounds every OTHER
 * channel's query to that same 200ms -- it cannot come back healthy, but it
 * cannot hang the response either, and the wedged channel itself is
 * reported as "timeout" (a real, displayed result -- requirement: never
 * silently report a silent channel as healthy), not dropped from the
 * payload. Worst case for MAX31856_CHANNEL_COUNT channels queried serially,
 * with the owner task fully wedged on every one, is
 * MAX31856_CHANNEL_COUNT * THERMO_OWNER_WAIT_MS -- three channels today,
 * comfortably under a second, and bounded rather than open-ended either
 * way.
 *
 * SCOPE: reports every WIRED channel slot (0..MAX31856_CHANNEL_COUNT-1),
 * not zones_config's thermo_count -- this board's zones config currently
 * reports thermo_count=1 while three MAX31856 channels actually answer at
 * boot (device log). A wired channel with a real fault is exactly the case
 * this page exists to surface; gating the list by zone configuration would
 * hide it behind a setting that has nothing to do with the sensor being
 * faulted. "shown":"wired_channels" in the response says which policy is in
 * effect so the page can say so too, rather than leaving it ambiguous. */
static esp_err_t thermo_faults_get_handler(httpd_req_t *req)
{
    char json[1536];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    APPEND("{\"shown\":\"wired_channels\",\"channel_count\":%u,\"channels\":[",
          (unsigned)MAX31856_CHANNEL_COUNT);

    for (uint8_t ch = 0; ch < MAX31856_CHANNEL_COUNT; ch++) {
        MAX31856Reading reading;
        esp_err_t err = thermo_owner_command_read(ch, &reading);

        /* Four distinct, deliberately separate states -- collapsing
         * "timeout" into "spi_failed" (both leave reading.spi_failed true,
         * per thermo_owner_command_read()'s own never-stale contract) would
         * hide exactly the distinction the owner asked for: a channel that
         * never answered the owner task at all vs. one that answered with a
         * real SPI-level failure vs. one that was never wired up. */
        const char *state;
        if (err == ESP_ERR_TIMEOUT) {
            state = "timeout";
        } else if (err == ESP_ERR_NOT_FOUND) {
            state = "absent";
        } else if (reading.spi_failed) {
            state = "spi_failed";
        } else {
            state = "ok";
        }

        /* Same null-not-zero convention as dashboard_http.c's channels
         * array: NaN/UNKNOWN have no honest JSON numeric literal. */
        char cj_buf[16];
        if (isnan(reading.cj_temperature_c)) {
            snprintf(cj_buf, sizeof(cj_buf), "null");
        } else {
            snprintf(cj_buf, sizeof(cj_buf), "%.2f", (double)reading.cj_temperature_c);
        }
        char age_buf[16];
        if (reading.age_ms == MAX31856_READING_AGE_UNKNOWN) {
            snprintf(age_buf, sizeof(age_buf), "null");
        } else {
            snprintf(age_buf, sizeof(age_buf), "%lu", (unsigned long)reading.age_ms);
        }

        APPEND("%s{\"channel\":%u,\"state\":\"%s\",\"fault_status\":%u,\"fault_pin\":%s,"
              "\"cj_c\":%s,\"age_ms\":%s,\"stale\":%s}",
              ch == 0 ? "" : ",", (unsigned)ch, state, (unsigned)reading.fault_status,
              reading.fault_pin_asserted ? "true" : "false", cj_buf, age_buf,
              reading.stale ? "true" : "false");
    }
    APPEND("]}");

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
#undef APPEND
}

esp_err_t diagnostics_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t diagnostics_uri = {
        .uri = "/diagnostics", .method = HTTP_GET, .handler = diagnostics_page_get_handler,
    };
    static const httpd_uri_t thermo_faults_uri = {
        .uri = "/diagnostics/thermo", .method = HTTP_GET, .handler = thermo_faults_page_get_handler,
    };
    static const httpd_uri_t safety_uri = {
        .uri = "/safety", .method = HTTP_GET, .handler = safety_page_get_handler,
    };
    static const httpd_uri_t thermo_faults_api_uri = {
        .uri = "/api/thermo/faults", .method = HTTP_GET, .handler = thermo_faults_get_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &diagnostics_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/diagnostics) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &thermo_faults_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/diagnostics/thermo) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &safety_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/safety) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &thermo_faults_api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/thermo/faults) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "diagnostics/safety/thermo-faults pages up");
    return ESP_OK;
}
