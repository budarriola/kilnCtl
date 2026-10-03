// test_bootloader_recovery_update_main.c -- entry point for the SEPARATE
// bootloader recovery_update.c host test executable (build_host_tests.ps1).
// It #includes the real recovery_update.c against SDK stubs under
// stubs/bootloader_sdk_stub, so it cannot share test_main.c's exe (the stub
// XIP_BASE/flash symbols would collide with the main exe's own backends).
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_bootloader_recovery_update(void);

int main(void)
{
    run_test_bootloader_recovery_update();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    return 0;
}
