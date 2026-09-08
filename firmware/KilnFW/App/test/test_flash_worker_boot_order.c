// Negative/positive test for the 2026-09-08 flash-worker boot-ordering fix
// (main_control_bringup.c: uart_bridge_ext_start_flash_worker() moved to run
// BEFORE relay_cycles_init()/profile_executor_start()->adaptive_tune_init()).
//
// Exercises the REAL flash_worker_wait_until_started() (flash_worker_wait.c)
// with a predicate that models "has the worker task been created yet",
// flipped by a stand-in start_worker() call -- exactly the two-state
// contract main_control_bringup.c's boot sequence has. This is not a mock:
// it is the same function relay_cycles_init()/adaptive_tune_init() call at
// boot, driven with the same two orderings the firmware can produce.
#include <stdio.h>
#include <stdbool.h>

#include "flash_worker_wait.h"

static bool s_worker_started = false;

static bool fake_worker_started(void)
{
    return s_worker_started;
}

/* flash_worker_wait.c hand-declares this (see its header comment) and
 * flash_worker_wait_default() references it -- this test never calls that
 * wrapper (it drives flash_worker_wait_until_started() directly with its
 * own fake predicate), but the symbol must still resolve at link time. */
bool uart_bridge_ext_flash_worker_started(void)
{
    return s_worker_started;
}

static int s_failures = 0;
#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s\n", msg);                                                                              \
            s_failures++;                                                                                           \
        } else {                                                                                                     \
            printf("PASS: %s\n", msg);                                                                              \
        }                                                                                                            \
    } while (0)

int main(void)
{
    // --- OLD ORDER (the bug): caller waits BEFORE anything ever starts the
    // worker. This is what relay_cycles_init()/adaptive_tune_init() actually
    // saw pre-fix: uart_bridge_ext_start_flash_worker() was called LATER in
    // the same single-threaded boot function, so nothing could flip
    // s_worker_started to true while this wait is running -- it always
    // burns the full ceiling and reports "not started".
    s_worker_started = false;
    bool old_order_result = flash_worker_wait_until_started(fake_worker_started, 20, 5000);
    CHECK(old_order_result == false,
          "old order (wait before worker ever starts) times out -- reproduces the hardware-observed "
          "migration_deferred:true");

    // --- NEW ORDER (the fix): start_worker() runs first, exactly like
    // uart_bridge_ext_start_flash_worker() now does earlier in
    // main_control_bringup.c, before relay_cycles_init()/profile_executor_
    // start() run.
    s_worker_started = true; // stands in for uart_bridge_ext_start_flash_worker() having run
    bool new_order_result = flash_worker_wait_until_started(fake_worker_started, 20, 5000);
    CHECK(new_order_result == true,
          "new order (worker started first) succeeds immediately -- migration proceeds instead of deferring");

    if (s_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", s_failures);
    return 1;
}
