#include "setup_wizard_http.h"

#include "esp_log.h"

#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "setup_wizard_http";

// Embedded via EMBED_TXTFILES, gzip'd at configure time -- same convention as
// every other *_page.html in this component (see CMakeLists.txt's
// KILNCTL_GZIP_ASSETS comment, and readiness_http.c's identical symbol
// naming for readiness_page.html).
extern const uint8_t setup_wizard_page_html_gz_start[] asm("_binary_setup_wizard_page_html_gz_start");
extern const uint8_t setup_wizard_page_html_gz_end[] asm("_binary_setup_wizard_page_html_gz_end");

/* TODO.md 10.6a's content negotiation, same as readiness_http.c's
 * page_get_handler(): absent Accept-Encoding is legal and served gzip
 * (RFC 9110 s12.5.3); a header that explicitly excludes gzip gets an
 * uncompressed 406 rather than a body it cannot decode. */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "setup_wizard_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)setup_wizard_page_html_gz_start,
                           (size_t)(setup_wizard_page_html_gz_end - setup_wizard_page_html_gz_start));
}

esp_err_t setup_wizard_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/setup", .method = HTTP_GET, .handler = page_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/setup) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "setup wizard page up");
    return ESP_OK;
}
