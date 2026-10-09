#include "kilnlink/kilnlink_get_stack_margin.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0   u8  cmd (0x2B)
 */

size_t kilnlink_get_stack_margin_encode(const kilnlink_get_stack_margin_t *msg, uint8_t *out,
                                         size_t out_cap,
                                         kilnlink_get_stack_margin_status_t *status)
{
    (void)msg;
    kilnlink_get_stack_margin_status_t local_status = KILNLINK_GET_STACK_MARGIN_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_GET_STACK_MARGIN_OK;

    if (out_cap < KILNLINK_GET_STACK_MARGIN_LEN) {
        *status = KILNLINK_GET_STACK_MARGIN_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_GET_STACK_MARGIN_CMD;
    return KILNLINK_GET_STACK_MARGIN_LEN;
}

kilnlink_get_stack_margin_status_t kilnlink_get_stack_margin_decode(
    const uint8_t *payload, size_t len, kilnlink_get_stack_margin_t *out)
{
    if (len != KILNLINK_GET_STACK_MARGIN_LEN) {
        return KILNLINK_GET_STACK_MARGIN_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_GET_STACK_MARGIN_CMD) {
        return KILNLINK_GET_STACK_MARGIN_ERR_WRONG_CMD;
    }
    if (out) {
        out->reserved = 0;
    }
    return KILNLINK_GET_STACK_MARGIN_OK;
}
