#include "kilnlink/kilnlink_set_ct_cal.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x19)
 *   1      u8  channel
 *   2      u8  calibrated (0/1)
 *   3..6   f32 LE  gain
 *   7..10  f32 LE  offset
 */
#define OFF_CHANNEL    1u
#define OFF_CALIBRATED 2u
#define OFF_GAIN       3u
#define OFF_OFFSET     7u

size_t kilnlink_set_ct_cal_encode(const kilnlink_set_ct_cal_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_set_ct_cal_status_t *status)
{
    kilnlink_set_ct_cal_status_t local_status = KILNLINK_SET_CT_CAL_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_SET_CT_CAL_OK;

    if (out_cap < KILNLINK_SET_CT_CAL_LEN) {
        *status = KILNLINK_SET_CT_CAL_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_SET_CT_CAL_CMD;
    out[OFF_CHANNEL] = msg->channel;
    out[OFF_CALIBRATED] = msg->calibrated;
    kilnlink_put_f32le(out, OFF_GAIN, msg->gain);
    kilnlink_put_f32le(out, OFF_OFFSET, msg->offset);

    return KILNLINK_SET_CT_CAL_LEN;
}

kilnlink_set_ct_cal_status_t kilnlink_set_ct_cal_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_set_ct_cal_t *out)
{
    if (len != KILNLINK_SET_CT_CAL_LEN) {
        return KILNLINK_SET_CT_CAL_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_SET_CT_CAL_CMD) {
        return KILNLINK_SET_CT_CAL_ERR_WRONG_CMD;
    }

    out->channel = payload[OFF_CHANNEL];
    out->calibrated = payload[OFF_CALIBRATED];
    out->gain = kilnlink_get_f32le(payload, OFF_GAIN);
    out->offset = kilnlink_get_f32le(payload, OFF_OFFSET);

    return KILNLINK_SET_CT_CAL_OK;
}
