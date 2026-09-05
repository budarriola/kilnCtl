// Host tests for App/drivers/backlight_pwm.c -- DISPLAY_ST7796_PLAN.md
// Phase 7 / 3.4.1 backlight PWM driver.
//
// Only backlight_duty_percent_for_state() is under test here: it is pure
// integer logic with no ESP-IDF dependency (see backlight_pwm.h's header
// comment on the pure/hardware split, same convention as boot_button.c's
// boot_button_step()/state_refuses_bypass()). backlight_pwm_init()/_start()
// are also exercised, but only their disabled-build (#else) branch --
// CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE is off in the host build (not defined
// by stubs/sdkconfig.h, same as every other default-off KILNCTL_* flag
// there), so backlight_pwm.c compiles that branch here: init/start simply
// return ESP_ERR_NOT_SUPPORTED without touching anything, which the last
// test below confirms directly.
#include "test_common.h"

#include "../drivers/backlight_pwm.c"

// ---------------------------------------------------------------------------
// backlight_duty_percent_for_state() -- pure on/idle percent mapping.
// ---------------------------------------------------------------------------

static void test_on_state_uses_on_percent(void)
{
    TEST_CHECK(backlight_duty_percent_for_state(true, 100, 0) == 100,
               "screen on, on_percent=100: duty is 100");
    TEST_CHECK(backlight_duty_percent_for_state(true, 75, 20) == 75,
               "screen on, on_percent=75: duty is 75 (idle_percent ignored)");
}

static void test_idle_state_uses_idle_percent(void)
{
    TEST_CHECK(backlight_duty_percent_for_state(false, 100, 0) == 0,
               "screen off, idle_percent=0: duty is 0 (fully off)");
    TEST_CHECK(backlight_duty_percent_for_state(false, 75, 20) == 20,
               "screen off, idle_percent=20: duty is 20 (on_percent ignored)");
}

static void test_percent_clamped_to_100(void)
{
    // Kconfig's `range 0 100` already prevents an out-of-range percent in
    // practice; this proves the belt-and-suspenders clamp inside
    // backlight_duty_percent_for_state() itself actually fires rather than
    // being dead code -- negative-tested below by temporarily removing it.
    TEST_CHECK(backlight_duty_percent_for_state(true, 250, 0) == 100,
               "on_percent > 100 clamps to 100");
    TEST_CHECK(backlight_duty_percent_for_state(false, 0, 200) == 100,
               "idle_percent > 100 clamps to 100");
}

// ---------------------------------------------------------------------------
// backlight_pwm_init()/_start() with CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE off
// (the host build's compiled branch, and the shipped default).
// ---------------------------------------------------------------------------

static void test_disabled_build_init_and_start_report_not_supported(void)
{
    // init() must not crash on a NULL query_fn/query_ctx here even though it
    // stores them -- the disabled branch never calls/dereferences either --
    // and both calls must report the "no flying wire fitted" outcome rather
    // than pretending to succeed.
    backlight_pwm_t bl;
    esp_err_t init_err = backlight_pwm_init(&bl, NULL, NULL);
    TEST_CHECK(init_err == ESP_ERR_NOT_SUPPORTED, "disabled build: init() reports ESP_ERR_NOT_SUPPORTED");
    TEST_CHECK(!bl.ready, "disabled build: init() leaves ready == false");

    esp_err_t start_err = backlight_pwm_start(&bl);
    TEST_CHECK(start_err == ESP_ERR_NOT_SUPPORTED, "disabled build: start() reports ESP_ERR_NOT_SUPPORTED");
}

void run_test_backlight_pwm(void)
{
    test_on_state_uses_on_percent();
    test_idle_state_uses_idle_percent();
    test_percent_clamped_to_100();
    test_disabled_build_init_and_start_report_not_supported();
}
