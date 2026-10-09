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

// Exposed wrapper over the same sha256() above -- see http_session_iface.h's
// declaration comment: section 6's login handler must hash the token it
// mints with this SAME function, not a second independent implementation.
void http_session_hash_token(const char *token, size_t token_len, uint8_t out[32]) {
    sha256((const uint8_t *)token, token_len, out);
}

// timeout_s: web_auth_policy_t.web_timeout_s is -1 for "never" (item 8);
// web_auth_session.h's WEB_AUTH_TIMEOUT_NEVER_S is 0u for the same concept
// -- two different sentinel encodings for one idea, mapped explicitly here
// rather than passed through, so a future divergence between either
// module's sentinel choice is a compile-visible one-line fix at this one
// boundary, not a silent reinterpretation.
// Item 3 fix (2026-09-17 adversarial review, 1179e2d3): an UNREADABLE policy
// record must fail closed on BOTH halves it governs -- the enable flag (that
// half was already correct: web_auth_policy_effective_enabled() collapses
// UNREADABLE to true, forcing auth on) AND expiry. Before this fix,
// resolve_timeout_s() returned WEB_AUTH_TIMEOUT_NEVER_S (0) for any non-OK
// status, and web_auth_session_is_valid() treats 0 as "never expires" -- so
// a policy record that went corrupt (e.g. a flash bit-flip, or a
// wrong-version blob) turned every existing session immortal, at the exact
// moment the enable-flag half of the same code was declaring the interface
// MORE locked down, not less. Both halves of one corrupt-record case must
// land on the safe side: this resolver now reports UNREADABLE explicitly so
// the caller can deny outright rather than substitute "never expires".
typedef struct {
    uint32_t timeout_s;
    bool     unreadable;
} resolved_timeout_t;

static resolved_timeout_t resolve_timeout_s(void) {
    web_auth_policy_t policy;
    web_auth_load_status_t status = web_auth_store_load_policy(&policy);
    if (status == WEB_AUTH_LOAD_UNREADABLE) {
        // A record that exists but fails its version/CRC check -- distinct
        // from ABSENT (never written), which is not a corruption and is
        // never reached here in practice anyway (ABSENT collapses
        // web_enabled to false, so http_auth_check() never calls this
        // resolver at all in that case). Fail closed: treat every session
        // as already expired rather than picking a substitute duration.
        resolved_timeout_t r = { .timeout_s = WEB_AUTH_TIMEOUT_NEVER_S, .unreadable = true };
        return r;
    }
    if (status != WEB_AUTH_LOAD_OK) {
        // ABSENT: web_timeout_s has no persisted meaning yet. Falling back
        // to WEB_AUTH_TIMEOUT_NEVER_S here is safe, not a fail-open hole --
        // see the UNREADABLE branch above for why this path is not expected
        // to be reached while auth is actually enabled.
        resolved_timeout_t r = { .timeout_s = WEB_AUTH_TIMEOUT_NEVER_S, .unreadable = false };
        return r;
    }
    resolved_timeout_t r = { .unreadable = false };
    r.timeout_s = (policy.web_timeout_s < 0) ? WEB_AUTH_TIMEOUT_NEVER_S : (uint32_t)policy.web_timeout_s;
    return r;
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
    // Finding 1 fix (2026-09-17 review / prior defect 5): client_ip used to
    // be discarded here (`(void)client_ip;`) even though the slot's binding
    // is captured at login (web_auth_login_http.c) -- the header's stated
    // MUST ("a slot whose IP binding does not match client_ip" resolves to
    // HTTP_AUTH_ROLE_NONE) was unimplemented. web_auth_effective_role() now
    // takes and enforces this binding; this seam's only remaining job is to
    // pass the caller's resolved address through unmodified, same
    // NUL-terminated-or-NULL contract this function's own header already
    // documents.
    if (!token || token[0] == '\0') {
        return HTTP_AUTH_ROLE_NONE;
    }

    uint8_t token_hash[32];
    sha256((const uint8_t *)token, strlen(token), token_hash);

    resolved_timeout_t timeout = resolve_timeout_s();
    if (timeout.unreadable) {
        // Item 3 fix: an unreadable policy record denies outright rather
        // than resolving a (possibly still-live) session against a
        // substitute "never expires" timeout -- see resolve_timeout_s()'s
        // comment. This deliberately never even reaches the table lookup:
        // there is no safe timeout value to hand web_auth_effective_role()
        // here, so this seam does not try to invent one.
        return HTTP_AUTH_ROLE_NONE;
    }

    // web_enabled is passed as true here, deliberately not the real policy
    // flag: http_auth_check() (http_auth_enforce.c) already consults the
    // separate http_auth_policy_web_enabled() seam and only calls this
    // resolver at all once that has already said auth is on -- see this
    // seam's own header comment for why passing the real flag again here
    // would be redundant, not more correct.
    web_auth_session_role_t role = web_auth_effective_role(http_session_table(), /*web_enabled=*/true,
                                                             token_hash, client_ip, timeout.timeout_s,
                                                             (uint32_t)hal_time_now_ms());
    return to_enforce_role(role);
}

bool http_auth_session_status(const char *token, const char *client_ip, web_auth_session_role_t *role_out,
                               uint32_t *last_seen_ms_out, uint32_t *timeout_s_out) {
    if (role_out) {
        *role_out = WEB_AUTH_SESSION_ROLE_NONE;
    }
    if (last_seen_ms_out) {
        *last_seen_ms_out = 0;
    }
    resolved_timeout_t timeout = resolve_timeout_s();
    if (timeout_s_out) {
        *timeout_s_out = timeout.timeout_s;
    }
    // Item 3's fail-closed rule applies here too: an unreadable policy
    // record must not be treated as "never expires" -- report no valid
    // session rather than resolve against a substitute timeout.
    if (timeout.unreadable) {
        return false;
    }
    if (!token || token[0] == '\0') {
        return false;
    }

    uint8_t token_hash[32];
    sha256((const uint8_t *)token, strlen(token), token_hash);

    web_auth_table_t *t = http_session_table();
    int idx = web_auth_table_find_by_token(t, token_hash);
    if (idx < 0) {
        return false;
    }

    uint32_t now = (uint32_t)hal_time_now_ms();
    if (!web_auth_session_is_valid(t->slots[idx].last_seen_ms, timeout.timeout_s, now)) {
        return false;
    }
    // Finding 3 fix (2026-09-17 review): this status poll used to report a
    // matching, still-valid token's role from ANY address -- no IP check at
    // all, even though the slot's binding is captured at login. Apply the
    // same exact-match rule web_auth_effective_role() enforces for every
    // other route: a NULL client_ip (no determinable peer address) or one
    // that does not match the address this session was issued to reports
    // "no session" here too, exactly like an expired or unknown token.
    if (!client_ip || strcmp(t->slots[idx].client_ip, client_ip) != 0) {
        return false;
    }

    if (role_out) {
        *role_out = t->slots[idx].role;
    }
    if (last_seen_ms_out) {
        *last_seen_ms_out = t->slots[idx].last_seen_ms;
    }
    return true;
}

void http_auth_session_touch(const char *token, const char *client_ip, bool via_ap) {
    if (!token || token[0] == '\0') {
        return;
    }

    uint8_t token_hash[32];
    sha256((const uint8_t *)token, strlen(token), token_hash);

    web_auth_table_t *t = http_session_table();
    int idx = web_auth_table_find_by_token(t, token_hash);
    if (idx < 0) {
        return;
    }

    uint32_t now = (uint32_t)hal_time_now_ms();
    resolved_timeout_t timeout = resolve_timeout_s();
    // Item 3's fail-closed rule: an unreadable policy record must not
    // resolve against a substitute "never expires" timeout, so treat it as
    // not touchable rather than guessing.
    if (timeout.unreadable) {
        return;
    }
    // Never revive an already-expired session -- this is the server-side
    // enforcement guarantee: a client ignoring the lock prompt and still
    // sending requests past the deadline must not be able to extend itself
    // back to life just by trying.
    if (!web_auth_session_is_valid(t->slots[idx].last_seen_ms, timeout.timeout_s, now)) {
        return;
    }
    // Finding 3 fix: same exact-match IP binding as http_auth_session_status()
    // and http_auth_session_resolve() above -- a request presented from the
    // wrong address must not extend a session it cannot otherwise use.
    if (!client_ip || strcmp(t->slots[idx].client_ip, client_ip) != 0) {
        return;
    }
    web_auth_table_touch(t, (size_t)idx, now);
    // 2026-09-29: re-tag via_ap on every successful touch, not just at
    // login -- "last used", not "origin only" (see web_auth_session.h's
    // via_ap field comment).
    web_auth_table_set_via_ap(t, (size_t)idx, via_ap);
}

bool http_auth_any_session_active(void) {
    // Fail closed toward TRUE on an unreadable policy record -- see this
    // function's header comment for why that direction is deliberately the
    // opposite of every other resolver here.
    resolved_timeout_t timeout = resolve_timeout_s();
    if (timeout.unreadable) {
        return true;
    }
    web_auth_table_t *t = http_session_table();
    uint32_t now = (uint32_t)hal_time_now_ms();
    for (size_t i = 0; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        if (!t->slots[i].in_use) {
            continue;
        }
        if (web_auth_session_is_valid(t->slots[i].last_seen_ms, timeout.timeout_s, now)) {
            return true;
        }
    }
    return false;
}

bool http_auth_any_ap_session_active(bool ap_station_present) {
    // Same fail-closed-toward-TRUE direction as http_auth_any_session_active()
    // above, for the identical reason -- see this function's header comment.
    resolved_timeout_t timeout = resolve_timeout_s();
    if (timeout.unreadable) {
        return true;
    }
    web_auth_table_t *t = http_session_table();
    uint32_t now = (uint32_t)hal_time_now_ms();
    if (!web_auth_table_any_ap_session_active(t, timeout.timeout_s, now)) {
        return false;
    }
    // Review fix (2026-09-29, round 2): a WEB_AUTH_TIMEOUT_NEVER_S session is
    // always "valid" by definition (see web_auth_session_is_valid()), so on
    // its own it would pin the AP up forever -- one login under a never-
    // expire policy, no way for the table to age it back out. Require an
    // actual AP station also associated in that one case; an ordinary
    // (non-never) session's own timeout already does the aging and this
    // extra check is skipped for it.
    if (timeout.timeout_s == WEB_AUTH_TIMEOUT_NEVER_S) {
        return ap_station_present;
    }
    return true;
}

void http_auth_session_logout(const char *token) {
    if (!token || token[0] == '\0') {
        return;
    }

    uint8_t token_hash[32];
    sha256((const uint8_t *)token, strlen(token), token_hash);

    web_auth_table_t *t = http_session_table();
    int idx = web_auth_table_find_by_token(t, token_hash);
    if (idx < 0) {
        return;
    }
    web_auth_table_destroy_session(t, (size_t)idx);
}
