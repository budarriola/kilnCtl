// update_task_reboot_policy.h -- the PURE decision half of
// update_task_reboot_allowed() (SAFETY_CMD_REBOOT, 0x29), pulled out of
// update_task.c 2026-09-09 so it can be host-compiled and driven directly.
//
// WHY THIS FILE EXISTS: update_task.c pulls in FreeRTOS.h/pico/flash.h/
// hardware/flash.h/hardware/watchdog.h, none of which exist off real
// hardware (see that file's own header comment), so nothing in it can be
// host-compiled -- test_reboot_in_place_wiring.c's own comment says so, and
// its test of the ARMED/TRANSFER_ACTIVE gate was consequently a SOURCE-TEXT
// SCAN (grepping the compiled .c file for the right identifiers), not a
// behavioural test. A textual scan is satisfied by the identifiers merely
// being PRESENT; it proves nothing about what the function actually
// RETURNS for a given input, and is vacuous against exactly the class of
// bug a real behavioural test would catch (e.g. the two conditions
// swapped, or the wrong reason code attached to the wrong condition).
//
// This header/source pair holds ONLY the two-input decision table --
// no FreeRTOS, no hardware/*.h, no I/O, freestanding C11 (CommonFW/
// README.md rules 1-6 apply here too even though this lives in SaftyFW,
// not CommonFW) -- so it compiles and links on the host exactly as written
// for the RP2040 build. update_task.c's own update_task_reboot_allowed()
// is now a thin wrapper: it gathers the two live inputs
// (safety_core_get_output_status(), update_task_transfer_active()) and
// hands them to update_task_reboot_policy_decide() below, unchanged. See
// update_task.c for that wrapper and update_task.h's own doc comment on
// update_task_reboot_allowed() for the policy's role in the wire protocol.
#ifndef SAFTYFW_TASKS_UPDATE_TASK_REBOOT_POLICY_H
#define SAFTYFW_TASKS_UPDATE_TASK_REBOOT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The whole of update_task_reboot_allowed()'s decision, as a pure function
// of its two live inputs:
//
//   relay_energized  -- safety_core_get_output_status()'s first out-param.
//                        Refuses (KILNLINK_REBOOT_RESULT_REASON_ARMED) if
//                        true: rebooting while holding heating permission
//                        drops supervision mid-firing.
//   transfer_active  -- update_task_transfer_active(). Refuses
//                        (KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE) if
//                        true: a reset mid-transfer leaves a partially
//                        written slot with no record that it is partial.
//
// relay_energized is checked FIRST -- both this ordering and the two
// reasons themselves are pinned by test_update_task_reboot_policy.c's own
// "both true" case, which must report ARMED, never TRANSFER_ACTIVE:
// heating permission is the more urgent fact when both are true at once.
//
// out_reason/out_reason_code follow update_task_reboot_allowed()'s own
// contract (update_task.h): either may be NULL, and out_reason_code is
// KILNLINK_REBOOT_RESULT_REASON_NONE on an allowed decision.
bool update_task_reboot_policy_decide(bool relay_energized, bool transfer_active,
                                       const char **out_reason, uint8_t *out_reason_code);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UPDATE_TASK_REBOOT_POLICY_H
