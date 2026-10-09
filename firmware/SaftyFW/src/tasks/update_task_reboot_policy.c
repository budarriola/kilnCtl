// update_task_reboot_policy.c -- see the header for why this pure decision
// lives in its own freestanding file, separate from update_task.c.
#include "update_task_reboot_policy.h"

#include "kilnlink/kilnlink_reboot_result.h"

bool update_task_reboot_policy_decide(bool relay_energized, bool transfer_active,
                                       const char **out_reason, uint8_t *out_reason_code)
{
    if (relay_energized) {
        if (out_reason) {
            *out_reason = "refused: relay is ARMED, reboot is refused while ARMED "
                          "(same gate as config writes and rollback)";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_REBOOT_RESULT_REASON_ARMED;
        }
        return false;
    }

    if (transfer_active) {
        if (out_reason) {
            *out_reason = "refused: a firmware transfer is in progress, reboot would leave a "
                          "partially written slot";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_REBOOT_RESULT_REASON_TRANSFER_ACTIVE;
        }
        return false;
    }

    if (out_reason) {
        *out_reason = "ok";
    }
    if (out_reason_code) {
        *out_reason_code = KILNLINK_REBOOT_RESULT_REASON_NONE;
    }
    return true;
}
