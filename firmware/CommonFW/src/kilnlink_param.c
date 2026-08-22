#include "kilnlink/kilnlink_param.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4/6:
 *   0      u8  cmd (0x1E)
 *   1..2   u16 LE  param_id
 *   3      u8  found (0/1)
 *   4      u8  type (KILNLINK_PARAM_TYPE_*; ignore when found == 0)
 *   5..    value, kilnlink_param_value_len(type) bytes (present only when found == 1)
 */
#define OFF_PARAM_ID 1u
#define OFF_FOUND    3u
#define OFF_TYPE     4u
#define OFF_VALUE    5u

size_t kilnlink_param_encode(const kilnlink_param_t *msg, uint8_t *out, size_t out_cap,
                              kilnlink_param_status_t *status)
{
    kilnlink_param_status_t local_status = KILNLINK_PARAM_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_PARAM_OK;

    size_t vlen = 0;
    if (msg->found) {
        vlen = kilnlink_param_value_len(msg->type);
        if (vlen == 0) {
            *status = KILNLINK_PARAM_ERR_BAD_TYPE;
            return 0;
        }
    }

    size_t total = KILNLINK_PARAM_HDR_LEN + vlen;
    if (out_cap < total) {
        *status = KILNLINK_PARAM_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_PARAM_CMD;
    kilnlink_put_u16le(out, OFF_PARAM_ID, msg->param_id);
    out[OFF_FOUND] = msg->found ? 1u : 0u;
    out[OFF_TYPE] = msg->found ? msg->type : 0u;
    if (msg->found) {
        if (!kilnlink_param_value_encode(msg->type, &msg->value, out, OFF_VALUE)) {
            *status = KILNLINK_PARAM_ERR_BAD_TYPE; /* unreachable: type already validated above */
            return 0;
        }
    }

    return total;
}

kilnlink_param_status_t kilnlink_param_decode(const uint8_t *payload, size_t len,
                                               kilnlink_param_t *out)
{
    if (len < KILNLINK_PARAM_HDR_LEN) {
        return KILNLINK_PARAM_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_PARAM_CMD) {
        return KILNLINK_PARAM_ERR_WRONG_CMD;
    }

    uint8_t found = payload[OFF_FOUND];
    if (found != 0 && found != 1) {
        return KILNLINK_PARAM_ERR_BAD_FOUND;
    }

    if (!found) {
        /* No value follows -- the frame must be exactly the header, not
         * longer (trailing bytes after a "not found" answer are exactly
         * the kind of confident-wrong-length this codec must reject). */
        if (len != KILNLINK_PARAM_HDR_LEN) {
            return KILNLINK_PARAM_ERR_LENGTH_MISMATCH;
        }
        out->param_id = kilnlink_get_u16le(payload, OFF_PARAM_ID);
        out->found = 0;
        out->type = 0;
        out->value.u8_val = 0; /* not meaningful when found == 0; zeroed rather than left stale */
        return KILNLINK_PARAM_OK;
    }

    uint8_t type = payload[OFF_TYPE];
    size_t vlen = kilnlink_param_value_len(type);
    if (vlen == 0) {
        return KILNLINK_PARAM_ERR_BAD_TYPE;
    }
    if (len != KILNLINK_PARAM_HDR_LEN + vlen) {
        return KILNLINK_PARAM_ERR_LENGTH_MISMATCH;
    }

    out->param_id = kilnlink_get_u16le(payload, OFF_PARAM_ID);
    out->found = 1;
    out->type = type;
    if (!kilnlink_param_value_decode(type, payload, OFF_VALUE, &out->value)) {
        return KILNLINK_PARAM_ERR_BAD_TYPE; /* unreachable: type already validated above */
    }

    return KILNLINK_PARAM_OK;
}
