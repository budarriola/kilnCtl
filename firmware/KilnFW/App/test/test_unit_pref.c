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
static void up_mount_scratch(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
}

/* Stages the value+rev a LEGACY (pre dual-write-close) firmware left in NVS:
 * unit_pref.c no longer has any NVS writer, so legacy-board tests write it. */
static void up_stage_legacy_nvs(uint8_t raw, uint32_t rev)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "stage: open the legacy namespace");
    hal_kv_set_u8(&h, NVS_KEY_UNIT_PREF, raw);
    hal_kv_set_u32(&h, NVS_KEY_UNIT_PREF_REV, rev);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

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
    up_mount_scratch();
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
    cfg_fs_deinit();
}

static void test_set_without_cfg_partition_fails_loud(void)
{
    TEST_SECTION("unit_pref_set: no cfg partition mounted -- the save fails loud, nothing falls back to NVS");
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();

    TEST_CHECK(unit_pref_set(UNIT_PREF_FAHRENHEIT) != ESP_OK, "set() reports the failed cfg write");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "no NVS write was made as a fallback");
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
    up_stage_legacy_nvs(0xAA, 1); // a legacy NVS copy holding an out-of-range byte, no cfg file

    simulate_reboot();
    unit_pref_start();
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS,
               "an out-of-range stored byte must NOT be trusted -- falls back to the safe default");
}

// ---------------------------------------------------------------------
// cfg_fs dual-write coverage (docs/FILESYSTEM_USER_DATA_PLAN.md section 5
// step 3). Everything above already proves the partition-absent path.
// ---------------------------------------------------------------------

static void test_save_lands_in_cfg_file_only(void)
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
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "NVS was never written -- the dual-write window is closed");
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
    up_stage_legacy_nvs((uint8_t)UNIT_PREF_FAHRENHEIT, 1); // legacy NVS-only board, cfg_fs not mounted yet

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

static void test_dualwrite_status_reports_divergence(void)
{
    TEST_SECTION("unit_pref_get_dualwrite_status: GET /api/cfgfs's per-item row -- healthy (equal content) "
                 "reports diverged:false, and a real content disagreement between file and NVS reports "
                 "diverged:true, without performing any resync write as a side effect (a status read must "
                 "be safe to call repeatedly)");
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    /* Healthy: both sides agree (same value, same rev). */
    uint8_t celsius = (uint8_t)UNIT_PREF_CELSIUS;
    TEST_CHECK(pref_cfg_fs_save(UNIT_PREF_FILE_PATH, &celsius, sizeof(celsius), 3) == ESP_OK, "file write");
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_u8(&h, NVS_KEY_UNIT_PREF, celsius);
    hal_kv_set_u32(&h, NVS_KEY_UNIT_PREF_REV, 3);
    hal_kv_commit(&h);
    hal_kv_close(&h);

    bool file_valid = false, nvs_valid = false, diverged = true /* poison */;
    uint32_t file_rev = 0, nvs_rev = 0;
    unit_pref_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && nvs_valid && file_rev == 3 && nvs_rev == 3, "both sides reported, revs match");
    TEST_CHECK(!diverged, "equal content -- NOT diverged");

    /* Now make them genuinely disagree: NVS moves to Fahrenheit at rev 4,
     * the file is left at Celsius/rev 3 -- exactly the "prior file write
     * failed" shape every bridge's own resolve() would flag. */
    uint8_t fahrenheit = (uint8_t)UNIT_PREF_FAHRENHEIT;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_u8(&h, NVS_KEY_UNIT_PREF, fahrenheit);
    hal_kv_set_u32(&h, NVS_KEY_UNIT_PREF_REV, 4);
    hal_kv_commit(&h);
    hal_kv_close(&h);

    unit_pref_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && nvs_valid && file_rev == 3 && nvs_rev == 4, "both sides still reported, revs now differ");
    /* NEGATIVE TARGET (see this task's report for the deliberate-break/
     * restore proof): if unit_pref_get_dualwrite_status() ever stopped
     * comparing real content and instead only compared revs, or always
     * returned diverged=false, this is the check that would catch it. */
    TEST_CHECK(diverged, "file (Celsius) and NVS (Fahrenheit) genuinely disagree -- DIVERGED");

    /* The status read must not have resynced anything -- re-reading the
     * file directly must still show the ORIGINAL Celsius/rev 3 content. */
    uint8_t file_raw_check = 0xFF;
    uint32_t rev_check = 0;
    bool valid_check = false;
    pref_cfg_fs_load_raw(UNIT_PREF_FILE_PATH, sizeof(file_raw_check), unit_pref_validate, &file_raw_check,
                          &rev_check, &valid_check);
    TEST_CHECK(valid_check && file_raw_check == celsius && rev_check == 3,
               "a status read performs NO resync write -- the file is untouched by the divergence check");

    cfg_fs_deinit();
}

static void test_mount_failed_falls_through_to_nvs_only(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();
    up_stage_legacy_nvs((uint8_t)UNIT_PREF_FAHRENHEIT, 1);

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    simulate_reboot();
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err == ESP_OK, "start() with a FAILED mount still succeeds (non-fatal, falls back to NVS)");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_FAHRENHEIT, "mount-failed: the NVS value is still adopted correctly");

    cfg_fs_deinit();
}

// ---------------------------------------------------------------------
// Startup-fault return coverage (a548dfc6): unit_pref_start() now returns an
// error on a partition-init failure or an unrecovered hal_kv open/read error
// so main_network_http.c can latch a startup fault. Each case also proves the
// safe default is still applied (simulate_reboot() first poisons the RAM
// value, so a start() that bailed without resetting would show).
// ---------------------------------------------------------------------

/* Occupy every fake_kv partition slot with unrelated names so
 * hal_kv_init_partition(KILN_NVS_PARTITION) genuinely fails with HAL_NO_MEM --
 * a real hal_kv_init_partition failure, no injection knob needed. */
static void up_exhaust_partition_slots(void)
{
    static const char *const filler[] = { "fill_a", "fill_b", "fill_c", "fill_d" };
    for (unsigned i = 0; i < sizeof(filler) / sizeof(filler[0]); i++) {
        hal_kv_init_partition(filler[i]);
    }
}

static void test_start_partition_init_failure_returns_error_and_defaults(void)
{
    TEST_SECTION("unit_pref_start: NVS partition init failure returns non-OK, Celsius default still applied");
    up_cfg_fs_reset();
    fake_kv_reset_all();
    up_exhaust_partition_slots();
    TEST_CHECK(hal_kv_init_partition(KILN_NVS_PARTITION) != HAL_OK,
               "precondition: the NVS partition genuinely cannot be initialised");
    simulate_reboot();

    esp_err_t err = unit_pref_start();
    TEST_CHECK(err != ESP_OK, "partition-init failure is reported (not swallowed as ESP_OK)");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS, "partition-init failure: the Celsius default is still applied");
}

static void test_start_open_error_without_file_returns_error(void)
{
    TEST_SECTION("unit_pref_start: hal_kv_open error with no file fallback returns non-OK, default applied");
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    TEST_CHECK(fake_kv_script_next_open_status(NVS_NAMESPACE, HAL_IO), "precondition: open failure armed");
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err != ESP_OK, "open error with nothing recoverable is reported");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS, "open error: the Celsius default is still applied");
}

static void test_start_read_error_without_file_returns_error(void)
{
    TEST_SECTION("unit_pref_start: hal_kv read error (corrupt committed key) with no file fallback returns non-OK");
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    unit_pref_start();
    up_stage_legacy_nvs((uint8_t)UNIT_PREF_FAHRENHEIT, 1); // precondition: Fahrenheit in the legacy NVS copy
    TEST_CHECK(fake_kv_script_corrupt_key(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_KEY_UNIT_PREF),
               "precondition: the committed value is corrupted (reads return HAL_IO)");

    simulate_reboot();
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err != ESP_OK, "read error with nothing recoverable is reported");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_CELSIUS, "read error: the Celsius default is still applied");
}

static void test_start_nvs_error_but_file_fallback_returns_ok_with_file_value(void)
{
    TEST_SECTION("unit_pref_start: hal_kv open error but a valid cfg file supplies the value -- ESP_OK, file value applied");
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    uint8_t fahrenheit = (uint8_t)UNIT_PREF_FAHRENHEIT;
    TEST_CHECK(pref_cfg_fs_save(UNIT_PREF_FILE_PATH, &fahrenheit, sizeof(fahrenheit), 2) == ESP_OK,
               "precondition: the file holds Fahrenheit at rev 2");
    simulate_reboot();
    s_unit_pref = UNIT_PREF_CELSIUS; // opposite of the expected result so the check below is not vacuous

    TEST_CHECK(fake_kv_script_next_open_status(NVS_NAMESPACE, HAL_IO), "precondition: open failure armed");
    esp_err_t err = unit_pref_start();
    TEST_CHECK(err == ESP_OK, "file fallback recovered a value: start() reports success despite the NVS error");
    TEST_CHECK(unit_pref_get() == UNIT_PREF_FAHRENHEIT, "the file value (Fahrenheit) is applied");
    TEST_CHECK(s_unit_pref_rev == 2, "the file's rev is adopted");

    cfg_fs_deinit();
}

static pref_cfg_fs_write_fn_t g_up_orig_write_fn;
static int g_up_depth_at_write = -1;
static esp_err_t up_depth_probe_write(const char *rel_path, const void *data, size_t len)
{
    g_up_depth_at_write = g_test_stub_lock_depth;
    return g_up_orig_write_fn(rel_path, data, len);
}

/* Same-rev race guard: the rev read, the commit and the rev bump are one section under
 * the save lock (unit_pref_set runs on httpd, the LCD task and the UART bridge task). */
static void test_save_holds_lock_across_rev_read_and_commit(void)
{
    up_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(UP_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    unit_pref_start();
    int base = g_test_stub_lock_depth;

    g_up_orig_write_fn = pref_cfg_fs_get_write_fn();
    pref_cfg_fs_set_write_fn(up_depth_probe_write);
    uint32_t r0 = s_unit_pref_rev;
    TEST_CHECK(unit_pref_set(UNIT_PREF_FAHRENHEIT) == ESP_OK, "first save ok");
    TEST_CHECK(g_up_depth_at_write == base + 1, "save lock is held while the file is written");
    TEST_CHECK(g_test_stub_lock_depth == base, "save lock released after the save");
    TEST_CHECK(unit_pref_set(UNIT_PREF_CELSIUS) == ESP_OK, "second save ok");
    TEST_CHECK(s_unit_pref_rev == r0 + 2, "two saves get distinct, consecutive revs");
    pref_cfg_fs_set_write_fn(g_up_orig_write_fn);
    cfg_fs_deinit();
}

void run_test_unit_pref(void)
{
    TEST_SECTION("unit_pref");
    test_default_is_celsius_on_empty_nvs();
    test_persistence_round_trip();
    test_set_refuses_invalid_value();
    test_corrupted_value_falls_back_to_safe_default();
    test_set_without_cfg_partition_fails_loud();
    test_save_lands_in_cfg_file_only();
    test_nvs_fallback_when_file_absent_then_migrates();
    test_divergence_tie_break_higher_rev_wins();
    test_dualwrite_status_reports_divergence();
    test_mount_failed_falls_through_to_nvs_only();
    test_save_holds_lock_across_rev_read_and_commit();

    test_start_partition_init_failure_returns_error_and_defaults();
    test_start_open_error_without_file_returns_error();
    test_start_read_error_without_file_returns_error();
    test_start_nvs_error_but_file_fallback_returns_ok_with_file_value();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
