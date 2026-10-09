// Host tests for App/drivers/control/ramp_assist_cfg.c -- the kiln-wide (not
// per-zone) persisted on/off flag for the forthcoming "ramp assist" feature.
// #includes ramp_assist_cfg.c directly (same convention as
// test_watchdog_cfg.c/test_unit_pref-shaped modules) to reach its
// s_ramp_assist_enabled file-scope state for simulate_reboot() below.
//
// THE LOAD-BEARING PROPERTY THIS FILE EXISTS TO PROVE: the persisted default
// is FALSE (disabled) on every path -- empty NVS, a load failure, and a
// corrupt/out-of-range stored byte all resolve to disabled, never the other
// way. That is what makes it safe for a PID tuning run or an A/B controller
// comparison to trust "the board has never touched this setting" as "ramp
// assist is off" (see ramp_assist_cfg.h's header comment for why that matters
// -- an unassisted run silently becoming assisted invalidates every tracking-
// error measurement it produces).
//
// Uses fake_kv.h's RAM-backed hal_kv fake to simulate actual persistence
// across simulated reboots (HW_ABSTRACTION.md Phase 3 item 3, the
// nvs.h -> hal_kv.h migration; this file previously used stubs/nvs.h's
// opt-in "real" u8-key store, same as test_watchdog_cfg.c's original form).
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TRA_MKDIR(p) _mkdir(p)
#define TRA_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TRA_MKDIR(p) mkdir((p), 0755)
#define TRA_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

#include "../drivers/control/ramp_assist_cfg.c"

static const char *RA_SCRATCH_BASE = "cfg_fs_test_ramp_assist";

/* Same "delete known filenames, then rmdir" discipline
 * test_zones_config_cfg_fs.c's reset_all() documents -- a bare rmdir() only
 * succeeds against an EMPTY directory, and a leftover ramp_assist.dat (or an
 * orphaned .tmp/ramp_assist.dat) from a PRIOR run of this test binary would
 * silently defeat that, stranding stale on-disk state for the next run. */
static void ra_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", RA_SCRATCH_BASE, RAMP_ASSIST_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", RA_SCRATCH_BASE, RAMP_ASSIST_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", RA_SCRATCH_BASE);
    TRA_RMDIR(tmp);
    TRA_RMDIR(RA_SCRATCH_BASE);
    TRA_MKDIR(RA_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (s_ramp_assist_enabled's RAM state) while leaving the stubbed NVS
// blob (the flash stand-in) exactly as it was.
// ---------------------------------------------------------------------------
static void ra_mount_scratch(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
}

/* Stages the value+rev a LEGACY (pre dual-write-close) firmware left in NVS:
 * ramp_assist_cfg.c no longer has any NVS writer. */
static void ra_stage_legacy_nvs(uint8_t raw, uint32_t rev)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "stage: open the legacy namespace");
    hal_kv_set_u8(&h, NVS_KEY_RAMP_ASSIST, raw);
    hal_kv_set_u32(&h, NVS_KEY_RAMP_ASSIST_REV, rev);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void simulate_reboot(void)
{
    s_ramp_assist_enabled = true; // deliberately the WRONG value -- proves ramp_assist_cfg_start()
                                   // actually overwrites it rather than the test happening to already
                                   // hold the expected result before start() runs.
}

static void test_default_is_disabled_on_empty_nvs(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "ramp_assist_cfg_start() succeeds against an empty stub");
    TEST_CHECK(ramp_assist_cfg_enabled() == false,
               "empty NVS (nothing ever stored, e.g. a board that predates this feature or a fresh "
               "board) -- the safe default is DISABLED, i.e. ramp_assist_cfg_enabled() must be false. "
               "This is the load-bearing default: a PID tuning run must be able to trust an "
               "unconfigured board to run raw, unassisted");
}

static void test_persistence_round_trip(void)
{
    ra_mount_scratch();
    simulate_reboot();

    ramp_assist_cfg_start();
    TEST_CHECK(!ramp_assist_cfg_enabled(), "precondition: starts disabled (safe default)");

    esp_err_t err = ramp_assist_cfg_set_enabled(true);
    TEST_CHECK(err == ESP_OK, "set_enabled(true) persists successfully in the stub");
    TEST_CHECK(ramp_assist_cfg_enabled(), "immediately reflects the new value, this boot");

    // Simulate a reboot -- the persisted value must survive it. This is the
    // scenario item 1 in the task ("survives a whole-page save round trip AND
    // a reboot") is actually asking to be proven, not merely "the setter
    // returned ESP_OK".
    simulate_reboot();
    ramp_assist_cfg_start();
    TEST_CHECK(ramp_assist_cfg_enabled(),
               "ramp_assist_cfg_start() on the next boot reloads enabled=true from NVS");

    // Flip it back and confirm THAT round-trips too, not just "true" sticking
    // (a setter that only ever writes 1 and never actually clears the byte
    // would pass the check above and still be broken).
    err = ramp_assist_cfg_set_enabled(false);
    TEST_CHECK(err == ESP_OK, "set_enabled(false) persists successfully");
    simulate_reboot();
    ramp_assist_cfg_start();
    TEST_CHECK(!ramp_assist_cfg_enabled(),
               "the re-disabled value also survives a simulated reboot, not just the enabled one");
    cfg_fs_deinit();
}

static void test_set_without_cfg_partition_fails_loud(void)
{
    TEST_SECTION("ramp_assist_cfg_set_enabled: no cfg partition mounted -- fails loud, nothing falls back to NVS");
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    ramp_assist_cfg_start();

    TEST_CHECK(ramp_assist_cfg_set_enabled(true) != ESP_OK, "set_enabled() reports the failed cfg write");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "no NVS write was made as a fallback");
}

static void test_corrupted_value_falls_back_to_safe_default(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    ramp_assist_cfg_start();

    // Corrupt the persisted byte directly to something out of range for this
    // module's 0/1 encoding -- simulates a bit flip that survives the u8
    // storage layer without also being caught at that layer (unlike
    // watchdog_cfg.c's CRC'd record, this module has no CRC of its own, so
    // its own explicit range check (raw != 0 && raw != 1) is the ENTIRE
    // defense here -- this test is what proves that check actually does
    // something rather than being dead code). Written through the real
    // hal_kv_set_u8()/hal_kv_commit() round trip, same as
    // test_boot_guard.c's/test_watchdog_cfg.c's blob-corruption approach.
    // A legacy NVS copy holding an out-of-range byte (no cfg file).
    ra_stage_legacy_nvs(0xAA, 1);

    simulate_reboot();
    ramp_assist_cfg_start();
    TEST_CHECK(!ramp_assist_cfg_enabled(),
               "an out-of-range stored byte must NOT be trusted to still mean enabled=true -- this is "
               "exactly the failure this module's explicit range check exists to catch: a corrupted "
               "value must resolve to the SAFE default (disabled), not to whatever garbage the flash "
               "actually held");
}

// ---------------------------------------------------------------------
// cfg_fs dual-write coverage (docs/FILESYSTEM_USER_DATA.md section 5
// step 3, "Migrate prefs"). Everything above this point already proves the
// partition-absent path (cfg_fs is never mounted in those tests, so
// pref_cfg_fs_resolve() is always a pass-through to the NVS candidate) --
// what follows exercises the file mounted and actually in play.
// ---------------------------------------------------------------------

static void test_save_lands_in_cfg_file_only(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    simulate_reboot();
    ramp_assist_cfg_start();

    TEST_CHECK(ramp_assist_cfg_set_enabled(true) == ESP_OK, "set_enabled(true) succeeds with cfg_fs mounted");

    uint8_t file_raw = 0;
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(RAMP_ASSIST_FILE_PATH, sizeof(file_raw), ramp_assist_validate, &file_raw, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_raw == 1, "the file was actually written and decodes to enabled=true");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "NVS was never written -- the dual-write window is closed");
    TEST_CHECK(file_rev == s_ramp_assist_rev, "file rev matches the in-RAM rev this save just bumped to");

    cfg_fs_deinit();
}

static void test_file_preferred_when_both_valid_and_equal(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    ramp_assist_cfg_start();
    ramp_assist_cfg_set_enabled(true);

    simulate_reboot();
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "reload with cfg_fs mounted succeeds");
    TEST_CHECK(ramp_assist_cfg_enabled(), "reload reads the dual-written value back (file path exercised)");

    cfg_fs_deinit();
}

static void test_nvs_fallback_when_file_absent(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    ramp_assist_cfg_start();
    // Write with cfg_fs NOT mounted (partition absent this boot) -- only NVS
    // gets the value, matching every board today.
    TEST_CHECK(cfg_fs_get_status() != CFG_FS_STATUS_MOUNTED, "precondition: cfg_fs is not mounted yet");
    ra_stage_legacy_nvs(1, 1); // legacy NVS-only board

    // NOW mount cfg_fs (simulates the partition becoming available on a
    // later boot / firmware update) and reboot -- the file is empty, so the
    // NVS candidate must be adopted, and this first load should also
    // migrate it out to the file (lazy one-item migration).
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts on this later boot");
    simulate_reboot();
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds");
    TEST_CHECK(ramp_assist_cfg_enabled(), "NVS fallback: value came from NVS since no file existed yet");

    bool exists = false;
    cfg_fs_exists(RAMP_ASSIST_FILE_PATH, &exists);
    TEST_CHECK(exists, "the NVS candidate was opportunistically migrated out to the file on this load");

    cfg_fs_deinit();
}

static void test_divergence_tie_break_higher_rev_wins(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    ramp_assist_cfg_start();
    ramp_assist_cfg_set_enabled(true); // file+NVS both rev 1, both true

    // Simulate "a prior file write failed and only NVS advanced": bump ONLY
    // NVS's rev+value directly, behind the file's back, exactly the
    // divergence pref_cfg_fs_resolve()'s tie-break exists to catch.
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_u8(&h, NVS_KEY_RAMP_ASSIST, 0);
    hal_kv_set_u32(&h, NVS_KEY_RAMP_ASSIST_REV, 5);
    hal_kv_commit(&h);
    hal_kv_close(&h);

    simulate_reboot();
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds across the diverged sides");
    TEST_CHECK(!ramp_assist_cfg_enabled(),
               "higher rev (NVS, rev 5) wins over the lower-rev file (rev 1) -- disabled adopted");

    // The file must have been resynced to the winning (NVS) value so the
    // divergence does not persist to the next boot.
    uint8_t file_raw = 1;
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(RAMP_ASSIST_FILE_PATH, sizeof(file_raw), ramp_assist_validate, &file_raw, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_raw == 0 && file_rev == 5, "the file was resynced from the winning NVS side");

    cfg_fs_deinit();
}

// docs/audits/filesystem_migration_review_2026-09-07.md section 2: the
// EQUAL-rev, DIFFERING-bytes case -- the one `file_rev >= nvs_rev` got
// wrong (adopted the stale file) and check_cfg_fs_tie_break.ps1 now guards
// against reintroducing. This is NOT the same scenario as
// test_divergence_tie_break_higher_rev_wins() above (that one has NVS
// STRICTLY ahead, rev 5 vs rev 1) -- an equal rev with differing bytes can
// only happen when something wrote the NVS blob directly without knowing
// about the rev counter, exactly what firmware from before this dual-write
// existed does on a rollback: it writes NVS_KEY_RAMP_ASSIST but never
// touches NVS_KEY_RAMP_ASSIST_REV, so rolling forward again finds
// file_rev == nvs_rev with the file now stale. The correct answer is
// unconditional: NVS is the newer side in this case, never the file.
static void test_equal_rev_divergence_adopts_nvs_not_the_stale_file(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    ramp_assist_cfg_start();
    ramp_assist_cfg_set_enabled(true); // file+NVS both rev 1, both true

    // Simulate "an edit made on rolled-back (pre-dual-write) firmware":
    // overwrite ONLY the NVS blob, leaving its rev untouched at 1 -- exactly
    // what a build that has never heard of NVS_KEY_RAMP_ASSIST_REV would do.
    // The file is left at rev 1/true, now stale relative to this edit.
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_u8(&h, NVS_KEY_RAMP_ASSIST, 0);
    hal_kv_set_u32(&h, NVS_KEY_RAMP_ASSIST_REV, 1); // legacy rev key: matches the file's rev, bytes differ
    hal_kv_commit(&h);
    hal_kv_close(&h);

    // Precondition: this really is the equal-rev, differing-bytes case, not
    // some other scenario -- confirmed directly against NVS before trusting
    // the outcome below.
    uint32_t precondition_rev = 0;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "precondition: NVS namespace opens");
    hal_kv_get_u32(&h, NVS_KEY_RAMP_ASSIST_REV, &precondition_rev);
    hal_kv_close(&h);
    TEST_CHECK(precondition_rev == 1, "precondition: NVS rev is still 1 (untouched by the rollback-style write)");

    simulate_reboot();
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() succeeds across the equal-rev divergence");
    TEST_CHECK(!ramp_assist_cfg_enabled(),
               "equal rev, differing bytes: NVS (false) wins over the now-stale file (true) -- "
               "the edit made on rolled-back firmware is NOT discarded");

    // The file must be resynced to the winning (NVS) value, same as the
    // strictly-higher-rev case above -- the divergence must not survive to
    // the next boot.
    uint8_t file_raw = 1;
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(RAMP_ASSIST_FILE_PATH, sizeof(file_raw), ramp_assist_validate, &file_raw, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_raw == 0 && file_rev == 1, "the file was resynced from the winning NVS side");

    cfg_fs_deinit();
}

static void test_mount_failed_falls_through_to_nvs_only(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    ramp_assist_cfg_start();
    ra_stage_legacy_nvs(1, 1);

    // A base_dir that cannot be listed at all -- cfg_fs_init() returns
    // ESP_FAIL / status UNAVAILABLE (cfg_fs.h's documented mount-failed
    // contract), never crashes, and every later cfg_fs_* call degrades to a
    // clean no-op.
    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    simulate_reboot();
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "start() with a FAILED mount still succeeds (non-fatal, falls back to NVS)");
    TEST_CHECK(ramp_assist_cfg_enabled(), "mount-failed: the NVS value is still adopted correctly");

    cfg_fs_deinit();
}

// MSVC/cl.exe (/std:c11) has no nested-function extension, so this stands
// alone at file scope -- installed only for the duration of one test below.
static esp_err_t ra_always_fail_write(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static void test_interrupted_write_leaves_old_file_intact(void)
{
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    simulate_reboot();
    ramp_assist_cfg_start();
    ramp_assist_cfg_set_enabled(true); // establishes a good, committed file at rev 1

    // Simulate an interrupted write by installing a write function that
    // always fails (stands in for a power cut between the temp-file write
    // and the rename -- cfg_fs_write_atomic()'s own contract already
    // guarantees the OLD file survives untouched in that case; this proves
    // ramp_assist_cfg.c's caller-side behavior when that failure is
    // reported back up).
    pref_cfg_fs_set_write_fn(ra_always_fail_write);

    esp_err_t err = ramp_assist_cfg_set_enabled(false);
    // The cfg file is the only store now, so a failed write is returned to
    // the caller (fail loud) and nothing falls back to NVS.
    TEST_CHECK(err != ESP_OK, "set_enabled() reports the failed cfg write");
    TEST_CHECK(ramp_assist_cfg_enabled() == false,
               "the in-RAM value still took effect this boot (documented in-RAM-first policy); only the "
               "persistence failed, and the caller was told");

    pref_cfg_fs_reset_write_fn_for_test();

    // The OLD file (rev 1, enabled=true) must still be exactly what is on
    // disk -- the failed write must not have corrupted or partially
    // overwritten it.
    uint8_t file_raw = 0xFF;
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(RAMP_ASSIST_FILE_PATH, sizeof(file_raw), ramp_assist_validate, &file_raw, &file_rev,
                          &file_valid);
    TEST_CHECK(file_valid && file_raw == 1 && file_rev == 1,
               "the interrupted write left the OLD file (rev 1, enabled=true) completely intact");

    cfg_fs_deinit();
}

// ---------------------------------------------------------------------
// Startup-fault return coverage (a548dfc6): ramp_assist_cfg_start() now returns an
// error on a partition-init failure or an unrecovered hal_kv open/read error
// so main_network_http.c can latch a startup fault. Each case also proves the
// safe default is still applied (simulate_reboot() first poisons the RAM
// value, so a start() that bailed without resetting would show).
// ---------------------------------------------------------------------

/* Occupy every fake_kv partition slot with unrelated names so
 * hal_kv_init_partition(KILN_NVS_PARTITION) genuinely fails with HAL_NO_MEM --
 * a real hal_kv_init_partition failure, no injection knob needed. */
static void ra_exhaust_partition_slots(void)
{
    static const char *const filler[] = { "fill_a", "fill_b", "fill_c", "fill_d" };
    for (unsigned i = 0; i < sizeof(filler) / sizeof(filler[0]); i++) {
        hal_kv_init_partition(filler[i]);
    }
}

static void test_ra_start_partition_init_failure_returns_error_and_defaults(void)
{
    TEST_SECTION("ramp_assist_cfg_start: NVS partition init failure returns non-OK, disabled default still applied");
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    ra_exhaust_partition_slots();
    TEST_CHECK(hal_kv_init_partition(KILN_NVS_PARTITION) != HAL_OK,
               "precondition: the NVS partition genuinely cannot be initialised");
    simulate_reboot();

    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err != ESP_OK, "partition-init failure is reported (not swallowed as ESP_OK)");
    TEST_CHECK(ramp_assist_cfg_enabled() == false, "partition-init failure: the disabled default is still applied");
}

static void test_ra_start_open_error_without_file_returns_error(void)
{
    TEST_SECTION("ramp_assist_cfg_start: hal_kv_open error with no file fallback returns non-OK, default applied");
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    TEST_CHECK(fake_kv_script_next_open_status(NVS_NAMESPACE, HAL_IO), "precondition: open failure armed");
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err != ESP_OK, "open error with nothing recoverable is reported");
    TEST_CHECK(ramp_assist_cfg_enabled() == false, "open error: the disabled default is still applied");
}

static void test_ra_start_read_error_without_file_returns_error(void)
{
    TEST_SECTION("ramp_assist_cfg_start: hal_kv read error (corrupt committed key) with no file fallback returns non-OK");
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    ramp_assist_cfg_start();
    ra_stage_legacy_nvs(1, 1); // precondition: enabled in the legacy NVS copy
    TEST_CHECK(fake_kv_script_corrupt_key(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_KEY_RAMP_ASSIST),
               "precondition: the committed value is corrupted (reads return HAL_IO)");

    simulate_reboot();
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err != ESP_OK, "read error with nothing recoverable is reported");
    TEST_CHECK(ramp_assist_cfg_enabled() == false, "read error: the disabled default is still applied");
}

static void test_ra_start_nvs_error_but_file_fallback_returns_ok_with_file_value(void)
{
    TEST_SECTION("ramp_assist_cfg_start: hal_kv open error but a valid cfg file supplies the value -- ESP_OK, file value applied");
    ra_cfg_fs_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(RA_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    uint8_t enabled_raw = 1;
    TEST_CHECK(pref_cfg_fs_save(RAMP_ASSIST_FILE_PATH, &enabled_raw, sizeof(enabled_raw), 2) == ESP_OK,
               "precondition: the file holds enabled at rev 2");
    simulate_reboot();
    s_ramp_assist_enabled = false; // opposite of the expected result so the check below is not vacuous

    TEST_CHECK(fake_kv_script_next_open_status(NVS_NAMESPACE, HAL_IO), "precondition: open failure armed");
    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "file fallback recovered a value: start() reports success despite the NVS error");
    TEST_CHECK(ramp_assist_cfg_enabled() == true, "the file value (enabled) is applied");
    TEST_CHECK(s_ramp_assist_rev == 2, "the file's rev is adopted");

    cfg_fs_deinit();
}

void run_test_ramp_assist_cfg(void)
{
    test_default_is_disabled_on_empty_nvs();
    test_persistence_round_trip();
    test_set_without_cfg_partition_fails_loud();
    test_corrupted_value_falls_back_to_safe_default();
    test_save_lands_in_cfg_file_only();
    test_file_preferred_when_both_valid_and_equal();
    test_nvs_fallback_when_file_absent();
    test_divergence_tie_break_higher_rev_wins();
    test_equal_rev_divergence_adopts_nvs_not_the_stale_file();
    test_mount_failed_falls_through_to_nvs_only();
    test_interrupted_write_leaves_old_file_intact();
    test_ra_start_partition_init_failure_returns_error_and_defaults();
    test_ra_start_open_error_without_file_returns_error();
    test_ra_start_read_error_without_file_returns_error();
    test_ra_start_nvs_error_but_file_fallback_returns_ok_with_file_value();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}
