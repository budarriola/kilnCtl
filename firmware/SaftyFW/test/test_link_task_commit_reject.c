// test_link_task_commit_reject.c -- host test for the REAL
// link_task_commit_config_reject_reason_for() (tasks/link_task_commit_reject.c),
// the mapping from a config_store write decision to the
// COMMIT_CONFIG_REJECTED wire reason the commissioning page renders.
//
// Added 2026-09-15 for the adversarial re-review of d43e96b2, which found
// F2 (the ARMED_MIXED mapping) shipped with no test at all AND with the
// wrong input -- it read the CACHED thermocouple type where it needed the
// PERSISTED one. Both halves are covered here: the mapping itself, and
// the fact that it keys strictly on the two type bytes it is handed, so a
// caller passing the cached type instead of the persisted one gets a
// demonstrably different answer (test_config_store_flash.c constructs the
// real disagreement against real flash).
//
// This links the real production function, not a test-local
// reimplementation.
#include <stdio.h>

#include "test_common.h"
#include "tasks/link_task_commit_reject.h"

void run_test_link_task_commit_reject(void)
{
    TEST_SECTION("link_task_commit_config_reject_reason_for(): decision -> wire reason "
                 "(2026-09-15 re-review of d43e96b2, F2 + defect 1)");

    // Two distinct, valid MAX31856 type codes. Their concrete values do not
    // matter to the mapping -- only whether they are equal.
    const uint8_t type_k = 0x03u;
    const uint8_t type_n = 0x02u;
    const uint8_t type_s = 0x06u;

    // --- The F2 behaviour itself -------------------------------------
    TEST_CHECK(link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_REFUSED_ARMED, type_k,
                                                          type_n) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED,
               "ARMED refusal whose candidate ALSO changes tc_type is MIXED -- the operator is "
               "told a tc_type-only change would have been allowed");

    TEST_CHECK(link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_REFUSED_ARMED, type_k,
                                                          type_k) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_ARMED,
               "ARMED refusal that does NOT touch tc_type stays the plain ARMED reason");

    // --- It keys on the bytes it is handed, nothing else -------------
    // This is the property that makes passing the PERSISTED type (rather
    // than the cached one) load-bearing: hand it the two records'
    // disagreeing values and it answers differently. If this function
    // silently consulted some global instead of its parameters, both of
    // these would return the same reason.
    TEST_CHECK(link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_REFUSED_ARMED, type_n,
                                                          type_s) !=
                   link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_REFUSED_ARMED,
                                                              type_s, type_s),
               "the persisted-vs-candidate comparison is made from the PARAMETERS -- a caller "
               "passing the cached tc_type instead of the persisted one gets a different wire "
               "reason (defect 1: that is exactly how the wire reason and the Pico's own log "
               "line came to disagree about one refusal)");

    // --- Every other decision ignores the type bytes entirely --------
    // Swept with DIFFERING type bytes deliberately: a mapping that leaked
    // the MIXED test into any other branch would fail here.
    TEST_CHECK(link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON,
                                                          type_k, type_n) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON,
               "HEAT_ON maps to ARMED_HEAT_ON even when tc_type also differs");
    TEST_CHECK(link_task_commit_config_reject_reason_for(
                   CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_UNKNOWN, type_k, type_n) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_UNKNOWN,
               "HEAT_UNKNOWN maps to ARMED_HEAT_UNKNOWN even when tc_type also differs");
    TEST_CHECK(link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_FLASH_FAILURE, type_k,
                                                          type_n) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_STORAGE,
               "a flash failure maps to STORAGE even when tc_type also differs");
    TEST_CHECK(link_task_commit_config_reject_reason_for(CONFIG_STORE_WRITE_OK, type_k, type_n) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN,
               "CONFIG_STORE_WRITE_OK is not a refusal -- it falls to UNKNOWN rather than "
               "mislabelling a success as an ARMED refusal");

    // An out-of-range decision value (a future enum member reaching an
    // un-updated build) must not be silently classified as an ARMED
    // refusal -- UNKNOWN is the honest answer.
    TEST_CHECK(link_task_commit_config_reject_reason_for((config_store_write_decision_t)0x7Fu,
                                                          type_k, type_n) ==
                   KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN,
               "an unrecognised decision value maps to UNKNOWN, not to a specific ARMED reason");
}
