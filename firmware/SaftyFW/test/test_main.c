// Host test entry point. Builds and runs standalone (no pico-sdk/FreeRTOS),
// see build_host_tests.ps1. TODO.md Phase 4. Structure copied from
// firmware/KilnFW/App/test/test_main.c.
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_safety_guards(void);
void run_test_link_frame(void);

int main(void)
{
    run_test_safety_guards();
    run_test_link_frame();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
