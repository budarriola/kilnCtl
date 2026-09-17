// Host tests for App/drivers/net/web_auth_session.c --
// docs/WEB_AUTH_PLAN.md section 4 (session mechanism) and the web half of
// section 8 (inactivity lock + 10 s stay-unlocked prompt). No ESP-IDF
// dependency.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/web_auth_session.h"

static void make_hash(uint8_t out[WEB_AUTH_TOKEN_HASH_LEN], uint8_t seed)
{
    for (size_t i = 0; i < WEB_AUTH_TOKEN_HASH_LEN; i++) {
        out[i] = (uint8_t)(seed + i);
    }
}

static void test_table_init_and_lookup(void)
{
    TEST_SECTION("web_auth_table -- init and lookup basics");

    web_auth_table_t t;
    web_auth_table_init(&t);

    uint8_t h1[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(h1, 1);
    TEST_CHECK(web_auth_table_find_by_token(&t, h1) == -1, "empty table finds nothing");

    size_t idx = web_auth_table_create_session(&t, h1, "10.0.0.5", WEB_AUTH_ROLE_ADMIN, 1000);
    TEST_CHECK(idx < WEB_AUTH_WEB_SLOT_COUNT, "create_session returns a real slot index");
    TEST_CHECK(web_auth_table_find_by_token(&t, h1) == (int)idx, "lookup finds the session just created");
    TEST_CHECK(strcmp(t.slots[idx].client_ip, "10.0.0.5") == 0, "client_ip stored");
    TEST_CHECK(t.slots[idx].role == WEB_AUTH_ROLE_ADMIN, "role stored");

    uint8_t h2[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(h2, 200);
    TEST_CHECK(web_auth_table_find_by_token(&t, h2) == -1, "an unknown token is never found -- distinct hash");
}

// *** This is the "unknown token" acceptance case named in the task: a token
// that was never issued (and is not merely expired) must resolve to NONE via
// web_auth_effective_role(), with auth enabled. ***
static void test_unknown_token_effective_role(void)
{
    TEST_SECTION("web_auth_effective_role -- unknown token");

    web_auth_table_t t;
    web_auth_table_init(&t);

    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 5);
    web_auth_table_create_session(&t, issued, "1.2.3.4", WEB_AUTH_ROLE_USER, 0);

    uint8_t unknown[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(unknown, 99);
    TEST_CHECK(web_auth_effective_role(&t, true, unknown, 300, 100) == WEB_AUTH_ROLE_NONE,
               "a token that was never issued resolves to NONE, not the role of some other slot");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, 300, 100) == WEB_AUTH_ROLE_USER,
               "sanity: the actually-issued token still resolves correctly");
    TEST_CHECK(web_auth_effective_role(&t, true, NULL, 300, 100) == WEB_AUTH_ROLE_NONE,
               "no token presented at all (NULL) is NONE, not a crash");
}

static void test_lru_eviction(void)
{
    TEST_SECTION("web_auth_table -- LRU eviction at capacity");

    web_auth_table_t t;
    web_auth_table_init(&t);

    uint8_t hashes[WEB_AUTH_WEB_SLOT_COUNT + 1][WEB_AUTH_TOKEN_HASH_LEN];
    for (int i = 0; i < (int)WEB_AUTH_WEB_SLOT_COUNT; i++) {
        make_hash(hashes[i], (uint8_t)(10 + i));
        // Ascending last_seen_ms so slot 0 is always the least-recently-seen
        // going into the 9th create.
        web_auth_table_create_session(&t, hashes[i], "0.0.0.0", WEB_AUTH_ROLE_USER, (uint32_t)(1000 + i));
    }
    for (int i = 0; i < (int)WEB_AUTH_WEB_SLOT_COUNT; i++) {
        TEST_CHECK(web_auth_table_find_by_token(&t, hashes[i]) != -1, "all 8 initial sessions are present");
    }

    make_hash(hashes[WEB_AUTH_WEB_SLOT_COUNT], 250);
    web_auth_table_create_session(&t, hashes[WEB_AUTH_WEB_SLOT_COUNT], "0.0.0.0", WEB_AUTH_ROLE_ADMIN, 5000);

    TEST_CHECK(web_auth_table_find_by_token(&t, hashes[0]) == -1,
               "the 9th session evicts the least-recently-seen (slot 0's token)");
    for (int i = 1; i < (int)WEB_AUTH_WEB_SLOT_COUNT; i++) {
        TEST_CHECK(web_auth_table_find_by_token(&t, hashes[i]) != -1,
                   "the other 7 original sessions survive the eviction");
    }
    TEST_CHECK(web_auth_table_find_by_token(&t, hashes[WEB_AUTH_WEB_SLOT_COUNT]) != -1,
               "the 9th (newest) session is present");
}

static void test_touch_updates_lru_order(void)
{
    TEST_SECTION("web_auth_table -- touch changes eviction order");

    web_auth_table_t t;
    web_auth_table_init(&t);

    uint8_t hashes[WEB_AUTH_WEB_SLOT_COUNT][WEB_AUTH_TOKEN_HASH_LEN];
    for (int i = 0; i < (int)WEB_AUTH_WEB_SLOT_COUNT; i++) {
        make_hash(hashes[i], (uint8_t)(30 + i));
        web_auth_table_create_session(&t, hashes[i], "0.0.0.0", WEB_AUTH_ROLE_USER, (uint32_t)(1000 + i));
    }
    // Touch slot 0 (originally the oldest) so it is now the most recent.
    int idx0 = web_auth_table_find_by_token(&t, hashes[0]);
    web_auth_table_touch(&t, (size_t)idx0, 9999);

    uint8_t newcomer[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(newcomer, 250);
    web_auth_table_create_session(&t, newcomer, "0.0.0.0", WEB_AUTH_ROLE_ADMIN, 10000);

    TEST_CHECK(web_auth_table_find_by_token(&t, hashes[0]) != -1,
               "a touched slot is no longer the LRU victim");
    TEST_CHECK(web_auth_table_find_by_token(&t, hashes[1]) == -1,
               "slot 1 (now the actual least-recently-seen) is evicted instead");
}

static void test_destroy_session_and_role(void)
{
    TEST_SECTION("web_auth_table -- explicit teardown (logout, role-wide invalidation)");

    web_auth_table_t t;
    web_auth_table_init(&t);

    uint8_t user_h[WEB_AUTH_TOKEN_HASH_LEN];
    uint8_t admin_h[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(user_h, 40);
    make_hash(admin_h, 60);
    size_t user_idx = web_auth_table_create_session(&t, user_h, "0.0.0.0", WEB_AUTH_ROLE_USER, 1000);
    web_auth_table_create_session(&t, admin_h, "0.0.0.0", WEB_AUTH_ROLE_ADMIN, 1000);

    web_auth_table_destroy_session(&t, user_idx);
    TEST_CHECK(web_auth_table_find_by_token(&t, user_h) == -1, "logout removes exactly that session");
    TEST_CHECK(web_auth_table_find_by_token(&t, admin_h) != -1, "the other session is untouched by a logout");

    // Section 6: "changing a password invalidates every session for that role."
    web_auth_table_init(&t);
    uint8_t admin1[WEB_AUTH_TOKEN_HASH_LEN], admin2[WEB_AUTH_TOKEN_HASH_LEN], user1[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(admin1, 1);
    make_hash(admin2, 2);
    make_hash(user1, 3);
    web_auth_table_create_session(&t, admin1, "0.0.0.0", WEB_AUTH_ROLE_ADMIN, 1000);
    web_auth_table_create_session(&t, admin2, "0.0.0.0", WEB_AUTH_ROLE_ADMIN, 1000);
    web_auth_table_create_session(&t, user1, "0.0.0.0", WEB_AUTH_ROLE_USER, 1000);
    web_auth_table_destroy_role(&t, WEB_AUTH_ROLE_ADMIN);
    TEST_CHECK(web_auth_table_find_by_token(&t, admin1) == -1, "role-wide destroy clears the first admin slot");
    TEST_CHECK(web_auth_table_find_by_token(&t, admin2) == -1, "role-wide destroy clears the second admin slot");
    TEST_CHECK(web_auth_table_find_by_token(&t, user1) != -1, "a user-role slot is untouched by an admin-role destroy");

    web_auth_table_destroy_all(&t);
    TEST_CHECK(web_auth_table_find_by_token(&t, user1) == -1, "destroy_all clears the remaining session too");
}

// *** Expiry acceptance case: valid at timeout-1ms, invalid at timeout+1ms
// (WEB_AUTH_PLAN.md section 8 acceptance), boundary inclusive at exactly
// timeout, and "never" always valid. ***
static void test_expiry_boundaries(void)
{
    TEST_SECTION("web_auth_session_is_valid -- expiry boundaries");

    uint32_t last_seen = 100000;
    uint32_t timeout_s = 60; // 60000 ms
    TEST_CHECK(web_auth_session_is_valid(last_seen, timeout_s, last_seen + 60000u - 1u),
               "valid at timeout - 1 ms");
    TEST_CHECK(web_auth_session_is_valid(last_seen, timeout_s, last_seen + 60000u),
               "valid exactly at the timeout boundary (inclusive)");
    TEST_CHECK(!web_auth_session_is_valid(last_seen, timeout_s, last_seen + 60000u + 1u),
               "invalid at timeout + 1 ms");

    TEST_CHECK(web_auth_session_is_valid(last_seen, WEB_AUTH_TIMEOUT_NEVER_S, last_seen + 100000000u),
               "'never' (timeout_s == 0) is always valid, however long it has been");
}

static void test_prompt_window(void)
{
    TEST_SECTION("web_auth_session_in_prompt_window -- 10 s stay-unlocked window");

    uint32_t last_seen = 0;
    uint32_t timeout_s = 60; // 60000 ms total; prompt window is the last 10000 ms of that

    TEST_CHECK(!web_auth_session_in_prompt_window(last_seen, timeout_s, 49999u),
               "just before timeout - 10s: prompt not yet open");
    TEST_CHECK(web_auth_session_in_prompt_window(last_seen, timeout_s, 50000u),
               "prompt window opens exactly at timeout - 10 s");
    TEST_CHECK(web_auth_session_in_prompt_window(last_seen, timeout_s, 60000u),
               "prompt window still open exactly at the timeout boundary itself");
    TEST_CHECK(!web_auth_session_in_prompt_window(last_seen, timeout_s, 60001u),
               "once actually expired, the prompt window is closed (session is just gone)");

    TEST_CHECK(!web_auth_session_in_prompt_window(last_seen, WEB_AUTH_TIMEOUT_NEVER_S, 999999999u),
               "a session that never expires never enters the prompt window");
}

static void test_touch_extends_and_clears_prompt(void)
{
    TEST_SECTION("web_auth_table_touch -- keepalive-vs-activity semantics belong to the caller;"
                 " touch() itself always extends and clears prompted");

    web_auth_table_t t;
    web_auth_table_init(&t);
    uint8_t h[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(h, 7);
    size_t idx = web_auth_table_create_session(&t, h, "0.0.0.0", WEB_AUTH_ROLE_USER, 0);
    t.slots[idx].prompted = true; // simulate the prompt having been shown

    web_auth_table_touch(&t, idx, 5000);
    TEST_CHECK(t.slots[idx].last_seen_ms == 5000, "touch updates last_seen_ms to the full new value (extends by"
                                                    " the full timeout relative to the new now)");
    TEST_CHECK(!t.slots[idx].prompted, "touch clears a pending prompt flag");

    // Out-of-range / not-in-use indices are harmless no-ops.
    web_auth_table_touch(&t, WEB_AUTH_WEB_SLOT_COUNT + 5, 6000);
    web_auth_table_destroy_session(&t, idx);
    web_auth_table_touch(&t, idx, 7000); // now not in_use
    TEST_CHECK(!t.slots[idx].in_use, "touch on a destroyed slot does not resurrect it");
}

// *** Auth-disabled inert path acceptance case: web_enabled == false must
// short-circuit to ADMIN with no table access, regardless of table state or
// token. ***
static void test_auth_disabled_inert_path(void)
{
    TEST_SECTION("web_auth_effective_role -- auth disabled collapses to full access, inertly");

    web_auth_table_t t;
    web_auth_table_init(&t); // empty table -- no session exists anywhere

    uint8_t random_token[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(random_token, 123);
    TEST_CHECK(web_auth_effective_role(&t, false, random_token, 60, 1000) == WEB_AUTH_ROLE_ADMIN,
               "auth disabled + no session anywhere still resolves to ADMIN (full access)");
    TEST_CHECK(web_auth_effective_role(&t, false, NULL, 60, 1000) == WEB_AUTH_ROLE_ADMIN,
               "auth disabled + no token presented at all still resolves to ADMIN");

    // Even an expired/garbage table must not leak through and produce NONE:
    // the whole point of the early return is that the table is never
    // consulted while auth is off.
    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 1);
    web_auth_table_create_session(&t, issued, "0.0.0.0", WEB_AUTH_ROLE_USER, 0);
    TEST_CHECK(web_auth_effective_role(&t, false, issued, 60, 999999999u) == WEB_AUTH_ROLE_ADMIN,
               "auth disabled ignores real (even long-expired) session state and still grants ADMIN");

    // And the reverse: with auth ENABLED, an unknown/garbage token on a
    // non-empty table must not accidentally resolve to ADMIN.
    TEST_CHECK(web_auth_effective_role(&t, true, random_token, 60, 1000) == WEB_AUTH_ROLE_NONE,
               "auth enabled + unrelated token is NONE -- the disabled-path shortcut does not leak"
               " into the enabled path");
}

static void test_lcd_session(void)
{
    TEST_SECTION("web_auth_lcd_session -- single-session lifecycle, independent of the web table");

    web_auth_lcd_session_t s;
    web_auth_lcd_session_init(&s);
    TEST_CHECK(!s.active, "freshly initialized LCD session is inactive");

    web_auth_lcd_session_create(&s, WEB_AUTH_ROLE_ADMIN, 1000);
    TEST_CHECK(s.active && s.role == WEB_AUTH_ROLE_ADMIN, "create activates with the given role");
    TEST_CHECK(web_auth_session_is_valid(s.last_seen_ms, 60, 1000 + 60000u), "valid at its own boundary");
    TEST_CHECK(!web_auth_session_is_valid(s.last_seen_ms, 60, 1000 + 60000u + 1u), "expires past its own timeout");

    web_auth_lcd_session_touch(&s, 5000);
    TEST_CHECK(s.last_seen_ms == 5000, "touch updates last_seen_ms");

    web_auth_lcd_session_destroy(&s);
    TEST_CHECK(!s.active, "destroy deactivates the session");

    // Independence from web timeout: the LCD's own timeout value is
    // whatever the caller passes -- this module keeps no separate LCD
    // timeout constant, per the owner's "two independent timeouts" decision
    // (section 8) -- the web and LCD callers simply pass their own
    // policy-stored value in.
    web_auth_lcd_session_create(&s, WEB_AUTH_ROLE_USER, 0);
    TEST_CHECK(web_auth_session_is_valid(s.last_seen_ms, 5 /* 5 s LCD timeout */, 4999),
               "an LCD session honors whatever (possibly much shorter) timeout its caller passes,"
               " independent of any web session in play");
}

void run_test_web_auth(void)
{
    test_table_init_and_lookup();
    test_unknown_token_effective_role();
    test_lru_eviction();
    test_touch_updates_lru_order();
    test_destroy_session_and_role();
    test_expiry_boundaries();
    test_prompt_window();
    test_touch_extends_and_clears_prompt();
    test_auth_disabled_inert_path();
    test_lcd_session();
}
