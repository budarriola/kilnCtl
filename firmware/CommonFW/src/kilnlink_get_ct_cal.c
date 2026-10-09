#include "kilnlink/kilnlink_get_ct_cal.h"
/* Command byte is KILNLINK_GET_CT_CAL_CMD (0x22, its own id since
 * KILNLINK_PROTOCOL_VERSION 7 -- see the header's comment). */

size_t kilnlink_get_ct_cal_encode(const kilnlink_get_ct_cal_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_get_ct_cal_status_t *status)
{
    kilnlink_get_ct_cal_status_t local_status = KILNLINK_GET_CT_CAL_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_GET_CT_CAL_OK;
    (void)msg; /* no fields to read */

    if (out_cap < KILNLINK_GET_CT_CAL_LEN) {
        *status = KILNLINK_GET_CT_CAL_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_GET_CT_CAL_CMD;

    return KILNLINK_GET_CT_CAL_LEN;
}

kilnlink_get_ct_cal_status_t kilnlink_get_ct_cal_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_get_ct_cal_t *out)
{
    if (len != KILNLINK_GET_CT_CAL_LEN) {
        return KILNLINK_GET_CT_CAL_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_GET_CT_CAL_CMD) {
        return KILNLINK_GET_CT_CAL_ERR_WRONG_CMD;
    }

    if (out) {
        out->reserved = 0;
    }

    return KILNLINK_GET_CT_CAL_OK;
}
