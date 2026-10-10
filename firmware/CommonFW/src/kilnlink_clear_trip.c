#include "kilnlink/kilnlink_clear_trip.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x0A)
 *   1..2   u16 trip_mask
 */
#define OFF_TRIP_MASK 1u
#define OFF_TRIP_SEQ 3u /* KILNLINK_CLEAR_TRIP_LEN_V2 only (protocol 17) */

size_t kilnlink_clear_trip_encode(const kilnlink_clear_trip_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_clear_trip_status_t *status)
{
    kilnlink_clear_trip_status_t local_status = KILNLINK_CLEAR_TRIP_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CLEAR_TRIP_OK;

    size_t need = msg->has_trip_seq ? KILNLINK_CLEAR_TRIP_LEN_V2 : KILNLINK_CLEAR_TRIP_LEN;
    if (out_cap < need) {
        *status = KILNLINK_CLEAR_TRIP_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_CLEAR_TRIP_CMD;
    kilnlink_put_u16le(out, OFF_TRIP_MASK, msg->trip_mask);
    if (msg->has_trip_seq) {
        out[OFF_TRIP_SEQ] = msg->trip_seq;
    }

    return need;
}

kilnlink_clear_trip_status_t kilnlink_clear_trip_decode(const uint8_t *payload, size_t len,
                                                         kilnlink_clear_trip_t *out)
{
    if (len != KILNLINK_CLEAR_TRIP_LEN && len != KILNLINK_CLEAR_TRIP_LEN_V2) {
        return KILNLINK_CLEAR_TRIP_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_CLEAR_TRIP_CMD) {
        return KILNLINK_CLEAR_TRIP_ERR_WRONG_CMD;
    }

    out->trip_mask = kilnlink_get_u16le(payload, OFF_TRIP_MASK);
    out->has_trip_seq = (len == KILNLINK_CLEAR_TRIP_LEN_V2);
    out->trip_seq = out->has_trip_seq ? payload[OFF_TRIP_SEQ] : 0u;

    return KILNLINK_CLEAR_TRIP_OK;
}
