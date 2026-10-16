// test_update_task_reboot_policy.c -- BEHAVIOURAL test of the pure decision
// behind update_task_reboot_allowed() (SAFETY_CMD_REBOOT, 0x29).
//
// test_reboot_in_place_wiring.c's test_reboot_allowed_is_the_armed_gate_only()
// could only scan update_task.c's SOURCE TEXT for the right identifiers
// (strstr(body, "KILNLINK_REBOOT_RESULT_REASON_ARMED") etc), because
// update_task.c pulls in FreeRTOS.h/pico headers that do not exist off
// target. That scan is satisfied by the identifiers merely being present in
// the function body -- deleting the string literal breaks it, but so would
// nothing else: it proves no relationship between INPUT (relay state,
// transfer state) and OUTPUT (the returned bool and reason code). A version
// of update_task_reboot_allowed() that always refused with ARMED regardless
// of input, or that swapped the two conditions, or that returned the wrong
// reason for the right condition, would pass that scan unchanged.
//
// This file drives the REAL production decision table
// (update_task_reboot_policy_decide(), update_task_reboot_policy.c -- linked
// in for real, not restated) across the full 2x2 state matrix and asserts
// both the returned bool and the reason code every time.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "kilnlink/kilnlink_reboot_result.h"
#include "kilnlink/kilnlink_rollback_result.h"
#include "tasks/update_task_reboot_policy.h"

static void test_neither_armed_nor_transfer_allows(void)
{
    TEST_SECTION("update_task_reboot_policy_decide -- relay open, no transfer -> allowed");

    const char *reason = NULL;
    uint8_t reason_code = 0xFF;
    bool allowed = update_task_reboot_policy_decide(false, false, false, &reason, &reason_code);

    TEST_CHECK(allowed, "neither condition is true -- the reboot is allowed");
    TEST_CHECK(reason_code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_NONE,
               "allowed decision reports reason NONE");
    TEST_CHECK(reason != NULL && strcmp(reason, "ok") == 0,
               "allowed decision's human reason string is \"ok\"");
}

static void test_armed_alone_refuses_with_armed_reason(void)
{
    TEST_SECTION("update_task_reboot_policy_decide -- relay ARMED, no transfer -> refused ARMED");

    const char *reason = NULL;
    uint8_t reason_code = 0xFF;
    bool allowed = update_task_reboot_policy_decide(true, false, false, &reason, &reason_code);

    TEST_CHECK(!allowed, "an energized relay refuses the reboot");
    TEST_CHECK(reason_code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_ARMED,
               "the refusal reports reason ARMED, not TRANSFER_ACTIVE or NONE");
    TEST_CHECK(reason != NULL && strstr(reason, "ARMED") != NULL,
               "the human reason string names the relay as ARMED");
}

static void test_transfer_alone_refuses_with_transfer_reason(void)
{
    TEST_SECTION("update_task_reboot_policy_decide -- relay open, transfer ACTIVE -> refused "
                 "TRANSFER_ACTIVE");

    const char *reason = NULL;
    uint8_t reason_code = 0xFF;
    bool allowed = update_task_reboot_policy_decide(false, true, false, &reason, &reason_code);

    TEST_CHECK(!allowed, "an in-progress transfer refuses the reboot");
    TEST_CHECK(reason_code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE,
               "the refusal reports reason TRANSFER_ACTIVE, not ARMED or NONE -- this is "
               "exactly the case sw_reset_http.c's opus-review fix (2026-09-09) exists for: "
               "the ESP must not tell the operator the relay is armed when the real cause is "
               "an in-flight transfer");
    TEST_CHECK(reason != NULL && strstr(reason, "transfer") != NULL,
               "the human reason string names the transfer, not the relay");
}

static void test_both_true_prefers_armed(void)
{
    TEST_SECTION("update_task_reboot_policy_decide -- BOTH relay ARMED and transfer ACTIVE -> "
                 "refused ARMED (checked first)");

    const char *reason = NULL;
    uint8_t reason_code = 0xFF;
    bool allowed = update_task_reboot_policy_decide(true, true, false, &reason, &reason_code);

    TEST_CHECK(!allowed, "both conditions true still refuses");
    TEST_CHECK(reason_code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_ARMED,
               "when both are true, ARMED is reported -- the more urgent fact (live heating "
               "permission) takes priority over the transfer-in-progress refusal");
}

// Every out-param is documented as optional; a caller (a future one, or a
// test) passing NULL for either must not crash.
static void test_null_out_params_tolerated(void)
{
    TEST_SECTION("update_task_reboot_policy_decide -- NULL out_reason/out_reason_code do not crash");

    bool allowed_a = update_task_reboot_policy_decide(true, false, false, NULL, NULL);
    TEST_CHECK(!allowed_a, "ARMED refusal still returned with both out-params NULL");

    uint8_t reason_code = 0xFF;
    bool allowed_b = update_task_reboot_policy_decide(false, true, false, NULL, &reason_code);
    TEST_CHECK(!allowed_b, "TRANSFER_ACTIVE refusal returned with out_reason NULL");
    TEST_CHECK(reason_code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE,
               "out_reason_code is still written when out_reason is NULL");

    const char *reason = NULL;
    bool allowed_c = update_task_reboot_policy_decide(false, false, false, &reason, NULL);
    TEST_CHECK(allowed_c, "allowed decision returned with out_reason_code NULL");
    TEST_CHECK(reason != NULL && strcmp(reason, "ok") == 0,
               "out_reason is still written when out_reason_code is NULL");
}

// Guard audit 2026-10-14 A1: S9 (welded K4) latched refuses reboot and rollback.
static void test_a1_s9_latched_refuses_reboot(void)
{
    TEST_SECTION("A1 reboot policy -- S9 latched, relay open, no transfer -> refused TRIP_INEFFECTIVE");

    const char *reason = NULL;
    uint8_t code = 0xFF;
    bool allowed = update_task_reboot_policy_decide(false, false, true, &reason, &code);
    TEST_CHECK(!allowed, "S9 latched refuses the reboot (a reboot would clear the latch)");
    TEST_CHECK(code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_TRIP_INEFFECTIVE,
               "refusal reports TRIP_INEFFECTIVE");
    TEST_CHECK(reason != NULL && strstr(reason, "S9") != NULL, "reason string names S9");

    code = 0xFF;
    allowed = update_task_reboot_policy_decide(true, false, true, &reason, &code);
    TEST_CHECK(!allowed && code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_ARMED,
               "ARMED is still reported first when both ARMED and S9");

    code = 0xFF;
    allowed = update_task_reboot_policy_decide(false, true, true, &reason, &code);
    TEST_CHECK(!allowed && code == (uint8_t)KILNLINK_REBOOT_RESULT_REASON_TRIP_INEFFECTIVE,
               "S9 is reported before TRANSFER_ACTIVE");

    allowed = update_task_reboot_policy_decide(false, false, true, NULL, NULL);
    TEST_CHECK(!allowed, "S9 refusal survives NULL out-params");

    allowed = update_task_reboot_policy_decide(false, false, false, &reason, &code);
    TEST_CHECK(allowed, "no S9 -> other trip reasons are NOT refused (benign reboot resumes)");
}

static void test_a1_rollback_gate(void)
{
    TEST_SECTION("A1 rollback gate -- ARMED, S9, and allowed");

    const char *reason = NULL;
    uint8_t code = 0xFF;
    TEST_CHECK(!update_task_rollback_gate_decide(true, false, &reason, &code) &&
                   code == (uint8_t)KILNLINK_ROLLBACK_RESULT_REASON_ARMED,
               "ARMED refuses with ARMED");
    code = 0xFF;
    TEST_CHECK(!update_task_rollback_gate_decide(false, true, &reason, &code) &&
                   code == (uint8_t)KILNLINK_ROLLBACK_RESULT_REASON_TRIP_INEFFECTIVE,
               "S9 latched refuses with TRIP_INEFFECTIVE");
    TEST_CHECK(reason != NULL && strstr(reason, "S9") != NULL, "S9 reason string names S9");
    code = 0xFF;
    TEST_CHECK(!update_task_rollback_gate_decide(true, true, &reason, &code) &&
                   code == (uint8_t)KILNLINK_ROLLBACK_RESULT_REASON_ARMED,
               "both -> ARMED first");
    code = 0xFE;
    TEST_CHECK(update_task_rollback_gate_decide(false, false, &reason, &code) && code == 0xFE,
               "neither -> allowed, code untouched");
    TEST_CHECK(!update_task_rollback_gate_decide(false, true, NULL, NULL),
               "NULL out-params tolerated");
}

void run_test_update_task_reboot_policy(void)
{
    test_neither_armed_nor_transfer_allows();
    test_armed_alone_refuses_with_armed_reason();
    test_transfer_alone_refuses_with_transfer_reason();
    test_both_true_prefers_armed();
    test_null_out_params_tolerated();
    test_a1_s9_latched_refuses_reboot();
    test_a1_rollback_gate();
}
