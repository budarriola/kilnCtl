#include "kilnlink/kilnlink_ceiling.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x09)
 *   1..4   f32 firing_max_c
 */
#define OFF_FIRING_MAX 1u

size_t kilnlink_ceiling_encode(const kilnlink_ceiling_t *msg, uint8_t *out, size_t out_cap,
                                kilnlink_ceiling_status_t *status)
{
    kilnlink_ceiling_status_t local_status = KILNLINK_CEILING_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CEILING_OK;

    if (out_cap < KILNLINK_CEILING_LEN) {
        *status = KILNLINK_CEILING_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_CEILING_CMD;
    kilnlink_put_f32le(out, OFF_FIRING_MAX, msg->firing_max_c);

    return KILNLINK_CEILING_LEN;
}

kilnlink_ceiling_status_t kilnlink_ceiling_decode(const uint8_t *payload, size_t len,
                                                   kilnlink_ceiling_t *out)
{
    if (len != KILNLINK_CEILING_LEN) {
        return KILNLINK_CEILING_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_CEILING_CMD) {
        return KILNLINK_CEILING_ERR_WRONG_CMD;
    }

    out->firing_max_c = kilnlink_get_f32le(payload, OFF_FIRING_MAX);

    return KILNLINK_CEILING_OK;
}
