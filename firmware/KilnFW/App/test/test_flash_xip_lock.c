// REVIEW_FLASH_MMAP_SERIALIZATION MED-1: every SPI1 os_func start/end bracket and every
// mmap-reader bracket hold the same lock; a failed start releases it; uninstalled is a
// pass-through.
#define FLASH_XIP_LOCK_CORE_TEST 1
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "flash_xip_lock.h"

int g_test_failures = 0;
int g_test_count = 0;

static char g_ev[64];
static int  g_nev;
static int  g_depth;
static int  g_start_rc;

static void ev(char c) { if (g_nev < (int)sizeof(g_ev) - 1) g_ev[g_nev++] = c; g_ev[g_nev] = 0; }
static void take(void) { g_depth++; ev('T'); }
static void give(void) { g_depth--; ev('G'); }
static const flash_xip_lock_ops_t s_ops = { take, give };

static int orig_start(void *arg, uint32_t flags)
{
    TEST_CHECK(arg == (void *)0x1234 && flags == 7u, "orig start gets the caller's arg and flags");
    ev(g_depth > 0 ? 'S' : 's'); /* 'S' = ran under the lock */
    return g_start_rc;
}
static int orig_end(void *arg) { (void)arg; ev(g_depth > 0 ? 'E' : 'e'); return 0; }

static void reset(void) { g_nev = 0; g_ev[0] = 0; g_depth = 0; g_start_rc = 0; }

static void test_bracket_holds_lock(void)
{
    reset();
    flash_xip_lock_set_ops(&s_ops);
    int rc = flash_xip_lock_wrap_start(orig_start, (void *)0x1234, 7u);
    TEST_CHECK(rc == 0, "start ok");
    TEST_CHECK(g_depth == 1, "lock held between start and end");
    flash_xip_lock_wrap_end(orig_end, (void *)0x1234);
    TEST_CHECK(strcmp(g_ev, "TSEG") == 0, "take, start, end, give in that order");
    TEST_CHECK(g_depth == 0, "lock released after end");
}

static void test_failed_start_releases(void)
{
    reset();
    flash_xip_lock_set_ops(&s_ops);
    g_start_rc = 0x103;
    int rc = flash_xip_lock_wrap_start(orig_start, (void *)0x1234, 7u);
    TEST_CHECK(rc == 0x103, "start error propagated");
    TEST_CHECK(g_depth == 0, "failed start releases the lock (IDF skips end)");
    TEST_CHECK(strcmp(g_ev, "TSG") == 0, "take, start, give");
}

static void test_reader_excludes_and_nests(void)
{
    reset();
    flash_xip_lock_set_ops(&s_ops);
    flash_xip_reader_enter();
    TEST_CHECK(g_depth == 1, "reader holds the lock");
    /* A reader that also writes (mark-valid, esp_ota_end) re-enters from its own task. */
    flash_xip_lock_wrap_start(orig_start, (void *)0x1234, 7u);
    flash_xip_lock_wrap_end(orig_end, (void *)0x1234);
    TEST_CHECK(g_depth == 1, "nested SPI1 op leaves the reader's hold intact");
    flash_xip_reader_exit();
    TEST_CHECK(g_depth == 0, "reader exit releases");
    TEST_CHECK(strcmp(g_ev, "TTSEGG") == 0, "reader T, nested TSEG, reader G");
}

static void test_uninstalled_passthrough(void)
{
    reset();
    flash_xip_lock_set_ops(NULL);
    flash_xip_reader_enter();
    flash_xip_lock_wrap_start(orig_start, (void *)0x1234, 7u);
    flash_xip_lock_wrap_end(orig_end, (void *)0x1234);
    flash_xip_reader_exit();
    TEST_CHECK(strcmp(g_ev, "se") == 0, "no ops: original start/end only, no lock calls");
}

int main(void)
{
    TEST_SECTION("flash_xip_lock: SPI1 ops vs mmap readers (REVIEW_FLASH_MMAP_SERIALIZATION)");
    test_bracket_holds_lock();
    test_failed_start_releases();
    test_reader_excludes_and_nests();
    test_uninstalled_passthrough();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
