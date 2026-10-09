// http_auth_enforce.c -- see http_auth_enforce.h for the design. This file
// is pure logic: no ESP-IDF calls, no locks, no allocation, no logging.
#include "http_auth_enforce.h"

#include <string.h>

bool http_auth_lookup_tier(const char *uri, httpd_method_t method, route_tier_t *out_tier) {
    if (uri == NULL) {
        return false;
    }
    size_t n = sizeof(kRouteTierTable) / sizeof(kRouteTierTable[0]);
    for (size_t i = 0; i < n; i++) {
        const route_tier_entry_t *e = &kRouteTierTable[i];
        if (e->method == method && e->uri != NULL && strcmp(e->uri, uri) == 0) {
            if (out_tier != NULL) {
                *out_tier = e->tier;
            }
            return true;
        }
    }
    return false;
}

route_tier_t http_auth_effective_tier(const char *uri, httpd_method_t method) {
    route_tier_t tier = ROUTE_TIER_ADMIN; // fail-closed default if the lookup below misses
    (void)http_auth_lookup_tier(uri, method, &tier);
    return tier;
}

http_auth_decision_t http_auth_check(route_tier_t tier, http_auth_role_t role, bool web_enabled,
                                      bool bootstrap_needed, bool wifi_unprovisioned) {
    // Section 11: auth off collapses every tier to full access, checked
    // first and unconditionally so nothing below this line can veto it.
    if (!web_enabled) {
        return HTTP_AUTH_DECISION_ALLOW;
    }

    // The Dashboard (and everything else classified OPEN) is viewable with
    // no credential even with auth on, regardless of role -- including
    // HTTP_AUTH_ROLE_NONE.
    if (tier == ROUTE_TIER_OPEN) {
        return HTTP_AUTH_DECISION_ALLOW;
    }

    // Plan items 10/11's administrator-bootstrap route: reachable with no
    // session at all, but ONLY while bootstrap_needed -- unconditional on
    // role in both directions (see http_auth_check()'s own doc comment for
    // why this must also close for role == ADMIN once bootstrap is no
    // longer needed). Checked before the no-session denial below, since the
    // true case has no session to deny.
    if (tier == ROUTE_TIER_ADMIN_BOOTSTRAP) {
        return bootstrap_needed ? HTTP_AUTH_DECISION_ALLOW : HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
    }

    // Owner decision 2026-09-28: /wifi, /networks and /scan stay reachable
    // with no session ONLY while the board is unprovisioned -- the AP
    // captive-portal first-time-setup flow must work before any credential
    // exists. Checked here, before the no-session denial below, since the
    // true case has no session to deny. Once provisioned this falls through
    // to the switch below, where it is treated exactly like ROUTE_TIER_ADMIN
    // (an administrator session required, same as the sibling
    // /provision//forget//ip_config routes) -- reassigning `tier` rather than
    // duplicating ROUTE_TIER_ADMIN's case body keeps that single copy the
    // only place "administrator required" is decided.
    if (tier == ROUTE_TIER_WIFI_SETUP) {
        if (wifi_unprovisioned) {
            return HTTP_AUTH_DECISION_ALLOW;
        }
        tier = ROUTE_TIER_ADMIN;
    }

    // Plan section 9: a route that can only ever reduce heat/risk
    // (ROUTE_TIER_SAFETY_REDUCE) must be reachable regardless of role or
    // session state -- including HTTP_AUTH_ROLE_NONE and a locked-out
    // client. Checked here, before the no-session denial below, so nothing
    // past this line can veto it. UNUSED as of the 2026-09-28 owner-decision
    // follow-up: the current-sweep/autotune/danger-mode aborts that used to
    // carry this tier moved to ROUTE_TIER_ADMIN (route_tier_table.h), and
    // POST /api/profile_exec/stop moved to ROUTE_TIER_USER the same day --
    // no route uses ROUTE_TIER_SAFETY_REDUCE today. The tier and this
    // handling are kept, not deleted, for a future route that needs the
    // "always reachable, can only reduce risk" shape again.
    if (tier == ROUTE_TIER_SAFETY_REDUCE) {
        return HTTP_AUTH_DECISION_ALLOW;
    }

    // Everything past this point requires SOME session. No session, an
    // expired one, and an unresolvable one all arrive as HTTP_AUTH_ROLE_NONE
    // (see the session-resolver contract in http_session_iface.h) and are
    // denied identically here.
    if (role == HTTP_AUTH_ROLE_NONE) {
        return HTTP_AUTH_DECISION_DENY_NO_SESSION;
    }

    switch (tier) {
        case ROUTE_TIER_USER:
            // role is USER or ADMIN here (NONE was already handled above,
            // and route_tier_t/http_auth_role_t admit no other values) --
            // both satisfy USER.
            return HTTP_AUTH_DECISION_ALLOW;
        case ROUTE_TIER_ADMIN:
            // bootstrap_needed closes even an admin-role stale session --
            // see http_auth_check()'s own doc comment: a physical
            // credential reset (item 10) clears the credential record but
            // does not invalidate existing sessions, so role == ADMIN alone
            // is not sufficient while bootstrap is outstanding.
            if (bootstrap_needed) {
                return HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
            }
            return (role == HTTP_AUTH_ROLE_ADMIN) ? HTTP_AUTH_DECISION_ALLOW
                                                   : HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
        case ROUTE_TIER_OPEN:
        case ROUTE_TIER_SAFETY_REDUCE:
            // Both unreachable: handled above. Kept as explicit cases
            // (rather than falling into `default`) so this switch names
            // every enum value from route_tier_table.h and a future added
            // tier fails to compile silently-correct instead of falling
            // through this switch's default -- see the `default` case
            // immediately below.
            return HTTP_AUTH_DECISION_ALLOW;
        case ROUTE_TIER_ADMIN_BOOTSTRAP:
            // Unreachable: handled above, before the no-session check. Kept
            // explicit for the same exhaustiveness reason as the two cases
            // above.
            return HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
        case ROUTE_TIER_WIFI_SETUP:
            // Unreachable: handled above, where the true (unprovisioned)
            // case returns ALLOW directly and the false case reassigns
            // `tier` to ROUTE_TIER_ADMIN before this switch is ever reached.
            // Kept explicit for the same exhaustiveness reason as the other
            // cases here.
            return HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
        default:
            // A tier value route_tier_table.h did not define (e.g. a new
            // enumerator added there with no case added here). Fail closed:
            // deny, never allow, on anything this switch does not
            // explicitly recognise.
            return HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
    }
}

bool http_auth_decision_counts_as_activity(route_tier_t tier, http_auth_decision_t decision) {
    if (decision != HTTP_AUTH_DECISION_ALLOW) {
        return false;
    }
    // ROUTE_TIER_WIFI_SETUP counts too: once provisioned it is gated exactly
    // like ROUTE_TIER_ADMIN (http_auth_check()'s own comment), so an ALLOW
    // there is a real administrator session worth extending the same way. An
    // ALLOW while still unprovisioned never reaches here with role != NONE
    // in the shape that would extend anything -- there is no session yet to
    // extend -- so including this tier is safe in both states, not just the
    // provisioned one.
    return tier == ROUTE_TIER_USER || tier == ROUTE_TIER_ADMIN || tier == ROUTE_TIER_WIFI_SETUP;
}

bool http_auth_is_page_shell_get(const char *uri, httpd_method_t method) {
    if (uri == NULL || method != HTTP_GET || strncmp(uri, "/api/", 5) == 0) {
        return false;
    }
    bool listed = false;
    for (size_t i = 0; i < PAGE_SHELL_URI_COUNT; i++) {
        if (kPageShellUris[i] != NULL && strcmp(kPageShellUris[i], uri) == 0) {
            listed = true;
            break;
        }
    }
    // Belt and braces: an allowlisted uri with no GET row at all would be an
    // untabled route (fail-closed ADMIN everywhere else); never treat that
    // as a shell.
    return listed && http_auth_lookup_tier(uri, HTTP_GET, NULL);
}

bool http_auth_refusal_should_close(size_t content_len) {
    return content_len > HTTP_AUTH_REFUSAL_DRAIN_MAX_BYTES;
}
