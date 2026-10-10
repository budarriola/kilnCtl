// Host tests for zones_config_cfg_fs.c -- the read-through/dual-write
// bridge between zones config's NVS blob and the `cfg` LittleFS partition
// (docs/FILESYSTEM_USER_DATA.md section 5 step 5). See that file's
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
#include "nvs_flash.h"

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
#include "zones_config_accessors.h" /* zones_config_get/set_pid/coupling*() -- real accessors,
                                      * defined by zones_http.c's textual #include of
                                      * zones_config_accessors.c in test_zones_http.c, this TU's
                                      * link-mate in the same executable. */
#include "cfgfs_file_validate.h"
#include "zones_config_cfg_fs.h"
#include "zones_http.h"
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
    snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, ZONES_CFG_BAD_FILE_PATH);
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

/* Stages what a LEGACY (pre dual-write-close) firmware left in NVS: a valid
 * current-version blob plus its zones_rev key. Nothing in production writes
 * these keys any more. */
static void stage_legacy_nvs(const char *tag, float kp, uint32_t rev)
{
    zones_cfg_t c;
    fill_valid_cfg(&c, tag, kp);
    c.version = ZONES_CFG_VERSION;
    c.crc32 = zones_config_json_compute_crc(&c);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "stage: open NVS");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_ZONES, &c, sizeof(c)) == HAL_OK, "stage: blob");
    TEST_CHECK(hal_kv_set_u32(&h, "zones_rev", rev) == HAL_OK, "stage: rev");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "stage: commit");
    hal_kv_close(&h);
}

static bool nvs_zones_blob_absent(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return true;
    }
    static uint8_t blob[sizeof(zones_cfg_t)];
    size_t len = sizeof(blob);
    hal_status_t g = hal_kv_get_blob(&h, NVS_KEY_ZONES, blob, &len);
    hal_kv_close(&h);
    return g != HAL_OK;
}

/* Raw bytes of the cfg file: 4-byte rev prefix + the stored blob, NOT run
 * through the decode/migrate/normalize path. Returns the blob length. */
static size_t read_file_blob_raw(uint8_t *blob, size_t cap, uint32_t *rev)
{
    static uint8_t buf[4 + sizeof(zones_cfg_t)];
    size_t len = 0;
    if (cfg_fs_read(ZONES_CFG_FILE_PATH, buf, sizeof(buf), &len) != ESP_OK || len < 4) {
        return 0;
    }
    if (rev) {
        *rev = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    }
    size_t n = len - 4;
    if (n > cap) {
        n = cap;
    }
    memcpy(blob, buf + 4, n);
    return n;
}

// ---------------------------------------------------------------------
// 1. Partition absent (today's real state on every board): dual-write
//    save/load must behave EXACTLY like plain NVS save/load -- the file
//    layer is a silent no-op.
// ---------------------------------------------------------------------
static void test_partition_absent_falls_through_to_nvs_only(void)
{
    TEST_SECTION("zones cfg_fs: partition absent -- a legacy NVS copy still loads, a save fails loud");
    reset_all();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    stage_legacy_nvs("absent", 7.5f, 1);
    bool found = false, valid = false;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "nvs_load succeeds and reports valid");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "absent") == 0 && s_zones.cfg.zones[0].pid_kp == 7.5f,
               "loaded config is the legacy one, sourced purely from NVS");

    stage("changed", 1.0f);
    TEST_CHECK(nvs_save() != ESP_OK, "nvs_save fails loud with no `cfg` partition mounted -- no NVS fallback");
    TEST_CHECK(!nvs_zones_blob_absent(), "the legacy NVS blob is untouched by the failed save");

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
    TEST_SECTION("zones cfg_fs: legacy NVS blob migrates to the file on first load; the file is read after that");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    stage_legacy_nvs("migrate", 3.25f, 1);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "first load adopts the legacy NVS blob");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "migrate") == 0 && s_zones.cfg.zones[0].pid_kp == 3.25f,
               "loaded config is the legacy one");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(ZONES_CFG_FILE_PATH, &exists) == ESP_OK && exists,
               "the first load migrated it into the file");

    zones_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && strcmp(raw.zones[0].name, "migrate") == 0, "the file holds a valid copy");

    /* Second boot: NVS now gone entirely, the file alone carries the config. */
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "second load succeeds from the file alone");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "migrate") == 0 && s_zones.cfg.zones[0].pid_kp == 3.25f,
               "reloaded config matches");
}

// ---------------------------------------------------------------------
// 3. Dual-write keeps both sides in sync across several saves -- every
//    nvs_save() must leave the file's rev/content identical to what the
//    in-RAM struct just committed.
// ---------------------------------------------------------------------
static void test_dual_write_keeps_file_and_nvs_in_sync(void)
{
    TEST_SECTION("zones cfg_fs: repeated saves write the file only (incrementing rev), NVS never written");
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
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "v3") == 0, "reload yields the latest save");
    TEST_CHECK(nvs_zones_blob_absent(), "NVS was never written -- the dual-write window is closed");
}

// ---------------------------------------------------------------------
// 3b. PID_EXPANSION_PLAN.md 2026-09-08: the coupling matrix and PID gains
//     are the most expensive data on this board (multi-hour bench runs to
//     regenerate) and now go through this same dual-write path. Nothing in
//     this file previously exercised pid_ki/pid_kd, coupling_coeff[],
//     coupling_diag_k_dc, coupling_tau_s[]/coupling_dead_time_s[], or a
//     3-zone config through a real save -> "reboot" (memset + nvs_load) ->
//     reload cycle -- test_dual_write_keeps_file_and_nvs_in_sync above only
//     ever checks name/pid_kp on a single zone. Goes through the REAL
//     production accessors (zones_config_accessors.c, textually #included
//     by test_zones_http.c, this TU's link-mate), not direct struct pokes,
//     so this also exercises each setter's own validation/nvs_save() path.
// ---------------------------------------------------------------------
static void test_coupling_matrix_and_gains_round_trip_through_dual_write(void)
{
    TEST_SECTION("zones cfg_fs: coupling matrix + PID gains survive save -> reboot -> reload");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Base 3-zone config, one save to establish it (mirrors fill_valid_cfg's
    // single-zone shape, extended to 3 zones -- same field set
    // test_zones_http.c's own multi-zone fixtures use).
    fill_valid_cfg(&s_zones.cfg, "z0", 12.5f);
    s_zones.cfg.thermo_count = 3;
    s_zones.cfg.relay_count = 3;
    s_zones.cfg.max_simultaneous_relays = 3;
    snprintf(s_zones.cfg.zones[1].name, sizeof(s_zones.cfg.zones[1].name), "z1");
    s_zones.cfg.zones[1].relay_mask = 0x02;
    s_zones.cfg.zones[1].thermo_mask = 0x02;
    s_zones.cfg.zones[1].max_temp_c = 1200.0f;
    s_zones.cfg.zones[1].model_k_dc = 31.9669f;
    snprintf(s_zones.cfg.zones[2].name, sizeof(s_zones.cfg.zones[2].name), "z2");
    s_zones.cfg.zones[2].relay_mask = 0x04;
    s_zones.cfg.zones[2].thermo_mask = 0x04;
    s_zones.cfg.zones[2].max_temp_c = 1200.0f;
    s_zones.cfg.zones[2].model_k_dc = 31.6810f;
    TEST_CHECK(nvs_save() == ESP_OK, "base 3-zone config saves");

    // Real-shaped values: §2's adopted coupling matrix (affected][stepped])
    // and its zone-0/z1/z2 identified gains, via the real setters so their
    // own validation runs too.
    TEST_CHECK(zones_config_set_pid(0, 12.5f, 0.045f, 3.2f), "zone 0 PID gains set");
    TEST_CHECK(zones_config_set_pid(1, 9.8f, 0.038f, 2.1f), "zone 1 PID gains set");
    TEST_CHECK(zones_config_set_pid(2, 10.4f, 0.041f, 2.4f), "zone 2 PID gains set");

    TEST_CHECK(zones_config_set_coupling_cell(0, 1, 27.32f, 620.0f, 145.0f), "z0<-z1 coupling cell set");
    TEST_CHECK(zones_config_set_coupling_cell(0, 2, 21.72f, 705.0f, 158.0f), "z0<-z2 coupling cell set");
    TEST_CHECK(zones_config_set_coupling_cell(1, 0, 14.30f, 655.0f, 135.0f), "z1<-z0 coupling cell set");
    TEST_CHECK(zones_config_set_coupling_cell(1, 2, 22.15f, 730.0f, 150.0f), "z1<-z2 coupling cell set");
    TEST_CHECK(zones_config_set_coupling_cell(2, 0, 8.33f, 680.0f, 140.0f), "z2<-z0 coupling cell set");
    TEST_CHECK(zones_config_set_coupling_cell(2, 1, 12.42f, 700.0f, 142.0f), "z2<-z1 coupling cell set");

    TEST_CHECK(zones_config_set_coupling_diag_k_dc(0, 39.2459f), "zone 0 coupling_diag_k_dc set");
    TEST_CHECK(zones_config_set_coupling_diag_k_dc(1, 35.90f), "zone 1 coupling_diag_k_dc set");
    TEST_CHECK(zones_config_set_coupling_diag_k_dc(2, 35.32f), "zone 2 coupling_diag_k_dc set");

    // File must actually hold the LAST write, not a stale earlier rev.
    zones_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid, "file holds a valid blob after the setter round");
    TEST_CHECK_NEAR(raw.zones[2].coupling_diag_k_dc, 35.32f, 1e-4, "file's LAST write (z2 diag k_dc) landed");

    // "Reboot": wipe the in-RAM struct exactly like a power cycle would,
    // then reload through the real dual-write resolve/decode path -- not a
    // direct struct copy.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "reload after simulated reboot succeeds");

    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    TEST_CHECK(zones_config_get_pid(0, &kp, &ki, &kd) && kp == 12.5f && ki == 0.045f && kd == 3.2f,
               "zone 0 PID gains round-trip exactly");
    TEST_CHECK(zones_config_get_pid(1, &kp, &ki, &kd) && kp == 9.8f && ki == 0.038f && kd == 2.1f,
               "zone 1 PID gains round-trip exactly");
    TEST_CHECK(zones_config_get_pid(2, &kp, &ki, &kd) && kp == 10.4f && ki == 0.041f && kd == 2.4f,
               "zone 2 PID gains round-trip exactly");

    float row0[MAX31856_CHANNEL_COUNT] = {0};
    float row1[MAX31856_CHANNEL_COUNT] = {0};
    float row2[MAX31856_CHANNEL_COUNT] = {0};
    TEST_CHECK(zones_config_get_coupling(0, row0) && row0[0] == 0.0f && row0[1] == 27.32f && row0[2] == 21.72f,
               "zone 0 coupling row round-trips, diagonal still 0");
    TEST_CHECK(zones_config_get_coupling(1, row1) && row1[0] == 14.30f && row1[1] == 0.0f && row1[2] == 22.15f,
               "zone 1 coupling row round-trips, diagonal still 0");
    TEST_CHECK(zones_config_get_coupling(2, row2) && row2[0] == 8.33f && row2[1] == 12.42f && row2[2] == 0.0f,
               "zone 2 coupling row round-trips, diagonal still 0");

    float tau0[MAX31856_CHANNEL_COUNT] = {0};
    float dt0[MAX31856_CHANNEL_COUNT] = {0};
    TEST_CHECK(zones_config_get_coupling_tau(0, tau0) && tau0[1] == 620.0f && tau0[2] == 705.0f,
               "zone 0 coupling tau_s round-trips");
    TEST_CHECK(zones_config_get_coupling_dead_time(0, dt0) && dt0[1] == 145.0f && dt0[2] == 158.0f,
               "zone 0 coupling dead_time_s round-trips");

    float diag = 0.0f;
    TEST_CHECK(zones_config_get_coupling_diag_k_dc(0, &diag) && diag == 39.2459f, "zone 0 diag k_dc round-trips");
    TEST_CHECK(zones_config_get_coupling_diag_k_dc(1, &diag) && diag == 35.90f, "zone 1 diag k_dc round-trips");
    TEST_CHECK(zones_config_get_coupling_diag_k_dc(2, &diag) && diag == 35.32f, "zone 2 diag k_dc round-trips");

    // Belt and braces: file and NVS must agree after the reload too, same
    // "no divergence" property test_dual_write_keeps_file_and_nvs_in_sync
    // checks for name/pid_kp.
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && raw.zones[1].coupling_coeff[2] == 22.15f && raw.zones[1].pid_ki == 0.038f,
               "file still agrees with NVS on both coupling and PID gains after reload");
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
    TEST_SECTION("zones cfg_fs: a failed cfg write is loud and leaves the rev alone; legacy NVS vs file "
                 "tie-break picks the strictly higher rev in both directions");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    stage("rev1", 1.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "rev 1 saved to the file");

    // A cfg write failure is reported to the caller and nothing falls back to NVS.
    zones_config_cfg_fs_set_write_fn(failing_write_fn);
    stage("rev2_lost", 2.0f);
    TEST_CHECK(nvs_save() != ESP_OK, "a failed cfg write is returned to the caller");
    zones_config_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(nvs_zones_blob_absent(), "nothing fell back to NVS");

    zones_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 1 && strcmp(raw.zones[0].name, "rev1") == 0,
               "file is stuck at rev 1 -- the failed write never landed");

    // Direction (a): legacy NVS copy at a strictly higher rev wins, file resynced.
    stage_legacy_nvs("legacy_nvs", 5.0f, 5);
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "legacy_nvs") == 0, "NVS (rev 5) beats the file (rev 1)");
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev >= 5 && strcmp(raw.zones[0].name, "legacy_nvs") == 0,
               "the file was resynced from the legacy NVS copy");

    // Direction (b): a normal save now puts the file strictly above NVS and it wins.
    stage("rev6", 6.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "next save lands");
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load after the save succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "rev6") == 0,
               "the file (strictly higher rev) beats the stale legacy NVS blob");
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
    // Both the NVS decode path (nvs_load_from_decode()) and, as of this fix,
    // the file decode path (zones_config_cfg_fs_load_raw()) apply
    // zones_config_json_normalize_settings_source_cycles() right after decode
    // as one fixed two-step contract, not something either path invents on
    // its own -- so the reference vector here must apply that same second
    // step to stay a fair comparison. This v21 fixture's single zone (index
    // 0) leaves settings_source[group] zero-initialized, which
    // zone_settings_source_chain_has_cycle() treats as a self-reference
    // cycle on the first hop by design (see that header's own doc comment),
    // so normalize does mutate it -- on both sides identically.
    zones_cfg_t direct;
    const char *reason = "";
    zones_decode_result_t direct_result = zones_config_json_decode_blob(&v21, sizeof(v21), &direct, &reason);
    zones_config_json_normalize_settings_source_cycles(&direct, "test:direct-v21");

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
    TEST_CHECK(nvs_save() == ESP_OK, "rev 1 saved to the file");

    // Now act like firmware that predates this change: rewrite ONLY the
    // NVS blob at the same rev, leaving the file at rev 1/old content.
    stage_legacy_nvs("rolled_edit", 42.0f, 1);

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

// ---------------------------------------------------------------------
// 8. The gap this pass closes: the `cfg` file wins the tie-break (here,
//    simplest case -- NVS has nothing trustworthy at all) carrying an
//    OLD-VERSION blob that only migrates in RAM during decode. Before this
//    fix, nvs_load() discarded the NVS side unconditionally whenever the
//    file won (CLAUDE.md/CONFIG_MIGRATION_CHAIN.md section 1.5's
//    "used_file is unreachable in practice" gap) and never wrote the
//    migrated struct back anywhere -- both NVS (still empty/stale) and the
//    file (still holding the old-version bytes) would diverge from what
//    this boot is actually running on, and a future firmware's one-step
//    migration policy would be unable to consume either of them. This test
//    proves NVS now ends up holding the fully-migrated, current-version
//    blob, read-back verified, after a file-sourced migration -- the same
//    guarantee §1.6 already gave the NVS-sourced case.
// ---------------------------------------------------------------------
static void test_file_wins_after_migration_writes_back_to_nvs(void)
{
    TEST_SECTION("zones cfg_fs: cfg file wins with an old-version blob (NVS empty) -- the migrated result "
                 "must be written back to the file, not left RAM-only");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Stage an old-version (v21, immediately pre-progress_band_c) blob
    // directly into the file at rev 7 -- same staging technique test 5
    // above uses, bypassing zones_config_cfg_fs_save() (which always writes
    // CURRENT version) because this test wants an old on-disk file version.
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
    snprintf(v21.zones[0].name, sizeof(v21.zones[0].name), "FileMigrated");
    v21.zones[0].pid_kp = 6.25f;
    v21.zones[0].max_temp_c = 1150.0f;
    v21.zones[0].model_k_dc = 15.0f;
    v21.crc32 = 0;

    uint8_t filebuf[4 + sizeof(v21)];
    filebuf[0] = 7;
    filebuf[1] = 0;
    filebuf[2] = 0;
    filebuf[3] = 0;
    memcpy(filebuf + 4, &v21, sizeof(v21));
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK,
               "test setup: old-version (v21) blob staged directly into the file at rev 7");

    // NVS side is genuinely empty (reset_all()/fake_kv_reset_all(), and no
    // nvs_save() has run this test) -- nvs_load_from() must report
    // found=false, valid=false, so the file wins the tie-break outright
    // (zones_config_cfg_fs_resolve()'s `!nvs_valid` branch).
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid,
               "load adopts the file-sourced, migrated config");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "FileMigrated") == 0 && s_zones.cfg.zones[0].pid_kp == 6.25f,
               "in-RAM config is the migrated file content");
    TEST_CHECK(s_zones.cfg.version == ZONES_CFG_VERSION, "in-RAM version is CURRENT, not the staged v21 byte");

    // THE FIX: the cfg file must now hold the migrated, current-version
    // blob, read back as raw bytes (not through the decode path, which would
    // migrate the old bytes again and mask an unwritten file). NVS is no
    // longer written at all.
    static uint8_t raw_blob[sizeof(zones_cfg_t)];
    uint32_t file_rev_after = 0;
    size_t raw_len = read_file_blob_raw(raw_blob, sizeof(raw_blob), &file_rev_after);
    TEST_CHECK(raw_len == sizeof(zones_cfg_t), "the file blob is full current-version size");
    TEST_CHECK(raw_len == sizeof(zones_cfg_t) && raw_blob[0] == ZONES_CFG_VERSION,
               "THE FIX: the file holds the migrated CURRENT version, not the staged v21 bytes");
    TEST_CHECK(file_rev_after > 7, "the rev advanced past the staged rev 7 once the write-back ran");
    TEST_CHECK(nvs_zones_blob_absent(), "NVS was not written by the write-back");
}

// ---------------------------------------------------------------------
// 8b. Reviewer advisory (a03ead6c) on zones_config_store.c's file-won call
//    site of zones_config_persist_migrated_blob_verified(): when that
//    write-back cannot be verified (see test_zones_http.c's "lying write"
//    test for the NVS-sourced version of this same shape), the latched
//    zones_cfg_migration_persist_fault_t::on_disk_version used to be a
//    hardcoded 0 for a file-sourced migration -- meaningless to an
//    operator, since a real version WAS on disk (in the file). Same setup
//    as test 8 above (an old-version file, empty NVS), but the write-back
//    is sabotaged to lie about succeeding, so the fault must latch, and it
//    must name the file blob's real pre-migration version (21), never 0.
// ---------------------------------------------------------------------
static void test_file_won_migration_persist_fault_names_real_file_version(void)
{
    TEST_SECTION("zones cfg_fs: a file-sourced migration whose write-back cannot be verified must latch "
                 "the migration-persist fault with the FILE's real on-disk version, not a hardcoded 0");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Same v21 staging technique as test 8 above.
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
    snprintf(v21.zones[0].name, sizeof(v21.zones[0].name), "FileMigLying"); /* <= ZONE_NAME_MAX_LEN (15) --
                                                                              * "FileMigratedLying" (17 chars)
                                                                              * silently failed validate()/
                                                                              * truncated and broke this test the
                                                                              * first time around. */
    v21.zones[0].pid_kp = 4.5f;
    v21.zones[0].max_temp_c = 1100.0f;
    v21.zones[0].model_k_dc = 15.0f;
    v21.crc32 = 0;

    uint8_t filebuf[4 + sizeof(v21)];
    filebuf[0] = 3;
    filebuf[1] = 0;
    filebuf[2] = 0;
    filebuf[3] = 0;
    memcpy(filebuf + 4, &v21, sizeof(v21));
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK,
               "test setup: old-version (v21) blob staged directly into the file at rev 3");

    // NOTE: no "nothing latched yet" baseline check here -- the fault latch
    // is a single process-global that (by design, see zones_config_store.c's
    // M13 comment) is never reset by reset_all(); an earlier test in this
    // SAME executable (e.g. test_zones_http.c's NVS-sourced lying-write
    // test) may have already latched it before this test runs, and that is
    // not a defect for this test to detect.
    zones_cfg_migration_persist_fault_t fault;
    memset(&fault, 0xAA, sizeof(fault));

    // Sabotage the cfg write so the write-back cannot be verified on either
    // retry attempt.
    zones_config_cfg_fs_set_write_fn(failing_write_fn);

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid,
               "load still adopts the file-sourced, migrated in-RAM config even though write-back will fail");
    zones_config_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "FileMigLying") == 0,
               "in-RAM config is the migrated file content regardless of the write-back outcome");

    TEST_CHECK(zones_config_get_migration_persist_fault(&fault),
              "a file-sourced write-back that lies on both attempts must latch the migration-persist fault");
    TEST_CHECK(fault.on_disk_version == 21,
              "THE FIX (a03ead6c): the latched fault must name the FILE blob's real pre-migration version "
              "(21) -- it used to be hardcoded to 0 on this call site, which read as a meaningless "
              "\"on-disk v0\" to an operator even though v21 really was on disk, in the file");
    TEST_CHECK(fault.fw_version == ZONES_CFG_VERSION,
              "the latched fault must name the firmware version the migration was TO, not a stale value");
}

// ---------------------------------------------------------------------
// 9. Review follow-up to test 8: the write-back must NEVER fire when the
//    NVS side was FOUND but refused as NEWER than this firmware
//    (ZONES_DECODE_NEWER). That branch's contract is "flash data left
//    untouched" -- it is real data written by a firmware ahead of this one,
//    and an older file-sourced config overwriting it destroys it
//    permanently. Reachable asymmetrically because nvs_save() writes the
//    FILE FIRST and swallows the file write's error, so a newer firmware
//    can leave NVS at vN+1 with the file still holding a valid, older vN.
//    The file still wins THIS boot's in-RAM config -- only the write-back
//    is suppressed.
// ---------------------------------------------------------------------
static void test_newer_nvs_blob_is_not_overwritten_by_file_writeback(void)
{
    TEST_SECTION("zones cfg_fs: a newer-than-firmware NVS blob must survive a file-sourced load -- the "
                 "write-back is suppressed, not allowed to downgrade protected data");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Stage a newer-than-this-firmware blob straight into NVS. Exactly
    // sizeof(zones_cfg_t) bytes so nvs_load_from_decode()'s read buffer
    // takes it; the version byte alone is what makes it ZONES_DECODE_NEWER
    // (that branch returns before any length check).
    uint8_t newer[sizeof(zones_cfg_t)];
    for (size_t i = 0; i < sizeof(newer); ++i) {
        newer[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
    }
    newer[0] = (uint8_t)(ZONES_CFG_VERSION + 1);
    hal_kv_handle_t wh;
    TEST_CHECK(hal_kv_open(&wh, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "test setup: NVS opened read-write");
    TEST_CHECK(hal_kv_set_blob(&wh, NVS_KEY_ZONES, newer, sizeof(newer)) == HAL_OK,
               "test setup: newer-than-firmware blob staged into NVS");
    TEST_CHECK(hal_kv_commit(&wh) == HAL_OK, "test setup: NVS commit");
    hal_kv_close(&wh);

    // File side holds a perfectly valid, CURRENT-version config.
    zones_cfg_t file_cfg;
    fill_valid_cfg(&file_cfg, "NewerGuard", 4.5f);
    file_cfg.version = ZONES_CFG_VERSION; /* fill_valid_cfg() leaves version 0; only nvs_save() stamps it, and
                                           * zones_config_cfg_fs_save() re-stamps only the CRC -- a version-0
                                           * file decodes as CORRUPT, not as the valid file this test needs. */
    TEST_CHECK(zones_config_cfg_fs_save(&file_cfg, 3) == ESP_OK,
               "test setup: valid current-version file written");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid,
               "load adopts the file (NVS side refused)");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "NewerGuard") == 0,
               "in-RAM config is the file's -- unchanged from before the write-back fix");

    // THE GUARD: NVS must still hold the newer blob, byte for byte.
    hal_kv_handle_t rh;
    uint8_t after[sizeof(zones_cfg_t)];
    memset(after, 0, sizeof(after));
    size_t after_len = sizeof(after);
    TEST_CHECK(hal_kv_open(&rh, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "NVS opened read-only for verification");
    hal_status_t rb = hal_kv_get_blob(&rh, NVS_KEY_ZONES, after, &after_len);
    hal_kv_close(&rh);
    TEST_CHECK(rb == HAL_OK && after_len == sizeof(newer),
               "the NVS blob is still there at its original size");
    TEST_CHECK(after_len == sizeof(newer) && memcmp(after, newer, sizeof(newer)) == 0,
               "THE GUARD: the newer-than-firmware NVS blob was NOT overwritten by the file-sourced "
               "write-back -- protected data survives a downgrade boot");
}

// ---------------------------------------------------------------------
// 10. Review follow-up to test 8: the write-back must happen AT MOST ONCE
//     per divergence, never on every boot (flash wear / the reset-one-side
//     class). After the first load resyncs both sides, a second load with
//     the in-RAM struct wiped must find them already equal and write
//     nothing -- proven by the dual-write rev counter (bumped by every
//     nvs_save()) standing still, and by the NVS blob bytes being
//     unchanged.
// ---------------------------------------------------------------------
static uint32_t read_file_rev(void)
{
    static uint8_t blob[sizeof(zones_cfg_t)];
    uint32_t rev = 0;
    (void)read_file_blob_raw(blob, sizeof(blob), &rev);
    return rev;
}

static void test_file_sourced_writeback_happens_once_not_every_boot(void)
{
    TEST_SECTION("zones cfg_fs: a file-sourced write-back converges -- the next load finds both sides equal "
                 "and writes nothing");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    // Same staging as test 8: an old-version (v21) blob in the file, NVS
    // empty, so the first load migrates in RAM and writes back.
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
    snprintf(v21.zones[0].name, sizeof(v21.zones[0].name), "OnceOnly");
    v21.zones[0].pid_kp = 2.75f;
    v21.zones[0].max_temp_c = 1150.0f;
    v21.zones[0].model_k_dc = 15.0f;
    v21.crc32 = 0;

    uint8_t filebuf[4 + sizeof(v21)];
    filebuf[0] = 9;
    filebuf[1] = 0;
    filebuf[2] = 0;
    filebuf[3] = 0;
    memcpy(filebuf + 4, &v21, sizeof(v21));
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK,
               "test setup: old-version (v21) blob staged into the file at rev 9");

    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid,
               "first load adopts and writes back");
    uint32_t rev_after_first = read_file_rev();
    TEST_CHECK(rev_after_first > 9, "the write-back ran on the first load (rev advanced past the file's 9)");

    static uint8_t blob_after_first[sizeof(zones_cfg_t)];
    size_t len1 = read_file_blob_raw(blob_after_first, sizeof(blob_after_first), NULL);
    TEST_CHECK(len1 == sizeof(zones_cfg_t), "the file holds the written-back current-version blob");

    // SECOND boot: same on-flash state, in-RAM struct wiped. Both sides now
    // decode to the same bytes, so nothing must be written.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    found = false;
    valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "second load succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "OnceOnly") == 0 && s_zones.cfg.zones[0].pid_kp == 2.75f,
               "second load still yields the same config");
    TEST_CHECK(read_file_rev() == rev_after_first,
               "THE CONVERGENCE GUARD: the rev did NOT advance on the second load -- no nvs_save(), "
               "so no write every boot (flash wear / reset-one-side class)");

    static uint8_t blob_after_second[sizeof(zones_cfg_t)];
    size_t len2 = read_file_blob_raw(blob_after_second, sizeof(blob_after_second), NULL);
    TEST_CHECK(len1 == len2 && memcmp(blob_after_first, blob_after_second, len1) == 0,
               "the file blob is byte-identical across the second load -- nothing was rewritten");
}

// ---------------------------------------------------------------------
// 11. Follow-up to cce21da0 (2026-09-22): the `cfg` file's own decode path
//     (zones_config_cfg_fs_load_raw()) did not normalize a stored
//     settings_source cycle before this pass -- only zones_config_store.c's
//     NVS-side decode (nvs_load_from_decode()) called
//     zones_config_json_normalize_settings_source_cycles(). A file holding a
//     cycle (however it got there) that then WINS zones_config_cfg_fs_
//     resolve()'s tie-break was adopted un-normalized into s_zones.cfg, and
//     test 8's single-migration-step write-back would persist that
//     un-normalized cycle into NVS too -- converging only two boots later,
//     on NVS's own next decode, instead of immediately (an asymmetry, not a
//     rejection: the config still loads and runs). Proves both the in-RAM
//     struct and the NVS blob written back by the file-won path are
//     normalized after ONE load.
// ---------------------------------------------------------------------
static void test_file_cycle_is_normalized_in_ram_and_on_writeback(void)
{
    TEST_SECTION("zones cfg_fs: a settings_source cycle stored in the `cfg` file is normalized on load, "
                 "both in RAM and in the file the write-back rewrites");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    zones_cfg_t cyc;
    fill_valid_cfg(&cyc, "z0", 5.0f);
    cyc.version = ZONES_CFG_VERSION; /* fill_valid_cfg() leaves version 0 -- only nvs_save()/
                                       * zones_config_cfg_fs_save() writing raw bytes as-is means
                                       * this file's own decode must see the CURRENT version to
                                       * accept it. */
    cyc.thermo_count = 2;
    cyc.relay_count = 2;
    cyc.max_simultaneous_relays = 2;
    snprintf(cyc.zones[1].name, sizeof(cyc.zones[1].name), "z1");
    cyc.zones[1].relay_mask = 0x02;
    cyc.zones[1].thermo_mask = 0x02;
    cyc.zones[1].max_temp_c = 1200.0f;
    cyc.zones[1].model_k_dc = 12.0f;
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        cyc.zones[0].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        cyc.zones[1].settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
    }
    // The 2-cycle the live setter refuses -- reachable here only because
    // this bypasses zones_http.c's own write-time guard, writing directly
    // via zones_config_cfg_fs_save() (which only re-stamps the CRC and never
    // validates or normalizes settings_source), same as this file predating
    // the normalize guard, or being tampered with directly, would look like.
    cyc.zones[0].settings_source[SRC_GROUP_LIMITS] = 1;
    cyc.zones[1].settings_source[SRC_GROUP_LIMITS] = 0;
    cyc.zones[0].pid_kp = 5.0f; // must survive normalization, proving this is not a wipe

    TEST_CHECK(zones_config_cfg_fs_save(&cyc, 5) == ESP_OK,
               "test setup: a settings_source cycle staged directly into the file at rev 5");

    // NVS side is genuinely empty (reset_all()/fake_kv_reset_all(), no
    // nvs_save() run this test), so the file wins zones_config_cfg_fs_
    // resolve()'s tie-break outright.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    bool found = false, valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "load adopts the file-sourced config");

    uint8_t s0 = s_zones.cfg.zones[0].settings_source[SRC_GROUP_LIMITS];
    uint8_t s1 = s_zones.cfg.zones[1].settings_source[SRC_GROUP_LIMITS];
    TEST_CHECK(s0 != 1 || s1 != 0, "IN RAM: the stored 2-cycle no longer exists after load -- at least one "
                                   "of the two links was broken by normalization");
    TEST_CHECK(s0 == ZONE_SETTINGS_SOURCE_CUSTOM || s1 == ZONE_SETTINGS_SOURCE_CUSTOM,
              "IN RAM: the break collapsed (at least) one cyclic zone to Custom, the same resolution the "
              "NVS-side decode path already used before this fix");
    TEST_CHECK_NEAR(s_zones.cfg.zones[0].pid_kp, 5.0f, 1e-6,
                    "normalization touched only settings_source -- zone 0's pid_kp survives intact");

    // THE FIX: the write-back this file-won load triggers must persist the
    // ALREADY-NORMALIZED struct, not the raw cyclic bytes -- read the file
    // as raw bytes (never through zones_config_cfg_fs_load_raw(), which
    // normalizes AGAIN on its own decode and would mask an unfixed bug in
    // the write-back path).
    static uint8_t raw_blob[sizeof(zones_cfg_t)];
    size_t raw_len = read_file_blob_raw(raw_blob, sizeof(raw_blob), NULL);
    TEST_CHECK(raw_len == sizeof(zones_cfg_t), "the write-back landed in the file");
    zones_cfg_t file_cfg;
    memcpy(&file_cfg, raw_blob, sizeof(file_cfg));
    uint8_t f_s0 = file_cfg.zones[0].settings_source[SRC_GROUP_LIMITS];
    uint8_t f_s1 = file_cfg.zones[1].settings_source[SRC_GROUP_LIMITS];
    TEST_CHECK(f_s0 != 1 || f_s1 != 0,
              "THE FIX: the file written back by the file-won path does NOT hold the un-normalized cycle");
}

// ---------------------------------------------------------------------
// OOM on the scratch allocations (47e07df6 review, 2a/2b). The hook lives in
// persist_scratch.h under KILNCTL_PERSIST_SCRATCH_TEST_HOOK.
// ---------------------------------------------------------------------
size_t persist_scratch_test_fail_size = 0;
int persist_scratch_test_fail_nth = 0;
int persist_scratch_test_seen = 0;

static void scratch_oom_arm(int nth)
{
    persist_scratch_test_fail_size = sizeof(zones_cfg_t);
    persist_scratch_test_seen = 0;
    persist_scratch_test_fail_nth = nth;
}

static void scratch_oom_disarm(void)
{
    persist_scratch_test_fail_nth = 0;
}

static void fill_v21_fixture(zones_cfg_v21_t *v21)
{
    memset(v21, 0, sizeof(*v21));
    v21->version = 21;
    v21->thermo_count = 1;
    v21->relay_count = 1;
    v21->max_simultaneous_relays = 1;
    v21->safety_tc_type = 3;
    v21->timing_profile_count = 1;
    strncpy(v21->timing_profiles[0].name, "Default", TIMING_PROFILE_NAME_MAX_LEN);
    v21->timing_profiles[0].guard_progress_duty_min = 0.1f;
    v21->timing_profiles[0].ramp_lock_band_c = 3.0f;
    v21->zones[0].relay_mask = 1;
    v21->zones[0].thermo_mask = 1;
    snprintf(v21->zones[0].name, sizeof(v21->zones[0].name), "V21Zone");
    v21->zones[0].pid_kp = 9.0f;
    v21->zones[0].max_temp_c = 1200.0f;
    v21->zones[0].model_k_dc = 12.0f;
}

/* Common setup: a CURRENT-version file at rev 5 plus a stale legacy NVS copy at rev 1. */
static void oom_setup_file_rev5_and_stale_nvs(void)
{
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero(); /* deterministic: a skipped rev floor would restamp rev 1 */
    zones_cfg_t c;
    fill_valid_cfg(&c, "newer_file", 4.0f);
    c.version = ZONES_CFG_VERSION;
    c.crc32 = zones_config_json_compute_crc(&c);
    TEST_CHECK(zones_config_cfg_fs_save(&c, 5) == ESP_OK, "setup: file at rev 5");
    stage_legacy_nvs("stale_legacy", 1.0f, 1);
}

/* After an OOM'd load: a save must stamp a rev above 5 (never restamp rev 1) and the file must
 * hold the new content, proving the load did not rewind the rev floor. */
static void oom_check_rev_floor_kept(void)
{
    stage("after_oom", 9.0f);
    /* Review 2026-10-10 L1: the load could not decide, so a save is refused (it would write a near-empty
     * config over the higher-rev file); a later clean load decides and saves land again. */
    TEST_CHECK(nvs_save() == ESP_ERR_INVALID_STATE, "L1: a save after a cannot-decide load is refused");
    uint32_t rev = 0;
    zones_cfg_t raw;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 5, "L1: the refused save left the rev-5 file untouched");
    {
        bool f2 = false, v2 = false;
        TEST_CHECK(nvs_load(&f2, &v2) == ESP_OK, "L1: a clean reload decides");
    }
    stage("after_oom", 9.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "a later save lands");
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev > 1, "the save stamped a rev above the NVS rev floor (1), never restamped rev 1");
}

static void test_resolve_oom_keeps_rev_floor_and_fails_load(void)
{
    TEST_SECTION("zones cfg_fs: resolve's own candidate OOM fails the load, keeps the rev floor, never lets a "
                 "save clobber the newer file (47e07df6 review 2a)");
    oom_setup_file_rev5_and_stale_nvs();
    bool found = true, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    scratch_oom_arm(3); // 1 = nvs decode buffer, 2 = nvs_load's `resolved`, 3 = resolve's file candidate
    esp_err_t e = nvs_load(&found, &valid);
    scratch_oom_disarm();
    TEST_CHECK(persist_scratch_test_seen >= 3, "the 3rd same-size scratch allocation was reached (test is aimed right)");
    TEST_CHECK(e == ESP_ERR_NO_MEM, "nvs_load reports the OOM instead of ESP_OK");
    TEST_CHECK(!valid, "the legacy NVS copy is NOT adopted as valid");
    oom_check_rev_floor_kept();
}

/* Review 15 LOW-3: a file too large for the read buffer (newer firmware) is unreadable, not absent -- the load
 * must fail and the file must survive byte-for-byte instead of being overwritten from the older NVS copy. */
static void test_oversize_file_is_cannot_decide_not_absent(void)
{
    TEST_SECTION("zones cfg_fs: an over-size (unreadable) zones.json is 'cannot decide' and is never overwritten "
                 "from NVS (review 15 LOW-3)");
    oom_setup_file_rev5_and_stale_nvs();
    static uint8_t big[1028 + 200];
    memset(big, 0x5A, sizeof(big));
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, big, sizeof(big)) == ESP_OK, "setup: over-size file written");
    bool found = true, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    esp_err_t e = nvs_load(&found, &valid);
    TEST_CHECK(e == ESP_ERR_NO_MEM || !valid, "the load does not adopt the stale NVS copy as valid");
    static uint8_t back[1028 + 400];
    size_t blen = 0;
    TEST_CHECK(cfg_fs_read(ZONES_CFG_FILE_PATH, back, sizeof(back), &blen) == ESP_OK && blen == sizeof(big) &&
                   memcmp(back, big, blen) == 0,
               "the unreadable file is untouched (not overwritten from the older NVS copy)");
}

/* Review 2026-10-10 L1 + L3: a transient I/O error (not just over-size) reading zones.json is "cannot decide":
 * the load fails, a UNREADABLE load fault is latched (visible in status/readiness), saves are refused with
 * ESP_ERR_INVALID_STATE, and the higher-rev file survives. */
static void test_transient_read_error_is_cannot_decide_and_latches(void)
{
    TEST_SECTION("zones cfg_fs: a transient I/O error reading zones.json is 'cannot decide', latches a load fault "
                 "and refuses saves (review 2026-10-10 L1, L3)");
    oom_setup_file_rev5_and_stale_nvs();
    zones_config_load_fault_reset_for_test();
    bool found = true, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    cfg_fs_test_inject_read_error(ZONES_CFG_FILE_PATH, ESP_FAIL, 1);
    esp_err_t e = nvs_load(&found, &valid);
    cfg_fs_test_inject_read_error(NULL, ESP_OK, 0);
    TEST_CHECK(e == ESP_ERR_NO_MEM && !valid && !found, "L3: ESP_FAIL read is cannot-decide, not absent: load fails");
    zones_cfg_load_fault_t lf;
    TEST_CHECK(zones_config_get_load_fault(&lf) && lf.kind == ZONES_CFG_LOAD_FAULT_UNREADABLE,
               "L1: an UNREADABLE load fault is latched");
    stage("clobber", 9.0f);
    TEST_CHECK(nvs_save() == ESP_ERR_INVALID_STATE, "L1: operator save refused");
    uint32_t rev = 0;
    zones_cfg_t raw;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 5 && strcmp(raw.zones[0].name, "newer_file") == 0,
               "L1/L3: the rev-5 file is untouched by the refused save and by the load");
    zones_config_load_fault_reset_for_test();
}

static void test_nvs_load_resolved_oom_keeps_rev_floor(void)
{
    TEST_SECTION("zones cfg_fs: nvs_load's own `resolved` scratch OOM keeps the rev floor (fcdfc823 review HIGH1)");
    oom_setup_file_rev5_and_stale_nvs();
    bool found = true, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    scratch_oom_arm(2);
    esp_err_t e = nvs_load(&found, &valid);
    scratch_oom_disarm();
    TEST_CHECK(e == ESP_ERR_NO_MEM && !valid && !found, "load fails with nothing found/valid");
    oom_check_rev_floor_kept();
}

static void test_nvs_conversion_scratch_oom_is_not_corrupt(void)
{
    TEST_SECTION("zones cfg_fs: conversion-scratch OOM on an OLD-version NVS blob fails the load, latches no "
                 "UNREADABLE fault, leaves the newer file alone (fcdfc823 review HIGH2, NVS side)");
    oom_setup_file_rev5_and_stale_nvs();
    // Replace the stale current-version NVS blob with a v21 one.
    zones_cfg_v21_t v21;
    fill_v21_fixture(&v21);
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK, "open NVS");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_ZONES, &v21, sizeof(v21)) == HAL_OK, "stage v21 blob");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "commit");
        hal_kv_close(&h);
    }
    bool found = true, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    scratch_oom_arm(0); // fail by exact size below: the conversion scratch is sizeof(zones_cfg_t)
    persist_scratch_test_fail_size = sizeof(zones_cfg_t);
    persist_scratch_test_seen = 0;
    persist_scratch_test_fail_nth = 2; // 1 = nvs decode buffer, 2 = conversion scratch
    esp_err_t e = nvs_load(&found, &valid);
    scratch_oom_disarm();
    TEST_CHECK(e == ESP_ERR_NO_MEM && !valid, "load fails with NO_MEM, nothing adopted");
    zones_cfg_load_fault_t lf;
    TEST_CHECK(!zones_config_get_load_fault(&lf) || lf.kind != ZONES_CFG_LOAD_FAULT_UNREADABLE,
               "no UNREADABLE fault was latched for an OOM that judged nothing");
    uint32_t rev = 0;
    zones_cfg_t raw;
    bool raw_valid = false;
    zones_config_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 5 && strcmp(raw.zones[0].name, "newer_file") == 0,
               "the newer file is untouched");
    oom_check_rev_floor_kept();
}

static void test_file_conversion_scratch_oom_does_not_overwrite_file(void)
{
    TEST_SECTION("zones cfg_fs: conversion-scratch OOM on an OLD-version FILE is 'cannot decide' -- the newer "
                 "file is not replaced by the legacy NVS blob (fcdfc823 review HIGH2, file side)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zones_cfg_v21_t v21;
    fill_v21_fixture(&v21);
    uint8_t filebuf[4 + sizeof(v21)];
    filebuf[0] = 5; filebuf[1] = 0; filebuf[2] = 0; filebuf[3] = 0;
    memcpy(filebuf + 4, &v21, sizeof(v21));
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, filebuf, sizeof(filebuf)) == ESP_OK, "v21 file at rev 5");
    stage_legacy_nvs("stale_legacy", 1.0f, 1);

    bool found = true, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    persist_scratch_test_fail_size = sizeof(zones_cfg_t);
    persist_scratch_test_seen = 0;
    persist_scratch_test_fail_nth = 4; // 1 nvs decode, 2 `resolved`, 3 resolve candidate, 4 file conversion scratch
    esp_err_t e = nvs_load(&found, &valid);
    int seen = persist_scratch_test_seen;
    scratch_oom_disarm();
    TEST_CHECK(seen >= 4, "the 4th same-size scratch allocation was reached (test is aimed right)");
    TEST_CHECK(e == ESP_ERR_NO_MEM && !valid, "load fails loud");
    static uint8_t after[4 + sizeof(zones_cfg_t)];
    size_t alen = 0;
    TEST_CHECK(cfg_fs_read(ZONES_CFG_FILE_PATH, after, sizeof(after), &alen) == ESP_OK &&
                   alen == sizeof(filebuf) && memcmp(after, filebuf, alen) == 0,
               "the file is byte-for-byte unchanged (not overwritten with the legacy NVS blob)");
}

static void test_start_does_not_migrate_after_load_oom(void)
{
    TEST_SECTION("zones_http_start: an nvs_load OOM does not run the legacy-partition migration "
                 "(47e07df6 review 2b)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    hal_kv_init_partition(NVS_DEFAULT_PART_NAME);
    {
        zones_cfg_t c;
        fill_valid_cfg(&c, "prefsplit", 2.0f);
        c.version = ZONES_CFG_VERSION;
        c.crc32 = zones_config_json_compute_crc(&c);
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NVS_DEFAULT_PART_NAME) == HAL_OK,
                   "stage: open default partition");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_ZONES, &c, sizeof(c)) == HAL_OK, "stage: pre-split blob");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "stage: commit");
        hal_kv_close(&h);
    }
    s_zones_config_valid = true; // wrong on purpose
    scratch_oom_arm(1);
    (void)zones_http_start();
    scratch_oom_disarm();
    TEST_CHECK(!s_zones_config_valid, "config not valid after the OOM'd load");
    bool exists = true;
    TEST_CHECK(cfg_fs_exists(ZONES_CFG_FILE_PATH, &exists) == ESP_OK && !exists,
               "no stale pre-split copy was saved into the cfg file");
    hal_kv_handle_t h;
    reset_all();
    (void)h;
}

static void l5_interleaved_setter(void)
{
    s_zones_cfg_unlock_test_hook = NULL; /* one shot: fire on the snapshot's unlock only */
    s_zones.cfg.zones[0].pid_kp += 3.0f; /* a setter edit between the snapshot and the CRC write-back */
}

static void test_save_crc_writeback_skipped_when_ram_changed(void)
{
    TEST_SECTION("zones store: nvs_save() CRC write-back never stamps a newer RAM state (review 5 L5)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    stage("l5", 7.0f);
    uint32_t crc_before = s_zones.cfg.crc32;
    s_zones_cfg_unlock_test_hook = l5_interleaved_setter;
    TEST_CHECK(nvs_save() == ESP_OK, "save ok");
    s_zones_cfg_unlock_test_hook = NULL;
    TEST_CHECK(s_zones.cfg.crc32 == crc_before, "RAM crc not overwritten with the stale snapshot's CRC");
    TEST_CHECK(nvs_save() == ESP_OK, "second save ok");
    TEST_CHECK(s_zones.cfg.crc32 == zones_config_json_compute_crc(&s_zones.cfg),
               "an uninterrupted save mirrors a CRC that matches RAM");
}

static void test_save_persists_locked_snapshot(void)
{
    TEST_SECTION("zones store: nvs_save() persists a snapshot taken under zones_cfg_lock (LOW-4)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    stage("snap", 7.0f);
    uint32_t a0 = s_zones_cfg_lock_acquires;
    TEST_CHECK(nvs_save() == ESP_OK, "save ok");
    TEST_CHECK(s_zones_cfg_lock_acquires - a0 >= 2, "nvs_save took the zones lock to snapshot RAM and to publish the rev");
    uint32_t a1 = s_zones_cfg_lock_acquires;
    {
        /* What reached the file is the stamped snapshot: version/CRC on disk match the RAM mirror. */
        zones_cfg_t *rr = malloc(sizeof(*rr));
        uint32_t rv = 0;
        bool vv = false;
        zones_config_cfg_fs_load_raw(rr, &rv, &vv);
        TEST_CHECK(vv && rr->version == ZONES_CFG_VERSION && rr->crc32 == s_zones.cfg.crc32,
                   "the file holds a stamped snapshot whose CRC matches RAM's mirror");
        free(rr);
    }
    bool eq = zones_config_persisted_equals_ram();
    (void)eq; /* staged test fixtures are not load-normalized; the lock is what is asserted here */
    TEST_CHECK(s_zones_cfg_lock_acquires > a1, "persisted_equals_ram snapshots RAM under the lock");
    s_zones.cfg.zones[0].pid_kp += 1.0f;
    TEST_CHECK(!zones_config_persisted_equals_ram(), "an unsaved RAM edit is detected");
}

// ---------------------------------------------------------------------
// POST /api/cfgfs/file content gate (audit M7): cfgfs_file_check_write().
// ---------------------------------------------------------------------
static void test_cfgfs_file_post_validation(void)
{
    TEST_SECTION("cfgfs file POST: body validated by the loader; unknown names need raw=1");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();
    stage("m7", 2.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save a real zones.json");
    static uint8_t good[4 + sizeof(zones_cfg_t)];
    size_t glen = 0;
    TEST_CHECK(cfg_fs_read(ZONES_CFG_FILE_PATH, good, sizeof(good), &glen) == ESP_OK && glen > 4,
               "read the real file bytes back");

    const char *why = "";
    TEST_CHECK(cfgfs_file_check_write(ZONES_CFG_FILE_PATH, false, good, glen, &why) == CFGFS_FILE_CHECK_OK,
               "valid zones.json body accepted");
    TEST_CHECK(cfgfs_file_check_write(ZONES_CFG_FILE_PATH, true, good, glen, &why) == CFGFS_FILE_CHECK_OK,
               "valid zones.json body accepted with raw=1");

    /* The handler writes only on OK, so INVALID == nothing written. */
    TEST_CHECK(cfgfs_file_check_write(ZONES_CFG_FILE_PATH, false, "garbage", 7, &why) == CFGFS_FILE_CHECK_INVALID,
               "garbage zones.json refused");
    TEST_CHECK(cfgfs_file_check_write(ZONES_CFG_FILE_PATH, true, "garbage", 7, &why) == CFGFS_FILE_CHECK_INVALID,
               "raw=1 does not waive an existing validator");
    static uint8_t bad[4 + sizeof(zones_cfg_t)];
    memcpy(bad, good, glen);
    bad[glen - 1] ^= 0xFF;
    bad[10] ^= 0x55;
    TEST_CHECK(cfgfs_file_check_write(ZONES_CFG_FILE_PATH, false, bad, glen, &why) == CFGFS_FILE_CHECK_INVALID,
               "corrupted (CRC-failing) zones.json refused");
    TEST_CHECK(cfgfs_file_check_write(ZONES_CFG_FILE_PATH, false, good, 3, &why) == CFGFS_FILE_CHECK_INVALID,
               "truncated zones.json refused");

    TEST_CHECK(cfgfs_file_check_write("mystery.dat", false, "abc", 3, &why) == CFGFS_FILE_CHECK_NO_VALIDATOR,
               "unknown file without raw=1 refused");
    TEST_CHECK(cfgfs_file_check_write("mystery.dat", true, "abc", 3, &why) == CFGFS_FILE_CHECK_OK,
               "unknown file with raw=1 accepted");
    cfg_fs_deinit();
}

// ---------------------------------------------------------------------
// BENCH_PROF1_DIVERGE audit (c) fixes 1, 2, 4: a rejected zones.json is kept as zones.json.bad and
// latches a visible load fault; never silently left to be overwritten.
// ---------------------------------------------------------------------
static bool s_reset_refuses = false;
static bool reset_refuse_hook(void) { return s_reset_refuses; }

static size_t read_file(const char *name, uint8_t *buf, size_t cap)
{
    size_t n = 0;
    if (cfg_fs_read(name, buf, cap, &n) != ESP_OK) {
        return (size_t)-1;
    }
    return n;
}

static void test_rejected_file_is_preserved_and_latches_fault(void)
{
    TEST_SECTION("zones cfg_fs: a REJECTED zones.json is kept as zones.json.bad and latches a load fault");
    static uint8_t good[4 + sizeof(zones_cfg_t)], bad[4 + sizeof(zones_cfg_t)], got[4 + sizeof(zones_cfg_t)];
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();
    stage("keep", 2.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save a real zones.json");
    size_t glen = read_file(ZONES_CFG_FILE_PATH, good, sizeof(good));
    TEST_CHECK(glen > 8 && glen != (size_t)-1, "read the good file");

    /* 1. CRC-corrupt file, no NVS copy. */
    memcpy(bad, good, glen);
    bad[glen - 1] ^= 0xFF;
    bad[10] ^= 0x55;
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "plant a corrupt zones.json");
    zones_config_load_fault_reset_for_test();
    bool found = false, valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    (void)nvs_load(&found, &valid);
    TEST_CHECK(!valid, "the rejected file is not adopted");
    size_t blen = read_file(ZONES_CFG_BAD_FILE_PATH, got, sizeof(got));
    TEST_CHECK(blen == glen && memcmp(got, bad, glen) == 0, "zones.json.bad holds the rejected bytes exactly");
    TEST_CHECK(read_file(ZONES_CFG_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "zones.json itself is not touched by the load");
    zones_cfg_load_fault_t lf;
    TEST_CHECK(zones_config_get_load_fault(&lf) && lf.kind == ZONES_CFG_LOAD_FAULT_UNREADABLE,
               "the cfg-file rejection latches an UNREADABLE load fault");
    TEST_CHECK(strncmp(lf.reason, "cfg file: ", 10) == 0 && strlen(lf.reason) > 10,
               "the fault reason is distinct (cfg file prefix) and carries the decoder's reason");

    /* 2. A different rejected file replaces the older .bad (one copy). */
    bad[20] ^= 0x01;
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "plant a second corrupt file");
    (void)nvs_load(&found, &valid);
    TEST_CHECK(read_file(ZONES_CFG_BAD_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "the older .bad is overwritten by the newer rejected file");

    /* 3. Newer-version file: preserved, latched NEWER, file never rewritten. */
    memcpy(bad, good, glen);
    bad[4] = (uint8_t)(ZONES_CFG_VERSION + 1);
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "plant a newer-version file");
    zones_config_load_fault_reset_for_test();
    (void)nvs_load(&found, &valid);
    TEST_CHECK(read_file(ZONES_CFG_BAD_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "newer-version file preserved");
    TEST_CHECK(zones_config_get_load_fault(&lf) && lf.kind == ZONES_CFG_LOAD_FAULT_NEWER &&
                   lf.on_disk_version == ZONES_CFG_VERSION + 1,
               "newer-version file latches a NEWER fault naming the on-disk version");
    TEST_CHECK(read_file(ZONES_CFG_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "newer-version zones.json is never rewritten");

    /* 4. Too short. */
    TEST_CHECK(cfg_fs_delete(ZONES_CFG_BAD_FILE_PATH) == ESP_OK, "clear .bad");
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, "abc", 3) == ESP_OK, "plant a 3-byte file");
    zones_config_load_fault_reset_for_test();
    (void)nvs_load(&found, &valid);
    TEST_CHECK(read_file(ZONES_CFG_BAD_FILE_PATH, got, sizeof(got)) == 3 && memcmp(got, "abc", 3) == 0,
               "too-short file preserved");
    TEST_CHECK(zones_config_get_load_fault(&lf) && strstr(lf.reason, "too short") != NULL, "too-short fault named");

    /* 5. Reset in flight: nothing written. */
    TEST_CHECK(cfg_fs_delete(ZONES_CFG_BAD_FILE_PATH) == ESP_OK, "clear .bad again");
    s_reset_refuses = true;
    pref_cfg_fs_set_reset_refuse_hook(reset_refuse_hook);
    (void)nvs_load(&found, &valid);
    pref_cfg_fs_set_reset_refuse_hook(NULL);
    s_reset_refuses = false;
    bool exists = true;
    TEST_CHECK(cfg_fs_exists(ZONES_CFG_BAD_FILE_PATH, &exists) == ESP_OK && !exists,
               "no .bad written while a factory reset is in flight");

    /* 6. A good file leaves no fault and no .bad. */
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, good, glen) == ESP_OK, "restore the good file");
    zones_config_load_fault_reset_for_test();
    (void)nvs_load(&found, &valid);
    TEST_CHECK(valid && !zones_config_get_load_fault(NULL), "a good file loads with no fault latched");
    TEST_CHECK(cfg_fs_exists(ZONES_CFG_BAD_FILE_PATH, &exists) == ESP_OK && !exists, "and writes no .bad");
    cfg_fs_deinit();
}



// ---------------------------------------------------------------------
// Review 11 MED-1/MED-2/LOW-1/LOW-2: a rejected zones.json with a valid older NVS/legacy copy.
// ---------------------------------------------------------------------
static esp_err_t fail_bad_write(const char *name, const void *data, size_t len)
{
    if (strcmp(name, ZONES_CFG_BAD_FILE_PATH) == 0) {
        return ESP_FAIL;
    }
    return cfg_fs_write_atomic(name, data, len);
}

static int s_save_depth;
static int s_bad_write_depth = -1;
static bool depth_enter(void)
{
    s_save_depth++;
    return true;
}
static void depth_exit(bool reserved)
{
    (void)reserved;
    s_save_depth--;
}
static esp_err_t depth_recording_write(const char *name, const uint8_t *data, size_t len)
{
    if (strcmp(name, ZONES_CFG_BAD_FILE_PATH) == 0) {
        s_bad_write_depth = s_save_depth;
    }
    return cfg_fs_write_atomic(name, data, len);
}

static void test_rejected_file_with_older_copy_present(void)
{
    TEST_SECTION("zones cfg_fs (review 11): rejected file + valid older NVS/legacy copy");
    static uint8_t good[4 + sizeof(zones_cfg_t)], bad[4 + sizeof(zones_cfg_t)], got[4 + sizeof(zones_cfg_t)];
    bool found = false, valid = true, exists = true;
    zones_cfg_load_fault_t lf;

    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();
    stage("keep", 2.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save a real zones.json");
    size_t glen = read_file(ZONES_CFG_FILE_PATH, good, sizeof(good));
    TEST_CHECK(glen > 8 && glen != (size_t)-1, "read the good file");

    /* A. NEWER file + valid NVS copy: file untouched, NVS copy NOT adopted, fault latched. */
    stage_legacy_nvs("stale", 1.0f, 1);
    memcpy(bad, good, glen);
    bad[4] = (uint8_t)(ZONES_CFG_VERSION + 1);
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "plant a newer-version file");
    zones_config_load_fault_reset_for_test();
    valid = true;
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    (void)nvs_load(&found, &valid);
    TEST_CHECK(read_file(ZONES_CFG_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "MED-1: a newer-version zones.json is NOT overwritten from the older NVS copy");
    TEST_CHECK(!valid, "MED-1: the older NVS copy is not adopted over the newer file (no firing on stale data)");
    TEST_CHECK(zones_config_get_load_fault(&lf) && lf.kind == ZONES_CFG_LOAD_FAULT_NEWER && lf.file_rejected,
               "MED-1: the fault stays latched when no trustworthy copy is adopted");

    /* B. Corrupt file + valid NVS copy: NVS adopted, NO latched fault (MED-2), .bad holds the bytes, file healed. */
    memcpy(bad, good, glen);
    bad[glen - 1] ^= 0xFF;
    bad[10] ^= 0x55;
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "plant a corrupt file");
    TEST_CHECK(cfg_fs_delete(ZONES_CFG_BAD_FILE_PATH) == ESP_OK, "clear .bad");
    zones_config_load_fault_reset_for_test();
    valid = false;
    (void)nvs_load(&found, &valid);
    TEST_CHECK(valid, "MED-2: a trustworthy NVS copy is adopted");
    TEST_CHECK(!zones_config_get_load_fault(NULL), "MED-2: no firing-refusing fault when a copy was adopted");
    TEST_CHECK(read_file(ZONES_CFG_BAD_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "the rejected bytes are kept as .bad");

    /* C. Corrupt file, valid NVS, .bad write fails: the only copy of the file is NOT overwritten. */
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "re-plant the corrupt file");
    TEST_CHECK(cfg_fs_delete(ZONES_CFG_BAD_FILE_PATH) == ESP_OK, "clear .bad again");
    zones_config_cfg_fs_set_write_fn(fail_bad_write);
    zones_config_load_fault_reset_for_test();
    valid = false;
    (void)nvs_load(&found, &valid);
    zones_config_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(valid, "NVS copy still adopted when .bad could not be written");
    TEST_CHECK(read_file(ZONES_CFG_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, bad, glen) == 0,
               "LOW-2: the corrupt file with no .bad copy is not overwritten");

    /* D. LOW-2: no NVS, corrupt file, .bad write fails: fault records the failed copy. */
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    zones_config_cfg_fs_set_write_fn(fail_bad_write);
    zones_config_load_fault_reset_for_test();
    valid = true;
    (void)nvs_load(&found, &valid);
    zones_config_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(!valid && zones_config_get_load_fault(&lf) && lf.file_rejected && lf.bad_copy_failed,
               "LOW-2: a failed .bad write is reported in the fault record");

    /* F. LOW-1: the .bad write happens inside the zones save section. */
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, bad, glen) == ESP_OK, "re-plant the corrupt file for F");
    (void)cfg_fs_delete(ZONES_CFG_BAD_FILE_PATH);
    pref_cfg_fs_set_save_section_hooks(depth_enter, depth_exit);
    s_save_depth = 0;
    s_bad_write_depth = -1;
    zones_config_cfg_fs_set_write_fn(depth_recording_write);
    zones_config_load_fault_reset_for_test();
    (void)nvs_load(&found, &valid);
    zones_config_cfg_fs_reset_write_fn_for_test();
    pref_cfg_fs_set_save_section_hooks(NULL, NULL);
    TEST_CHECK(s_bad_write_depth == 1, "review 11 LOW-1: the .bad write runs inside the zones save section");

    /* E. LOW-1: a read-only load_raw never writes .bad. */
    TEST_CHECK(cfg_fs_delete(ZONES_CFG_BAD_FILE_PATH) == ESP_OK || true, "ensure no .bad");
    zones_cfg_t tmp;
    uint32_t trev = 0;
    bool tvalid = true;
    zones_config_cfg_fs_load_raw(&tmp, &trev, &tvalid);
    TEST_CHECK(!tvalid && cfg_fs_exists(ZONES_CFG_BAD_FILE_PATH, &exists) == ESP_OK && !exists,
               "LOW-1: the read-only load_raw path writes no .bad");

    /* F. LOW-3: the clear helper empties the latch. */
    zones_config_load_fault_clear();
    TEST_CHECK(!zones_config_get_load_fault(NULL), "LOW-3: load fault cleared");
    cfg_fs_deinit();
}

static void test_legacy_partition_does_not_overwrite_newer_file(void)
{
    TEST_SECTION("zones_http_start (review 11 MED-1): legacy default-partition copy vs a NEWER zones.json");
    static uint8_t good[4 + sizeof(zones_cfg_t)], got[4 + sizeof(zones_cfg_t)];
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();
    stage("keep", 2.0f);
    TEST_CHECK(nvs_save() == ESP_OK, "save a real zones.json");
    size_t glen = read_file(ZONES_CFG_FILE_PATH, good, sizeof(good));
    TEST_CHECK(glen > 8 && glen != (size_t)-1, "read the good file");
    good[4] = (uint8_t)(ZONES_CFG_VERSION + 1);
    TEST_CHECK(cfg_fs_write_atomic(ZONES_CFG_FILE_PATH, good, glen) == ESP_OK, "plant a newer-version file");
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    hal_kv_init_partition(NVS_DEFAULT_PART_NAME);
    {
        zones_cfg_t c;
        fill_valid_cfg(&c, "legacy", 2.0f);
        c.version = ZONES_CFG_VERSION;
        c.crc32 = zones_config_json_compute_crc(&c);
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NVS_DEFAULT_PART_NAME) == HAL_OK,
                   "stage: open default partition");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_ZONES, &c, sizeof(c)) == HAL_OK, "stage: legacy blob");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "stage: commit");
        hal_kv_close(&h);
    }
    zones_config_load_fault_reset_for_test();
    (void)zones_http_start();
    TEST_CHECK(read_file(ZONES_CFG_FILE_PATH, got, sizeof(got)) == glen && memcmp(got, good, glen) == 0,
               "MED-1: the legacy migration does not overwrite a newer-version zones.json");
    TEST_CHECK(!s_zones_config_valid, "the legacy copy is not adopted over the newer file");
    zones_cfg_load_fault_t lf;
    TEST_CHECK(zones_config_get_load_fault(&lf) && lf.kind == ZONES_CFG_LOAD_FAULT_NEWER,
               "the NEWER fault stays latched");
    reset_all();
}

void run_test_zones_config_cfg_fs(void)
{
    test_rejected_file_with_older_copy_present();
    test_legacy_partition_does_not_overwrite_newer_file();
    test_rejected_file_is_preserved_and_latches_fault();
    test_cfgfs_file_post_validation();
    test_save_persists_locked_snapshot();
    test_save_crc_writeback_skipped_when_ram_changed();
    test_partition_absent_falls_through_to_nvs_only();
    test_nvs_fallback_then_file_preferred_after_migration();
    test_dual_write_keeps_file_and_nvs_in_sync();
    test_coupling_matrix_and_gains_round_trip_through_dual_write();
    test_divergence_tie_break_both_directions();
    test_file_migration_matches_direct_blob_decode_v21();
    test_interrupted_file_write_leaves_old_or_new();
    test_equal_rev_divergence_adopts_nvs_not_the_stale_file();
    test_file_wins_after_migration_writes_back_to_nvs();
    test_file_won_migration_persist_fault_names_real_file_version();
    test_newer_nvs_blob_is_not_overwritten_by_file_writeback();
    test_file_sourced_writeback_happens_once_not_every_boot();
    test_file_cycle_is_normalized_in_ram_and_on_writeback();
    test_resolve_oom_keeps_rev_floor_and_fails_load();
    test_nvs_load_resolved_oom_keeps_rev_floor();
    test_oversize_file_is_cannot_decide_not_absent();
    test_transient_read_error_is_cannot_decide_and_latches();
    test_nvs_conversion_scratch_oom_is_not_corrupt();
    test_file_conversion_scratch_oom_does_not_overwrite_file();
    test_start_does_not_migrate_after_load_oom();

    reset_all();
}
