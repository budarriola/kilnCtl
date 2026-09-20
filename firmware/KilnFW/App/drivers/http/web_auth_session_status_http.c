// web_auth_session_status_http.c -- see web_auth_session_status_http.h for
// scope. Docs/WEB_AUTH_PLAN.md section 8, web-GUI half.
#include "web_auth_session_status_http.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "hal_time.h"            // hal_time_now_ms()
#include "http_auth_http.h"      // kiln_http_register(), http_auth_extract_session_token()
#include "http_auth_policy_iface.h"
#include "http_session_iface.h"  // http_auth_session_status()
#include "web_auth_session.h"    // web_auth_session_in_prompt_window(), WEB_AUTH_*
#include "wifi_provision_http.h" // wifi_provision_http_get_server()

// GET client IP -- same extraction helper other routes forward-declare
// against ota_http.c's real (non-static) definition (see e.g.
// web_auth_login_http.c's identical forward declaration and comment).
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len);

static const char *TAG = "web_auth_session_status_http";

static const char *role_name(web_auth_session_role_t role)
{
    switch (role) {
        case WEB_AUTH_SESSION_ROLE_ADMIN:
            return "admin";
        case WEB_AUTH_SESSION_ROLE_USER:
            return "user";
        case WEB_AUTH_SESSION_ROLE_NONE:
        default:
            return "none";
    }
}

// GET /api/auth/session -- ROUTE_TIER_OPEN, see this module's header comment
// for why this must never itself count as activity. Resolves its own role
// via the shared cookie-extraction helper rather than the pre-handler
// (which never runs a session lookup for OPEN tiers), and reports without
// touching last_seen_ms -- http_auth_session_status()'s own contract.
static esp_err_t session_status_get_handler(httpd_req_t *req)
{
    // Section 11: auth off collapses every tier to full access. Report that
    // here explicitly, rather than falling through to "no session", so the
    // page never renders a lock UI on a board that has never had auth
    // turned on -- same collapse http_auth_check() itself performs.
    if (!http_auth_policy_web_enabled()) {
        // web_auth_admin_bootstrap_needed() is defined as effective_enabled &&
        // !admin_configured -- with effective_enabled false here it can never
        // be true, so this is reported directly rather than computed.
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                   "{\"role\":\"admin\",\"prompt\":false,\"seconds_left\":-1,"
                                   "\"bootstrap_needed\":false}");
    }

    char token[128];
    bool have_token = http_auth_extract_session_token(req, token, sizeof(token));

    // Finding 3 fix (2026-09-17 review): this route is ROUTE_TIER_OPEN, so
    // the shared pre-handler never resolves a role (or an IP) for it --
    // http_auth_session_status() now requires the caller's address too, the
    // same binding http_auth_session_resolve() already enforces for every
    // other route, so a stolen cookie can no longer be confirmed live/admin
    // from an address it was never issued to.
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    web_auth_session_role_t role = WEB_AUTH_SESSION_ROLE_NONE;
    uint32_t last_seen_ms = 0;
    uint32_t timeout_s = 0;
    bool valid = http_auth_session_status(have_token ? token : NULL, ip, &role, &last_seen_ms, &timeout_s);

    bool prompt = false;
    long seconds_left = -1; // -1 means "never" or "no session" -- no countdown to show
    if (valid && timeout_s != WEB_AUTH_TIMEOUT_NEVER_S) {
        uint32_t now = (uint32_t)hal_time_now_ms();
        prompt = web_auth_session_in_prompt_window(last_seen_ms, timeout_s, now);
        uint32_t elapsed_ms = now - last_seen_ms; // both from the same monotonic clock
        uint32_t timeout_ms = timeout_s * 1000u;
        seconds_left = (elapsed_ms >= timeout_ms) ? 0 : (long)((timeout_ms - elapsed_ms) / 1000u);
    }

    // Surfaces the same predicate POST /api/auth/bootstrap_password's own
    // handler gates on (web_auth_admin_bootstrap_needed(), net/web_auth_session.h)
    // so the OPEN login page can tell "first run / locked out, no admin
    // credential yet" apart from "credential set, please log in" without
    // itself needing an ADMIN-tier route -- this route is ROUTE_TIER_OPEN
    // already and adds no new capability: bootstrap_password's real gate is
    // still that same predicate, re-checked independently at its own
    // enforcement point (http_auth_enforce.c) and its own handler (defence
    // in depth comment there). This is read-only reporting, not a second
    // gate.
    // Resolved through http_auth_policy_admin_bootstrap_needed(), the single
    // accessor kiln_http_prehandler() itself uses -- never re-derived here
    // from raw policy/credential state, and never with a hardcoded
    // effective_enabled. http_auth_policy_iface.h says so explicitly: a
    // second copy of that collapse is the reset-one-side-of-a-pair shape
    // CLAUDE.md warns about, and it would silently drift the moment
    // http_auth_policy_web_enabled()'s ABSENT/OK/UNREADABLE handling changes.
    bool bootstrap_needed = http_auth_policy_admin_bootstrap_needed();

    // Deliberately still 128 B, unchanged by the bootstrap_needed field:
    // this repo's rule against enlarging httpd-stack locals is categorical,
    // and no growth is needed here. Worst case is 90 bytes -- the longest
    // role_name() is "admin" (5), prompt/bootstrap_needed are at most
    // "false" (5 each), and seconds_left is a long, at most 20 characters
    // even at LONG_MIN -- so the fixed scaffolding (55 B) plus 5 + 5 + 5 +
    // 20 stays under 91. snprintf's return is checked below, so a future
    // field that does overflow fails loud (500) rather than truncating the
    // JSON silently.
    char body[128];
    int n = snprintf(body, sizeof(body),
                      "{\"role\":\"%s\",\"prompt\":%s,\"seconds_left\":%ld,\"bootstrap_needed\":%s}",
                      role_name(valid ? role : WEB_AUTH_SESSION_ROLE_NONE), prompt ? "true" : "false",
                      seconds_left, bootstrap_needed ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// POST /api/auth/session/extend -- ROUTE_TIER_USER. Reaching this handler
// body at all already means the shared pre-handler's own decision was
// ALLOW for a USER/ADMIN-tier route, which kiln_http_prehandler() has
// already treated as activity and touched the session for -- see
// http_auth_http.c's kiln_http_prehandler(). Nothing else to do here.
static esp_err_t session_extend_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_auth_session_status_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/api/auth/session",
        .method = HTTP_GET,
        .handler = session_status_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/auth/session) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t extend_uri = {
        .uri = "/api/auth/session/extend",
        .method = HTTP_POST,
        .handler = session_extend_post_handler,
    };
    err = kiln_http_register(server, &extend_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/auth/session/extend) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "web session status routes up: GET /api/auth/session, POST /api/auth/session/extend");
    return ESP_OK;
}
