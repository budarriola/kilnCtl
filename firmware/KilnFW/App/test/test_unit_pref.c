// Host tests for App/drivers/persist/unit_pref.c -- the board-wide C/F
// display-unit preference. #includes unit_pref.c directly (same convention
// as test_ramp_assist_cfg.c/test_display_power_cfg.c) to reach its
// s_unit_pref/s_unit_pref_rev file-scope state for simulate_reboot() below.
//
// No host test existed for this module before this pass (unit_pref.c had
// no test file at all -- grepped and confirmed absent from
// build_host_tests.ps1's $sources). Added alongside the docs/
// FILESYSTEM_USER_DATA_PLAN.md section 5 step 3 dual-write work so the
// module's pre-existing NVS behavior (default/round-trip/corrupt-value
// fallback) is proven, not just the new file-backed path this pass adds.
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TUP_MKDIR(p) _mkdir(p)
#define TUP_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TUP_MKDIR(p) mkdir((p), 0755)
#define TUP_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

#include "../drivers/persist/unit_pref.c"

static const char *UP_SCRATCH_BASE = "cfg_fs_test_unit_pref";

static void up_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", UP_SCRATCH_BASE, UNIT_PREF_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", UP_SCRATCH_BASE, UNIT_PREF_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", UP_SCRATCH_BASE);
    TUP_RMDIR(tmp);
    TUP_RMDIR(UP_SCRATCH_BASE);
    TUP_MKDIR(UP_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (RAM state) while leaving the stubbed NVS blob (the flash stand-in)
// exactly as it was.
// ---------------------------------------------------------------------------
static void simulate_reboot(void)
{
    s_unit_pref = UNIT_PREF_FAHRENHEIT; // deliberately the WRONG value -- proves unit_pref_start()
                                         // actually overwrites it rather than the test happening to
                                         // already hold the expected result before start() runs.
}

static void test_default_is_celsius_on_empty_nvs(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    esp_err_t err = unit_pref_start();
    TEST_CHECK(err == ESP_OK, "unit_pref_start() succeeds against an empty stub");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS, "empty NVS: the safe default is Celsius");
}

static void test_persistence_round_trip(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();

    TEST_CHECK(unit_pref_set(UNIT_PREF_FAHRENHEIT) == ESP_OK, "set(Fahrenheit) persists successfully");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_FAHRENHEIT, "in-RAM value updates immediately");

    simulate_reboot();
    unit_pref_start();
    TEST_CHECK(unit_pref_get() == UNIT_PREF_FAHRENHEIT, "the value survives a simulated reboot");

    TEST_CHECK(unit_pref_set(UNIT_PREF_CELSIUS) == ESP_OK, "set(Celsius) persists successfully");
    simulate_reboot();
    unit_pref_start();
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS, "flipping back also survives a reboot");
}

static void test_set_refuses_invalid_value(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();
    unit_pref_t before = unit_pref_get();

    esp_err_t err = unit_pref_set((unit_pref_t)77);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "an out-of-range enum value is refused");
    TEST_CHECK(unit_pref_get() == before, "refused set: in-RAM value untouched");
}

static void test_corrupted_value_falls_back_to_safe_default(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();
    unit_pref_set(UNIT_PREF_FAHRENHEIT);

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "precondition: the unit_pref namespace opens");
    TEST_CHECK(hal_kv_set_u8(&h, NVS_KEY_UNIT_PREF, 0xAA) == HAL_OK && hal_kv_commit(&h) == HAL_OK,
               "precondition: an out-of-range byte is written back");
    hal_kv_close(&h);

    simulate_reboot();
    unit_pref_start();
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS,
               "an out-of-range stored byte must NOT be trusted -- falls back to the safe default");
}

// ---------------------------------------------------------------------
// cfg_fs dual-write coverage (docs/FILESYSTEM_USER_DATA_PLAN.md section 5
// step 3). Everything above already proves the partition-absent path.
// ---------------------------------------------------------------------

static void test_dual_write_lands_on_both_file_and_nvs(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    simulate_reboot();
    unit_pref_start();

    TEST_CHECK(unit_pref_set(UNIT_PREF_FAHRENHEIT) == ESP_OK, "set() succeeds with cfg_fs mounted");

    uint8_t file_raw = 0;
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(UNIT_PREF_FILE_PATH, sizeof(file_raw), unit_pref_validate, &file_raw, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_raw == (uint8_t)UNIT_PREF_FAHRENHEIT, "the file decodes to Fahrenheit");

    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    uint8_t nvs_raw = 0;
    hal_kv_get_u8(&h, NVS_KEY_UNIT_PREF, &nvs_raw);
    hal_kv_close(&h);
    TEST_CHECK(nvs_raw == (uint8_t)UNIT_PREF_FAHRENHEIT, "NVS also holds Fahrenheit -- both sides written");
    TEST_CHECK(file_rev == s_unit_pref_rev, "file rev matches the in-RAM rev this save just bumped to");

    cfg_fs_deinit();
}

static void test_nvs_fallback_when_file_absent_then_migrates(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();
    unit_pref_set(UNIT_PREF_FAHRENHEIT); // NVS-only, cfg_fs not mounted yet

    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts on a later boot");
    simulate_reboot();
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_FAHRENHEIT, "NVS fallback: value came from NVS, no file existed yet");

    bool exists = false;
    cfg_fs_exists(UNIT_PREF_FILE_PATH, &exists);
    TEST_CHECK(exists, "the NVS candidate was opportunistically migrated out to the file on this load");

    cfg_fs_deinit();
}

static void test_divergence_tie_break_higher_rev_wins(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    unit_pref_start();
    unit_pref_set(UNIT_PREF_FAHRENHEIT); // file+NVS both rev 1

    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_u8(&h, NVS_KEY_UNIT_PREF, (uint8_t)UNIT_PREF_CELSIUS);
    hal_kv_set_u32(&h, NVS_KEY_UNIT_PREF_REV, 5);
    hal_kv_commit(&h);
    hal_kv_close(&h);

    simulate_reboot();
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds across the diverged sides");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS, "higher rev (NVS, rev 5) wins over the lower-rev file (rev 1)");

    uint8_t file_raw = (uint8_t)UNIT_PREF_FAHRENHEIT;
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(UNIT_PREF_FILE_PATH, sizeof(file_raw), unit_pref_validate, &file_raw, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_raw == (uint8_t)UNIT_PREF_CELSIUS && file_rev == 5,
               "the file was resynced from the winning NVS side");

    cfg_fs_deinit();
}

static void test_mount_failed_falls_through_to_nvs_only(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();
    unit_pref_set(UNIT_PREF_FAHRENHEIT);

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    simulate_reboot();
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err == ESP_OK, "start() with a FAILED mount still succeeds (non-fatal, falls back to NVS)");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_FAHRENHEIT, "mount-failed: the NVS value is still adopted correctly");

    cfg_fs_deinit();
}

void run_test_unit_pref(void)
{
    TEST_SECTION("unit_pref");
    test_default_is_celsius_on_empty_nvs();
    test_persistence_round_trip();
    test_set_refuses_invalid_value();
    test_corrupted_value_falls_back_to_safe_default();
    test_dual_write_lands_on_both_file_and_nvs();
    test_nvs_fallback_when_file_absent_then_migrates();
    test_divergence_tie_break_higher_rev_wins();
    test_mount_failed_falls_through_to_nvs_only();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
