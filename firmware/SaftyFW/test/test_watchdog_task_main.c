// Own main for the watchdog_task host exe (links the task harness, which must
// not leak into other tests' executables).
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_watchdog_task_loop(void);

int main(void)
{
    run_test_watchdog_task_loop();

    printf("\n%d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
