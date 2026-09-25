// Host tests for App/drivers/http/totp_http_core.c -- the pure logic behind
// auth_totp_http.c/security_http.c's TOTP routes: the 503-before-anything
// clock gate, the RAM-only pending-enrollment-secret state machine, and the
// single-use/expiring reset-token table. Joins the main combined host-test
// executable; calls only totp_http_core.h's public API, no direct #include
// of the .c file (no static internals this test needs to reach).
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/http/totp_http_core.h"

// --- 503-before-anything-else ordering --------------------------------------

static void test_clock_ready(void)
{
    TEST_SECTION("totp_http_clock_ready -- 503 ordering gate");
    TEST_CHECK(!totp_http_clock_ready(false), "an unsynced clock is never ready");
    TEST_CHECK(totp_http_clock_ready(true), "a synced clock is ready");
}

// --- pending-enrollment-secret state machine --------------------------------

static void test_pending_begin_and_valid(void)
{
    TEST_SECTION("totp_pending -- begin() then is_valid() within the TTL");
    totp_pending_enrollment_t p;
    memset(&p, 0, sizeof(p));
    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0xAB, sizeof(secret));

    totp_pending_begin(&p, secret, 1000u);
    TEST_CHECK(totp_pending_is_valid(&p, 1000u), "valid immediately after begin()");
    TEST_CHECK(memcmp(p.secret, secret, sizeof(secret)) == 0, "the pending secret is stored verbatim");
    TEST_CHECK(totp_pending_is_valid(&p, 1000u + TOTP_PENDING_TTL_MS - 1u),
               "still valid one ms before the TTL boundary");
}

static void test_pending_expires_and_zeroes(void)
{
    TEST_SECTION("totp_pending -- is_valid() lazily expires and zeroes past the TTL");
    totp_pending_enrollment_t p;
    memset(&p, 0, sizeof(p));
    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0xCD, sizeof(secret));

    totp_pending_begin(&p, secret, 1000u);
    uint32_t past_ttl = 1000u + TOTP_PENDING_TTL_MS + 1u;
    TEST_CHECK(!totp_pending_is_valid(&p, past_ttl), "past the TTL, is_valid() reports false");
    TEST_CHECK(!p.active, "the pending slot is deactivated as a side effect of the expiry check");
    uint8_t zero[TOTP_SECRET_LEN];
    memset(zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(p.secret, zero, sizeof(zero)) == 0, "the expired secret bytes are zeroed, never left behind");
}

static void test_pending_never_active_is_invalid(void)
{
    TEST_SECTION("totp_pending -- a never-started pending slot is never valid");
    totp_pending_enrollment_t p;
    memset(&p, 0, sizeof(p));
    TEST_CHECK(!totp_pending_is_valid(&p, 12345u), "no begin() call yet -- always invalid");
}

static void test_pending_begin_overwrites_previous(void)
{
    TEST_SECTION("totp_pending -- a second begin() overwrites and zeroes the first pending secret");
    totp_pending_enrollment_t p;
    memset(&p, 0, sizeof(p));
    uint8_t first[TOTP_SECRET_LEN];
    memset(first, 0x11, sizeof(first));
    uint8_t second[TOTP_SECRET_LEN];
    memset(second, 0x22, sizeof(second));

    totp_pending_begin(&p, first, 100u);
    totp_pending_begin(&p, second, 200u);
    TEST_CHECK(memcmp(p.secret, second, sizeof(second)) == 0, "the second secret replaces the first");
    TEST_CHECK(totp_pending_is_valid(&p, 200u), "valid relative to the second begin()'s start time");
    // 100 + TTL + 1 is past the FIRST begin()'s expiry but still before the
    // SECOND's (200 + TTL) -- still valid here proves expiry is measured
    // from the second begin(), not the first.
    TEST_CHECK(totp_pending_is_valid(&p, 100u + TOTP_PENDING_TTL_MS + 1u),
               "expiry is measured from the SECOND begin(), not the first");
    TEST_CHECK(!totp_pending_is_valid(&p, 200u + TOTP_PENDING_TTL_MS + 1u),
               "past the second begin()'s own expiry, it is finally invalid");
}

static void test_pending_clear(void)
{
    TEST_SECTION("totp_pending_clear -- zeroes and deactivates on demand");
    totp_pending_enrollment_t p;
    memset(&p, 0, sizeof(p));
    uint8_t secret[TOTP_SECRET_LEN];
    memset(secret, 0x33, sizeof(secret));
    totp_pending_begin(&p, secret, 5000u);

    totp_pending_clear(&p);
    TEST_CHECK(!p.active, "clear() deactivates");
    uint8_t zero[TOTP_SECRET_LEN];
    memset(zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(p.secret, zero, sizeof(zero)) == 0, "clear() zeroes the secret bytes");
    TEST_CHECK(!totp_pending_is_valid(&p, 5000u), "a cleared pending is never valid");
}

// --- reset-token table -------------------------------------------------------

static void test_reset_token_single_use(void)
{
    TEST_SECTION("totp_reset_token -- store then consume once, second consume fails");
    totp_reset_token_table_t t;
    totp_reset_token_table_init(&t);
    totp_reset_token_store(&t, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "administrator", 1000u);

    TEST_CHECK(totp_reset_token_consume(&t, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "administrator", 1001u) ==
                   TOTP_RESET_TOKEN_OK,
               "first consume of a fresh token succeeds");
    TEST_CHECK(totp_reset_token_consume(&t, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "administrator", 1002u) ==
                   TOTP_RESET_TOKEN_ALREADY_USED,
               "a second consume of the same token reports ALREADY_USED, not OK or NOT_FOUND");
}

static void test_reset_token_expiry(void)
{
    TEST_SECTION("totp_reset_token -- expires after TOTP_RESET_TOKEN_TTL_MS");
    totp_reset_token_table_t t;
    totp_reset_token_table_init(&t);
    totp_reset_token_store(&t, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "administrator", 1000u);

    uint32_t just_before = 1000u + TOTP_RESET_TOKEN_TTL_MS - 1u;
    uint32_t just_after = 1000u + TOTP_RESET_TOKEN_TTL_MS + 1u;
    TEST_CHECK(totp_reset_token_consume(&t, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "administrator", just_after) ==
                   TOTP_RESET_TOKEN_EXPIRED,
               "past the TTL, consume() reports EXPIRED, not NOT_FOUND or OK");
    // Re-store fresh (the previous slot's consume() above did not mark it
    // used, since it returned EXPIRED before that line) and confirm the
    // not-yet-expired boundary still works.
    totp_reset_token_table_init(&t);
    totp_reset_token_store(&t, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "administrator", 1000u);
    TEST_CHECK(totp_reset_token_consume(&t, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "administrator", just_before) ==
                   TOTP_RESET_TOKEN_OK,
               "just before the TTL boundary, consume() still succeeds");
}

static void test_reset_token_wrong_token_or_username(void)
{
    TEST_SECTION("totp_reset_token -- wrong token or wrong username is NOT_FOUND, never a partial match");
    totp_reset_token_table_t t;
    totp_reset_token_table_init(&t);
    totp_reset_token_store(&t, "cccccccccccccccccccccccccccccccc", "administrator", 1000u);

    TEST_CHECK(totp_reset_token_consume(&t, "dddddddddddddddddddddddddddddddd", "administrator", 1001u) ==
                   TOTP_RESET_TOKEN_NOT_FOUND,
               "the right username but wrong token is NOT_FOUND");
    TEST_CHECK(totp_reset_token_consume(&t, "cccccccccccccccccccccccccccccccc", "someoneelse", 1001u) ==
                   TOTP_RESET_TOKEN_NOT_FOUND,
               "the right token but wrong username is NOT_FOUND (never leaks a cross-user match)");
    // Confirm the real token/username pair is still intact (not consumed by
    // either failed attempt above).
    TEST_CHECK(totp_reset_token_consume(&t, "cccccccccccccccccccccccccccccccc", "administrator", 1002u) ==
                   TOTP_RESET_TOKEN_OK,
               "the correct pair still consumes OK after two unrelated failed lookups");
}

static void test_reset_token_wrong_length_never_matches(void)
{
    TEST_SECTION("totp_reset_token -- a prefix, an extension or an empty token is NOT_FOUND");
    totp_reset_token_table_t t;
    totp_reset_token_table_init(&t);
    totp_reset_token_store(&t, "ffffffffffffffffffffffffffffffff", "administrator", 1000u);

    TEST_CHECK(totp_reset_token_consume(&t, "fffffffffffffffffffffffffffffff", "administrator", 1001u) ==
                   TOTP_RESET_TOKEN_NOT_FOUND,
               "a 31-char prefix of the real token is NOT_FOUND");
    TEST_CHECK(totp_reset_token_consume(&t, "ffffffffffffffffffffffffffffffff0", "administrator", 1001u) ==
                   TOTP_RESET_TOKEN_NOT_FOUND,
               "the real token with one extra char appended is NOT_FOUND");
    TEST_CHECK(totp_reset_token_consume(&t, "", "administrator", 1001u) == TOTP_RESET_TOKEN_NOT_FOUND,
               "an empty token is NOT_FOUND");
    TEST_CHECK(totp_reset_token_consume(&t, "fffffffffffffffffffffffffffffffe", "administrator", 1001u) ==
                   TOTP_RESET_TOKEN_NOT_FOUND,
               "a token differing only in its LAST char is NOT_FOUND (whole width compared)");
    TEST_CHECK(totp_reset_token_consume(&t, "ffffffffffffffffffffffffffffffff", "administrator", 1002u) ==
                   TOTP_RESET_TOKEN_OK,
               "none of the above consumed the real token");
}

static void test_reset_token_table_capacity_evicts_soonest_expiry(void)
{
    TEST_SECTION("totp_reset_token -- a full table of live tokens evicts the one closest to expiring");
    totp_reset_token_table_t t;
    totp_reset_token_table_init(&t);
    // Fill every slot with a live (active, unused, unexpired) token, each
    // with a different expiry, in insertion order 0..3 by slot index --
    // slot 0 (token "0000...") expires soonest.
    for (unsigned i = 0; i < TOTP_RESET_TOKEN_SLOTS; i++) {
        char token[TOTP_RESET_TOKEN_HEX_LEN + 1];
        memset(token, '0' + (int)i, TOTP_RESET_TOKEN_HEX_LEN);
        token[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
        // Store at increasing now_ms so each token's absolute expiry
        // (now_ms + TTL) increases with i -- the first-stored token expires
        // soonest.
        totp_reset_token_store(&t, token, "administrator", 1000u + i);
    }

    // Table is full of live tokens; one more store must evict the
    // soonest-to-expire one, i.e. slot 0's token ("0000...").
    char new_token[TOTP_RESET_TOKEN_HEX_LEN + 1];
    memset(new_token, 'e', TOTP_RESET_TOKEN_HEX_LEN);
    new_token[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
    totp_reset_token_store(&t, new_token, "administrator", 5000u);

    char evicted[TOTP_RESET_TOKEN_HEX_LEN + 1];
    memset(evicted, '0', TOTP_RESET_TOKEN_HEX_LEN);
    evicted[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
    TEST_CHECK(totp_reset_token_consume(&t, evicted, "administrator", 5001u) == TOTP_RESET_TOKEN_NOT_FOUND,
               "the soonest-to-expire token was evicted to make room");
    TEST_CHECK(totp_reset_token_consume(&t, new_token, "administrator", 5001u) == TOTP_RESET_TOKEN_OK,
               "the newly stored token took its place and consumes OK");

    // The other three original tokens (slots 1..3) must still be present.
    for (unsigned i = 1; i < TOTP_RESET_TOKEN_SLOTS; i++) {
        char token[TOTP_RESET_TOKEN_HEX_LEN + 1];
        memset(token, '0' + (int)i, TOTP_RESET_TOKEN_HEX_LEN);
        token[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
        TEST_CHECK(totp_reset_token_consume(&t, token, "administrator", 5001u) == TOTP_RESET_TOKEN_OK,
                   "a non-soonest-expiring original token survives the eviction");
    }
}

static void test_reset_token_store_reuses_expired_slot(void)
{
    TEST_SECTION("totp_reset_token -- an expired-but-not-evicted slot is reused before evicting a live one");
    totp_reset_token_table_t t;
    totp_reset_token_table_init(&t);
    // Fill all slots but one with tokens that will have expired by the time
    // we store one more.
    for (unsigned i = 0; i + 1 < TOTP_RESET_TOKEN_SLOTS; i++) {
        char token[TOTP_RESET_TOKEN_HEX_LEN + 1];
        memset(token, '0' + (int)i, TOTP_RESET_TOKEN_HEX_LEN);
        token[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
        totp_reset_token_store(&t, token, "administrator", 0u); // expires at TOTP_RESET_TOKEN_TTL_MS
    }
    // Last slot: a live token, stored "later" so it has not expired yet.
    char live_token[TOTP_RESET_TOKEN_HEX_LEN + 1];
    memset(live_token, 'l', TOTP_RESET_TOKEN_HEX_LEN);
    live_token[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
    totp_reset_token_store(&t, live_token, "administrator", 1000000u);

    uint32_t now = TOTP_RESET_TOKEN_TTL_MS + 1u; // the first N-1 tokens are now expired; the live one is not
    char new_token[TOTP_RESET_TOKEN_HEX_LEN + 1];
    memset(new_token, 'n', TOTP_RESET_TOKEN_HEX_LEN);
    new_token[TOTP_RESET_TOKEN_HEX_LEN] = '\0';
    totp_reset_token_store(&t, new_token, "administrator", now);

    TEST_CHECK(totp_reset_token_consume(&t, new_token, "administrator", now + 1u) == TOTP_RESET_TOKEN_OK,
               "the new token was stored");
    TEST_CHECK(totp_reset_token_consume(&t, live_token, "administrator", now + 1u) == TOTP_RESET_TOKEN_OK,
               "the still-live token was NOT evicted -- an expired slot was reused instead");
}

// --- board-wide failed-/forgot-attempt cap (plan section 4) -----------------

static void test_forgot_board_cap_success_never_resets(void)
{
    TEST_SECTION("totp_forgot_board_cap -- cleared only by reboot, never by a success");
    totp_forgot_board_cap_t c;
    memset(&c, 0, sizeof(c));
    TEST_CHECK(!totp_forgot_board_cap_blocked(&c), "a fresh boot is not blocked");
    for (unsigned i = 0; i + 1u < TOTP_FORGOT_BOARD_CAP; i++) {
        totp_forgot_board_cap_record(&c, false);
    }
    TEST_CHECK(!totp_forgot_board_cap_blocked(&c), "one failure short of the cap is not blocked");
    totp_forgot_board_cap_record(&c, true);
    TEST_CHECK(c.failures == TOTP_FORGOT_BOARD_CAP - 1u,
               "a successful verification leaves the failure count unchanged (no fresh budget)");
    totp_forgot_board_cap_record(&c, false);
    TEST_CHECK(totp_forgot_board_cap_blocked(&c),
               "the cap-th failure blocks, even with a success recorded in between");
    totp_forgot_board_cap_record(&c, true);
    TEST_CHECK(totp_forgot_board_cap_blocked(&c), "a success never unblocks a capped board");
    totp_forgot_board_cap_record(&c, false);
    TEST_CHECK(c.failures == TOTP_FORGOT_BOARD_CAP, "the count saturates at the cap");
}

void run_test_totp_http_core(void)
{
    test_clock_ready();
    test_pending_begin_and_valid();
    test_pending_expires_and_zeroes();
    test_pending_never_active_is_invalid();
    test_pending_begin_overwrites_previous();
    test_pending_clear();
    test_reset_token_single_use();
    test_reset_token_expiry();
    test_reset_token_wrong_token_or_username();
    test_reset_token_wrong_length_never_matches();
    test_reset_token_table_capacity_evicts_soonest_expiry();
    test_reset_token_store_reuses_expired_slot();
    test_forgot_board_cap_success_never_resets();
}
