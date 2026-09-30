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
    bool ok = http_auth_session_status("tok-status-1", "10.0.0.5", &role, &last_seen, &timeout_s);
    TEST_CHECK(ok, "a live session within its timeout reports valid");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_USER, "role reported correctly");
    TEST_CHECK(last_seen == 0, "last_seen_ms is still the original issue time -- status must not touch");
    TEST_CHECK(timeout_s == 60, "policy's web_timeout_s reported verbatim");

    // *** The keepalive-vs-activity distinction section 8 names explicitly:
    // calling status() repeatedly must never itself extend the session. ***
    fake_time_advance_ms(29999); // now at 59.999s -- 1ms before expiry
    ok = http_auth_session_status("tok-status-1", "10.0.0.5", &role, &last_seen, &timeout_s);
    TEST_CHECK(ok, "still valid at timeout-1ms (status polling along the way changed nothing)");
    TEST_CHECK(last_seen == 0, "last_seen_ms UNCHANGED after a second status() call -- proves no touch");

    fake_time_advance_ms(2); // now at 60.001s -- 1ms past expiry
    ok = http_auth_session_status("tok-status-1", "10.0.0.5", &role, &last_seen, &timeout_s);
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
    TEST_CHECK(!http_auth_session_status(NULL, "10.0.0.5", &role, &last_seen, &timeout_s), "NULL token -- not valid");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_NONE, "NULL token resets role to NONE, not left poisoned");
    TEST_CHECK(last_seen == 0, "NULL token resets last_seen to 0");
    TEST_CHECK(timeout_s == 120, "timeout_s is still reported even with no token -- caller can render "
                                 "a countdown before any session exists");

    TEST_CHECK(!http_auth_session_status("never-issued", "10.0.0.5", &role, &last_seen, &timeout_s),
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

    http_auth_session_touch("tok-touch-1", "10.0.0.5", false);

    web_auth_session_role_t role;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(http_auth_session_status("tok-touch-1", "10.0.0.5", &role, &last_seen, &timeout_s),
               "sanity: still valid right after touch");
    TEST_CHECK(last_seen == 50000, "touch moved last_seen_ms to the current time -- extends by the "
                                   "full timeout from now, not merely delaying expiry a little");

    // Advance another 59s (109s total since issue, but only 59s since the
    // touch) -- must STILL be valid, proving the touch really reset the
    // clock rather than just nudging the original deadline.
    fake_time_advance_ms(59000);
    TEST_CHECK(http_auth_session_status("tok-touch-1", "10.0.0.5", &role, &last_seen, &timeout_s),
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
    http_auth_session_touch("tok-touch-2", "10.0.0.5", false);

    web_auth_session_role_t role;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(!http_auth_session_status("tok-touch-2", "10.0.0.5", &role, &last_seen, &timeout_s),
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
    TEST_CHECK(!http_auth_session_status("tok-unreadable", "10.0.0.5", &role, &last_seen, &timeout_s),
               "an UNREADABLE policy denies a still-fresh session outright -- fail closed, "
               "not 'never expires'");
    TEST_CHECK(role == WEB_AUTH_SESSION_ROLE_NONE, "role collapses to NONE under UNREADABLE");

    http_auth_session_touch("tok-unreadable", "10.0.0.5", false);
    // The touch above must have been a no-op: prove the underlying slot's
    // last_seen_ms was never moved by reading it back after the policy is
    // repaired.
    set_policy_timeout(60);
    TEST_CHECK(http_auth_session_status("tok-unreadable", "10.0.0.5", &role, &last_seen, &timeout_s),
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
    http_auth_session_touch("never-issued-2", "10.0.0.5", false);
    http_auth_session_touch(NULL, "10.0.0.5", false);
    http_auth_session_touch("", "10.0.0.5", false);

    set_policy_timeout(-1); // "never" (web_auth_store's -1 sentinel)
    make_session("tok-never", WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    fake_time_advance_ms(1000u * 3600u * 24u); // a full day later
    http_auth_session_touch("tok-never", "10.0.0.5", false);

    web_auth_session_role_t role;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(http_auth_session_status("tok-never", "10.0.0.5", &role, &last_seen, &timeout_s),
               "a 'never' timeout session is always valid, touch or no touch");
    TEST_CHECK(timeout_s == WEB_AUTH_TIMEOUT_NEVER_S, "resolve_timeout_s() maps policy -1 to the "
                                                       "session module's own NEVER sentinel");
}

// *** 2026-09-17 adversarial review, Finding 1 / prior defect 5: this is the
// REAL seam the finding named -- http_auth_session_resolve()'s
// `(void)client_ip;` used to discard the address entirely, so a valid
// bearer token replayed from any OTHER host on the LAN resolved to its full
// stored role. Drives the actual production function, not just the pure
// web_auth_effective_role() module test in test_web_auth.c. ***
static void test_resolve_denies_mismatched_client_ip(void)
{
    TEST_SECTION("http_auth_session_resolve -- denies a token replayed from a different client_ip "
                 "(Finding 1)");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-ip-bound", WEB_AUTH_SESSION_ROLE_ADMIN, 0); // recorded client_ip: "10.0.0.5"

    TEST_CHECK(http_auth_session_resolve("tok-ip-bound", "10.0.0.5") == HTTP_AUTH_ROLE_ADMIN,
               "the address the session was actually issued to resolves its real role");
    TEST_CHECK(http_auth_session_resolve("tok-ip-bound", "10.0.0.99") == HTTP_AUTH_ROLE_NONE,
               "a valid token replayed from a DIFFERENT address is denied -- the stolen-cookie "
               "replay scenario Finding 1 describes");
    TEST_CHECK(http_auth_session_resolve("tok-ip-bound", NULL) == HTTP_AUTH_ROLE_NONE,
               "an unresolvable peer address (NULL client_ip) also denies rather than being "
               "treated as an exemption from the check");
}

// *** 2026-09-17 adversarial review, Finding 3: http_auth_session_status()
// and http_auth_session_touch() used to take no client_ip at all and applied
// no IP check -- GET /api/auth/session (ROUTE_TIER_OPEN) could confirm a
// stolen cookie live/admin from any address, a session oracle for exactly
// the replay scenario the IP binding exists to deny. Mirrors
// test_resolve_denies_mismatched_client_ip() above but drives the two
// functions Finding 1's fix originally missed. ***
static void test_status_and_touch_deny_mismatched_client_ip(void)
{
    TEST_SECTION("http_auth_session_status/_touch -- deny a token replayed from a different "
                 "client_ip (Finding 3)");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-ip-bound-2", WEB_AUTH_SESSION_ROLE_ADMIN, 0); // recorded client_ip: "10.0.0.5"

    web_auth_session_role_t role = WEB_AUTH_SESSION_ROLE_NONE;
    uint32_t last_seen = 0;
    uint32_t timeout_s = 0;
    TEST_CHECK(http_auth_session_status("tok-ip-bound-2", "10.0.0.5", &role, &last_seen, &timeout_s),
               "the address the session was actually issued to reports the real status");
    TEST_CHECK(!http_auth_session_status("tok-ip-bound-2", "10.0.0.99", &role, &last_seen, &timeout_s),
               "a valid token polled from a DIFFERENT address is denied -- the stolen-cookie "
               "session-oracle scenario Finding 3 describes");
    TEST_CHECK(!http_auth_session_status("tok-ip-bound-2", NULL, &role, &last_seen, &timeout_s),
               "an unresolvable peer address (NULL client_ip) also denies rather than being "
               "treated as an exemption from the check");

    // Advance the clock before the mismatched touches below so before/after
    // can actually differ if a touch IP check were missing -- minted and
    // read at the same t=0 instant, before == after trivially regardless of
    // whether the touch was refused or performed (2026-09-17 review, Finding
    // 1: this made http_auth_session_touch()'s IP check untested even
    // though the assertions below looked like they covered it). Same idiom
    // as test_touch_extends_valid_session() above.
    fake_time_advance_ms(30000);

    uint32_t before = 0;
    TEST_CHECK(http_auth_session_status("tok-ip-bound-2", "10.0.0.5", &role, &before, &timeout_s),
               "sanity read of last_seen_ms before the mismatched touch attempts below");

    http_auth_session_touch("tok-ip-bound-2", "10.0.0.99", false);
    uint32_t after_wrong_ip = 0;
    http_auth_session_status("tok-ip-bound-2", "10.0.0.5", &role, &after_wrong_ip, &timeout_s);
    TEST_CHECK(after_wrong_ip == before,
               "a touch from the wrong address must not extend a session it cannot otherwise use");

    http_auth_session_touch("tok-ip-bound-2", NULL, false);
    uint32_t after_null_ip = 0;
    http_auth_session_status("tok-ip-bound-2", "10.0.0.5", &role, &after_null_ip, &timeout_s);
    TEST_CHECK(after_null_ip == before,
               "a touch with no determinable peer address must not extend the session either");
}

// New for the logout route (docs/agent task, 2026-09-22): http_auth_session_logout()
// must actually destroy the slot (a subsequent resolve() denies it, not just
// leaves it stale), must be idempotent (a second logout, or a logout of a
// token that was never issued, is a silent no-op rather than a crash), and
// must not require the timeout/IP-binding checks touch() enforces -- logging
// out an already-expired or foreign-IP session must still succeed at making
// the cookie useless.
static void test_logout_destroys_the_session(void)
{
    TEST_SECTION("http_auth_session_logout -- destroys the slot so a later resolve() denies it");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-logout-1", WEB_AUTH_SESSION_ROLE_ADMIN, 0);

    TEST_CHECK(http_auth_session_resolve("tok-logout-1", "10.0.0.5") == HTTP_AUTH_ROLE_ADMIN,
               "sanity: the session resolves before logout");

    http_auth_session_logout("tok-logout-1");

    TEST_CHECK(http_auth_session_resolve("tok-logout-1", "10.0.0.5") == HTTP_AUTH_ROLE_NONE,
               "the session is gone after logout -- the exact token/IP that used to resolve "
               "ADMIN now resolves to no session at all");
}

static void test_logout_is_idempotent_and_tolerates_unknown_tokens(void)
{
    TEST_SECTION("http_auth_session_logout -- idempotent, and a no-op on tokens that never existed");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-logout-2", WEB_AUTH_SESSION_ROLE_USER, 0);

    // Never issued -- must not crash, must not disturb the real session.
    http_auth_session_logout("never-issued-logout");
    http_auth_session_logout(NULL);
    http_auth_session_logout("");
    TEST_CHECK(http_auth_session_resolve("tok-logout-2", "10.0.0.5") == HTTP_AUTH_ROLE_USER,
               "logging out unrelated/absent tokens left the real session untouched");

    // Logging the real one out twice must not crash the second time.
    http_auth_session_logout("tok-logout-2");
    http_auth_session_logout("tok-logout-2");
    TEST_CHECK(http_auth_session_resolve("tok-logout-2", "10.0.0.5") == HTTP_AUTH_ROLE_NONE,
               "the session stays gone after a repeated logout call");
}

static void test_logout_does_not_require_a_still_valid_session(void)
{
    TEST_SECTION("http_auth_session_logout -- succeeds on an already-expired or foreign-IP session, "
                 "unlike touch()");

    reset_all();
    set_policy_timeout(60);
    make_session("tok-logout-3", WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    fake_time_advance_ms(60001); // 1ms past expiry -- resolve() already denies this

    TEST_CHECK(http_auth_session_resolve("tok-logout-3", "10.0.0.5") == HTTP_AUTH_ROLE_NONE,
               "sanity: the session is already expired before logout is even called");

    // An expired session's find-by-token lookup must still locate the slot
    // (only resolve()'s validity/IP gate denies it, not the table lookup
    // itself) -- logout must still tear it down rather than silently no-op
    // because it "looked" already gone. resolve() cannot witness that: it
    // already denied this token BEFORE the logout call, so asserting on it
    // again afterwards would pass whether or not the slot was really
    // destroyed. Inspect the table's own in_use flags instead -- the only
    // observation that can actually fail if logout skipped an expired slot.
    http_auth_session_logout("tok-logout-3");
    size_t live_slots = 0;
    for (size_t i = 0; i < WEB_AUTH_WEB_SLOT_COUNT; i++) {
        if (http_session_table()->slots[i].in_use) {
            live_slots++;
        }
    }
    TEST_CHECK(live_slots == 0,
               "logout freed the already-expired slot itself -- an expired session must be "
               "destroyed, not merely left un-resolvable");

    make_session("tok-logout-4", WEB_AUTH_SESSION_ROLE_ADMIN, 0); // client_ip "10.0.0.5"
    // A logout call carries no client_ip parameter at all -- there is nothing
    // to mismatch. This test exists to document that http_auth_session_logout()'s
    // signature deliberately has no IP parameter (unlike touch()/status()),
    // so it can never be refused on IP-binding grounds the way touch() is
    // (see test_status_and_touch_deny_mismatched_client_ip above).
    http_auth_session_logout("tok-logout-4");
    TEST_CHECK(http_auth_session_resolve("tok-logout-4", "10.0.0.5") == HTTP_AUTH_ROLE_NONE,
               "logout tore the session down with no IP check to satisfy");
}

// 2026-09-28 AP-fallback teardown gate: http_auth_any_session_active() is
// what wifi_prov_link.c's ap_teardown_should_defer() asks (test_wifi_prov.c
// only fakes it). Drives the REAL function against the REAL table/policy:
// empty table, a live session, the same session once expired, a logout,
// and the deliberate fail-toward-TRUE on an UNREADABLE policy record.
static void test_any_session_active(void)
{
    TEST_SECTION("http_auth_any_session_active -- counts only still-valid sessions, "
                 "fails toward TRUE on an UNREADABLE policy");

    reset_all();
    set_policy_timeout(60);
    TEST_CHECK(!http_auth_any_session_active(), "empty table -- nobody logged in");

    make_session("tok-any-1", WEB_AUTH_SESSION_ROLE_USER, 0);
    fake_time_advance_ms(59000);
    TEST_CHECK(http_auth_any_session_active(), "a session inside its timeout counts as logged in");

    fake_time_advance_ms(2000); // 61s -- past the 60s timeout, slot still in_use
    TEST_CHECK(!http_auth_any_session_active(),
               "an expired-but-still-in_use slot does NOT count -- an idle tab must not hold the AP up");

    make_session("tok-any-2", WEB_AUTH_SESSION_ROLE_ADMIN, 61000);
    TEST_CHECK(http_auth_any_session_active(), "a fresh login counts again");
    http_auth_session_logout("tok-any-2");
    TEST_CHECK(!http_auth_any_session_active(), "logout ends it");

    make_session("tok-any-3", WEB_AUTH_SESSION_ROLE_USER, 61000);
    fake_time_advance_ms(120000); // well expired under a readable policy
    TEST_CHECK(!http_auth_any_session_active(), "setup: expired under the readable policy");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "setup: open kiln_auth");
    uint8_t blob[64];
    size_t blob_len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, "auth_policy", blob, &blob_len) == HAL_OK, "setup: read policy blob");
    TEST_CHECK(blob_len > 0 && blob_len <= sizeof(blob), "setup: policy blob length sane");
    blob[blob_len - 1] ^= 0xFFu; // corrupt the trailing crc32
    TEST_CHECK(hal_kv_set_blob(&h, "auth_policy", blob, blob_len) == HAL_OK, "setup: write corrupted blob");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: commit corruption");
    hal_kv_close(&h);
    TEST_CHECK(http_auth_any_session_active(),
               "UNREADABLE policy reports TRUE (keep the AP up) even with every session expired");
    set_policy_timeout(60); // repair for later tests
}

// 2026-09-29 owner decision: http_auth_any_ap_session_active() is the fix for
// the bug ap_teardown_should_defer()'s old http_auth_any_session_active()
// signal caused -- a LAN-only session (via_ap never set) must NOT count,
// only one whose via_ap tag is true, and "last used" wins (a touch can flip
// the tag either direction). Drives the REAL function against the REAL
// table/policy, same idiom as test_any_session_active() above.
static void test_any_ap_session_active(void)
{
    TEST_SECTION("http_auth_any_ap_session_active -- counts only still-valid, AP-tagged sessions; "
                 "a LAN-only session never counts; fails toward TRUE on an UNREADABLE policy");

    reset_all();
    set_policy_timeout(60);
    TEST_CHECK(!http_auth_any_ap_session_active(), "empty table -- nobody logged in");

    // A session created and touched only ever over the LAN (via_ap stays
    // false, its default at creation) must NOT count -- this is the actual
    // regression: the PC's MCP tools hold an admin session this way, never
    // through the AP, and must never defer AP teardown by itself.
    make_session("tok-lan-only", WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    fake_time_advance_ms(1000);
    http_auth_session_touch("tok-lan-only", "10.0.0.5", /*via_ap=*/false);
    TEST_CHECK(!http_auth_any_ap_session_active(),
               "a LAN-only session (via_ap never set) does not defer AP teardown");
    TEST_CHECK(http_auth_any_session_active(),
               "sanity: the same session DOES count toward the old, broader any-session signal -- "
               "proving these two are genuinely different predicates, not a rename");

    // A session touched over the AP (via_ap=true) DOES count.
    http_auth_session_touch("tok-lan-only", "10.0.0.5", /*via_ap=*/true);
    TEST_CHECK(http_auth_any_ap_session_active(), "a session last used over the AP defers AP teardown");

    // "Last used", not "origin only": touching it again over the LAN flips it
    // back off.
    http_auth_session_touch("tok-lan-only", "10.0.0.5", /*via_ap=*/false);
    TEST_CHECK(!http_auth_any_ap_session_active(),
               "touching the same session over the LAN again stops it deferring -- via_ap tracks "
               "LAST use, not merely how the session was created");

    // Expiry still applies exactly like the broader signal: an idle,
    // AP-tagged session must not hold the AP up forever.
    http_auth_session_touch("tok-lan-only", "10.0.0.5", /*via_ap=*/true);
    TEST_CHECK(http_auth_any_ap_session_active(), "setup: AP-tagged and fresh");
    fake_time_advance_ms(61000); // past the 60s timeout
    TEST_CHECK(!http_auth_any_ap_session_active(),
               "an expired-but-still-in_use AP-tagged slot does NOT count");

    // UNREADABLE policy still fails closed toward TRUE, same direction as
    // http_auth_any_session_active().
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_auth", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "setup: open kiln_auth");
    uint8_t blob[64];
    size_t blob_len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, "auth_policy", blob, &blob_len) == HAL_OK, "setup: read policy blob");
    TEST_CHECK(blob_len > 0 && blob_len <= sizeof(blob), "setup: policy blob length sane");
    blob[blob_len - 1] ^= 0xFFu; // corrupt the trailing crc32
    TEST_CHECK(hal_kv_set_blob(&h, "auth_policy", blob, blob_len) == HAL_OK, "setup: write corrupted blob");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: commit corruption");
    hal_kv_close(&h);
    TEST_CHECK(http_auth_any_ap_session_active(),
               "UNREADABLE policy reports TRUE (keep the AP up) even with every session expired");
    set_policy_timeout(60); // repair for later tests
}

void run_test_http_session_iface(void) {
    test_status_reports_without_touching();
    test_status_unknown_and_no_token();
    test_touch_extends_valid_session();
    test_touch_never_revives_expired_session();
    test_unreadable_policy_fails_closed();
    test_touch_unknown_token_and_never_timeout();
    test_resolve_denies_mismatched_client_ip();
    test_status_and_touch_deny_mismatched_client_ip();
    test_logout_destroys_the_session();
    test_logout_is_idempotent_and_tolerates_unknown_tokens();
    test_logout_does_not_require_a_still_valid_session();
    test_any_session_active();
    test_any_ap_session_active();
}
