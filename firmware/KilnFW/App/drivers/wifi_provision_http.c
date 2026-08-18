#include "wifi_provision_http.h"

#include <stdbool.h>
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
 * system appends for a text file. TODO.md 10.6a: these three are embedded
 * pre-gzipped (CMakeLists.txt gzips them at configure time before
 * idf_component_register runs), so the filename -- and therefore the
 * symbol -- carries a trailing "_gz". theme.css is the new shared-palette
 * route from TODO.md 10.6's follow-up pass; it lives here (not a 7th .c
 * file) because this module is the one that calls httpd_start() and owns
 * the single httpd_handle_t every other *_http.c module registers routes
 * onto (see wifi_provision_http_get_server() below) -- registering it here
 * means it's reachable on the SAME server instance regardless of whether
 * the device is in AP-only provisioning mode or fully provisioned, so a
 * phone on the fallback AP during first-boot setup can load it same as a
 * browser on the home network. */
extern const uint8_t wifi_provision_page_html_gz_start[] asm("_binary_wifi_provision_page_html_gz_start");
extern const uint8_t wifi_provision_page_html_gz_end[] asm("_binary_wifi_provision_page_html_gz_end");
extern const uint8_t main_page_html_gz_start[] asm("_binary_main_page_html_gz_start");
extern const uint8_t main_page_html_gz_end[] asm("_binary_main_page_html_gz_end");
extern const uint8_t theme_css_gz_start[] asm("_binary_theme_css_gz_start");
extern const uint8_t theme_css_gz_end[] asm("_binary_theme_css_gz_end");

static httpd_handle_t s_server;

/* TODO.md 10.6a: defensive check before relying on a client to have asked
 * for gzip -- every real browser sends Accept-Encoding: gzip, but this
 * codebase's convention is to never trust a client to be well-behaved
 * (see e.g. every POST handler's Content-Length bound in this file) rather
 * than assume. There is no uncompressed fallback blob embedded alongside
 * the gzip one (named gap, see TODO.md 10.6a's status note) -- a client
 * that doesn't advertise support still gets the gzip body, just with a
 * warning logged rather than silently mis-served. */
static bool client_accepts_gzip(httpd_req_t *req)
{
    char enc[32];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) != ESP_OK) {
        return false; /* header absent, or longer than this buffer -- treat as "no" either way */
    }
    return strstr(enc, "gzip") != NULL;
}

static esp_err_t send_embedded_gzip_html(httpd_req_t *req, const char *page_name,
                                          const uint8_t *start, const uint8_t *end)
{
    if (!client_accepts_gzip(req)) {
        ESP_LOGW(TAG, "%s: client did not advertise Accept-Encoding: gzip; serving gzip body anyway "
                      "(TODO.md 10.6a: no uncompressed fallback embedded this pass)", page_name);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t theme_css_get_handler(httpd_req_t *req)
{
    if (!client_accepts_gzip(req)) {
        ESP_LOGW(TAG, "theme.css: client did not advertise Accept-Encoding: gzip; serving gzip body "
                      "anyway (TODO.md 10.6a: no uncompressed fallback embedded this pass)");
    }
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)theme_css_gz_start,
                           (size_t)(theme_css_gz_end - theme_css_gz_start));
}

static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    return send_embedded_gzip_html(req, "wifi_provision_page.html",
                                   wifi_provision_page_html_gz_start, wifi_provision_page_html_gz_end);
}

/* "/" is the landing page a client actually lands on -- both a phone
 * joining the fallback AP fresh (expects setup) and a browser reaching the
 * board over the home network once it's already provisioned (expects the
 * main page, not to be walked through setup again). Same server, same
 * routes underneath either way; this just decides which one "/" means. */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (wifi_prov_is_sta_connected()) {
        return send_embedded_gzip_html(req, "main_page.html", main_page_html_gz_start, main_page_html_gz_end);
    }
    return send_embedded_gzip_html(req, "wifi_provision_page.html",
                                   wifi_provision_page_html_gz_start, wifi_provision_page_html_gz_end);
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

    /* 2026-08-13: the board's OWN AP password, deliberately exposed here --
     * unlike any *saved network's* password (never sent by this server,
     * anywhere -- see wifi_prov_get_saved_networks()'s doc comment), an
     * operator on the setup page needs to see what they configured for the
     * board's own identity. See wifi_prov_get_ap_password()'s doc comment
     * for the full reasoning. */
    char ap_password_escaped[WIFI_PROV_PASSWORD_MAX_LEN * 2 + 1];
    json_escape(wifi_prov_get_ap_password(), ap_password_escaped, sizeof(ap_password_escaped));

    char sta_ip[16];
    bool sta_connected = wifi_prov_is_sta_connected();
    if (!sta_connected || wifi_prov_get_sta_ip(sta_ip, sizeof(sta_ip)) != ESP_OK) {
        sta_ip[0] = '\0';
    }

    int8_t sta_rssi = wifi_prov_get_sta_rssi();
    uint8_t ap_clients = wifi_prov_get_ap_client_count();

    /* mode is the one explicit toggle the page renders; state is the
     * finer-grained detail of what's happening while home mode acts on a
     * join (unprovisioned/connecting/connected/reconnecting) -- both are
     * sent so the page can show one coherent switch plus a status line
     * without guessing at either from the other. */
    char json[352 + WIFI_PROV_PASSWORD_MAX_LEN * 2 + 24];
    int n = snprintf(json, sizeof(json),
                     "{\"mode\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"sta_connected\":%s,"
                     "\"sta_ip\":\"%s\",\"ap_ssid\":\"%s\",\"ap_password\":\"%s\",\"sta_rssi\":%d,"
                     "\"ap_clients\":%u}",
                     mode_name(wifi_prov_get_mode()), state_name(wifi_prov_get_state()), ssid_escaped,
                     sta_connected ? "true" : "false", sta_ip, ap_ssid_escaped, ap_password_escaped,
                     (int)sta_rssi, (unsigned)ap_clients);
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

/* WIFI_PROV_MAX_SAVED_NETWORKS is a .c-file-private define in wifi_prov.c,
 * not exposed via wifi_prov.h -- 8 is a literal mirror of that value, kept
 * generous (wifi_prov_get_saved_networks() truncates to whatever bound is
 * passed here, it doesn't care if this is exact). */
#define NETWORKS_SAVED_MAX 8

static esp_err_t networks_get_handler(httpd_req_t *req)
{
    static wifi_prov_saved_network_t saved[NETWORKS_SAVED_MAX];
    size_t saved_count = 0;
    wifi_prov_get_saved_networks(saved, sizeof(saved) / sizeof(saved[0]), &saved_count);

    static wifi_prov_scan_result_t scanned[20];
    size_t scan_count = 0;
    esp_err_t scan_err = wifi_prov_scan(scanned, sizeof(scanned) / sizeof(scanned[0]), &scan_count);
    if (scan_err != ESP_OK) {
        /* AP-mode (ESP_ERR_NOT_SUPPORTED) or any other transient scan
         * failure -- this is a merged status readout, not a scan endpoint,
         * so it degrades to saved-only (all in_range:false) rather than
         * failing the whole response. */
        if (scan_err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "wifi_prov_scan failed in /networks: %s", esp_err_to_name(scan_err));
        }
        scan_count = 0;
    }

    const char *active_ssid = wifi_prov_get_saved_ssid();
    bool sta_connected = wifi_prov_is_sta_connected();

    /* Bounded by NETWORKS_SAVED_MAX + MAX_SCAN entries above, each
     * contributing a fixed-size chunk -- no per-request allocation sized
     * from anything a client sent. Sized generously over
     * (20 scan + 8 saved) * ~80 bytes/entry for the extra fields this
     * response carries versus /scan's plain entries. */
    char json[(20 + NETWORKS_SAVED_MAX) * 96 + 16];
    size_t o = 0;
    json[o++] = '[';
    bool first = true;

    for (size_t i = 0; i < saved_count; i++) {
        const char *ssid = saved[i].ssid;
        bool in_range = false;
        int8_t rssi = 0;
        bool secure = false;
        for (size_t j = 0; j < scan_count; j++) {
            if (strcmp(ssid, scanned[j].ssid) == 0) {
                in_range = true;
                rssi = scanned[j].rssi;
                secure = scanned[j].secure;
                break;
            }
        }
        bool connected = sta_connected && strcmp(ssid, active_ssid) == 0;

        char ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
        json_escape(ssid, ssid_escaped, sizeof(ssid_escaped));

        int n;
        if (in_range) {
            /* rssi/secure key omitted entirely (not emitted as null) when a
             * saved network isn't currently in scan range -- simplest to
             * parse client-side, documented here and in the endpoint doc. */
            n = snprintf(json + o, sizeof(json) - o,
                         "%s{\"ssid\":\"%s\",\"saved\":true,\"in_range\":true,\"rssi\":%d,"
                         "\"secure\":%s,\"connected\":%s}",
                         first ? "" : ",", ssid_escaped, (int)rssi, secure ? "true" : "false",
                         connected ? "true" : "false");
        } else {
            n = snprintf(json + o, sizeof(json) - o,
                         "%s{\"ssid\":\"%s\",\"saved\":true,\"in_range\":false,\"connected\":%s}",
                         first ? "" : ",", ssid_escaped, connected ? "true" : "false");
        }
        if (n < 0 || (size_t)n >= sizeof(json) - o) {
            break; /* ran out of room -- stop here rather than overrun */
        }
        o += (size_t)n;
        first = false;
    }

    for (size_t j = 0; j < scan_count; j++) {
        bool already_saved = false;
        for (size_t i = 0; i < saved_count; i++) {
            if (strcmp(scanned[j].ssid, saved[i].ssid) == 0) {
                already_saved = true;
                break;
            }
        }
        if (already_saved) {
            continue; /* already emitted above with saved:true, in_range:true */
        }

        bool connected = sta_connected && strcmp(scanned[j].ssid, active_ssid) == 0;
        char ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
        json_escape(scanned[j].ssid, ssid_escaped, sizeof(ssid_escaped));

        int n = snprintf(json + o, sizeof(json) - o,
                         "%s{\"ssid\":\"%s\",\"saved\":false,\"in_range\":true,\"rssi\":%d,"
                         "\"secure\":%s,\"connected\":%s}",
                         first ? "" : ",", ssid_escaped, (int)scanned[j].rssi,
                         scanned[j].secure ? "true" : "false", connected ? "true" : "false");
        if (n < 0 || (size_t)n >= sizeof(json) - o) {
            break; /* ran out of room -- stop here rather than overrun */
        }
        o += (size_t)n;
        first = false;
    }

    if (o + 1 < sizeof(json)) {
        json[o++] = ']';
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

static esp_err_t forget_post_handler(httpd_req_t *req)
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
            ESP_LOGW(TAG, "forget body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char ssid[WIFI_PROV_SSID_MAX_LEN + 1];
    int ssid_len = http_form_find_field(body, "ssid", ssid, sizeof(ssid));
    if (ssid_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid missing or too long");
        return ESP_OK;
    }

    esp_err_t err = wifi_prov_forget_network(ssid, (size_t)ssid_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_forget_network failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not forget network");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
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

    esp_err_t err = wifi_prov_add_network(ssid, (size_t)ssid_len, password, (size_t)password_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_add_network failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "saved network list is full");
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not save credentials");
        }
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
     * next route added anywhere doesn't quietly repeat this exact bug.
     *
     * **Found the hard way again (2026-08-18)**: 40 was itself exactly one
     * short. Full current count, in main.c's registration order: wifi_prov
     * (this file, 8: index/wifi_page/status/scan/provision/networks/forget/
     * theme.css) + dashboard_http.c (14) + board_temps.c (1) + zones_http.c
     * (3) + rules_http.c (3) + profiles_http.c (5) + factory_reset.c (1) +
     * readiness_http.c (2) + ota_http.c (4) = 41 -- one over the 40 cap, in
     * a normal (non-sim) build, no Kconfig option required to hit it. Since
     * ota_http.c registers last (main.c, after readiness_http_start()), and
     * /api/ota/pico/status is the last of its 4 routes, THAT is the handler
     * that silently lost the race: httpd_register_uri_handler returned an
     * error ota_http_start() does check and log (ota_http.c logs
     * ESP_LOGE and bails), so this one was not silent, but the next route
     * added anywhere past 41 would be. Bumped to 56 -- generous headroom
     * over 41, not tuned to it, same reasoning as every bump before this
     * one. This did NOT explain a hang on /status or /scan (both register
     * within this file's first 8, far under either cap) -- see the
     * still-missing-error-check note on this file's own
     * httpd_register_uri_handler() calls just below for the other half of
     * this pass's fix. */
    config.max_uri_handlers = 56;
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
    static const httpd_uri_t networks_uri = {
        .uri = "/networks", .method = HTTP_GET, .handler = networks_get_handler,
    };
    static const httpd_uri_t forget_uri = {
        .uri = "/forget", .method = HTTP_POST, .handler = forget_post_handler,
    };
    /* TODO.md 10.6's shared-theme follow-up: registered here, not a 7th
     * *_http.c file, because this is the module that owns s_server -- see
     * the extern-symbol block's comment above for why that also makes it
     * reachable during AP-only provisioning, not just once fully
     * provisioned. */
    static const httpd_uri_t theme_css_uri = {
        .uri = "/theme.css", .method = HTTP_GET, .handler = theme_css_get_handler,
    };
    /* Unlike every other *_http.c module's registration block, these 8 were
     * firing-and-forgetting httpd_register_uri_handler()'s return value --
     * the one gap in the codebase's own convention (see e.g. ota_http.c's
     * ESP_LOGE-and-bail after every one of its calls). That gap is exactly
     * how the max_uri_handlers overflow above stayed invisible until it was
     * chased down by hand: a route that loses the registration race here
     * (this module's own 8, or a later module's, since they all share this
     * one table) still 404s cleanly against a real client (esp_http_server's
     * own not-found handler answers anything it never matched) -- but the
     * failure itself was never logged anywhere, so diagnosing "this one path
     * came back 404, all its siblings work" meant re-deriving this exact
     * headcount by hand instead of reading one ESP_LOGE line. Checked and
     * logged now, matching every other module, so the next overflow says so
     * instead of just going quiet. */
#define REGISTER_OR_LOG(uri_ptr)                                                                \
    do {                                                                                        \
        esp_err_t reg_err = httpd_register_uri_handler(s_server, (uri_ptr));                    \
        if (reg_err != ESP_OK) {                                                                \
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s) failed: %s", (uri_ptr)->uri,          \
                     esp_err_to_name(reg_err));                                                 \
        }                                                                                        \
    } while (0)

    REGISTER_OR_LOG(&index_uri);
    REGISTER_OR_LOG(&wifi_page_uri);
    REGISTER_OR_LOG(&status_uri);
    REGISTER_OR_LOG(&scan_uri);
    REGISTER_OR_LOG(&provision_uri);
    REGISTER_OR_LOG(&networks_uri);
    REGISTER_OR_LOG(&forget_uri);
    REGISTER_OR_LOG(&theme_css_uri);

#undef REGISTER_OR_LOG

    ESP_LOGI(TAG, "provisioning HTTP server up");
    return ESP_OK;
}

httpd_handle_t wifi_provision_http_get_server(void)
{
    return s_server;
}
