#include "kilnlink/kilnlink_rollback.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x17)
 * (no other fields -- see kilnlink_rollback.h's header comment)
 */

size_t kilnlink_rollback_encode(const kilnlink_rollback_t *msg, uint8_t *out, size_t out_cap,
                                 kilnlink_rollback_status_t *status)
{
    (void)msg; /* nothing to read -- see kilnlink_rollback.h */

    kilnlink_rollback_status_t local_status = KILNLINK_ROLLBACK_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_ROLLBACK_OK;

    if (out_cap < KILNLINK_ROLLBACK_LEN) {
        *status = KILNLINK_ROLLBACK_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_ROLLBACK_CMD;

    return KILNLINK_ROLLBACK_LEN;
}

kilnlink_rollback_status_t kilnlink_rollback_decode(const uint8_t *payload, size_t len,
                                                     kilnlink_rollback_t *out)
{
    if (len != KILNLINK_ROLLBACK_LEN) {
        return KILNLINK_ROLLBACK_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_ROLLBACK_CMD) {
        return KILNLINK_ROLLBACK_ERR_WRONG_CMD;
    }

    if (out) {
        out->reserved0 = 0;
    }

    return KILNLINK_ROLLBACK_OK;
}
