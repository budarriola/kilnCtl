#include "kilnlink/kilnlink_ct_auto_zero_begin.h"

#define OFF_CHANNEL 1u

size_t kilnlink_ct_auto_zero_begin_encode(const kilnlink_ct_auto_zero_begin_t *msg, uint8_t *out,
                                           size_t out_cap,
                                           kilnlink_ct_auto_zero_begin_status_t *status)
{
    kilnlink_ct_auto_zero_begin_status_t local_status = KILNLINK_CT_AUTO_ZERO_BEGIN_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CT_AUTO_ZERO_BEGIN_OK;

    if (out_cap < KILNLINK_CT_AUTO_ZERO_BEGIN_LEN) {
        *status = KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_CT_AUTO_ZERO_BEGIN_CMD;
    out[OFF_CHANNEL] = msg ? msg->channel : 0u;

    return KILNLINK_CT_AUTO_ZERO_BEGIN_LEN;
}

kilnlink_ct_auto_zero_begin_status_t kilnlink_ct_auto_zero_begin_decode(
    const uint8_t *payload, size_t len, kilnlink_ct_auto_zero_begin_t *out)
{
    if (len != KILNLINK_CT_AUTO_ZERO_BEGIN_LEN) {
        return KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_CT_AUTO_ZERO_BEGIN_CMD) {
        return KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_WRONG_CMD;
    }
    if (out) {
        out->channel = payload[OFF_CHANNEL];
    }
    return KILNLINK_CT_AUTO_ZERO_BEGIN_OK;
}
