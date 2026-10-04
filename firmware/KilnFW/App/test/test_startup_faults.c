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

    startup_fault_note(STARTUP_FAULT_LOG_STORE);
    char buf[128];
    unsigned named = startup_fault_summarize(buf, sizeof(buf));
    TEST_CHECK(named == 2, "summary names both faults");
    TEST_CHECK(strstr(buf, "guard 9") && strstr(buf, "log store"), "summary carries both labels");

    /* Truncation never overruns and always terminates. */
    char tiny[12];
    memset(tiny, 'x', sizeof(tiny));
    (void)startup_fault_summarize(tiny, sizeof(tiny));
    TEST_CHECK(memchr(tiny, '\0', sizeof(tiny)) != NULL, "a tiny buffer is still NUL-terminated");

    startup_fault_reset_for_test();
}
