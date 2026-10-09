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

    size_t idx = web_auth_table_create_session(&t, h1, "10.0.0.5", WEB_AUTH_SESSION_ROLE_ADMIN, 1000);
    TEST_CHECK(idx < WEB_AUTH_WEB_SLOT_COUNT, "create_session returns a real slot index");
    TEST_CHECK(web_auth_table_find_by_token(&t, h1) == (int)idx, "lookup finds the session just created");
    TEST_CHECK(strcmp(t.slots[idx].client_ip, "10.0.0.5") == 0, "client_ip stored");
    TEST_CHECK(t.slots[idx].role == WEB_AUTH_SESSION_ROLE_ADMIN, "role stored");

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
    web_auth_table_create_session(&t, issued, "1.2.3.4", WEB_AUTH_SESSION_ROLE_USER, 0);

    uint8_t unknown[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(unknown, 99);
    TEST_CHECK(web_auth_effective_role(&t, true, unknown, "1.2.3.4", 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "a token that was never issued resolves to NONE, not the role of some other slot");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, "1.2.3.4", 300, 100) == WEB_AUTH_SESSION_ROLE_USER,
               "sanity: the actually-issued token still resolves correctly, from the address it was issued to");
    TEST_CHECK(web_auth_effective_role(&t, true, NULL, "1.2.3.4", 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
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
        web_auth_table_create_session(&t, hashes[i], "0.0.0.0", WEB_AUTH_SESSION_ROLE_USER, (uint32_t)(1000 + i));
    }
    for (int i = 0; i < (int)WEB_AUTH_WEB_SLOT_COUNT; i++) {
        TEST_CHECK(web_auth_table_find_by_token(&t, hashes[i]) != -1, "all 8 initial sessions are present");
    }

    make_hash(hashes[WEB_AUTH_WEB_SLOT_COUNT], 250);
    web_auth_table_create_session(&t, hashes[WEB_AUTH_WEB_SLOT_COUNT], "0.0.0.0", WEB_AUTH_SESSION_ROLE_ADMIN, 5000);

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
        web_auth_table_create_session(&t, hashes[i], "0.0.0.0", WEB_AUTH_SESSION_ROLE_USER, (uint32_t)(1000 + i));
    }
    // Touch slot 0 (originally the oldest) so it is now the most recent.
    int idx0 = web_auth_table_find_by_token(&t, hashes[0]);
    web_auth_table_touch(&t, (size_t)idx0, 9999);

    uint8_t newcomer[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(newcomer, 250);
    web_auth_table_create_session(&t, newcomer, "0.0.0.0", WEB_AUTH_SESSION_ROLE_ADMIN, 10000);

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
    size_t user_idx = web_auth_table_create_session(&t, user_h, "0.0.0.0", WEB_AUTH_SESSION_ROLE_USER, 1000);
    web_auth_table_create_session(&t, admin_h, "0.0.0.0", WEB_AUTH_SESSION_ROLE_ADMIN, 1000);

    web_auth_table_destroy_session(&t, user_idx);
    TEST_CHECK(web_auth_table_find_by_token(&t, user_h) == -1, "logout removes exactly that session");
    TEST_CHECK(web_auth_table_find_by_token(&t, admin_h) != -1, "the other session is untouched by a logout");

    // Section 6: "changing a password invalidates every session for that role."
    web_auth_table_init(&t);
    uint8_t admin1[WEB_AUTH_TOKEN_HASH_LEN], admin2[WEB_AUTH_TOKEN_HASH_LEN], user1[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(admin1, 1);
    make_hash(admin2, 2);
    make_hash(user1, 3);
    web_auth_table_create_session(&t, admin1, "0.0.0.0", WEB_AUTH_SESSION_ROLE_ADMIN, 1000);
    web_auth_table_create_session(&t, admin2, "0.0.0.0", WEB_AUTH_SESSION_ROLE_ADMIN, 1000);
    web_auth_table_create_session(&t, user1, "0.0.0.0", WEB_AUTH_SESSION_ROLE_USER, 1000);
    web_auth_table_destroy_role(&t, WEB_AUTH_SESSION_ROLE_ADMIN);
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
    size_t idx = web_auth_table_create_session(&t, h, "0.0.0.0", WEB_AUTH_SESSION_ROLE_USER, 0);
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
    TEST_CHECK(web_auth_effective_role(&t, false, random_token, "9.9.9.9", 60, 1000) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "auth disabled + no session anywhere still resolves to ADMIN (full access)");
    TEST_CHECK(web_auth_effective_role(&t, false, NULL, NULL, 60, 1000) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "auth disabled + no token or client_ip presented at all still resolves to ADMIN");

    // Even an expired/garbage table must not leak through and produce NONE:
    // the whole point of the early return is that the table is never
    // consulted while auth is off.
    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 1);
    web_auth_table_create_session(&t, issued, "0.0.0.0", WEB_AUTH_SESSION_ROLE_USER, 0);
    TEST_CHECK(web_auth_effective_role(&t, false, issued, "9.9.9.9", 60, 999999999u) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "auth disabled ignores real (even long-expired) session state and a mismatched client_ip,"
               " and still grants ADMIN");

    // And the reverse: with auth ENABLED, an unknown/garbage token on a
    // non-empty table must not accidentally resolve to ADMIN.
    TEST_CHECK(web_auth_effective_role(&t, true, random_token, "9.9.9.9", 60, 1000) == WEB_AUTH_SESSION_ROLE_NONE,
               "auth enabled + unrelated token is NONE -- the disabled-path shortcut does not leak"
               " into the enabled path");
}

static void test_lcd_session(void)
{
    TEST_SECTION("web_auth_lcd_session -- single-session lifecycle, independent of the web table");

    web_auth_lcd_session_t s;
    web_auth_lcd_session_init(&s);
    TEST_CHECK(!s.active, "freshly initialized LCD session is inactive");

    web_auth_lcd_session_create(&s, WEB_AUTH_SESSION_ROLE_ADMIN, 1000);
    TEST_CHECK(s.active && s.role == WEB_AUTH_SESSION_ROLE_ADMIN, "create activates with the given role");
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
    web_auth_lcd_session_create(&s, WEB_AUTH_SESSION_ROLE_USER, 0);
    TEST_CHECK(web_auth_session_is_valid(s.last_seen_ms, 5 /* 5 s LCD timeout */, 4999),
               "an LCD session honors whatever (possibly much shorter) timeout its caller passes,"
               " independent of any web session in play");
}

// --- Items 10 & 11: the admin-credential bootstrap state must resolve
// identically no matter which of the three paths (first boot, field
// upgrade, physical reset) could in principle produce it -- see this
// predicate's header comment for why only the physical reset actually
// reaches it today. Exercised here purely on the two booleans the real
// callers (item 10's reset, item 6's login/password page) would derive from
// web_auth_policy_effective_enabled() and web_auth_store_password_
// configured()/_pin_configured() -- this test does not itself depend on
// which module produced them, which IS the point: the predicate cannot be
// fooled into a different answer by its caller's history. ------------------
static void test_admin_bootstrap_needed(void)
{
    TEST_SECTION("web_auth_admin_bootstrap_needed -- neither a lockout nor a silent bypass");

    TEST_CHECK(web_auth_admin_bootstrap_needed(true, false) == true,
               "enabled + no administrator credential (the post-physical-reset state, item 10) "
               "must report bootstrap-needed");
    TEST_CHECK(web_auth_admin_bootstrap_needed(true, true) == false,
               "enabled + administrator credential present is the ordinary case -- no bootstrap");
    TEST_CHECK(web_auth_admin_bootstrap_needed(false, false) == false,
               "auth OFF must never report bootstrap-needed regardless of credential state -- "
               "first boot and field upgrade both collapse to effective_enabled==false before "
               "this predicate is ever consulted, so this also covers those two paths");
    TEST_CHECK(web_auth_admin_bootstrap_needed(false, true) == false,
               "auth off with a credential already configured is still not a bootstrap case");
}

// *** 2026-09-17 adversarial review, Finding 1 / prior defect 5: a session
// token replayed from a DIFFERENT client_ip than the one it was issued to
// must resolve to NONE, not the session's real role -- the stolen-cookie
// replay scenario the review's Finding 1 walks through. Before the fix,
// web_auth_effective_role() had no client_ip parameter at all and
// http_auth_session_resolve() discarded the address entirely
// ((void)client_ip;), so a captured cookie worked from anywhere on the LAN. ***
static void test_effective_role_ip_binding(void)
{
    TEST_SECTION("web_auth_effective_role -- client_ip binding (Finding 1)");

    web_auth_table_t t;
    web_auth_table_init(&t);

    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 42);
    web_auth_table_create_session(&t, issued, "192.168.1.50", WEB_AUTH_SESSION_ROLE_ADMIN, 0);

    TEST_CHECK(web_auth_effective_role(&t, true, issued, "192.168.1.50", 300, 100) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "the address the session was actually issued to still resolves its real role");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, "192.168.1.99", 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "a valid token replayed from a DIFFERENT address on the same LAN is denied, not"
               " granted its stored role -- the cookie-theft/replay scenario this binding exists"
               " for");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, NULL, 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "an unresolvable peer address (NULL) can never match a real binding, so it denies"
               " too, rather than being treated as \"skip the check\"");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, "192.168.1.5", 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "a mere prefix match (192.168.1.5 vs the stored 192.168.1.50) is not a match --"
               " this binding is exact-string, not prefix-based");
}

// *** 2026-09-17 adversarial review, Finding 1: WEB_AUTH_CLIENT_IP_LEN used
// to be 16 ("enough for a dotted-quad IPv4 string"), which silently
// truncated any IPv6 address (every real one is longer than 15 characters)
// to 15 characters before storing it. web_auth_effective_role() then
// compared that truncated stored copy against the caller's untruncated
// address on every subsequent request, so an IPv6 client could log in but
// could never pass the IP-binding check again -- a silent 401 lockout on
// the very next request. No existing test before this fix used an address
// longer than 15 characters ("10.0.0.5", "192.168.1.50", "1.2.3.4",
// "9.9.9.9" are all IPv4 and comfortably short), which is why this shipped.
// This test uses a realistic IPv6 link-local address (the CLAUDE.md-named
// "likely first case" once a client reaches the board over IPv6). ***
static void test_client_ip_full_length_ipv6_round_trips(void)
{
    TEST_SECTION("web_auth_effective_role -- a full IPv6 address round-trips (Finding 1)");

    web_auth_table_t t;
    web_auth_table_init(&t);

    const char *ipv6 = "fe80::a1b2:c3d4:e5f6:7890"; // 25 chars, well past the old 15-char ceiling
    TEST_CHECK(strlen(ipv6) > 15, "test sanity: this address is longer than the old truncation point");

    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 7);
    size_t idx = web_auth_table_create_session(&t, issued, ipv6, WEB_AUTH_SESSION_ROLE_ADMIN, 0);

    TEST_CHECK(strcmp(t.slots[idx].client_ip, ipv6) == 0,
               "the full IPv6 address is stored without truncation");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, ipv6, 300, 100) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "the very next request from the SAME full address still resolves its real role -- "
               "before the fix, the stored (truncated) copy never matched this untruncated compare");
}

// *** Finding 1's second consequence: truncating to 15 characters collapsed
// two DISTINCT IPv6 clients that merely share a long common prefix (very
// common for two hosts on the same fe80::/10 link) onto the same stored
// string -- a cross-client session-binding hole, not merely a lockout. Two
// sessions minted for two different real addresses must never be
// indistinguishable from each other. ***
static void test_client_ip_distinct_ipv6_addresses_do_not_collide(void)
{
    TEST_SECTION("web_auth_effective_role -- distinct IPv6 addresses sharing a long prefix do not collide (Finding 1)");

    web_auth_table_t t;
    web_auth_table_init(&t);

    // Identical in their first 15 characters ("fe80::1111:1111"), distinct
    // thereafter -- exactly the shape that collapsed under the old 16-byte
    // (15 usable + NUL) buffer.
    const char *ip_a = "fe80::1111:1111:0001";
    const char *ip_b = "fe80::1111:1111:0002";
    TEST_CHECK(strncmp(ip_a, ip_b, 15) == 0, "test sanity: the two addresses share a 15-character prefix");
    TEST_CHECK(strcmp(ip_a, ip_b) != 0, "test sanity: the two addresses are still distinct in full");

    uint8_t token_a[WEB_AUTH_TOKEN_HASH_LEN];
    uint8_t token_b[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(token_a, 11);
    make_hash(token_b, 211);

    web_auth_table_create_session(&t, token_a, ip_a, WEB_AUTH_SESSION_ROLE_ADMIN, 0);
    web_auth_table_create_session(&t, token_b, ip_b, WEB_AUTH_SESSION_ROLE_USER, 0);

    // Client A's session, presented from client A's real address, resolves.
    TEST_CHECK(web_auth_effective_role(&t, true, token_a, ip_a, 300, 100) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "client A's session resolves from client A's own address");
    // Client A's session must NOT resolve from client B's address -- if the
    // two addresses had collapsed to the same stored string (the pre-fix
    // bug), this compare would spuriously match too.
    TEST_CHECK(web_auth_effective_role(&t, true, token_a, ip_b, 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "client A's session is denied when presented from client B's distinct address, "
               "even though the two addresses share a long common prefix");
    TEST_CHECK(web_auth_effective_role(&t, true, token_b, ip_b, 300, 100) == WEB_AUTH_SESSION_ROLE_USER,
               "client B's session resolves from client B's own address");
    TEST_CHECK(web_auth_effective_role(&t, true, token_b, ip_a, 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "client B's session is denied when presented from client A's distinct address");
}

// *** 2026-09-17 review, Finding 2: both IPv6 tests above use 25- and
// 20-character addresses -- comfortably short of WEB_AUTH_CLIENT_IP_LEN's
// full 46-byte (45 usable + NUL) capacity, the same convenient-short-
// literal habit that let the original 16-byte truncation ship green. This
// test drives an address at the actual maximum storable length (45
// characters) to pin the boundary, not merely "longer than the old bug's
// ceiling". ***
static void test_client_ip_max_length_round_trips(void)
{
    TEST_SECTION("web_auth_effective_role -- a client_ip at the full 45-char maximum round-trips "
                 "(Finding 2)");

    web_auth_table_t t;
    web_auth_table_init(&t);

    // 45 characters -- exactly WEB_AUTH_CLIENT_IP_LEN (46) minus the NUL
    // terminator, the longest string this module ever stores without
    // hitting the reject-on-overflow path below. This is the IPv4-mapped
    // IPv6 form, the genuine longest output of the one real producer,
    // ota_http_get_client_ip()'s inet_ntop(AF_INET6, ...) call
    // (ota_http.c) -- not a %scope-suffixed literal like the one this test
    // used before the 2026-09-17 review's follow-up pass: that call passes
    // inet_ntop only the bare 16-byte in6_addr, never sin6_scope_id, so a
    // "%eth1" suffix can never actually be produced here. The length (45)
    // was already right; only the form was unreachable.
    const char *ipv6_max = "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255";
    TEST_CHECK(strlen(ipv6_max) == WEB_AUTH_CLIENT_IP_LEN - 1u,
               "test sanity: this address is exactly the maximum storable length");

    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 17);
    size_t idx = web_auth_table_create_session(&t, issued, ipv6_max, WEB_AUTH_SESSION_ROLE_ADMIN, 0);

    TEST_CHECK(strcmp(t.slots[idx].client_ip, ipv6_max) == 0,
               "the maximum-length address is stored in full, not truncated by even one byte");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, ipv6_max, 300, 100) == WEB_AUTH_SESSION_ROLE_ADMIN,
               "the very next request from the same maximum-length address still resolves its role");
}

// *** Finding 2's second half: drives web_auth_table_create_session()'s
// reject-on-overflow branch (web_auth_session.c ~58-65) with a string that
// does not fit even in the 46-byte buffer. This branch is dead code in
// production today -- every real producer is ota_http_get_client_ip()'s own
// `char ip[46]` (http_auth_http.c/web_auth_login_http.c), which can never
// hand this function a string this long -- so this test is pinning the
// fail-closed failure mode for a future/hypothetical producer, not
// reproducing a reachable defect. ***
static void test_client_ip_overflow_fails_closed(void)
{
    TEST_SECTION("web_auth_table_create_session -- an over-length client_ip fails closed rather "
                 "than truncating (Finding 2; not reachable from any producer today)");

    web_auth_table_t t;
    web_auth_table_init(&t);

    char too_long[47]; // one byte past WEB_AUTH_CLIENT_IP_LEN (46)
    memset(too_long, 'a', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    TEST_CHECK(strlen(too_long) == WEB_AUTH_CLIENT_IP_LEN,
               "test sanity: this string is exactly one byte too long to store");

    uint8_t issued[WEB_AUTH_TOKEN_HASH_LEN];
    make_hash(issued, 19);
    size_t idx = web_auth_table_create_session(&t, issued, too_long, WEB_AUTH_SESSION_ROLE_ADMIN, 0);

    TEST_CHECK(t.slots[idx].client_ip[0] == '\0',
               "an over-length client_ip is recorded as empty rather than a guessed-at truncation");
    TEST_CHECK(web_auth_effective_role(&t, true, issued, too_long, 300, 100) == WEB_AUTH_SESSION_ROLE_NONE,
               "the over-length address itself can never match the empty stored binding");
    // NOT asserted here: presenting an empty client_ip WOULD match this
    // empty stored binding (web_auth_effective_role() is a plain strcmp,
    // and strcmp("", "") == 0 -- see the create_session comment in
    // web_auth_session.h, Finding 4 of the 2026-09-17 review). The only
    // reason that never happens in production is that ota_http_get_client_ip()
    // never emits an empty string, not anything in this module -- so this
    // test does not claim a guarantee this code does not actually provide.
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
    test_effective_role_ip_binding();
    test_client_ip_full_length_ipv6_round_trips();
    test_client_ip_distinct_ipv6_addresses_do_not_collide();
    test_client_ip_max_length_round_trips();
    test_client_ip_overflow_fails_closed();
    test_lcd_session();
    test_admin_bootstrap_needed();
}
