/* Host tests for startup_faults.c and readiness_startup_status() -- the M13
 * sweep's "a required task failed to start" surface (startup_faults.h). */

#include <string.h>

#include "test_common.h"
#include "../drivers/common/startup_faults.h"
#include "../drivers/http/readiness_http.h"

void run_test_startup_faults(void)
{
    TEST_SECTION("startup_faults -- failed-to-start latch and readiness item");

    startup_fault_reset_for_test();
    TEST_CHECK(startup_fault_count() == 0, "a fresh boot has no startup faults");
    TEST_CHECK(readiness_startup_status(startup_fault_count()) == READY_OK, "no faults reads ok");

    /* Every id has a label and a consequence; a short table is a build-time
     * hole that would print "(null)" to an operator. */
    for (int i = 0; i < (int)STARTUP_FAULT_COUNT; i++) {
        TEST_CHECK(startup_fault_name((startup_fault_t)i) != NULL && startup_fault_name((startup_fault_t)i)[0],
                   "every id has a name");
        TEST_CHECK(startup_fault_impact((startup_fault_t)i) != NULL && startup_fault_impact((startup_fault_t)i)[0],
                   "every id has an impact string");
        /* readiness_http.c's detail[192] holds 17 + 63 + 4 + impact; keep every
         * impact short enough that the worst case still fits. */
        TEST_CHECK(strlen(startup_fault_impact((startup_fault_t)i)) <= 100, "impact fits the readiness detail budget");
        TEST_CHECK(strlen(startup_fault_name((startup_fault_t)i)) <= 40, "name is short enough to summarize");
    }
    TEST_CHECK(startup_fault_name(STARTUP_FAULT_COUNT) == NULL, "out-of-range name is NULL");

    startup_fault_note(STARTUP_FAULT_EXEC_WATCHDOG);
    startup_fault_note(STARTUP_FAULT_EXEC_WATCHDOG); /* idempotent */
    TEST_CHECK(startup_fault_is_set(STARTUP_FAULT_EXEC_WATCHDOG), "noted fault is set");
    TEST_CHECK(!startup_fault_is_set(STARTUP_FAULT_LOG_STORE), "other faults stay clear");
    TEST_CHECK(startup_fault_count() == 1, "noting twice counts once");
    TEST_CHECK(readiness_startup_status(startup_fault_count()) == READY_NOT_DONE, "any fault reads not_done");

    startup_fault_note(STARTUP_FAULT_COUNT); /* ignored */
    startup_fault_note((startup_fault_t)-1);  /* ignored */
    TEST_CHECK(startup_fault_count() == 1, "out-of-range notes are ignored");
    TEST_CHECK((int)STARTUP_FAULT_COUNT == 25, "25 startup fault ids (13 sweep-3 + 12 sweep-4)");
    startup_fault_note(STARTUP_FAULT_TOUCH); /* last id */
    TEST_CHECK(startup_fault_is_set(STARTUP_FAULT_TOUCH) && startup_fault_count() == 2, "the last id latches");
    startup_fault_note(STARTUP_FAULT_BOOT_GUARD_NVS);
    TEST_CHECK(startup_fault_is_set(STARTUP_FAULT_BOOT_GUARD_NVS), "a sweep-4 id latches");
    unsigned fresh_count = startup_fault_count();
    TEST_CHECK(fresh_count == 3, "three distinct ids counted");
    startup_fault_reset_for_test();
    startup_fault_note(STARTUP_FAULT_EXEC_WATCHDOG);

    startup_fault_note(STARTUP_FAULT_LOG_STORE);
    char buf[128];
    unsigned named = startup_fault_summarize(buf, sizeof(buf));
    TEST_CHECK(named == 2, "summary names both faults");
    TEST_CHECK(strstr(buf, "guard 9") && strstr(buf, "log store"), "summary carries both labels");

    /* Truncation never overruns and always terminates. */
    char tiny[12];
    memset(tiny, 'x', sizeof(tiny));
    /* Latch state at this point: EXEC_WATCHDOG (26-char label) and LOG_STORE. */
    unsigned tiny_named = startup_fault_summarize(tiny, sizeof(tiny));
    TEST_CHECK(memchr(tiny, '\0', sizeof(tiny)) != NULL, "a tiny buffer is still NUL-terminated");
    TEST_CHECK(tiny_named == 0, "a buffer too small for the first name writes none");
    TEST_CHECK(strcmp(tiny, "...") == 0, "a buffer too small for the first name holds just the marker");

    /* The real readiness buffer size (readiness_http.c passes 64) with all N
     * faults latched: must fit, terminate, mark the truncation, and count only
     * the names actually written. */
    startup_fault_reset_for_test();
    for (int i = 0; i < (int)STARTUP_FAULT_COUNT; i++) {
        startup_fault_note((startup_fault_t)i);
    }
    TEST_CHECK(startup_fault_count() == (unsigned)STARTUP_FAULT_COUNT, "all faults latch");
    char real[64];
    memset(real, 'x', sizeof(real));
    unsigned real_named = startup_fault_summarize(real, sizeof(real));
    TEST_CHECK(memchr(real, '\0', sizeof(real)) != NULL, "64-byte summary is NUL-terminated within the buffer");
    size_t real_len = strnlen(real, sizeof(real));
    TEST_CHECK(real_named > 0 && real_named < (unsigned)STARTUP_FAULT_COUNT, "all faults do not fit in 64 bytes");
    TEST_CHECK(real_len >= 3 && strcmp(real + real_len - 3, "...") == 0, "truncated summary ends with the marker");
    unsigned seen = 0;
    for (int i = 0; i < (int)STARTUP_FAULT_COUNT; i++) {
        if (strstr(real, startup_fault_name((startup_fault_t)i)) != NULL) {
            seen++;
        }
    }
    TEST_CHECK(seen == real_named, "named equals the number of labels actually written");

    /* A roomy buffer holds all of them with no marker. */
    char big[1024];
    unsigned big_named = startup_fault_summarize(big, sizeof(big));
    TEST_CHECK(big_named == (unsigned)STARTUP_FAULT_COUNT, "a roomy buffer names every fault");
    TEST_CHECK(strstr(big, "...") == NULL, "no marker when nothing was cut");

    startup_fault_reset_for_test();
}
