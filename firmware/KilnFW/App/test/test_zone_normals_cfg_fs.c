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

#include "cfg_fs.h"
#include "pref_cfg_fs.h"
#include "zones_http_internal.h"

static const char *SCRATCH_BASE = "cfg_fs_test_zone_normals";

static void reset_all(void)
{
    char path[600];
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

void run_test_zone_normals_cfg_fs(void)
{
    test_unmounted_uses_nvs_only();
    test_save_writes_both();
    test_nvs_empty_file_present_resolves_from_file();
    test_both_present_higher_rev_wins();
    reset_all();
}
