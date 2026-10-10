// Own main for the thermo_task host exe (separate from the main exe because it
// links the task harness and stubs that must not leak into other tests).
#include <stdio.h>

#include "test_common.h"

int g_test_failures;
int g_test_count;

void run_test_thermo_task_faults(void);

int main(void)
{
    run_test_thermo_task_faults();
    printf("\n%d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
