#include "kilnlink/kilnlink_set_clock.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x0C)
 *   1..8   u64 epoch_ms
 */
#define OFF_EPOCH_MS 1u

size_t kilnlink_set_clock_encode(const kilnlink_set_clock_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_set_clock_status_t *status)
{
    kilnlink_set_clock_status_t local_status = KILNLINK_SET_CLOCK_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_SET_CLOCK_OK;

    if (out_cap < KILNLINK_SET_CLOCK_LEN) {
        *status = KILNLINK_SET_CLOCK_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_SET_CLOCK_CMD;
    kilnlink_put_u64le(out, OFF_EPOCH_MS, msg->epoch_ms);

    return KILNLINK_SET_CLOCK_LEN;
}

kilnlink_set_clock_status_t kilnlink_set_clock_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_set_clock_t *out)
{
    if (len != KILNLINK_SET_CLOCK_LEN) {
        return KILNLINK_SET_CLOCK_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_SET_CLOCK_CMD) {
        return KILNLINK_SET_CLOCK_ERR_WRONG_CMD;
    }

    out->epoch_ms = kilnlink_get_u64le(payload, OFF_EPOCH_MS);

    return KILNLINK_SET_CLOCK_OK;
}
