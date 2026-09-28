// Host tests for App/drivers/ui/lcd_auth_state.c -- docs/WEB_AUTH_PLAN.md
// section 7 (LCD two-PIN keypad entry) and the LCD half of section 8
// (inactivity lock + 10s stay-unlocked prompt). No LVGL, no ESP-IDF.
#include <string.h>

#include "test_common.h"

#include "../drivers/ui/lcd_auth_state.h"

static void test_pin_entry(void)
{
    TEST_SECTION("lcd_pin_entry -- buffer edge cases");

    lcd_pin_entry_t e;
    lcd_pin_entry_reset(&e);
    TEST_CHECK(e.len == 0, "reset starts empty");
    TEST_CHECK(!lcd_pin_entry_can_submit(&e), "empty entry cannot submit");

    TEST_CHECK(!lcd_pin_entry_push_digit(&e, 'a'), "non-digit rejected");
    TEST_CHECK(e.len == 0, "rejected push leaves length unchanged");

    for (int i = 0; i < 3; i++) {
        TEST_CHECK(lcd_pin_entry_push_digit(&e, (char)('1' + i)), "digit push accepted below max");
    }
    TEST_CHECK(e.len == 3, "3 digits entered");
    TEST_CHECK(!lcd_pin_entry_can_submit(&e), "3 digits is below LCD_PIN_MIN_DIGITS (4) -- cannot submit");

    TEST_CHECK(lcd_pin_entry_push_digit(&e, '4'), "4th digit accepted");
    TEST_CHECK(lcd_pin_entry_can_submit(&e), "4 digits meets the minimum -- can submit");

    for (int i = 0; i < 4; i++) {
        lcd_pin_entry_push_digit(&e, '9');
    }
    TEST_CHECK(e.len == LCD_PIN_MAX_DIGITS, "length caps at LCD_PIN_MAX_DIGITS (8)");
    TEST_CHECK(!lcd_pin_entry_push_digit(&e, '9'), "push beyond max digits is refused");
    TEST_CHECK(e.len == LCD_PIN_MAX_DIGITS, "refused push does not grow length past the cap");
    TEST_CHECK(lcd_pin_entry_can_submit(&e), "exactly at the max digits is still submittable");

    TEST_CHECK(lcd_pin_entry_backspace(&e), "backspace at max succeeds");
    TEST_CHECK(e.len == LCD_PIN_MAX_DIGITS - 1, "backspace shrinks length by one");

    lcd_pin_entry_reset(&e);
    TEST_CHECK(!lcd_pin_entry_backspace(&e), "backspace on empty entry is a no-op, reports false");
}

static lcd_pin_role_t s_fake_verify_result;
static char s_fake_verify_last_digits[LCD_PIN_MAX_DIGITS + 1];
static uint8_t s_fake_verify_last_len;

static lcd_pin_role_t fake_verify_fn(const char *digits, uint8_t len)
{
    memcpy(s_fake_verify_last_digits, digits, len);
    s_fake_verify_last_digits[len] = '\0';
    s_fake_verify_last_len = len;
    return s_fake_verify_result;
}

static void test_verify_seam(void)
{
    TEST_SECTION("lcd_auth_state -- verify-fn seam");

    TEST_CHECK(lcd_auth_default_verify("1234", 4) == LCD_PIN_ROLE_NONE,
               "default verify fails closed (no credential module wired in yet)");

    lcd_auth_state_set_verify_fn(fake_verify_fn);
    s_fake_verify_result = LCD_PIN_ROLE_ADMIN;

    lcd_keypad_state_t ks;
    lcd_keypad_state_init(&ks);
    lcd_pin_entry_push_digit(&ks.entry, '1');
    lcd_pin_entry_push_digit(&ks.entry, '2');
    lcd_pin_entry_push_digit(&ks.entry, '3');
    lcd_pin_entry_push_digit(&ks.entry, '4');

    lcd_pin_role_t role = LCD_PIN_ROLE_NONE;
    lcd_keypad_submit_result_t r = lcd_keypad_state_submit(&ks, 1000, &role);
    TEST_CHECK(r == LCD_KEYPAD_SUBMIT_GRANTED, "installed verify fn's ADMIN result is honoured");
    TEST_CHECK(role == LCD_PIN_ROLE_ADMIN, "granted role matches what the seam returned");
    TEST_CHECK(strcmp(s_fake_verify_last_digits, "1234") == 0, "verify fn was called with the entered digits");
    TEST_CHECK(s_fake_verify_last_len == 4, "verify fn was called with the correct length");

    lcd_auth_state_set_verify_fn(NULL);
    TEST_CHECK(lcd_auth_default_verify("1234", 4) == LCD_PIN_ROLE_NONE,
               "passing NULL restores the fail-closed default");
}

static void test_keypad_submit_outcomes(void)
{
    TEST_SECTION("lcd_keypad_state_submit -- four outcomes + lockout escalation");

    lcd_auth_state_set_verify_fn(fake_verify_fn);

    // TOO_SHORT
    {
        lcd_keypad_state_t ks;
        lcd_keypad_state_init(&ks);
        lcd_pin_entry_push_digit(&ks.entry, '1');
        lcd_pin_role_t role = LCD_PIN_ROLE_ADMIN; // pre-poison, must be reset to NONE
        lcd_keypad_submit_result_t r = lcd_keypad_state_submit(&ks, 0, &role);
        TEST_CHECK(r == LCD_KEYPAD_SUBMIT_TOO_SHORT, "1 digit -- too short to submit");
        TEST_CHECK(role == LCD_PIN_ROLE_NONE, "out_role reset to NONE on a non-granted result");
    }

    // DENIED, then lockout escalation via the shared login_backoff primitive.
    // login_backoff_record_failure() locks immediately on the FIRST failure
    // (unlike the old ota_auth threshold scheme) -- each subsequent attempt
    // here is timestamped past the PRIOR step's lock duration so it actually
    // reaches the verify seam and gets a fresh DENIED, mirroring
    // test_login_backoff.c's test_ladder_escalation().
    {
        lcd_keypad_state_t ks;
        lcd_keypad_state_init(&ks);
        s_fake_verify_result = LCD_PIN_ROLE_NONE;

        uint32_t now = 0;
        for (uint32_t step = 0; step < LOGIN_BACKOFF_LADDER_LEN; step++) {
            lcd_pin_entry_reset(&ks.entry);
            lcd_pin_entry_push_digit(&ks.entry, '0');
            lcd_pin_entry_push_digit(&ks.entry, '0');
            lcd_pin_entry_push_digit(&ks.entry, '0');
            lcd_pin_entry_push_digit(&ks.entry, '0');
            lcd_pin_role_t role;
            lcd_keypad_submit_result_t r = lcd_keypad_state_submit(&ks, now, &role);
            TEST_CHECK(r == LCD_KEYPAD_SUBMIT_DENIED, "wrong PIN, past the prior lock window -- DENIED");
            now += LOGIN_BACKOFF_LADDER_MS[step]; // advance past this failure's own lock
        }

        // Still within the lock window opened by the 5th (last-step) failure --
        // even a correct PIN must be refused.
        lcd_pin_entry_reset(&ks.entry);
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_role_t role;
        lcd_keypad_submit_result_t r = lcd_keypad_state_submit(&ks, now - 1, &role);
        TEST_CHECK(r == LCD_KEYPAD_SUBMIT_LOCKED_OUT,
                   "still inside the active lock window -- LOCKED_OUT via the shared login_backoff primitive");
    }

    // GRANTED
    {
        lcd_keypad_state_t ks;
        lcd_keypad_state_init(&ks);
        s_fake_verify_result = LCD_PIN_ROLE_USER;
        lcd_pin_entry_push_digit(&ks.entry, '1');
        lcd_pin_entry_push_digit(&ks.entry, '2');
        lcd_pin_entry_push_digit(&ks.entry, '3');
        lcd_pin_entry_push_digit(&ks.entry, '4');
        lcd_pin_role_t role;
        lcd_keypad_submit_result_t r = lcd_keypad_state_submit(&ks, 0, &role);
        TEST_CHECK(r == LCD_KEYPAD_SUBMIT_GRANTED, "correct PIN -- GRANTED");
        TEST_CHECK(role == LCD_PIN_ROLE_USER, "granted role matches verify fn's return");

        // A subsequent correct submit still succeeds -- success clears the
        // lockout's failure_count rather than only halting its escalation.
        lcd_pin_entry_reset(&ks.entry);
        lcd_pin_entry_push_digit(&ks.entry, '1');
        lcd_pin_entry_push_digit(&ks.entry, '2');
        lcd_pin_entry_push_digit(&ks.entry, '3');
        lcd_pin_entry_push_digit(&ks.entry, '4');
        r = lcd_keypad_state_submit(&ks, 1, &role);
        TEST_CHECK(r == LCD_KEYPAD_SUBMIT_GRANTED, "success does not leave a stale lockout state behind");
    }

    // LOCKED_OUT short-circuits even a correct PIN while the lockout window holds.
    // A single failure already opens a lock window (login_backoff.h's ladder
    // step 0 = 5000ms) -- no need to walk the whole ladder to prove this.
    {
        lcd_keypad_state_t ks;
        lcd_keypad_state_init(&ks);
        s_fake_verify_result = LCD_PIN_ROLE_NONE;
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_entry_push_digit(&ks.entry, '0');
        lcd_pin_role_t role;
        lcd_keypad_state_submit(&ks, 0, &role); // one failure, opens the 5000ms window

        s_fake_verify_result = LCD_PIN_ROLE_ADMIN; // now "enter" the correct PIN
        lcd_pin_entry_reset(&ks.entry);
        lcd_pin_entry_push_digit(&ks.entry, '1');
        lcd_pin_entry_push_digit(&ks.entry, '2');
        lcd_pin_entry_push_digit(&ks.entry, '3');
        lcd_pin_entry_push_digit(&ks.entry, '4');
        lcd_keypad_submit_result_t r = lcd_keypad_state_submit(&ks, LOGIN_BACKOFF_LADDER_MS[0] - 1, &role);
        TEST_CHECK(r == LCD_KEYPAD_SUBMIT_LOCKED_OUT,
                   "a correct PIN entered during an active lockout window is still refused");
    }

    lcd_auth_state_set_verify_fn(NULL);
}

static void test_lock_state_boundaries(void)
{
    TEST_SECTION("lcd_lock_state -- inactivity lock + 10s prompt boundaries");

    lcd_lock_state_t ls;
    lcd_lock_state_init(&ls, 60, 0);
    TEST_CHECK(lcd_lock_is_locked(&ls), "freshly initialised session starts locked");

    lcd_lock_grant(&ls, LCD_PIN_ROLE_USER, 0);
    TEST_CHECK(!lcd_lock_is_locked(&ls), "granting unlocks");

    // Prompt opens exactly at timeout - 10s.
    TEST_CHECK(lcd_lock_tick(&ls, 49999) == LCD_LOCK_TICK_OK, "1ms before the prompt window -- still plain OK");
    TEST_CHECK(lcd_lock_tick(&ls, 50000) == LCD_LOCK_TICK_PROMPT, "exactly at timeout-10s -- prompt opens");
    TEST_CHECK(ls.prompt_open, "prompt_open flag set once the prompt window is entered");

    // Valid right up to timeout, expires the ms after.
    TEST_CHECK(lcd_lock_tick(&ls, 59999) == LCD_LOCK_TICK_PROMPT, "1ms before timeout -- still just prompting");
    TEST_CHECK(!lcd_lock_is_locked(&ls), "session still granted at timeout-1ms");
    TEST_CHECK(lcd_lock_tick(&ls, 60000) == LCD_LOCK_TICK_EXPIRED, "exactly at timeout -- expires");
    TEST_CHECK(lcd_lock_is_locked(&ls), "session locked once expired");

    // Re-grant, confirm accepting extends by the FULL timeout, not just to the boundary.
    lcd_lock_grant(&ls, LCD_PIN_ROLE_USER, 100000);
    TEST_CHECK(lcd_lock_tick(&ls, 100000 + 50000) == LCD_LOCK_TICK_PROMPT, "prompt opens again at the new grant+50s");
    lcd_lock_note_activity(&ls, 100000 + 50000); // "Stay Unlocked" accepted
    TEST_CHECK(!ls.prompt_open, "accepting closes the prompt");
    TEST_CHECK(lcd_lock_tick(&ls, 100000 + 50000 + 49999) == LCD_LOCK_TICK_OK,
               "accepting re-armed the FULL 60s window from the acceptance moment -- still outside the new "
               "10s prompt window, not merely extended to the old boundary (which would already be inside it)");
    TEST_CHECK(lcd_lock_tick(&ls, 100000 + 50000 + 60000) == LCD_LOCK_TICK_EXPIRED,
               "expires 60s after the acceptance moment, not 60s after the original grant");

    // A keepalive-equivalent action while genuinely locked must NOT extend anything.
    lcd_lock_force_lock(&ls);
    TEST_CHECK(lcd_lock_is_locked(&ls), "force_lock locks immediately");
    uint32_t before = ls.last_activity_ms;
    lcd_lock_note_activity(&ls, 999999);
    TEST_CHECK(ls.last_activity_ms == before, "note_activity while locked is a no-op -- does not silently re-grant");
    TEST_CHECK(lcd_lock_is_locked(&ls), "still locked after a no-op activity note");

    // The "never" sentinel.
    lcd_lock_state_t never;
    lcd_lock_state_init(&never, LCD_LOCK_TIMEOUT_NEVER, 0);
    lcd_lock_grant(&never, LCD_PIN_ROLE_ADMIN, 0);
    TEST_CHECK(lcd_lock_tick(&never, 1000000000) == LCD_LOCK_TICK_OK,
               "LCD_LOCK_TIMEOUT_NEVER never expires and never prompts, regardless of elapsed time");
    TEST_CHECK(!lcd_lock_is_locked(&never), "never-timeout session stays granted");

    // Saturating-subtraction edge case: a timeout shorter than the 10s prompt window.
    lcd_lock_state_t short_to;
    lcd_lock_state_init(&short_to, 5, 0); // 5s timeout, prompt window is 10s
    lcd_lock_grant(&short_to, LCD_PIN_ROLE_USER, 0);
    TEST_CHECK(lcd_lock_tick(&short_to, 0) == LCD_LOCK_TICK_PROMPT,
               "timeout shorter than the prompt window -- prompts immediately upon grant rather than "
               "underflowing prompt_at_ms below zero");
    TEST_CHECK(lcd_lock_tick(&short_to, 5000) == LCD_LOCK_TICK_EXPIRED, "still expires at its own real timeout");
}

static void test_lock_has_role_ordering(void)
{
    TEST_SECTION("lcd_pin_role_t ordering (used by ui_lcd_lock.c's has_role/run_gated)");

    TEST_CHECK(LCD_PIN_ROLE_NONE < LCD_PIN_ROLE_USER, "NONE orders below USER");
    TEST_CHECK(LCD_PIN_ROLE_USER < LCD_PIN_ROLE_ADMIN, "USER orders below ADMIN");
}

void run_test_lcd_auth_state(void)
{
    test_pin_entry();
    test_verify_seam();
    test_keypad_submit_outcomes();
    test_lock_state_boundaries();
    test_lock_has_role_ordering();
}
