#include "kilnlink/kilnlink_trip.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 6 Frame D:
 *   0        u8  cmd (0x0D)
 *   1        u8  trip_seq
 *   2        u8  trip_reason
 *   3..6     u32 uptime_ms
 *   7..10    f32 safety_tc_c
 *   11..14   f32 deciding_threshold
 *   15..18   f32 current_a[0]
 *   19..22   f32 current_a[1]
 *   23..26   f32 current_a[2]
 *   27       u8  relay_recent_mask
 *   28       u8  context_age_100ms
 */
#define OFF_TRIP_SEQ 1u
#define OFF_TRIP_REASON 2u
#define OFF_UPTIME 3u
#define OFF_TC 7u
#define OFF_THRESHOLD 11u
#define OFF_CURRENTS 15u
#define OFF_RELAY_MASK 27u
#define OFF_CONTEXT_AGE 28u

size_t kilnlink_trip_encode(const kilnlink_trip_t *tr, uint8_t *out, size_t out_cap,
                            kilnlink_trip_status_t *status)
{
    kilnlink_trip_status_t local_status = KILNLINK_TRIP_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_TRIP_OK;

    if (out_cap < KILNLINK_TRIP_LEN) {
        *status = KILNLINK_TRIP_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_TRIP_CMD;
    out[OFF_TRIP_SEQ] = tr->trip_seq;
    out[OFF_TRIP_REASON] = tr->trip_reason;
    kilnlink_put_u32le(out, OFF_UPTIME, tr->uptime_ms);
    kilnlink_put_f32le(out, OFF_TC, tr->safety_tc_c);
    kilnlink_put_f32le(out, OFF_THRESHOLD, tr->deciding_threshold);
    for (unsigned ch = 0; ch < KILNLINK_TRIP_CHANNELS; ch++) {
        kilnlink_put_f32le(out, OFF_CURRENTS + (size_t)ch * 4u, tr->current_a[ch]);
    }
    out[OFF_RELAY_MASK] = tr->relay_recent_mask;
    out[OFF_CONTEXT_AGE] = tr->context_age_100ms;

    return KILNLINK_TRIP_LEN;
}

kilnlink_trip_status_t kilnlink_trip_decode(const uint8_t *payload, size_t len,
                                            kilnlink_trip_t *out)
{
    if (len != KILNLINK_TRIP_LEN) {
        return KILNLINK_TRIP_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_TRIP_CMD) {
        return KILNLINK_TRIP_ERR_WRONG_CMD;
    }

    out->trip_seq = payload[OFF_TRIP_SEQ];
    out->trip_reason = payload[OFF_TRIP_REASON];
    out->uptime_ms = kilnlink_get_u32le(payload, OFF_UPTIME);
    out->safety_tc_c = kilnlink_get_f32le(payload, OFF_TC);
    out->deciding_threshold = kilnlink_get_f32le(payload, OFF_THRESHOLD);
    for (unsigned ch = 0; ch < KILNLINK_TRIP_CHANNELS; ch++) {
        out->current_a[ch] = kilnlink_get_f32le(payload, OFF_CURRENTS + (size_t)ch * 4u);
    }
    out->relay_recent_mask = payload[OFF_RELAY_MASK];
    out->context_age_100ms = payload[OFF_CONTEXT_AGE];

    return KILNLINK_TRIP_OK;
}
