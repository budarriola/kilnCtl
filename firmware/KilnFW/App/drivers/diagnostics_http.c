#include "diagnostics_http.h"

#include <stdint.h>

#include "esp_log.h"

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

    ESP_LOGI(TAG, "diagnostics/safety/thermo-faults pages up");
    return ESP_OK;
}
