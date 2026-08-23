#include "diagnostics_http.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "crash_report.h"
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
    web_set_asset_cache_headers(req);
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

/* Own copy of dashboard_http.c's json_escape() -- that one is `static` to its
 * own TU, and duplicating six lines is cheaper (and matches this codebase's
 * existing precedent, e.g. run_state.c/ota_record.c/crash_report.c's three
 * independent nvs_partition_init() copies) than introducing a shared header
 * for one tiny helper. */
static void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* GET /api/crash_report -- the last-crash summary crash_report.c persisted
 * (task/cause/PC/backtrace/reset-reason), for the diagnostics page's "Last
 * crash" section. {"present":false} is a complete, valid response (the
 * common case: no crash on record) -- every other field is only present
 * alongside "present":true. */
static esp_err_t crash_report_get_handler(httpd_req_t *req)
{
    crash_report_record_t rec;
    bool present = crash_report_get(&rec);

    char json[768];
    size_t o = 0;
    int n;

    if (!present) {
        n = snprintf(json, sizeof(json), "{\"present\":false}");
        o = (n > 0) ? (size_t)n : 0;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, o);
    }

    char task_esc[sizeof(rec.exc_task) * 2 + 1];
    char cause_str_esc[sizeof(rec.exc_cause_str) * 2 + 1];
    char reset_reason_esc[sizeof(rec.reset_reason) * 2 + 1];
    json_escape(rec.exc_task, task_esc, sizeof(task_esc));
    json_escape(rec.exc_cause_str, cause_str_esc, sizeof(cause_str_esc));
    json_escape(rec.reset_reason, reset_reason_esc, sizeof(reset_reason_esc));

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    APPEND("{\"present\":true,\"acknowledged\":%s,\"exc_cause\":%lu,\"exc_cause_str\":\"%s\","
          "\"exc_pc\":\"0x%08lx\",\"exc_addr\":\"0x%08lx\",\"exc_task\":\"%s\","
          "\"found_on_boot_reset_reason\":\"%s\","
          "\"backtrace\":[",
          rec.acknowledged ? "true" : "false", (unsigned long)rec.exc_cause, cause_str_esc,
          (unsigned long)rec.exc_pc, (unsigned long)rec.exc_addr, task_esc, reset_reason_esc);
    for (uint8_t i = 0; i < rec.bt_count && i < CRASH_REPORT_BT_MAX; i++) {
        /* Hex strings, not JSON numbers: these are code addresses, and the
         * only thing anyone does with them is paste them into addr2line.
         * Decimal ("299") is unusable for that and reads as a plausible
         * small integer rather than the obviously-wrong address it is. */
        APPEND("%s\"0x%08lx\"", i == 0 ? "" : ",", (unsigned long)rec.backtrace_pc[i]);
    }
    APPEND("],\"backtrace_corrupted\":%s}", rec.bt_corrupted ? "true" : "false");

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
#undef APPEND
}

/* POST /api/crash_report/ack -- operator has seen the record, stop showing
 * it as new. Does NOT erase the coredump/record -- see /clear for that. */
static esp_err_t crash_report_ack_post_handler(httpd_req_t *req)
{
    bool ok = crash_report_acknowledge();
    const char *json = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no crash record to acknowledge\"}";
    httpd_resp_set_type(req, "application/json");
    if (!ok) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return httpd_resp_send(req, json, strlen(json));
}

/* POST /api/crash_report/clear -- acknowledge AND erase the coredump image
 * plus this module's own NVS record, freeing the `coredump` partition slot
 * for the next crash. */
static esp_err_t crash_report_clear_post_handler(httpd_req_t *req)
{
    esp_err_t err = crash_report_clear();
    char json[128];
    int n;
    if (err == ESP_OK) {
        n = snprintf(json, sizeof(json), "{\"ok\":true}");
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (n > 0) ? (size_t)n : 0);
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
    static const httpd_uri_t crash_report_api_uri = {
        .uri = "/api/crash_report", .method = HTTP_GET, .handler = crash_report_get_handler,
    };
    static const httpd_uri_t crash_report_ack_uri = {
        .uri = "/api/crash_report/ack", .method = HTTP_POST, .handler = crash_report_ack_post_handler,
    };
    static const httpd_uri_t crash_report_clear_uri = {
        .uri = "/api/crash_report/clear", .method = HTTP_POST, .handler = crash_report_clear_post_handler,
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
    err = httpd_register_uri_handler(server, &crash_report_api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/crash_report) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &crash_report_ack_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/crash_report/ack) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &crash_report_clear_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/crash_report/clear) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "diagnostics/safety/thermo-faults pages up");
    return ESP_OK;
}
