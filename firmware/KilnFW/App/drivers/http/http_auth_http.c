// http_auth_http.c -- see http_auth_http.h for the design. This is the only
// file in this module that touches esp_http_server.h/ESP_LOGE; everything it
// decides comes from the pure functions in http_auth_enforce.c.
#include "http_auth_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- see s_routes below */
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

// Fixed-size wrapper table -- no heap allocation, matching this feature's
// RAM-cost discipline (plan section 4). 192 leaves headroom over the 155
// rows route_tier_table.h carried as of 2026-09-19 (see CLAUDE.md); if this
// ever fills, registration fails loudly (see kiln_http_register()) rather
// than silently overflowing.
//
// EXT_RAM_BSS_ATTR (PSRAM, not internal .dram0.bss): measured 2026-09-21 at
// 19200 B (192 * 100 B) of internal DRAM. An `nm --size-sort` diff between
// elf_archive/KilnCtrl-1204ef14664b.elf (d459d124, 2026-09-16) and da37ffa2
// showed +28769 B of internal .bss growth over that span, of which this
// table was 19200 B (docs/audits/dram_bss_profiles_fallback_2026-09-20.md,
// "Follow-up 2026-09-21"). Moving it to PSRAM raises total internal DRAM
// free by that ~19 kB deterministically; the effect on the idle
// largest-free-block figure reported by get_heap_status must be read back
// after flashing, not assumed. Every access to this table is from
// kiln_http_register() (HTTP server bringup, after app_main, so PSRAM is
// already mapped) and kiln_http_prehandler() (an ordinary httpd task
// callback on request dispatch) -- never an ISR, never DMA, never a
// flash-cache-disabled section, and no pointer into this table is ever
// handed to a flash write. Same fix shape as s_profiles_fallback in
// profiles_http.c.
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

static EXT_RAM_BSS_ATTR kiln_http_route_ctx_t s_routes[KILN_HTTP_MAX_ROUTES];
static size_t s_route_count = 0;

// Finds the named cookie's value inside a raw "Cookie:" header value (e.g.
// "a=1; kiln_sid=abcdef; b=2") and copies it, NUL-terminated, into `out`.
// Returns true and leaves a non-empty `out` only on a genuine match --
// `out[0]='\0'`/false otherwise (missing header, name not present, or a
// value that would not fit `out_len`). A raw header carrying more than one
// cookie (a real browser sends all cookies for the path in one header,
// separated by "; ") must not be hashed whole -- see resolve_role_for_request()
// below and WEB_AUTH_PLAN.md section 2b's 2026-09-17 review note.
static bool extract_named_cookie(const char *raw, const char *name, char *out, size_t out_len) {
    out[0] = '\0';
    if (!raw || !name || out_len == 0) {
        return false;
    }
    size_t name_len = strlen(name);
    const char *p = raw;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *val = p + name_len + 1;
            const char *end = strchr(val, ';');
            size_t val_len = end ? (size_t)(end - val) : strlen(val);
            if (val_len > 0 && val_len < out_len) {
                memcpy(out, val, val_len);
                out[val_len] = '\0';
                return true;
            }
            // Finding 2 fix (2026-09-17 review): this occurrence's value is
            // empty or too long to fit `out` -- fall through and keep
            // scanning for another occurrence of `name` later in the
            // header, instead of aborting the whole parse. A cookie parser
            // that stops on the first match, valid or not, lets an
            // attacker who can plant ANY cookie on this host (e.g.
            // `kiln_sid=;`, sorted ahead of the real one by path/order)
            // permanently mask a genuine session cookie later in the same
            // header -- see the review's failure scenario. Falling through
            // here still requires advancing past THIS occurrence via the
            // same separator-search below, not re-scanning from `val`.
        }
        const char *sep = strchr(p, ';');
        if (!sep) {
            break;
        }
        p = sep + 1;
    }
    return false;
}

// Extracts a session token for http_auth_session_resolve() from the
// request's Cookie header, parsing out only the named session cookie
// (HTTP_SESSION_COOKIE_NAME, http_session_iface.h) rather than hashing the
// whole raw header. A browser's Cookie header can legally carry more than
// one cookie for a path in any order ("a=1; kiln_sid=X; b=2") -- hashing the
// header verbatim would make the resolved session depend on which OTHER
// cookies happen to be present, which is inert only until something other
// than this login route ever sets a second cookie on the same path
// (WEB_AUTH_PLAN.md section 2b review, 2026-09-17). A missing header, a
// missing name, or an oversized value all resolve to an empty token, which
// http_auth_session_resolve()'s contract already requires mapping to
// HTTP_AUTH_ROLE_NONE -- so a header this function can't safely parse fails
// closed, it does not get silently truncated and possibly still matched.
//
// Finding 4 fix (2026-09-17 review): this comment used to claim a fixed
// "128 + 128 + 46 bytes" stack budget, describing a 128-byte `char cookie[128]`
// stack local that no longer exists -- the Cookie header is now malloc'd here
// (up to KILN_HTTP_MAX_COOKIE_HDR_LEN, 4096 bytes, on the heap, freed before
// return) precisely so a long header cannot blow the shared 8 KB httpd task
// stack. The only stack-resident buffer this function itself owns is `out`,
// sized by the caller (128 bytes at every call site today); the ip[46] buffer
// referenced by the stale comment belongs to a different function
// (resolve_role_for_request() below), not this one.
// Exported so a route that needs the raw session token itself (today: the
// section 8 status-poll/extend routes, web_auth_session_status_http.c) can
// reuse this SAME cookie-extraction path rather than parsing the Cookie
// header a second time -- there must stay exactly one cookie-extraction
// site (http_session_iface.h's own contract). Returns true and leaves `out`
// non-empty only on a genuine kiln_sid cookie match, false (and out[0]='\0')
// otherwise -- same contract as extract_named_cookie() above.
// Finding 3 fix (2026-09-17 review): the old implementation read the whole
// Cookie header into a fixed `char cookie[128]` local and silently skipped
// parsing entirely once the header reached 128 bytes -- an ordinary browser
// carrying `kiln_sid=` (this feature's own 73-byte cookie) plus a couple of
// unrelated cookies from any other service on the same host crosses that
// threshold easily, and the operator was logged out with no diagnostic.
// The fix must not grow that buffer: it lived on the shared 8 KB httpd task
// stack, and an oversized httpd-task local is exactly the crash class
// CLAUDE.md's "httpd stack blob class" note warns about. Instead, heap-
// allocate a buffer sized to the header's own reported length -- no copy of
// the whole header ever needs to sit on the stack. ESP-IDF's own
// CONFIG_HTTPD_MAX_REQ_HDR_LEN already bounds cookie_hdr_len (default
// 512 B) before this ever runs; KILN_HTTP_MAX_COOKIE_HDR_LEN below is a
// second, explicit cap so a future increase to that Kconfig value cannot
// turn this into an unbounded per-request allocation -- a header at or
// above the cap fails closed (no token), same as the old oversized-cookie
// path did, rather than being served from an ever-larger heap buffer.
#define KILN_HTTP_MAX_COOKIE_HDR_LEN 4096u

bool http_auth_extract_session_token(httpd_req_t *req, char *out, size_t out_len) {
    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    size_t cookie_hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_hdr_len == 0 || cookie_hdr_len >= KILN_HTTP_MAX_COOKIE_HDR_LEN) {
        return false;
    }

    char *cookie = malloc(cookie_hdr_len + 1);
    if (!cookie) {
        // Allocation failure fails closed (no token), same as every other
        // unparsable-header case here -- never falls back to a truncated
        // stack copy.
        return false;
    }
    bool matched = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, cookie_hdr_len + 1) == ESP_OK) {
        matched = extract_named_cookie(cookie, HTTP_SESSION_COOKIE_NAME, out, out_len);
    }
    free(cookie);
    return matched;
}

static void resolve_role_for_request(httpd_req_t *req, http_auth_role_t *out_role, char *out_token,
                                      size_t out_token_len) {
    char token[128];
    bool have_token = http_auth_extract_session_token(req, token, sizeof(token));

    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    *out_role = http_auth_session_resolve(have_token ? token : NULL, ip);

    if (out_token != NULL && out_token_len > 0) {
        if (have_token) {
            strncpy(out_token, token, out_token_len - 1);
            out_token[out_token_len - 1] = '\0';
        } else {
            out_token[0] = '\0';
        }
    }
}

// Fix for the OPEN-tier build-identity leak on GET /api/ota/esp/status
// (WEB_AUTH_PLAN.md section 2b): an OPEN-tier handler's shared pre-handler
// (kiln_http_prehandler() below) never resolves
// a role at all for OPEN routes -- see its own comment -- so a handler that
// needs to vary ITS OWN response body by caller role (e.g.
// ota_esp_status_get_handler() trimming exact commit/dirty/build-date
// identity to admins only, while the route itself must stay reachable with
// no session per WEB_AUTH_PLAN.md section 2b) cannot reuse that decision and
// has to resolve one for itself. This is that one shared answer, reusing
// resolve_role_for_request()'s exact cookie-extraction logic above rather
// than a second copy in the caller -- another instance of the
// reset-one-side-of-a-pair class CLAUDE.md warns about if this drifted from
// the pre-handler's own resolution.
//
// Folds in section 11's "a board with auth off is exactly as open as the
// board is today" rule directly: with web auth off there is no session
// concept to fail a caller against, so this returns true unconditionally,
// matching http_auth_check()'s own ALLOW-everything behaviour for that case
// rather than requiring every caller to re-derive it.
bool http_auth_caller_is_admin(httpd_req_t *req) {
    if (!http_auth_policy_web_enabled()) {
        return true;
    }
    http_auth_role_t role = HTTP_AUTH_ROLE_NONE;
    resolve_role_for_request(req, &role, NULL, 0);
    return role == HTTP_AUTH_ROLE_ADMIN;
}

// Owner report, 2026-09-24 (verbatim): "When I open the webpage, don't show
// the login until I do something that would require it ... a pop-up ... that
// allows me to cancel the action." The previous behaviour (see the removed
// 302-to-/login and 302-to-/?admin_required= branches this replaces) forced a
// full-page redirect to the login form -- or to the dashboard with a query
// flag -- the instant a bare page GET landed on a USER/ADMIN-tier route with
// no or an insufficient session, which is exactly the "shows the login before
// I do anything" experience the owner asked to remove.
//
// Every one of these page-shell routes (route_tier_table.h's "Page shells
// other than /" section, plus /settings/*, /safety*, /diagnostics,
// /profiles, /live_profile, /readiness, /setup, /ota) serves ONLY a static,
// compiled-in HTML/JS document -- never per-caller data embedded server-side;
// every real read or write those pages perform happens client-side against
// their own /api/... routes, which keep the EXACT SAME tier gate this
// function already enforces above (this function changes nothing about the
// switch a few lines up, only what happens for these two decisions on a
// page-shell GET). So serving the shell unconditionally on GET reveals
// nothing an unauthenticated caller could not already learn by reading the
// same static bytes any other way, and it does not let an unauthenticated
// caller read or change one byte of protected state -- the API-tier gate is
// untouched. The page's own JS (app.js's global fetch wrapper) is what
// notices the first 401/403 an actual data/action fetch gets back and opens
// the shared, cancelable login modal in place -- see app.js.
//
// Deliberately narrow: only a GET on a uri listed in route_tier_table.h's
// kPageShellUris[] allowlist qualifies (http_auth_is_page_shell_get(),
// host-tested). A POST (mutating) request, any "/api/..." route, and any
// non-/api GET NOT on that list (a future download/export route, or an
// untabled route that fails closed to ADMIN) still falls through to the
// ordinary 401/403 handling below unchanged.
static bool is_page_shell_get(const kiln_http_route_ctx_t *ctx) {
    return http_auth_is_page_shell_get(ctx->uri, ctx->method);
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
    // Fed straight to http_auth_check() -- never re-derived there or
    // anywhere else, per web_auth_admin_bootstrap_needed()'s own header
    // comment (net/web_auth_session.h) naming this the one predicate for
    // this state.
    bool bootstrap_needed = http_auth_policy_admin_bootstrap_needed();

    http_auth_role_t role = HTTP_AUTH_ROLE_NONE;
    char token[128];
    token[0] = '\0';
    // Skip the session lookup entirely for OPEN and SAFETY_REDUCE routes
    // (e.g. POST /api/autotune/abort) and whenever web auth is off --
    // http_auth_check() would ALLOW all of those regardless of role, and
    // this also means the Dashboard's hot GET /api/status path never pays
    // for a session-table lookup it cannot need.
    // Not an optimization this decision structurally depends on:
    // http_auth_check() called with role == HTTP_AUTH_ROLE_NONE
    // unconditionally still ALLOWs every one of those cases on its own, so
    // a future change here that always resolves the role first cannot make
    // this less safe, only slower.
    if (web_enabled && ctx->tier != ROUTE_TIER_OPEN && ctx->tier != ROUTE_TIER_SAFETY_REDUCE) {
        resolve_role_for_request(req, &role, token, sizeof(token));
    }

    http_auth_decision_t decision = http_auth_check(ctx->tier, role, web_enabled, bootstrap_needed);
    // Section 8: any request actually ALLOWED against a real credential tier
    // (USER/ADMIN) is "activity" and extends the session -- the ONE place
    // this touch happens, so a route never has to remember to call it
    // itself. http_auth_session_touch() re-validates before touching, so
    // this can never revive an already-expired session (see its own
    // comment) -- the server, not this call site, is what makes the
    // expiry real.
    if (token[0] != '\0' && http_auth_decision_counts_as_activity(ctx->tier, decision)) {
        // Finding 3 fix (2026-09-17 review): http_auth_session_touch() now
        // enforces the same IP binding http_auth_session_resolve() already
        // does, so it needs the caller's address too -- re-extract it the
        // same way resolve_role_for_request() did above rather than
        // threading it back out of that function's own signature.
        char ip[46];
        ota_http_get_client_ip(req, ip, sizeof(ip));
        http_auth_session_touch(token, ip);
    }
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
            // Owner report, 2026-09-24 (see is_page_shell_get() above):
            // no page load ever shows or redirects to a login form now.
            // A page-shell GET serves the shell directly -- it's the SAME
            // static bytes an unauthenticated caller could already fetch
            // by any other means. This supersedes the 2026-09-21 "just show
            // a login page no matter where they come from" redirect: that
            // was itself the "shows the login before I've done anything"
            // behaviour the 2026-09-24 report asks to remove.
            //
            // Owner decision, 2026-09-28 (web-auth gate tightening): every
            // page but the dashboard now requires login, so app.js's
            // pollSession() (see maybeGateThisPage()) proactively raises the
            // SAME cancelable modal right after this shell loads, rather
            // than waiting for the page's first data/action fetch to fail --
            // still never a separate "authentication required" page, and
            // Cancel still returns to the dashboard.
            if (is_page_shell_get(ctx)) {
                req->user_ctx = ctx->real_user_ctx;
                return ctx->real_handler(req);
            }
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_send(req, "authentication required", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        case HTTP_AUTH_DECISION_DENY_INSUFFICIENT:
            // Same reasoning: a signed-in session with the wrong role
            // hitting an ADMIN-tier page by a bookmark or typed URL now
            // renders that page directly instead of detouring through the
            // dashboard with admin_required=<uri> -- the page's own fetches
            // are what raise the modal, in place, the moment one is tried.
            if (is_page_shell_get(ctx)) {
                req->user_ctx = ctx->real_user_ctx;
                return ctx->real_handler(req);
            }
            // AJAX/API path:
            // ordinary 403, tagged with X-Kiln-Auth-Reason so a client can
            // tell THIS apart from an unrelated 403 (ota_http.c's verify
            // failure, ota_http_recovery.c's "not in recovery mode",
            // diagnostics_http.c's kiln_auth-namespace refusal,
            // profiles_live_http.c's builtin-origin refusal) without
            // guessing from body text -- the smallest honest signal that
            // lets app.js distinguish "no session at all" (401, handled
            // above) from "signed in, wrong role" (this case), per the
            // owner's 2026-09-21 request. Set with httpd_resp_set_hdr()
            // before a manual httpd_resp_send() (not
            // httpd_resp_send_err(), whose own header handling is not
            // guaranteed to preserve one set beforehand) -- same pattern
            // the 401 branch above already uses.
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_hdr(req, "X-Kiln-Auth-Reason", "insufficient_role");
            httpd_resp_send(req, "insufficient role for this route", HTTPD_RESP_USE_STRLEN);
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
