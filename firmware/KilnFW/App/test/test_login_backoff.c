// Host tests for App/drivers/net/login_backoff.c -- the escalating
// 5s/10s/30s/60s/300s backoff ladder (owner decision 2026-09-21), extracted
// 2026-09-28 so the LCD PIN keypad (lcd_auth_state.c) and the web login
// (web_auth_login_http.c) share this exact policy instead of two
// hand-copied definitions. No ESP-IDF dependency.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/login_backoff.h"

static void test_ladder_escalation(void)
{
    TEST_SECTION("login_backoff -- ladder escalation, 5s/10s/30s/60s/300s");

    login_backoff_state_t s;
    memset(&s, 0, sizeof(s));

    TEST_CHECK(!login_backoff_is_locked(&s, 0), "freshly-zeroed state is not locked");

    uint32_t expected[5] = { 5000u, 10000u, 30000u, 60000u, 300000u };
    uint32_t now = 0;
    for (int i = 0; i < 5; i++) {
        login_backoff_record_failure(&s, now);
        TEST_CHECK(s.failure_count == (uint32_t)(i + 1), "failure_count advances one per failure");
        TEST_CHECK(login_backoff_is_locked(&s, now), "locked immediately after a failure");
        TEST_CHECK(!login_backoff_is_locked(&s, now + expected[i]),
                   "unlocked exactly at now + this step's ladder duration");
        TEST_CHECK(login_backoff_is_locked(&s, now + expected[i] - 1),
                   "still locked 1ms before that boundary");
        now += expected[i]; // advance past this step's lock before the next failure
    }

    // A 6th failure clamps at the ladder's last step (300s), does not grow further.
    login_backoff_record_failure(&s, now);
    TEST_CHECK(s.failure_count == LOGIN_BACKOFF_LADDER_LEN, "failure_count clamps at the ladder length");
    TEST_CHECK(!login_backoff_is_locked(&s, now + 300000u), "6th failure still uses the 300s step, not longer");
}

static void test_cycle_reset(void)
{
    TEST_SECTION("login_backoff -- cycle reset once the last step's lock expires");

    login_backoff_state_t s;
    memset(&s, 0, sizeof(s));

    uint32_t now = 0;
    for (int i = 0; i < 5; i++) {
        login_backoff_record_failure(&s, now);
        now += LOGIN_BACKOFF_LADDER_MS[i];
    }
    TEST_CHECK(s.failure_count == LOGIN_BACKOFF_LADDER_LEN, "reached the top of the ladder");

    // Before the last lock expires, cycle_reset_if_due must not touch anything.
    login_backoff_cycle_reset_if_due(&s, now - 1);
    TEST_CHECK(s.failure_count == LOGIN_BACKOFF_LADDER_LEN, "no reset while still locked");

    // Once the last lock has expired, the next touch resets the cycle.
    login_backoff_cycle_reset_if_due(&s, now);
    TEST_CHECK(s.failure_count == 0, "cycle resets once the top-step lock has expired");
    TEST_CHECK(s.locked_until_ms == 0, "locked_until_ms cleared by the cycle reset");

    // A subsequent failure starts back at the first (5s) step, not the 300s tier.
    login_backoff_record_failure(&s, now);
    TEST_CHECK(!login_backoff_is_locked(&s, now + 5000u), "post-reset failure uses the FIRST ladder step (5s)");
}

static void test_success_clears(void)
{
    TEST_SECTION("login_backoff -- success clears the ladder entirely");

    login_backoff_state_t s;
    memset(&s, 0, sizeof(s));
    login_backoff_record_failure(&s, 0);
    login_backoff_record_failure(&s, 5000);
    TEST_CHECK(s.failure_count == 2, "two failures recorded");

    login_backoff_record_success(&s);
    TEST_CHECK(s.failure_count == 0, "success clears failure_count");
    TEST_CHECK(s.locked_until_ms == 0, "success clears locked_until_ms");
    TEST_CHECK(!login_backoff_is_locked(&s, 5001), "not locked immediately after a success");

    // Next failure after a success starts back at the first step.
    login_backoff_record_failure(&s, 10000);
    TEST_CHECK(!login_backoff_is_locked(&s, 10000 + 5000u), "post-success failure uses the FIRST ladder step (5s)");
}

void run_test_login_backoff(void)
{
    test_ladder_escalation();
    test_cycle_reset();
    test_success_clears();
}
