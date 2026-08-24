#include "kilnlink/kilnlink_get_param.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x23)
 *   1..2   u16 LE  param_id
 */
#define OFF_PARAM_ID 1u

size_t kilnlink_get_param_encode(const kilnlink_get_param_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_get_param_status_t *status)
{
    kilnlink_get_param_status_t local_status = KILNLINK_GET_PARAM_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_GET_PARAM_OK;

    if (out_cap < KILNLINK_GET_PARAM_LEN) {
        *status = KILNLINK_GET_PARAM_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_GET_PARAM_CMD;
    kilnlink_put_u16le(out, OFF_PARAM_ID, msg->param_id);

    return KILNLINK_GET_PARAM_LEN;
}

kilnlink_get_param_status_t kilnlink_get_param_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_get_param_t *out)
{
    if (len != KILNLINK_GET_PARAM_LEN) {
        return KILNLINK_GET_PARAM_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_GET_PARAM_CMD) {
        return KILNLINK_GET_PARAM_ERR_WRONG_CMD;
    }

    out->param_id = kilnlink_get_u16le(payload, OFF_PARAM_ID);

    return KILNLINK_GET_PARAM_OK;
}
