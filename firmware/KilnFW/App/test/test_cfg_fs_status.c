// Host tests for cfg_fs_status.c -- the /api/cfgfs JSON builder
// (docs/FILESYSTEM_USER_DATA_PLAN.md observability pass). Same real-temp-
// directory approach as test_cfg_fs.c: cfg_fs_status_build_json() is pure
// stdio + cfg_fs.h's public API, so this exercises the exact code that
// runs on-device against "/cfg" -- only the base directory string differs.
//
// Coverage: unmounted, mount-failed (partition-absent style), mounted +
// empty filesystem, mounted with files present (sizes verified against
// what was actually written), and the dual-write section's diverged/
// not-diverged/absent cases. Capacity is exercised via a synthetic
// cfg_fs_capacity_info_t -- esp_littlefs_info() itself is ESP-IDF-only and
// deliberately kept out of this pure module (see cfg_fs_status.h).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TCFS_MKDIR(p) _mkdir(p)
#define TCFS_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TCFS_MKDIR(p) mkdir((p), 0755)
#define TCFS_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "../drivers/persist/cfg_fs.h"
#include "../drivers/persist/cfg_fs_status.h"

static void reset_scratch(const char *base)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp", base);
    TCFS_RMDIR(path);
    TCFS_RMDIR(base);
    TCFS_MKDIR(base);
}

/* Cheap "does this JSON contain this substring" check -- these tests do not
 * need a real JSON parser, just confirmation that the fields this module
 * promises to report are actually present with the right value. */
static int json_has(const char *json, const char *needle)
{
    return strstr(json, needle) != NULL;
}

static void test_unmounted(void)
{
    TEST_SECTION("cfg_fs_status: never-mounted status reports mounted:false, status:unmounted, a reason, no "
                 "file/capacity sections claiming knowledge they don't have");
    cfg_fs_deinit();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs starts unmounted for this test");

    char json[2048];
    size_t len = 0;
    esp_err_t err = cfg_fs_status_build_json(NULL, NULL, NULL, json, sizeof(json), &len);
    TEST_CHECK(err == ESP_OK, "build succeeds even when nothing is mounted");
    TEST_CHECK(json_has(json, "\"mounted\":false"), "reports mounted:false");
    TEST_CHECK(json_has(json, "\"status\":\"unmounted\""), "reports status:unmounted");
    TEST_CHECK(json_has(json, "\"reason\":"), "carries a reason string when not mounted");
    TEST_CHECK(json_has(json, "\"capacity\":{\"known\":false}"), "capacity reported unknown, not zeroed");
    TEST_CHECK(json_has(json, "\"file_count\":0"), "no files when unmounted");
    TEST_CHECK(json_has(json, "\"dual_write\":{\"zones\":{\"file_backed\":false}"),
               "dual-write section present but zones not file-backed when dual info is NULL");
}

static void test_unavailable(void)
{
    TEST_SECTION("cfg_fs_status: mount-failed (partition absent) reports status:unavailable with its own reason");
    cfg_fs_deinit();
    const char *missing = "cfg_fs_status_test_missing_dir";
    TCFS_RMDIR(missing);
    TEST_CHECK(cfg_fs_init(missing, NULL) != ESP_OK, "init against a nonexistent directory fails");
    TEST_CHECK(cfg_fs_get_status() == CFG_FS_STATUS_UNAVAILABLE, "status is UNAVAILABLE");

    char json[2048];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(NULL, NULL, NULL, json, sizeof(json), &len) == ESP_OK, "build succeeds");
    TEST_CHECK(json_has(json, "\"status\":\"unavailable\""), "reports status:unavailable");
    TEST_CHECK(json_has(json, "mount was attempted and failed"), "reason names mount failure, distinct from "
                                                                  "the never-mounted reason");
    cfg_fs_deinit();
}

static void test_mounted_empty(void)
{
    TEST_SECTION("cfg_fs_status: mounted, empty filesystem -- mounted:true, zero files, tmp_entries_now:0");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_empty";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");

    char json[2048];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, json, sizeof(json), &len) == ESP_OK, "build succeeds");
    TEST_CHECK(json_has(json, "\"mounted\":true"), "reports mounted:true");
    TEST_CHECK(!json_has(json, "\"reason\":"), "no reason field once mounted");
    TEST_CHECK(json_has(json, "\"file_count\":0"), "no files yet");
    TEST_CHECK(json_has(json, "\"files\":[]"), "empty files array");
    TEST_CHECK(json_has(json, "\"tmp_entries_now\":0"), "nothing stuck in .tmp/ right after mount");
    cfg_fs_deinit();
}

static void test_mounted_with_files(void)
{
    TEST_SECTION("cfg_fs_status: mounted with files present -- file_count and per-file size_bytes match what "
                 "was actually written, and capacity/dual-write sections carry the values passed in");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_files";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");

    const char zones_payload[7] = "abcdef"; /* 6 bytes + NUL not written -- len below is what matters */
    TEST_CHECK(cfg_fs_write_atomic("zones.json", zones_payload, 6) == ESP_OK, "write zones.json (6 bytes)");
    const char prefs_payload[13] = "hello world!"; /* 12 bytes */
    TEST_CHECK(cfg_fs_write_atomic("prefs.json", prefs_payload, 12) == ESP_OK, "write prefs.json (12 bytes)");

    cfg_fs_capacity_info_t cap = { .known = true, .total_bytes = 524288, .used_bytes = 4096 };
    cfg_fs_zones_dualwrite_info_t dual = { .file_valid = true, .file_rev = 5, .nvs_rev = 5 };

    char json[2048];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(base, &cap, &dual, json, sizeof(json), &len) == ESP_OK, "build succeeds");
    TEST_CHECK(json_has(json, "\"file_count\":2"), "both files counted");
    TEST_CHECK(json_has(json, "\"name\":\"zones.json\",\"size_bytes\":6"), "zones.json size matches what was written");
    TEST_CHECK(json_has(json, "\"name\":\"prefs.json\",\"size_bytes\":12"), "prefs.json size matches what was written");
    TEST_CHECK(json_has(json, "\"capacity\":{\"known\":true,\"total_bytes\":524288,\"used_bytes\":4096,"
                              "\"free_bytes\":520192}"),
               "capacity section echoes the caller-supplied values and computes free correctly");
    TEST_CHECK(json_has(json, "\"zones\":{\"file_backed\":true,\"file_rev\":5,\"nvs_rev\":5,\"diverged\":false}"),
               "dual-write: equal revs is not diverged");
    TEST_CHECK(json_has(json, "\"nvs_only\":["), "still lists the not-yet-migrated items");

    /* Now the divergence case: NVS strictly ahead of the file means a prior
     * file write failed -- must be flagged, not silently reported healthy. */
    dual.nvs_rev = 6;
    TEST_CHECK(cfg_fs_status_build_json(base, &cap, &dual, json, sizeof(json), &len) == ESP_OK, "build succeeds");
    TEST_CHECK(json_has(json, "\"diverged\":true"), "nvs_rev > file_rev is flagged diverged");

    cfg_fs_deinit();
}

static void test_buffer_too_small(void)
{
    TEST_SECTION("cfg_fs_status: NEGATIVE TARGET -- a buffer too small to hold the JSON fails loudly "
                 "(ESP_ERR_INVALID_SIZE), never silently truncates");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_toosmall";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(cfg_fs_write_atomic("zones.json", "x", 1) == ESP_OK, "write one file");

    char tiny[8];
    size_t len = 999;
    esp_err_t err = cfg_fs_status_build_json(base, NULL, NULL, tiny, sizeof(tiny), &len);
    TEST_CHECK(err == ESP_ERR_INVALID_SIZE, "reports truncation as an error rather than shipping a partial JSON");

    cfg_fs_deinit();
}

void run_test_cfg_fs_status(void)
{
    test_unmounted();
    test_unavailable();
    test_mounted_empty();
    test_mounted_with_files();
    test_buffer_too_small();
    cfg_fs_deinit();
}
