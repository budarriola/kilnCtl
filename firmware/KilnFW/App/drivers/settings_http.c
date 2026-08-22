#include "settings_http.h"

#include <stdint.h>

#include "esp_log.h"

#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "settings_http";

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component (see web_encoding.h's header comment for the flash-
 * budget reasoning that makes gzip the only stored representation). */
extern const uint8_t settings_page_html_gz_start[] asm("_binary_settings_page_html_gz_start");
extern const uint8_t settings_page_html_gz_end[] asm("_binary_settings_page_html_gz_end");
extern const uint8_t manual_page_html_gz_start[] asm("_binary_manual_page_html_gz_start");
extern const uint8_t manual_page_html_gz_end[] asm("_binary_manual_page_html_gz_end");
extern const uint8_t settings_display_page_html_gz_start[] asm("_binary_settings_display_page_html_gz_start");
extern const uint8_t settings_display_page_html_gz_end[] asm("_binary_settings_display_page_html_gz_end");

/* Same content-negotiation shape as diagnostics_http.c's send_gz_page():
 * web_client_accepts_gzip() covers the "no Accept-Encoding header" (legal,
 * served gzip per RFC 9110 s12.5.3) and "header present but excludes gzip"
 * (406, since this server keeps no uncompressed copy) cases; see
 * web_encoding.h for the full rationale. One tiny helper instead of two
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

static esp_err_t settings_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "settings_page.html", settings_page_html_gz_start, settings_page_html_gz_end);
}

static esp_err_t manual_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "manual_page.html", manual_page_html_gz_start, manual_page_html_gz_end);
}

/* GET /settings/display -- the theme + °C/°F preferences, split out of
 * /settings 2026-08-22. Owner report: "both display theme and reset menu
 * items take me to the same page" -- nav.js's menu always had two separate
 * entries for these, but both used to point at /settings (one at #display,
 * one at #danger), so either menu choice landed on the same document. This
 * route gives Display its own destination; /settings keeps the Danger zone
 * and its own #danger entry still resolves there. */
static esp_err_t settings_display_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "settings_display_page.html", settings_display_page_html_gz_start,
                        settings_display_page_html_gz_end);
}

esp_err_t settings_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t settings_uri = {
        .uri = "/settings", .method = HTTP_GET, .handler = settings_page_get_handler,
    };
    static const httpd_uri_t manual_uri = {
        .uri = "/settings/manual", .method = HTTP_GET, .handler = manual_page_get_handler,
    };
    static const httpd_uri_t display_uri = {
        .uri = "/settings/display", .method = HTTP_GET, .handler = settings_display_page_get_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &settings_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &manual_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/manual) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &display_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/display) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "settings/manual/display pages up");
    return ESP_OK;
}
