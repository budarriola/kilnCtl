#include "wifi_provision_http.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"

#include "http_form.h"
#include "web_encoding.h"
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
/* UI_PLAN.md Web "page structure rework" section 4 item 1's shared-nav
 * follow-up to theme.css: /nav.js and /app.js, registered here for exactly
 * the same reason theme.css is -- this module owns s_server, so both are
 * reachable during AP-only provisioning as well as once fully provisioned. */
extern const uint8_t nav_js_gz_start[] asm("_binary_nav_js_gz_start");
extern const uint8_t nav_js_gz_end[] asm("_binary_nav_js_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");

static httpd_handle_t s_server;

/* TODO.md 10.6a: content negotiation now lives in web_encoding.h's shared
 * web_client_accepts_gzip() -- absent Accept-Encoding means "anything is
 * acceptable" (RFC 9110 s12.5.3) and is served gzip without a warning; a
 * header that explicitly excludes gzip gets an uncompressed 406 instead of
 * a body it cannot decode. */
static esp_err_t send_embedded_gzip_html(httpd_req_t *req, const char *page_name,
                                          const uint8_t *start, const uint8_t *end)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, page_name);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)start, (size_t)(end - start));
}

static esp_err_t theme_css_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "theme.css");
    }
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)theme_css_gz_start,
                           (size_t)(theme_css_gz_end - theme_css_gz_start));
}

/* text/javascript per RFC 9239 (obsoletes the older application/javascript
 * recommendation) -- both are gzip-only embedded assets like theme.css, so
 * this shares web_client_accepts_gzip()'s 406 path rather than reimplementing
 * the negotiation, same as every other handler in this file. */
static esp_err_t nav_js_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "nav.js");
    }
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)nav_js_gz_start,
                           (size_t)(nav_js_gz_end - nav_js_gz_start));
}

static esp_err_t app_js_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "app.js");
    }
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)app_js_gz_start,
                           (size_t)(app_js_gz_end - app_js_gz_start));
}

static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    /* 2026-08-21, TODO.md section 1 fix: a direct hit on /wifi is one of the
     * ways an operator proves a static IP is actually reachable -- see
     * wifi_prov_note_possible_static_reachability()'s doc comment. Cheap
     * no-op unless ip_mode is STATIC and unconfirmed. */
    wifi_prov_note_possible_static_reachability(httpd_req_to_sockfd(req));
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
    /* 2026-08-21, TODO.md section 1 fix: this is the single most likely hit
     * an operator makes right after typing a newly-configured static IP into
     * a browser, so it's the primary confirmation point -- see
     * wifi_prov_note_possible_static_reachability()'s doc comment. */
    wifi_prov_note_possible_static_reachability(httpd_req_to_sockfd(req));
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

/* 2026-08-20, web-GUI-only static-IP addition (see wifi_prov.h). */
static const char *ip_mode_name(wifi_prov_ip_mode_t m)
{
    return m == WIFI_PROV_IP_MODE_STATIC ? "static" : "dhcp";
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
    /* 2026-08-21, TODO.md section 1 fix: /status is polled periodically by
     * every page's nav/status widget, so this is the confirmation path for a
     * tab that was already open (or a bookmark) before a static-IP join
     * landed -- see wifi_prov_note_possible_static_reachability()'s doc
     * comment. Cheap no-op unless ip_mode is STATIC and unconfirmed. */
    wifi_prov_note_possible_static_reachability(httpd_req_to_sockfd(req));

    char ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
    json_escape(wifi_prov_get_saved_ssid(), ssid_escaped, sizeof(ssid_escaped));

    char ap_ssid_escaped[WIFI_PROV_SSID_MAX_LEN * 2 + 1];
    json_escape(wifi_prov_get_ap_ssid(), ap_ssid_escaped, sizeof(ap_ssid_escaped));

    /* 2026-08-13: the board's OWN AP password, deliberately exposed here --
     * unlike any *saved network's* password (never sent by this server,
     * anywhere -- see wifi_prov_get_saved_networks()'s doc comment), an
     * operator on the setup page needs to see what they configured for the
     * board's own identity. See wifi_prov_get_ap_password()'s doc comment
     * for the full reasoning.
     *
     * 2026-08-22: narrowed to requests that arrived ON the SoftAP. That
     * original reasoning holds exactly there and nowhere else -- a client
     * associated to the AP had to know the password to associate at all, so
     * echoing it back discloses nothing. Served over the STA interface, the
     * same field handed the board's AP password to every unauthenticated
     * device on the house LAN, which is a real disclosure and was never the
     * intent. Off-AP callers get an empty string; ap_password_known tells
     * the page which case it is, so it can render "hidden -- open this page
     * from the board's own Wi-Fi to see it" rather than "no password set",
     * which would be a lie about an AP that is in fact protected. */
    const bool on_ap = wifi_prov_request_arrived_on_ap(httpd_req_to_sockfd(req));
    char ap_password_escaped[WIFI_PROV_PASSWORD_MAX_LEN * 2 + 1];
    json_escape(on_ap ? wifi_prov_get_ap_password() : "", ap_password_escaped,
                sizeof(ap_password_escaped));

    char sta_ip[16];
    bool sta_connected = wifi_prov_is_sta_connected();
    if (!sta_connected || wifi_prov_get_sta_ip(sta_ip, sizeof(sta_ip)) != ESP_OK) {
        sta_ip[0] = '\0';
    }

    int8_t sta_rssi = wifi_prov_get_sta_rssi();
    uint8_t ap_clients = wifi_prov_get_ap_client_count();

    /* 2026-08-20, web-GUI-only: current STA IP mode and, if static, the
     * configured values -- so the page can prefill its static-IP fields on
     * load without a separate round trip. Dotted-quad strings, never
     * user-supplied at read time (they came from a prior POST this same
     * server validated), so no json_escape() needed -- same as sta_ip just
     * above. */
    const char *ip_mode = ip_mode_name(wifi_prov_get_ip_mode());
    const char *static_ip = wifi_prov_get_static_ip();
    const char *static_netmask = wifi_prov_get_static_netmask();
    const char *static_gateway = wifi_prov_get_static_gateway();

    /* mode is the one explicit toggle the page renders; state is the
     * finer-grained detail of what's happening while home mode acts on a
     * join (unprovisioned/connecting/connected/reconnecting) -- both are
     * sent so the page can show one coherent switch plus a status line
     * without guessing at either from the other. */
    /* +64 over the previous size for the two new booleans and their keys. */
    char json[352 + WIFI_PROV_PASSWORD_MAX_LEN * 2 + 24 + 3 * WIFI_PROV_IPV4_STR_MAX + 32 + 64];
    int n = snprintf(json, sizeof(json),
                     "{\"mode\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"sta_connected\":%s,"
                     "\"sta_ip\":\"%s\",\"ap_ssid\":\"%s\",\"ap_password\":\"%s\",\"sta_rssi\":%d,"
                     "\"ap_clients\":%u,\"ip_mode\":\"%s\",\"static_ip\":\"%s\","
                     "\"static_netmask\":\"%s\",\"static_gateway\":\"%s\","
                     "\"ap_password_known\":%s,\"ap_password_set\":%s}",
                     mode_name(wifi_prov_get_mode()), state_name(wifi_prov_get_state()), ssid_escaped,
                     sta_connected ? "true" : "false", sta_ip, ap_ssid_escaped, ap_password_escaped,
                     (int)sta_rssi, (unsigned)ap_clients, ip_mode, static_ip, static_netmask,
                     static_gateway, on_ap ? "true" : "false",
                     wifi_prov_get_ap_password()[0] ? "true" : "false");
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

/* 2026-08-20, web-GUI-only: POST /ip_config -- switches the STA interface
 * between DHCP and a static IP. mode=dhcp needs no other fields; mode=static
 * requires ip/netmask/gateway, each validated as dotted-quad IPv4 by
 * wifi_prov_set_static_ip() itself (ESP_ERR_INVALID_ARG on anything else) --
 * this handler never passes an unvalidated string to esp_netif. Deliberately
 * a separate endpoint from /provision: that one's body-field dispatch
 * (mode=home|ap, ap_ssid/ap_password, ssid/password) is already a 3-way
 * branch on which fields are present, and ip_config's "mode" value space
 * (dhcp/static) is unrelated to and easily confused with /provision's own
 * "mode" field (home/ap) if merged into the same handler. */
#define IP_CONFIG_BODY_MAX 128 /* "mode=static&ip=255.255.255.255&netmask=255.255.255.255&gateway=255.255.255.255" + slack */

static esp_err_t ip_config_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > IP_CONFIG_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[IP_CONFIG_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "ip_config body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char mode_val[8]; /* "dhcp" (4) or "static" (6), plus NUL */
    int mode_len = http_form_find_field(body, "mode", mode_val, sizeof(mode_val));
    if (mode_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode missing or too long");
        return ESP_OK;
    }

    if (mode_len == 4 && strncmp(mode_val, "dhcp", 4) == 0) {
        esp_err_t err = wifi_prov_set_dhcp();
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not switch to DHCP");
            return ESP_OK;
        }
        return httpd_resp_sendstr(req, "ok");
    }
    if (mode_len != 6 || strncmp(mode_val, "static", 6) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be 'dhcp' or 'static'");
        return ESP_OK;
    }

    char ip[WIFI_PROV_IPV4_STR_MAX];
    char netmask[WIFI_PROV_IPV4_STR_MAX];
    char gateway[WIFI_PROV_IPV4_STR_MAX];
    int ip_len = http_form_find_field(body, "ip", ip, sizeof(ip));
    int netmask_len = http_form_find_field(body, "netmask", netmask, sizeof(netmask));
    int gateway_len = http_form_find_field(body, "gateway", gateway, sizeof(gateway));
    if (ip_len < 0 || netmask_len < 0 || gateway_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "static mode requires ip, netmask, and gateway");
        return ESP_OK;
    }

    esp_err_t err = wifi_prov_set_static_ip(ip, netmask, gateway);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_prov_set_static_ip failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "ip/netmask/gateway must each be a valid dotted-quad IPv4 address");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not set static IP");
        }
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

/* Captive-portal redirect, 2026-08-19 (explicit user report of phones
 * joining the fallback AP then giving up -- see wifi_prov.c's
 * dns_hijack_task() header comment for the full mechanism). Every phone
 * OS's connectivity-check probe (Android's /generate_204, Apple's
 * /hotspot-detect.html, Windows' /connecttest.txt, etc.) asks for a
 * specific, never-registered path -- none of them are routes this server
 * knows, so they all land here already, with no per-OS special-casing
 * needed. A plain 302 to "/" is enough: index_get_handler() there already
 * serves wifi_provision_page.html whenever the board isn't STA-connected,
 * which is a different response than every probe expects (a 204 with no
 * body, or specific known text) -- that mismatch is exactly what makes
 * every major OS treat the network as "captive" and pop its own sign-in
 * browser, no captive-portal-specific content negotiation required on this
 * end. */
static esp_err_t captive_portal_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t wifi_provision_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true; /* recycle the oldest connection under
                                     * pressure instead of refusing new ones
                                     * -- keeps a stuck/slow client from
                                     * locking a phone out permanently. */
    /* Socket-pool sizing, 2026-08-20 (live-tested wedge). Confirmed on the
     * bench: a burst of 10 near-simultaneous `curl` GETs to /status hit the
     * DEFAULT (7) open-socket cap -- 3 connection-refused outright, the rest
     * queue behind httpd's single worker task and stall to their client
     * timeout. A second burst right after times out entirely, and the
     * server doesn't recover until ~5 s of idle time lets lru_purge_enable
     * (above) age out the backlog. Real-world trigger: a phone and a PC
     * both polling the dashboard at once. max_open_sockets must satisfy
     * ESP-IDF's own invariant (max_open_sockets <= CONFIG_LWIP_MAX_SOCKETS
     * - 3, checked inside httpd_start()), so this bump is paired with
     * raising CONFIG_LWIP_MAX_SOCKETS 10 -> 16 in sdkconfig -- see that
     * file's comment at the same line. 13 (16-3) gives real headroom over
     * the 10-connection burst that wedged the board without chasing the
     * internal-SRAM-per-socket cost too far (see this project's earlier
     * SRAM-starvation history -- 10-13 sockets is the sane range, not
     * dozens). backlog_conn (kernel-level pending-accept queue, separate
     * from httpd's own open-socket cap) is also bumped from its default of
     * 5 so a burst larger than max_open_sockets still gets queued by the OS
     * instead of refused at the TCP level. */
    config.max_open_sockets = 13;
    config.backlog_conn = 10;
    /* Same burst: sockets sitting idle-but-stuck (e.g. a client that opened
     * a connection but is slow to send/read) held their slot for the full
     * 5 s default recv/send timeout, which is most of what made the second
     * burst hang instead of failing fast. Shortened to 3 s so a stuck
     * socket is recycled faster under load. Safe to do server-wide: the OTA
     * upload handlers (ota_http.c, ota_esp_do_transfer()/pico equivalent)
     * do NOT rely on this default -- they set their own 30 s per-recv
     * setsockopt(SO_RCVTIMEO) directly on the connection's socket fd
     * specifically so a global default bump/cut here can't affect them
     * (see ota_http.c's comment at that setsockopt() call). */
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
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
    /* **Recounted 2026-08-21, by hand, `grep -n '\.uri = "'` over every .c
     * file under App/drivers/ (plus board_temps.c, factory_reset.c,
     * sim_backend.c, the other files that also register routes on this
     * s_server): 59 URI registrations exist in the tree today --
     * 57 in a normal build plus 2 more (`/api/sim` GET+POST, sim_backend.c)
     * only compiled in under CONFIG_KILNCTL_SIM_PLANT. That is already 3
     * over the previous 56, meaning the 2026-08-18 comment's own math was
     * already stale before this pass (it undercounted by not tracking
     * ip_config_uri, and then diagnostics_http.c's 3 new routes
     * (/diagnostics, /diagnostics/thermo, /safety) plus this pass's /nav.js
     * and /app.js pushed it over). Every one of those registrations DOES
     * check httpd_register_uri_handler()'s return value and ESP_LOGEs the
     * failing URI (verified across all of board_temps.c, dashboard_http.c,
     * diagnostics_http.c, factory_reset.c, ota_http.c, profiles_http.c,
     * readiness_http.c, rules_http.c, sim_backend.c, zones_http.c, and this
     * file's own REGISTER_OR_LOG macro below) -- so an overflow is at least
     * logged, never truly silent, but ESP_LOGE only helps if someone is
     * watching the log at that exact boot; a page 404ing with no visible
     * cause otherwise is still the actual user-facing symptom, which is why
     * the cap itself has to stay ahead of the real count rather than relying
     * on the log line to save anyone.
     *
     * Set to 72: rounds the current 59 up with headroom for the next couple
     * of routes, same reasoning as every bump before this one -- not set to
     * an arbitrary large number, because each slot in this table costs a
     * small fixed amount of RAM (esp_http_server allocates the
     * config.max_uri_handlers array up front at httpd_start()), so doubling
     * or hard-coding a huge cap "to be safe" is a real, if small, per-slot
     * cost paid on every boot whether or not those slots are ever used. */
    /* 2026-08-21: raised 72 -> 80. The kiln-config store added 6 routes,
     * taking the real count to 71 of 72 -- one spare. That is too thin to
     * be safe here, because over-cap registration is NOT fatal: the failure
     * shows up only as a 404 on whichever page lost the race, which is
     * exactly the kind of defect that gets diagnosed as "the web UI is
     * broken" rather than "the table was full". Worse, enabling
     * CONFIG_KILNCTL_SIM_PLANT adds 2 more and would have put the build 1
     * over immediately. 80 restores real headroom while keeping the
     * per-slot RAM reasoning below intact. */
    /* 2026-08-21 follow-up: raised 80 -> 84. safety_cfg_http.c (this pass's
     * ESP-side commissioning surface, docs/COMMISSIONING.md sec 3.1) adds 3
     * routes (GET/POST /api/safety/commissioning, POST .../bench_preset),
     * which would have left only ~4-6 spare slots against the prior cap
     * (71 real routes + 2 more under CONFIG_KILNCTL_SIM_PLANT, per the
     * comment above) -- thin enough that the NEXT small addition anywhere in
     * the tree silently 404s a page again, exactly the failure mode the
     * 72->80 bump above was already fixing. 84 restores the same few-routes
     * headroom this cap has been kept at every time before.
     *
     * **Found the hard way a FOURTH time (2026-08-24, bench, commit
     * 750dc33)**: 84 was already one short by the time it shipped. Boot log:
     * `httpd_register_uri_handler(/api/safety/commissioning/bench_preset)
     * failed: ESP_ERR_HTTPD_HANDLERS_FULL`. Recounted by machine this time
     * (tools/check_uri_handler_cap.ps1, see below), not by hand: every
     * `.uri = "..."` literal under firmware/KilnFW/App/drivers (every .c file
     * there), comments
     * stripped. Per-file, in a normal build: wifi_provision_http.c (11) +
     * dashboard_http.c (20) + board_temps.c (2) + zones_http.c (3) +
     * rules_http.c (4) + profiles_http.c (8) + factory_reset.c (1) +
     * readiness_http.c (2) + diagnostics_http.c (9) + settings_http.c (3) +
     * ota_http.c (9) + kiln_cfg_http.c (6) + backup_http.c (3) +
     * safety_cfg_http.c (4) = 85. safety_cfg_http.c actually carries 4
     * routes today, not the 3 the paragraph above counted (it also has the
     * page route, GET /safety/commissioning) -- that single miscount plus
     * backup_http.c's 3 routes (kiln_cfg_http.c's config-store surface,
     * added since the 72->80 bump but never rolled into this comment) is
     * most of how 84 went stale: nobody re-derived the number from source,
     * they trusted the running total. Add sim_backend.c's 2
     * CONFIG_KILNCTL_SIM_PLANT-only routes for the true worst case: 87.
     *
     * RAM cost of a bump here: esp_http_server allocates
     * `hd->hd_calls = calloc(config.max_uri_handlers, sizeof(httpd_uri_t *))`
     * once at httpd_start() (esp-idf components/esp_http_server/src/
     * httpd_main.c) -- an array of POINTERS, not of httpd_uri_t structs (the
     * structs themselves are static const in each driver file already, cap-
     * independent). sizeof(httpd_uri_t *) is 4 bytes on this target's 32-bit
     * Xtensa pointers, so each extra slot costs 4 bytes, not the several-
     * hundred-byte size of the struct it points to. Bumping 84 -> 95 (below)
     * costs 11 * 4 = 44 bytes -- against the 12483-byte dram_free this
     * project has measured at the uart_bridges_1 heap stage (and the
     * documented ~11.9 kB failure floor where HTTP sockets start resetting
     * and /app.js comes back truncated), 44 bytes is noise, not a threat.
     *
     * Set to 95: 87 plus 8 spare slots, the same order of headroom every
     * bump above used (7-13), not double the real count. This time the
     * "keep it ahead of the real count" promise is backed by
     * tools/check_uri_handler_cap.ps1, which recounts `.uri = "..."` under
     * every .c file in drivers/ and fails the moment this cap falls behind again --
     * because after three prose-comment-only bumps still missing the real
     * count, and now a fourth, a comment alone has a 0% success rate on
     * this exact bug. */
    config.max_uri_handlers = 95;
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
    /* 2026-08-20, web-GUI-only DHCP/static IP toggle -- see
     * ip_config_post_handler()'s comment. */
    static const httpd_uri_t ip_config_uri = {
        .uri = "/ip_config", .method = HTTP_POST, .handler = ip_config_post_handler,
    };
    /* TODO.md 10.6's shared-theme follow-up: registered here, not a 7th
     * *_http.c file, because this is the module that owns s_server -- see
     * the extern-symbol block's comment above for why that also makes it
     * reachable during AP-only provisioning, not just once fully
     * provisioned. */
    static const httpd_uri_t theme_css_uri = {
        .uri = "/theme.css", .method = HTTP_GET, .handler = theme_css_get_handler,
    };
    /* UI_PLAN.md Web section 4 item 1 -- shared nav/app JS, same reasoning
     * and same module as theme_css_uri above. This file now registers 11
     * routes (was 9 before this pass); see config.max_uri_handlers above for
     * the 2026-08-21 recount across the whole tree (59 total, cap now 72). */
    static const httpd_uri_t nav_js_uri = {
        .uri = "/nav.js", .method = HTTP_GET, .handler = nav_js_get_handler,
    };
    static const httpd_uri_t app_js_uri = {
        .uri = "/app.js", .method = HTTP_GET, .handler = app_js_get_handler,
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
    REGISTER_OR_LOG(&ip_config_uri);
    REGISTER_OR_LOG(&theme_css_uri);
    REGISTER_OR_LOG(&nav_js_uri);
    REGISTER_OR_LOG(&app_js_uri);

#undef REGISTER_OR_LOG

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_portal_404_handler);

    ESP_LOGI(TAG, "provisioning HTTP server up");
    return ESP_OK;
}

httpd_handle_t wifi_provision_http_get_server(void)
{
    return s_server;
}
