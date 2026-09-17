// Host tests for App/drivers/http/http_session_iface.c's section 8 (web-GUI
// inactivity lock) additions: http_auth_session_status() (the passive status
// poll, must never touch) and http_auth_session_touch() (activity
// extension, must never revive an expired session). Drives the REAL
// production functions against the REAL session table (http_session_table())
// and a REAL persisted policy (web_auth_store_set_policy()/fake_kv.c), with
// fake_time.c's controllable clock standing in for hal_time_now_ms() -- no
// transcribed copy of any of this logic.
#include <string.h>

#include "test_common.h"

#include "../drivers/http/http_session_iface.h"
#include "../drivers/net/web_auth_session.h"
#include "../drivers/persist/web_auth_store.h"
#include "fake_time.h"
#include "hal_kv.h"

static void set_policy_timeout(int32_t web_timeout_s)
{
    web_auth_policy_t policy = { .web_enabled = true, .lcd_enabled = false,
                                  .web_timeout_s = web_timeout_s, .lcd_timeout_s = -1 };
    TEST_CHECK(web_auth_store_set_policy(&policy) == HAL_OK, "setup: policy persisted");
}

// Mints a real session directly in http_session_iface.c's own table, hashed
// with its own exposed http_session_hash_token() -- the same table/hash
// http_auth_session_status()/_touch() read, so this is the real seam, not a
// second table.
static void make_session(const char *token, web_auth_session_role_t role, uint32_t issued_ms)
{
    uint8_t hash[32];
    http_session_hash_token(token, strlen(token), hash);
    web_auth_table_create_session(http_session_table(), hash, "10.0.0.5", role, issued_ms);
}

static void reset_all(void)
{
    fake_time_reset_all();
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init default nvs partition");
    web_auth_table_init(http_session_table());
}

static void test_status_reports_without_touching(void)
{
    TEST_SECTION("http_auth_session_status -- reports role/last_seen without extending it");

    reset_all();
    set_policy_timeout(60); // 60s timeout
    make_session("tok-status-1", WEB_AUTH_SESSION_ROLE_USER, 0);
    fake_time_advance_ms(30000); // 30s in -- well inside the 60s window

    web_auth_session_role_t role = WEB_AUTH_SESSION_ROLE_NONE;
    uint32_t last_seen = 12345;
    uint32_t timeout_s = 0;
    bool ok = http_auth_session_status("tok-status-1", &role, &last_seen, &timeout_s);
    TEST_CHECK(ok, "a live session within its timeout reports valid");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_USER, "role reported correctly");
    TEST_CHECK(last_seen == 0, "last_seen_ms is still the original issue time -- status must not touch");
    TEST_CHECK(timeout_s == 60, "policy's web_timeout_s reported verbatim");

    // *** The keepalive-vs-activity distinction section 8 names explicitly:
    // calling status() repeatedly must never itself extend the session. ***
    fake_time_advance_ms(29999); // now at 59.999s -- 1ms before expiry
    ok = http_auth_session_status("tok-status-1", &role, &last_seen, &timeout_s);
    TEST_CHECK(ok, "still valid at timeout-1ms (status polling along the way changed nothing)");
    TEST_CHECK(last_seen == 0, "last_seen_ms UNCHANGED after a second status() call -- proves no touch");

    fake_time_advance_ms(2); // now at 60.001s -- 1ms past expiry
    ok = http_auth_session_status("tok-status-1", &role, &last_seen, &timeout_s);
    TEST_CHECK(!ok, "invalid at timeout+1ms -- status() reports expiry, does not mask it");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_NONE, "role collapses to NONE once expired");
}

static void test_status_unknown_and_no_token(void)
{
    TEST_SECTION("http_auth_session_status -- unknown token / no token / auth disabled collapse");

    reset_all();
    set_policy_timeout(120);

    web_auth_session_role_t role = WEB_AUTH_SESSION_ROLE_ADMIN; // poison, must be reset to NONE
    uint32_t last_seen = 999;
    uint32_t timeout_s = 0;
    TEST_CHECK(!http_auth_session_status(NULL, &role, &last_seen, &timeout_s), "NULL token -- not valid");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_NONE, "NULL token resets role to NONE, not left poisoned");
    TEST_CHECK(last_seen == 0, "NULL token resets last_seen to 0");
    TEST_CHECK(timeout_s == 120, "timeout_s is still reported even with no token -- caller can render "
                                 "a countdown before any session exists");

    TEST_CHECK(!http_auth_session_status("never-issued", &role, &last_seen, &timeout_s),
               "a token that was never issued is not valid");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_NONE, "unknown token resolves to NONE");
}

static void test_touch_extends_valid_session(void)
{
    TEST_SECTION("http_auth_session_touch -- extends a still-valid session by the full timeout");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-touch-1", WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    fake_time_advance_ms(50000); // 50s in, still valid

    http_auth_session_touch("tok-touch-1");

    web_auth_session_role_t role;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(http_auth_session_status("tok-touch-1", &role, &last_seen, &timeout_s),
               "sanity: still valid right after touch");
    TEST_CHECK(last_seen == 50000, "touch moved last_seen_ms to the current time -- extends by the "
                                   "full timeout from now, not merely delaying expiry a little");

    // Advance another 59s (109s total since issue, but only 59s since the
    // touch) -- must STILL be valid, proving the touch really reset the
    // clock rather than just nudging the original deadline.
    fake_time_advance_ms(59000);
    TEST_CHECK(http_auth_session_status("tok-touch-1", &role, &last_seen, &timeout_s),
               "valid at touch+59s (109s since original issue) -- the touch, not the issue time, "
               "is what the timeout is measured from");
}

static void test_touch_never_revives_expired_session(void)
{
    TEST_SECTION("http_auth_session_touch -- never revives an already-expired session (server-side "
                 "enforcement independent of the browser)");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-touch-2", WEB_AUTH_SESSION_ROLE_USER, 0);
    fake_time_advance_ms(60001); // 1ms past expiry

    // *** This is the acceptance property the task states explicitly: "a
    // client that ignores the prompt must still be refused." A malicious or
    // buggy client hammering an activity route (or this touch call
    // directly) after the deadline must not be able to resurrect the
    // session. ***
    http_auth_session_touch("tok-touch-2");

    web_auth_session_role_t role;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(!http_auth_session_status("tok-touch-2", &role, &last_seen, &timeout_s),
               "touching an already-expired session leaves it expired -- touch is not a revival path");
    TEST_CHECK(last_seen == 0, "last_seen_ms in the underlying slot was never moved by the touch "
                               "(status reports 0 because the session no longer validates)");
}

// Item 3 fix (2026-09-17 adversarial review, 1179e2d3, landed concurrently
// with this file on origin/main): resolve_timeout_s() now fails closed on
// an UNREADABLE policy record instead of substituting WEB_AUTH_TIMEOUT_NEVER_S
// (which web_auth_session_is_valid() reads as "never expires"). Both of this
// file's own functions must inherit that fail-closed behaviour rather than
// re-introducing the same immortal-session hole through a different call
// site -- corrupt the persisted policy blob's CRC via the public hal_kv API
// only (same technique as test_ota_http.c's own UNREADABLE test) and confirm
// a previously-valid session is denied by status() and left untouched by
// touch(), exactly as if it had expired.
static void test_unreadable_policy_fails_closed(void)
{
    TEST_SECTION("http_auth_session_status/_touch -- UNREADABLE policy record fails closed, "
                 "never substitutes 'never expires' for a session that predates the corruption");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-unreadable", WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    fake_time_advance_ms(1000); // still well within the healthy 60s window

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK,
               "setup: open kiln_auth namespace for corruption");
    uint8_t blob[64];
    size_t blob_len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, "auth_policy", blob, &blob_len) == HAL_OK,
               "setup: read back the persisted policy blob");
    TEST_CHECK(blob_len > 0 && blob_len <= sizeof(blob), "setup: policy blob length sane");
    blob[blob_len - 1] ^= 0xFFu; // flip the last byte -- inside the trailing crc32 field
    TEST_CHECK(hal_kv_set_blob(&h, "auth_policy", blob, blob_len) == HAL_OK,
               "setup: write back the corrupted policy blob");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: commit the corruption");
    hal_kv_close(&h);

    web_auth_session_role_t role = WEB_AUTH_SESSION_ROLE_ADMIN; // poison
    uint32_t last_seen = 999;
    uint32_t timeout_s = 999;
    TEST_CHECK(!http_auth_session_status("tok-unreadable", &role, &last_seen, &timeout_s),
               "an UNREADABLE policy denies a still-fresh session outright -- fail closed, "
               "not 'never expires'");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_NONE, "role collapses to NONE under UNREADABLE");

    http_auth_session_touch("tok-unreadable");
    // The touch above must have been a no-op: prove the underlying slot's
    // last_seen_ms was never moved by reading it back after the policy is
    // repaired.
    set_policy_timeout(60);
    TEST_CHECK(http_auth_session_status("tok-unreadable", &role, &last_seen, &timeout_s),
               "sanity: repairing the policy makes the session resolvable again");
    TEST_CHECK(last_seen == 0, "touch() during UNREADABLE never moved last_seen_ms -- "
                               "it is still the original issue time (0), not bumped by the "
                               "no-op touch attempted while the policy was corrupt");
}

static void test_touch_unknown_token_and_never_timeout(void)
{
    TEST_SECTION("http_auth_session_touch -- no-op on unknown/absent token; WEB_AUTH_TIMEOUT_NEVER_S "
                 "sessions always touch successfully");

    reset_all();
    set_policy_timeout(60);

    // No session exists for this token at all -- must not crash, must not
    // create one.
    http_auth_session_touch("never-issued-2");
    http_auth_session_touch(NULL);
    http_auth_session_touch("");

    set_policy_timeout(-1); // "never" (web_auth_store's -1 sentinel)
    make_session("tok-never", WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    fake_time_advance_ms(1000u * 3600u * 24u); // a full day later
    http_auth_session_touch("tok-never");

    web_auth_session_role_t role;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(http_auth_session_status("tok-never", &role, &last_seen, &timeout_s),
               "a 'never' timeout session is always valid, touch or no touch");
    TEST_CHECK(timeout_s == WEB_AUTH_TIMEOUT_NEVER_S, "resolve_timeout_s() maps policy -1 to the "
                                                       "session module's own NEVER sentinel");
}

void run_test_http_session_iface(void) {
    test_status_reports_without_touching();
    test_status_unknown_and_no_token();
    test_touch_extends_valid_session();
    test_touch_never_revives_expired_session();
    test_unreadable_policy_fails_closed();
    test_touch_unknown_token_and_never_timeout();
}
