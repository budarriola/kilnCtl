// Host test entry point. Builds and runs standalone (no pico-sdk/FreeRTOS),
// see build_host_tests.ps1. TODO.md Phase 4. Structure copied from
// firmware/KilnFW/App/test/test_main.c.
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_safety_guards(void);
void run_test_link_frame(void);
void run_test_bootloader_metadata(void);
void run_test_update(void);
void run_test_kilnlink_power(void);
void run_test_tx_watermark(void);
void run_test_relay_grace(void);
void run_test_config_store(void);
void run_test_snapshots(void);

int main(void)
{
    run_test_safety_guards();
    run_test_link_frame();
    run_test_bootloader_metadata();
    run_test_update();
    run_test_kilnlink_power();
    run_test_tx_watermark();
    run_test_relay_grace();
    run_test_config_store();
    run_test_snapshots();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
