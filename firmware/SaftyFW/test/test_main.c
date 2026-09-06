// Host test entry point. Builds and runs standalone (no pico-sdk/FreeRTOS),
// see build_host_tests.ps1. TODO.md Phase 4. Structure copied from
// firmware/KilnFW/App/test/test_main.c.
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_safety_guards(void);
void run_test_link_frame(void);
void run_test_link_frame_wire(void);
void run_test_bootloader_metadata(void);
void run_test_update(void);
void run_test_kilnlink_power(void);
void run_test_tx_watermark(void);
void run_test_relay_grace(void);
void run_test_config_store(void);
void run_test_ct_amps_cal(void);
void run_test_snapshots(void);
void run_test_boot_checkin_coverage(void);
void run_test_watchdog_budget_coverage(void);
void run_test_watchdog_gate(void);
void run_test_kilnlink_inject_tc(void);
void run_test_uart_owner_tx_policy(void);
void run_test_clock_health(void);
void run_test_tick_timing(void);
void run_test_safety_core_stack_budget(void);
void run_test_clear_trip_diag_codec(void);
void run_test_watchdog_overdue_diag_codec(void);
void run_test_log_task_stack_budget(void);
void run_test_max31856_tc_type_policy(void);
void run_test_max31856_decode(void);
void run_test_max31856_tc_range_policy(void);
void run_test_max31856_fault_pin_policy(void);
void run_test_max31856_hal_spi(void);
void run_test_link_diag_flags(void);
void run_test_current_presence_policy(void);
void run_test_discrete_pin_policy(void);
void run_test_commissioning_gate(void);
void run_test_safety_core_s8_wiring(void);
void run_test_safety_core_polarity_wiring(void);
void run_test_update_task_relay_wiring(void);
void run_test_relay_owner_gpio_init(void);
void run_test_current_sense_hal_adc(void);
void run_test_guard_nuisance(void);
void run_test_debounce_policy(void);
void run_test_debounce_nuisance(void);

int main(void)
{
    run_test_safety_guards();
    run_test_guard_nuisance();
    run_test_debounce_policy();
    run_test_debounce_nuisance();
    run_test_link_frame();
    run_test_link_frame_wire();
    run_test_bootloader_metadata();
    run_test_update();
    run_test_kilnlink_power();
    run_test_tx_watermark();
    run_test_relay_grace();
    run_test_config_store();
    run_test_ct_amps_cal();
    run_test_snapshots();
    run_test_boot_checkin_coverage();
    run_test_watchdog_budget_coverage();
    run_test_watchdog_gate();
    run_test_kilnlink_inject_tc();
    run_test_uart_owner_tx_policy();
    run_test_clock_health();
    run_test_tick_timing();
    run_test_safety_core_stack_budget();
    run_test_clear_trip_diag_codec();
    run_test_watchdog_overdue_diag_codec();
    run_test_log_task_stack_budget();
    run_test_max31856_tc_type_policy();
    run_test_max31856_decode();
    run_test_max31856_tc_range_policy();
    run_test_max31856_fault_pin_policy();
    run_test_max31856_hal_spi();
    run_test_link_diag_flags();
    run_test_current_presence_policy();
    run_test_discrete_pin_policy();
    run_test_commissioning_gate();
    run_test_safety_core_s8_wiring();
    run_test_safety_core_polarity_wiring();
    run_test_update_task_relay_wiring();
    run_test_relay_owner_gpio_init();
    run_test_current_sense_hal_adc();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
