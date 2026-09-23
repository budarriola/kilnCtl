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
#include "zones_config_accessors.h" /* zones_config_get/set_pid/coupling*() -- real accessors,
                                      * defined by zones_http.c's textual #include of
                                      * zones_config_accessors.c in test_zones_http.c, this TU's
                                      * link-mate in the same executable. */
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

// ---------------------------------------------------------------------
// 8. The gap this pass closes: the `cfg` file wins the tie-break (here,
//    simplest case -- NVS has nothing trustworthy at all) carrying an
//    OLD-VERSION blob that only migrates in RAM during decode. Before this
//    fix, nvs_load() discarded the NVS side unconditionally whenever the
//    file won (CLAUDE.md/CONFIG_MIGRATION_CHAIN_PLAN.md section 1.5's
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
                 "must be written back to NVS too, not left RAM-only");
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

    // THE FIX: NVS must now actually hold the migrated, current-version
    // blob -- read back from flash, never trust an in-RAM assertion alone
    // (same discipline zones_config_persist_migrated_blob_verified() itself
    // uses).
    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    TEST_CHECK(open_err == HAL_OK, "NVS opened read-only for verification");
    uint8_t readback[sizeof(zones_cfg_t)];
    size_t readback_len = sizeof(readback);
    hal_status_t rb_err = HAL_IO;
    uint32_t nvs_rev_after = 0;
    if (open_err == HAL_OK) {
        rb_err = hal_kv_get_blob(&h, NVS_KEY_ZONES, readback, &readback_len);
        (void)hal_kv_get_u32(&h, "zones_rev", &nvs_rev_after);
        hal_kv_close(&h);
    }
    TEST_CHECK(rb_err == HAL_OK, "THE FIX: the migrated blob must now be readable back from NVS -- before this "
                                 "pass, a file-sourced migration was never persisted to NVS at all");
    TEST_CHECK(readback_len == sizeof(zones_cfg_t), "the persisted NVS blob is full current-version size");
    TEST_CHECK(readback_len == sizeof(zones_cfg_t) && readback[0] == ZONES_CFG_VERSION,
               "THE FIX: NVS holds the migrated CURRENT version, not left empty/stale for a future one-step "
               "migration to choke on");
    TEST_CHECK(nvs_rev_after > 7, "the dual-write rev counter advanced past the file's staged rev 7 once the "
                                 "write-back ran");

    // Both copies must now agree: the file itself (already migrated, so
    // this also confirms the write-back's nvs_save() call re-wrote the file
    // at the new rev rather than leaving it at the old rev-7/v21 bytes).
    zones_cfg_t file_raw;
    uint32_t file_rev_after = 0;
    bool file_raw_valid = false;
    zones_config_cfg_fs_load_raw(&file_raw, &file_rev_after, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev_after == nvs_rev_after,
               "file and NVS report the SAME rev after the write-back -- no lingering divergence");
    zones_cfg_t nvs_readback_cfg;
    memcpy(&nvs_readback_cfg, readback, sizeof(nvs_readback_cfg)); /* properly aligned copy, never a cast of
                                                                     * the raw byte buffer -- see
                                                                     * zones_config_cfg_fs.c's own alignment
                                                                     * comment on why that matters. */
    TEST_CHECK(memcmp(&file_raw, &nvs_readback_cfg, sizeof(file_raw)) == 0,
               "file and NVS hold byte-identical content after the write-back");
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
static uint32_t read_nvs_zones_rev(void)
{
    hal_kv_handle_t h;
    uint32_t rev = 0;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK) {
        (void)hal_kv_get_u32(&h, "zones_rev", &rev);
        hal_kv_close(&h);
    }
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
    uint32_t rev_after_first = read_nvs_zones_rev();
    TEST_CHECK(rev_after_first > 9, "the write-back ran on the first load (rev advanced past the file's 9)");

    uint8_t blob_after_first[sizeof(zones_cfg_t)];
    memset(blob_after_first, 0, sizeof(blob_after_first));
    size_t len1 = sizeof(blob_after_first);
    hal_kv_handle_t h1;
    TEST_CHECK(hal_kv_open(&h1, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "NVS opened after the first load");
    TEST_CHECK(hal_kv_get_blob(&h1, NVS_KEY_ZONES, blob_after_first, &len1) == HAL_OK,
               "NVS holds the written-back blob after the first load");
    hal_kv_close(&h1);

    // SECOND boot: same on-flash state, in-RAM struct wiped. Both sides now
    // decode to the same bytes, so nothing must be written.
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
    found = false;
    valid = false;
    TEST_CHECK(nvs_load(&found, &valid) == ESP_OK && found && valid, "second load succeeds");
    TEST_CHECK(strcmp(s_zones.cfg.zones[0].name, "OnceOnly") == 0 && s_zones.cfg.zones[0].pid_kp == 2.75f,
               "second load still yields the same config");
    TEST_CHECK(read_nvs_zones_rev() == rev_after_first,
               "THE CONVERGENCE GUARD: the rev counter did NOT advance on the second load -- no nvs_save(), "
               "so no write every boot (flash wear / reset-one-side class)");

    uint8_t blob_after_second[sizeof(zones_cfg_t)];
    memset(blob_after_second, 0, sizeof(blob_after_second));
    size_t len2 = sizeof(blob_after_second);
    hal_kv_handle_t h2;
    TEST_CHECK(hal_kv_open(&h2, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "NVS opened after the second load");
    TEST_CHECK(hal_kv_get_blob(&h2, NVS_KEY_ZONES, blob_after_second, &len2) == HAL_OK, "NVS still readable");
    hal_kv_close(&h2);
    TEST_CHECK(len1 == len2 && memcmp(blob_after_first, blob_after_second, len1) == 0,
               "the NVS blob is byte-identical across the second load -- nothing was rewritten");
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
                 "both in RAM and in the NVS blob the file-won migration path writes back");
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
    // ALREADY-NORMALIZED struct, not the raw cyclic bytes -- read NVS back
    // directly (never through nvs_load_from(), which would normalize AGAIN
    // on ITS OWN decode and mask an unfixed bug in the file-side path).
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "NVS opened read-only for verification");
    uint8_t readback[sizeof(zones_cfg_t)];
    memset(readback, 0, sizeof(readback));
    size_t readback_len = sizeof(readback);
    hal_status_t rb_err = hal_kv_get_blob(&h, NVS_KEY_ZONES, readback, &readback_len);
    hal_kv_close(&h);
    TEST_CHECK(rb_err == HAL_OK && readback_len == sizeof(zones_cfg_t), "the write-back landed in NVS");
    zones_cfg_t nvs_cfg;
    memcpy(&nvs_cfg, readback, sizeof(nvs_cfg));
    uint8_t nvs_s0 = nvs_cfg.zones[0].settings_source[SRC_GROUP_LIMITS];
    uint8_t nvs_s1 = nvs_cfg.zones[1].settings_source[SRC_GROUP_LIMITS];
    TEST_CHECK(nvs_s0 != 1 || nvs_s1 != 0,
              "THE FIX: the NVS blob written back by the file-won migration path does NOT hold the "
              "un-normalized cycle -- before this fix, the raw cyclic bytes were persisted verbatim and "
              "only converged on NVS's own next decode, one boot later");
}

void run_test_zones_config_cfg_fs(void)
{
    test_partition_absent_falls_through_to_nvs_only();
    test_nvs_fallback_then_file_preferred_after_migration();
    test_dual_write_keeps_file_and_nvs_in_sync();
    test_coupling_matrix_and_gains_round_trip_through_dual_write();
    test_divergence_tie_break_both_directions();
    test_file_migration_matches_direct_blob_decode_v21();
    test_interrupted_file_write_leaves_old_or_new();
    test_equal_rev_divergence_adopts_nvs_not_the_stale_file();
    test_file_wins_after_migration_writes_back_to_nvs();
    test_newer_nvs_blob_is_not_overwritten_by_file_writeback();
    test_file_sourced_writeback_happens_once_not_every_boot();
    test_file_cycle_is_normalized_in_ram_and_on_writeback();

    reset_all();
}
