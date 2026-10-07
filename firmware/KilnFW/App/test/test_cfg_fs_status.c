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
    esp_err_t err = cfg_fs_status_build_json(NULL, NULL, NULL, 0, NULL, json, sizeof(json), &len);
    TEST_CHECK(err == ESP_OK, "build succeeds even when nothing is mounted");
    TEST_CHECK(json_has(json, "\"mounted\":false"), "reports mounted:false");
    TEST_CHECK(json_has(json, "\"status\":\"unmounted\""), "reports status:unmounted");
    TEST_CHECK(json_has(json, "\"reason\":"), "carries a reason string when not mounted");
    TEST_CHECK(json_has(json, "\"capacity\":{\"known\":false}"), "capacity reported unknown, not zeroed");
    TEST_CHECK(json_has(json, "\"file_count\":0"), "no files when unmounted");
    TEST_CHECK(json_has(json, "\"dual_write\":{\"write_mode\":\"cfg_only\",\"items\":[]"),
               "dual-write section present but empty when no items are supplied");
    TEST_CHECK(json_has(json, "\"format\":{\"known\":false}"),
               "format section present but reports known:false when no progress info is supplied "
               "(NULL fmt) -- the common case, no auto/deferred format has run this boot");
}

static void test_format_progress(void)
{
    TEST_SECTION("cfg_fs_status: deferred auto-format progress section (docs/audits/boot_hang_2026-09-08.md "
                 "follow-up) -- in-progress, completed/succeeded, and completed/failed are all reported "
                 "distinctly, and a not-yet-over-ceiling in-progress format is NOT flagged stalled");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_format";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");

    char json[2048];
    size_t len = 0;

    cfg_fs_format_progress_t in_progress = { .known = true, .in_progress = true, .elapsed_ms = 5000 };
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, &in_progress, json, sizeof(json), &len) == ESP_OK,
               "build succeeds");
    TEST_CHECK(json_has(json, "\"format\":{\"known\":true,\"in_progress\":true,\"completed\":false,"
                              "\"succeeded\":false,\"elapsed_ms\":5000,\"stalled\":false"),
               "5 s into a format is reported in-progress, not stalled -- well under the ceiling");

    cfg_fs_format_progress_t done_ok = { .known = true, .completed = true, .succeeded = true, .elapsed_ms = 6200 };
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, &done_ok, json, sizeof(json), &len) == ESP_OK,
               "build succeeds");
    TEST_CHECK(json_has(json, "\"completed\":true,\"succeeded\":true,\"elapsed_ms\":6200"),
               "a completed successful format reports its final duration");
    TEST_CHECK(!json_has(json, "\"error\":"), "a successful format never carries an error field");

    cfg_fs_format_progress_t done_failed = { .known = true, .completed = true, .succeeded = false,
                                              .elapsed_ms = 1200, .result = ESP_ERR_TIMEOUT };
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, &done_failed, json, sizeof(json), &len) == ESP_OK,
               "build succeeds");
    TEST_CHECK(json_has(json, "\"completed\":true,\"succeeded\":false"),
               "a completed but failed format is distinguished from a completed success");
    /* NEGATIVE TARGET: before this fix, a failed format surfaced only the
     * bare "succeeded":false boolean -- the actual esp_err_t (e.g. from
     * esp_littlefs_format(), or ESP_ERR_TIMEOUT from cfg_fs_mount.c's
     * wait_for_flash_worker() giving up) was logged, if anywhere, and never
     * reached GET /api/cfgfs at all. This is the one-query-away contract the
     * "16 ms failure, no visible cause" investigation asked for. */
    TEST_CHECK(json_has(json, "\"error\":\"ESP_ERR_TIMEOUT\""),
               "a failed format names its esp_err_t so the cause is one GET /api/cfgfs away, not a fresh "
               "investigation");

    cfg_fs_deinit();
}

static void test_format_stalled_ceiling(void)
{
    TEST_SECTION("cfg_fs_status: NEGATIVE TARGET -- cfg_fs_format_is_stalled() flags an in-progress format "
                 "past CFG_FS_FORMAT_CEILING_MS as stalled, and never flags a completed one regardless of "
                 "duration");
    TEST_CHECK(!cfg_fs_format_is_stalled(true, CFG_FS_FORMAT_CEILING_MS),
               "exactly at the ceiling is not yet stalled (strictly greater-than)");
    TEST_CHECK(cfg_fs_format_is_stalled(true, CFG_FS_FORMAT_CEILING_MS + 1),
               "one ms past the ceiling IS stalled");
    TEST_CHECK(!cfg_fs_format_is_stalled(false, CFG_FS_FORMAT_CEILING_MS + 60000),
               "a format that already finished is never reported stalled no matter how long it took");

    /* Wired end-to-end through the JSON builder too, not just the pure
     * predicate -- GET /api/cfgfs is what an operator actually reads. */
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_stalled";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");
    cfg_fs_format_progress_t stuck = { .known = true, .in_progress = true,
                                        .elapsed_ms = CFG_FS_FORMAT_CEILING_MS + 5000 };
    char json[2048];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, &stuck, json, sizeof(json), &len) == ESP_OK,
               "build succeeds");
    TEST_CHECK(json_has(json, "\"stalled\":true"), "GET /api/cfgfs surfaces the stalled flag once the ceiling "
                                                    "is exceeded, distinguishing a stuck format from a slow one");
    cfg_fs_deinit();
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
    TEST_CHECK(cfg_fs_status_build_json(NULL, NULL, NULL, 0, NULL, json, sizeof(json), &len) == ESP_OK, "build succeeds");
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
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, NULL, json, sizeof(json), &len) == ESP_OK, "build succeeds");
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
    cfg_fs_dualwrite_item_t items[2] = {
        { .name = "zones", .file_valid = true, .file_rev = 5, .nvs_valid = true, .nvs_rev = 5, .diverged = false },
        { .name = "unit_pref", .file_valid = true, .file_rev = 2, .nvs_valid = true, .nvs_rev = 2, .diverged = false },
    };

    char json[2048];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(base, &cap, items, 2, NULL, json, sizeof(json), &len) == ESP_OK,
               "build succeeds");
    TEST_CHECK(json_has(json, "\"file_count\":2"), "both files counted");
    TEST_CHECK(json_has(json, "\"name\":\"zones.json\",\"size_bytes\":6"), "zones.json size matches what was written");
    TEST_CHECK(json_has(json, "\"name\":\"prefs.json\",\"size_bytes\":12"), "prefs.json size matches what was written");
    TEST_CHECK(json_has(json, "\"capacity\":{\"known\":true,\"total_bytes\":524288,\"used_bytes\":4096,"
                              "\"free_bytes\":520192}"),
               "capacity section echoes the caller-supplied values and computes free correctly");
    TEST_CHECK(json_has(json, "\"dual_write\":{\"write_mode\":\"cfg_only\",\"items\":["
                              "{\"name\":\"zones\",\"file_backed\":true,\"file_rev\":5,\"nvs_backed\":true,"
                              "\"nvs_rev\":5,\"diverged\":false,\"nvs_stale\":false,\"migration_deferred\":false},"
                              "{\"name\":\"unit_pref\",\"file_backed\":true,\"file_rev\":2,\"nvs_backed\":true,"
                              "\"nvs_rev\":2,\"diverged\":false,\"nvs_stale\":false,\"migration_deferred\":false}]"),
               "dual-write: every item passed in gets its own row, not just zones -- 70ed6514 fixed the stale "
               "lists but left per-item detail zones-only; this is the widened per-bridge picture");
    TEST_CHECK(json_has(json, "\"nvs_only\":[]"),
               "nvs_only is EMPTY -- 762bb29e gave relay_cycles/adaptive_tune/firing_stats real cfg_fs "
               "bridges too (the last three items docs/FILESYSTEM_USER_DATA_PLAN.md section 5 tracked), "
               "so nothing remains genuinely NVS-only; see test_all_17_items_report_dualwrite_state() below "
               "for the full per-item picture");
    TEST_CHECK(json_has(json, "\"nvs_permanent\":["), "nvs_permanent section present");
    TEST_CHECK(json_has(json, "\"wifi_creds\""),
               "an intentionally-NVS-forever item (wifi creds) is reported in nvs_permanent, "
               "never in nvs_only -- an operator must not read it as a pending migration");

    /* dual_write_window: not supplied -> known:false; supplied -> every field echoed. */
    TEST_CHECK(json_has(json, "\"dual_write_window\":{\"known\":false}"),
               "no window supplied: reported as unknown, not as zeros");
    {
        dualwrite_window_status_t win = { .consecutive_clean_boots = 7, .clean_boots_target = 20,
                                           .firing_complete = true, .restore_verified = false,
                                           .window_may_close = false };
        char wjson[2048];
        size_t wlen = 0;
        TEST_CHECK(cfg_fs_status_build_json_ex(base, &cap, items, 2, NULL, &win, wjson, sizeof(wjson), &wlen) == ESP_OK,
                   "build_json_ex succeeds with a window");
        TEST_CHECK(json_has(wjson, "\"dual_write_window\":{\"known\":true,\"consecutive_clean_boots\":7,"
                                   "\"clean_boots_target\":20,\"firing_complete\":true,"
                                   "\"restore_verified\":false,\"window_may_close\":false}"),
                   "window fields rendered faithfully");
        win.window_may_close = true;
        win.restore_verified = true;
        TEST_CHECK(cfg_fs_status_build_json_ex(base, &cap, items, 2, NULL, &win, wjson, sizeof(wjson), &wlen) == ESP_OK &&
                       json_has(wjson, "\"restore_verified\":true,\"window_may_close\":true}"),
                   "flipped flags are rendered independently");
        TEST_CHECK(json_has(wjson, "\"dual_write\":{\"write_mode\":\"cfg_only\",\"items\":[") && json_has(wjson, "\"format\":{"),
                   "window section sits between dual_write and format without disturbing either");
    }

    /* Now the divergence case: caller (a real bridge's own accessor, on
     * device) reports diverged when both sides are valid and disagree --
     * this test only checks that whatever the caller passes for `diverged`
     * on any one item is rendered faithfully and independently per item,
     * not derived or re-computed here (cfg_fs_status.c is a dumb renderer;
     * see cfg_fs_status_item_diverged()'s own tests below for the shared
     * divergence RULE itself). */
    items[1].diverged = true;
    TEST_CHECK(cfg_fs_status_build_json(base, &cap, items, 2, NULL, json, sizeof(json), &len) == ESP_OK,
               "build succeeds");
    TEST_CHECK(json_has(json, "\"name\":\"zones\",\"file_backed\":true,\"file_rev\":5,\"nvs_backed\":true,"
                              "\"nvs_rev\":5,\"diverged\":false"),
               "zones (untouched) still reports diverged:false");
    TEST_CHECK(json_has(json, "\"name\":\"unit_pref\",\"file_backed\":true,\"file_rev\":2,\"nvs_backed\":true,"
                              "\"nvs_rev\":2,\"diverged\":true"),
               "unit_pref's diverged:true is surfaced independently of zones' row -- one item diverging "
               "must not be lost among, or confused with, another item's healthy row");

    cfg_fs_deinit();
}

static void test_item_diverged_rule(void)
{
    TEST_SECTION("cfg_fs_status_item_diverged: THE shared divergence rule every bridge's status accessor "
                 "calls -- both sides must be valid AND their content must differ; a rev mismatch alone is "
                 "NOT sufficient (dual-write's file-then-NVS ordering makes that the ordinary in-flight "
                 "case), and equal-rev-differing-content (check_cfg_fs_tie_break.ps1's dangerous case) IS "
                 "flagged since this rule is driven by content_equal, never by comparing revs itself");
    TEST_CHECK(!cfg_fs_status_item_diverged(false, false, false), "neither side valid -- not diverged");
    TEST_CHECK(!cfg_fs_status_item_diverged(true, false, false), "only file valid -- not diverged (nothing to "
                                                                  "disagree with)");
    TEST_CHECK(!cfg_fs_status_item_diverged(false, true, false), "only NVS valid -- not diverged");
    TEST_CHECK(!cfg_fs_status_item_diverged(true, true, true), "both valid, content equal -- not diverged");
    TEST_CHECK(cfg_fs_status_item_diverged(true, true, false), "both valid, content differs -- DIVERGED, "
                                                                "regardless of which side's rev is higher");
}

/* 2026-09-08: verifies all 17 dual-write items (the 14 already reporting
 * plus relay_cycles/adaptive_tune/firing_stats, moved off the stale
 * nvs_only list in this pass) render their own row, AND that a simulated
 * divergence on any ONE of the three new items surfaces independently --
 * same "one item diverging must not be lost among another item's healthy
 * row" property test_mounted_with_files() above already established for
 * zones/unit_pref, now covering the three items this pass adds. Exercises
 * only cfg_fs_status_build_json() (the pure renderer) with a synthetic
 * items array -- the real per-bridge accessors (relay_cycles_get_
 * dualwrite_status() etc.) are ESP-IDF/NVS-backed and covered separately
 * on-device; this test's job is the JSON shape, not those accessors'
 * internals. */
static void test_all_17_items_report_dualwrite_state(void)
{
    TEST_SECTION("cfg_fs_status: all 17 dual-write items (2026-09-08 -- relay_cycles/adaptive_tune/"
                 "firing_stats moved off the stale nvs_only list) each report their own row, and a "
                 "divergence on any one of the three new items is surfaced independently");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_17items";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");

    cfg_fs_dualwrite_item_t items[17] = {
        { .name = "zones", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "kiln_cfg_store", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "unit_pref", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "ramp_assist", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "display_power", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "tz", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile0", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile1", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile2", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile3", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile4", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile5", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile6", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "profile7", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1 },
        { .name = "relay_cycles", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1,
          .diverged = false },
        { .name = "adaptive_tune", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1,
          .diverged = false },
        { .name = "firing_stats", .file_valid = true, .file_rev = 1, .nvs_valid = true, .nvs_rev = 1,
          .diverged = false },
    };

    char json[4096];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, items, 17, NULL, json, sizeof(json), &len) == ESP_OK,
               "build succeeds with all 17 items");
    for (size_t i = 0; i < 17; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "\"name\":\"%s\"", items[i].name);
        TEST_CHECK(json_has(json, needle), items[i].name);
    }
    TEST_CHECK(json_has(json, "\"nvs_only\":[]"), "nvs_only empty with the full 17-item set too");

    /* Simulated divergence, one at a time, on each of the three NEW items
     * (relay_cycles/adaptive_tune/firing_stats) -- proving the JSON
     * builder surfaces a divergence on any one of them independently of
     * the other 16 healthy rows, the same property already proven for
     * zones/unit_pref in test_mounted_with_files(). */
    const char *new_item_names[3] = { "relay_cycles", "adaptive_tune", "firing_stats" };
    for (size_t which = 0; which < 3; which++) {
        cfg_fs_dualwrite_item_t items2[17];
        memcpy(items2, items, sizeof(items));
        for (size_t i = 0; i < 17; i++) {
            if (strcmp(items2[i].name, new_item_names[which]) == 0) {
                items2[i].diverged = true;
            }
        }
        char json2[4096];
        size_t len2 = 0;
        TEST_CHECK(cfg_fs_status_build_json(base, NULL, items2, 17, NULL, json2, sizeof(json2), &len2) == ESP_OK,
                   "build succeeds with one item diverged");
        char needle[96];
        snprintf(needle, sizeof(needle), "\"name\":\"%s\",\"file_backed\":true,\"file_rev\":1,"
                                          "\"nvs_backed\":true,\"nvs_rev\":1,\"diverged\":true",
                 new_item_names[which]);
        TEST_CHECK(json_has(json2, needle), new_item_names[which]);
        /* The OTHER two new items, and every pre-existing item, must still
         * read diverged:false in this same response -- a divergence on one
         * item must never bleed into another's row. */
        for (size_t other = 0; other < 3; other++) {
            if (other == which) {
                continue;
            }
            char needle2[96];
            snprintf(needle2, sizeof(needle2), "\"name\":\"%s\",\"file_backed\":true,\"file_rev\":1,"
                                                "\"nvs_backed\":true,\"nvs_rev\":1,\"diverged\":false",
                     new_item_names[other]);
            TEST_CHECK(json_has(json2, needle2), new_item_names[other]);
        }
    }

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
    esp_err_t err = cfg_fs_status_build_json(base, NULL, NULL, 0, NULL, tiny, sizeof(tiny), &len);
    TEST_CHECK(err == ESP_ERR_INVALID_SIZE, "reports truncation as an error rather than shipping a partial JSON");

    cfg_fs_deinit();
}

/* Worst-case sizing for the real GET /api/cfgfs response (2026-10-04, when the
 * zone_normals and relay_names rows made it 13 rows; WP-1 of SPARE_RELAY_ONOFF_PLAN added aux_outputs + aux_out.dat, 14 rows / 12 files; WP9 added update_repo + update_repo.dat, 15 rows / 13 files). Renders through cfg_fs_status_build_json_ex(),
 * the same entry point diagnostics_http.c's handler calls, with every section
 * populated the way the handler populates it: all 15 real row names at
 * UINT32_MAX revs with diverged and migration_deferred set, capacity known at
 * SIZE_MAX-ish sizes, a completed-and-failed format object, a filled
 * dual-write window, and the 18 real root files (2026-10-07: 20 rows) with multi-digit sizes. The
 * host esp_err_to_name() stub returns "ESP_FAIL", so the longest real error name
 * (ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED, 38 chars) is added to the measured
 * length by hand. The result must fit the handler's cfgfs_status_scratch_t.json
 * (CFG_FS_STATUS_HANDLER_JSON_BUF, 5632 B) with at least 400 B to spare. */
#define WORST_CASE_HANDLER_BUF CFG_FS_STATUS_HANDLER_JSON_BUF
static void test_worst_case_fits_handler_buffer(void)
{
    TEST_SECTION("cfg_fs_status: worst-case /api/cfgfs (20 rows at UINT32_MAX, known capacity, failed format, "
                 "filled window, 18 root files) via the handler's _ex path fits the handler buffer with 400 B spare");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_worstcase";
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");

    static const char *const files[18] = { "zones.json",    "kiln_configs.json", "unit_pref.dat",
                                           "ki_base.dat",   "ramp_assist.dat",   "tz.dat",
                                           "display_power.dat", "iter_tune.bin", "relay_cycles.dat",
                                           "relay_names.dat", "zone_normals.dat", "aux_out.dat",
                                           "update_repo.dat", "prof_fav.bin",  "ct_verify.bin",
                                           "setup_wiz.bin", "prof_live_rec.bin", "prof_live_work.bin" };
    static char payload[100000];
    memset(payload, 'x', sizeof(payload));
    for (size_t i = 0; i < 18; i++) {
        TEST_CHECK(cfg_fs_write_atomic(files[i], payload, sizeof(payload)) == ESP_OK, "write a real root file");
    }
    TEST_CHECK(cfg_fs_write_atomic("profiles/prof1.json", payload, sizeof(payload)) == ESP_OK,
               "write a profiles/ file so the subdirs[] summary renders at six-digit bytes");

    static const char *const names[20] = { "zones",         "kiln_cfg_store", "unit_pref",     "profiles_hidden",
                                           "zone_normals",  "ramp_assist",    "display_power", "tz",
                                           "profiles",      "relay_cycles",   "adaptive_tune", "firing_stats",
                                           "relay_names",   "aux_outputs",    "update_repo",
                                           "iter_tune",     "profiles_favorites", "ct_verify_store",
                                           "setup_wizard_progress", "live_profile" };
    cfg_fs_dualwrite_item_t items[20];
    for (size_t i = 0; i < 20; i++) {
        items[i] = (cfg_fs_dualwrite_item_t){ .name = names[i], .file_valid = true, .file_rev = UINT32_MAX,
                                              .nvs_valid = true, .nvs_rev = UINT32_MAX, .diverged = true,
                                              .migration_deferred = true };
    }
    cfg_fs_capacity_info_t cap = { .known = true, .total_bytes = 4294967295u, .used_bytes = 4294967295u };
    cfg_fs_format_progress_t fmt = { .known = true, .in_progress = false, .completed = true, .succeeded = false,
                                     .elapsed_ms = UINT32_MAX, .result = ESP_FAIL };
    dualwrite_window_status_t win = { .consecutive_clean_boots = UINT32_MAX, .clean_boots_target = UINT32_MAX,
                                      .firing_complete = true, .restore_verified = true, .window_may_close = true };

    static char json[WORST_CASE_HANDLER_BUF];
    size_t len = 0;
    esp_err_t err = cfg_fs_status_build_json_ex(base, &cap, items, 20, &fmt, &win, json, sizeof(json), &len);
    TEST_CHECK(err == ESP_OK, "worst-case render fits the handler's buffer");
    if (err == ESP_OK) {
        size_t real_len = len + (strlen("ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED") - strlen(esp_err_to_name(ESP_FAIL)));
        printf("  worst-case /api/cfgfs length: %lu B rendered, %lu B with longest esp_err name, buffer %u B\n",
               (unsigned long)len, (unsigned long)real_len, WORST_CASE_HANDLER_BUF);
        TEST_CHECK(json_has(json, "\"name\":\"zone_normals\",\"file_backed\":true,\"file_rev\":4294967295,"
                                   "\"nvs_backed\":true,\"nvs_rev\":4294967295,\"diverged\":true"),
                   "zone_normals row carries the same fields as its siblings");
        TEST_CHECK(json_has(json, "\"name\":\"zone_normals.dat\""), "the files[] list is populated (sizes rendered)");
        TEST_CHECK(json_has(json, "\"name\":\"update_repo\",\"file_backed\":true,\"file_rev\":4294967295,"
                                   "\"nvs_backed\":true,\"nvs_rev\":4294967295,\"diverged\":true"),
                   "update_repo row carries the same fields as its siblings");
        TEST_CHECK(json_has(json, "\"name\":\"update_repo.dat\""), "update_repo.dat is in the files[] list");
        TEST_CHECK(json_has(json, "\"error\":"), "the format error string is rendered");
        TEST_CHECK(real_len + 400 <= WORST_CASE_HANDLER_BUF, "at least 400 B of margin remains before the cap");
    }
    cfg_fs_deinit();
}

/* GET /api/cfgfs used to omit every file in a subdirectory: cfg_fs_list("")
 * skips directories, so profiles/prof<id>.json never appeared anywhere.
 * The builder now summarizes each known subdirectory (count + bytes). */
static void test_subdirs_summarized(void)
{
    TEST_SECTION("cfg_fs_status: files under profiles/ are summarized in subdirs[] (count and bytes), not "
                 "silently absent");
    cfg_fs_deinit();
    const char *base = "cfg_fs_status_test_subdirs";
    {
        /* reset_scratch() only removes .tmp and base; clear profiles/ first. */
        char p1[600], p2[600], pd[600], pt[600];
        snprintf(p1, sizeof(p1), "%s/profiles/prof1.json", base);
        snprintf(p2, sizeof(p2), "%s/profiles/prof2.json", base);
        snprintf(pd, sizeof(pd), "%s/profiles", base);
        snprintf(pt, sizeof(pt), "%s/profiles/hidden.json", base);
        remove(p1);
        remove(p2);
        remove(pt);
        TCFS_RMDIR(pd);
    }
    reset_scratch(base);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "cfg_fs mounts");

    char json[4608];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, NULL, json, sizeof(json), &len) == ESP_OK,
               "build succeeds with no subdirectory present");
    TEST_CHECK(json_has(json, "\"subdirs\":[{\"name\":\"profiles\",\"file_count\":0,\"size_bytes\":0,"
                              "\"unknown_size\":0}]"),
               "an absent profiles/ directory reads as 0 files, not as an error");

    TEST_CHECK(cfg_fs_write_atomic("zones.json", "abcdef", 6) == ESP_OK, "write a root file");
    TEST_CHECK(cfg_fs_write_atomic("profiles/prof1.json", "0123456789", 10) == ESP_OK, "write profiles/prof1.json");
    TEST_CHECK(cfg_fs_write_atomic("profiles/prof2.json", "abcde", 5) == ESP_OK, "write profiles/prof2.json");
    TEST_CHECK(cfg_fs_write_atomic("profiles/hidden.json", "xyz", 3) == ESP_OK, "write profiles/hidden.json");
    TEST_CHECK(cfg_fs_status_build_json(base, NULL, NULL, 0, NULL, json, sizeof(json), &len) == ESP_OK,
               "build succeeds with subdirectory files");
    TEST_CHECK(json_has(json, "\"file_count\":1,\"files\":[{\"name\":\"zones.json\",\"size_bytes\":6}]"),
               "files[] still lists only root files (the existing contract is unchanged)");
    TEST_CHECK(json_has(json, "\"subdirs\":[{\"name\":\"profiles\",\"file_count\":3,\"size_bytes\":18,"
                              "\"unknown_size\":0}]"),
               "profiles/ reports 3 files and 18 bytes");
    cfg_fs_deinit();
}

/* nvs_permanent must name every store that is NVS-only today (2026-10-07
 * audit, see cfg_fs_status.c). */
static void test_nvs_permanent_lists_current_stores(void)
{
    TEST_SECTION("cfg_fs_status: nvs_permanent names every NVS-only store, including the ones added after the "
                 "list was first written");
    cfg_fs_deinit();
    char json[4608];
    size_t len = 0;
    TEST_CHECK(cfg_fs_status_build_json(NULL, NULL, NULL, 0, NULL, json, sizeof(json), &len) == ESP_OK, "build");
    const char *perm = strstr(json, "\"nvs_permanent\":[");
    TEST_CHECK(perm != NULL, "nvs_permanent present");
    static const char *const want[] = { "profiles_favorites", "live_profile",          "firing_shadow",
                                        "ct_verify_store",    "kiln_cfg_swap",         "aux_convert_journal",
                                        "run_state_breadcrumb", "setup_wizard_progress", "pico_update_attempts",
                                        "pico_image_manifest", "estop_verification", "dualwrite_window" };
    for (size_t i = 0; perm && i < sizeof(want) / sizeof(want[0]); i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "\"%s\"", want[i]);
        const char *hit = strstr(perm, needle);
        const char *end = perm ? strchr(perm, ']') : NULL;
        TEST_CHECK(hit != NULL && end != NULL && hit < end, want[i]);
    }
}

void run_test_cfg_fs_status(void)
{
    test_unmounted();
    test_unavailable();
    test_mounted_empty();
    test_mounted_with_files();
    test_all_17_items_report_dualwrite_state();
    test_format_progress();
    test_format_stalled_ceiling();
    test_buffer_too_small();
    test_worst_case_fits_handler_buffer();
    test_subdirs_summarized();
    test_nvs_permanent_lists_current_stores();
    test_item_diverged_rule();
    cfg_fs_deinit();
}
