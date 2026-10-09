#include "kilnlink/kilnlink_reboot_result.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0   u8  cmd (0x2A)
 *   1   u8  accepted
 *   2   u8  reason
 */
#define OFF_ACCEPTED 1u
#define OFF_REASON   2u

size_t kilnlink_reboot_result_encode(const kilnlink_reboot_result_t *msg, uint8_t *out, size_t out_cap,
                                     kilnlink_reboot_result_status_t *status)
{
    kilnlink_reboot_result_status_t local_status = KILNLINK_REBOOT_RESULT_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_REBOOT_RESULT_OK;

    if (out_cap < KILNLINK_REBOOT_RESULT_LEN) {
        *status = KILNLINK_REBOOT_RESULT_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_REBOOT_RESULT_CMD;
    out[OFF_ACCEPTED] = msg->accepted;
    out[OFF_REASON] = msg->reason;

    return KILNLINK_REBOOT_RESULT_LEN;
}

kilnlink_reboot_result_status_t kilnlink_reboot_result_decode(const uint8_t *payload, size_t len,
                                                              kilnlink_reboot_result_t *out)
{
    if (len != KILNLINK_REBOOT_RESULT_LEN) {
        return KILNLINK_REBOOT_RESULT_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_REBOOT_RESULT_CMD) {
        return KILNLINK_REBOOT_RESULT_ERR_WRONG_CMD;
    }

    if (out) {
        out->accepted = payload[OFF_ACCEPTED];
        out->reason = payload[OFF_REASON];
    }

    return KILNLINK_REBOOT_RESULT_OK;
}
