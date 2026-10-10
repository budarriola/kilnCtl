// Safety link review 2026-10-09 F3: a persistent COMMIT_CONFIG read-back must
// also require the Pico's DIAG CONFIG_VOLATILE_DIRTY bit clear, because
// GET_CONFIG_PAGE serves the RAM record (which equals a prior volatile install
// even when the Pico refused the flash write while ARMED).
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/safety/safety_cfg_persist_verdict.h"

void run_test_safety_cfg_persist_verdict(void)
{
    TEST_SECTION("safety_cfg_persist_verdict -- F3: persisted only when DIAG volatile-dirty bit reads clear");

    TEST_CHECK(safety_cfg_persist_verdict(true, 0x00u) == SAFETY_CFG_PERSIST_VERDICT_PERSISTED,
               "DIAG seen, dirty clear -> persisted");
    TEST_CHECK(safety_cfg_persist_verdict(true, SAFETY_LINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY) ==
                   SAFETY_CFG_PERSIST_VERDICT_STILL_DIRTY,
               "dirty bit set -> not persisted (refused flash write)");
    TEST_CHECK(safety_cfg_persist_verdict(true, 0x7Fu) == SAFETY_CFG_PERSIST_VERDICT_PERSISTED,
               "every other DIAG flag set but bit 7 clear -> persisted");
    TEST_CHECK(safety_cfg_persist_verdict(true, 0xFFu) == SAFETY_CFG_PERSIST_VERDICT_STILL_DIRTY,
               "bit 7 among others -> dirty");
    TEST_CHECK(safety_cfg_persist_verdict(false, 0x00u) == SAFETY_CFG_PERSIST_VERDICT_UNKNOWN,
               "no DIAG ever received -> unknown, never persisted");

    static const char *const candidates[] = {
        "../drivers/safety/safety_cfg_write.c",
        "App/drivers/safety/safety_cfg_write.c",
        "firmware/KilnFW/App/drivers/safety/safety_cfg_write.c",
    };
    char *text = test_read_source_anchored(__FILE__, "../drivers/safety/safety_cfg_write.c", candidates,
                                            sizeof(candidates) / sizeof(candidates[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate safety_cfg_write.c");
        return;
    }
    TEST_CHECK(strstr(text, "safety_cfg_persist_verdict(st.diag_ever_received, st.diag_flags)") != NULL,
               "confirm_commit_landed() consults the verdict");
    TEST_CHECK(strstr(text, "/*require_persisted=*/!volatile_install") != NULL,
               "a persistent (non-volatile) commit requires the persisted verdict");
    TEST_CHECK(strstr(text, "v != SAFETY_CFG_PERSIST_VERDICT_PERSISTED") != NULL,
               "anything but PERSISTED fails the commit");
    free(text);
}
