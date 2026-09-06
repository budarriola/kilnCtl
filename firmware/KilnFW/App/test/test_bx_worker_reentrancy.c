// S4 (2026-09-01 audit of ae5905f): pins bx_caller_is_worker_task()
// (drivers/bx_worker_reentrancy.h), the two-pointer comparison
// uart_bridge_ext.c's bx_run_on_internal_stack() re-entrancy backstop and
// uart_bridge_ext_is_on_flash_worker() both reduce to. uart_bridge_ext.c
// itself links no host test (see that header's own comment for why), so
// this is deliberately the smallest possible extract of the actual
// decision -- no FreeRTOS/hardware stubs needed at all -- so that AT LEAST
// the comparison the backstop depends on cannot silently regress.
#include "test_common.h"

#include "../drivers/common/bx_worker_reentrancy.h"

static void test_null_worker_handle_never_matches(void)
{
    // Before bx_worker_ensure_started()'s task-create has run (or a worker
    // that was never started), s_bx_worker_task_handle is NULL -- must
    // never be mistaken for "the caller is already on it", including
    // against a current handle that also happens to be NULL/0.
    TEST_CHECK(!bx_caller_is_worker_task(NULL, NULL),
               "S4: a NULL worker handle must never match, even against a NULL current handle");
    TEST_CHECK(!bx_caller_is_worker_task(NULL, (const void *)1),
               "S4: a NULL worker handle must never match any current handle");
}

static void test_matching_handles_are_the_worker(void)
{
    int fake_task;
    TEST_CHECK(bx_caller_is_worker_task(&fake_task, &fake_task),
               "S4: identical non-NULL worker/current handles must be recognized as the worker task -- this is "
               "exactly what saves the accept path (R1) and the halt path (S1) when their own explicit "
               "is_on_flash_worker() checks are skipped, so this comparison regressing silently reopens both "
               "deadlocks");
}

static void test_different_handles_are_not_the_worker(void)
{
    int fake_worker, fake_other_task;
    TEST_CHECK(!bx_caller_is_worker_task(&fake_worker, &fake_other_task),
               "S4: two distinct non-NULL handles must never be reported as a match -- a false positive here "
               "would run an ordinary caller's job INLINE on its own (possibly PSRAM) stack, silently "
               "reintroducing the cache-disable-vs-PSRAM-stack hazard this whole executor exists to prevent");
}

void run_test_bx_worker_reentrancy(void)
{
    TEST_SECTION("bx_worker_reentrancy: bx_caller_is_worker_task() (S4)");
    test_null_worker_handle_never_matches();
    test_matching_handles_are_the_worker();
    test_different_handles_are_not_the_worker();
}
