// Minimal host-test harness -- copied from firmware/KilnFW/App/test/test_common.h
// verbatim (ARCHITECTURE.md section 10 says to reuse it if it's generic
// enough, and it is: no framework dependency, just enough to run as a
// single console executable and return a real exit code for CI).
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <math.h>
#include <stdio.h>

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
