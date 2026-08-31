#include "settings_http.h"

#include <stdint.h>

#include "esp_log.h"

#include "http_form.h"
#include "time_sync.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "settings_http";

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component (see web_encoding.h's header comment for the flash-
 * budget reasoning that makes gzip the only stored representation). */
extern const uint8_t settings_page_html_gz_start[] asm("_binary_settings_page_html_gz_start");
extern const uint8_t settings_page_html_gz_end[] asm("_binary_settings_page_html_gz_end");
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
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t settings_page_get_handler(httpd_req_t *req)
{
    return send_gz_page(req, "settings_page.html", settings_page_html_gz_start, settings_page_html_gz_end);
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

/* POST /api/settings/tz -- 2026-08-30, PROFILES.md "Scheduled start +
 * candling": the write side of time_sync.c's persisted POSIX TZ string.
 * Same bounded-body-then-validate-then-commit shape every other settings
 * POST in this codebase uses (dashboard_http.c's unit_pref_post_handler,
 * zones_http.c's zones_post_handler). "Refuse, never clamp": a body that is
 * missing, oversized, or fails time_sync_tz_is_valid() is refused outright
 * with 400 -- nothing here truncates or substitutes a guess. The real gate
 * is time_sync_set_tz() itself (it re-validates independently); this
 * handler's own length/emptiness check exists only to give a clear 400
 * before ever calling into that module, not as the authoritative check. */
#define TZ_BODY_MAX 96 /* "tz=<=63 printable bytes>" plus headroom over the ~67-byte worst case */

static esp_err_t settings_tz_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > TZ_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[TZ_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char tz_val[TIME_SYNC_TZ_MAX_LEN + 1];
    int tz_len = http_form_find_field(body, "tz", tz_val, sizeof(tz_val));
    /* http_form_find_field() returns -2 for a value that did not fit
     * tz_val's capacity -- refuse outright, same "reject, don't truncate"
     * discipline as every other field parser in this codebase (see
     * zones_http.c's parse_zone_fields() comment on the same -2 case). A
     * present-but-empty "tz=" (tz_len == 0) and a missing "tz" field
     * (tz_len == -1) are both refused too: time_sync_tz_is_valid() would
     * reject an empty string anyway, but failing fast here with a clearer
     * message is worth the one extra branch. */
    if (tz_len <= 0 || !time_sync_tz_is_valid(tz_val)) {
        /* time_sync_tz_is_valid() requires actual POSIX TZ grammar, not
         * just printable ASCII -- an IANA name like "America/Chicago" is
         * refused here, not silently accepted and then ignored by tzset()
         * (Finding 2). */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "tz must be a POSIX TZ rule (e.g. EST5EDT,M3.2.0,M11.1.0), not an IANA zone name");
        return ESP_OK;
    }

    esp_err_t err = time_sync_set_tz(tz_val);
    if (err != ESP_OK) {
        /* time_sync_set_tz() only returns non-OK for the same validation
         * failure already checked above (defense in depth) or a persist
         * failure it has already applied live and logged -- either way the
         * live value took effect, so this is reported as an error to the
         * client rather than silently swallowed, but is not a 500: the
         * board is in a consistent (if not-yet-persisted) state. */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             "tz applied live but could not be saved");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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
    static const httpd_uri_t display_uri = {
        .uri = "/settings/display", .method = HTTP_GET, .handler = settings_display_page_get_handler,
    };
    static const httpd_uri_t tz_uri = {
        .uri = "/api/settings/tz", .method = HTTP_POST, .handler = settings_tz_post_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &settings_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &display_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/display) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &tz_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/settings/tz) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "settings/display/timezone pages up");
    return ESP_OK;
}
