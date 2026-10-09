#include "kilnlink/kilnlink_ct_auto_zero_status.h"

#include "kilnlink/kilnlink_bytes.h"

#define OFF_STATE          1u
#define OFF_CHANNEL        2u
#define OFF_SAMPLES_TAKEN  3u
#define OFF_SAMPLES_TARGET 5u
#define OFF_ZERO_COUNTS    7u

size_t kilnlink_ct_auto_zero_status_encode(const kilnlink_ct_auto_zero_status_t *msg, uint8_t *out,
                                            size_t out_cap,
                                            kilnlink_ct_auto_zero_status_codec_t *status)
{
    kilnlink_ct_auto_zero_status_codec_t local_status = KILNLINK_CT_AUTO_ZERO_STATUS_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CT_AUTO_ZERO_STATUS_OK;

    if (out_cap < KILNLINK_CT_AUTO_ZERO_STATUS_LEN) {
        *status = KILNLINK_CT_AUTO_ZERO_STATUS_ERR_BUFFER_TOO_SMALL;
        return 0;
    }
    if (!msg) {
        *status = KILNLINK_CT_AUTO_ZERO_STATUS_ERR_BUFFER_TOO_SMALL; /* nothing to encode */
        return 0;
    }

    out[0] = KILNLINK_CT_AUTO_ZERO_STATUS_CMD;
    out[OFF_STATE] = msg->state;
    out[OFF_CHANNEL] = msg->channel;
    kilnlink_put_u16le(out, OFF_SAMPLES_TAKEN, msg->samples_taken);
    kilnlink_put_u16le(out, OFF_SAMPLES_TARGET, msg->samples_target);
    kilnlink_put_u16le(out, OFF_ZERO_COUNTS, msg->zero_counts);

    return KILNLINK_CT_AUTO_ZERO_STATUS_LEN;
}

kilnlink_ct_auto_zero_status_codec_t kilnlink_ct_auto_zero_status_decode(
    const uint8_t *payload, size_t len, kilnlink_ct_auto_zero_status_t *out)
{
    if (len != KILNLINK_CT_AUTO_ZERO_STATUS_LEN) {
        return KILNLINK_CT_AUTO_ZERO_STATUS_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_CT_AUTO_ZERO_STATUS_CMD) {
        return KILNLINK_CT_AUTO_ZERO_STATUS_ERR_WRONG_CMD;
    }
    if (out) {
        out->state = payload[OFF_STATE];
        out->channel = payload[OFF_CHANNEL];
        out->samples_taken = kilnlink_get_u16le(payload, OFF_SAMPLES_TAKEN);
        out->samples_target = kilnlink_get_u16le(payload, OFF_SAMPLES_TARGET);
        out->zero_counts = kilnlink_get_u16le(payload, OFF_ZERO_COUNTS);
    }
    return KILNLINK_CT_AUTO_ZERO_STATUS_OK;
}
