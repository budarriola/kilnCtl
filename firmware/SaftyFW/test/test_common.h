// Minimal host-test harness -- copied from firmware/KilnFW/App/test/test_common.h
// verbatim (ARCHITECTURE.md section 10 says to reuse it if it's generic
// enough, and it is: no framework dependency, just enough to run as a
// single console executable and return a real exit code for CI).
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ported from firmware/KilnFW/App/test/test_common.h's own copy (2026-09-04
 * CWD-independence fix): several source-text-scanning tests here fopen()
 * a source file located via a small list of CWD-relative candidate paths
 * ("../src/tasks/foo.c", "src/tasks/foo.c") that only covers running the
 * test binary from this test/ directory or from firmware/SaftyFW/ -- from
 * the repo root, or any other directory, every one of them "could not
 * locate" its target. __FILE__ is filled in by the compiler at compile
 * time from the path handed to it, so the directory it names does not
 * depend on the CWD the resulting .exe is later run from -- anchor to it
 * instead. */
static inline char *test_resolve_from_here(const char *this_file, const char *rel)
{
    const char *slash = strrchr(this_file, '/');
    const char *bslash = strrchr(this_file, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    size_t dirlen = slash ? (size_t)(slash - this_file) : 0;
    size_t rellen = strlen(rel);
    size_t total = dirlen + 1 /* separator */ + rellen + 1 /* NUL */;
    char *out = (char *)malloc(total);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    if (dirlen) {
        memcpy(out + pos, this_file, dirlen);
        pos += dirlen;
        out[pos++] = '/';
    }
    memcpy(out + pos, rel, rellen);
    pos += rellen;
    out[pos] = '\0';
    return out;
}

/* Reads the whole file at `path` into a malloc'd, NUL-terminated buffer.
 * Returns NULL (without printing anything) if it cannot be opened/read/
 * sized -- callers are expected to try further candidates and only report
 * a hard failure once all of them are exhausted. */
static inline char *test_read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            buf = (char *)malloc((size_t)len + 1);
            if (buf) {
                size_t got = fread(buf, 1, (size_t)len, f);
                buf[got] = '\0';
            }
        }
    }
    fclose(f);
    return buf;
}

/* Locates a source file for a text-scanning host test regardless of the
 * working directory the test binary is launched from: first tries the
 * path anchored to this test file's own on-disk location (correct for ANY
 * CWD), then falls back to trying each of `candidates` as given (kept as a
 * defensive second layer for the CWDs those literal, unanchored relative
 * paths already happened to cover). `this_file` is always __FILE__ from
 * the calling test's own translation unit; `anchor_rel` is that same path
 * family's entry as written relative to this test file's directory (e.g.
 * "../src/tasks/safety_core.c"). Returns a malloc'd string the caller must
 * free(), or NULL if every candidate failed -- callers must treat NULL as
 * a hard TEST_CHECK(false, ...) failure, never a silent skip. */
static inline char *test_read_source_anchored(const char *this_file, const char *anchor_rel,
                                               const char *const *candidates, size_t count)
{
    char *anchored_path = test_resolve_from_here(this_file, anchor_rel);
    if (anchored_path) {
        char *text = test_read_whole_file(anchored_path);
        free(anchored_path);
        if (text) {
            return text;
        }
    }
    for (size_t i = 0; i < count; i++) {
        char *text = test_read_whole_file(candidates[i]);
        if (text) {
            return text;
        }
    }
    return NULL;
}

/* Defined once in test_main.c -- each test_*.c includes this header, and a
 * `static` counter here would give every translation unit its own private
 * copy (internal linkage), silently zeroing out cross-file totals. */
extern int g_test_failures;
extern int g_test_count;

#define TEST_CHECK(cond, msg)                                                                        \
    do {                                                                                              \
        g_test_count++;                                                                               \
        if (!(cond)) {                                                                                \
            g_test_failures++;                                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                    \
        }                                                                                              \
    } while (0)

#define TEST_CHECK_NEAR(actual, expected, tol, msg)                                                   \
    do {                                                                                              \
        g_test_count++;                                                                               \
        double a_ = (double)(actual);                                                                 \
        double e_ = (double)(expected);                                                                \
        double t_ = (double)(tol);                                                                     \
        if (fabs(a_ - e_) > t_) {                                                                      \
            g_test_failures++;                                                                         \
            printf("  FAIL %s:%d: %s (got %.4f, want %.4f +/-%.4f)\n", __FILE__, __LINE__, msg, a_, e_, t_); \
        }                                                                                              \
    } while (0)

#define TEST_SECTION(name) printf("-- %s --\n", name)

#endif // TEST_COMMON_H
