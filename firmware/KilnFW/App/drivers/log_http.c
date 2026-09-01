#include "log_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "log_store.h"
#include "wifi_provision_http.h"

static const char *TAG = "log_http";

/* Memory budget: ONE line buffer, sized at LOG_STORE_MAX_LINE_LEN (matches
 * telemetry_log.c's own TELEMETRY_LOG_LINE_BUF-derived worst case), and
 * allocated from PSRAM (MALLOC_CAP_SPIRAM) rather than the request handler's
 * own internal-DRAM stack -- same discipline as the rest of this project
 * since the internal-DRAM-exhaustion incident that truncated /app.js (see
 * project memory "ESP internal DRAM exhaustion"). No other buffer: the
 * response is streamed one httpd_resp_send_chunk() at a time straight from
 * log_store_reader_next(), never assembled in memory first. */
static esp_err_t stream_kind(httpd_req_t *req, log_store_kind_t kind)
{
    httpd_resp_set_type(req, "text/plain");

    char *line = (char *)heap_caps_malloc(LOG_STORE_MAX_LINE_LEN + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    log_store_reader_t *rd = log_store_reader_open(kind);
    if (!rd) {
        free(line);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "log store unavailable");
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    while (err == ESP_OK && log_store_reader_next(rd, line, LOG_STORE_MAX_LINE_LEN)) {
        size_t n = strnlen(line, LOG_STORE_MAX_LINE_LEN);
        line[n] = '\n';
        err = httpd_resp_send_chunk(req, line, n + 1);
    }

    log_store_reader_close(rd);
    free(line);

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); /* terminates the chunked response */
    }
    return ESP_OK;
}

static esp_err_t firing_get_handler(httpd_req_t *req)
{
    return stream_kind(req, LOG_STORE_KIND_FIRING);
}

static esp_err_t autotune_get_handler(httpd_req_t *req)
{
    return stream_kind(req, LOG_STORE_KIND_AUTOTUNE);
}

esp_err_t log_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t firing_uri = {
        .uri = "/api/logs/firing", .method = HTTP_GET, .handler = firing_get_handler,
    };
    static const httpd_uri_t autotune_uri = {
        .uri = "/api/logs/autotune", .method = HTTP_GET, .handler = autotune_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &firing_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/logs/firing) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/logs/autotune) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "log read-back API up (/api/logs/firing, /api/logs/autotune)");
    return ESP_OK;
}
