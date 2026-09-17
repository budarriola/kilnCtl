// http_session_iface.c -- real implementation of the
// http_auth_session_resolve() seam (http_session_iface.h), now that plan
// section 4's session mechanism has landed
// (net/web_auth_session.h/.c, commit bc481c5a). Replaces
// http_session_iface_stub.c, which always returned HTTP_AUTH_ROLE_NONE;
// deleted by this same change per that stub's own header comment ("delete
// this file, and only this file").
//
// Session table ownership: section 6 (the login/password page, and the
// endpoint that calls web_auth_table_create_session() on a successful
// login) has not landed yet, so nothing else in this tree currently needs
// to reach this table. This file is therefore the table's owner for now --
// a single static instance, exposed via http_session_table() below so that
// section 6's login handler adopts THIS instance rather than creating a
// second, disconnected one (which would be exactly the reset-one-side-of-
// a-pair shape CLAUDE.md warns about: sessions created into one table,
// looked up in another, silently never matching). Whoever lands section 6
// should call http_session_table() and web_auth_table_create_session()
// against its result, not add a new static web_auth_table_t anywhere else.
#include "http_session_iface.h"

#include <string.h>

#include "psa/crypto.h"

#include "hal_time.h" // hal_time_now_ms()
#include "web_auth_session.h"
#include "web_auth_store.h" // web_auth_policy_t, web_auth_store_load_policy()

static web_auth_table_t s_web_auth_table;
static bool s_web_auth_table_init_done = false;

web_auth_table_t *http_session_table(void) {
    if (!s_web_auth_table_init_done) {
        web_auth_table_init(&s_web_auth_table);
        s_web_auth_table_init_done = true;
    }
    return &s_web_auth_table;
}

// Plain SHA-256 of the raw bearer token -- NOT HMAC (there is no shared
// secret involved here; the token itself is the secret, minted randomly at
// login by whichever module creates the session). Same PSA hash primitive
// family as web_auth_store.c's hmac_sha256()/ota_http.c's hmac_sha256(),
// duplicated locally for the same reason those two are duplicated rather
// than shared: this file must not pull in either module's larger
// dependency set just for one hash call.
static void sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    size_t hash_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, msg, msg_len, out, 32, &hash_len);
    if (status != PSA_SUCCESS) {
        // Fail closed: an all-zero hash matches no real session (a real
        // token_hash is the output of a successful hash of 128+ bits of
        // random data, astronomically unlikely to collide with all-zero),
        // so a hash failure here degrades to "no session found", never to
        // treating the token as valid.
        memset(out, 0, 32);
    }
}

// timeout_s: web_auth_policy_t.web_timeout_s is -1 for "never" (item 8);
// web_auth_session.h's WEB_AUTH_TIMEOUT_NEVER_S is 0u for the same concept
// -- two different sentinel encodings for one idea, mapped explicitly here
// rather than passed through, so a future divergence between either
// module's sentinel choice is a compile-visible one-line fix at this one
// boundary, not a silent reinterpretation.
static uint32_t resolve_timeout_s(void) {
    web_auth_policy_t policy;
    web_auth_load_status_t status = web_auth_store_load_policy(&policy);
    if (status != WEB_AUTH_LOAD_OK) {
        // No readable policy record: web_timeout_s has no persisted meaning
        // yet. Falling back to WEB_AUTH_TIMEOUT_NEVER_S here is safe, not a
        // fail-open hole -- http_auth_check() only ever reaches this
        // resolver when http_auth_policy_web_enabled() (the OTHER, separate
        // seam) has already said auth is on, and a missing/unreadable
        // POLICY record's own fail-closed behaviour is that seam's job
        // (see http_auth_policy_iface.c), not this one's to re-derive.
        return WEB_AUTH_TIMEOUT_NEVER_S;
    }
    if (policy.web_timeout_s < 0) {
        return WEB_AUTH_TIMEOUT_NEVER_S;
    }
    return (uint32_t)policy.web_timeout_s;
}

// web_auth_session_role_t (this seam's session module) and http_auth_role_t
// (http_auth_enforce.h, section 5's own role type) are two separate enums
// owned by two different passes -- map explicitly rather than casting, so a
// future renumbering of either one is a compile-visible fix here, not a
// silent reinterpretation. Both currently share the same NONE/USER/ADMIN
// ordering, but that is not guaranteed to stay true.
static http_auth_role_t to_enforce_role(web_auth_session_role_t role) {
    switch (role) {
        case WEB_AUTH_SESSION_ROLE_ADMIN:
            return HTTP_AUTH_ROLE_ADMIN;
        case WEB_AUTH_SESSION_ROLE_USER:
            return HTTP_AUTH_ROLE_USER;
        case WEB_AUTH_SESSION_ROLE_NONE:
        default:
            return HTTP_AUTH_ROLE_NONE;
    }
}

http_auth_role_t http_auth_session_resolve(const char *token, const char *client_ip) {
    (void)client_ip; // the session table's client_ip binding is section 6's
                      // (login-time capture) concern; this resolver only
                      // looks a token up by hash, matching
                      // web_auth_effective_role()'s own signature, which
                      // does not take a client_ip either.
    if (!token || token[0] == '\0') {
        return HTTP_AUTH_ROLE_NONE;
    }

    uint8_t token_hash[32];
    sha256((const uint8_t *)token, strlen(token), token_hash);

    // web_enabled is passed as true here, deliberately not the real policy
    // flag: http_auth_check() (http_auth_enforce.c) already consults the
    // separate http_auth_policy_web_enabled() seam and only calls this
    // resolver at all once that has already said auth is on -- see this
    // seam's own header comment for why passing the real flag again here
    // would be redundant, not more correct.
    web_auth_session_role_t role = web_auth_effective_role(http_session_table(), /*web_enabled=*/true,
                                                             token_hash, resolve_timeout_s(),
                                                             (uint32_t)hal_time_now_ms());
    return to_enforce_role(role);
}
