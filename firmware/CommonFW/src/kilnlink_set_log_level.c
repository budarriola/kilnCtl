#include "kilnlink/kilnlink_set_log_level.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x1B)
 *   1      u8  level
 */
#define OFF_LEVEL 1u

size_t kilnlink_set_log_level_encode(const kilnlink_set_log_level_t *msg, uint8_t *out,
                                      size_t out_cap, kilnlink_set_log_level_status_t *status)
{
    kilnlink_set_log_level_status_t local_status = KILNLINK_SET_LOG_LEVEL_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_SET_LOG_LEVEL_OK;

    if (out_cap < KILNLINK_SET_LOG_LEVEL_LEN) {
        *status = KILNLINK_SET_LOG_LEVEL_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_SET_LOG_LEVEL_CMD;
    out[OFF_LEVEL] = msg->level;

    return KILNLINK_SET_LOG_LEVEL_LEN;
}

kilnlink_set_log_level_status_t kilnlink_set_log_level_decode(const uint8_t *payload, size_t len,
                                                                kilnlink_set_log_level_t *out)
{
    if (len != KILNLINK_SET_LOG_LEVEL_LEN) {
        return KILNLINK_SET_LOG_LEVEL_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_SET_LOG_LEVEL_CMD) {
        return KILNLINK_SET_LOG_LEVEL_ERR_WRONG_CMD;
    }

    out->level = payload[OFF_LEVEL];

    return KILNLINK_SET_LOG_LEVEL_OK;
}
