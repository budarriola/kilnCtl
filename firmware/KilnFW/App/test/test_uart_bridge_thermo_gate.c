// Host tests for drivers/bridge/uart_bridge_thermo_gate.h (review 12 Part B):
// the THERMO UART writers are refused while a profile or autotune is active,
// read-only subcommands never are, and uart_bridge_thermo.c really consults the
// gate before its dispatch switch (source scan: the task loop itself is not
// hostable). Links the real system_mode_gate.c.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/bridge/uart_bridge_thermo_gate.h"

int g_test_failures = 0;
int g_test_count = 0;

static bool s_profile = false;
static bool s_autotune = false;
void relay_authority_heat_run_active(bool *p, bool *a)
{
    if (p) *p = s_profile;
    if (a) *a = s_autotune;
}

static const uint8_t k_writers[] = { THERMO_CMD_CONFIG_CHANNEL, THERMO_CMD_SET_THRESHOLDS,
                                     THERMO_CMD_SET_CJ_OFFSET, THERMO_CMD_CLEAR_FAULTS,
                                     THERMO_CMD_WRITE_REG };
static const uint8_t k_readers[] = { THERMO_CMD_ONE_SHOT, THERMO_CMD_READ, THERMO_CMD_READ_FAULTS,
                                     THERMO_CMD_SET_AUTO_REPORT, THERMO_CMD_READ_REG };

static void check_all(bool profile, bool autotune, bool writers_refused)
{
    char reason[SYSTEM_MODE_GATE_REASON_MAX];
    s_profile = profile;
    s_autotune = autotune;
    for (size_t i = 0; i < sizeof(k_writers); i++) {
        reason[0] = '\0';
        bool refused = thermo_bridge_write_refused(k_writers[i], reason, sizeof(reason));
        TEST_CHECK(refused == writers_refused, "writer refusal must match run state");
        if (writers_refused) {
            TEST_CHECK(reason[0] != '\0', "a refusal must carry a reason");
        }
    }
    for (size_t i = 0; i < sizeof(k_readers); i++) {
        TEST_CHECK(!thermo_bridge_write_refused(k_readers[i], reason, sizeof(reason)),
                   "read-only subcommands are never gated");
    }
}

static void test_gate(void)
{
    check_all(false, false, false);
    check_all(true, false, true);
    check_all(false, true, true);
    check_all(true, true, true);
}

static void test_source_consults_gate(void)
{
    char *path = test_resolve_from_here(__FILE__, "../drivers/bridge/uart_bridge_thermo.c");
    FILE *f = path ? fopen(path, "rb") : NULL;
    TEST_CHECK(f != NULL, "could not open uart_bridge_thermo.c");
    if (!f) { free(path); return; }
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    free(path);
    const char *gate = strstr(buf, "thermo_bridge_write_refused(subcmd");
    const char *sw = strstr(buf, "switch (subcmd) {");
    TEST_CHECK(gate != NULL, "thermo bridge must call thermo_bridge_write_refused()");
    TEST_CHECK(sw != NULL && gate != NULL && gate < sw, "gate must run before the dispatch switch");
}

int main(void)
{
    test_gate();
    test_source_consults_gate();
    printf("test_uart_bridge_thermo_gate: %d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
