#include "kilnlink/kilnlink_get_ct_auto_zero.h"

size_t kilnlink_get_ct_auto_zero_encode(const kilnlink_get_ct_auto_zero_t *msg, uint8_t *out,
                                         size_t out_cap, kilnlink_get_ct_auto_zero_status_t *status)
{
    (void)msg;
    kilnlink_get_ct_auto_zero_status_t local_status = KILNLINK_GET_CT_AUTO_ZERO_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_GET_CT_AUTO_ZERO_OK;

    if (out_cap < KILNLINK_GET_CT_AUTO_ZERO_LEN) {
        *status = KILNLINK_GET_CT_AUTO_ZERO_ERR_BUFFER_TOO_SMALL;
        return 0;
    }
    out[0] = KILNLINK_GET_CT_AUTO_ZERO_CMD;
    return KILNLINK_GET_CT_AUTO_ZERO_LEN;
}

kilnlink_get_ct_auto_zero_status_t kilnlink_get_ct_auto_zero_decode(
    const uint8_t *payload, size_t len, kilnlink_get_ct_auto_zero_t *out)
{
    if (len != KILNLINK_GET_CT_AUTO_ZERO_LEN) {
        return KILNLINK_GET_CT_AUTO_ZERO_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_GET_CT_AUTO_ZERO_CMD) {
        return KILNLINK_GET_CT_AUTO_ZERO_ERR_WRONG_CMD;
    }
    if (out) {
        out->_unused = 0;
    }
    return KILNLINK_GET_CT_AUTO_ZERO_OK;
}
