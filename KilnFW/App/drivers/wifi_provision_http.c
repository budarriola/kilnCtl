#include "wifi_provision_http.h"

#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"

#include "http_form.h"
#include "wifi_prov.h"

static const char *TAG = "wifi_prov_http";

/* application/x-www-form-urlencoded body, worst case ~3x expansion from
 * percent-encoding on both fields plus the "ssid=&password=" framing --
 * generous headroom over that. Content-Length is checked against this
 * *before* a single byte is read, so an attacker claiming a huge body just
 * gets a 400, never a read into memory sized from their own claim. */
#define PROV_BODY_MAX 384

/* Embedded via EMBED_TXTFILES in CMakeLists.txt -- symbol names are the
 * filename with non-alnum characters replaced by '_', plus a NUL the build
 * system appends for a text file. */
extern const uint8_t wifi_provision_page_html_start[] asm("_binary_wifi_provision_page_html_start");
extern const uint8_t wifi_provision_page_html_end[] asm("_binary_wifi_provision_page_html_end");
extern const uint8_t main_page_html_start[] asm("_binary_main_page_html_start");
extern const uint8_t main_page_html_end[] asm("_binary_main_page_html_end");

static httpd_handle_t s_server;

static esp_err_t send_embedded_html(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, wifi_provision_page_html_start, wifi_provision_page_html_end);
}

/* "/" is the landing page a client actually lands on -- both a phone
 * joining the fallback AP fresh (expects setup) and a browser reaching the
 * board over the home network once it's already provisioned (expects the
 * main page, not to be walked through setup again). Same server, same
 * routes underneath either way; this just decides which one "/" means. */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (wifi_prov_is_sta_connected()) {
        return send_embedded_html(req, main_page_html_start, main_page_html_end);
    }
    return send_embedded_html(req, wifi_provision_page_html_start, wifi_provision_page_html_end);
}

static const char *state_name(wifi_prov_state_t s)
{
    switch (s) {
    case WIFI_PROV_STATE_AP_MODE:
        return "ap";
    case WIFI_PROV_STATE_UNPROVISIONED:
        return "unprovisioned";
    case WIFI_PROV_STATE_CONNECTING:
        return "connecting";
    case WIFI_PROV_STATE_CONNECTED:
        return "connected";
    case WIFI_PROV_STATE_RECONNECTING:
        return "reconnecting";
    default:
        return "unknown";
    }
}

static const char *mode_name(wifi_prov_mode_t m)
{
    return m == WIFI_PROV_MODE_AP ? "ap" : "home";
}

/* Escapes '"' and '\\' for embedding an untrusted-ish string (a saved SSID,
 * which came from a POST body at some point) into a JSON string literal.
 * Anything else is passed through -- this is a status readout, not a strict
 * validator, so it favors not truncating a legitimate SSID over rejecting
 * one with unusual characters. out_cap includes the closing NUL. */
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

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
    json_escape(wifi_prov_get_saved_ssid(), ssid_escaped, sizeof(ssid_escaped));

    char ap_ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
    json_escape(wifi_prov_get_ap_ssid(), ap_ssid_escaped, sizeof(ap_ssid_escaped));

    char sta_ip[16];
    bool sta_connected = wifi_prov_is_sta_connected();
    if (!sta_connected || wifi_prov_get_sta_ip(sta_ip, sizeof(sta_ip)) != ESP_OK) {
        sta_ip[0] = '\0';
    }

    /* mode is the one explicit toggle the page renders; state is the
     * finer-grained detail of what's happening while home mode acts on a
     * join (unprovisioned/connecting/connected/reconnecting) -- both are
     * sent so the page can show one coherent switch plus a status line
     * without guessing at either from the other. */
    char json[288];
    int n = snprintf(json, sizeof(json),
                     "{\"mode\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"sta_connected\":%s,"
                     "\"sta_ip\":\"%s\",\"ap_ssid\":\"%s\"}",
                     mode_name(wifi_prov_get_mode()), state_name(wifi_prov_get_state()), ssid_escaped,
                     sta_connected ? "true" : "false", sta_ip, ap_ssid_escaped);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof(json)) {
        n = (int)sizeof(json) - 1; /* truncated is fine for a status readout; never overrun */
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    static wifi_prov_scan_result_t results[20];
    size_t count = 0;
    esp_err_t err = wifi_prov_scan(results, sizeof(results) / sizeof(results[0]), &count);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP mode: scanning is disabled");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_OK;
    }

    /* Bounded by MAX_SCAN entries above, each contributing a fixed-size
     * chunk -- no per-request allocation sized from anything a client sent,
     * this is entirely device-controlled. */
    char json[20 * 64 + 16];
    size_t o = 0;
    json[o++] = '[';
    for (size_t i = 0; i < count; i++) {
        char ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
        json_escape(results[i].ssid, ssid_escaped, sizeof(ssid_escaped));
        int n = snprintf(json + o, sizeof(json) - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                         i == 0 ? "" : ",", ssid_escaped, (int)results[i].rssi,
                         results[i].secure ? "true" : "false");
        if (n < 0 || (size_t)n >= sizeof(json) - o) {
            break; /* ran out of room -- stop here rather than overrun */
        }
        o += (size_t)n;
    }
    if (o + 1 < sizeof(json)) {
        json[o++] = ']';
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

static esp_err_t provision_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PROV_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[PROV_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            /* Client went away or the socket errored mid-body -- log and
             * bail without ever trusting the partial buffer. */
            ESP_LOGW(TAG, "provision body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char mode_val[5]; /* "home" (4) or "ap" (2), plus NUL */
    int mode_len = http_form_find_field(body, "mode", mode_val, sizeof(mode_val));
    if (mode_len >= 0) {
        /* The provisioning page's single toggle: "home" or "ap". Switching
         * to "home" leaves AP mode without requiring new credentials in the
         * same request -- e.g. "go back to trying the network I already
         * have saved" -- distinct from submitting a brand new ssid/password
         * below (which also leaves AP mode, per wifi_prov_set_credentials()). */
        wifi_prov_mode_t want_mode;
        if (mode_len == 4 && strncmp(mode_val, "home", 4) == 0) {
            want_mode = WIFI_PROV_MODE_HOME;
        } else if (mode_len == 2 && strncmp(mode_val, "ap", 2) == 0) {
            want_mode = WIFI_PROV_MODE_AP;
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be 'home' or 'ap'");
            return ESP_OK;
        }
        esp_err_t err = wifi_prov_set_mode(want_mode);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not change mode");
            return ESP_OK;
        }
        return httpd_resp_sendstr(req, "ok");
    }

    /* ap_ssid and/or ap_password change the fallback AP's OWN identity, not
     * station credentials -- see wifi_prov_set_ap_ssid()/
     * wifi_prov_set_ap_password(). Distinct field names from "ssid"/
     * "password" below (the *station* network's credentials) so the two
     * intents can never collide in one request. The AP-mode page submits
     * both fields together in one POST, so both are applied here rather
     * than treating them as mutually exclusive like "mode" above. */
    char ap_ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    int ap_ssid_len = http_form_find_field(body, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    if (ap_ssid_len == -2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ap_ssid too long");
        return ESP_OK;
    }

    char ap_password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
    int ap_password_len = http_form_find_field(body, "ap_password", ap_password, sizeof(ap_password));
    if (ap_password_len == -2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ap_password too long");
        return ESP_OK;
    }

    if (ap_ssid_len >= 0 || ap_password_len >= 0) {
        if (ap_ssid_len >= 0) {
            esp_err_t err = wifi_prov_set_ap_ssid(ap_ssid, (size_t)ap_ssid_len);
            if (err != ESP_OK) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ap_ssid must be 1-32 characters");
                return ESP_OK;
            }
        }
        if (ap_password_len >= 0) {
            esp_err_t err = wifi_prov_set_ap_password(ap_password, (size_t)ap_password_len);
            if (err != ESP_OK) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                     "ap_password must be empty (open) or 8-63 characters");
                return ESP_OK;
            }
        }
        return httpd_resp_sendstr(req, "ok");
    }

    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    char password[WIFI_PROV_PASSWORD_MAX_LEN + 1];
    int ssid_len = http_form_find_field(body, "ssid", ssid, sizeof(ssid));
    if (ssid_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid missing or too long");
        return ESP_OK;
    }
    int password_len = http_form_find_field(body, "password", password, sizeof(password));
    if (password_len == -2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too long");
        return ESP_OK;
    }
    if (password_len < 0) {
        password[0] = '\0';
        password_len = 0;
    }

    esp_err_t err = wifi_prov_set_credentials(ssid, (size_t)ssid_len, password, (size_t)password_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_set_credentials failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not save credentials");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t wifi_provision_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true; /* recycle the oldest connection under
                                     * pressure instead of refusing new ones
                                     * -- keeps a stuck/slow client from
                                     * locking a phone out permanently. */
    /* Default (8) is one short of this file's own 5 routes plus
     * dashboard_http.c's 2 -- bumped with headroom rather than tuned to the
     * exact current count, so the next route added here doesn't silently
     * fail httpd_register_uri_handler. Bumped to 24 when
     * zones_http.c/rules_http.c/profiles_http.c added 11 more routes
     * (TODO.md sections 3/5). **Found the hard way (2026-08-11)**: 24
     * turned out to be exactly one too few once dashboard_http.c's
     * autotune/control/history routes (TODO.md 6A.4/6A.9) landed --
     * registration order is wifi_prov(5) -> dashboard_http(14) ->
     * zones_http(3) -> rules_http(3) -> profiles_http(5) = 30 total, so
     * profiles_http.c's routes (registered last) all failed silently
     * (logged, not fatal) and /profiles + /api/profiles came back 404
     * against the live board. Bumped to 40 -- headroom over the current 30,
     * not tuned to it, same reasoning as every bump before this one, so the
     * next route added anywhere doesn't quietly repeat this exact bug. */
    config.max_uri_handlers = 40;
    /* Default (4096) is tight for the largest POST handlers on this server:
     * zones_post_handler (zones_http.c) alone stacks a 2561-byte body
     * buffer plus a ~170-byte zones_cfg_t scratch copy on top of whatever
     * headroom httpd's own request-parsing call chain has already used
     * before a handler even runs. Bumped to 8192 after that handler was
     * observed to hang/reset under load rather than crash outright (the
     * usual signature of a stack overflow corrupting FreeRTOS/lwIP state
     * quietly instead of tripping an assert) -- see docs/PROJECT_STATUS.md
     * for the date/context. */
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = index_get_handler,
    };
    static const httpd_uri_t wifi_page_uri = {
        .uri = "/wifi", .method = HTTP_GET, .handler = wifi_page_get_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t scan_uri = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler,
    };
    static const httpd_uri_t provision_uri = {
        .uri = "/provision", .method = HTTP_POST, .handler = provision_post_handler,
    };
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &wifi_page_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &scan_uri);
    httpd_register_uri_handler(s_server, &provision_uri);

    ESP_LOGI(TAG, "provisioning HTTP server up");
    return ESP_OK;
}

httpd_handle_t wifi_provision_http_get_server(void)
{
    return s_server;
}
