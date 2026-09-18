// Host tests for App/drivers/http/security_http_core.c (WEB_AUTH_PLAN.md
// item 6, the password page). No ESP-IDF dependency -- exercises the
// dispatch/validation logic against an injected fake security_backend_
// vtable_t, never the placeholder backend, so these tests prove THIS
// page's orchestration (role gating, PIN/timeout validation, session-
// invalidation call-through) independent of whether items 2/3/4 have
// landed yet.
#include <string.h>

#include "test_common.h"

#include "../drivers/http/security_backend.h"
#include "../drivers/http/security_http_core.h"

// ---- Fake backend -----------------------------------------------------
// Records every call so a test can assert both the return-mapping AND that
// the right backend entry point actually ran (or, just as important,
// did NOT run -- e.g. a rejected weak password must never reach the store).
static struct {
    int set_web_password_calls;
    security_role_t last_web_password_role;
    char last_username[64];
    char last_password[160];
    security_err_t set_web_password_result;

    int set_lcd_pin_calls;
    security_role_t last_pin_role;
    char last_pin[16];
    security_err_t set_lcd_pin_result;

    int set_policy_calls;
    security_policy_t last_policy;
    security_err_t set_policy_result;

    int invalidate_calls;
    security_role_t last_invalidated_role;
    security_role_t invalidated_roles_seen[4]; // order the seam was called in, this test cycle
    int invalidated_roles_seen_count;

    int clear_all_credentials_calls;
    security_err_t clear_all_credentials_result;
} fake;

static void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.set_web_password_result = SECURITY_OK;
    fake.set_lcd_pin_result = SECURITY_OK;
    fake.set_policy_result = SECURITY_OK;
    fake.clear_all_credentials_result = SECURITY_OK;
}

static security_err_t fake_set_web_password(security_role_t role, const char *username, const char *password)
{
    fake.set_web_password_calls++;
    fake.last_web_password_role = role;
    fake.last_username[0] = '\0';
    if (username) {
        strncpy(fake.last_username, username, sizeof(fake.last_username) - 1);
    }
    strncpy(fake.last_password, password, sizeof(fake.last_password) - 1);
    return fake.set_web_password_result;
}

static security_err_t fake_set_lcd_pin(security_role_t role, const char *pin)
{
    fake.set_lcd_pin_calls++;
    fake.last_pin_role = role;
    strncpy(fake.last_pin, pin, sizeof(fake.last_pin) - 1);
    return fake.set_lcd_pin_result;
}

static security_err_t fake_set_policy(const security_policy_t *policy)
{
    fake.set_policy_calls++;
    fake.last_policy = *policy;
    return fake.set_policy_result;
}

static bool fake_get_config(security_config_t *out)
{
    memset(out, 0, sizeof(*out));
    return true;
}

static void fake_invalidate_sessions_for_role(security_role_t role)
{
    fake.invalidate_calls++;
    fake.last_invalidated_role = role;
    if (fake.invalidated_roles_seen_count < 4) {
        fake.invalidated_roles_seen[fake.invalidated_roles_seen_count++] = role;
    }
}

static security_err_t fake_clear_all_credentials(void)
{
    fake.clear_all_credentials_calls++;
    return fake.clear_all_credentials_result;
}

static const security_backend_vtable_t fake_vtable = {
    .set_web_password = fake_set_web_password,
    .set_lcd_pin = fake_set_lcd_pin,
    .set_policy = fake_set_policy,
    .get_config = fake_get_config,
    .invalidate_sessions_for_role = fake_invalidate_sessions_for_role,
    .clear_all_credentials = fake_clear_all_credentials,
};

static security_request_t blank_request(security_cmd_t cmd)
{
    security_request_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = cmd;
    return req;
}

// ---- security_pin_is_valid / security_timeout_minutes_is_valid --------

static void test_pin_validation(void)
{
    TEST_SECTION("security_pin_is_valid -- item 3's 4-8 digit / must-differ rule");

    TEST_CHECK(security_pin_is_valid("1234", ""), "4 digits, no other PIN yet -- valid");
    TEST_CHECK(security_pin_is_valid("12345678", ""), "8 digits -- valid (upper bound)");
    TEST_CHECK(!security_pin_is_valid("123", ""), "3 digits -- too short, refused");
    TEST_CHECK(!security_pin_is_valid("123456789", ""), "9 digits -- too long, refused");
    TEST_CHECK(!security_pin_is_valid("12a4", ""), "non-digit character -- refused");
    TEST_CHECK(!security_pin_is_valid("", ""), "empty PIN -- refused");
    TEST_CHECK(!security_pin_is_valid("1234", "1234"), "identical to the other PIN -- refused (item 3)");
    TEST_CHECK(security_pin_is_valid("1234", "5678"), "differs from the other PIN -- valid");
}

static void test_timeout_validation(void)
{
    TEST_SECTION("security_timeout_minutes_is_valid -- item 8's 1-60 or never range");

    TEST_CHECK(security_timeout_minutes_is_valid(1), "1 minute -- valid (lower bound)");
    TEST_CHECK(security_timeout_minutes_is_valid(60), "60 minutes -- valid (upper bound)");
    TEST_CHECK(security_timeout_minutes_is_valid(-1), "-1 (\"never\") -- valid sentinel, not an error");
    TEST_CHECK(!security_timeout_minutes_is_valid(0), "0 minutes -- refused");
    TEST_CHECK(!security_timeout_minutes_is_valid(61), "61 minutes -- refused");
    TEST_CHECK(!security_timeout_minutes_is_valid(-2), "-2 -- refused, only -1 is the sentinel");
}

// ---- security_http_dispatch -------------------------------------------

static void test_admin_only_gate(void)
{
    TEST_SECTION("security_http_dispatch -- item 6's ADMIN-only gate");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_SET_ADMIN_PASSWORD);
    strcpy(req.username, "admin");
    strcpy(req.password, "irrelevant-for-this-check");

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_USER, &req, &result);
    TEST_CHECK(result.http_status == 403, "a user-role caller gets 403 on SET_ADMIN_PASSWORD");
    TEST_CHECK(fake.set_web_password_calls == 0, "the backend is never touched for a refused caller");
    TEST_CHECK(fake.invalidate_calls == 0, "no session invalidation on a refused call either");

    // Same gate for every other command -- the plan says "every method",
    // not just the password one.
    fake_reset();
    security_request_t policy_req = blank_request(SECURITY_CMD_SET_POLICY);
    policy_req.policy.web_timeout_min = 10;
    policy_req.policy.lcd_timeout_min = 10;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_USER, &policy_req, &result);
    TEST_CHECK(result.http_status == 403, "a user-role caller gets 403 on SET_POLICY too");
    TEST_CHECK(fake.set_policy_calls == 0, "policy backend never touched for a refused caller");

    // Item 12b: the new clear-credentials action gets the same ADMIN-only
    // gate as every other command on this page -- a `user` session must not
    // be able to wipe everyone's credentials.
    fake_reset();
    security_request_t clear_req = blank_request(SECURITY_CMD_CLEAR_CREDENTIALS);
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_USER, &clear_req, &result);
    TEST_CHECK(result.http_status == 403, "a user-role caller gets 403 on CLEAR_CREDENTIALS");
    TEST_CHECK(fake.clear_all_credentials_calls == 0, "the store is never touched for a refused caller");
    TEST_CHECK(fake.invalidate_calls == 0, "no session invalidation on a refused clear either");
}

static void test_set_admin_password_success_invalidates_admin_only(void)
{
    TEST_SECTION("SET_ADMIN_PASSWORD success -- invalidates the administrator role, not user");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_SET_ADMIN_PASSWORD);
    strcpy(req.username, "administrator");
    strcpy(req.password, "a real administrator password");

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 200, "administrator changing the admin password succeeds");
    TEST_CHECK(fake.set_web_password_calls == 1, "backend set_web_password called exactly once");
    TEST_CHECK(fake.last_web_password_role == SECURITY_ROLE_ADMIN, "called with the ADMIN role");
    TEST_CHECK(strcmp(fake.last_username, "administrator") == 0, "username passed through unchanged");
    TEST_CHECK(strcmp(fake.last_password, "a real administrator password") == 0,
               "password passed through unchanged (never truncated/mangled)");
    TEST_CHECK(fake.invalidate_calls == 1, "session invalidation seam called exactly once");
    TEST_CHECK(fake.last_invalidated_role == SECURITY_ROLE_ADMIN,
               "invalidation targets the ADMIN role, matching item 6's \"that role\" rule");
    TEST_CHECK(result.invalidated_sessions && result.invalidated_role == SECURITY_ROLE_ADMIN,
               "result reports the invalidation so the HTTP layer can force a re-login");
}

static void test_set_user_password_invalidates_user_only(void)
{
    TEST_SECTION("SET_USER_PASSWORD success -- invalidates the user role only");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_SET_USER_PASSWORD);
    strcpy(req.password, "a real user password");

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 200, "administrator setting the user password succeeds");
    TEST_CHECK(fake.last_web_password_role == SECURITY_ROLE_USER, "backend called with the USER role");
    TEST_CHECK(fake.last_username[0] == '\0', "no username is passed for the single `user` record");
    TEST_CHECK(fake.invalidate_calls == 1 && fake.last_invalidated_role == SECURITY_ROLE_USER,
               "invalidation targets USER, never ADMIN -- other-role sessions must stay intact (item 6)");
}

static void test_weak_password_never_reaches_backend(void)
{
    TEST_SECTION("a backend-rejected (weak) password never invalidates sessions");

    fake_reset();
    fake.set_web_password_result = SECURITY_ERR_WEAK;
    security_request_t req = blank_request(SECURITY_CMD_SET_ADMIN_PASSWORD);
    strcpy(req.username, "administrator");
    strcpy(req.password, "short");

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 400, "a weak-password refusal maps to 400, not 200 or 500");
    TEST_CHECK(fake.set_web_password_calls == 1, "the backend WAS asked (strength check lives behind the seam)");
    TEST_CHECK(fake.invalidate_calls == 0, "no session invalidation follows a refused change");
    TEST_CHECK(!result.invalidated_sessions, "result does not claim an invalidation that didn't happen");
}

static void test_empty_password_refused_before_backend(void)
{
    TEST_SECTION("an empty password is refused by this page's own check, before the backend is called");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_SET_USER_PASSWORD);
    req.password[0] = '\0';

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 400, "empty password -- 400");
    TEST_CHECK(fake.set_web_password_calls == 0, "backend never called for an empty password");
}

static void test_lcd_pin_dispatch(void)
{
    TEST_SECTION("SET_LCD_PIN -- validated here, invalidates the matching LCD role");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_SET_LCD_PIN);
    req.lcd_pin_role = SECURITY_ROLE_USER;
    strcpy(req.lcd_pin, "4321");
    req.lcd_pin_other[0] = '\0';

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);
    TEST_CHECK(result.http_status == 200, "valid 4-digit user PIN accepted");
    TEST_CHECK(fake.set_lcd_pin_calls == 1 && fake.last_pin_role == SECURITY_ROLE_USER,
               "backend called with the USER role");
    TEST_CHECK(fake.invalidate_calls == 1 && fake.last_invalidated_role == SECURITY_ROLE_USER,
               "invalidates the LCD session tagged for the role whose PIN changed");

    // Out-of-range PIN never reaches the backend at all.
    fake_reset();
    security_request_t bad = blank_request(SECURITY_CMD_SET_LCD_PIN);
    bad.lcd_pin_role = SECURITY_ROLE_ADMIN;
    strcpy(bad.lcd_pin, "12");
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &bad, &result);
    TEST_CHECK(result.http_status == 400, "2-digit PIN refused");
    TEST_CHECK(fake.set_lcd_pin_calls == 0, "backend never called for an out-of-range PIN");

    // Same-as-other-PIN never reaches the backend either.
    fake_reset();
    security_request_t dup = blank_request(SECURITY_CMD_SET_LCD_PIN);
    dup.lcd_pin_role = SECURITY_ROLE_ADMIN;
    strcpy(dup.lcd_pin, "9999");
    strcpy(dup.lcd_pin_other, "9999");
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &dup, &result);
    TEST_CHECK(result.http_status == 400, "PIN identical to the other role's PIN is refused");
    TEST_CHECK(fake.set_lcd_pin_calls == 0, "backend never called when the two PINs would collide");
}

static void test_policy_dispatch_never_invalidates(void)
{
    TEST_SECTION("SET_POLICY -- validated here, never invalidates sessions (item 6: \"a preference\")");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_SET_POLICY);
    req.policy.web_enabled = true;
    req.policy.lcd_enabled = false;
    req.policy.web_timeout_min = 15;
    req.policy.lcd_timeout_min = -1;

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 200, "valid policy accepted");
    TEST_CHECK(fake.set_policy_calls == 1, "backend set_policy called exactly once");
    TEST_CHECK(fake.last_policy.web_enabled && !fake.last_policy.lcd_enabled &&
               fake.last_policy.web_timeout_min == 15 && fake.last_policy.lcd_timeout_min == -1,
               "policy fields passed through unchanged");
    TEST_CHECK(fake.invalidate_calls == 0, "a policy change never calls the session-invalidation seam");
    TEST_CHECK(!result.invalidated_sessions, "result does not report an invalidation for a policy change");

    // Out-of-range timeout never reaches the backend.
    fake_reset();
    security_request_t bad = blank_request(SECURITY_CMD_SET_POLICY);
    bad.policy.web_timeout_min = 0;
    bad.policy.lcd_timeout_min = 10;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &bad, &result);
    TEST_CHECK(result.http_status == 400, "0-minute timeout refused");
    TEST_CHECK(fake.set_policy_calls == 0, "backend never called for an out-of-range timeout");
}

static void test_not_implemented_maps_to_501(void)
{
    TEST_SECTION("SECURITY_ERR_NOT_IMPLEMENTED (the placeholder backend's answer today) maps to 501");

    fake_reset();
    fake.set_web_password_result = SECURITY_ERR_NOT_IMPLEMENTED;
    security_request_t req = blank_request(SECURITY_CMD_SET_ADMIN_PASSWORD);
    strcpy(req.username, "administrator");
    strcpy(req.password, "a real administrator password");

    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);
    TEST_CHECK(result.http_status == 501, "not-implemented is reported as 501, never disguised as 200");
    TEST_CHECK(fake.invalidate_calls == 0, "no session invalidation when nothing was actually saved");
}

static void test_unknown_command_refused(void)
{
    TEST_SECTION("SECURITY_CMD_UNKNOWN is refused, not silently ignored");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_UNKNOWN);
    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);
    TEST_CHECK(result.http_status == 400, "unknown command -- 400");
    TEST_CHECK(fake.set_web_password_calls == 0 && fake.set_lcd_pin_calls == 0 && fake.set_policy_calls == 0,
               "no backend entry point is called for an unrecognized command");
}

static void test_clear_credentials_success_invalidates_both_roles(void)
{
    TEST_SECTION("CLEAR_CREDENTIALS success -- clears the store, invalidates ADMIN and USER");

    fake_reset();
    security_request_t req = blank_request(SECURITY_CMD_CLEAR_CREDENTIALS);
    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 200, "an admin caller clearing credentials succeeds");
    TEST_CHECK(fake.clear_all_credentials_calls == 1, "the store's clear-all entry point is called exactly once");
    TEST_CHECK(fake.invalidate_calls == 2, "both roles' sessions are invalidated, not just the caller's");
    TEST_CHECK(fake.invalidated_roles_seen_count == 2 &&
                   fake.invalidated_roles_seen[0] == SECURITY_ROLE_ADMIN &&
                   fake.invalidated_roles_seen[1] == SECURITY_ROLE_USER,
               "ADMIN is invalidated before USER, and both roles are covered");
    TEST_CHECK(result.invalidated_sessions, "result reports the invalidation so the caller's own session drops too");
}

static void test_clear_credentials_storage_failure_maps_to_500(void)
{
    TEST_SECTION("CLEAR_CREDENTIALS -- a failed read-back verification maps to 500, skips invalidation");

    fake_reset();
    fake.clear_all_credentials_result = SECURITY_ERR_STORAGE;
    security_request_t req = blank_request(SECURITY_CMD_CLEAR_CREDENTIALS);
    security_result_t result;
    security_http_dispatch(&fake_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 500, "a storage failure maps to 500, not a false 200");
    TEST_CHECK(fake.clear_all_credentials_calls == 1, "the store was asked");
    TEST_CHECK(fake.invalidate_calls == 0,
               "sessions are never invalidated when the underlying clear could not be confirmed");
    TEST_CHECK(!result.invalidated_sessions, "result does not claim an invalidation that didn't happen");
}

static void test_clear_credentials_null_backend_member_does_not_crash(void)
{
    // Finding 7 fix (2026-09-17 review): a vtable can be non-NULL (the
    // dispatch-level check at the top of security_http_dispatch() passes)
    // while an individual member is NULL -- a partially-populated backend.
    // Before the fix, SECURITY_CMD_CLEAR_CREDENTIALS called
    // vt->clear_all_credentials() with no per-pointer check, which is a
    // hard crash (call through a NULL function pointer) on the host build
    // too, so this test would abort the whole test binary before the fix
    // rather than merely failing a TEST_CHECK -- that crash IS the
    // "red before the fix" signal for this finding.
    TEST_SECTION("CLEAR_CREDENTIALS -- vtable present but clear_all_credentials member is NULL (Finding 7)");

    fake_reset();
    security_backend_vtable_t partial_vtable = fake_vtable;
    partial_vtable.clear_all_credentials = NULL;

    security_request_t req = blank_request(SECURITY_CMD_CLEAR_CREDENTIALS);
    security_result_t result;
    security_http_dispatch(&partial_vtable, SECURITY_ROLE_ADMIN, &req, &result);

    TEST_CHECK(result.http_status == 500, "a missing backend op maps to 500, not a crash");
    TEST_CHECK(fake.invalidate_calls == 0, "sessions are never invalidated when the op couldn't run at all");
}

void run_test_security_http_core(void)
{
    test_pin_validation();
    test_timeout_validation();
    test_admin_only_gate();
    test_set_admin_password_success_invalidates_admin_only();
    test_set_user_password_invalidates_user_only();
    test_weak_password_never_reaches_backend();
    test_empty_password_refused_before_backend();
    test_lcd_pin_dispatch();
    test_policy_dispatch_never_invalidates();
    test_not_implemented_maps_to_501();
    test_unknown_command_refused();
    test_clear_credentials_success_invalidates_both_roles();
    test_clear_credentials_storage_failure_maps_to_500();
    test_clear_credentials_null_backend_member_does_not_crash();
}
