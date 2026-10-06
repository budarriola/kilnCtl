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
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TDP_MKDIR(p) _mkdir(p)
#define TDP_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TDP_MKDIR(p) mkdir((p), 0755)
#define TDP_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

#include "../drivers/persist/display_power_cfg.c"

static const char *DP_SCRATCH_BASE = "cfg_fs_test_display_power";

static void dp_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", DP_SCRATCH_BASE, DISPLAY_POWER_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", DP_SCRATCH_BASE, DISPLAY_POWER_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", DP_SCRATCH_BASE);
    TDP_RMDIR(tmp);
    TDP_RMDIR(DP_SCRATCH_BASE);
    TDP_MKDIR(DP_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (the RAM-cached settings) while leaving the stubbed NVS blob (the
// flash stand-in) exactly as it was.
// ---------------------------------------------------------------------------
static void dp_mount_scratch(void)
{
    dp_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(DP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
}

/* Stages the blob+rev a LEGACY (pre dual-write-close) firmware left in NVS:
 * display_power_cfg.c no longer has any NVS writer. */
static void dp_stage_legacy_nvs(uint8_t brightness, uint8_t timeout, uint8_t keep_on, uint8_t on_error,
                                uint32_t rev)
{
    display_power_cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = DISPLAY_POWER_CFG_VERSION;
    blob.brightness_percent = brightness;
    blob.timeout_setting = timeout;
    blob.keep_on_while_firing = keep_on;
    blob.display_on_error = on_error;
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "stage: open the legacy namespace");
    hal_kv_set_blob(&h, NVS_KEY_DISPLAY_POWER, &blob, sizeof(blob));
    hal_kv_set_u32(&h, NVS_KEY_DISPLAY_POWER_REV, rev);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

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
    dp_mount_scratch();
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
    cfg_fs_deinit();
}

static void test_set_without_cfg_partition_fails_loud(void)
{
    TEST_SECTION("display_power_cfg_set: no cfg partition mounted -- fails loud, nothing falls back to NVS");
    dp_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();

    TEST_CHECK(display_power_cfg_set(42, DISPLAY_TIMEOUT_10_MIN, true, true) != ESP_OK,
               "set() reports the failed cfg write");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "no NVS write was made as a fallback");
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

// ---------------------------------------------------------------------
// cfg_fs dual-write coverage (docs/FILESYSTEM_USER_DATA_PLAN.md section 5
// step 3). Everything above already proves the partition-absent path
// (cfg_fs is never mounted there) -- this exercises the file mounted.
// ---------------------------------------------------------------------

static void test_dual_write_lands_on_both_file_and_nvs(void)
{
    dp_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(DP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    simulate_reboot();
    display_power_cfg_start();

    TEST_CHECK(display_power_cfg_set(55, DISPLAY_TIMEOUT_5_MIN, false, true) == ESP_OK,
               "set() succeeds with cfg_fs mounted");

    display_power_cfg_blob_t file_blob;
    memset(&file_blob, 0, sizeof(file_blob));
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(DISPLAY_POWER_FILE_PATH, sizeof(file_blob), display_power_validate, &file_blob, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_blob.brightness_percent == 55 && file_blob.timeout_setting == DISPLAY_TIMEOUT_5_MIN,
               "the file was written and decodes to the values just set");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "NVS was never written -- the dual-write window is closed");
    TEST_CHECK(file_rev == s_display_power_rev, "file rev matches the in-RAM rev this save just bumped to");

    cfg_fs_deinit();
}

static void test_nvs_fallback_when_file_absent_then_migrates(void)
{
    dp_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();
    dp_stage_legacy_nvs(80, (uint8_t)DISPLAY_TIMEOUT_10_MIN, 1, 0, 1); // legacy NVS-only board, cfg_fs not mounted yet

    TEST_CHECK(cfg_fs_init(DP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts on a later boot");
    simulate_reboot();
    esp_err_t err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds");
    TEST_CHECK(display_power_cfg_brightness_percent() == 80, "NVS fallback: value came from NVS, no file existed yet");

    bool exists = false;
    cfg_fs_exists(DISPLAY_POWER_FILE_PATH, &exists);
    TEST_CHECK(exists, "the NVS candidate was opportunistically migrated out to the file on this load");

    cfg_fs_deinit();
}

static void test_divergence_tie_break_higher_rev_wins(void)
{
    dp_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(DP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    display_power_cfg_start();
    display_power_cfg_set(20, DISPLAY_TIMEOUT_NEVER, true, true); // file+NVS both rev 1

    // Simulate a prior file write that failed after NVS already advanced --
    // bump ONLY NVS's blob+rev directly, behind the file's back.
    display_power_cfg_blob_t newer;
    memset(&newer, 0, sizeof(newer));
    newer.version = DISPLAY_POWER_CFG_VERSION;
    newer.brightness_percent = 99;
    newer.timeout_setting = (uint8_t)DISPLAY_TIMEOUT_1_MIN;
    newer.keep_on_while_firing = 0;
    newer.display_on_error = 0;
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_DISPLAY_POWER, &newer, sizeof(newer));
    hal_kv_set_u32(&h, NVS_KEY_DISPLAY_POWER_REV, 9);
    hal_kv_commit(&h);
    hal_kv_close(&h);

    simulate_reboot();
    esp_err_t err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds across the diverged sides");
    TEST_CHECK(display_power_cfg_brightness_percent() == 99,
               "higher rev (NVS, rev 9) wins over the lower-rev file (rev 1)");

    display_power_cfg_blob_t file_blob;
    memset(&file_blob, 0, sizeof(file_blob));
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(DISPLAY_POWER_FILE_PATH, sizeof(file_blob), display_power_validate, &file_blob, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_blob.brightness_percent == 99 && file_rev == 9,
               "the file was resynced from the winning NVS side");

    cfg_fs_deinit();
}

static void test_mount_failed_falls_through_to_nvs_only(void)
{
    dp_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    display_power_cfg_start();
    dp_stage_legacy_nvs(33, (uint8_t)DISPLAY_TIMEOUT_1_MIN, 0, 0, 1);

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    simulate_reboot();
    esp_err_t err = display_power_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() with a FAILED mount still succeeds (non-fatal, falls back to NVS)");
    TEST_CHECK(display_power_cfg_brightness_percent() == 33, "mount-failed: the NVS value is still adopted correctly");

    cfg_fs_deinit();
}

void run_test_display_power_cfg(void)
{
    test_defaults_on_empty_nvs();
    test_persistence_round_trip();
    test_set_without_cfg_partition_fails_loud();
    test_set_refuses_out_of_range_brightness();
    test_set_refuses_invalid_timeout_setting();
    test_corrupt_blob_size_falls_back_to_defaults();
    test_out_of_range_field_in_wellformed_blob_falls_back_to_defaults();
    test_dual_write_lands_on_both_file_and_nvs();
    test_nvs_fallback_when_file_absent_then_migrates();
    test_divergence_tie_break_higher_rev_wins();
    test_mount_failed_falls_through_to_nvs_only();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
