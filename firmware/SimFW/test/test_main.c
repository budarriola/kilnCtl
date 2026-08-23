// Host test entry point for SimFW's pure sim/ modules. Builds and runs
// standalone (no pico-sdk/FreeRTOS), see build_host_tests.ps1. Structure
// copied from firmware/SaftyFW/test/test_main.c per docs/PLAN.md section
// 13.1's instruction to use this repo's established host-test pattern.
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_thermal_model(void);
void run_test_max31856_regs(void);
void run_test_max31856_resp_image(void);
void run_test_sine_synth(void);
void run_test_fault_engine(void);
void run_test_tc_fault_state(void);
void run_test_gap_closure_logic(void);
void run_test_cmd_task_gap_closure(void);
void run_test_k4_gating_logic(void);
void run_test_ct_calibration(void);
void run_test_dut_power_domains(void);
void run_test_ct_i2s_gen(void);

int main(void)
{
    run_test_thermal_model();
    run_test_max31856_regs();
    run_test_max31856_resp_image();
    run_test_sine_synth();
    run_test_fault_engine();
    run_test_tc_fault_state();
    run_test_gap_closure_logic();
    run_test_cmd_task_gap_closure();
    run_test_k4_gating_logic();
    run_test_ct_calibration();
    run_test_dut_power_domains();
    run_test_ct_i2s_gen();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
