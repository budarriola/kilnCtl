// Host tests for App/drivers/persist/display_power_cfg.c -- persisted brightness/
// idle-timeout/keep-on-while-firing/display-on-error settings, owner request
// 2026-09-04. #includes display_power_cfg.c directly (same convention as
// test_ramp_assist_cfg.c/test_unit_pref-shaped modules) to reach its
// s_brightness_percent/etc file-scope state for simulate_reboot() below.
//
// THE LOAD-BEARING PROPERTY THIS FILE EXISTS TO PROVE: every failure path
// (empty NVS, a load failure, a wrong-size/wrong-version blob, an
// out-of-range field inside an otherwise well-formed blob) resolves to the
// SAFE defaults documented in display_power_cfg.h -- never a partially
// trusted or garbage value.
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "../drivers/persist/display_power_cfg.c"

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (the RAM-cached settings) while leaving the stubbed NVS blob (the
// flash stand-in) exactly as it was.
// ---------------------------------------------------------------------------
static void simulate_reboot(void)
{
    // Deliberately the WRONG values -- proves display_power_cfg_start()
    // actually overwrites them rather than the test happening to already
    // hold the expected result before start() runs. f028e2f flipped both
    // switches' empty-NVS default to true, so "wrong" for them is now false
    // (it used to be true, back when the default was false).
    s_brightness_percent = 3;
    s_timeout_setting = DISPLAY_TIMEOUT_1_MIN;
    s_keep_on_while_firing = false;
    s_display_on_error = false;
}

static void test_defaults_on_empty_nvs(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    esp_err_t err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "display_power_cfg_start() succeeds against an empty stub");
    TEST_CHECK(display_power_cfg_brightness_percent() == 100, "empty NVS: brightness defaults to 100%");
    TEST_CHECK(display_power_cfg_timeout_setting() == DISPLAY_TIMEOUT_NEVER, "empty NVS: timeout defaults to Never");
    // f028e2f (owner decision, 2026-09-04): both switches now default ON.
    TEST_CHECK(display_power_cfg_keep_on_while_firing() == true, "empty NVS: keep-on-while-firing defaults to true");
    TEST_CHECK(display_power_cfg_display_on_error() == true, "empty NVS: display-on-error defaults to true");
}

static void test_persistence_round_trip(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();

    esp_err_t err = display_power_cfg_set(42, DISPLAY_TIMEOUT_10_MIN, true, true);
    TEST_CHECK(err == ESP_OK, "display_power_cfg_set() with valid values succeeds");
    TEST_CHECK(display_power_cfg_brightness_percent() == 42, "in-RAM value updates immediately: brightness");
    TEST_CHECK(display_power_cfg_timeout_setting() == DISPLAY_TIMEOUT_10_MIN, "in-RAM value updates immediately: timeout");
    TEST_CHECK(display_power_cfg_keep_on_while_firing() == true, "in-RAM value updates immediately: keep-on-while-firing");
    TEST_CHECK(display_power_cfg_display_on_error() == true, "in-RAM value updates immediately: display-on-error");

    simulate_reboot();
    err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "reload after simulated reboot succeeds");
    TEST_CHECK(display_power_cfg_brightness_percent() == 42, "reload: brightness survives the round trip");
    TEST_CHECK(display_power_cfg_timeout_setting() == DISPLAY_TIMEOUT_10_MIN, "reload: timeout survives the round trip");
    TEST_CHECK(display_power_cfg_keep_on_while_firing() == true, "reload: keep-on-while-firing survives the round trip");
    TEST_CHECK(display_power_cfg_display_on_error() == true, "reload: display-on-error survives the round trip");
}

static void test_set_refuses_out_of_range_brightness(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();
    uint8_t before = display_power_cfg_brightness_percent();

    esp_err_t err = display_power_cfg_set(101, DISPLAY_TIMEOUT_5_MIN, false, false);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "brightness_percent=101 (>100) is refused with ESP_ERR_INVALID_ARG");
    TEST_CHECK(display_power_cfg_brightness_percent() == before,
               "refused set: in-RAM brightness is left completely untouched (never clamped)");
}

static void test_set_refuses_invalid_timeout_setting(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();
    display_timeout_setting_t before = display_power_cfg_timeout_setting();

    esp_err_t err = display_power_cfg_set(50, (display_timeout_setting_t)77, true, false);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "timeout_setting=77 (out of range) is refused with ESP_ERR_INVALID_ARG");
    TEST_CHECK(display_power_cfg_timeout_setting() == before,
               "refused set: in-RAM timeout_setting is left completely untouched");
}

static void test_corrupt_blob_size_falls_back_to_defaults(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start(); // establishes defaults + opens the namespace once

    // Directly stash a wrong-size blob under the same key, bypassing
    // display_power_cfg_set() entirely -- simulates a blob written by an
    // incompatible future/older schema.
    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    TEST_CHECK(open_err == HAL_OK, "test setup: hal_kv_open for the corrupt-blob stash succeeds");
    uint8_t wrong_size_blob[2] = { 1, 2 };
    hal_status_t set_err = hal_kv_set_blob(&h, NVS_KEY_DISPLAY_POWER, wrong_size_blob, sizeof(wrong_size_blob));
    TEST_CHECK(set_err == HAL_OK, "test setup: stashing the wrong-size blob succeeds");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    simulate_reboot();
    esp_err_t err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() against a wrong-size blob still returns ESP_OK (non-fatal)");
    TEST_CHECK(display_power_cfg_brightness_percent() == 100, "wrong-size blob: falls back to default brightness");
    TEST_CHECK(display_power_cfg_timeout_setting() == DISPLAY_TIMEOUT_NEVER, "wrong-size blob: falls back to default timeout");
}

static void test_out_of_range_field_in_wellformed_blob_falls_back_to_defaults(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();

    // A blob that is the RIGHT size and version but carries a corrupted
    // field (brightness_percent > 100) -- proves the field-level range
    // check actually fires, not just the size/version check.
    display_power_cfg_blob_t bad_blob;
    memset(&bad_blob, 0, sizeof(bad_blob));
    bad_blob.version = DISPLAY_POWER_CFG_VERSION;
    bad_blob.brightness_percent = 250; // corrupted
    bad_blob.timeout_setting = (uint8_t)DISPLAY_TIMEOUT_5_MIN;
    bad_blob.keep_on_while_firing = 0;
    bad_blob.display_on_error = 0;

    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_DISPLAY_POWER, &bad_blob, sizeof(bad_blob));
    hal_kv_commit(&h);
    hal_kv_close(&h);

    simulate_reboot();
    esp_err_t err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() against an out-of-range field still returns ESP_OK (non-fatal)");
    TEST_CHECK(display_power_cfg_brightness_percent() == 100,
               "brightness_percent=250 in an otherwise well-formed blob is refused, falls back to default");
}

void run_test_display_power_cfg(void)
{
    test_defaults_on_empty_nvs();
    test_persistence_round_trip();
    test_set_refuses_out_of_range_brightness();
    test_set_refuses_invalid_timeout_setting();
    test_corrupt_blob_size_falls_back_to_defaults();
    test_out_of_range_field_in_wellformed_blob_falls_back_to_defaults();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
