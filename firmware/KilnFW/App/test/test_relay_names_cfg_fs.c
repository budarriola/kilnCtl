// Host tests for relay names' cfg_fs dual-write bridge -- item 3,
// docs/FILESYSTEM_USER_DATA.md section 5 step 5 close-out. Unlike zones
// config (a bespoke 22-version migration chain), relay names is a small
// fixed-size struct with no migration chain, so it reuses the SAME generic
// pref_cfg_fs.h bridge unit_pref.c/time_sync.c share -- see
// zones_config_store.c's relay_names_load()/relay_names_save() and
// pref_cfg_fs.h's "WHY GENERIC" for the full design.
//
// Linked as a SEPARATE translation unit into the "zones_http" host-test
// executable (build_host_tests.ps1's $cmd2) alongside test_zones_http.c,
// which #includes zones_config_store.c (defining the real, non-static
// relay_names_load()/relay_names_save()/s_relay_names) directly -- this file
// reaches those same symbols the ordinary way (declared via
// zones_http_internal.h, defined in the other TU, resolved at link time).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TRNCF_MKDIR(p) _mkdir(p)
#define TRNCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TRNCF_MKDIR(p) mkdir((p), 0755)
#define TRNCF_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "hal_kv.h"

#include "cfg_fs.h"
#include "pref_cfg_fs.h"
#include "esp_crc.h" /* esp_crc32_le() -- test fixture below needs the SAME crc primitive
                        * compute_relay_names_crc() (zones_config_store.c, file-scope-static
                        * so not callable directly from this TU) uses. */
#include "zones_http_internal.h" /* s_relay_names, relay_names_load()/relay_names_save() */

static const char *SCRATCH_BASE = "cfg_fs_test_relay_names";

static void reset_all(void)
{
    /* Same "delete known filenames before rmdir" fix as
     * test_zones_config_cfg_fs.c's reset_all() -- a leftover file from a
     * prior run of this test binary defeats a bare rmdir(). */
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", SCRATCH_BASE, RELAY_NAMES_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, RELAY_NAMES_FILE_PATH);
    remove(path);

    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", SCRATCH_BASE);
    TRNCF_RMDIR(tmp);
    TRNCF_RMDIR(SCRATCH_BASE);
    TRNCF_MKDIR(SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
}

/* zones_config_store.c's dual-write rev counter (s_relay_names_rev) is a
 * process-wide static in the OTHER translation unit (test_zones_http.c's
 * #include of zones_config_store.c) -- reset_all() cannot see or clear it
 * directly. Same convention as test_zones_config_cfg_fs.c's
 * prime_rev_to_zero(): call the real relay_names_load() once against a
 * guaranteed-empty NVS + freshly (re)mounted, empty file, which resolves
 * rev to 0 and adopts it, exactly the boot-time behavior this mirrors. Call
 * AFTER reset_all() so both sides really are empty when this runs. */
static void prime_rev_to_zero(void)
{
    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
}

/* Stages what a LEGACY (pre dual-write-close) firmware left in NVS: a
 * production-shaped v2 blob (one name in relay 1) plus its rev key. Nothing in
 * the firmware writes these keys any more. */
static void put_v2_nvs_blob(const char *name1, uint32_t rev)
{
    relay_names_cfg_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.version = RELAY_NAMES_CFG_VERSION;
    strncpy(v2.names[0], name1, RELAY_NAME_MAX_LEN);
    relay_names_cfg_t crc_tmp = v2;
    crc_tmp.crc32 = 0;
    v2.crc32 = esp_crc32_le(0, (const uint8_t *)&crc_tmp, sizeof(crc_tmp));
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "test setup: open NVS read-write");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_RELAY_NAMES, &v2, sizeof(v2)) == HAL_OK, "test setup: v2 blob");
    TEST_CHECK(hal_kv_set_u32(&h, NVS_KEY_RELAY_NAMES_REV, rev) == HAL_OK, "test setup: rev");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "test setup: NVS commit");
    hal_kv_close(&h);
}

/* true when the relay-names blob key is absent from NVS (never written). */
static bool nvs_relay_names_absent(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return true;
    }
    relay_names_cfg_t raw;
    size_t len = sizeof(raw);
    hal_status_t g = hal_kv_get_blob(&h, NVS_KEY_RELAY_NAMES, &raw, &len);
    hal_kv_close(&h);
    return g != HAL_OK;
}

static void set_name(uint8_t relay1based, const char *name)
{
    strncpy(s_relay_names.cfg.names[relay1based - 1], name, RELAY_NAME_MAX_LEN);
    s_relay_names.cfg.names[relay1based - 1][RELAY_NAME_MAX_LEN] = '\0';
}

// ---------------------------------------------------------------------
// 1. Partition absent: save/load must behave exactly like plain NVS.
// ---------------------------------------------------------------------
static void test_partition_absent_falls_through_to_nvs_only(void)
{
    TEST_SECTION("relay names cfg_fs: partition absent -- a legacy NVS copy still loads; save fails loud, no NVS "
                 "fallback");
    reset_all();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    put_v2_nvs_blob("Legacy Name", 1);
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "Legacy Name") == 0,
               "the legacy NVS-only copy still loads with no `cfg` partition");

    set_name(1, "Top Element");
    TEST_CHECK(relay_names_save() != ESP_OK, "relay_names_save fails loud with no `cfg` partition mounted");

    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "Legacy Name") == 0,
               "the NVS copy was NOT overwritten by the failed save -- there is no NVS fallback");
}

// ---------------------------------------------------------------------
// 2. File-preferred read + NVS fallback/migration.
// ---------------------------------------------------------------------
static void test_nvs_fallback_then_file_preferred_after_migration(void)
{
    TEST_SECTION("relay names cfg_fs: a save lands in the file only; a reload prefers the file");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    set_name(1, "Zone1");
    set_name(2, "Zone2");
    TEST_CHECK(relay_names_save() == ESP_OK, "relay_names_save succeeds (cfg file only)");
    TEST_CHECK(nvs_relay_names_absent(), "NVS was never written -- the dual-write window is closed");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(RELAY_NAMES_FILE_PATH, &exists) == ESP_OK && exists,
               "the save actually created the file");

    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "Zone1") == 0 && strcmp(s_relay_names.cfg.names[1], "Zone2") == 0,
               "reloaded names match what was saved");

    relay_names_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &raw_valid);
    // NULL validator here would accept anything -- re-run manually via a
    // second, correctly-validated load to prove the FILE itself, not just
    // NVS, is what round-tripped.
    TEST_CHECK(raw_valid && rev == 1, "the file holds a rev-1 copy after one save");
    TEST_CHECK(strcmp(raw.names[0], "Zone1") == 0, "file content matches the saved names");
}

// ---------------------------------------------------------------------
// 3. Dual-write sync across repeated saves.
// ---------------------------------------------------------------------
static void test_dual_write_keeps_file_and_nvs_in_sync(void)
{
    TEST_SECTION("relay names cfg_fs: repeated saves advance the file rev; NVS is never written");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    set_name(1, "v1");
    TEST_CHECK(relay_names_save() == ESP_OK, "save 1");
    set_name(1, "v2");
    TEST_CHECK(relay_names_save() == ESP_OK, "save 2");
    set_name(1, "v3");
    TEST_CHECK(relay_names_save() == ESP_OK, "save 3");

    relay_names_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 3, "file rev tracks three saves");
    TEST_CHECK(strcmp(raw.names[0], "v3") == 0, "file holds the LATEST save");

    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "v3") == 0, "the reload resolves to the latest file");
    TEST_CHECK(nvs_relay_names_absent(), "NVS still untouched after three saves");
}

// ---------------------------------------------------------------------
// 4. Divergence tie-break, both directions.
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
    TEST_SECTION("relay names cfg_fs: a failed file write is reported, leaves the file at the old rev, and the "
                 "next save reuses the unadvanced rev; a higher-rev legacy NVS copy still wins a load");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    set_name(1, "rev1");
    TEST_CHECK(relay_names_save() == ESP_OK, "rev 1 saved to the file");

    pref_cfg_fs_set_write_fn(failing_write_fn);
    set_name(1, "rev2_failed");
    TEST_CHECK(relay_names_save() != ESP_OK, "rev 2 save reports the cfg write failure");
    pref_cfg_fs_reset_write_fn_for_test();

    relay_names_cfg_t raw_before;
    uint32_t rev_before = 0;
    bool raw_valid_before = false;
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw_before), NULL, &raw_before, &rev_before,
                          &raw_valid_before);
    TEST_CHECK(raw_valid_before && rev_before == 1 && strcmp(raw_before.names[0], "rev1") == 0,
               "file is stuck at rev 1 -- the failed write never landed");
    TEST_CHECK(nvs_relay_names_absent(), "nothing fell back to NVS");

    set_name(1, "rev2_retry");
    TEST_CHECK(relay_names_save() == ESP_OK, "the retry succeeds");
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw_before), NULL, &raw_before, &rev_before,
                          &raw_valid_before);
    TEST_CHECK(raw_valid_before && rev_before == 2 && strcmp(raw_before.names[0], "rev2_retry") == 0,
               "the retry reused the unadvanced rev: file is at rev 2, not 3");

    /* A legacy NVS copy with a strictly higher rev (rolled-back firmware kept
     * saving) wins the tie-break and is migrated into the file. */
    put_v2_nvs_blob("nvs_rev9", 9);
    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "nvs_rev9") == 0, "the higher-rev NVS copy wins");
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw_before), NULL, &raw_before, &rev_before,
                          &raw_valid_before);
    TEST_CHECK(raw_valid_before && strcmp(raw_before.names[0], "nvs_rev9") == 0 && rev_before >= 9,
               "and it was migrated into the file at the NVS rev");
}

// ---------------------------------------------------------------------
// 5. EQUAL revs with differing content adopt NVS (the rollback round trip).
// ---------------------------------------------------------------------
static void test_equal_rev_divergence_adopts_nvs_not_the_stale_file(void)
{
    TEST_SECTION("relay names cfg_fs: equal revs with differing content adopt NVS (rolled-back-firmware edit "
                 "survives a roll-forward)");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    set_name(1, "before_rollback");
    TEST_CHECK(relay_names_save() == ESP_OK, "rev 1 written to the file");

    // Act like firmware that predates this change: rewrite ONLY the NVS
    // blob, leaving NVS_KEY_RELAY_NAMES_REV at 1 and the file at rev 1/old
    // content.
    relay_names_cfg_t rolled_back;
    memset(&rolled_back, 0, sizeof(rolled_back));
    rolled_back.version = RELAY_NAMES_CFG_VERSION;
    strncpy(rolled_back.names[0], "rolled_edit", RELAY_NAME_MAX_LEN);
    // crc32 recomputed the same way compute_relay_names_crc() does --
    // duplicated here (rather than calling that static function, which is
    // not exported outside its TU) purely so this fixture is a
    // production-shaped blob relay_names_load()'s CRC gate will accept.
    relay_names_cfg_t tmp = rolled_back;
    tmp.crc32 = 0;
    rolled_back.crc32 = esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));

    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "test setup: NVS opened for the pre-dual-write-style blob rewrite");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_RELAY_NAMES, &rolled_back, sizeof(rolled_back)) == HAL_OK,
                   "test setup: NVS blob staged, as older firmware that still dual-wrote would have left it");
        TEST_CHECK(hal_kv_set_u32(&h, NVS_KEY_RELAY_NAMES_REV, 1) == HAL_OK, "test setup: NVS rev key at 1");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "test setup: NVS commit");
        hal_kv_close(&h);
    }

    // Fixture check: really is the equal-rev case.
    relay_names_cfg_t file_raw;
    uint32_t file_rev = 0;
    bool file_raw_valid = false;
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(file_raw), NULL, &file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 1 && strcmp(file_raw.names[0], "before_rollback") == 0,
               "fixture check: the file still holds the OLD content at rev 1");
    {
        hal_kv_handle_t h;
        uint32_t nvs_rev = 0;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
                   "fixture check: NVS opened read-only");
        TEST_CHECK(hal_kv_get_u32(&h, NVS_KEY_RELAY_NAMES_REV, &nvs_rev) == HAL_OK && nvs_rev == 1,
                   "fixture check: rev is still 1 -- file_rev == nvs_rev, the EQUAL-rev case");
        hal_kv_close(&h);
    }

    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "rolled_edit") == 0,
               "NVS wins the EQUAL-rev tie -- the edit made on rolled-back firmware is NOT discarded in favour of "
               "the stale file");

    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(file_raw), NULL, &file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && strcmp(file_raw.names[0], "rolled_edit") == 0,
               "the losing file was resynced from NVS, so the divergence does not persist across boots");
}

// ---------------------------------------------------------------------
// 6. Interrupted write leaves old-or-new.
// ---------------------------------------------------------------------
static void test_interrupted_file_write_leaves_old_or_new(void)
{
    TEST_SECTION("relay names cfg_fs: an interrupted file write leaves the OLD committed names intact");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    set_name(1, "committed");
    TEST_CHECK(relay_names_save() == ESP_OK, "an initial, fully-committed save lands");

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp/%s", SCRATCH_BASE, RELAY_NAMES_FILE_PATH);
    FILE *f = fopen(tmp_path, "wb");
    TEST_CHECK(f != NULL, "test setup: orphaned temp file created");
    if (f) {
        static const char partial[] = "not even close to a valid rev+blob";
        fwrite(partial, 1, sizeof(partial), f);
        fclose(f);
    }

    relay_names_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 1 && strcmp(raw.names[0], "committed") == 0,
               "the OLD committed names read back untouched -- the orphaned temp file is invisible through the "
               "real read path");
}

// ---------------------------------------------------------------------
// 7. Mount failed falls through to NVS.
// ---------------------------------------------------------------------
static void test_mount_failed_falls_through_to_nvs_only(void)
{
    TEST_SECTION("relay names cfg_fs: cfg_fs mount FAILED -- a legacy NVS copy still loads, non-fatal");
    reset_all();

    put_v2_nvs_blob("unmounted_case", 1);

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();
    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "unmounted_case") == 0,
               "mount-failed: the NVS value is still adopted correctly");
    set_name(1, "cannot_persist");
    TEST_CHECK(relay_names_save() != ESP_OK, "and a save fails loud");

    cfg_fs_deinit();
}

// ---------------------------------------------------------------------
// 8. A v1-LENGTH file is migrated, not silently ignored.
//
// The generic bridge is parameterized by ONE fixed item size, so it can only
// ever see a v2-length file: a v1-length one fails pref_cfg_fs_load_raw()'s
// `len != 4 + item_size` check and is dropped without a word. For relay
// names that would mean losing the names a v1 board had dual-written, which
// is the same data-loss hazard the NVS-side migration exists to close --
// just through the other door. relay_names_load() runs a v1 file pre-pass to
// cover it; this test is what proves that pre-pass is real.
// ---------------------------------------------------------------------
static void test_v1_file_is_migrated_not_ignored(void)
{
    TEST_SECTION("relay names cfg_fs: a v1-LENGTH file is migrated to v2 in place, names preserved, rather than "
                 "being dropped by the fixed-item-size bridge");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();

    /* A production-shaped v1 file at rev 5, with NVS left empty -- so the
     * file is the ONLY place these names exist. If the pre-pass does not
     * work, they are gone. */
    relay_names_cfg_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1;
    strncpy(v1.names[0], "FileOnlyName", RELAY_NAME_MAX_LEN);
    strncpy(v1.names[2], "ThirdRelay", RELAY_NAME_MAX_LEN);
    /* Same crc primitive compute_relay_names_crc_v1() uses (that function is
     * file-scope-static in the other TU), over the frozen v1 layout. */
    relay_names_cfg_v1_t crc_tmp = v1;
    crc_tmp.crc32 = 0;
    v1.crc32 = esp_crc32_le(0, (const uint8_t *)&crc_tmp, sizeof(crc_tmp));

    TEST_CHECK(pref_cfg_fs_save(RELAY_NAMES_FILE_PATH, &v1, sizeof(v1), 5) == ESP_OK,
               "test setup: a v1-length file is written at rev 5");

    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));
    relay_names_load();

    TEST_CHECK(strcmp(s_relay_names.cfg.names[0], "FileOnlyName") == 0,
               "the v1 file's name must survive the migration -- it exists nowhere else");
    TEST_CHECK(strcmp(s_relay_names.cfg.names[2], "ThirdRelay") == 0,
               "every populated slot survives, not just the first");
    TEST_CHECK(s_relay_names.cfg.version == RELAY_NAMES_CFG_VERSION, "the adopted config is stamped v2");
    TEST_CHECK(s_relay_names.cfg.types[0] == 0 && s_relay_names.cfg.types[2] == 0,
               "every migrated device type defaults to UNSET (0)");

    /* And the file itself was rewritten at v2 length, at the SAME rev -- so
     * the upgrade is durable and does not disturb the tie-break. */
    relay_names_cfg_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    pref_cfg_fs_load_raw(RELAY_NAMES_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid, "the file now reads back at the v2 item size -- it was rewritten, not left v1");
    TEST_CHECK(rev == 5, "the in-place upgrade preserves the rev, so the file/NVS tie-break is unaffected");
    TEST_CHECK(strcmp(raw.names[0], "FileOnlyName") == 0, "the rewritten v2 file carries the migrated names");
}

/* Overwrites the NVS relay-names blob with a production-shaped v1 blob (one
 * name in relay 1, valid v1 CRC) at the given rev, leaving the file alone. */
static void put_v1_nvs_blob(const char *name1, uint32_t rev)
{
    relay_names_cfg_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1;
    strncpy(v1.names[0], name1, RELAY_NAME_MAX_LEN);
    relay_names_cfg_v1_t crc_tmp = v1;
    crc_tmp.crc32 = 0;
    v1.crc32 = esp_crc32_le(0, (const uint8_t *)&crc_tmp, sizeof(crc_tmp));
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "test setup: open NVS read-write");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_RELAY_NAMES, &v1, sizeof(v1)) == HAL_OK, "test setup: v1 blob");
    TEST_CHECK(hal_kv_set_u32(&h, NVS_KEY_RELAY_NAMES_REV, rev) == HAL_OK, "test setup: rev");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "test setup: NVS commit");
    hal_kv_close(&h);
}

// ---------------------------------------------------------------------
// 9. GET /api/cfgfs "relay_names" row accessor.
// ---------------------------------------------------------------------
static void test_dualwrite_status_row(void)
{
    TEST_SECTION("relay names cfg_fs: relay_names_get_dualwrite_status() reports presence, revs, divergence");
    bool fv = true, nv = true, dv = true;
    uint32_t fr = 99, nr = 99;

    reset_all();
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(!fv && !nv && fr == 0 && nr == 0 && !dv, "nothing saved: both sides absent, not diverged");

    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    prime_rev_to_zero();
    set_name(1, "A");
    TEST_CHECK(relay_names_save() == ESP_OK, "save 1");
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && !nv && fr == 1 && nr == 0 && !dv, "after one save: file valid at rev 1, no NVS copy, not diverged");

    pref_cfg_fs_set_write_fn(failing_write_fn);
    set_name(1, "B");
    TEST_CHECK(relay_names_save() != ESP_OK, "save 2 reports the file write failure");
    pref_cfg_fs_reset_write_fn_for_test();
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && !nv && fr == 1 && !dv, "file stuck at rev 1, still no NVS copy: not diverged");

    relay_names_get_dualwrite_status(NULL, NULL, NULL, NULL, NULL);
    TEST_CHECK(true, "NULL out-params do not crash");

    /* Legacy NVS copy at the SAME rev with different names: diverged. */
    put_v2_nvs_blob("LegacyDiffers", 1);
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && nv && fr == 1 && nr == 1 && dv, "equal rev, different content: diverged");

    cfg_fs_deinit();
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(!fv && nv && nr == 1 && !dv, "unmounted: file side absent, NVS valid, not diverged");

    /* v1 NVS blob (a board that never re-saved since the v2 bump) against a
     * v2 file. The status read decodes the v1 blob in memory (and stays
     * silent about it) so a migrated-equivalent file reads in sync, and a
     * different one reads diverged. */
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts (v1 NVS cases)");
    prime_rev_to_zero();
    set_name(1, "SameName");
    TEST_CHECK(relay_names_save() == ESP_OK, "v2 file saved at rev 1");
    put_v1_nvs_blob("SameName", 1);
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && nv && fr == 1 && nr == 1 && !dv, "v1 NVS blob vs migrated-equivalent v2 file: valid, in sync");

    put_v1_nvs_blob("OtherName", 1);
    relay_names_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && nv && dv, "v1 NVS blob with different names vs v2 file: diverged");
    cfg_fs_deinit();
}

void run_test_relay_names_cfg_fs(void)
{
    test_v1_file_is_migrated_not_ignored();
    test_partition_absent_falls_through_to_nvs_only();
    test_nvs_fallback_then_file_preferred_after_migration();
    test_dual_write_keeps_file_and_nvs_in_sync();
    test_divergence_tie_break_both_directions();
    test_equal_rev_divergence_adopts_nvs_not_the_stale_file();
    test_interrupted_file_write_leaves_old_or_new();
    test_mount_failed_falls_through_to_nvs_only();
    test_dualwrite_status_row();

    reset_all();
}
