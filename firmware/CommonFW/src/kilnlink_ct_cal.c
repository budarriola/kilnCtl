#include "kilnlink/kilnlink_ct_cal.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 6:
 *   0            u8  cmd (0x1A)
 *   1..9         channel 0: calibrated u8(1) + gain f32(4) + offset f32(4)
 *   10..18       channel 1: same layout
 *   19..27       channel 2: same layout
 */
#define OFF_CHANNELS 1u

static size_t channel_off(unsigned ch)
{
    return OFF_CHANNELS + (size_t)ch * KILNLINK_CT_CAL_CHANNEL_LEN;
}

size_t kilnlink_ct_cal_encode(const kilnlink_ct_cal_t *cal, uint8_t *out, size_t out_cap,
                               kilnlink_ct_cal_status_t *status)
{
    kilnlink_ct_cal_status_t local_status = KILNLINK_CT_CAL_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CT_CAL_OK;

    if (out_cap < KILNLINK_CT_CAL_LEN) {
        *status = KILNLINK_CT_CAL_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_CT_CAL_CMD;
    for (unsigned ch = 0; ch < KILNLINK_CT_CAL_NUM_CHANNELS; ch++) {
        size_t base = channel_off(ch);
        out[base] = cal->channels[ch].calibrated;
        kilnlink_put_f32le(out, base + 1u, cal->channels[ch].gain);
        kilnlink_put_f32le(out, base + 5u, cal->channels[ch].offset);
    }

    return KILNLINK_CT_CAL_LEN;
}

kilnlink_ct_cal_status_t kilnlink_ct_cal_decode(const uint8_t *payload, size_t len,
                                                 kilnlink_ct_cal_t *out)
{
    if (len != KILNLINK_CT_CAL_LEN) {
        return KILNLINK_CT_CAL_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_CT_CAL_CMD) {
        return KILNLINK_CT_CAL_ERR_WRONG_CMD;
    }

    for (unsigned ch = 0; ch < KILNLINK_CT_CAL_NUM_CHANNELS; ch++) {
        size_t base = channel_off(ch);
        out->channels[ch].calibrated = payload[base];
        out->channels[ch].gain = kilnlink_get_f32le(payload, base + 1u);
        out->channels[ch].offset = kilnlink_get_f32le(payload, base + 5u);
    }

    return KILNLINK_CT_CAL_OK;
}
