#include "kilnlink/kilnlink_set_param.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x1C)
 *   1..2   u16 LE  param_id
 *   3      u8  type (KILNLINK_PARAM_TYPE_*)
 *   4..    value, kilnlink_param_value_len(type) bytes
 */
#define OFF_PARAM_ID 1u
#define OFF_TYPE     3u
#define OFF_VALUE    4u

size_t kilnlink_set_param_encode(const kilnlink_set_param_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_set_param_status_t *status)
{
    kilnlink_set_param_status_t local_status = KILNLINK_SET_PARAM_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_SET_PARAM_OK;

    size_t vlen = kilnlink_param_value_len(msg->type);
    if (vlen == 0) {
        *status = KILNLINK_SET_PARAM_ERR_BAD_TYPE;
        return 0;
    }

    size_t total = KILNLINK_SET_PARAM_HDR_LEN + vlen;
    if (out_cap < total) {
        *status = KILNLINK_SET_PARAM_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_SET_PARAM_CMD;
    kilnlink_put_u16le(out, OFF_PARAM_ID, msg->param_id);
    out[OFF_TYPE] = msg->type;
    /* vlen is nonzero and msg->type was already validated above, so this
     * cannot fail -- but the return is not discarded, in case a future type
     * is added to one side without the other. */
    if (!kilnlink_param_value_encode(msg->type, &msg->value, out, OFF_VALUE)) {
        *status = KILNLINK_SET_PARAM_ERR_BAD_TYPE;
        return 0;
    }

    return total;
}

kilnlink_set_param_status_t kilnlink_set_param_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_set_param_t *out)
{
    if (len < KILNLINK_SET_PARAM_HDR_LEN) {
        return KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_SET_PARAM_CMD) {
        return KILNLINK_SET_PARAM_ERR_WRONG_CMD;
    }

    uint8_t type = payload[OFF_TYPE];
    size_t vlen = kilnlink_param_value_len(type);
    if (vlen == 0) {
        return KILNLINK_SET_PARAM_ERR_BAD_TYPE;
    }
    /* Exact-length check, not >=: this is what catches both an over-long
     * frame (trailing garbage past a valid value) and a truncated-mid-value
     * frame (len landed between the header and a full value) in one test,
     * without ever indexing past `payload[len - 1]` first. */
    if (len != KILNLINK_SET_PARAM_HDR_LEN + vlen) {
        return KILNLINK_SET_PARAM_ERR_LENGTH_MISMATCH;
    }

    out->param_id = kilnlink_get_u16le(payload, OFF_PARAM_ID);
    out->type = type;
    if (!kilnlink_param_value_decode(type, payload, OFF_VALUE, &out->value)) {
        return KILNLINK_SET_PARAM_ERR_BAD_TYPE; /* unreachable: type already validated above */
    }

    return KILNLINK_SET_PARAM_OK;
}
