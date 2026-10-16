// update_task_reboot_policy.c -- see the header for why this pure decision
// lives in its own freestanding file, separate from update_task.c.
#include "update_task_reboot_policy.h"

#include "kilnlink/kilnlink_reboot_result.h"
#include "kilnlink/kilnlink_rollback_result.h"

bool update_task_reboot_policy_decide(bool relay_energized, bool transfer_active, bool trip_ineffective,
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

    /* Guard audit 2026-10-14 A1: a reboot clears every latch at boot, including S9
     * (K4 welded, unclearable by CLEAR_TRIP). Refuse while S9 is latched. Other trip
     * reasons are deliberately NOT refused (owner decision: a benign reboot resumes). */
    if (trip_ineffective) {
        if (out_reason) {
            *out_reason = "refused: S9 TRIP_INEFFECTIVE is latched (K4 may be welded), a reboot "
                          "would clear it; power must be removed at the breaker";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_REBOOT_RESULT_REASON_TRIP_INEFFECTIVE;
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

bool update_task_rollback_gate_decide(bool relay_energized, bool trip_ineffective,
                                       const char **out_reason, uint8_t *out_reason_code)
{
    if (relay_energized) {
        if (out_reason) {
            *out_reason = "refused: relay is ARMED, rollback is refused while ARMED "
                          "(same gate as config writes)";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_ARMED;
        }
        return false;
    }
    /* Guard audit 2026-10-14 A1: same reasoning as the reboot gate above. */
    if (trip_ineffective) {
        if (out_reason) {
            *out_reason = "refused: S9 TRIP_INEFFECTIVE is latched (K4 may be welded), a rollback "
                          "reboot would clear it";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_TRIP_INEFFECTIVE;
        }
        return false;
    }
    if (out_reason) {
        *out_reason = "ok";
    }
    return true;
}
