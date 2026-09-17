// Host tests for App/drivers/net/web_auth_login.c -- the pure role-selection
// logic docs/WEB_AUTH_PLAN.md section 6's login handler calls into. Calls
// the real production function, web_auth_login_role_for_username(), never a
// transcribed copy of its logic.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/web_auth_login.h"

static void test_matching_username_is_admin(void) {
    TEST_SECTION("submitted username == stored admin username -> ADMIN");

    TEST_CHECK(web_auth_login_role_for_username("alice", "alice") == WEB_AUTH_SESSION_ROLE_ADMIN,
               "exact match resolves to ADMIN");
    TEST_CHECK(web_auth_login_role_for_username("Alice", "alice") != WEB_AUTH_SESSION_ROLE_ADMIN,
               "comparison is case-sensitive: 'Alice' != 'alice' -> not ADMIN");
}

static void test_mismatched_username_is_user(void) {
    TEST_SECTION("submitted username != stored admin username -> USER");

    TEST_CHECK(web_auth_login_role_for_username("bob", "alice") == WEB_AUTH_SESSION_ROLE_USER,
               "a different, non-empty username resolves to USER");
}

static void test_no_admin_configured_is_always_user(void) {
    TEST_SECTION("no administrator credential configured (NULL/empty admin username) -> always USER");

    TEST_CHECK(web_auth_login_role_for_username("alice", NULL) == WEB_AUTH_SESSION_ROLE_USER,
               "NULL admin username -> USER even if the submitted name would otherwise match nothing");
    TEST_CHECK(web_auth_login_role_for_username("alice", "") == WEB_AUTH_SESSION_ROLE_USER,
               "empty admin username -> USER");
}

static void test_missing_submitted_username_is_user(void) {
    TEST_SECTION("no/empty submitted username -> USER, never a crash, never ADMIN");

    TEST_CHECK(web_auth_login_role_for_username(NULL, "alice") == WEB_AUTH_SESSION_ROLE_USER,
               "NULL submitted username -> USER");
    TEST_CHECK(web_auth_login_role_for_username("", "alice") == WEB_AUTH_SESSION_ROLE_USER,
               "empty submitted username -> USER");
    TEST_CHECK(web_auth_login_role_for_username(NULL, NULL) == WEB_AUTH_SESSION_ROLE_USER,
               "both NULL -> USER, not a crash");
}

static void test_never_returns_none(void) {
    TEST_SECTION("this function never returns WEB_AUTH_SESSION_ROLE_NONE -- that value means "
                 "\"no session\", not \"no role to check\"");

    TEST_CHECK(web_auth_login_role_for_username("x", "y") != WEB_AUTH_SESSION_ROLE_NONE,
               "mismatched, non-empty inputs never yield NONE");
    TEST_CHECK(web_auth_login_role_for_username(NULL, NULL) != WEB_AUTH_SESSION_ROLE_NONE,
               "NULL inputs never yield NONE either");
}

void run_test_web_auth_login(void) {
    test_matching_username_is_admin();
    test_mismatched_username_is_user();
    test_no_admin_configured_is_always_user();
    test_missing_submitted_username_is_user();
    test_never_returns_none();
}
