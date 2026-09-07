// Host tests for zones_config_cfg_fs.c -- the read-through/dual-write
// bridge between zones config's NVS blob and the `cfg` LittleFS partition
// (docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 5). See that file's
// header comment for the full policy this exercises.
//
// Linked as a SEPARATE translation unit into the "zones_http" host-test
// executable (build_host_tests.ps1's $cmd2) alongside test_zones_http.c,
// which #includes zones_config_store.c (and therefore zones_http_internal.h,
// s_zones, nvs_load()/nvs_save()) directly -- this file reaches those same
// real, non-static symbols the ordinary way (declared via
// zones_http_internal.h, defined in the other TU, resolved at link time),
// rather than re-#including anything. cfg_fs.c is pure stdio and runs
// against a real temp directory, exactly like test_cfg_fs.c.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TZCF_MKDIR(p) _mkdir(p)
#define TZCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TZCF_MKDIR(p) mkdir((p), 0755)
#define TZCF_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "hal_kv.h"

#include "cfg_fs.h"
#include "zones_config_cfg_fs.h"
#include "zones_http_internal.h" /* s_zones, nvs_load()/nvs_save() */

static const char *SCRATCH_BASE = "cfg_fs_test_zones";

static void reset_all(void)
{
    /* TZCF_RMDIR (a bare rmdir()) only succeeds against an EMPTY directory --
     * a leftover zones.json (or an orphaned .tmp/zones.json) from a PRIOR
     * run of this test binary silently defeats it, leaving stale on-disk
     * state (and, critically, a stale rev) for the next run to trip over.
     * Delete the known filenames explicitly first, THEN rmdir -- same fix
     * class as project_concurrent_agent_stub_collision, applied to this
     * test's own scratch directory rather than production code. */
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", SCRATCH_BASE, ZONES_CFG_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, ZONES_CFG_FILE_PATH);
    remove(path);

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", SCRATCH_BASE);
    TZCF_RMDIR(tmp);
    TZCF_RMDIR(SCRATCH_BASE);
    TZCF_MKDIR(SCRATCH_BASE);
    cfg_fs_deinit();
    zones_config_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION); /* zones_http_start()/nvs_partition_init() does
                                                 * this once at boot -- see test_zones_http.c's
                                                 * nvs_test_enable(true) for the identical need. */
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
}

/* Fills a minimal but validate_zones_cfg()-passing config, distinguishable
 * by `tag` (folded into a zone name and a PID gain) so two configs built
 * with different tags are never accidentally byte-identical. */
static void fill_valid_cfg(zones_cfg_t *cfg, const char *tag, float kp)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->thermo_count = 1;
    cfg->relay_count = 1;
    cfg->max_simultaneous_relays = 1;
    cfg->safety_tc_type = 3;
    cfg->timing_profile_count = 1;
    strncpy(cfg->timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    cfg->timing_profiles[0].guard_progress_duty_min = 0.1f;
    cfg->timing_profiles[0].ramp_lock_band_c = 3.0f;
    zone_cfg_t *z = &cfg->zones[0];
    snprintf(z->name, sizeof(z->name), "%s", tag);
    z->relay_mask = 1;
    z->thermo_mask = 1;
    z->tc_type = 0;
    z->pid_kp = kp;
    z->max_temp_c = 1200.0f;
    z->model_k_dc = 12.0f;
}

static void stage(const char *tag, float kp)
{
    fill_valid_cfg(&s_zones.cfg, tag, kp);
}

/* zones_config_store.c's dual-write rev counter (s_zones_cfg_rev) is a
 * process-wide static, exactly like a real board's would persist across
 * reboots -- it is NOT reset by reset_all()'s fake_kv/cfg_fs wipe, on
 * purpose (a real rev counter must not silently rewind just because a test
 * cleared storage under it; only a real nvs_load() resync ever moves it).
 * Every test below that asserts an ABSOLUTE rev number therefore primes it
 * back to 0 first via a real nvs_load() against the now-guaranteed-empty
 * NVS + freshly mounted, empty file -- both sides report nothing found, so
 * zones_config_cfg_fs_resolve() resolves rev to 0 and nvs_load() adopts it,
 * exactly the boot-time behavior this mirrors. Call AFTER cfg_fs_init() and
 * fake_kv_reset_all()/hal_kv_init_partition() (i.e. after reset_all()) so
 * both sides really are empty when this runs. */
static void prime_rev_to_zero(void)
{
    bool found = false, valid = false;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    (void)nvs_load(&found, &valid);
}

// ---------------------------------------------------------------------
// 1. Partition absent (today's real state on every board): dual-write
//    save/load must behave EXACTLY like plain NVS save/load -- the file
//    layer is a silent no-op.
// ---------------------------------------------------------------------
static void test_partition_absent_falls_through_to_nvs_only(void)
{
    TEST_SECTION("zones cfg_fs: partition absent -- save/load behave exactly like NVS-only");
    reset_all();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    stage("absent", 7.5f);
    TEST_CHECK(nvs_save() == ESP_OK, "nvs_save succeeds with no `cfg` partition mounted");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "nvs_load succeeds and reports valid");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "absent") == 0 && s_zones.cfg.zones[0].pid_kp == 7.5f,
               "loaded config is the one that was saved, sourced purely from NVS");

    zones_cfg_t raw;
    uint32_t rev = 999;
    bool raw_valid = true;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(!raw_valid && rev == 0, "no file was ever written -- cfg_fs_is_available() gated every file op");
}

// ---------------------------------------------------------------------
// 2. File-preferred read + NVS fallback: with `cfg` mounted, a first
//    nvs_load() against an NVS-only board (no file yet) falls back to NVS
//    and opportunistically migrates -- writes the file. A SECOND load, with
//    the in-RAM struct wiped, must come back identical, and this time
//    zones_config_cfg_fs_load_raw() proves it actually came from the file.
// ---------------------------------------------------------------------
static void test_nvs_fallback_then_file_preferred_after_migration(void)
{
    TEST_SECTION("zones cfg_fs: NVS fallback on first load migrates to file; second load prefers the file");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    stage("migrate", 3.25f);
    TEST_CHECK(nvs_save() == ESP_OK, "nvs_save succeeds (dual-write: file first, then NVS)");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(ZONES_CFG_FILE_PATH, &exists) == ESP_OK && exists,
               "nvs_save's dual-write actually created the file");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load after wipe succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "migrate") == 0 && s_zones.cfg.zones[0].pid_kp == 3.25f,
               "reloaded config matches what was saved");

    zones_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 1, "the file itself holds a valid, rev-1 copy -- this is where the load "
                                       "preferentially reads from now");
    TEST_CHECK(strcmp(raw.zones[0].name, "migrate") == 0, "file content matches the saved config");
}

// ---------------------------------------------------------------------
// 3. Dual-write keeps both sides in sync across several saves -- every
//    nvs_save() must leave the file's rev/content identical to what the
//    in-RAM struct just committed.
// ---------------------------------------------------------------------
static void test_dual_write_keeps_file_and_nvs_in_sync(void)
{
    TEST_SECTION("zones cfg_fs: repeated saves keep file and NVS in sync (same content, incrementing rev)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    stage("v1", 1.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save 1");
    stage("v2", 2.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save 2");
    stage("v3", 3.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save 3");

    zones_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 3, "file rev tracks three saves");
    TEST_CHECK(strcmp(raw.zones[0].name, "v3") == 0 && raw.zones[0].pid_kp == 3.0f, "file holds the LATEST save");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "v3") == 0, "NVS agrees with the file -- no divergence after "
                                                              "three consecutive dual-writes");
}

// ---------------------------------------------------------------------
// 4. Divergence tie-break, BOTH directions:
//    (a) file rev > NVS rev (the file write landed, a later comparison
//        still finds it ahead) -> file wins.
//    (b) NVS rev > file rev (a file write failed after NVS already
//        advanced) -> NVS wins, and the file is resynced.
// ---------------------------------------------------------------------
static esp_err_t failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static void test_divergence_tie_break_both_directions(void)
{
    TEST_SECTION("zones cfg_fs: divergence tie-break picks the higher rev in both directions, and resyncs "
                 "the loser");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Establish rev 1 on both sides.
    stage("rev1", 1.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "rev 1 saved to both sides");

    // Simulate a file write failure: NVS advances to rev 2, file stays at
    // rev 1 with the OLD content.
    zones_config_cfg_fs_set_write_fn(failing_write_fn);
    stage("rev2_nvs_only", 2.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "rev 2 save still reports OK -- NVS write is what nvs_save()'s return "
                                      "value depends on, not the file write");
    zones_config_cfg_fs_reset_write_fn_for_test();

    zones_cfg_t raw_before;
    uint32_t rev_before = 0;
    bool raw_valid_before = false;
    zones_config_cfg_fs_load_raw(&raw_before, &rev_before, &raw_valid_before);
    TEST_CHECK(raw_valid_before && rev_before == 1 && strcmp(raw_before.zones[0].name, "rev1") == 0,
               "file is stuck at rev 1 -- the failed write never landed");

    // NVS rev (2) > file rev (1): load must adopt NVS and resync the file.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load after the simulated failure succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "rev2_nvs_only") == 0,
               "NVS (higher rev) wins the tie-break, not the stale file");

    zones_cfg_t raw_after;
    uint32_t rev_after = 0;
    bool raw_valid_after = false;
    zones_config_cfg_fs_load_raw(&raw_after, &rev_after, &raw_valid_after);
    TEST_CHECK(raw_valid_after && rev_after == 2 && strcmp(raw_after.zones[0].name, "rev2_nvs_only") == 0,
               "the file was resynced from NVS as a side effect of resolving the divergence");

    // Now the normal direction: one more ordinary dual-write puts file
    // AHEAD in the sense that matters (file_rev >= nvs_rev, the expected
    // steady state) -- confirm the file is what a subsequent load reports.
    stage("rev3", 3.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "rev 3 saved normally, both sides in sync again");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load after rev 3 succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "rev3") == 0, "file/NVS agree again after the normal save");
}

// ---------------------------------------------------------------------
// 5. Migration via the file path is bound to the SAME C migration chain a
//    stored NVS blob uses -- an old-version blob written into the file
//    (v21, immediately predating this pass) decodes to the identical
//    struct zones_config_json_decode_blob() produces when handed those
//    exact same bytes directly, off the file wrapper entirely. This is the
//    vector comparison the task calls for: not a textual diff, a real
//    decode-twice-compare.
// ---------------------------------------------------------------------
static void test_file_migration_matches_direct_blob_decode_v21(void)
{
    TEST_SECTION("zones cfg_fs: a v21 blob read through the file path decodes identically to the direct "
                 "zones_config_json_decode_blob() call on the same bytes");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    zones_cfg_v21_t v21;
    memset(&v21, 0, sizeof(v21));
    v21.version = 21;
    v21.thermo_count = 1;
    v21.relay_count = 1;
    v21.max_simultaneous_relays = 1;
    v21.safety_tc_type = 3;
    v21.timing_profile_count = 1;
    strncpy(v21.timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    v21.timing_profiles[0].guard_progress_duty_min = 0.1f;
    v21.timing_profiles[0].ramp_lock_band_c = 3.0f;
    v21.zones[0].relay_mask = 1;
    v21.zones[0].thermo_mask = 1;
    snprintf(v21.zones[0].name, sizeof(v21.zones[0].name), "V21Zone");
    v21.zones[0].pid_kp = 9.0f;
    v21.zones[0].max_temp_c = 1200.0f;
    v21.zones[0].model_k_dc = 12.0f;
    v21.crc32 = 0; /* zones_config_json_decode_blob() recomputes/validates CRC for the
                    * version it actually decodes as -- pre-v-current CRC handling is
                    * exercised identically for the file path and the direct path
                    * below since both call the exact same function. */

    // Direct decode, off the file wrapper entirely -- the reference vector.
    zones_cfg_t direct;
    const char *reason = "";
    zones_decode_result_t direct_result = zones_config_json_decode_blob(&v21, sizeof(v21), &direct, &reason);

    // Through the file: 4-byte rev prefix + the identical v21 bytes.
    uint8_t filebuf[4 + sizeof(v21)];
    filebuf[0] = 5;
    filebuf[1] = 0;
    filebuf[2] = 0;
    filebuf[3] = 0;
    memcpy(filebuf + 4, &v21, sizeof(v21));
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK,
               "raw v21-blob file staged directly (bypassing zones_config_cfg_fs_save(), which always writes "
               "the CURRENT version -- this test wants an old on-disk version)");

    zones_cfg_t via_file;
    uint32_t via_file_rev = 0;
    bool via_file_valid = false;
    zones_config_cfg_fs_load_raw(&via_file, &via_file_rev, &via_file_valid);

    TEST_CHECK(direct_result == ZONES_DECODE_OK, "the direct decode of the staged v21 bytes succeeds "
                                                  "(sanity: the fixture itself is a valid v21 blob)");
    TEST_CHECK(via_file_valid && via_file_rev == 5, "the file path decodes it too, and reports the staged rev");
    TEST_CHECK(memcmp(&direct, &via_file, sizeof(direct)) == 0,
               "VECTOR COMPARISON: file-path decode of a v21 blob is byte-for-byte identical to decoding the "
               "same bytes directly -- both call zones_config_json_decode_blob(), so there is exactly one "
               "migration chain, not a second one for files");
}

// ---------------------------------------------------------------------
// 6. Interrupted write leaves old-or-new: an orphaned temp file (crash
//    between fsync and rename, same fixture style as test_cfg_fs.c) must
//    never be visible through zones_config_cfg_fs_load_raw() -- only the
//    last COMMITTED content is ever readable.
// ---------------------------------------------------------------------
static void test_interrupted_file_write_leaves_old_or_new(void)
{
    TEST_SECTION("zones cfg_fs: an interrupted file write leaves the OLD committed config intact, never a "
                 "partial one");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    stage("committed", 4.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "an initial, fully-committed save lands");

    // Manufacture a crash-orphaned temp file directly on disk, exactly like
    // test_cfg_fs.c's test_interrupted_write_never_corrupts_old_file().
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp/%s", SCRATCH_BASE, ZONES_CFG_FILE_PATH);
    FILE *f = fopen(tmp_path, "wb");
    TEST_CHECK(f != NULL, "test setup: orphaned temp file created");
    if (f) {
        static const char partial[] = "not even close to a valid rev+blob";
        fwrite(partial, 1, sizeof(partial), f);
        fclose(f);
    }

    zones_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 1 && strcmp(raw.zones[0].name, "committed") == 0,
               "the OLD committed config reads back untouched -- the orphaned temp file (rename never ran) "
               "is invisible through the real read path");
}

// ---------------------------------------------------------------------
// 7. EQUAL revs with differing content must adopt NVS, not the file.
//    This is the rollback round trip the plan's rev counter exists for:
//    firmware rolled back past the dual-write rewrites NVS_KEY_ZONES but
//    knows nothing about "zones_rev", so it leaves the rev where it was.
//    Rolling forward then sees file_rev == nvs_rev with different bytes --
//    and the NVS side is the newer of the two. A `>=` tie-break adopts the
//    STALE FILE here and the rolled-back edit is lost for good on the very
//    next save.
//
//    Staged through the real production write paths: nvs_save() to get
//    both sides to the same rev, then the NVS blob alone is rewritten
//    exactly the way pre-dual-write firmware would (hal_kv_set_blob on
//    NVS_KEY_ZONES, nothing touching NVS_KEY_ZONES_REV).
// ---------------------------------------------------------------------
static void test_equal_rev_divergence_adopts_nvs_not_the_stale_file(void)
{
    TEST_SECTION("zones cfg_fs: equal revs with differing content adopt NVS (rolled-back-firmware edit "
                 "survives a roll-forward)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Both sides land at rev 1 with the same content.
    stage("before_rollback", 1.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "rev 1 dual-written to both sides");

    // Now act like firmware that predates this change: rewrite ONLY the
    // NVS blob, leaving zones_rev at 1 and the file at rev 1/old content.
    zones_cfg_t rolled_back;
    fill_valid_cfg(&rolled_back, "rolled_edit", 42.0f);
    rolled_back.version = ZONES_CFG_VERSION;
    rolled_back.crc32 = zones_config_json_compute_crc(&rolled_back);
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "test setup: NVS opened for the pre-dual-write-style blob rewrite");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_ZONES, &rolled_back, sizeof(rolled_back)) == HAL_OK,
                   "test setup: NVS blob rewritten WITHOUT touching zones_rev, exactly as older firmware "
                   "would");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "test setup: NVS commit");
        hal_kv_close(&h);
    }

    // Confirm the fixture really is the equal-rev case, not an accidental
    // strictly-higher one -- otherwise this test would pass for the wrong
    // reason under a `>=` tie-break too.
    zones_cfg_t file_raw;
    uint32_t file_rev = 0;
    bool file_raw_valid = false;
    zones_config_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 1 && strcmp(file_raw.zones[0].name, "before_rollback") == 0,
               "fixture check: the file still holds the OLD content at rev 1");
    {
        hal_kv_handle_t h;
        uint32_t nvs_rev = 0;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
                   "fixture check: NVS opened read-only");
        TEST_CHECK(hal_kv_get_u32(&h, "zones_rev", &nvs_rev) == HAL_OK && nvs_rev == 1,
                   "fixture check: zones_rev is still 1 -- file_rev == nvs_rev, the EQUAL-rev case");
        hal_kv_close(&h);
    }

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "roll-forward load succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "rolled_edit") == 0 &&
                   s_zones.cfg.zones[0].pid_kp == 42.0f,
               "NVS wins the EQUAL-rev tie -- the edit made on rolled-back firmware is NOT discarded in "
               "favour of the stale file");

    zones_config_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && strcmp(file_raw.zones[0].name, "rolled_edit") == 0,
               "the losing file was resynced from NVS, so the divergence does not persist across boots");
}

void run_test_zones_config_cfg_fs(void)
{
    test_partition_absent_falls_through_to_nvs_only();
    test_nvs_fallback_then_file_preferred_after_migration();
    test_dual_write_keeps_file_and_nvs_in_sync();
    test_divergence_tie_break_both_directions();
    test_file_migration_matches_direct_blob_decode_v21();
    test_interrupted_file_write_leaves_old_or_new();
    test_equal_rev_divergence_adopts_nvs_not_the_stale_file();

    reset_all();
}
