// Host test entry point. Builds and runs standalone (no ESP-IDF/FreeRTOS),
// see build_host_tests.ps1. TODO.md 6A.8.
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_pid(void);
void run_test_thermal_guard(void);
void run_test_heater_output(void);
void run_test_closed_loop(void);
void run_test_pid_autotune(void);
void run_test_sim_kiln(void);
void run_test_ota_auth(void);
void run_test_ota_interlock(void);

int main(void)
{
    run_test_pid();
    run_test_thermal_guard();
    run_test_heater_output();
    run_test_closed_loop();
    run_test_pid_autotune();
    run_test_sim_kiln();
    run_test_ota_auth();
    run_test_ota_interlock();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
