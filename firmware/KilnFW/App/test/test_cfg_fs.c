// Host tests for cfg_fs.c -- the foundation storage module for the `cfg`
// LittleFS partition (docs/FILESYSTEM_USER_DATA_PLAN.md sections 3/5, step
// 1-2). Same reasoning as test_log_store.c: cfg_fs.c is pure stdio with no
// ESP-IDF/FreeRTOS dependency, so these tests run it against a REAL temp
// directory on disk -- no filesystem stub needed, and this is exactly the
// code that will run on-device against "/cfg" once esp_vfs_littlefs_
// register() is wired up in cfg_fs_mount.c (device-only, not built here).
//
// Joins the main executable's $sources list in build_host_tests.ps1, same
// as test_log_store.c -- no #include-the-.c-directly seam issue, and each
// test case gets its own scratch directory for isolation.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TCF_MKDIR(p) _mkdir(p)
#define TCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TCF_MKDIR(p) mkdir((p), 0755)
#define TCF_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "../drivers/persist/cfg_fs.h"

/* Wipes and recreates a scratch base directory (plus its .tmp/ subdir, plus
 * one optional data subdirectory) before a test case starts -- same
 * "PROVEN able to fail" reasoning as test_log_store.c's reset_scratch_dir():
 * this binary reuses the same on-disk paths across runs, so a previous
 * run's leftovers must never leak into the next assertion. */
static void reset_scratch(const char *base, const char *extra_subdir)
{
    char path[600];

    if (extra_subdir) {
        snprintf(path, sizeof(path), "%s/%s", base, extra_subdir);
        /* best-effort: only ever holds a handful of small files in these tests */
        TCF_RMDIR(path);
    }
    /* Defensive: a prior FAILING run (e.g. this file's own negative-test
     * break, run by hand) can leave a stray obstruction directory sitting
     * INSIDE .tmp/, which would make plain TCF_RMDIR(".tmp") fail (not
     * empty) and strand the whole scratch dir dirty for every later run.
     * Known obstruction names used by tests in this file are cleared
     * explicitly before the plain rmdir is attempted. */
    snprintf(path, sizeof(path), "%s/.tmp/relay_cycles.json", base);
    TCF_RMDIR(path);
    snprintf(path, sizeof(path), "%s/.tmp/zones.json", base);
    remove(path);
    snprintf(path, sizeof(path), "%s/.tmp", base);
    TCF_RMDIR(path);
    TCF_RMDIR(base);
    TCF_MKDIR(base);

    cfg_fs_deinit();
}

static bool raw_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------
// 1. Basic mount + write/read/exists/delete round trip through the real
//    public API.
// ---------------------------------------------------------------------
static void test_mount_and_round_trip(void)
{
    TEST_SECTION("cfg_fs: mount, then write/read/exists/delete round trip");

    const char *base = "cfg_fs_test_roundtrip";
    reset_scratch(base, NULL);

    TEST_CHECK(cfg_fs_get_status() == CFG_FS_STATUS_UNMOUNTED, "starts unmounted");
    TEST_CHECK(!cfg_fs_is_available(), "not available before init");

    size_t reaped = 999;
    TEST_CHECK(cfg_fs_init(base, &reaped) == ESP_OK, "init succeeds against a real, existing directory");
    TEST_CHECK(reaped == 0, "nothing to reap on a fresh directory");
    TEST_CHECK(cfg_fs_get_status() == CFG_FS_STATUS_MOUNTED, "status becomes MOUNTED");
    TEST_CHECK(cfg_fs_is_available(), "is_available() true once mounted");

    bool exists = true;
    TEST_CHECK(cfg_fs_exists("prefs.json", &exists) == ESP_OK && !exists, "file does not exist yet");

    static const char payload[] = "{\"schema\":1,\"unit\":\"C\"}";
    TEST_CHECK(cfg_fs_write_atomic("prefs.json", payload, sizeof(payload)) == ESP_OK, "write succeeds");

    TEST_CHECK(cfg_fs_exists("prefs.json", &exists) == ESP_OK && exists, "file exists after write");

    char readback[128];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("prefs.json", readback, sizeof(readback), &out_len) == ESP_OK &&
                   out_len == sizeof(payload) && memcmp(readback, payload, sizeof(payload)) == 0,
               "read returns exactly what was written");

    TEST_CHECK(cfg_fs_delete("prefs.json") == ESP_OK, "delete succeeds");
    TEST_CHECK(cfg_fs_exists("prefs.json", &exists) == ESP_OK && !exists, "file gone after delete");
    TEST_CHECK(cfg_fs_delete("prefs.json") == ESP_ERR_NOT_FOUND, "deleting again reports not-found, not success");

    // A real I/O failure (remove() of a non-empty directory: ENOTEMPTY on
    // POSIX, EACCES on Windows) must be ESP_FAIL, never NOT_FOUND.
    char blocker[600];
    snprintf(blocker, sizeof(blocker), "%s/blocker", base);
    TCF_RMDIR(blocker);
    TEST_CHECK(TCF_MKDIR(blocker) == 0, "test setup: a directory exists at the delete target");
    char inner[640];
    snprintf(inner, sizeof(inner), "%s/inner.txt", blocker);
    FILE *f = fopen(inner, "wb");
    TEST_CHECK(f != NULL, "test setup: the directory is non-empty");
    if (f) {
        fclose(f);
    }
    TEST_CHECK(cfg_fs_delete("blocker") == ESP_FAIL, "a remove() failure other than ENOENT reports ESP_FAIL");
    remove(inner);
    TCF_RMDIR(blocker);
}

// ---------------------------------------------------------------------
// 2. A second write to the same path REPLACES the first -- proves the
//    atomic path handles overwrite (not just create), which on Windows
//    specifically requires MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)
//    rather than a bare rename() (which fails if the destination exists).
// ---------------------------------------------------------------------
static void test_overwrite_replaces_old_content(void)
{
    TEST_SECTION("cfg_fs: atomic write overwrites existing content, not just creates new files");

    const char *base = "cfg_fs_test_overwrite";
    reset_scratch(base, NULL);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "init succeeds");

    static const char v1[] = "version one, a longer original payload to overwrite";
    static const char v2[] = "v2";
    TEST_CHECK(cfg_fs_write_atomic("doc.json", v1, sizeof(v1)) == ESP_OK, "first write succeeds");
    TEST_CHECK(cfg_fs_write_atomic("doc.json", v2, sizeof(v2)) == ESP_OK, "second (overwriting) write succeeds");

    char buf[128];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("doc.json", buf, sizeof(buf), &out_len) == ESP_OK && out_len == sizeof(v2) &&
                   memcmp(buf, v2, sizeof(v2)) == 0,
               "readback is v2 only -- no leftover trailing bytes from the longer v1");
}

// ---------------------------------------------------------------------
// 3. NEGATIVE-TEST TARGET: an interrupted write must never corrupt the
//    final file. Simulated the same way a real power-loss would land: a
//    temp file is left on disk, fully written but the rename step never
//    ran (process died between fsync and rename). The OLD final file must
//    still read back correctly, byte for byte, and cfg_fs_init()'s .tmp/
//    sweep must remove the orphaned temp file and report it reaped.
//
//    This is the test the FILESYSTEM_USER_DATA_PLAN.md task explicitly
//    calls out to run against a DELIBERATELY BROKEN cfg_fs_write_atomic()
//    (see the accompanying negative-test note in the commit/report) --
//    the assertions below are written against the real production
//    function's documented contract, not a test-local mirror of it, so
//    breaking cfg_fs.c's actual rename call is what makes this fail.
// ---------------------------------------------------------------------
static void test_interrupted_write_never_corrupts_old_file(void)
{
    TEST_SECTION("cfg_fs: a write interrupted before rename leaves the OLD file intact, and the sweep reaps "
                 "the orphaned temp file");

    const char *base = "cfg_fs_test_atomic";
    reset_scratch(base, NULL);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "init succeeds");

    static const char original[] = "the original, already-committed content that must survive";
    TEST_CHECK(cfg_fs_write_atomic("zones.json", original, sizeof(original)) == ESP_OK,
               "the original write completes normally");

    /* Simulate "process died between fsync and rename": manufacture an
     * orphaned temp file directly on disk, exactly where cfg_fs_write_
     * atomic()'s own flatten_for_tmp() convention would have put it, WITHOUT
     * going through the public API (there is no API to stop halfway through
     * a write -- that is the point: this reproduces the on-disk STATE a
     * crash leaves, not the code path). */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp/zones.json", base);
    FILE *f = fopen(tmp_path, "wb");
    TEST_CHECK(f != NULL, "test setup: orphaned temp file created");
    static const char partial[] = "only some of the new bytes made it to disk before the crash";
    if (f) {
        fwrite(partial, 1, sizeof(partial), f);
        fclose(f);
    }

    char final_path[600];
    snprintf(final_path, sizeof(final_path), "%s/zones.json", base);
    TEST_CHECK(raw_file_exists(final_path), "the final path still exists (rename never ran)");

    char buf[256];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("zones.json", buf, sizeof(buf), &out_len) == ESP_OK && out_len == sizeof(original) &&
                   memcmp(buf, original, sizeof(original)) == 0,
               "readback through the real API is still the ORIGINAL content, untouched by the orphaned "
               "temp file -- never a truncated/partial file at the final path");

    /* Now prove the sweep: re-init (as a real boot would after the crash)
     * must remove the orphaned temp file and report exactly one reaped. */
    cfg_fs_deinit();
    size_t reaped = 0;
    TEST_CHECK(cfg_fs_init(base, &reaped) == ESP_OK, "re-init after the simulated crash succeeds");
    TEST_CHECK(reaped == 1, "exactly one stale temp file was reaped");
    TEST_CHECK(!raw_file_exists(tmp_path), "the orphaned temp file is actually gone from disk, not just uncounted");

    TEST_CHECK(cfg_fs_read("zones.json", buf, sizeof(buf), &out_len) == ESP_OK && out_len == sizeof(original) &&
                   memcmp(buf, original, sizeof(original)) == 0,
               "the original content is still intact after the sweep");
}

// ---------------------------------------------------------------------
// 3b. NEGATIVE-TEST TARGET (direct hit on the rename-based mechanism
//     itself, not just its aftermath): obstruct the TEMP file's location
//     with a directory of the same name, so cfg_fs_write_atomic()'s own
//     fopen(tmp_path, "wb") call fails. This must report failure AND leave
//     the pre-existing final file completely untouched.
//
//     A write_atomic() that (wrongly) opened `final_path` directly instead
//     of going through `.tmp/` would sail right past this obstruction --
//     it targets a different location -- and would happily clobber the old
//     content with the new (possibly truncated) write, returning ESP_OK.
//     That is exactly the class of implementation this test exists to
//     catch; see this task's negative-test note for a real run of it
//     against a deliberately broken cfg_fs_write_atomic().
// ---------------------------------------------------------------------
static void test_obstructed_temp_write_leaves_old_file_untouched(void)
{
    TEST_SECTION("cfg_fs: NEGATIVE TARGET -- obstructing the temp file must fail the write and leave the old "
                 "final file byte-for-byte intact");

    const char *base = "cfg_fs_test_obstruct";
    reset_scratch(base, NULL);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "init succeeds");

    static const char original[] = "old committed content that a failed write must never disturb";
    TEST_CHECK(cfg_fs_write_atomic("relay_cycles.json", original, sizeof(original)) == ESP_OK,
               "the original write completes normally");

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp/relay_cycles.json", base);
    TCF_RMDIR(tmp_path); /* defensive: clear any leftover obstruction from a previous run that failed midway */
    TEST_CHECK(TCF_MKDIR(tmp_path) == 0, "test setup: a directory now sits where the temp file needs to go");

    static const char attempted_new[] = "X";
    esp_err_t rc = cfg_fs_write_atomic("relay_cycles.json", attempted_new, sizeof(attempted_new));
    TEST_CHECK(rc != ESP_OK, "write_atomic() reports failure when it cannot create its own temp file");

    char buf[256];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("relay_cycles.json", buf, sizeof(buf), &out_len) == ESP_OK &&
                   out_len == sizeof(original) && memcmp(buf, original, sizeof(original)) == 0,
               "the OLD content is still exactly what it was -- a failed write never reached the final path");

    TCF_RMDIR(tmp_path);
}

// ---------------------------------------------------------------------
// 4. Mount failure (base directory does not exist / cannot be listed)
//    degrades cleanly: status becomes UNAVAILABLE, every call fails with
//    ESP_ERR_INVALID_STATE, nothing crashes or blocks.
// ---------------------------------------------------------------------
static void test_mount_failure_degrades_cleanly(void)
{
    TEST_SECTION("cfg_fs: mount failure (missing base dir) degrades to UNAVAILABLE, never crashes");

    cfg_fs_deinit();
    const char *missing = "cfg_fs_test_this_directory_does_not_exist";
    TCF_RMDIR(missing); /* make sure it really is absent */

    TEST_CHECK(cfg_fs_init(missing, NULL) != ESP_OK, "init against a nonexistent directory fails");
    TEST_CHECK(cfg_fs_get_status() == CFG_FS_STATUS_UNAVAILABLE, "status is UNAVAILABLE, not MOUNTED");
    TEST_CHECK(!cfg_fs_is_available(), "is_available() is false");

    bool exists = true;
    TEST_CHECK(cfg_fs_exists("anything.json", &exists) == ESP_ERR_INVALID_STATE,
               "exists() fails clean rather than touching an uninitialized base path");
    char buf[16];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("anything.json", buf, sizeof(buf), &out_len) == ESP_ERR_INVALID_STATE, "read() fails clean");
    TEST_CHECK(cfg_fs_write_atomic("anything.json", "x", 1) == ESP_ERR_INVALID_STATE, "write_atomic() fails clean");
    TEST_CHECK(cfg_fs_delete("anything.json") == ESP_ERR_INVALID_STATE, "delete() fails clean");
    cfg_fs_entry_t entries[4];
    size_t count = 999;
    TEST_CHECK(cfg_fs_list("", entries, 4, &count) == ESP_ERR_INVALID_STATE && count == 0,
               "list() fails clean and reports zero entries");

    cfg_fs_deinit();
}

// ---------------------------------------------------------------------
// 5. Recovery mode skips the mount ENTIRELY through the real
//    cfg_fs_mount_or_skip() gate -- the same function the device glue
//    (cfg_fs_mount.c, not host-built) calls with
//    boot_guard_is_recovery_mode() in place of this test's literal `true`.
// ---------------------------------------------------------------------
static void test_recovery_mode_skips_mount(void)
{
    TEST_SECTION("cfg_fs: recovery mode skips the mount entirely (real gate function, not a mirror)");

    const char *base = "cfg_fs_test_recovery";
    reset_scratch(base, NULL);

    TEST_CHECK(cfg_fs_mount_or_skip(true, base, NULL) == ESP_OK, "mount_or_skip(recovery=true) returns OK");
    TEST_CHECK(cfg_fs_get_status() == CFG_FS_STATUS_UNMOUNTED,
               "status stays UNMOUNTED -- cfg_fs_init() was never called, even though `base` is a real, "
               "perfectly mountable directory");
    TEST_CHECK(!cfg_fs_is_available(), "not available under recovery mode");
    TEST_CHECK(cfg_fs_skipped_for_recovery(), "the skip is recorded as a recovery-mode skip");
    TEST_CHECK(strcmp(cfg_fs_not_mounted_text(), CFG_FS_RECOVERY_SKIPPED_TEXT) == 0,
               "recovery-mode refusal text is the recovery variant");
    TEST_CHECK(strstr(cfg_fs_not_mounted_text(), "format_confirm") == NULL,
               "recovery-mode refusal text never points at the format route: the partition still holds "
               "the only saved config after the NVS dual-write close");
    TEST_CHECK(strstr(cfg_fs_not_mounted_text(), "/api/ota/esp/recovery_exit") != NULL,
               "recovery-mode refusal text names the recovery_exit route");

    /* Same real directory, recovery=false this time -> mounts normally.
     * Proves the skip above was really the recovery flag, not something
     * wrong with `base` itself. */
    TEST_CHECK(cfg_fs_mount_or_skip(false, base, NULL) == ESP_OK, "mount_or_skip(recovery=false) mounts");
    TEST_CHECK(cfg_fs_get_status() == CFG_FS_STATUS_MOUNTED, "status is MOUNTED once recovery mode is off");
    TEST_CHECK(!cfg_fs_skipped_for_recovery(), "a normal mount clears the recovery-skip flag");

    cfg_fs_deinit();
    TEST_CHECK(!cfg_fs_skipped_for_recovery(), "deinit leaves the recovery-skip flag clear");
    TEST_CHECK(strcmp(cfg_fs_not_mounted_text(), CFG_FS_NOT_MOUNTED_TEXT) == 0,
               "outside recovery mode the refusal text is the format-confirm one");
}

// ---------------------------------------------------------------------
// 6. list()/exists() round trip across several files, including the
//    one-level-nested "profiles/<id>.json" shape the plan's layout uses,
//    and confirms `.tmp/` itself never shows up in a listing.
// ---------------------------------------------------------------------
static void test_list_round_trip_with_nesting(void)
{
    TEST_SECTION("cfg_fs: list() enumerates files, including one level of nesting, and excludes .tmp");

    const char *base = "cfg_fs_test_list";
    reset_scratch(base, "profiles");
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "init succeeds");

    TEST_CHECK(cfg_fs_write_atomic("prefs.json", "a", 1) == ESP_OK, "write prefs.json");
    TEST_CHECK(cfg_fs_write_atomic("tune.json", "b", 1) == ESP_OK, "write tune.json");
    TEST_CHECK(cfg_fs_write_atomic("profiles/0.json", "c", 1) == ESP_OK,
               "write profiles/0.json -- one level of nesting, parent dir auto-created");
    TEST_CHECK(cfg_fs_write_atomic("profiles/1.json", "d", 1) == ESP_OK, "write profiles/1.json");

    cfg_fs_entry_t top[8];
    size_t top_count = 0;
    TEST_CHECK(cfg_fs_list("", top, 8, &top_count) == ESP_OK, "list base dir succeeds");
    TEST_CHECK(top_count == 2, "exactly the two top-level files are listed (prefs.json, tune.json) -- "
                               "'profiles' subdirectory and '.tmp' are both excluded");

    cfg_fs_entry_t profs[8];
    size_t prof_count = 0;
    TEST_CHECK(cfg_fs_list("profiles", profs, 8, &prof_count) == ESP_OK, "list profiles/ succeeds");
    TEST_CHECK(prof_count == 2, "both profile files are listed");

    /* max_out clamping: request room for only 1 even though 2 exist. */
    cfg_fs_entry_t small[1];
    size_t small_count = 999;
    TEST_CHECK(cfg_fs_list("profiles", small, 1, &small_count) == ESP_OK && small_count == 1,
               "list() clamps to max_out without overflowing the caller's array");
}

// ---------------------------------------------------------------------
// 7. write_atomic()'s post-rename read-back verify streams the comparison
//    through a small fixed chunk rather than malloc()'ing the whole blob
//    (docs/audits/heap_low_water_9051_2026-09-21.md). This payload is
//    several multiples of that chunk size plus a remainder, with a
//    distinct byte pattern so a chunk-boundary bug (off-by-one in the
//    stream loop, a chunk silently skipped, `expected`/`chunk` pointer
//    arithmetic wrong) would corrupt the comparison rather than coincide
//    with all-zero or repeating bytes.
// ---------------------------------------------------------------------
static void test_write_large_payload_spans_multiple_verify_chunks(void)
{
    TEST_SECTION("cfg_fs: write_atomic() verifies a payload spanning multiple readback chunks");

    const char *base = "cfg_fs_test_largeverify";
    reset_scratch(base, NULL);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "init succeeds");

    /* 256-byte chunk size in cfg_fs.c: use a length that is neither an
     * exact multiple nor smaller than one chunk. */
    static uint8_t payload[900];
    for (size_t i = 0; i < sizeof(payload); i++) {
        /* (i*37+5) alone is periodic with period 256 (37 is odd, so mod 256
         * it cycles every 256 indices) -- that would make every 256-byte
         * chunk of this payload byte-identical to the others, which could
         * mask a bug that compares the wrong chunk against the wrong
         * offset. The `(i >> 8) * 91` term breaks that periodicity so each
         * chunk's content is genuinely distinct from every other chunk's. */
        payload[i] = (uint8_t)((i * 37 + 5 + (i >> 8) * 91) & 0xFF);
    }

    TEST_CHECK(cfg_fs_write_atomic("big.bin", payload, sizeof(payload)) == ESP_OK,
               "write_atomic succeeds and its internal read-back verify passes across chunk boundaries");

    uint8_t readback[sizeof(payload)];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("big.bin", readback, sizeof(readback), &out_len) == ESP_OK &&
                   out_len == sizeof(payload) && memcmp(readback, payload, sizeof(payload)) == 0,
               "the full multi-chunk payload reads back byte-for-byte identical");
}

// ---------------------------------------------------------------------
// 8. write_atomic()'s early-rejection path, after the path buffers moved
//    off the stack and into one heap-allocated cfg_fs_write_scratch_t
//    (docs/audits/bx_flash_worker_panic_after_cfgfs_format_2026-09-21.md --
//    a real hardware stack overflow in bx_flash_worker on the first cfg
//    write after a format). That refactor turned a plain `return
//    ESP_ERR_INVALID_ARG` into a free-then-return branch, so a rejection is
//    now a place a leak or a use-after-free can hide, and -- much more
//    importantly -- a place where an early return must STILL leave any
//    existing file completely untouched. All four malformed inputs below
//    are refused by split_one_level(), so they exercise that one
//    free-then-return branch four different ways, not four distinct
//    branches; the path_join/flatten and ESP_ERR_NO_MEM free-then-return
//    branches are not covered here. Nothing covered this rejection path
//    before; the pre-existing tests only ever pass well-formed relative
//    paths.
// ---------------------------------------------------------------------
static void test_write_atomic_rejections_leave_existing_file_untouched(void)
{
    TEST_SECTION("cfg_fs: write_atomic() rejects malformed rel_paths and leaves the existing file intact");

    const char *base = "cfg_fs_test_reject";
    reset_scratch(base, NULL);
    TEST_CHECK(cfg_fs_init(base, NULL) == ESP_OK, "init succeeds");

    TEST_CHECK(cfg_fs_write_atomic("keep.dat", "GOOD", 4) == ESP_OK, "the file that must survive is written");

    /* A PARENT directory component at/over CFG_FS_MAX_NAME: split_one_level()
     * refuses on `dir_len >= dir_cap`, which is the FIRST free-then-return
     * branch after the scratch allocation and the one that writes into
     * sc->parent_rel. (A long BARE name is NOT refused here -- split_one_level()
     * only bounds the directory component -- so this case, not that one, is
     * what actually exercises the branch.) */
    char longparent[CFG_FS_MAX_NAME + 8];
    memset(longparent, 'x', sizeof(longparent) - 1);
    longparent[sizeof(longparent) - 1] = '\0';
    char longpath[CFG_FS_MAX_NAME + 20];
    snprintf(longpath, sizeof(longpath), "%s/f.dat", longparent);
    TEST_CHECK(cfg_fs_write_atomic(longpath, "BAD", 3) == ESP_ERR_INVALID_ARG,
               "an over-length parent directory component is refused with INVALID_ARG");

    /* Two levels of nesting: split_one_level() allows exactly one. */
    TEST_CHECK(cfg_fs_write_atomic("a/b/c.json", "BAD", 3) == ESP_ERR_INVALID_ARG,
               "two levels of nesting are refused with INVALID_ARG");

    /* A trailing slash has no bare filename at all. */
    TEST_CHECK(cfg_fs_write_atomic("dir/", "BAD", 3) == ESP_ERR_INVALID_ARG,
               "a rel_path with no bare filename is refused with INVALID_ARG");

    /* An empty rel_path. */
    TEST_CHECK(cfg_fs_write_atomic("", "BAD", 3) == ESP_ERR_INVALID_ARG,
               "an empty rel_path is refused with INVALID_ARG");

    /* The whole point: none of those rejections may have disturbed the
     * filesystem. The surviving file must still hold its original bytes,
     * and no stray temp entry may have been left behind. */
    char back[16];
    size_t out_len = 0;
    TEST_CHECK(cfg_fs_read("keep.dat", back, sizeof(back), &out_len) == ESP_OK && out_len == 4 &&
                   memcmp(back, "GOOD", 4) == 0,
               "keep.dat still holds its original bytes after every rejected write");

    cfg_fs_entry_t top[8];
    size_t top_count = 0;
    TEST_CHECK(cfg_fs_list("", top, 8, &top_count) == ESP_OK && top_count == 1,
               "exactly one file exists -- no rejected write created or orphaned anything");
}

void run_test_cfg_fs(void)
{
    test_mount_and_round_trip();
    test_overwrite_replaces_old_content();
    test_interrupted_write_never_corrupts_old_file();
    test_obstructed_temp_write_leaves_old_file_untouched();
    test_mount_failure_degrades_cleanly();
    test_recovery_mode_skips_mount();
    test_list_round_trip_with_nesting();
    test_write_large_payload_spans_multiple_verify_chunks();
    test_write_atomic_rejections_leave_existing_file_untouched();

    cfg_fs_deinit();
}
