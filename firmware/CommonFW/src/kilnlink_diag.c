#include "kilnlink/kilnlink_diag.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 6 Frame B:
 *   0        u8  cmd (0x08)
 *   1        u8  trip_reason
 *   2..3     u16 warn_mask
 *   4..5     u16 trip_mask
 *   6..9     u32 uptime_ms
 *   10       u8  boot_reason
 *   11       u8  context_age_100ms
 *   12..15   u32 context_frames_ok
 *   16..19   u32 context_frames_bad
 *   20..23   u32 tx_frames_dropped
 *   24       u8  state
 *   25       u8  flags
 */
#define OFF_TRIP_REASON 1u
#define OFF_WARN_MASK 2u
#define OFF_TRIP_MASK 4u
#define OFF_UPTIME 6u
#define OFF_BOOT_REASON 10u
#define OFF_CONTEXT_AGE 11u
#define OFF_CONTEXT_OK 12u
#define OFF_CONTEXT_BAD 16u
#define OFF_TX_DROPPED 20u
#define OFF_STATE 24u
#define OFF_FLAGS 25u

size_t kilnlink_diag_encode(const kilnlink_diag_t *dg, uint8_t *out, size_t out_cap,
                            kilnlink_diag_status_t *status)
{
    kilnlink_diag_status_t local_status = KILNLINK_DIAG_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_DIAG_OK;

    if (out_cap < KILNLINK_DIAG_LEN) {
        *status = KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_DIAG_CMD;
    out[OFF_TRIP_REASON] = dg->trip_reason;
    kilnlink_put_u16le(out, OFF_WARN_MASK, dg->warn_mask);
    kilnlink_put_u16le(out, OFF_TRIP_MASK, dg->trip_mask);
    kilnlink_put_u32le(out, OFF_UPTIME, dg->uptime_ms);
    out[OFF_BOOT_REASON] = dg->boot_reason;
    out[OFF_CONTEXT_AGE] = dg->context_age_100ms;
    kilnlink_put_u32le(out, OFF_CONTEXT_OK, dg->context_frames_ok);
    kilnlink_put_u32le(out, OFF_CONTEXT_BAD, dg->context_frames_bad);
    kilnlink_put_u32le(out, OFF_TX_DROPPED, dg->tx_frames_dropped);
    out[OFF_STATE] = dg->state;
    out[OFF_FLAGS] = dg->flags;

    return KILNLINK_DIAG_LEN;
}

kilnlink_diag_status_t kilnlink_diag_decode(const uint8_t *payload, size_t len,
                                            kilnlink_diag_t *out)
{
    if (len != KILNLINK_DIAG_LEN) {
        return KILNLINK_DIAG_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_DIAG_CMD) {
        return KILNLINK_DIAG_ERR_WRONG_CMD;
    }

    out->trip_reason = payload[OFF_TRIP_REASON];
    out->warn_mask = kilnlink_get_u16le(payload, OFF_WARN_MASK);
    out->trip_mask = kilnlink_get_u16le(payload, OFF_TRIP_MASK);
    out->uptime_ms = kilnlink_get_u32le(payload, OFF_UPTIME);
    out->boot_reason = payload[OFF_BOOT_REASON];
    out->context_age_100ms = payload[OFF_CONTEXT_AGE];
    out->context_frames_ok = kilnlink_get_u32le(payload, OFF_CONTEXT_OK);
    out->context_frames_bad = kilnlink_get_u32le(payload, OFF_CONTEXT_BAD);
    out->tx_frames_dropped = kilnlink_get_u32le(payload, OFF_TX_DROPPED);
    out->state = payload[OFF_STATE];
    out->flags = payload[OFF_FLAGS];

    return KILNLINK_DIAG_OK;
}
