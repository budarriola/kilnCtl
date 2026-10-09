// Host tests for zone normals' cfg_fs dual-write bridge -- docs/CONFIG_FILESYSTEM.md
// item 2. Same generic pref_cfg_fs.h bridge as relay names (see
// test_relay_names_cfg_fs.c); linked as a separate TU into the "zones_http"
// host-test executable, reaching zones_config_store.c's real
// zone_normals_load()/zone_normals_set() through zones_http_internal.h.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TZNCF_MKDIR(p) _mkdir(p)
#define TZNCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TZNCF_MKDIR(p) mkdir((p), 0755)
#define TZNCF_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"
#include "hal_kv.h"

#include "esp_crc.h"

#include "cfg_fs.h"
#include "kiln_scope_cfg_files.h"
#include "pref_cfg_fs.h"
#include "firing_stats_cfg_fs.h"
#include "profiles_cfg_fs.h"
#include "profiles_scope_cfg_files.h"
#include "zones_http_internal.h"

static const char *SCRATCH_BASE = "cfg_fs_test_zone_normals";

/* Ids the profiles-scope test writes for both owners; reset_all() removes them
 * (and the decoys) so an aborted earlier run cannot leave files or
 * subdirectories behind. */
static const uint8_t kProfilesTestIds[] = { 0, 7, PROFILES_MAX_COUNT - 1, 150, 255 };
static const char *const kProfilesTestDecoys[] = {
    "profiles/hidden.json", "profiles/other.json", "stats/readme.dat", "profiles/prof07.json", "zones.json",
};

static void remove_profiles_test_files(void)
{
    char path[700];
    char rel[64];
    for (size_t i = 0; i < sizeof(kProfilesTestIds); i++) {
        snprintf(rel, sizeof(rel), PROFILES_CFG_FS_PATH_FMT, (unsigned)kProfilesTestIds[i]);
        snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, rel);
        remove(path);
        snprintf(rel, sizeof(rel), FIRING_STATS_CFG_FS_PATH_FMT, (unsigned)kProfilesTestIds[i]);
        snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, rel);
        remove(path);
    }
    for (size_t i = 0; i < sizeof(kProfilesTestDecoys) / sizeof(kProfilesTestDecoys[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, kProfilesTestDecoys[i]);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, PROFILES_CFG_FS_DIR);
    TZNCF_RMDIR(path);
    snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, FIRING_STATS_CFG_FS_DIR);
    TZNCF_RMDIR(path);
}

static void reset_all(void)
{
    char path[600];
    remove_profiles_test_files();
    snprintf(path, sizeof(path), "%s/.tmp/%s", SCRATCH_BASE, ZONE_NORMALS_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", SCRATCH_BASE, ZONE_NORMALS_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", SCRATCH_BASE);
    TZNCF_RMDIR(tmp);
    TZNCF_RMDIR(SCRATCH_BASE);
    TZNCF_MKDIR(SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    zone_normals_load(); /* resets the in-RAM record and rev counter to blank */
}

static bool normal_of(uint8_t zone, float *amps)
{
    bool measured = false;
    return zones_config_get_normal_current(zone, amps, &measured) && measured;
}

static void test_unmounted_uses_nvs_only(void)
{
    TEST_SECTION("zone normals cfg_fs: cfg_fs unmounted -- save/load behave exactly like NVS-only");
    reset_all();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted");
    TEST_CHECK(zone_normals_set(1, 7.5f), "zone_normals_set succeeds with no `cfg` partition");
    zone_normals_load();
    float a = 0;
    TEST_CHECK(normal_of(1, &a) && a == 7.5f, "value reloaded from NVS alone");
}

static void test_save_writes_both(void)
{
    TEST_SECTION("zone normals cfg_fs: save writes the file and NVS");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    TEST_CHECK(zone_normals_set(0, 3.25f), "save");
    zone_normals_cfg_t raw;
    uint32_t rev = 0;
    bool ok = false;
    pref_cfg_fs_load_raw(ZONE_NORMALS_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &ok);
    TEST_CHECK(ok && rev == 1 && raw.normal_current_a[0] == 3.25f, "file holds rev 1 with the saved normal");
    hal_kv_handle_t h;
    uint8_t blob[sizeof(zone_normals_cfg_t)];
    size_t len = sizeof(blob);
    uint32_t nrev = 0;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK, "open NVS");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_ZONE_NORMALS, blob, &len) == HAL_OK && len == sizeof(blob),
               "NVS blob present");
    TEST_CHECK(hal_kv_get_u32(&h, NVS_KEY_ZONE_NORMALS_REV, &nrev) == HAL_OK && nrev == 1, "NVS rev is 1");
    hal_kv_close(&h);
}

static void test_nvs_empty_file_present_resolves_from_file(void)
{
    TEST_SECTION("zone normals cfg_fs: NVS empty, file present -- resolves from the file");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    TEST_CHECK(zone_normals_set(2, 11.0f), "save dual-writes");
    fake_kv_reset_all(); /* NVS wiped, file survives */
    hal_kv_init_partition(KILN_NVS_PARTITION);
    zone_normals_load();
    float a = 0;
    TEST_CHECK(normal_of(2, &a) && a == 11.0f, "normal restored from the file with NVS empty");
}

static esp_err_t failing_write_fn(const char *p, const void *d, size_t n)
{
    (void)p;
    (void)d;
    (void)n;
    return ESP_FAIL;
}

static void test_both_present_higher_rev_wins(void)
{
    TEST_SECTION("zone normals cfg_fs: both present and diverged -- higher rev wins, loser resynced");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    TEST_CHECK(zone_normals_set(0, 1.0f), "rev 1 on both sides");
    pref_cfg_fs_set_write_fn(failing_write_fn);
    TEST_CHECK(zone_normals_set(0, 2.0f), "rev 2 lands in NVS only; file write fails but save still OK");
    pref_cfg_fs_reset_write_fn_for_test();
    zone_normals_load();
    float a = 0;
    TEST_CHECK(normal_of(0, &a) && a == 2.0f, "NVS (rev 2) beats the stale file (rev 1)");
    zone_normals_cfg_t raw;
    uint32_t rev = 0;
    bool ok = false;
    pref_cfg_fs_load_raw(ZONE_NORMALS_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &ok);
    TEST_CHECK(ok && rev == 2 && raw.normal_current_a[0] == 2.0f, "file resynced to rev 2");
}

static void test_future_version_file_rejected(void)
{
    TEST_SECTION("zone normals cfg_fs: file with valid rev+CRC but version=2 is rejected by the validator");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    zone_normals_cfg_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.version = 2;
    bad.normal_current_a[0] = 9.0f;
    bad.measured_mask = 1;
    /* Valid CRC (computed exactly as compute_zone_normals_crc() does: over the
     * struct with crc32 == 0) so ONLY the version check can reject this file. */
    bad.crc32 = 0;
    bad.crc32 = esp_crc32_le(0, (const uint8_t *)&bad, sizeof(bad));
    TEST_CHECK(pref_cfg_fs_save(ZONE_NORMALS_FILE_PATH, &bad, sizeof(bad), 5) == ESP_OK,
               "setup: wrote a version-2 file at rev 5 (framing and CRC valid)");
    zone_normals_load();
    float a = 0;
    TEST_CHECK(!normal_of(0, &a), "zone 0 stays unmeasured: stale-version file not adopted, NVS empty");
}

static void test_nvs_failure_does_not_advance_rev(void)
{
    TEST_SECTION("zone normals cfg_fs: NVS write failure leaves the RAM rev unadvanced");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    TEST_CHECK(zone_normals_set(0, 1.0f), "rev 1 saved");
    fake_kv_script_next_write_status(HAL_IO);
    TEST_CHECK(!zone_normals_set(0, 2.0f), "save reports failure when NVS write fails");
    TEST_CHECK(zone_normals_set(0, 3.0f), "next save succeeds");
    zone_normals_cfg_t raw;
    uint32_t rev = 0;
    bool ok = false;
    pref_cfg_fs_load_raw(ZONE_NORMALS_FILE_PATH, sizeof(raw), NULL, &raw, &rev, &ok);
    TEST_CHECK(ok && rev == 2, "RAM rev did not advance on the failed save: next save is rev 2, not 3");
}

static void test_kiln_reset_leaves_blank(void)
{
    TEST_SECTION("zone normals cfg_fs: kiln-scope reset deletes the cfg files, so load comes back blank");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    TEST_CHECK(zone_normals_set(1, 6.0f), "save dual-writes");
    fake_kv_reset_all(); /* what erasing kiln_nvs does */
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(kiln_scope_cfg_files_delete(NULL) == ESP_OK, "kiln-scope cfg delete succeeds");
    zone_normals_load();
    float a = 0;
    TEST_CHECK(!normal_of(1, &a), "no stale normal resurrected from the file");
}

static void test_kiln_scope_deletes_every_listed_file(void)
{
    TEST_SECTION("kiln scope cfg files: every kiln_nvs mirror is deleted, other files survive");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    size_t n = 0;
    const char *const *paths = kiln_scope_cfg_files_list(&n);
    TEST_CHECK(n >= 11, "the list covers every kiln_nvs mirror (11 today; tools/check_kiln_scope_cfg_mirrors.ps1 enforces completeness)");
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (size_t i = 0; i < n; i++) {
        TEST_CHECK(cfg_fs_write_atomic(paths[i], payload, sizeof(payload)) == ESP_OK, "setup: write listed file");
    }
    TEST_CHECK(cfg_fs_write_atomic("decoy_other.dat", payload, sizeof(payload)) == ESP_OK, "setup: decoy file");
    int deleted = 0;
    TEST_CHECK(kiln_scope_cfg_files_delete(&deleted) == ESP_OK && deleted == (int)n, "all listed files deleted");
    uint8_t buf[16];
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        TEST_CHECK(cfg_fs_read(paths[i], buf, sizeof(buf), &len) == ESP_ERR_NOT_FOUND, "listed file is gone");
    }
    TEST_CHECK(cfg_fs_read("decoy_other.dat", buf, sizeof(buf), &len) == ESP_OK, "an unlisted file is untouched");
    TEST_CHECK(kiln_scope_cfg_files_delete(&deleted) == ESP_OK && deleted == 0, "second pass: absent files are not errors");
    cfg_fs_delete("decoy_other.dat");
}

static void write_file(const char *rel)
{
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    TEST_CHECK(cfg_fs_write_atomic(rel, payload, sizeof(payload)) == ESP_OK, "setup: write file");
}

static bool file_exists(const char *rel)
{
    uint8_t buf[16];
    size_t len = 0;
    return cfg_fs_read(rel, buf, sizeof(buf), &len) == ESP_OK;
}

static void test_profiles_scope_deletes_slot_and_stats_files(void)
{
    TEST_SECTION("profiles scope cfg files: slot + firing-history mirrors deleted, foreign files survive");
    reset_all();
    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    size_t ndirs = 0;
    const char *const *dirs = profiles_scope_cfg_files_dirs(&ndirs);
    TEST_CHECK(ndirs == 2 && strcmp(dirs[0], PROFILES_CFG_FS_DIR) == 0 &&
                   strcmp(dirs[1], FIRING_STATS_CFG_FS_DIR) == 0,
               "coverage: exactly the profile-slot and firing-stats directories (add new profiles_nvs mirrors here)");

    /* Paths come from the OWNERS' own format macros, so a format change in
     * either owner is followed (or fails here), never silently missed. */
    const uint8_t *ids = kProfilesTestIds;
    const size_t nids = sizeof(kProfilesTestIds);
    char path[64];
    for (size_t i = 0; i < nids; i++) {
        snprintf(path, sizeof(path), PROFILES_CFG_FS_PATH_FMT, (unsigned)ids[i]);
        write_file(path);
        snprintf(path, sizeof(path), FIRING_STATS_CFG_FS_PATH_FMT, (unsigned)ids[i]);
        write_file(path);
    }
    write_file("profiles/hidden.json");   /* deleted by profiles_builtin_discard_file(), not this module */
    write_file("profiles/other.json");    /* foreign names survive */
    write_file("stats/readme.dat");
    write_file("profiles/prof07.json");   /* non-canonical id spelling is not an owner path */
    write_file("zones.json");

    int deleted = 0;
    TEST_CHECK(profiles_scope_cfg_files_delete(&deleted) == ESP_OK && deleted == (int)(2 * nids),
               "every slot and history file deleted (2 per id)");
    for (size_t i = 0; i < nids; i++) {
        snprintf(path, sizeof(path), PROFILES_CFG_FS_PATH_FMT, (unsigned)ids[i]);
        TEST_CHECK(!file_exists(path), "slot file is gone");
        snprintf(path, sizeof(path), FIRING_STATS_CFG_FS_PATH_FMT, (unsigned)ids[i]);
        TEST_CHECK(!file_exists(path), "firing-history file is gone");
    }
    TEST_CHECK(file_exists("profiles/hidden.json"), "hidden.json is not this module's file (no double delete)");
    TEST_CHECK(file_exists("profiles/other.json") && file_exists("stats/readme.dat") &&
                   file_exists("profiles/prof07.json") && file_exists("zones.json"),
               "files outside the owners' path formats are untouched");
    TEST_CHECK(profiles_scope_cfg_files_delete(&deleted) == ESP_OK && deleted == 0,
               "second pass: nothing left, not an error");
    cfg_fs_delete("profiles/hidden.json");
    cfg_fs_delete("profiles/other.json");
    cfg_fs_delete("stats/readme.dat");
    cfg_fs_delete("profiles/prof07.json");
    cfg_fs_delete("zones.json");
}

static void test_profiles_scope_unmounted_is_not_an_error(void)
{
    TEST_SECTION("profiles scope cfg files: unmounted cfg_fs is not an error");
    reset_all();
    int deleted = -1;
    TEST_CHECK(profiles_scope_cfg_files_delete(&deleted) == ESP_OK && deleted == 0, "unmounted: ESP_OK, nothing deleted");
}

static void test_dualwrite_status_row(void)
{
    TEST_SECTION("zone normals cfg_fs: zone_normals_get_dualwrite_status() reports file/NVS presence, revs, divergence");
    reset_all();
    bool fv = true, nv = true, dv = true;
    uint32_t fr = 99, nr = 99;
    zone_normals_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(!fv && !nv && !dv && fr == 0 && nr == 0, "nothing saved, cfg unmounted: both absent, rev 0, not diverged");

    TEST_CHECK(cfg_fs_init(SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    zone_normals_load();
    TEST_CHECK(zone_normals_set(0, 1.0f) && zone_normals_set(0, 2.0f) && zone_normals_set(1, 3.0f), "three saves");
    zone_normals_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && nv && fr == 3 && nr == 3 && !dv, "both sides valid at rev 3, same content, not diverged");

    /* NVS ahead with different content: file write fails on save 4. */
    pref_cfg_fs_set_write_fn(failing_write_fn);
    TEST_CHECK(zone_normals_set(2, 9.0f), "rev 4 lands in NVS only");
    pref_cfg_fs_reset_write_fn_for_test();
    zone_normals_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && nv && fr == 3 && nr == 4 && dv, "file rev 3 vs NVS rev 4 with differing content: diverged");

    /* The accessor must not have healed it (a status GET never writes). */
    zone_normals_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fr == 3 && nr == 4 && dv, "a second read still diverged: the accessor performed no resync write");

    fake_kv_reset_all(); /* NVS wiped, file survives */
    hal_kv_init_partition(KILN_NVS_PARTITION);
    zone_normals_get_dualwrite_status(&fv, &fr, &nv, &nr, &dv);
    TEST_CHECK(fv && !nv && fr == 3 && nr == 0 && !dv, "file only: file valid, NVS absent, not diverged");

    /* NULL out-params are tolerated. */
    zone_normals_get_dualwrite_status(NULL, NULL, NULL, NULL, NULL);
    TEST_CHECK(true, "NULL out-params do not crash");
}

void run_test_zone_normals_cfg_fs(void)
{
    test_unmounted_uses_nvs_only();
    test_save_writes_both();
    test_nvs_empty_file_present_resolves_from_file();
    test_both_present_higher_rev_wins();
    test_future_version_file_rejected();
    test_nvs_failure_does_not_advance_rev();
    test_kiln_reset_leaves_blank();
    test_kiln_scope_deletes_every_listed_file();
    test_profiles_scope_deletes_slot_and_stats_files();
    test_profiles_scope_unmounted_is_not_an_error();
    test_dualwrite_status_row();
    reset_all();
}
