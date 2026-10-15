// auth_reset_gesture_wiring.c -- the one place auth_reset_gesture.h's pure
// state machine is coupled to the real credential store
// (web_auth_store_clear_for_physical_reset(), firmware/KilnFW/App/drivers/
// persist/web_auth_store.h). Split into its own translation unit,
// deliberately separate from auth_reset_gesture.c, so that file stays free
// of any I/O dependency (no psa/crypto.h, no hal_kv.h) and keeps linking
// into every host-test executable that already includes it, including the
// large combined one that does not otherwise pull in psa/crypto.h's host
// stub (see App/test/build_host_tests.ps1's exe43 comment on
// test_web_auth_store.c for why that stub is kept isolated).
//
// Board-wide singleton: exactly one physical panel exists, so exactly one
// gesture state is needed. Not thread-safe by itself -- callers (the LCD
// PIN/keypad module that owns touch input, a separate slice of this plan)
// must only touch the returned pointer from the single UI/LVGL task, same
// assumption every other LVGL-adjacent module in this tree already makes.
#include "auth_reset_gesture.h"

#include "../http/http_session_iface.h" // http_session_table()
#include "../http/totp_http_core.h"    // totp_wipe_request_post()
#include "../persist/web_auth_store.h"
#include "../persist/totp_config.h"
#include "web_auth_session.h" // web_auth_table_destroy_all()

static auth_reset_gesture_state_t s_singleton;
static bool s_singleton_initialized;

// docs/TOTP_PASSWORD_RESET_PLAN.md WT-A part 2, section 6: additive to the
// pre-existing web_auth_store_clear_for_physical_reset() wiring below -- the
// four-corner gesture must also clear any enrolled TOTP secret, or a
// physical reset would leave the previous owner's authenticator app still
// able to satisfy the OPEN-tier /api/auth/forgot route for whatever
// administrator credential the gesture (or a subsequent bootstrap) sets up
// next. Nothing else about the gesture's state machine, corner order or
// timing changes -- only this one function pointer's target.
// totp_config.h has no psa/crypto.h dependency (net/totp.h's hand-rolled
// SHA-1/HMAC, not PSA), so it does not reintroduce the host-stub conflict
// this file's header comment warns about; it uses hal_kv.h the same way
// web_auth_store.c already does in this same translation unit.
// AUDIT_TOTP MED-1: the gesture must evict everyone, same as the web-side
// clear_credentials (security_backend_web_auth.c): besides the two stores it
// ends EVERY web session and posts a wipe request that the httpd worker applies
// (reset tokens + pending enrollment secret, totp_http_core.h) before it next
// reads them -- that RAM is owned by httpd and unlocked, so it is never touched
// from this (LVGL) task. totp_config_clear() takes totp_config's own lock, so
// it cannot interleave with a route's verify/enroll commit. Sessions are ended
// both before the clear (a held admin session cannot start an enrollment
// meanwhile) and after it (a login that verified against the old password just
// before the clear cannot leave a fresh session behind). The wipe request is
// posted FIRST (REVIEW_FIXERS MED-1): an enroll_confirm waiting on the totp
// state lock while totp_config_clear() runs gets the lock right after it, and
// its still_valid guard compares the wipe generation -- so the generation must
// already differ by then or the commit writes a secret after the reset. It is
// posted again last so a pending secret begun in between is also wiped.
static bool auth_reset_gesture_clear_all(void)
{
    totp_wipe_request_post();
    web_auth_table_destroy_all(http_session_table());
    bool web_ok = web_auth_store_clear_for_physical_reset();
    bool totp_ok = totp_config_clear();
    web_auth_table_destroy_all(http_session_table());
    totp_wipe_request_post();
    return web_ok && totp_ok;
}

auth_reset_gesture_state_t *auth_reset_gesture_singleton(void)
{
    if (!s_singleton_initialized) {
        auth_reset_gesture_reset(&s_singleton);
        s_singleton.clear_credentials_fn = auth_reset_gesture_clear_all;
        s_singleton_initialized = true;
    }
    return &s_singleton;
}
