// test_hal_spi_pico_main.c -- entry point for the SEPARATE hal_spi_pico.c
// adapter-logic test executable (build_host_tests.ps1's own comment on why
// this cannot share test_main.c's exe: it links the real hal_spi_pico.c,
// which defines the same hal_spi_* symbol names test_max31856_hal_spi.c's
// exe already links via fake_spi.c -- two hal_spi.h backends cannot coexist
// in one binary). Same TEST_CHECK/g_test_count convention as test_main.c,
// just its own process and its own exit code.
#include <stdio.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

void run_test_hal_spi_pico(void);

int main(void)
{
    run_test_hal_spi_pico();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    return 0;
}
