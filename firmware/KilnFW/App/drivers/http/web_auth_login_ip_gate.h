#ifndef WEB_AUTH_LOGIN_IP_GATE_H
#define WEB_AUTH_LOGIN_IP_GATE_H

#include <stdbool.h>

// Pure decision core for login_post_handler()'s session-mint gate
// (web_auth_login_http.c). Pulled out into its own header/function, same
// "host-testable without a real socket" pattern ota_http_util.h's
// ota_http_client_ip_finalize() uses, because login_post_handler() itself
// cannot be exercised without a full httpd_req_t and this file has no other
// seam for its decision logic.
//
// Context (2026-09-17/18 adversarial review, d2c51f55 and its follow-up):
// ota_http_get_client_ip()/ota_http_get_client_ip_checked() (ota_http.c)
// write the shared literal "unknown" into the caller's buffer on ANY
// address-lookup failure, and web_auth_session.h documents that string as a
// deliberate, non-unique collision sentinel -- every client whose lookup
// fails presents the identical string. Every OTHER caller of that function
// only ever COMPARES the result against an already-existing session's
// stored client_ip, so the collision is harmless there (worst case: a
// spurious mismatch, i.e. a denial). login_post_handler() is the one
// exception: it MINTS a new session bound to whatever string it got. Ever
// minting one bound to "unknown" would let any other client whose own
// lookup also fails present "unknown" and be accepted as that session --
// not a lockout, a cross-client session-binding hole.
//
// The fix is fail-closed: refuse to mint whenever the address could not be
// determined. `ip_known` must be the boolean ota_http_get_client_ip_checked()
// returned for THIS request -- never a re-derived `strcmp(ip, "unknown")`,
// which would just be a second, fragile copy of the same sentinel contract
// this comment already had to explain once.
static inline bool web_auth_login_may_mint_session(bool ip_known)
{
    return ip_known;
}

#endif // WEB_AUTH_LOGIN_IP_GATE_H
