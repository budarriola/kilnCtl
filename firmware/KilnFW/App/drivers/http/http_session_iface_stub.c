// http_session_iface_stub.c -- the placeholder implementation of the
// http_auth_session_resolve() seam (http_session_iface.h) until plan
// section 4's real session table lands. Delete this file, and only this
// file, once the real module supplies the same symbol -- do not delete the
// header, since that is the seam every caller (kiln_http_register()'s
// pre-handler) is written against.
//
// Deliberately always returns HTTP_AUTH_ROLE_NONE: with no real session
// table yet, "no session" is the only answer that keeps
// http_auth_check() fail-closed. Because ROUTE_TIER_OPEN routes ALLOW
// unconditionally (http_auth_enforce.c) and every other route needs a real
// role, this stub's effect, wired in today, is that every gated route
// behaves as if no one is ever logged in -- exactly right for a feature
// that has not shipped credential storage or a login endpoint yet, and it
// composes correctly with section 11's web_enabled flag: while web_enabled
// is false (the shipped default), http_auth_check() never even reaches
// this resolver, and once something turns web_enabled on ahead of section 4
// actually landing, this stub makes every non-OPEN route unreachable rather
// than silently open -- the safe direction to fail in.
#include "http_session_iface.h"

http_auth_role_t http_auth_session_resolve(const char *token, const char *client_ip) {
    (void)token;
    (void)client_ip;
    return HTTP_AUTH_ROLE_NONE;
}
