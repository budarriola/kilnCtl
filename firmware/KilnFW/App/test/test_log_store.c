// Host tests for log_store.c -- the rotating, capped on-flash store for
// firing/autotune logs (2026-09-01, "logs kept in external flash" gap).
//
// log_store.c is pure stdio (fopen/fread/fwrite/remove) with no ESP-IDF/
// FreeRTOS dependency at all, so these tests run it against a REAL temp
// directory on disk -- no filesystem stub needed (see log_store.h's own
// file banner for why: SPIFFS is mounted at a VFS path on-device, and
// standard C stdio already works transparently against any mounted VFS
// path, so this is exactly the code that runs on-device against "/logs").
//
// Its own executable (own $sources entry in build_host_tests.ps1, run via
// run_test_log_store() from test_main.c) -- no #include-the-.c-directly
// seam issue here, so it could have joined the main executable, but a
// filesystem-touching test gets its own working directory for the same
// isolation reason test_kiln_cfg_store.c/test_safety_cfg_store.c already
// use their own NVS stub instances.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TLS_MKDIR(p) _mkdir(p)
#define TLS_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TLS_MKDIR(p) mkdir((p), 0755)
#define TLS_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "../drivers/log_store.h"

/* Duplicates log_store.c's own private path-building convention on purpose:
 * these tests need to manufacture on-disk fixture state (pre-filled
 * segments, a blocked "directory where a file should be") that log_store's
 * public API has no way to set up directly, and to verify individual
 * segment files are actually gone after a rotation -- both require knowing
 * the exact on-disk layout, not just the API surface. */
static const char *kind_prefix(log_store_kind_t kind)
{
    return (kind == LOG_STORE_KIND_AUTOTUNE) ? "autotune" : "firing";
}

static void seg_path(const char *base, log_store_kind_t kind, size_t idx, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s_%010zu.log", base, kind_prefix(kind), idx);
}

static void manifest_path(const char *base, log_store_kind_t kind, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.manifest", base, kind_prefix(kind));
}

static void write_manifest(const char *base, log_store_kind_t kind, size_t oldest, size_t newest)
{
    char path[600];
    manifest_path(base, kind, path, sizeof(path));
    FILE *f = fopen(path, "w");
    fprintf(f, "%zu %zu 1\n", oldest, newest);
    fclose(f);
}

/* Fills a segment file with `size` bytes of harmless padding so its on-disk
 * size is exactly `size` -- used to manufacture "this segment is already
 * (near) full" fixture state without having to actually call
 * log_store_append() thousands of times to get there for real. */
static void fill_segment(const char *base, log_store_kind_t kind, size_t idx, size_t size)
{
    char path[600];
    seg_path(base, kind, idx, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    static const char pad[64] = "0123456789012345678901234567890123456789012345678901234567890\n";
    size_t written = 0;
    while (written < size) {
        size_t chunk = (size - written) < sizeof(pad) ? (size - written) : sizeof(pad);
        fwrite(pad, 1, chunk, f);
        written += chunk;
    }
    fclose(f);
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

static void rmdir_recursive_best_effort(const char *dir)
{
    /* Test-scratch cleanup only -- best effort, no assertion on failure. */
    (void)dir;
}

/* Wipes a test scratch directory back to empty BEFORE a case starts.
 * Necessary because these tests run against real files on real disk (the
 * whole point -- see this file's header comment) and this same directory
 * name is reused on every run of the host test binary: without this, a
 * PREVIOUS run's segment/manifest files stay on disk in append mode and
 * silently blend into the NEXT run's assertions -- exactly the kind of
 * false pass this repo's "PROVEN able to fail" rule exists to catch. Found
 * the hard way: the round-trip test read back 4 stale copies of its own 3
 * lines the first time this was skipped. */
static void reset_scratch_dir(const char *base, log_store_kind_t kind, size_t max_idx)
{
    char path[600];
    manifest_path(base, kind, path, sizeof(path));
    remove(path);
    for (size_t i = 0; i <= max_idx; i++) {
        seg_path(base, kind, i, path, sizeof(path));
        remove(path);       /* in case it is a file */
        TLS_RMDIR(path);    /* in case a previous run left it as a blocking directory */
    }
    TLS_RMDIR(base);
    TLS_MKDIR(base);
}

// ---------------------------------------------------------------------
// 1. Rotation bounds the on-disk size: manufacture the store already AT
//    its cap (LOG_STORE_MAX_SEGMENTS full segments), then write ENOUGH
//    MORE to force a real rotation past the cap, and assert the oldest
//    segment is actually gone and the total stays bounded. A test that
//    only writes less than the cap would never exercise trim_to_cap() at
//    all -- this one forces it to run for real, against real files.
// ---------------------------------------------------------------------
static void test_rotation_bounds_size(void)
{
    TEST_SECTION("log_store: rotation bounds on-disk size");

    const char *base = "log_store_test_rotation";
    log_store_kind_t kind = LOG_STORE_KIND_FIRING;
    reset_scratch_dir(base, kind, LOG_STORE_MAX_SEGMENTS + 4);
    TEST_CHECK(log_store_init(base) == ESP_OK, "init succeeds");

    /* Manufacture: LOG_STORE_MAX_SEGMENTS segments already on disk
     * (indices 0..MAX-1), the last one already at the per-segment cap so
     * the very next append is forced to rotate to a brand-new segment
     * (index MAX), pushing the retained count to MAX+1 -- exactly the
     * condition trim_to_cap() exists to correct. */
    size_t last_idx = LOG_STORE_MAX_SEGMENTS - 1;
    for (size_t i = 0; i < LOG_STORE_MAX_SEGMENTS; i++) {
        fill_segment(base, kind, i, i == last_idx ? LOG_STORE_SEGMENT_MAX_BYTES : (size_t)256);
    }
    write_manifest(base, kind, 0, last_idx);

    /* Reload so log_store picks up the manufactured state instead of
     * whatever it inferred on the first init() above (which saw nothing). */
    TEST_CHECK(log_store_init(base) == ESP_OK, "re-init picks up manufactured state");
    TEST_CHECK(log_store_segment_count(kind) == LOG_STORE_MAX_SEGMENTS, "starts exactly at the cap");

    char oldest_path[600];
    seg_path(base, kind, 0, oldest_path, sizeof(oldest_path));
    TEST_CHECK(file_exists(oldest_path), "oldest segment (index 0) exists before the push");

    /* This append cannot fit in the already-full last segment -> rotates to
     * a new segment (index MAX_SEGMENTS) -> count becomes MAX_SEGMENTS+1 ->
     * trim_to_cap() must delete index 0 to bring it back to MAX_SEGMENTS. */
    TEST_CHECK(log_store_append(kind, "the line that forces rotation past the cap") == ESP_OK,
               "append that forces rotation succeeds");

    TEST_CHECK(log_store_segment_count(kind) <= LOG_STORE_MAX_SEGMENTS,
               "segment count never exceeds the cap after rotation");
    TEST_CHECK(!file_exists(oldest_path), "oldest segment was actually deleted, not just uncounted");
    TEST_CHECK(log_store_total_bytes(kind) <= LOG_STORE_MAX_TOTAL_BYTES,
               "total retained bytes stays under the hard cap");

    rmdir_recursive_best_effort(base);
}

// ---------------------------------------------------------------------
// 2. A full/failing filesystem degrades safely: it must never wedge or
//    crash the caller. Simulated by putting a DIRECTORY exactly where the
//    next segment file needs to be created -- fopen(path, "a") on a
//    directory path fails predictably on both Windows and POSIX, which is
//    a portable stand-in for "the filesystem refused this write" without
//    needing to actually fill a whole disk.
// ---------------------------------------------------------------------
static void test_full_filesystem_degrades_safely(void)
{
    TEST_SECTION("log_store: full/failing filesystem degrades safely");

    const char *base = "log_store_test_full";
    log_store_kind_t kind = LOG_STORE_KIND_AUTOTUNE;
    reset_scratch_dir(base, kind, 4);
    TEST_CHECK(log_store_init(base) == ESP_OK, "init succeeds");

    TEST_CHECK(log_store_append(kind, "line before the obstruction") == ESP_OK,
               "a normal append succeeds first");

    /* Block the NEXT segment (index 1) with a directory of the same name,
     * then manufacture segment 0 as already full so the next append is
     * forced to try to create segment 1 -- and fail. */
    fill_segment(base, kind, 0, LOG_STORE_SEGMENT_MAX_BYTES);
    write_manifest(base, kind, 0, 0);
    TEST_CHECK(log_store_init(base) == ESP_OK, "re-init picks up the full segment 0");

    char blocked_path[600];
    seg_path(base, kind, 1, blocked_path, sizeof(blocked_path));
    TEST_CHECK(TLS_MKDIR(blocked_path) == 0, "test setup: directory created where segment 1 should be");

    /* This must NOT crash, hang, or assert -- it must simply report failure
     * and drop the line. Called several times in a row to prove it keeps
     * returning cleanly rather than wedging on the second call. */
    for (int i = 0; i < 5; i++) {
        esp_err_t rc = log_store_append(kind, "this line cannot be written -- segment 1 is blocked");
        TEST_CHECK(rc != ESP_OK, "append against a blocked segment reports failure, not success");
    }

    /* Recovery: once the obstruction is gone, the store must go on working
     * -- proving the earlier failures left no corrupted/stuck internal
     * state (the caller, telemetry_log.c, is never blocked or wedged by a
     * transient full-filesystem condition). */
    TEST_CHECK(TLS_RMDIR(blocked_path) == 0, "test cleanup: obstruction removed");
    TEST_CHECK(log_store_append(kind, "recovered after the obstruction was cleared") == ESP_OK,
               "append succeeds again once the obstruction is gone -- no wedge");

    rmdir_recursive_best_effort(base);
}

// ---------------------------------------------------------------------
// 3. Round-trip: what is written comes back byte-for-byte in the same
//    order, using an ASYMMETRIC fixture (three distinct, different-length
//    lines with no repeated structure) so an index/ordering bug fails
//    loudly instead of accidentally passing the way three identical or
//    palindromic lines could.
// ---------------------------------------------------------------------
static void test_round_trip_asymmetric(void)
{
    TEST_SECTION("log_store: round-trip returns the same values in the same order");

    const char *base = "log_store_test_roundtrip";
    log_store_kind_t kind = LOG_STORE_KIND_AUTOTUNE;
    reset_scratch_dir(base, kind, 2);
    TEST_CHECK(log_store_init(base) == ESP_OK, "init succeeds");

    const char *first = "KTEL1 TUNE state=IDENTIFY zone=0 t=12";
    const char *second = "second";
    const char *third = "KTEL1 TUNE state=DONE zone=2 gain_kp=1.2345 model_valid=1 abort_reason=none really long tail field";

    TEST_CHECK(log_store_append(kind, first) == ESP_OK, "append line 1");
    TEST_CHECK(log_store_append(kind, second) == ESP_OK, "append line 2");
    TEST_CHECK(log_store_append(kind, third) == ESP_OK, "append line 3");

    log_store_reader_t *rd = log_store_reader_open(kind);
    TEST_CHECK(rd != NULL, "reader opens");

    char out[LOG_STORE_MAX_LINE_LEN];

    TEST_CHECK(log_store_reader_next(rd, out, sizeof(out)) && strcmp(out, first) == 0,
               "line 1 comes back first and unchanged");
    TEST_CHECK(log_store_reader_next(rd, out, sizeof(out)) && strcmp(out, second) == 0,
               "line 2 comes back second and unchanged");
    TEST_CHECK(log_store_reader_next(rd, out, sizeof(out)) && strcmp(out, third) == 0,
               "line 3 comes back third and unchanged");
    TEST_CHECK(!log_store_reader_next(rd, out, sizeof(out)), "reader reports end-of-stream after 3 lines");

    log_store_reader_close(rd);
    rmdir_recursive_best_effort(base);
}

void run_test_log_store(void)
{
    test_rotation_bounds_size();
    test_full_filesystem_degrades_safely();
    test_round_trip_asymmetric();
}
