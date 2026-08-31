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
void run_test_pid_fuzzy(void);
void run_test_sim_kiln(void);
void run_test_ota_auth(void);
void run_test_ota_interlock(void);
void run_test_heat_interlock(void);
void run_test_heat_enable(void);
void run_test_thermo_combine(void);
void run_test_profile_feasibility(void);
void run_test_profile_plan_curve(void);
void run_test_wifi_prov(void);
void run_test_backup_import(void);
void run_test_ota_record(void);
void run_test_uart_log_bridge(void);
void run_test_safety_watchdog(void);
void run_test_safety_link(void);
void run_test_dashboard_safety_ready(void);
void run_test_readiness_commissioning(void);
void run_test_kiln_cfg_store(void);
void run_test_safety_cfg_store(void);
void run_test_boot_guard(void);
void run_test_boot_button(void);
void run_test_crash_report(void);
void run_test_watchdog_cfg(void);
void run_test_ui_page_home_graph(void);
void run_test_max31856_codec(void);
void run_test_owner_slot_pool(void);
void run_test_dram_margin(void);
void run_test_stack_margin(void);
void run_test_time_sync(void);
// run_test_safety_cfg_http() is NOT called here -- test_safety_cfg_http.c is
// its own separate executable (build_host_tests.ps1's third build+run step),
// same reason test_zones_http.c is: it #includes safety_cfg_http.c directly
// to reach its static parse_set_param_body()/build_commissioning_json()/
// apply_pairs() helpers, which means it must define its own fake bodies for
// safety_cfg_store_get_by_index() and friends -- and this executable already
// links the REAL ones via test_safety_cfg_store.c's #include of safety_cfg_
// store.c above. Linking both into one binary would multiply-define every
// safety_cfg_store_* symbol.

int main(void)
{
    run_test_pid();
    run_test_thermal_guard();
    run_test_heater_output();
    run_test_closed_loop();
    run_test_pid_autotune();
    run_test_pid_fuzzy();
    run_test_sim_kiln();
    run_test_ota_auth();
    run_test_ota_interlock();
    run_test_heat_interlock();
    run_test_heat_enable();
    run_test_thermo_combine();
    run_test_profile_feasibility();
    run_test_profile_plan_curve();
    run_test_wifi_prov();
    run_test_backup_import();
    run_test_ota_record();
    run_test_uart_log_bridge();
    run_test_safety_watchdog();
    run_test_safety_link();
    run_test_dashboard_safety_ready();
    run_test_readiness_commissioning();
    run_test_kiln_cfg_store();
    run_test_safety_cfg_store();
    run_test_boot_guard();
    run_test_boot_button();
    run_test_crash_report();
    run_test_watchdog_cfg();
    run_test_ui_page_home_graph();
    run_test_max31856_codec();
    run_test_owner_slot_pool();
    run_test_dram_margin();
    run_test_stack_margin();
    run_test_time_sync();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
