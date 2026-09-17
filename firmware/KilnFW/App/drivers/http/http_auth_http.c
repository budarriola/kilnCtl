// http_auth_http.c -- see http_auth_http.h for the design. This is the only
// file in this module that touches esp_http_server.h/ESP_LOGE; everything it
// decides comes from the pure functions in http_auth_enforce.c.
#include "http_auth_http.h"

#include <string.h>

#include "esp_log.h"

#include "http_auth_enforce.h"
#include "http_auth_policy_iface.h"
#include "http_session_iface.h"
// ota_http_get_client_ip() -- same client-IP extraction helper ota_http.c's
// own authenticated routes already use (no reason for a second one to
// exist), declared here rather than via ota_http_internal.h: that header is
// the internal seam for the ota_http.c FILE SPLIT's own sibling
// translation units, not a general-purpose public API, and this module is
// neither. The real function has external (non-static) linkage precisely
// so a caller outside that split can reach it -- see ota_http.c's
// definition and this repo's ota_http_internal.h header comment.
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len);

static const char *AUTH_HTTP_TAG = "http_auth";

// Fixed-size, .bss-resident wrapper table -- no heap allocation, matching
// this feature's RAM-cost discipline (plan section 4). 192 leaves headroom
// over the 138 rows route_tier_table.h currently carries; if this ever
// fills, registration fails loudly (see kiln_http_register()) rather than
// silently overflowing.
#define KILN_HTTP_MAX_ROUTES 192

typedef struct {
    bool in_use;
    esp_err_t (*real_handler)(httpd_req_t *r);
    void *real_user_ctx;
    route_tier_t tier;
    // Kept only for ESP_LOGE context if the shared pre-handler ever needs to
    // name the route it is denying; small, fixed-size, not a copy of
    // anything route_tier_table.h owns the classification of.
    char uri[80];
    httpd_method_t method;
} kiln_http_route_ctx_t;

static kiln_http_route_ctx_t s_routes[KILN_HTTP_MAX_ROUTES];
static size_t s_route_count = 0;

// Extracts a session token for http_auth_session_resolve() from the
// request's Cookie header. Deliberately passes the raw header value through
// unparsed -- section 4 (in flight when this was written) owns the actual
// cookie name/format; this pre-handler does not know or need to know it, it
// only needs to hand the resolver *something* to look up. A missing or
// oversized header both resolve to an empty token, which
// http_auth_session_resolve()'s contract already requires mapping to
// HTTP_AUTH_ROLE_NONE -- so a header this function can't safely copy fails
// closed, it does not get silently truncated and possibly still matched.
//
// Buffer sizes here are the whole of this function's stack budget: 128 + 46
// bytes, both fixed, well under the 256-byte-local ceiling this plan (and
// CLAUDE.md's httpd-stack history) sets for anything running on the shared
// 8 KB httpd task stack.
static void resolve_role_for_request(httpd_req_t *req, http_auth_role_t *out_role) {
    char cookie[128];
    cookie[0] = '\0';
    size_t cookie_hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_hdr_len > 0 && cookie_hdr_len < sizeof(cookie)) {
        if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
            cookie[0] = '\0';
        }
    }

    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    *out_role = http_auth_session_resolve(cookie[0] != '\0' ? cookie : NULL, ip);
}

static esp_err_t kiln_http_prehandler(httpd_req_t *req) {
    kiln_http_route_ctx_t *ctx = (kiln_http_route_ctx_t *)req->user_ctx;
    // Defensive: a NULL ctx can only happen if this function were ever
    // registered directly instead of through kiln_http_register(), which
    // always supplies a wrapper context. Fail closed rather than crash or
    // silently allow.
    if (ctx == NULL) {
        ESP_LOGE(AUTH_HTTP_TAG, "pre-handler invoked with no wrapper context -- denying");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "auth wiring error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool web_enabled = http_auth_policy_web_enabled();

    http_auth_role_t role = HTTP_AUTH_ROLE_NONE;
    // Skip the session lookup entirely for OPEN routes and whenever web
    // auth is off -- http_auth_check() would ALLOW either way, and this
    // also means the Dashboard's hot GET /api/status path never pays for a
    // session-table lookup it cannot need. Not an optimization this
    // decision structurally depends on: http_auth_check() called with
    // role == HTTP_AUTH_ROLE_NONE unconditionally still ALLOWs both of
    // those cases on its own, so a future change here that always resolves
    // the role first cannot make this less safe, only slower.
    if (web_enabled && ctx->tier != ROUTE_TIER_OPEN) {
        resolve_role_for_request(req, &role);
    }

    http_auth_decision_t decision = http_auth_check(ctx->tier, role, web_enabled);
    switch (decision) {
        case HTTP_AUTH_DECISION_ALLOW:
            // Restore the ORIGINAL user_ctx before calling into the real
            // handler -- httpd_register_uri_handler() overwrote
            // req->user_ctx with our wrapper context (ctx itself) at
            // dispatch time, and the real handler has no idea this wrapper
            // exists; it still expects whatever it was registered with.
            req->user_ctx = ctx->real_user_ctx;
            return ctx->real_handler(req);
        case HTTP_AUTH_DECISION_DENY_NO_SESSION:
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_send(req, "authentication required", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        case HTTP_AUTH_DECISION_DENY_INSUFFICIENT:
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "insufficient role for this route");
            return ESP_OK;
        default:
            // Unreachable given http_auth_check()'s own three-value enum,
            // but a switch with no default here would leave a future added
            // enumerator free to fall through to nothing (undefined
            // behaviour) instead of a compiler warning AND a safe answer.
            ESP_LOGE(AUTH_HTTP_TAG, "unrecognised auth decision %d for %s -- denying", (int)decision,
                     ctx->uri);
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "auth decision unrecognised");
            return ESP_OK;
    }
}

esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler) {
    if (uri_handler == NULL || uri_handler->uri == NULL || uri_handler->handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_route_count >= KILN_HTTP_MAX_ROUTES) {
        ESP_LOGE(AUTH_HTTP_TAG,
                 "route table full (%d) -- refusing to register %s unwrapped; raise "
                 "KILN_HTTP_MAX_ROUTES",
                 KILN_HTTP_MAX_ROUTES, uri_handler->uri);
        return ESP_ERR_NO_MEM;
    }

    route_tier_t tier;
    bool found = http_auth_lookup_tier(uri_handler->uri, uri_handler->method, &tier);
    if (!found) {
        // Fail closed at the runtime layer (this function's fail-safe): a
        // route with no row in route_tier_table.h is treated as ADMIN, the
        // most restrictive tier, and logged loudly so it is not merely
        // "silently more locked down" but actually noticed and fixed --
        // tools/check_route_tier_coverage.ps1 (section 1) is the build-time
        // half of the same guarantee, so this branch should never actually
        // fire on a tree that passes that check.
        tier = ROUTE_TIER_ADMIN;
        ESP_LOGE(AUTH_HTTP_TAG,
                 "ROUTE WITH NO TIER: %s method=%d has no row in route_tier_table.h -- "
                 "defaulting to ADMIN. Add a ROUTE_TIER() entry (see "
                 "tools/check_route_tier_coverage.ps1).",
                 uri_handler->uri, (int)uri_handler->method);
    }

    kiln_http_route_ctx_t *ctx = &s_routes[s_route_count];
    memset(ctx, 0, sizeof(*ctx));
    ctx->in_use = true;
    ctx->real_handler = uri_handler->handler;
    ctx->real_user_ctx = uri_handler->user_ctx;
    ctx->tier = tier;
    ctx->method = uri_handler->method;
    strncpy(ctx->uri, uri_handler->uri, sizeof(ctx->uri) - 1);
    s_route_count++;

    httpd_uri_t wrapped = *uri_handler;
    wrapped.handler = kiln_http_prehandler;
    wrapped.user_ctx = ctx;
    return httpd_register_uri_handler(server, &wrapped);
}
