#include "kilnlink/kilnlink_commit_config_rejected.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x20)
 *   1..2   u16 LE  param_id
 *   3      u8  reason
 */
#define OFF_PARAM_ID 1u
#define OFF_REASON   3u

size_t kilnlink_commit_config_rejected_encode(const kilnlink_commit_config_rejected_t *msg, uint8_t *out,
                                               size_t out_cap,
                                               kilnlink_commit_config_rejected_status_t *status)
{
    kilnlink_commit_config_rejected_status_t local_status = KILNLINK_COMMIT_CONFIG_REJECTED_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_COMMIT_CONFIG_REJECTED_OK;

    if (out_cap < KILNLINK_COMMIT_CONFIG_REJECTED_LEN) {
        *status = KILNLINK_COMMIT_CONFIG_REJECTED_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_COMMIT_CONFIG_REJECTED_CMD;
    kilnlink_put_u16le(out, OFF_PARAM_ID, msg->param_id);
    out[OFF_REASON] = msg->reason;

    return KILNLINK_COMMIT_CONFIG_REJECTED_LEN;
}

kilnlink_commit_config_rejected_status_t kilnlink_commit_config_rejected_decode(
    const uint8_t *payload, size_t len, kilnlink_commit_config_rejected_t *out)
{
    if (len != KILNLINK_COMMIT_CONFIG_REJECTED_LEN) {
        return KILNLINK_COMMIT_CONFIG_REJECTED_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_COMMIT_CONFIG_REJECTED_CMD) {
        return KILNLINK_COMMIT_CONFIG_REJECTED_ERR_WRONG_CMD;
    }

    out->param_id = kilnlink_get_u16le(payload, OFF_PARAM_ID);
    out->reason = payload[OFF_REASON];

    return KILNLINK_COMMIT_CONFIG_REJECTED_OK;
}
