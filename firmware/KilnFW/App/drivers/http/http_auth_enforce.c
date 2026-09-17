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

http_auth_decision_t http_auth_check(route_tier_t tier, http_auth_role_t role, bool web_enabled) {
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
            return (role == HTTP_AUTH_ROLE_ADMIN) ? HTTP_AUTH_DECISION_ALLOW
                                                   : HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
        case ROUTE_TIER_OPEN:
            // Unreachable: handled above. Kept as an explicit case (rather
            // than falling into `default`) so this switch names every enum
            // value from route_tier_table.h and a future added tier fails
            // to compile silently-correct instead of falling through this
            // switch's default -- see the `default` case immediately below.
            return HTTP_AUTH_DECISION_ALLOW;
        default:
            // A tier value route_tier_table.h did not define (e.g. a new
            // enumerator added there with no case added here). Fail closed:
            // deny, never allow, on anything this switch does not
            // explicitly recognise.
            return HTTP_AUTH_DECISION_DENY_INSUFFICIENT;
    }
}
