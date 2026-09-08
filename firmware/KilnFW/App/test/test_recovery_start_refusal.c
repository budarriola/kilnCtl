// Host tests for App/drivers/http/recovery_start_refusal.h -- the explicit,
// named recovery-mode refusal added for the 2026-09-08 UI aggregate review
// (d89256fe, docs/audits/ui_aggregate_review_2026-09-08.md) finding that
// app.js's recovery banner claimed "Firing is NOT available" while no HTTP
// route actually checked boot_guard_is_recovery_mode() -- the Start control
// stayed enabled and profile_exec_start_post_handler()/
// autotune_start_post_handler() (dashboard_exec_http.c/
// dashboard_autotune_http.c) would have accepted the request.
//
// recovery_start_refusal.h is a static-inline, header-only function -- it
// #includes boot_guard.h for the DECLARATION of boot_guard_is_recovery_mode()
// only. This file supplies a FAKE body for that one symbol (same convention
// test_ota_http.c already uses for the identical symbol) so both branches
// can be driven directly, without dragging in boot_guard.c's real NVS-backed
// counter state -- that module already has its own dedicated, thorough host
// test (test_boot_guard.c).
//
// Own, separate executable (own main(), not part of test_main.c's combined
// binary): test_boot_guard.c #includes boot_guard.c directly into that
// binary, which already defines the REAL boot_guard_is_recovery_mode() --
// this file's fake body would collide with it at link time.
//
// NEGATIVE TEST: test_recovery_mode_refuses_and_names_it() only passes
// because recovery_start_refusal.h's `if (!boot_guard_is_recovery_mode())
// return false;` line is present. Comment out that early return (or invert
// the `!`) and this test fails with:
//   FAIL test_recovery_start_refusal.c:<N>: recovery_mode_refuses_start() returns true in recovery mode
// i.e. a firing/autotune start request in recovery mode would be silently
// accepted at the API layer -- exactly the gap this task closes. Restore by
// reversing the edit by hand; `git diff` on recovery_start_refusal.h then
// comes back empty.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

static bool s_fake_recovery_mode = false;
bool boot_guard_is_recovery_mode(void) { return s_fake_recovery_mode; }

#include "../drivers/http/recovery_start_refusal.h"

static void test_not_recovery_mode_does_not_refuse(void)
{
    s_fake_recovery_mode = false;
    char err[192];
    strcpy(err, "unchanged");
    bool refused = recovery_mode_refuses_start(err, sizeof(err));
    TEST_CHECK(!refused, "recovery_mode_refuses_start() returns false outside recovery mode");
    TEST_CHECK(strcmp(err, "unchanged") == 0,
               "err_msg is left untouched when the request is not refused");
}

static void test_recovery_mode_refuses_and_names_it(void)
{
    s_fake_recovery_mode = true;
    char err[192];
    err[0] = '\0';
    bool refused = recovery_mode_refuses_start(err, sizeof(err));
    TEST_CHECK(refused, "recovery_mode_refuses_start() returns true in recovery mode");
    TEST_CHECK(strstr(err, "RECOVERY MODE") != NULL,
               "the refusal names recovery mode explicitly, matching app.js's banner claim "
               "instead of surfacing a generic 'not started' message");
    s_fake_recovery_mode = false;
}

// A firing/autotune request outside recovery mode must still succeed --
// this function must never become a second, wider gate. Covered by the
// first test above (refused == false) plus this explicit no-op-on-err_msg
// check, so a future edit cannot narrow the condition and still pass.
static void test_outside_recovery_mode_is_a_pure_no_op(void)
{
    s_fake_recovery_mode = false;
    TEST_CHECK(recovery_mode_refuses_start(NULL, 0) == false,
               "a NULL err_msg buffer is safe and the call still returns false outside recovery mode");
}

static void test_null_err_msg_is_safe_when_refusing(void)
{
    s_fake_recovery_mode = true;
    bool refused = recovery_mode_refuses_start(NULL, 0);
    TEST_CHECK(refused, "a NULL err_msg buffer is accepted (no crash) and the refusal still holds");
    s_fake_recovery_mode = false;
}

int main(void)
{
    test_not_recovery_mode_does_not_refuse();
    test_recovery_mode_refuses_and_names_it();
    test_outside_recovery_mode_is_a_pure_no_op();
    test_null_err_msg_is_safe_when_refusing();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
